// SPDX-License-Identifier: GPL-3.0-or-later
// Output pass (src/gfx/extras/output_pass.cpp) on the GPU, headless. Each case uploads a known source image, records OutputPass::record
// into a destination image, reads the destination back and checks it: against the CPU reference (hdr_math.h) for the HDR encodes, decoded
// with decode_hdr_capture; against the exact bytes for the SDR copy; against a CPU bilinear upscale for FSR 1. The group also requires zero
// validation errors and no leaked allocations.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.h"

#include "extras/hdr_capture.h"
#include "extras/hdr_math.h"
#include "extras/hdr_pattern.h"
#include "extras/output_pass.h"
#include "nxrt/gfx.h"

NXTEST_GROUP(extras_output)

namespace {

namespace hdr = nxrt::extras;
namespace gfx = nxrt::gfx;

// ---- half floats -----------------------------------------------------------------------------------------------------------------------

uint16_t half_from_float(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof x);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t e32 = (x >> 23) & 0xFFu;
    uint32_t man = x & 0x7FFFFFu;
    if (e32 == 0xFFu) return static_cast<uint16_t>(sign | 0x7C00u | (man ? 0x200u : 0u));
    const int exp = static_cast<int>(e32) - 127 + 15;
    if (exp >= 0x1F) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        man |= 0x800000u;
        const int shift = 14 - exp;
        uint32_t h = man >> shift;
        const uint32_t rem = man & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1u))) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = (static_cast<uint32_t>(exp) << 10) | (man >> 13);
    const uint32_t rem = man & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;   // a carry into the exponent is the right result
    return static_cast<uint16_t>(sign | h);
}

float float_from_half(uint16_t h) {
    const float sign = (h & 0x8000u) ? -1.0f : 1.0f;
    const int e = (h >> 10) & 0x1F;
    const uint32_t m = h & 0x3FFu;
    if (e == 0) return sign * std::ldexp(static_cast<float>(m), -24);
    return sign * std::ldexp(static_cast<float>(m | 0x400u), e - 25);   // the test values are finite and normal or zero
}

// The value the GPU actually sees for a float that is stored as a half.
float quant_half(float v) { return float_from_half(half_from_float(v)); }

// A w x h RGBA16F image: fill(x, y, rgba) sets the four values of a texel (alpha defaults to 1). Returns the bytes, 8 per texel.
std::vector<uint8_t> pack_half_rgba(uint32_t w, uint32_t h, const std::function<void(uint32_t, uint32_t, float*)>& fill) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 8);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            fill(x, y, v);
            for (int c = 0; c < 4; ++c) {
                const uint16_t hb = half_from_float(v[c]);
                uint8_t* p = &out[(static_cast<size_t>(y) * w + x) * 8 + 2 * c];
                p[0] = static_cast<uint8_t>(hb & 0xFFu);
                p[1] = static_cast<uint8_t>(hb >> 8);
            }
        }
    return out;
}

// ---- the colour ramp of cases 1 and 2 -----------------------------------------------------------------------------------------------

// 8x8 source: column x holds v = kRamp[x] as r = g = v and b = 0.5 v (scene-linear BT.709, 1.0 = paper white). Rows are identical.
constexpr float kRamp[8] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.9f, 8.0f};
constexpr float kPaper = 203.0f, kPeak = 1000.0f;
constexpr float kPeakRatio = kPeak / kPaper;   // 4.926 paper-white units

std::vector<uint8_t> ramp_source() {
    return pack_half_rgba(8, 8, [](uint32_t x, uint32_t, float* v) {
        v[0] = kRamp[x];
        v[1] = kRamp[x];
        v[2] = 0.5f * kRamp[x];
    });
}

// The input of column x as the GPU sees it (after the half-float rounding of the upload).
void ramp_input(uint32_t x, float in[3]) {
    in[0] = quant_half(kRamp[x]);
    in[1] = in[0];
    in[2] = quant_half(0.5f * kRamp[x]);
}

// CPU reference of the HDR10 path: encode_hdr10, then what decode_hdr_capture does with the code words (PQ inverse, divide by paper white,
// BT.2020 -> BT.709). Ideal values: the 10-bit quantisation is not modelled.
void cpu_hdr10_roundtrip(const float in[3], float out[3]) {
    hdr::HdrOut o;
    o.paper_white_nits = kPaper;
    o.peak_nits = kPeak;
    float pq[3];
    hdr::encode_hdr10(in, o, pq);
    float bt2020[3];
    for (int c = 0; c < 3; ++c) bt2020[c] = hdr::pq_eotf(pq[c]) / kPaper;
    hdr::bt2020_to_bt709(bt2020, out);
}

// ---- device, images and the pass -----------------------------------------------------------------------------------------------------

struct Rig {
    std::unique_ptr<gfx::Device> dev;
    hdr::OutputPass pass;
    gfx::Image src, dst, scene;      // scene: the picture before the UI (only the UI split case makes it)
    gfx::Buffer rb;                  // readback of the destination
    VkFormat dst_fmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0, bpp = 0;  // destination size and bytes per texel
};

// Frees what the rig made, then checks the validation counters and that the device holds no allocation.
void teardown(Rig& r) {
    if (!r.dev) return;
    r.dev->wait_idle();
    r.pass.shutdown();
    if (r.rb) r.dev->destroy_buffer(r.rb);
    if (r.dst) r.dev->destroy_image(r.dst);
    if (r.src) r.dev->destroy_image(r.src);
    if (r.scene) r.dev->destroy_image(r.scene);
    const gfx::ValidationStats v = r.dev->validation_stats();
    std::printf("    validation: %llu errors, %llu warnings\n", static_cast<unsigned long long>(v.errors), static_cast<unsigned long long>(v.warnings));
    if (!v.first_error.empty()) std::printf("      first error: %s\n", v.first_error.c_str());
    CHECK_EQ(v.errors, 0ull);
    CHECK_EQ(r.dev->memory_stats().allocations, 0ull);
    r.dev.reset();
}

// Runs teardown when the case ends, however it ends (a REQUIRE return included).
struct Teardown {
    Rig& r;
    ~Teardown() { teardown(r); }
};

// Headless device with validation on, and the output pass initialised. False when there is no device (the case skips).
bool make_device(Rig& r) {
    gfx::DeviceDesc dd;
    dd.validation = 1;
    dd.headless = 1;
    std::string err;
    r.dev = gfx::Device::create(dd, &err);
    if (!r.dev) {
        std::printf("    device: %s\n", err.c_str());
        return false;
    }
    CHECK(r.dev->validation_enabled());   // the zero counts below mean something only with the layer on
    CHECK(r.pass.init(*r.dev));
    return true;
}

// The source and destination formats must be usable as images here (sampled, colour attachment and transfer source). False: the case skips.
bool formats_supported(gfx::Device& dev, VkFormat src_fmt, VkFormat dst_fmt) {
    gfx::ImageDesc s;
    s.format = src_fmt;
    s.width = s.height = 8;
    s.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    gfx::ImageDesc d = s;
    d.format = dst_fmt;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    std::string why;
    if (!dev.image_supported(s, &why)) {
        std::printf("    source format %d: %s\n", static_cast<int>(src_fmt), why.c_str());
        return false;
    }
    if (!dev.image_supported(d, &why)) {
        std::printf("    destination format %d: %s\n", static_cast<int>(dst_fmt), why.c_str());
        return false;
    }
    return true;
}

// Sync2 layout transition of one colour image.
void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
             VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = ss;
    b.srcAccessMask = sa;
    b.dstStageMask = ds;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

bool make_image(gfx::Device& dev, const gfx::ImageDesc& d, gfx::Image* out) {
    std::string why;
    if (dev.create_image(d, out, &why) != VK_SUCCESS) {
        std::printf("    create_image %s: %s\n", d.name, why.c_str());
        CHECK_MSG(false, "create_image failed");
        return false;
    }
    return true;
}

// Uploads bytes (tightly packed, the size of fmt for w x h) into a sampled image, left in SHADER_READ_ONLY_OPTIMAL: the pass's source.
bool upload_source(Rig& r, VkFormat fmt, uint32_t w, uint32_t h, const std::vector<uint8_t>& bytes) {
    gfx::ImageDesc d;
    d.format = fmt;
    d.width = w;
    d.height = h;
    d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    d.name = "output test source";
    if (!make_image(*r.dev, d, &r.src)) return false;
    gfx::Buffer stage;
    if (r.dev->create_buffer(bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gfx::MemoryUse::Upload, &stage, "output test staging") != VK_SUCCESS) {
        CHECK_MSG(false, "staging buffer failed");
        return false;
    }
    std::memcpy(stage.mapped, bytes.data(), bytes.size());
    r.dev->flush_buffer(stage);
    const bool ok = r.dev->submit_once([&](VkCommandBuffer cmd) {
        barrier(cmd, r.src.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, stage.buffer, r.src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(cmd, r.src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    r.dev->destroy_buffer(stage);
    CHECK_MSG(ok, "upload submit failed");
    return ok;
}

// The destination: a colour attachment that can be copied out, and a readback buffer of the same size.
bool make_dst(Rig& r, VkFormat fmt, uint32_t w, uint32_t h, uint32_t bpp) {
    gfx::ImageDesc d;
    d.format = fmt;
    d.width = w;
    d.height = h;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    d.name = "output test destination";
    if (!make_image(*r.dev, d, &r.dst)) return false;
    r.dst_fmt = fmt;
    r.w = w;
    r.h = h;
    r.bpp = bpp;
    if (r.dev->create_buffer(static_cast<VkDeviceSize>(w) * h * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &r.rb,
                             "output test readback") != VK_SUCCESS) {
        CHECK_MSG(false, "readback buffer failed");
        return false;
    }
    return true;
}

// Parameters with the source already set; the case adjusts the rest.
hdr::OutputParams base_params(const Rig& r, uint32_t sw, uint32_t sh) {
    hdr::OutputParams p;
    p.src = r.src.view;
    p.src_extent = {sw, sh};
    p.keep_aspect = false;
    p.paper_white_nits = kPaper;
    p.peak_nits = kPeak;
    return p;
}

// Records OutputPass::record into the destination, copies it out and reads it back. Returns the encode mode the pass reported, or -2 when
// the submit failed. The destination starts undefined each time (the pass draws over all of it when keep_aspect is off, and the letterbox
// clears otherwise).
int run_pass(Rig& r, const hdr::OutputParams& p, std::vector<uint8_t>* out) {
    int mode = -2;
    const bool ok = r.dev->submit_once([&](VkCommandBuffer cmd) {
        barrier(cmd, r.dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        mode = r.pass.record(cmd, r.dst.view, r.dst_fmt, VkExtent2D{r.w, r.h}, p);
        barrier(cmd, r.dst.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {r.w, r.h, 1};
        vkCmdCopyImageToBuffer(cmd, r.dst.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.rb.buffer, 1, &region);
    });
    if (!ok) return -2;
    r.dev->invalidate_buffer(r.rb);
    const uint8_t* m = static_cast<const uint8_t*>(r.rb.mapped);
    out->assign(m, m + static_cast<size_t>(r.w) * r.h * r.bpp);
    return mode;
}

// Tolerance of the HDR checks: 2 percent relative or 0.01 absolute. The 10-bit PQ code steps are about 0.7 percent of the value at 200 nits
// and about 1 percent at 1000 nits, so 2 percent leaves margin.
bool close_to(double got, double want, double rel, double abs_tol) { return std::fabs(got - want) <= std::max(rel * std::fabs(want), abs_tol); }

// 8x8 RGBA8 grey levels: column x has v[x] in all three channels, alpha 255.
std::vector<uint8_t> grey_columns(const uint8_t v[8]) {
    std::vector<uint8_t> s(8 * 8 * 4);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const size_t o = (static_cast<size_t>(y) * 8 + x) * 4;
            s[o + 0] = s[o + 1] = s[o + 2] = v[x];
            s[o + 3] = 255;
        }
    return s;
}

// The shader's picture controls on one grey channel (the luma of a grey is the grey, so saturation does not move it).
double cpu_picture(double c, const float adj[4]) {
    const double b = adj[2];
    c = b < 0.0 ? (c + b) / (1.0 + b) : b + c * (1.0 - b);
    c = (c - 0.5) * adj[0] + 0.5;
    return std::pow(std::max(c, 0.0), 1.0 / adj[3]);
}

// The shader's HDR path for a grey sRGB byte, in paper-white units before the peak limit: sRGB decode, the picture controls (when adj is
// set) on rgb^(1/2.2), then the highlight expansion toward ratio.
double cpu_hdr_grey(uint8_t v, const float* adj, double ratio) {
    double l = hdr::srgb_eotf(v / 255.0f);
    if (adj) l = std::pow(cpu_picture(std::pow(l, 1.0 / 2.2), adj), 2.2);
    if (ratio > 1.001) {
        const double u = std::clamp((l - 0.25) / 0.75, 0.0, 1.0);
        l *= 1.0 + (ratio - 1.0) * std::pow(u, 2.2);
    }
    return l;
}

// The UI split's two pictures, both uploaded: the copy before the UI into r.scene, the presented picture into r.src.
bool upload_scene_and_final(Rig& r, const std::vector<uint8_t>& scene, const std::vector<uint8_t>& fin) {
    if (!upload_source(r, VK_FORMAT_R8G8B8A8_SRGB, 8, 8, scene)) return false;
    r.scene = r.src;
    r.src = gfx::Image();
    return upload_source(r, VK_FORMAT_R8G8B8A8_SRGB, 8, 8, fin);
}

// 8x8 white with UI on the right half: the presented picture is white everywhere, the copy before the UI is grey for x >= 4.
void white_with_ui_on_the_right(std::vector<uint8_t>* scene, std::vector<uint8_t>* fin) {
    const uint8_t s[8] = {255, 255, 255, 255, 128, 128, 128, 128}, f[8] = {255, 255, 255, 255, 255, 255, 255, 255};
    *scene = grey_columns(s);
    *fin = grey_columns(f);
}

// A new destination (and readback) of size x size, for a case that changes the destination.
bool remake_dst(Rig& r, VkFormat fmt, uint32_t size, uint32_t bpp) {
    if (r.rb) r.dev->destroy_buffer(r.rb);
    if (r.dst) r.dev->destroy_image(r.dst);
    r.rb = gfx::Buffer();
    r.dst = gfx::Image();
    return make_dst(r, fmt, size, size, bpp);
}


}  // namespace

// ---- 1: HDR10 encode against the CPU reference --------------------------------------------------------------------------------------

NXTEST(hdr10_encode_matches_cpu) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    REQUIRE(upload_source(r, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8, ramp_source()));
    REQUIRE(make_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 8, 8, 4));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::Hdr10;
    p.src_linear = true;
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 2);
    std::vector<float> lin;
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, got.data(), 8, 8, kPaper, &lin, nullptr, nullptr, nullptr));

    // Against the CPU reference chain (encode_hdr10, then the decoder's inverse).
    double max_abs = 0.0;
    size_t bad = 0;
    // Against the plan's simple expectation min(input, 1000/203) per channel: differs where the BT.2020 clamp bites (column 7).
    double max_naive = 0.0;
    int naive_col = -1;
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            float in[3], ref[3];
            ramp_input(x, in);
            cpu_hdr10_roundtrip(in, ref);
            for (int c = 0; c < 3; ++c) {
                const double g = lin[(static_cast<size_t>(y) * 8 + x) * 3 + c];
                if (!close_to(g, ref[c], 0.02, 0.01)) ++bad;
                max_abs = std::max(max_abs, std::fabs(g - ref[c]));
                const double naive = std::min(static_cast<double>(in[c]), static_cast<double>(kPeakRatio));
                if (std::fabs(g - naive) > max_naive) {
                    max_naive = std::fabs(g - naive);
                    naive_col = static_cast<int>(x);
                }
            }
        }
    std::printf("    hdr10: max abs error vs CPU reference %.5f, channels outside tolerance %zu of 192\n", max_abs, bad);
    std::printf("    hdr10: max deviation from min(input, 1000/203) %.4f (column %d; BT.2020 clamp, see test comment)\n", max_naive, naive_col);
    CHECK(bad == 0);
}

// ---- 2: scRGB encode ------------------------------------------------------------------------------------------------------------------

NXTEST(scrgb_encode_matches_cpu) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT)) {
        nxtest::skip("needed format not supported");
        return;
    }
    REQUIRE(upload_source(r, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8, ramp_source()));
    REQUIRE(make_dst(r, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8, 8));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::ScRgb;
    p.src_linear = true;
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 3);
    std::vector<float> lin;
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::Rgba16F, got.data(), 8, 8, kPaper, &lin, nullptr, nullptr, nullptr));

    // Expected after the decode: min(input, 1000/203) per channel (scRGB has no BT.2020 step, so only the peak limits it).
    double max_err = 0.0;
    size_t bad = 0;
    double worst_col_err[8] = {};
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            float in[3];
            ramp_input(x, in);
            for (int c = 0; c < 3; ++c) {
                const double e = std::min(static_cast<double>(in[c]), static_cast<double>(kPeakRatio));
                const double g = lin[(static_cast<size_t>(y) * 8 + x) * 3 + c];
                const double err = std::fabs(g - e);
                if (err > 0.01 * std::fabs(e) + 1e-4) ++bad;
                max_err = std::max(max_err, err);
                worst_col_err[x] = std::max(worst_col_err[x], err);
            }
        }
    std::printf("    scrgb: max abs error vs min(input, 1000/203) %.5f, channels outside 1 percent %zu of 192\n", max_err, bad);
    std::printf("    scrgb: column 7 (input 8.0) decoded to %.4f, expected %.4f\n", static_cast<double>(lin[(7 * 3)]), static_cast<double>(kPeakRatio));
    CHECK(bad == 0);
}

// ---- 3: SDR encode from scene-linear ------------------------------------------------------------------------------------------------

NXTEST(sdr_encode) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM)) {
        nxtest::skip("needed format not supported");
        return;
    }
    const float vals[4] = {0.0f, 0.1f, 0.5f, 1.0f};
    const std::vector<uint8_t> src = pack_half_rgba(8, 8, [&](uint32_t x, uint32_t, float* v) {
        v[0] = v[1] = v[2] = vals[x % 4];
    });
    REQUIRE(upload_source(r, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8, src));
    REQUIRE(make_dst(r, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 4));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::Sdr8;
    p.src_linear = true;
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 1);
    int max_diff = 0;
    int alpha_bad = 0;
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const float v = std::clamp(quant_half(vals[x % 4]), 0.0f, 1.0f);
            const int want = static_cast<int>(std::lround(255.0f * hdr::srgb_oetf(v)));
            const uint8_t* px = &got[(static_cast<size_t>(y) * 8 + x) * 4];
            for (int c = 0; c < 3; ++c) max_diff = std::max(max_diff, std::abs(static_cast<int>(px[c]) - want));
            if (px[3] != 255) ++alpha_bad;
        }
    std::printf("    sdr: max byte difference vs round(255 * srgb_oetf(clamp(v))) %d, alpha not 255: %d\n", max_diff, alpha_bad);
    std::printf("    sdr: value 0.5 -> byte %d (expected %ld)\n", got[(0 * 8 + 2) * 4], std::lround(255.0f * hdr::srgb_oetf(quant_half(0.5f))));
    CHECK(max_diff <= 1);
    CHECK_EQ(alpha_bad, 0);
}

// ---- 4: SDR copy is exact -----------------------------------------------------------------------------------------------------------

NXTEST(copy_mode_is_exact) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM)) {
        nxtest::skip("needed format not supported");
        return;
    }
    // Fixed-seed LCG bytes for RGB; alpha 255 so the expected output is the input bytes exactly.
    std::vector<uint8_t> src(8 * 8 * 4);
    uint32_t s = 12345u;
    for (size_t i = 0; i < src.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            s = s * 1664525u + 1013904223u;
            src[i + c] = static_cast<uint8_t>(s >> 24);
        }
        src[i + 3] = 255;
    }
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, src));
    REQUIRE(make_dst(r, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 4));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::Sdr8;
    p.src_linear = false;
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 0);
    size_t mismatch = 0;
    for (size_t i = 0; i < src.size(); ++i)
        if (got[i] != src[i]) ++mismatch;
    std::printf("    copy: %zu of %zu bytes differ from the input\n", mismatch, src.size());
    CHECK_EQ(mismatch, 0ull);
}

// ---- 5: FSR 1 upscales a gradient ---------------------------------------------------------------------------------------------------

NXTEST(fsr1_upscales_a_gradient) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM)) {
        nxtest::skip("needed format not supported");
        return;
    }
    constexpr uint32_t S = 32, D = 64;
    std::vector<uint8_t> src(S * S * 4);
    for (uint32_t y = 0; y < S; ++y)
        for (uint32_t x = 0; x < S; ++x) {
            uint8_t* px = &src[(static_cast<size_t>(y) * S + x) * 4];
            px[0] = px[1] = px[2] = static_cast<uint8_t>(x * 8);
            px[3] = 255;
        }
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_UNORM, S, S, src));
    REQUIRE(make_dst(r, VK_FORMAT_R8G8B8A8_UNORM, D, D, 4));
    hdr::OutputParams p = base_params(r, S, S);
    p.keep_aspect = true;
    p.out = hdr::OutputKind::Sdr8;
    p.src_linear = false;
    p.fsr1 = true;
    p.sharpness = 0.8f;
    CHECK_EQ(r.pass.fsr_frames(), 0u);
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 0);
    CHECK_EQ(r.pass.fsr_frames(), 1u);
    REQUIRE(got.size() == static_cast<size_t>(D) * D * 4);

    // Bilinear reference: pixel centres mapped to source texel space, clamped to the edge texels. Source value at texel x is 8 x.
    double bil[D];
    for (uint32_t x = 0; x < D; ++x) {
        double sx = (x + 0.5) * S / D - 0.5;
        sx = std::clamp(sx, 0.0, S - 1.0);
        const int x0 = static_cast<int>(std::floor(sx));
        const int x1 = std::min(x0 + 1, static_cast<int>(S) - 1);
        const double f = sx - x0;
        bil[x] = (1.0 - f) * (8.0 * x0) + f * (8.0 * x1);
    }
    int worst_drop = 0;
    size_t drop_bad = 0;
    int first_max = 0, last_min = 255;
    double sum_abs = 0.0;
    int max_abs = 0;
    for (uint32_t y = 0; y < D; ++y) {
        const uint8_t* row = &got[static_cast<size_t>(y) * D * 4];
        first_max = std::max(first_max, static_cast<int>(row[0]));
        last_min = std::min(last_min, static_cast<int>(row[(D - 1) * 4]));
        for (uint32_t x = 1; x < D; ++x) {
            const int drop = static_cast<int>(row[(x - 1) * 4]) - static_cast<int>(row[x * 4]);
            if (drop > 0) {
                worst_drop = std::max(worst_drop, drop);
                if (drop > 3) ++drop_bad;
            }
        }
        for (uint32_t x = 0; x < D; ++x) {
            const int d = std::abs(static_cast<int>(row[x * 4 + 0]) - static_cast<int>(std::lround(bil[x])));
            sum_abs += d;
            max_abs = std::max(max_abs, d);
        }
    }
    const double mean_abs = sum_abs / (static_cast<double>(D) * D);
    std::printf("    fsr1 gradient: largest decrease along a row %d levels (%zu steps of more than 3), first column max %d, last column min %d\n",
                worst_drop, drop_bad, first_max, last_min);
    std::printf("    fsr1 gradient: vs CPU bilinear, mean abs difference %.3f levels, max %d\n", mean_abs, max_abs);
    CHECK(drop_bad == 0);
    CHECK(first_max <= 12);
    CHECK(last_min >= 238);
    CHECK(mean_abs < 6.0);
}

// ---- 6: FSR 1 in HDR keeps highlights -----------------------------------------------------------------------------------------------

NXTEST(fsr1_hdr_keeps_highlights) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    constexpr uint32_t S = 32, D = 64;
    const std::vector<uint8_t> src = pack_half_rgba(S, S, [](uint32_t x, uint32_t, float* v) {
        v[0] = v[1] = v[2] = (x < S / 2) ? 0.2f : 3.0f;
    });
    REQUIRE(upload_source(r, VK_FORMAT_R16G16B16A16_SFLOAT, S, S, src));
    REQUIRE(make_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, D, D, 4));
    hdr::OutputParams p = base_params(r, S, S);
    p.keep_aspect = true;
    p.out = hdr::OutputKind::Hdr10;
    p.src_linear = true;
    p.fsr1 = true;
    CHECK_EQ(r.pass.fsr_frames(), 0u);
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 2);
    CHECK_EQ(r.pass.fsr_frames(), 1u);
    std::vector<float> lin;
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, got.data(), D, D, kPaper, &lin, nullptr, nullptr, nullptr));

    double hi_min = 1e9, hi_max = -1e9, lo_min = 1e9, lo_max = -1e9;
    size_t hi_bad = 0, lo_bad = 0;
    for (uint32_t y = 0; y < D; ++y)
        for (uint32_t x = 0; x < D; ++x) {
            const double g = lin[(static_cast<size_t>(y) * D + x) * 3];   // R channel (the three are equal here)
            if (x >= 40 && x <= 58) {
                hi_min = std::min(hi_min, g);
                hi_max = std::max(hi_max, g);
                if (std::fabs(g - 3.0) > 0.05 * 3.0) ++hi_bad;
            }
            if (x >= 5 && x <= 24) {
                lo_min = std::min(lo_min, g);
                lo_max = std::max(lo_max, g);
                if (std::fabs(g - 0.2) > 0.05 * 0.2) ++lo_bad;
            }
        }
    std::printf("    fsr1 hdr: columns 40..58 decoded %.4f..%.4f (want 3.0 +-5%%), %zu pixels outside\n", hi_min, hi_max, hi_bad);
    std::printf("    fsr1 hdr: columns 5..24 decoded %.4f..%.4f (want 0.2 +-5%%), %zu pixels outside\n", lo_min, lo_max, lo_bad);
    CHECK(hi_bad == 0);
    CHECK(lo_bad == 0);
}

NXTEST(hdr10_highlight_expansion_lifts_display_white) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    // Columns: x<4 = mid-grey (128 -> ~0.216 linear, <= 0.25 knee, stays ~1x paper white);
    //          x>=4 = peak sRGB white (255 -> 1.0 linear, expands to peak/paper = 1200/230 = 5.217).
    std::vector<uint8_t> src(8 * 8 * 4);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const uint8_t v = (x < 4) ? 128 : 255;
            const size_t o = (static_cast<size_t>(y) * 8 + x) * 4;
            src[o + 0] = src[o + 1] = src[o + 2] = v;
            src[o + 3] = 255;
        }
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_SRGB, 8, 8, src));
    REQUIRE(make_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 8, 8, 4));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::Hdr10;
    p.src_linear = true;
    p.paper_white_nits = 230.0f;
    p.peak_nits = 1200.0f;
    p.expand_ratio = 1200.0f / 230.0f;
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 2);

    std::vector<float> lin;
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, got.data(), 8, 8, 230.0f, &lin, nullptr, nullptr, nullptr));
    const double mid = lin[(0 * 8 + 1) * 3];
    const double peak_hi = lin[(0 * 8 + 6) * 3];
    std::printf("    hdr10 expand: sRGB 128 -> %.4f (expected ~0.216), sRGB 255 -> %.4f (expected ~5.217 = 1200 nits)\n", mid, peak_hi);
    CHECK(mid > 0.19 && mid < 0.25);
    CHECK(peak_hi > 5.0 && peak_hi < 5.35);
}

// The UI split: where the copy before the UI differs from the presented picture, the pixel stays at paper white. Both the plain encode
// and the FSR 1 path (2x, the copies read at the source size).
NXTEST(hdr10_highlight_expansion_skips_ui) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    std::vector<uint8_t> scene, fin;
    white_with_ui_on_the_right(&scene, &fin);
    REQUIRE(upload_scene_and_final(r, scene, fin));
    for (const uint32_t scale : {1u, 2u}) {
        const uint32_t w = 8 * scale;
        REQUIRE(remake_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, w, 4));
        hdr::OutputParams p = base_params(r, 8, 8);
        p.out = hdr::OutputKind::Hdr10;
        p.src_linear = true;
        p.paper_white_nits = 230.0f;
        p.peak_nits = 1200.0f;
        p.expand_ratio = 1200.0f / 230.0f;
        p.scene = r.scene.view;
        p.fsr1 = scale > 1;
        std::vector<uint8_t> got;
        CHECK_EQ(run_pass(r, p, &got), 2);
        std::vector<float> lin;
        REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, got.data(), w, w, 230.0f, &lin, nullptr, nullptr, nullptr));
        const double scene_px = lin[(1 * w + 1 * scale) * 3];
        const double ui_px = lin[(1 * w + 6 * scale) * 3];
        std::printf("    scale %u: scene white -> %.4f (expected ~5.217), UI white -> %.4f (expected ~1.0)\n", scale, scene_px, ui_px);
        CHECK(scene_px > 5.0 && scene_px < 5.35);
        CHECK(ui_px > 0.95 && ui_px < 1.05);
    }
}

// ---- the highlight expansion and the picture controls (CPU reference of the encode shader) --------------------------------------------


// The expansion through the FSR 1 path (2x): white still reaches the peak, mid grey stays at its level.
NXTEST(hdr10_highlight_expansion_through_fsr1) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    const uint8_t v[8] = {128, 128, 128, 128, 255, 255, 255, 255};
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_SRGB, 8, 8, grey_columns(v)));
    REQUIRE(make_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 16, 16, 4));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::Hdr10;
    p.src_linear = true;
    p.paper_white_nits = 230.0f;
    p.peak_nits = 1200.0f;
    p.expand_ratio = 1200.0f / 230.0f;
    p.fsr1 = true;
    std::vector<uint8_t> got;
    CHECK_EQ(run_pass(r, p, &got), 2);
    CHECK_EQ(r.pass.fsr_frames(), 1u);
    std::vector<float> lin;
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, got.data(), 16, 16, 230.0f, &lin, nullptr, nullptr, nullptr));
    const double mid = lin[(4 * 16 + 2) * 3], hi = lin[(4 * 16 + 13) * 3];
    std::printf("    fsr1 expand: sRGB 128 -> %.4f (expected ~0.216), sRGB 255 -> %.4f (expected ~5.217)\n", mid, hi);
    CHECK(mid > 0.19 && mid < 0.25);
    CHECK(hi > 5.0 && hi < 5.35);
}

// The expansion on scRGB: the levels of the formula, limited by the peak.
NXTEST(scrgb_highlight_expansion) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R16G16B16A16_SFLOAT)) {
        nxtest::skip("needed format not supported");
        return;
    }
    const uint8_t v[8] = {0, 64, 128, 180, 210, 235, 250, 255};
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_SRGB, 8, 8, grey_columns(v)));
    REQUIRE(make_dst(r, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8, 8));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::ScRgb;
    p.src_linear = true;
    p.expand_ratio = 1.0f + 0.4f * (kPeak / kPaper - 1.0f);
    std::vector<uint8_t> got;
    CHECK_EQ(run_pass(r, p, &got), 3);
    std::vector<float> lin;
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::Rgba16F, got.data(), 8, 8, kPaper, &lin, nullptr, nullptr, nullptr));
    size_t bad = 0;
    for (uint32_t x = 0; x < 8; ++x) {
        const double want = std::min(cpu_hdr_grey(v[x], nullptr, p.expand_ratio), static_cast<double>(kPeakRatio));
        const double g = lin[(3 * 8 + x) * 3];
        if (!close_to(g, want, 0.01, 0.002)) {
            ++bad;
            std::printf("    column %u: got %.4f, expected %.4f\n", x, g, want);
        }
    }
    CHECK_EQ(bad, 0ull);
}

// The picture controls in the SDR copy mode, against the formula.
NXTEST(picture_controls_sdr) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM)) {
        nxtest::skip("needed format not supported");
        return;
    }
    const uint8_t v[8] = {0, 16, 64, 100, 128, 180, 230, 255};
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, grey_columns(v)));
    REQUIRE(make_dst(r, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 4));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::Sdr8;
    p.picture = true;
    const float adj[4] = {1.1f, 1.15f, -0.03f, 1.2f};
    for (int i = 0; i < 4; ++i) p.pic[i] = adj[i];
    std::vector<uint8_t> got;
    CHECK_EQ(run_pass(r, p, &got), 0);
    size_t bad = 0;
    for (uint32_t x = 0; x < 8; ++x) {
        const double want = std::clamp(cpu_picture(v[x] / 255.0, adj), 0.0, 1.0) * 255.0;
        const int g = got[(3 * 8 + x) * 4];
        if (std::fabs(g - want) > 1.01) {
            ++bad;
            std::printf("    column %u: got %d, expected %.2f\n", x, g, want);
        }
    }
    CHECK_EQ(bad, 0ull);
}

// The picture controls and the expansion together on HDR10, against the formula.
NXTEST(picture_controls_hdr10_with_expansion) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    const uint8_t v[8] = {16, 64, 100, 128, 180, 210, 235, 255};
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_SRGB, 8, 8, grey_columns(v)));
    REQUIRE(make_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 8, 8, 4));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::Hdr10;
    p.src_linear = true;
    p.expand_ratio = 1.0f + 0.4f * (kPeak / kPaper - 1.0f);
    p.picture = true;
    const float adj[4] = {1.03f, 1.15f, -0.02f, 1.0f};
    for (int i = 0; i < 4; ++i) p.pic[i] = adj[i];
    std::vector<uint8_t> got;
    CHECK_EQ(run_pass(r, p, &got), 2);
    std::vector<float> lin;
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, got.data(), 8, 8, kPaper, &lin, nullptr, nullptr, nullptr));
    size_t bad = 0;
    for (uint32_t x = 0; x < 8; ++x) {
        const double want = std::min(cpu_hdr_grey(v[x], adj, p.expand_ratio), static_cast<double>(kPeakRatio));
        const double g = lin[(3 * 8 + x) * 3];
        if (!close_to(g, want, 0.02, 0.01)) {
            ++bad;
            std::printf("    column %u: got %.4f, expected %.4f\n", x, g, want);
        }
    }
    CHECK_EQ(bad, 0ull);
}

// A copy before the UI that equals the presented picture (no UI drawn) gives exactly the picture of no copy at all, with the picture
// controls on, in both HDR formats and through FSR 1.
NXTEST(ui_split_without_ui_changes_nothing) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32) ||
        !formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R16G16B16A16_SFLOAT)) {
        nxtest::skip("needed format not supported");
        return;
    }
    std::vector<uint8_t> src(8 * 8 * 4);
    uint32_t s = 777u;
    for (size_t i = 0; i < src.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            s = s * 1664525u + 1013904223u;
            src[i + c] = static_cast<uint8_t>(s >> 24);
        }
        src[i + 3] = 255;
    }
    REQUIRE(upload_scene_and_final(r, src, src));
    struct Case {
        VkFormat fmt;
        uint32_t bpp;
        hdr::OutputKind out;
        uint32_t size;
    };
    const Case cases[] = {{VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, hdr::OutputKind::Hdr10, 8},
                          {VK_FORMAT_R16G16B16A16_SFLOAT, 8, hdr::OutputKind::ScRgb, 8},
                          {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, hdr::OutputKind::Hdr10, 16}};
    for (const Case& c : cases) {
        REQUIRE(remake_dst(r, c.fmt, c.size, c.bpp));
        hdr::OutputParams p = base_params(r, 8, 8);
        p.out = c.out;
        p.src_linear = true;
        p.expand_ratio = 1.0f + 0.8f * (kPeak / kPaper - 1.0f);
        p.fsr1 = c.size > 8;
        p.picture = true;
        const float adj[4] = {1.1f, 1.15f, -0.03f, 1.0f};
        for (int i = 0; i < 4; ++i) p.pic[i] = adj[i];
        std::vector<uint8_t> plain, split;
        REQUIRE(run_pass(r, p, &plain) >= 0);
        p.scene = r.scene.view;
        REQUIRE(run_pass(r, p, &split) >= 0);
        size_t diff = 0;
        for (size_t i = 0; i < plain.size(); ++i)
            if (plain[i] != split[i]) ++diff;
        std::printf("    format %d, %ux%u: %zu of %zu bytes differ\n", static_cast<int>(c.fmt), c.size, c.size, diff, plain.size());
        CHECK_EQ(diff, 0ull);
    }
}

// The UI brightness: UI pixels go to ui_scale x paper white, scene pixels keep the expansion (or stay at 1 with no expansion). Without
// a copy before the UI, ui_scale changes nothing.
NXTEST(hdr10_ui_brightness) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    std::vector<uint8_t> scene, fin;
    white_with_ui_on_the_right(&scene, &fin);
    REQUIRE(upload_scene_and_final(r, scene, fin));
    REQUIRE(make_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 8, 8, 4));
    struct Case {
        float expand, ui;
        bool copy;
        double want_scene, want_ui;
    };
    const Case cases[] = {{1200.0f / 230.0f, 0.7f, true, 5.217, 0.7},
                          {1.0f, 1.5f, true, 1.0, 1.5},
                          {1.0f, 1.5f, false, 1.0, 1.0},
                          {1200.0f / 230.0f, 0.7f, false, 5.217, 5.217}};
    for (const Case& c : cases) {
        hdr::OutputParams p = base_params(r, 8, 8);
        p.out = hdr::OutputKind::Hdr10;
        p.src_linear = true;
        p.paper_white_nits = 230.0f;
        p.peak_nits = 1200.0f;
        p.expand_ratio = c.expand;
        p.ui_scale = c.ui;
        p.scene = c.copy ? r.scene.view : VK_NULL_HANDLE;
        std::vector<uint8_t> got;
        CHECK_EQ(run_pass(r, p, &got), 2);
        std::vector<float> lin;
        REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, got.data(), 8, 8, 230.0f, &lin, nullptr, nullptr, nullptr));
        const double sp = lin[(1 * 8 + 1) * 3], up = lin[(1 * 8 + 6) * 3];
        std::printf("    expand %.2f, ui %.2f, copy %d: scene %.4f (want %.3f), UI %.4f (want %.3f)\n", c.expand, c.ui, c.copy ? 1 : 0, sp,
                    c.want_scene, up, c.want_ui);
        CHECK(close_to(sp, c.want_scene, 0.03, 0.01));
        CHECK(close_to(up, c.want_ui, 0.03, 0.01));
    }
}

// The game's kept highlights (src_encoded): a float source with display-encoded values above 1 is decoded (sRGB up to 1, a 2.2 power above),
// the part above paper white is fitted under highlight_ratio; below 1 nothing changes.
NXTEST(hdr10_kept_highlights) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    const float enc[8] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 4.0f};
    REQUIRE(upload_source(r, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8, pack_half_rgba(8, 8, [&](uint32_t x, uint32_t, float* v) {
                              v[0] = v[1] = v[2] = enc[x];
                          })));
    REQUIRE(make_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 8, 8, 4));
    auto srgb = [](double e) { return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4); };
    struct Case { float ratio; bool clip, deband; };
    for (const Case c : {Case{3.0f, false, false}, Case{1.0f, false, false}, Case{5.0f, false, false}, Case{5.0f, true, false}, Case{5.0f, false, true}}) {
        const float ratio = c.ratio;
        hdr::OutputParams p = base_params(r, 8, 8);
        p.out = hdr::OutputKind::Hdr10;
        p.src_linear = true;
        p.src_encoded = true;
        p.highlight_ratio = ratio;
        p.clip_encoded = c.clip;
        p.deband = c.deband;   // its noise must leave the highlights above white alone
        p.paper_white_nits = 200.0f;
        p.peak_nits = 1000.0f;
        std::vector<uint8_t> got;
        CHECK_EQ(run_pass(r, p, &got), 2);
        std::vector<float> lin;
        REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, got.data(), 8, 8, 200.0f, &lin, nullptr, nullptr, nullptr));
        for (uint32_t x = 0; x < 8; ++x) {
            double want = c.clip ? srgb(std::min(enc[x], 1.0f)) : enc[x] <= 1.0f ? srgb(enc[x]) : std::pow(enc[x], 2.2);
            if (want > 1.0) {
                const double a = std::max(ratio - 1.0, 1e-4);
                want = 1.0 + a * (1.0 - std::exp(-(want - 1.0) / a));
            }
            const double g = lin[(3 * 8 + x) * 3 + 1];
            std::printf("    ratio %.1f, clip %d, deband %d, encoded %.2f: %.4f (want %.4f)\n", ratio, c.clip ? 1 : 0, c.deband ? 1 : 0, enc[x], g, want);
            CHECK(close_to(g, want, 0.02, c.deband ? 0.01 : 0.003));
        }
    }
}

// Debanding: noise of at most one 8-bit step on the sRGB code, averaging to the plain picture; pure black and white exactly as without it.
NXTEST(hdr10_deband) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    const uint8_t v[8] = {0, 1, 40, 90, 128, 200, 254, 255};
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_SRGB, 8, 8, grey_columns(v)));
    REQUIRE(make_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 8, 8, 4));
    auto run = [&](bool deband, float expand, std::vector<uint8_t>* got) {
        hdr::OutputParams p = base_params(r, 8, 8);
        p.out = hdr::OutputKind::Hdr10;
        p.src_linear = true;
        p.paper_white_nits = 230.0f;
        p.peak_nits = 1200.0f;
        p.expand_ratio = expand;
        p.deband = deband;
        return run_pass(r, p, got);
    };
    std::vector<uint8_t> off, on;
    // no expansion: the output decodes back to the source's sRGB code
    REQUIRE(run(false, 1.0f, &off) == 2);
    REQUIRE(run(true, 1.0f, &on) == 2);
    std::vector<float> loff, lon;
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, off.data(), 8, 8, 230.0f, &loff, nullptr, nullptr, nullptr));
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, on.data(), 8, 8, 230.0f, &lon, nullptr, nullptr, nullptr));
    int changed = 0;
    for (uint32_t x = 0; x < 8; ++x) {
        double sum = 0, worst = 0;
        for (uint32_t y = 0; y < 8; ++y) {
            const size_t i = (static_cast<size_t>(y) * 8 + x) * 4;
            const size_t c = (static_cast<size_t>(y) * 8 + x) * 3;
            if (x == 0 || x == 7) CHECK(std::memcmp(&off[i], &on[i], 4) == 0);   // black and white untouched
            if (std::memcmp(&off[i], &on[i], 4) != 0) ++changed;
            const double d = (hdr::srgb_oetf(lon[c]) - hdr::srgb_oetf(loff[c])) * 255.0;
            sum += d;
            worst = std::max(worst, std::fabs(d));
        }
        std::printf("    column %u (code %u): mean shift %.3f steps, largest %.3f steps\n", x, v[x], sum / 8.0, worst);
        if (x >= 2 && x <= 6) {
            CHECK(worst <= 1.3);                  // one step, plus the 10-bit PQ rounding
            CHECK(std::fabs(sum / 8.0) <= 0.6);   // the noise averages out
        }
    }
    CHECK(changed >= 16);   // the mid greys do get the noise
    // with the highlight expansion: white still exact
    REQUIRE(run(false, 1200.0f / 230.0f, &off) == 2);
    REQUIRE(run(true, 1200.0f / 230.0f, &on) == 2);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x : {0u, 7u}) {
            const size_t i = (static_cast<size_t>(y) * 8 + x) * 4;
            CHECK(std::memcmp(&off[i], &on[i], 4) == 0);
        }
}

// ---- HDR test patterns (extras/hdr_pattern.h) ----------------------------------------------------------------------------------------

namespace {
// The shader's tiles at destination pixel (x, y) of a w x h picture: the distance from the tile centre (max norm, 0..0.5) and the tile.
double pattern_tile(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int* cx, int* cy) {
    const double tx = (x + 0.5) / w * 8.0, ty = (y + 0.5) / h * 4.5 + 0.25;
    *cx = static_cast<int>(std::floor(tx));
    *cy = static_cast<int>(std::floor(ty));
    return std::max(std::fabs(tx - std::floor(tx) - 0.5), std::fabs(ty - std::floor(ty) - 0.5));
}
bool near_edge(double m) { return std::fabs(m - 0.07) < 0.01 || std::fabs(m - 0.13) < 0.01 || std::fabs(m - 0.3) < 0.01; }
uint32_t red_code(const std::vector<uint8_t>& px, size_t i) {
    uint32_t v;
    std::memcpy(&v, &px[i * 4], 4);
    return v & 1023u;
}
}  // namespace

// Peak: 10000 nits around the peak being tried, on black, with no peak limit; black: codes 1..16 through the encode; SDR: no pattern.
NXTEST(hdr10_test_patterns) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    const uint8_t v[8] = {128, 128, 128, 128, 128, 128, 128, 128};
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_SRGB, 8, 8, grey_columns(v)));
    const uint32_t W = 256, H = 144;
    REQUIRE(make_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, W, H, 4));
    auto params = [&](hdr::HdrPattern kind, float nits) {
        hdr::OutputParams p = base_params(r, 8, 8);
        p.keep_aspect = false;
        p.out = hdr::OutputKind::Hdr10;
        p.src_linear = true;
        p.paper_white_nits = 200.0f;
        p.peak_nits = 1000.0f;
        p.expand_ratio = 2.6f;
        p.deband = true;
        p.pattern.kind = kind;
        p.pattern.peak_nits = nits;
        return p;
    };
    const uint32_t c10000 = 1023, c0 = 0;
    for (float tried : {600.0f, 0.0f}) {   // 0: the current peak (1000)
        std::vector<uint8_t> got;
        REQUIRE(run_pass(r, params(hdr::HdrPattern::Peak, tried), &got) == 2);
        const uint32_t cin = static_cast<uint32_t>(std::lround(hdr::pq_oetf(tried > 0 ? tried : 1000.0f) * 1023.0));
        int bad = 0, inner = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                int cx, cy;
                const double m = pattern_tile(x, y, W, H, &cx, &cy);
                if (near_edge(m)) continue;
                const uint32_t want = m < 0.07 ? cin : (m < 0.13 ? c10000 : c0);
                const uint32_t have = red_code(got, static_cast<size_t>(y) * W + x);
                inner += m < 0.07;
                if (have + 1 < want || have > want + 1) ++bad;
            }
        int lit = 0;
        for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i) lit += red_code(got, i) > 0;
        std::printf("    peak pattern at %.0f nits: inner code %u, %d inner pixels, %d pixels off, %.1f%% of the screen lit\n", tried, cin, inner, bad,
                    100.0 * lit / (W * H));
        CHECK(lit * 10 < static_cast<int>(W * H));   // under 10%: the display's peak, not its limit for a large bright area
        CHECK(inner > 0);
        CHECK_EQ(bad, 0);
    }
    {
        std::vector<uint8_t> got;
        REQUIRE(run_pass(r, params(hdr::HdrPattern::Black, 0.0f), &got) == 2);
        std::vector<float> lin;
        REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, got.data(), W, H, 200.0f, &lin, nullptr, nullptr, nullptr));
        const double codes[8] = {1, 2, 3, 4, 6, 8, 12, 16};
        int bad = 0, black_not_zero = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                int cx, cy;
                const double m = pattern_tile(x, y, W, H, &cx, &cy);
                if (near_edge(m)) continue;
                const size_t i = static_cast<size_t>(y) * W + x;
                if (m >= 0.3) {
                    black_not_zero += red_code(got, i) != 0;
                    continue;
                }
                const double want = hdr::srgb_eotf(static_cast<float>(codes[(cx + cy * 3) % 8] / 255.0));
                if (!close_to(lin[i * 3], want, 0.05, 0.0005)) ++bad;   // no expansion, no deband on the pattern
            }
        std::printf("    black pattern: %d squares pixels off, %d background pixels not black\n", bad, black_not_zero);
        CHECK_EQ(bad, 0);
        CHECK_EQ(black_not_zero, 0);
    }
    {   // staircase: 8 bars of 300..1000 nits along the top 7%, black elsewhere
        std::vector<uint8_t> got;
        REQUIRE(run_pass(r, params(hdr::HdrPattern::Staircase, 0.0f), &got) == 2);
        int bad = 0, lit = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const double lx = (x + 0.5) / W, ly = (y + 0.5) / H, f = lx * 8.0 - std::floor(lx * 8.0);
                if (std::fabs(ly - 0.07) < 0.01 || std::fabs(f - 0.04) < 0.02 || std::fabs(f - 0.96) < 0.02) continue;
                const bool on = ly < 0.07 && f > 0.04 && f < 0.96;
                const uint32_t want = on ? static_cast<uint32_t>(std::lround(hdr::pq_oetf(static_cast<float>(300.0 + 100.0 * std::floor(lx * 8.0))) * 1023.0)) : 0u;
                const uint32_t have = red_code(got, static_cast<size_t>(y) * W + x);
                lit += on;
                if (have + 1 < want || have > want + 1) ++bad;
            }
        const size_t row = static_cast<size_t>(2) * W;
        std::printf("    staircase: %d bar pixels, %d pixels off, codes %u .. %u\n", lit, bad, red_code(got, row + W / 16), red_code(got, row + W - W / 16));
        CHECK(lit > 0);
        CHECK_EQ(bad, 0);
        CHECK(red_code(got, row + W - W / 16) > red_code(got, row + W / 16));   // brighter to the right
    }
    {   // no pattern: the picture
        std::vector<uint8_t> a, b;
        hdr::OutputParams p = params(hdr::HdrPattern::None, 0.0f);
        REQUIRE(run_pass(r, p, &a) == 2);
        p.deband = false;
        REQUIRE(run_pass(r, p, &b) == 2);
        const uint32_t code_mid = red_code(b, 0);
        CHECK(code_mid > 300 && code_mid < 700);   // grey 128, not a pattern
    }
}

NXTEST(sdr_ignores_test_patterns) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM)) {
        nxtest::skip("needed format not supported");
        return;
    }
    const uint8_t v[8] = {0, 30, 60, 90, 120, 160, 200, 255};
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_SRGB, 8, 8, grey_columns(v)));
    REQUIRE(make_dst(r, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 4));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::Sdr8;
    p.src_linear = true;
    std::vector<uint8_t> plain, with;
    REQUIRE(run_pass(r, p, &plain) == 1);
    p.pattern.kind = hdr::HdrPattern::Peak;
    p.pattern.peak_nits = 600.0f;
    REQUIRE(run_pass(r, p, &with) == 1);
    CHECK(plain == with);
}

NXTEST(hdr_pattern_lasts_while_asked) {
    CHECK(hdr::hdr_pattern().kind == hdr::HdrPattern::None);
    hdr::set_hdr_pattern(hdr::HdrPattern::Peak, 650.0f);
    hdr::HdrPatternLive l = hdr::hdr_pattern();
    CHECK(l.kind == hdr::HdrPattern::Peak);
    CHECK(l.peak_nits == 650.0f);
    hdr::set_hdr_pattern(hdr::HdrPattern::Black, -5.0f);
    l = hdr::hdr_pattern();
    CHECK(l.kind == hdr::HdrPattern::Black);
    CHECK(l.peak_nits == 0.0f);
    hdr::set_hdr_pattern(hdr::HdrPattern::Staircase, 0.0f);
    CHECK(hdr::hdr_pattern().kind == hdr::HdrPattern::Staircase);
    hdr::set_hdr_pattern(static_cast<hdr::HdrPattern>(7), 0.0f);   // not a pattern
    CHECK(hdr::hdr_pattern().kind == hdr::HdrPattern::None);
    hdr::set_hdr_pattern(hdr::HdrPattern::Peak, 650.0f);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));   // the menu stopped asking
    CHECK(hdr::hdr_pattern().kind == hdr::HdrPattern::None);
}

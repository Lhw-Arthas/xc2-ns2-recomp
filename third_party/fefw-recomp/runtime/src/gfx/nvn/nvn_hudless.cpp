// SPDX-License-Identifier: GPL-3.0-or-later
// The scene without the UI (nvn_hudless.h).
#include "nvn_hudless.h"

#include <string>
#include <vector>

#include "nxrt/extras.h"
#include "nxrt/log.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "hudless";

struct State {
    std::vector<Texture*> window;      // the textures of the window last presented
    TrackedImage copy[2];
    uint32_t cur = 0;                  // the image this frame's copy goes to
    Texture* captured = nullptr;       // this frame's copy is of this texture
    TrackedImage* shown = nullptr;     // the copy the last present used (kept for repeated presents)
    bool want_frame = false, want_always = false;
    bool frame_failed = false;         // a capture failed this frame: no other draw of it is copied
    bool failed = false;               // the copy image could not be made: no more captures for want_always
    uint64_t captures = 0;
};
State& st() {
    static State s;
    return s;
}

VkFormat unorm_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return f;
    }
}

VkFormat srgb_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
        default: return f;
    }
}

bool is_window_texture(const State& s, Texture* t) {
    for (Texture* w : s.window)
        if (w == t) return true;
    return false;
}

}  // namespace

bool TrackedImage::ensure(VkExtent2D e, VkFormat fmt, VkImageUsageFlags usage, const char* name) {
    if (img.image && img.extent.width == e.width && img.extent.height == e.height && img.format == fmt) return true;
    if (img.image) {
        gfx::Image old = img;
        defer_destroy([old]() mutable { device().destroy_image(old); });
    }
    *this = TrackedImage();
    gfx::ImageDesc d;
    d.format = fmt;
    d.width = e.width;
    d.height = e.height;
    d.usage = usage;
    d.name = name;
    std::string why;
    if (device().create_image(d, &img, &why) != VK_SUCCESS) {
        NXLOG_E(kTag, "%s %ux%u: not made (%s)", name, e.width, e.height, why.c_str());
        *this = TrackedImage();
        return false;
    }
    return true;
}

void TrackedImage::to(VkCommandBuffer cb, VkImageLayout l) {
    image_barrier(cb, img.image, layout, l, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
    layout = l;
}

VkFormat hudless_copy_format(VkFormat tex_fmt, bool scene_hdr) {
    // Match nvn_present's view choice: SDR passes UNORM bytes as-is; HDR output decodes 8-bit display-referred buffers through their sRGB view.
    return scene_hdr ? srgb_of(tex_fmt) : unorm_of(tex_fmt);
}

void hudless_want_this_frame() {
    State& s = st();
    if (!s.want_frame) s.captured = nullptr;   // a copy made before the first ask is redone at the next blended draw
    s.frame_failed = false;                    // each ask tries again after a failed copy
    s.want_frame = true;
}

void hudless_want_every_frame(bool on) { st().want_always = on; }

bool hudless_candidate(Texture* t, bool blended) {
    const State& s = st();
    return blended && (s.want_frame || (s.want_always && !s.failed)) && !s.frame_failed && !s.captured && t && is_window_texture(s, t);
}

void hudless_capture(Frame& f, Texture* t) {
    State& s = st();
    if (!tex_gpu(t) || tex_is_depth(t) || !tex_copy_source_ok(t)) {
        s.frame_failed = true;
        return;
    }
    const VkExtent3D e = tex_image_extent(t, 0);
    const VkFormat fmt = hudless_copy_format(tex_vk_format(t), extras::hdr_state().scene_hdr);
    TrackedImage& c = s.copy[s.cur];
    if (!c.ensure(VkExtent2D{e.width, e.height}, fmt, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  "hudless copy")) {
        s.frame_failed = true;
        s.failed = true;
        return;
    }
    tex_transition(t, f, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    c.to(f.cb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy r = {};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = e;
    vkCmdCopyImage(f.cb, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    memory_barrier(f.cb);
    s.captured = t;
    ++s.captures;
}

bool hudless_failed_this_frame() { return st().frame_failed; }

uint64_t hudless_capture_count() { return st().captures; }

VkImageView hudless_view(Texture* presented, Frame& f, bool again) {
    State& s = st();
    if (!again) s.shown = (presented && s.captured == presented) ? &s.copy[s.cur] : nullptr;
    if (!s.shown || !s.shown->img.image) return VK_NULL_HANDLE;
    s.shown->to(f.cb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return s.shown->img.view;
}

void hudless_end_present(Window* w, bool again) {
    State& s = st();
    if (w) {
        s.window.clear();
        for (const auto& t : w->textures) s.window.push_back(t.get());
    }
    if (s.captured && !again) s.cur ^= 1u;   // keep the copy just presented while the next frame fills the other one
    s.captured = nullptr;
    s.want_frame = false;
    s.frame_failed = false;
}

void hudless_shutdown() {
    State& s = st();
    for (TrackedImage& c : s.copy)
        if (c.img.image) device().destroy_image(c.img);
    s = State();
}

}  // namespace nxrt::nvn::be::gpu

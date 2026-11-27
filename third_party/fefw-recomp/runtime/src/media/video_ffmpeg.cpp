// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_ffmpeg.h"

#include <cstdio>
#include <mutex>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/gfx_tuning.h"

namespace nxrt::gfx {
namespace {
std::mutex g_render_gpu_mu;
RenderGpuInfo g_render_gpu;
}  // namespace

void note_render_gpu_info(int index, uint32_t vendor_id, const char* name) {
    std::lock_guard<std::mutex> lk(g_render_gpu_mu);
    g_render_gpu.vulkan_index = index;
    g_render_gpu.vendor_id = vendor_id;
    std::snprintf(g_render_gpu.name, sizeof g_render_gpu.name, "%s", name ? name : "");
}

RenderGpuInfo render_gpu_info() {
    std::lock_guard<std::mutex> lk(g_render_gpu_mu);
    return g_render_gpu;
}

void prewarm_hwdec_async() {
    static std::once_flag once;
    if (!nxrt::config_get_bool("media", "hwdec", true)) return;
    if (nxrt::config_get_str("media", "video_decoder", "library") == "off") return;
    std::call_once(once, [] {
        std::thread([] { nxrt::media::ffdec_prewarm(); }).detach();
    });
}
}  // namespace nxrt::gfx

#if NXRT_HAVE_FFMPEG
#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>

#include "nxrt/config.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/log.h"

namespace nxrt::media {
namespace {
constexpr const char* kTag = "ffdec";
unsigned long long s_calls = 0, s_pics = 0;   // for the "packets N, pictures M" line every 30 packets

AVCodecID codec_id(int codec) {
    switch (codec) {
        case 9: return AV_CODEC_ID_VP9;
        case 8: return AV_CODEC_ID_VP8;
        default: return AV_CODEC_ID_AV1;
    }
}
// [media] hwdec_backend: which hardware decode the film uses. d3d11va is the long-standing path; vulkan goes through
// FFmpeg's Vulkan Video decode, which on AMD avoids whatever makes D3D11VA hand back mosaic frames at 4K (2026-11-21).
// Both end in the same av_hwframe_transfer_data -> NV12 copy; the CPU decoders stay the fallback for everything.
struct HwBackendPref {
    AVHWDeviceType type = AV_HWDEVICE_TYPE_VULKAN;
    bool explicit_choice = false;
};

HwBackendPref hw_device_pref() {
    static const HwBackendPref pref = [] {
        HwBackendPref p;
        std::string b = config_get_str("media", "hwdec_backend", "auto");
        NXLOG_I(kTag, "hwdec_backend = '%s'", b.c_str());
        for (char& c : b)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (b == "d3d11va" || b == "d3d11") {
            p.type = AV_HWDEVICE_TYPE_D3D11VA;
            p.explicit_choice = true;
        } else if (b == "vulkan") {
            p.type = AV_HWDEVICE_TYPE_VULKAN;
            p.explicit_choice = true;
        } else {
            p.type = AV_HWDEVICE_TYPE_VULKAN;   // auto: Vulkan Video first, D3D11VA fallback
            p.explicit_choice = false;
        }
        return p;
    }();
    return pref;
}
// What was actually created: auto starts with Vulkan Video and falls back to D3D11VA, and the logs should say which one ran.
AVHWDeviceType g_used_type = AV_HWDEVICE_TYPE_NONE;

// One hardware decode device for the whole process. Creating it is the expensive part of the first frame: on 0.1.4 the
// presenting thread sat 183 ms inside a 213 ms frame at the start of a film, waiting for the first picture while the
// worker created the device under the decode. ffdec_prewarm() builds it during the loading screen instead, and every
// stream takes a reference to the same one; an unused device is one COM object and no GPU work.
std::mutex g_dev_mu;
AVBufferRef* g_dev = nullptr;

AVBufferRef* shared_device() {
    std::lock_guard<std::mutex> lk(g_dev_mu);
    if (g_dev) return g_dev;
    AVBufferRef* hw = nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    const HwBackendPref pref = hw_device_pref();
    AVHWDeviceType t = pref.type;
    const gfx::RenderGpuInfo rgpu = gfx::render_gpu_info();

    // Intel Vulkan Video (igvkMedia64.dll) crashes with 0xC0000005 on 4K AV1 cutscenes (#63, #87).
    // On auto, skip Vulkan Video when the render GPU itself is Intel; on dual-GPU laptops, pass the
    // render GPU's Vulkan device index so FFmpeg never silently picks GPU 0 (the Intel iGPU).
    if (t == AV_HWDEVICE_TYPE_VULKAN && !pref.explicit_choice && rgpu.vendor_id == 0x8086u) {
        NXLOG_I(kTag, "Intel render GPU (%s): skipping Vulkan Video (igvkMedia64 crash, #63/#87); trying D3D11VA",
                rgpu.name[0] ? rgpu.name : "0x8086");
        t = AV_HWDEVICE_TYPE_D3D11VA;
    }

    const std::string vk_dev_arg =
        (t == AV_HWDEVICE_TYPE_VULKAN && rgpu.vulkan_index >= 0) ? std::to_string(rgpu.vulkan_index) : std::string();
    const char* dev_cstr = vk_dev_arg.empty() ? nullptr : vk_dev_arg.c_str();

    if (av_hwdevice_ctx_create(&hw, t, dev_cstr, nullptr, 0) < 0) {
        if (t != AV_HWDEVICE_TYPE_VULKAN) return nullptr;
        // No Vulkan Video on the render GPU or driver: fall back to D3D11VA (never retry Vulkan with nullptr,
        // which would silently land on GPU 0 / Intel iGPU on Optimus laptops).
        t = AV_HWDEVICE_TYPE_D3D11VA;
        if (av_hwdevice_ctx_create(&hw, t, nullptr, nullptr, 0) < 0) return nullptr;
        NXLOG_I(kTag, "Vulkan Video is not available on GPU %d; using D3D11VA", rgpu.vulkan_index);
    }
    g_used_type = t;
    g_dev = hw;   // process lifetime: each decoder holds its own reference to it
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (t == AV_HWDEVICE_TYPE_VULKAN && rgpu.vulkan_index >= 0) {
        NXLOG_I(kTag, "Vulkan Video device (GPU %d: %s) in %lld ms (shared by every stream)",
                rgpu.vulkan_index, rgpu.name[0] ? rgpu.name : "?", static_cast<long long>(ms));
    } else {
        NXLOG_I(kTag, "%s device in %lld ms (shared by every stream)", t == AV_HWDEVICE_TYPE_VULKAN ? "Vulkan Video" : "D3D11VA",
                static_cast<long long>(ms));
    }
    return g_dev;
}

enum AVPixelFormat pick_fmt(AVCodecContext*, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_D3D11 || *p == AV_PIX_FMT_VULKAN || *p == AV_PIX_FMT_CUDA) return *p;
    return fmts[0];
}
}  // namespace

struct FfDec {
    AVCodecContext* ctx = nullptr;
    AVBufferRef* hw = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* sw = nullptr;
    bool failed = false;
};

bool ffdec_available(std::string* why) {
    if (!avcodec_find_decoder(AV_CODEC_ID_AV1)) { if (why) *why = "avcodec has no AV1 decoder"; return false; }
    if (!shared_device()) { if (why) *why = "no D3D11VA device"; return false; }
    return true;
}

FfDec* ffdec_create(int codec, std::string* why) {
    // By name: avcodec_find_decoder(AV_CODEC_ID_AV1) can answer with libdav1d, which has no hardware decoding at all.
    // The native "av1" decoder is the one with the D3D11VA hwaccel; "vp9"/"vp8" likewise.
    const char* want = codec == 10 ? "av1" : codec == 9 ? "vp9" : "vp8";
    const AVCodec* c = avcodec_find_decoder_by_name(want);
    if (!c) c = avcodec_find_decoder(codec_id(codec));
    if (!c) { if (why) *why = "no decoder for this codec"; return nullptr; }
    auto* d = new FfDec();
    d->ctx = avcodec_alloc_context3(c);
    if (!d->ctx) { delete d; return nullptr; }
    AVBufferRef* shared = shared_device();
    if (!shared) {
        avcodec_free_context(&d->ctx);
        delete d;
        if (why) *why = "D3D11VA device creation failed";
        return nullptr;
    }
    d->hw = av_buffer_ref(shared);
    d->ctx->hw_device_ctx = av_buffer_ref(d->hw);
    d->ctx->get_format = pick_fmt;
    d->ctx->thread_count = 0;
    // One picture per packet is what the movie library's pool expects (the dav1d path decodes with max_frame_delay 1);
    // without this the native decoder holds frames back and the picture never advances.
    d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(d->ctx, c, nullptr) < 0) {
        av_buffer_unref(&d->hw);
        avcodec_free_context(&d->ctx);
        delete d;
        if (why) *why = "avcodec_open2 failed";
        return nullptr;
    }
    d->frame = av_frame_alloc();
    d->sw = av_frame_alloc();
    d->pkt = av_packet_alloc();
    NXLOG_I(kTag, "libavcodec %s on %s", c->name, ffdec_hw_name());
    return d;
}

bool ffdec_prewarm() { return shared_device() != nullptr; }
const char* ffdec_hw_name() { return g_used_type == AV_HWDEVICE_TYPE_VULKAN ? "Vulkan Video" : "D3D11VA"; }

bool ffdec_decode(FfDec* d, const uint8_t* data, size_t size, int64_t pts, std::vector<FfDecPicture>* out) {
    if (!d || d->failed || !out) return false;
    if (av_new_packet(d->pkt, static_cast<int>(size)) < 0) return false;
    std::memcpy(d->pkt->data, data, size);
    d->pkt->pts = pts;
    if (avcodec_send_packet(d->ctx, d->pkt) < 0) { av_packet_unref(d->pkt); d->failed = true; return false; }
    av_packet_unref(d->pkt);
    for (;;) {
        const int r = avcodec_receive_frame(d->ctx, d->frame);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
        if (r < 0) { d->failed = true; return false; }
        AVFrame* f = d->frame;
        if (f->format == AV_PIX_FMT_D3D11 || f->format == AV_PIX_FMT_VULKAN || f->format == AV_PIX_FMT_CUDA) {
            if (av_hwframe_transfer_data(d->sw, f, 0) < 0) { av_frame_unref(d->frame); d->failed = true; return false; }
            f = d->sw;
        }
        // Once per stream: what the decoder actually handed back. A green picture with a correct-looking AV1 decode is a
        // chroma or colour-matrix problem, and the plane means tell a missing chroma plane (U/V flat at 128 or 0) apart from
        // a range or matrix mismatch (all three planes plausible but shifted). AMD's D3D11VA path is the one that goes green
        // on a plain 8-bit SDR swapchain (2026-11-21, issue: opening film).
        static bool told_first = false;
        if (!told_first && (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_NV12) && f->data[0]) {
            told_first = true;
            auto plane_mean = [](const uint8_t* d, int stride, int w, int h) {
                if (!d || w <= 0 || h <= 0) return -1.0;
                unsigned long long sum = 0;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) sum += d[static_cast<size_t>(y) * stride + x];
                return static_cast<double>(sum) / (static_cast<double>(w) * h);
            };
            const int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
            double u = -1, v = -1, ua = -1;
            if (f->format == AV_PIX_FMT_NV12) {
                // one interleaved plane: U at every other byte, V at the odd bytes
                if (f->data[1]) {
                    unsigned long long su = 0, sv = 0;
                    for (int y = 0; y < ch; ++y) {
                        const uint8_t* r = f->data[1] + static_cast<size_t>(y) * f->linesize[1];
                        for (int x = 0; x < cw; ++x) { su += r[2 * x]; sv += r[2 * x + 1]; }
                    }
                    u = static_cast<double>(su) / (static_cast<double>(cw) * ch);
                    v = static_cast<double>(sv) / (static_cast<double>(cw) * ch);
                }
            } else if (f->data[2]) {
                u = plane_mean(f->data[1], f->linesize[1], cw, ch);
                v = plane_mean(f->data[2], f->linesize[2], cw, ch);
                ua = plane_mean(f->data[2], f->linesize[2], cw, ch);
            }
            NXLOG_I(kTag, "first picture: %dx%d format %d, Y mean %.1f, U mean %.1f, V mean %.1f, strides %d/%d/%d, range %d, primaries %d, space %d",
                    f->width, f->height, static_cast<int>(f->format), plane_mean(f->data[0], f->linesize[0], f->width, f->height), u, v,
                    f->linesize[0], f->linesize[1], f->linesize[2], static_cast<int>(f->color_range),
                    static_cast<int>(f->color_primaries), static_cast<int>(f->colorspace));
        }
        // NXRT_FILM_DUMP=<path>: the first decoded picture as the player will write it (header w,h then the planes). Lets
        // the pixels be checked without guessing which stage turned them green.
        static const std::string dump_path = [] {
            const char* s = std::getenv("NXRT_FILM_DUMP");
            return s ? std::string(s) : std::string();
        }();
        static bool dumped_first = false;
        if (!dump_path.empty() && !dumped_first && (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_NV12) && f->data[0]) {
            dumped_first = true;
            if (FILE* fo = std::fopen(dump_path.c_str(), "wb")) {
                const uint32_t head[2] = {static_cast<uint32_t>(f->width), static_cast<uint32_t>(f->height)};
                std::fwrite(head, sizeof head, 1, fo);
                for (int y = 0; y < f->height; ++y) std::fwrite(f->data[0] + static_cast<size_t>(y) * f->linesize[0], 1, f->width, fo);
                if (f->format == AV_PIX_FMT_NV12) {
                    for (int y = 0; y < (f->height + 1) / 2; ++y)
                        std::fwrite(f->data[1] + static_cast<size_t>(y) * f->linesize[1], 1, f->width, fo);
                } else {
                    const int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2, pl = cw * ch;
                    std::vector<uint8_t> u(pl), v(pl);
                    for (int y = 0; y < ch; ++y) {
                        std::memcpy(u.data() + static_cast<size_t>(y) * cw, f->data[1] + static_cast<size_t>(y) * f->linesize[1], cw);
                        std::memcpy(v.data() + static_cast<size_t>(y) * cw, f->data[2] + static_cast<size_t>(y) * f->linesize[2], cw);
                    }
                    std::fwrite(u.data(), 1, pl, fo);
                    std::fwrite(v.data(), 1, pl, fo);
                }
                std::fclose(fo);
                NXLOG_I(kTag, "first picture dumped to %s (%dx%d, format %d)", dump_path.c_str(), f->width, f->height, static_cast<int>(f->format));
            }
        }
        if (f->format == AV_PIX_FMT_YUV420P && f->data[0] && f->data[1] && f->data[2]) {
            FfDecPicture p;
            p.w = f->width;
            p.h = f->height;
            p.pts = f->pts != AV_NOPTS_VALUE ? f->pts : pts;
            p.nv12.assign(static_cast<size_t>(p.w) * p.h * 3 / 2, 0);
            for (int y = 0; y < p.h; ++y) std::memcpy(p.nv12.data() + static_cast<size_t>(y) * p.w, f->data[0] + static_cast<size_t>(y) * f->linesize[0], p.w);
            uint8_t* uv = p.nv12.data() + static_cast<size_t>(p.w) * p.h;
            for (int y = 0; y < p.h / 2; ++y) {
                const uint8_t* u = f->data[1] + static_cast<size_t>(y) * f->linesize[1];
                const uint8_t* v = f->data[2] + static_cast<size_t>(y) * f->linesize[2];
                uint8_t* o = uv + static_cast<size_t>(y) * p.w;
                for (int x = 0; x < p.w / 2; ++x) { o[2 * x] = u[x]; o[2 * x + 1] = v[x]; }
            }
            out->push_back(std::move(p));
            ++s_pics;
        } else if (f->format == AV_PIX_FMT_NV12 && f->data[0] && f->data[1]) {
            FfDecPicture p;
            p.w = f->width;
            p.h = f->height;
            p.pts = f->pts != AV_NOPTS_VALUE ? f->pts : pts;
            p.nv12.resize(static_cast<size_t>(p.w) * p.h * 3 / 2);
            for (int y = 0; y < p.h; ++y) std::memcpy(p.nv12.data() + static_cast<size_t>(y) * p.w, f->data[0] + static_cast<size_t>(y) * f->linesize[0], p.w);
            uint8_t* uv = p.nv12.data() + static_cast<size_t>(p.w) * p.h;
            for (int y = 0; y < p.h / 2; ++y) std::memcpy(uv + static_cast<size_t>(y) * p.w, f->data[1] + static_cast<size_t>(y) * f->linesize[1], p.w);
            out->push_back(std::move(p));
            ++s_pics;
        }
        if (out->empty() && f->format != AV_PIX_FMT_NV12 && f->format != AV_PIX_FMT_YUV420P)
            NXLOG_W(kTag, "unexpected frame format %d (%dx%d)", static_cast<int>(f->format), f->width, f->height);
        av_frame_unref(d->frame);
        if (f == d->sw) av_frame_unref(d->sw);
    }
    ++s_calls;
    if (s_calls % 30 == 0) NXLOG_I(kTag, "packets %llu, pictures %llu", (unsigned long long)s_calls, (unsigned long long)s_pics);
    return true;
}

void ffdec_destroy(FfDec* d) {
    if (!d) return;
    if (d->pkt) av_packet_free(&d->pkt);
    if (d->sw) av_frame_free(&d->sw);
    if (d->frame) av_frame_free(&d->frame);
    if (d->ctx) avcodec_free_context(&d->ctx);
    if (d->hw) av_buffer_unref(&d->hw);
    delete d;
}
}  // namespace nxrt::media
#else
namespace nxrt::media {
bool ffdec_prewarm() { return false; }
const char* ffdec_hw_name() { return "none"; }
bool ffdec_available(std::string* why) { if (why) *why = "built without FFmpeg"; return false; }
FfDec* ffdec_create(int, std::string* why) { if (why) *why = "built without FFmpeg"; return nullptr; }
bool ffdec_decode(FfDec*, const uint8_t*, size_t, int64_t, std::vector<FfDecPicture>*) { return false; }
void ffdec_destroy(FfDec*) {}
}
#endif

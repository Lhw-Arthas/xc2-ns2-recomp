// SPDX-License-Identifier: GPL-3.0-or-later
// The NVN-on-Vulkan back end (S4d). install_backend() replaces the logging stubs of the NVN shell (nvn.h) with real
// implementations and starts the render thread that replays the game's command lists on a gfx::Device.
//   * Memory pools are the game's own memory: the back end imports it as Vulkan memory (VK_EXT_external_memory_host), so a GPU
//     address is a host address and buffers are zero-copy.
//   * Game threads only record. nvnQueueSubmitCommands hands the lists to the render thread and returns when they are recorded
//     into Vulkan (not when the GPU has run them); fences, nvnQueueFinish and the memory a game polls follow the GPU timeline.
//   * nvnQueuePresentTexture draws the window texture into the Renderer's frame (honouring the shell's FrameLimiter when one is
//     given) and presents it.
// Configuration (config.h, environment wins): [nvn] shader_cache (translated SPIR-V folder), bc7_cache (ASTC->BC7 cache folder),
// bc7_quality, log_unimplemented; see docs/CONFIG.md.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "nxrt/gfx.h"

namespace nxrt {
class FrameLimiter;
}

namespace nxrt::nvn {

struct BackendDesc {
    gfx::Device* device = nullptr;            // required; must outlive the back end
    gfx::Renderer* renderer = nullptr;        // optional: without it nvnQueuePresentTexture only waits for the GPU
    FrameLimiter* limiter = nullptr;          // optional: begin_frame() is called before each presented frame
    bool keep_aspect = false;                 // present with letterbox bars instead of stretching to the window
    bool register_entry_points = true;        // false for tests that call the Vulkan layer directly
};

bool install_backend(const BackendDesc& desc);   // idempotent per process: a second call with another device fails
void shutdown_backend();                         // waits for the GPU, destroys every Vulkan object of the back end
bool backend_installed();
// Writes the on-disk caches (the pipeline cache) now. For the paths that end the process without shutting the back end down (window closed).
void backend_flush_caches();
// Shows the game's last frame again (with the overlay on top) when the game has not presented for 50 ms and no present is queued. For while
// the game waits on the player in a library applet (the on-screen keyboard) and presents nothing. Any thread; does not wait.
void present_last_frame_again();

// Traces the next whole presented frame like [nvn] trace_frame (the scripted `@shot nvntrace_<name>` calls this).
void backend_trace_next_frame();
void backend_trace_frames(int count);   // trace the next `count` presented frames (draw list only is up to trace_targets)

// Updates keep-aspect letterboxing live on the presenting thread (also initialized from BackendDesc::keep_aspect).
void set_keep_aspect(bool on);

// Reports the game's live 3D scene render resolution and base/UI output resolution.
// When apply_present_crop is true and base_w/base_h are smaller than the NVN window texture,
// presentation automatically crops to (0, 0, base_w, base_h) so native 1080p/1440p fills the screen.
struct GameViewportInfo {
    uint32_t render_w = 0, render_h = 0;
    uint32_t base_w = 0, base_h = 0;
    bool apply_present_crop = false;
    bool dlss_bypassed = false;
};
void set_game_viewport_info(uint32_t render_w, uint32_t render_h, uint32_t base_w, uint32_t base_h,
                            bool apply_present_crop, bool dlss_bypassed = false);
GameViewportInfo game_viewport_info();

// Live 3D DLSS/FSR upscaler override for the F1 settings menu.
// upscaler: "fsr3" | "taau" | "bilinear" | "dlss" | "auto" (empty/null = keep current).
// fsr_version: "3.1" | "4.1" | "2.3" (empty/null = keep current).
// sharpness: 0.0..1.0 (< 0 = keep current).
void set_dlss_live(const char* upscaler, const char* fsr_version, float sharpness);
struct DlssLiveInfo {
    std::string upscaler;
    std::string fsr_version;
    std::string active_name;
    float sharpness = 0.0f;
    bool fsr_available = false;
};
DlssLiveInfo dlss_live_info();

// Live device VRAM & pool-copy/capture allocation info (nvn_gpu_pool.cpp) for F1 status UI and periodic logging.
struct VramLiveInfo {
    uint64_t vram_used_mb = 0;
    uint64_t vram_total_mb = 0;
    uint64_t pool_copy_mb = 0;
    uint64_t pool_cap_mb = 0;
    bool capture_on = false;
    bool budget_live = false;
};
VramLiveInfo vram_live_info();

// The size and NVNmemoryPoolFlags of the initialized memory pool behind a guest NVNmemoryPool object. False when `guest_pool` is not a live
// pool of the back end. For game modules that look after a game's own pool allocators: a texture the game places past the end of a pool the
// CPU cannot access (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) gets GPU memory of its own instead of failing (nvnTextureInitialize).
bool memory_pool_info(const void* guest_pool, uint64_t* size, int* flags);

// Shader preparation (S24): compiles the shader parts of every program in a list before the game runs, so the driver already has them when the
// game asks. The list is written by the setup tool from the player's own game files (<shader cache folder>/../programs.bin, "MPRPRG01").
// Needs the back end installed (any mode; `register_entry_points = false` is enough) and an open shader cache.
struct ShaderPrepareOptions {
    std::string programs_file;                                    // empty: programs.bin next to the shader cache folder
    bool only_if_needed = false;                                  // do nothing when the state file says this GPU and driver were prepared already
    bool force = false;                                           // ignore the state file
    int threads = 0;                                              // 0: all hardware threads but two
    std::function<bool(uint64_t done, uint64_t total)> progress;  // called often; returning false cancels
};
struct ShaderPrepareResult {
    uint64_t programs = 0;          // in the list
    uint64_t prepared = 0;          // programs whose parts were all made
    uint64_t missing_shaders = 0;   // skipped: a stage has no translated shader in the cache
    uint64_t failed = 0;            // a part could not be made
    uint64_t parts = 0;
    uint64_t fast_parts = 0;        // parts made in under 2 ms: the driver already had them
    double seconds = 0;
    bool cancelled = false;
    bool already_ready = false;     // only_if_needed and nothing to do
    std::string error;              // why nothing could be done
};
bool prepare_shaders(const ShaderPrepareOptions& o, ShaderPrepareResult* r);

struct BackendStats {
    uint64_t submits = 0, lists_replayed = 0, draws = 0, draws_skipped = 0, dispatches = 0, clears = 0, copies = 0, presents = 0;
    uint64_t passes = 0, pass_breaks = 0, pipelines_created = 0, pipeline_cache_hits = 0, descriptor_sets = 0;
    uint64_t textures_created = 0, texture_uploads = 0, texture_upload_bytes = 0, bc7_transcodes = 0, textures_failed = 0;
    uint64_t pools_imported = 0, unresolved_addresses = 0, missing_handles = 0, unsupported_ops = 0;
    uint64_t submit_replay_ns = 0, gpu_busy_ns = 0;
};
BackendStats backend_stats();
// Operations that were skipped because they are not implemented, with how often (for the log and docs/S4d.md).
std::string backend_unsupported_report();

}  // namespace nxrt::nvn

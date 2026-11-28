// SPDX-License-Identifier: GPL-3.0-or-later
// Display tuning that the NVN back end reads on the render thread (S10): texture filtering overrides. Play mode writes them
// from the settings page; the sampler cache includes them in its key, so a change takes effect on the next draw without a restart.
#pragma once
#include <atomic>
#include <cstdint>

namespace nxrt::gfx {

// Process-wide snapshot of the physical GPU chosen by Device::create(), kept header-light (no Vulkan headers)
// so media/FFmpeg (nxrt) can read it without depending on nxrt_gfx (issues #63/#87).
struct RenderGpuInfo {
    int vulkan_index = -1;       // index in vkEnumeratePhysicalDevices (-1 before Device::create)
    uint32_t vendor_id = 0;      // PCI vendor ID (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel)
    char name[256] = {};
};
void note_render_gpu_info(int index, uint32_t vendor_id, const char* name);
RenderGpuInfo render_gpu_info();
// Asynchronously prewarm the FFmpeg hardware video device (Vulkan Video / D3D11VA) once gfx::Device::create() has finished,
// avoiding both the #42 driver-init race at t=0.001s and the ~100 ms cold-start hitch on the first cutscene.
void prewarm_hwdec_async();

struct Tuning {
    std::atomic<int> anisotropy{0};        // 0 = what the game's samplers ask for; 2, 4, 8, 16 = at least this much on every filtering sampler
    std::atomic<float> lod_bias{0.0f};     // added to the LOD bias of every sampler (negative = sharper)
    std::atomic<float> upscale_lod_bias{0.0f};   // automatic negative mip bias from temporal upscaler (material samplers only, issue #79)
    std::atomic<float> render_scale{0.0f};  // the NVN back end's resolution scale from the game's play settings ([display] render_scale); 0 = not set
    std::atomic<bool> game_handles_render_quality{false};   // true when the game module implements [upscale] render_quality in-engine; NVN stays 1.0x
    // A game's live scale on some floats of one fragment shader's uniform block (Pokopia: the fog strength on its height-fog pass). When the
    // scale is not 1, that block is bound from a copy in the upload ring with the chosen floats multiplied; the game's memory is left alone.
    std::atomic<uint64_t> ufs_hash{0};     // the fragment shader's hash (its 16 hex digits as a number); 0 = off
    std::atomic<int> ufs_block{-1};        // the uniform block (NVN index, the N of fp_cN)
    std::atomic<uint32_t> ufs_mask{0};     // bit i: the float at byte offset 4 * i of the block
    std::atomic<float> ufs_scale{1.0f};
};

Tuning& tuning();                          // process-wide

}  // namespace nxrt::gfx

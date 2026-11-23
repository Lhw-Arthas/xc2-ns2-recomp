// SPDX-License-Identifier: GPL-3.0-or-later
// S38: how much video memory the device-local pool copies ([nvn] pool_mirror) may take. Pure, so the tests can call it.
#pragma once

#include <algorithm>
#include <cstdint>

namespace nxrt::nvn {

struct MirrorCap {
    uint64_t bytes = 0;     // most bytes the copies may take in all (0: no copies)
    const char* why = "";   // for the log line
};

constexpr uint64_t kMirrorCapMax = 8192ull << 20;      // the default cap on a large card
constexpr uint64_t kMirrorMinVram = 4ull << 30;        // below this much video memory there are no copies by default
// [nvn] mirror_capture = auto: the capture needs a whole-pool copy of every pool it serves (XC2 NS2: mesh pools of 500-640 MiB);
// pools past the copy budget stay in host memory (and OOM allocations fall back cleanly). Lowered from 3584 to 1536 MiB so 8 GB
// discrete cards (~1989 MiB budget) enable capture by default while 4 GB cards (1024 MiB) and integrated GPUs (0 MiB) stay off.
constexpr uint64_t kCaptureMinBudget = 1536ull << 20;

// vram_bytes: the largest device-local heap (0 when unknown). explicit_mb >= 0: [nvn] pool_mirror_mb was set in the ini or the
// environment, and that value wins; -1: not set, the cap follows the card: min(kMirrorCapMax, a quarter of the heap), none below 4 GiB.
// shared_memory: an integrated GPU or APU, whose "video memory" is system RAM; the copies would only cost RAM there, so none by default.
inline MirrorCap mirror_cap(uint64_t vram_bytes, int64_t explicit_mb, bool shared_memory = false) {
    if (explicit_mb >= 0) return {static_cast<uint64_t>(explicit_mb) << 20, "[nvn] pool_mirror_mb"};
    if (shared_memory) return {0, "integrated GPU (shares system memory)"};
    if (vram_bytes < kMirrorMinVram) return {0, "under 4 GiB of video memory"};
    return {std::min(kMirrorCapMax, vram_bytes / 4), vram_bytes / 4 < kMirrorCapMax ? "a quarter of the video memory" : "8192 MiB"};
}

// The largest device-local heap the device reported (0 before the device exists). The UI uses it to say whether a 4K
// preset fits next to the capture's copies; the copies themselves are sized from the same number.
uint64_t mirror_device_vram();

}  // namespace nxrt::nvn

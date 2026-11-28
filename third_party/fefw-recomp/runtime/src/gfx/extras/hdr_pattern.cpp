// SPDX-License-Identifier: GPL-3.0-or-later
// HDR test patterns (hdr_pattern.h).
#include "extras/hdr_pattern.h"

#include <atomic>
#include <chrono>

namespace nxrt::extras {
namespace {

constexpr int64_t kLastsNs = 250'000'000;

std::atomic<int> g_kind{0};
std::atomic<float> g_peak{0.0f};
std::atomic<int64_t> g_until{0};

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

void set_hdr_pattern(HdrPattern kind, float peak_nits) {
    g_kind.store(static_cast<int>(kind), std::memory_order_relaxed);
    g_peak.store(peak_nits > 0 ? peak_nits : 0.0f, std::memory_order_relaxed);
    g_until.store(now_ns() + kLastsNs, std::memory_order_release);
}

HdrPatternLive hdr_pattern() {
    HdrPatternLive r;
    if (now_ns() > g_until.load(std::memory_order_acquire)) return r;
    const int k = g_kind.load(std::memory_order_relaxed);
    if (k < static_cast<int>(HdrPattern::Peak) || k > static_cast<int>(HdrPattern::Staircase)) return r;
    r.kind = static_cast<HdrPattern>(k);
    r.peak_nits = g_peak.load(std::memory_order_relaxed);
    return r;
}

}  // namespace nxrt::extras

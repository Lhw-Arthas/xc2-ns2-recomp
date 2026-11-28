// SPDX-License-Identifier: GPL-3.0-or-later
// Live HDR on / off (hdr_live.h).
#include "extras/hdr_live.h"

#include <atomic>

namespace nxrt::extras {
namespace {
std::atomic<bool> g_sdr{false};
}

bool hdr_shown_as_sdr() { return g_sdr.load(std::memory_order_relaxed); }

void set_hdr_shown_as_sdr(bool on) { g_sdr.store(on, std::memory_order_relaxed); }

}  // namespace nxrt::extras

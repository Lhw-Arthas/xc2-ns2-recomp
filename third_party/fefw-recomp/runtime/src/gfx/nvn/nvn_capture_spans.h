// SPDX-License-Identifier: GPL-3.0-or-later
// Splitting a captured page range around the byte ranges the GPU owns (PR #72). Pure, so the tests can call it: a
// mistake here drops or duplicates bytes silently, which is exactly the failure the sub-page preservation exists to
// prevent - neighbouring CPU UBO writes on the same 4 KiB page must not overwrite unchanged GPU-written vertices.
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace nxrt::nvn {

// Writes the parts of [lo, hi) that `skip` does not cover into `out`, sorted and merged. `skip` is sorted in place.
// Returns how many bytes were left out, so the caller can report how much of the capture the GPU keeps for itself.
inline uint64_t capture_keep_spans(uint64_t lo, uint64_t hi, std::vector<std::pair<uint64_t, uint64_t>>& skip,
                                   std::vector<std::pair<uint64_t, uint64_t>>& out) {
    out.clear();
    if (lo >= hi) return 0;
    if (skip.empty()) {
        out.emplace_back(lo, hi);
        return 0;
    }
    std::sort(skip.begin(), skip.end());
    uint64_t a = lo, skipped = 0;
    for (const auto& s : skip) {
        if (s.second <= a || s.first >= hi) continue;   // behind the cursor, or past the range
        if (s.first > a) out.emplace_back(a, s.first);
        skipped += std::min(s.second, hi) - std::max(s.first, a);
        a = std::max(a, s.second);
        if (a >= hi) break;
    }
    if (a < hi) out.emplace_back(a, hi);
    return skipped;
}

}  // namespace nxrt::nvn
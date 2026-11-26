// SPDX-License-Identifier: GPL-3.0-or-later
// The span arithmetic behind the sub-page preservation of PR #72: a captured page range is split around the byte
// ranges the GPU owns, and what is left is what the CPU side may copy. Getting this wrong drops GPU-written vertices
// or copies stale host bytes over them, and nothing else in the suite would notice.
#include <cstdint>
#include <utility>
#include <vector>

#include "nvn_capture_spans.h"
#include "test_framework.h"

NXTEST_GROUP(gfx_nvn_capture_spans)

using nxrt::nvn::capture_keep_spans;

namespace {
using R = std::pair<uint64_t, uint64_t>;

std::vector<R> keep(uint64_t lo, uint64_t hi, std::vector<R> skip, uint64_t* skipped = nullptr) {
    std::vector<R> out;
    const uint64_t n = capture_keep_spans(lo, hi, skip, out);
    if (skipped) *skipped = n;
    return out;
}
}  // namespace

NXTEST(no_skip_keeps_the_whole_range) {
    const std::vector<R> out = keep(4096, 8192, {});
    CHECK_EQ(out.size(), 1u);
    CHECK_EQ(out[0].first, 4096ull);
    CHECK_EQ(out[0].second, 8192ull);
}

NXTEST(an_interior_skip_splits_the_range_in_two) {
    uint64_t skipped = 0;
    const std::vector<R> out = keep(0, 4096, {{1024, 2048}}, &skipped);
    CHECK_EQ(out.size(), 2u);
    CHECK_EQ(out[0].first, 0ull);
    CHECK_EQ(out[0].second, 1024ull);
    CHECK_EQ(out[1].first, 2048ull);
    CHECK_EQ(out[1].second, 4096ull);
    CHECK_EQ(skipped, 1024ull);
}

NXTEST(leading_and_trailing_skips_are_clipped) {
    // The GPU owns the head and the tail of the page: what is left is the middle.
    const std::vector<R> out = keep(0, 4096, {{0, 512}, {3584, 8192}});
    CHECK_EQ(out.size(), 1u);
    CHECK_EQ(out[0].first, 512ull);
    CHECK_EQ(out[0].second, 3584ull);
}

NXTEST(unsorted_and_overlapping_skips_are_merged) {
    uint64_t skipped = 0;
    const std::vector<R> out = keep(0, 1000, {{600, 700}, {100, 200}, {150, 250}, {600, 650}}, &skipped);
    CHECK_EQ(out.size(), 3u);
    CHECK_EQ(out[0].second, 100ull);
    CHECK_EQ(out[1].first, 250ull);
    CHECK_EQ(out[1].second, 600ull);
    CHECK_EQ(out[2].first, 700ull);
    CHECK_EQ(out[2].second, 1000ull);
    CHECK_EQ(skipped, 250ull);      // 100..250 counts once, not twice
}

NXTEST(a_skip_covering_everything_leaves_nothing) {
    uint64_t skipped = 0;
    const std::vector<R> out = keep(4096, 8192, {{0, 16384}}, &skipped);
    CHECK_EQ(out.size(), 0u);
    CHECK_EQ(skipped, 4096ull);
}

NXTEST(empty_and_inverted_ranges_produce_nothing) {
    CHECK_EQ(keep(4096, 4096, {{0, 100}}).size(), 0u);
    CHECK_EQ(keep(8192, 4096, {}).size(), 0u);
}
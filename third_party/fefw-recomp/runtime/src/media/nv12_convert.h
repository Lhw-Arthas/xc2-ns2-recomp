// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

namespace nxrt::media {

// Converts 8-bit planar I420 into tightly packed NV12 at dst_w x dst_h (even),
// using SSE2 to interleave U and V planes without zero-filling overwritten pixels.
inline void i420_to_nv12(uint8_t* dst, uint32_t dst_w, uint32_t dst_h,
                         uint32_t src_w, uint32_t src_h,
                         const uint8_t* py, ptrdiff_t sy,
                         const uint8_t* pu, ptrdiff_t su,
                         const uint8_t* pv, ptrdiff_t sv) {
    if (src_h < dst_h) {
        std::memset(dst + static_cast<size_t>(src_h) * dst_w, 0, static_cast<size_t>(dst_h - src_h) * dst_w);
    }
    if (src_w == dst_w && src_h == dst_h && sy == static_cast<ptrdiff_t>(dst_w)) {
        std::memcpy(dst, py, static_cast<size_t>(dst_w) * dst_h);
    } else {
        for (uint32_t r = 0; r < src_h; ++r) {
            uint8_t* row = dst + static_cast<size_t>(r) * dst_w;
            std::memcpy(row, py + static_cast<ptrdiff_t>(r) * sy, src_w);
            if (src_w < dst_w) std::memset(row + src_w, 0, dst_w - src_w);
        }
    }
    uint8_t* uv = dst + static_cast<size_t>(dst_w) * dst_h;
    const uint32_t cw = (src_w + 1) / 2;
    const uint32_t ch = (src_h + 1) / 2;
    const uint32_t uv_stride = ((dst_w + 1) / 2) * 2;
    for (uint32_t r = 0; r < ch; ++r) {
        const uint8_t* u = pu + static_cast<ptrdiff_t>(r) * su;
        const uint8_t* v = pv + static_cast<ptrdiff_t>(r) * sv;
        uint8_t* o = uv + static_cast<size_t>(r) * uv_stride;
        uint32_t x = 0;
        for (; x + 16 <= cw; x += 16) {
            const __m128i vu = _mm_loadu_si128(reinterpret_cast<const __m128i*>(u + x));
            const __m128i vv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(v + x));
            const __m128i lo = _mm_unpacklo_epi8(vu, vv);
            const __m128i hi = _mm_unpackhi_epi8(vu, vv);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(o + 2 * x), lo);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(o + 2 * x + 16), hi);
        }
        for (; x < cw; ++x) {
            o[2 * x] = u[x];
            o[2 * x + 1] = v[x];
        }
        if (cw * 2 < uv_stride) std::memset(o + cw * 2, 0, uv_stride - cw * 2);
    }
}

}  // namespace nxrt::media

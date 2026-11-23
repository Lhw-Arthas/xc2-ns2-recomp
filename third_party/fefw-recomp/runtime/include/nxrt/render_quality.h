// SPDX-License-Identifier: GPL-3.0-or-later
// [upscale] render_quality: stored as intent; the scene and output scales are derived from it and the output height at start-up.
#pragma once

#include <cstring>

namespace nxrt {

struct RenderQualityMode {
    const char* key;   // the [upscale] render_quality value
    double share;      // the upscaler's input as a share of the output height; 0: the console's own dynamic resolution
};

inline constexpr RenderQualityMode kRenderQualityModes[] = {
    {"native", 1.0}, {"quality", 2.0 / 3.0}, {"balanced", 0.58}, {"performance", 0.5}, {"ultra_performance", 1.0 / 3.0}, {"console", 0.0},
};
inline constexpr int kRenderQualityModeCount = static_cast<int>(sizeof kRenderQualityModes / sizeof kRenderQualityModes[0]);

// The mode named `key`, or null (not set / not known).
inline const RenderQualityMode* render_quality_mode(const char* key) {
    if (!key || !*key) return nullptr;
    for (const RenderQualityMode& m : kRenderQualityModes)
        if (std::strcmp(m.key, key) == 0) return &m;
    return nullptr;
}

// The scales a mode implies at output height out_h. False for the console mode (no scaling, no lock).
inline bool render_quality_scales(const RenderQualityMode& m, int out_h, double* scene, double* out) {
    if (m.share <= 0.0 || out_h <= 0) return false;
    *scene = out_h * m.share / 720.0;
    *out = out_h / 1080.0;
    return true;
}

}  // namespace nxrt

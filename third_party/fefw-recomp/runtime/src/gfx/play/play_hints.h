// SPDX-License-Identifier: GPL-3.0-or-later
// A few words on the current value of the picture and HDR sliders of the F1 menu, shown next to the value. Pure functions of the value.
#pragma once

namespace nxrt::play {

struct SliderHint {
    const char* tag_en = "";
    const char* tag_zh = "";
};

SliderHint hint_contrast(double v);
SliderHint hint_saturation(double v);
SliderHint hint_black(double v);
SliderHint hint_gamma(double v);
SliderHint hint_peak(double nits);          // 0 = Auto
SliderHint hint_paper_white(double nits);   // 0 = Auto
SliderHint hint_boost(double v);
SliderHint hint_ui_white(double nits, double paper_nits);   // 0 = Auto (paper white); paper_nits 0 = Auto

}  // namespace nxrt::play

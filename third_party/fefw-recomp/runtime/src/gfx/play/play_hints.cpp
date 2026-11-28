// SPDX-License-Identifier: GPL-3.0-or-later
// Slider value tags (play_hints.h).
#include "play_hints.h"

namespace nxrt::play {
namespace {

SliderHint tag(const char* en, const char* zh) {
    SliderHint h;
    h.tag_en = en;
    h.tag_zh = zh;
    return h;
}

}  // namespace

SliderHint hint_contrast(double v) {
    if (v < 0.98) return tag("flatter", "更平");
    if (v <= 1.02) return tag("game's own", "原版");
    if (v <= 1.12) return tag("punchier", "更有冲击力");
    return tag("strong", "强烈");
}

SliderHint hint_saturation(double v) {
    if (v < 0.05) return tag("black and white", "黑白");
    if (v < 0.95) return tag("muted", "偏淡");
    if (v <= 1.05) return tag("game's own", "原版");
    if (v <= 1.25) return tag("vivid", "鲜艳");
    return tag("very strong", "非常浓烈");
}

SliderHint hint_black(double v) {
    if (v < -0.06) return tag("very deep", "非常深");
    if (v < -0.005) return tag("deeper blacks", "更深的黑");
    if (v <= 0.005) return tag("game's own", "原版");
    return tag("lifted shadows", "阴影提亮");
}

SliderHint hint_gamma(double v) {
    if (v < 0.97) return tag("darker", "偏暗");
    if (v <= 1.03) return tag("game's own", "原版");
    return tag("brighter", "偏亮");
}

SliderHint hint_peak(double nits) {
    if (nits <= 0) return tag("from Windows", "取自 Windows");
    if (nits < 600) return tag("modest", "较低");
    if (nits <= 1200) return tag("typical HDR", "常见 HDR");
    return tag("very bright", "非常亮");
}

SliderHint hint_paper_white(double nits) {
    if (nits <= 0) return tag("from Windows", "取自 Windows");
    if (nits < 120) return tag("dark room", "暗室");
    if (nits <= 260) return tag("typical", "常用");
    return tag("bright room", "明亮房间");
}

SliderHint hint_boost(double v) {
    if (v < 0.05) return tag("SDR look", "SDR 观感");
    if (v < 0.5) return tag("gentle", "柔和");
    if (v <= 1.05) return tag("strong", "强烈");
    return tag("very strong", "非常强烈");
}

SliderHint hint_ui_white(double nits, double paper_nits) {
    if (nits <= 0) return tag("same as paper white", "与纸白相同");
    if (paper_nits > 0 && nits < paper_nits * 0.9) return tag("dimmer than the game", "比画面暗");
    if (paper_nits > 0 && nits > paper_nits * 1.1) return tag("brighter than the game", "比画面亮");
    return tag("about paper white", "约等于纸白");
}

}  // namespace nxrt::play

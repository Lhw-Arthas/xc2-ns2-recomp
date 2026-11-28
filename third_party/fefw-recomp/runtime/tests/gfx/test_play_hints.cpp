// SPDX-License-Identifier: GPL-3.0-or-later
// Slider value tags of the F1 menu (src/gfx/play/play_hints.cpp): the tag follows the value and has text in both languages.
#include "test_framework.h"

#include <cstring>
#include <string>

#include "play/play_hints.h"

NXTEST_GROUP(play_hints)

namespace {

namespace play = nxrt::play;

bool filled(const play::SliderHint& h) {
    return h.tag_en && *h.tag_en && h.tag_zh && *h.tag_zh;
}
bool tag_is(const play::SliderHint& h, const char* en) { return std::strcmp(h.tag_en, en) == 0; }

}  // namespace

NXTEST(every_hint_has_both_languages_across_the_range) {
    for (double v = 0.0; v <= 2.0; v += 0.01) {
        CHECK(filled(play::hint_contrast(v)));
        CHECK(filled(play::hint_saturation(v)));
        CHECK(filled(play::hint_gamma(v)));
        CHECK(filled(play::hint_boost(v)));
    }
    for (double v = -0.25; v <= 0.25; v += 0.01) {
        CHECK(filled(play::hint_black(v)));
    }
    for (double n = 0; n <= 2000; n += 50) {
        CHECK(filled(play::hint_peak(n)));
        CHECK(filled(play::hint_paper_white(n)));
        CHECK(filled(play::hint_ui_white(n, 200)));
        CHECK(filled(play::hint_ui_white(n, 0)));
    }
}

NXTEST(neutral_values_say_game_own) {
    CHECK(tag_is(play::hint_contrast(1.0), "game's own"));
    CHECK(tag_is(play::hint_saturation(1.0), "game's own"));
    CHECK(tag_is(play::hint_black(0.0), "game's own"));
    CHECK(tag_is(play::hint_gamma(1.0), "game's own"));
}

NXTEST(verdicts_follow_the_value) {
    CHECK(tag_is(play::hint_contrast(0.9), "flatter"));
    CHECK(tag_is(play::hint_contrast(1.06), "punchier"));
    CHECK(tag_is(play::hint_contrast(1.3), "strong"));
    CHECK(tag_is(play::hint_saturation(0.0), "black and white"));
    CHECK(tag_is(play::hint_saturation(1.15), "vivid"));
    CHECK(tag_is(play::hint_black(-0.02), "deeper blacks"));
    CHECK(tag_is(play::hint_black(-0.1), "very deep"));
    CHECK(tag_is(play::hint_black(0.05), "lifted shadows"));
    CHECK(tag_is(play::hint_gamma(0.8), "darker"));
    CHECK(tag_is(play::hint_gamma(1.2), "brighter"));
    CHECK(tag_is(play::hint_peak(0), "from Windows"));
    CHECK(tag_is(play::hint_peak(1000), "typical HDR"));
    CHECK(tag_is(play::hint_paper_white(0), "from Windows"));
    CHECK(tag_is(play::hint_paper_white(200), "typical"));
    CHECK(tag_is(play::hint_paper_white(100), "dark room"));
    CHECK(tag_is(play::hint_boost(0.0), "SDR look"));
    CHECK(tag_is(play::hint_boost(0.4), "gentle"));
    CHECK(tag_is(play::hint_ui_white(0, 200), "same as paper white"));
    CHECK(tag_is(play::hint_ui_white(120, 200), "dimmer than the game"));
    CHECK(tag_is(play::hint_ui_white(300, 200), "brighter than the game"));
}


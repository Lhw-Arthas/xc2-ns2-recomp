// SPDX-License-Identifier: GPL-3.0-or-later
// HDR test patterns for calibrating on the display itself (F1 menu, Graphics page). The menu asks for a pattern every frame it is shown;
// the output pass draws it in place of the game (extras/output_pass.h), and it goes away shortly after the menu stops asking.
//   Peak: small squares at 10000 nits, each with a smaller square at the peak being tried. Once the inner squares vanish, the display has
//         reached its brightest, so that peak is the display's. About 7% of the screen is lit: an OLED dims large bright areas.
//   Black: dark grey squares (8-bit codes 1 to 16 of the game's picture) on pure black, through the picture controls, for the black level.
//   Staircase: a strip of 8 bars along the top, 300 to 1000 nits in steps of 100: where the bars stop getting brighter is the limit.
#pragma once
#include <cstdint>

namespace nxrt::extras {

enum class HdrPattern : uint8_t { None = 0, Peak, Black, Staircase };

struct HdrPatternLive {
    HdrPattern kind = HdrPattern::None;
    float peak_nits = 0;   // Peak: the inner squares' brightness (0 = the current peak)
};

void set_hdr_pattern(HdrPattern kind, float peak_nits);   // any thread; lasts about a quarter of a second
HdrPatternLive hdr_pattern();                             // what to draw now (None once the asks stopped)

}  // namespace nxrt::extras

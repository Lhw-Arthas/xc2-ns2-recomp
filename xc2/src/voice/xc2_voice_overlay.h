// SPDX-License-Identifier: GPL-3.0-or-later
// Top-level facade for XC2 Native-Style Voice Visualization System.
#pragma once
#include <cstdint>

#include "nxrt/overlay.h"

namespace xc2::voice {

// Initialize with guest main module base (0x7100000000) and read [xc2] voice_* config keys.
void init(uint64_t main_base);
void shutdown();

// Toggle voice subtitles live (F5), Voice Log history panel (Ctrl+F5), and density preset (Shift+F5).
void toggle();
void toggle_log();
void cycle_density();
bool enabled();

// Phase 1 mock verification helpers (F6 / F7 / F8 when voice_debug or Ctrl+key is used).
// scenario: 0 = P0 Post-Battle Banter, 1 = P1 Combat Break + Driver/Blade Merge, 2 = P1 Aggro/Heal cycle.
void trigger_mock(int scenario);
void toggle_mock_combat_mode();

// Per-frame HUD draw callback invoked alongside xc2::nav::draw().
void draw(const nxrt::gfx::HudCanvas& c);

}  // namespace xc2::voice

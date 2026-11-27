// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only 3D quest/objective navigation overlay for Xenoblade Chronicles 2 (NS2 3.0.1).
// Reads only live camera matrices, player transform, active GfMenuLocation locators, and GmkLandmarkManager
// from guest memory via nxrt::mem::is_mapped() and draws 3D-projected beacons + elevation deltas + nearest
// skip-travel hints on nxrt::gfx::HudCanvas (Dear ImGui foreground draw list, SDR + HDR safe, zero input capture).
#pragma once
#include <cstdint>

#include "nxrt/overlay.h"

namespace xc2::nav {

// Initialize with the guest main module base (typically 0x7100000000) and read [xc2] nav_overlay / nav_debug config.
void init(uint64_t main_base);

// Toggle visibility live (e.g. F3 hotkey); shows a brief on-screen toast and logs the new state.
void toggle();
bool enabled();

// Per-frame HUD draw callback registered with nxrt::play::set_hud_callback().
void draw(const nxrt::gfx::HudCanvas& c);

}  // namespace xc2::nav

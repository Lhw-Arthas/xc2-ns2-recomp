// SPDX-License-Identifier: GPL-3.0-or-later
// Live HDR on / off ([hdr] mode, F1 menu): when the game starts with HDR (auto / on, and the display shows HDR) the HDR scene is set up, and the
// setting then only chooses the output; started with off, nothing of it is set up. While it is SDR, the patched tone-map shaders run as the game's own (nvn_pipeline.h GraphicsKey::sdr_tone_map).
#pragma once

namespace nxrt::extras {

bool hdr_shown_as_sdr();              // the HDR scene is set up but the output is SDR
void set_hdr_shown_as_sdr(bool on);   // the renderer, when it makes the swapchain

}  // namespace nxrt::extras

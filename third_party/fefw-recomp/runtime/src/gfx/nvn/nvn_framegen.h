// SPDX-License-Identifier: GPL-3.0-or-later
// The HUD-less colour of DLSS Frame Generation ([dlss] frame_gen, notes/framegen.md): the scene without the UI (nvn_hudless.h), copied
// after the frame's DLSS evaluation and, at the present, drawn by the present pass into an image of the backbuffer's size and format.
#pragma once
#include <functional>

#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu {

bool fg_hudless_on();      // [dlss] frame_gen and [dlss] frame_gen_hudless (defaults to game::Info::frame_gen_hudless_default), read once
void fg_note_eval();       // replay: this frame's DLSS evaluation fed frame generation
// present, after the picture is recorded: draw(src, dst) records the picture from src (a view in SHADER_READ_ONLY_OPTIMAL of an image the
// size of the presented texture) into dst (COLOR_ATTACHMENT_OPTIMAL, the backbuffer's size and format) as the present does
void fg_hudless_present(Texture* presented, Frame& f, VkExtent2D bb, VkFormat bb_format, bool again,
                        const std::function<void(VkImageView, VkImageView)>& draw);

}  // namespace nxrt::nvn::be::gpu

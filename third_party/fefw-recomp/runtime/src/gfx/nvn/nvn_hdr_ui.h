// SPDX-License-Identifier: GPL-3.0-or-later
// The game's UI on HDR output: kept out of the highlight boost and given its own brightness ([hdr] ui_split, ui_white_nits), using the
// copy of the scene before the UI (nvn_hudless.h).
#pragma once
#include "nvn_gpu_internal.h"
#include "nxrt/extras.h"

namespace nxrt::nvn::be::gpu {

struct HdrUiFrame {
    VkImageView scene = VK_NULL_HANDLE;   // the scene before the UI (SHADER_READ_ONLY_OPTIMAL), null when this frame has none
    float ui_scale = 1.0f;                // the UI's brightness over paper white
};

// present, before the picture is recorded. decoded: the window texture is read through its sRGB view (display-referred bytes).
HdrUiFrame hdr_ui_frame(Texture* presented, Frame& f, bool again, const extras::HdrState& hs, bool decoded);

}  // namespace nxrt::nvn::be::gpu

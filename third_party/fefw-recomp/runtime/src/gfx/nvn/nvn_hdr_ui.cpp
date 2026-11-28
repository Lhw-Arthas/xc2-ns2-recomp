// SPDX-License-Identifier: GPL-3.0-or-later
// The game's UI on HDR output (nvn_hdr_ui.h).
#include "nvn_hdr_ui.h"

#include "extras/hdr_math.h"
#include "nvn_hudless.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "hdr";

void note_frame(bool had_copy) {
    static uint64_t frames = 0, with_copy = 0;
    with_copy += had_copy ? 1 : 0;
    if (frames++ % 3600 == 0)
        NXLOG_I(kTag, "UI split: %llu of %llu frames had a copy before the UI (the others boost the whole picture)",
                static_cast<unsigned long long>(with_copy), static_cast<unsigned long long>(frames));
}

}  // namespace

HdrUiFrame hdr_ui_frame(Texture* presented, Frame& f, bool again, const extras::HdrState& hs, bool decoded) {
    static const bool on = config_get_bool("hdr", "ui_split", true);
    const bool hdr_output = decoded && hs.scene_hdr && hs.output != extras::OutputKind::Sdr8;
    const extras::HdrUiSplit split = extras::hdr_ui_split(hdr_output, hs.peak_ratio, hs.paper_white_nits, extras::settings().hdr.ui_white_nits);
    const bool wanted = on && split.wanted;
    hudless_want_every_frame(wanted);   // also with frame generation on: its copy is only made on frames with a DLSS evaluation
    HdrUiFrame r;
    r.ui_scale = split.ui_scale;
    if (!wanted) return r;
    r.scene = hudless_view(presented, f, again);
    if (!again) note_frame(r.scene != VK_NULL_HANDLE);
    return r;
}

}  // namespace nxrt::nvn::be::gpu

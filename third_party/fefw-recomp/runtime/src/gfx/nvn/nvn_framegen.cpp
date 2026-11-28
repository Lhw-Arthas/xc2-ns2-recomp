// SPDX-License-Identifier: GPL-3.0-or-later
// HUD-less colour for DLSS Frame Generation (nvn_framegen.h).
#include "nvn_framegen.h"

#include "extras/framegen.h"
#include "nvn_hudless.h"
#include "nxrt/config.h"
#include "nxrt/extras.h"
#include "nxrt/game.h"
#include "nxrt/log.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "framegen";

struct State {
    bool after_eval = false;
    TrackedImage bb[2];
    uint32_t cur = 0;
    uint64_t frames = 0, captured = 0, whole = 0;
};
State& st() {
    static State s;
    return s;
}

}  // namespace

bool fg_hudless_on() {
    static const bool v = extras::framegen_requested() &&
                          config_get_bool("dlss", "frame_gen_hudless", nxrt::game::info().frame_gen_hudless_default);
    return v;
}

void fg_note_eval() {
    if (!fg_hudless_on()) return;
    st().after_eval = true;
    hudless_want_this_frame();
}

void fg_hudless_present(Texture* presented, Frame& f, VkExtent2D bb, VkFormat bb_format, bool again,
                        const std::function<void(VkImageView, VkImageView)>& draw) {
    if (!fg_hudless_on()) return;
    State& s = st();
    const bool eval = s.after_eval;
    s.after_eval = false;
    if (again || !eval || hudless_failed_this_frame() || !presented || !extras::framegen_running()) return;
    const VkImageView copy = hudless_view(presented, f, false);
    if (!copy) {   // no blended draw into the window texture after the scene: nothing known about the UI
        ++s.whole;
        return;
    }
    ++s.captured;
    TrackedImage& b = s.bb[s.cur];
    s.cur ^= 1u;
    if (!b.ensure(bb, bb_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                  "framegen hudless"))
        return;
    b.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.to(f.cb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    draw(copy, b.img.view);
    b.to(f.cb, VK_IMAGE_LAYOUT_GENERAL);
    extras::framegen_hudless(f.cb, b.img.image, b.img.view, bb_format, bb);
    if (s.frames++ == 0 || s.frames % 1800 == 0)
        NXLOG_I(kTag, "HUD-less colour: %llu frames, %llu captured before the UI, %llu with no capture", static_cast<unsigned long long>(s.frames),
                static_cast<unsigned long long>(s.captured), static_cast<unsigned long long>(s.whole));
}

}  // namespace nxrt::nvn::be::gpu

// SPDX-License-Identifier: GPL-3.0-or-later
// Dual-track native XC2-style voice renderer on nxrt::gfx::HudCanvas (SDR + HDR safe,
// 16:9 HUD safe-zone anchored for 16:9 / 21:9 / 32:9).
#pragma once
#include "nxrt/overlay.h"
#include "xc2_voice_director.h"

namespace xc2::voice {

class VoiceRenderer {
public:
    void render(const nxrt::gfx::HudCanvas& c, const DirectorRenderState& st) const;
    void render_voice_log(const nxrt::gfx::HudCanvas& c, const std::deque<VoiceLogEntry>& history,
                          VoiceUILang ui_lang = VoiceUILang::Zh) const;

private:
    struct SafeRect {
        float x = 0.f;
        float y = 0.f;
        float w = 1280.f;
        float h = 720.f;
        float scale = 1.f;  // safe_h / 1080.0f
    };

    static SafeRect compute_safe_rect(uint32_t vp_w, uint32_t vp_h);
    void render_combat_strip(const nxrt::gfx::HudCanvas& c, const SafeRect& sr,
                             const DirectorRenderState& st) const;
    void render_exploration_bar(const nxrt::gfx::HudCanvas& c, const SafeRect& sr,
                                const DirectorRenderState& st) const;
    static void draw_diamond_avatar(const nxrt::gfx::HudCanvas& c, float cx, float cy,
                                    float half_diag, uint32_t fill_rgba, uint32_t border_rgba,
                                    const std::string& glyph, float alpha);
};

}  // namespace xc2::voice

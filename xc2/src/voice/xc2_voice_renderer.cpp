// SPDX-License-Identifier: GPL-3.0-or-later
#include "xc2_voice_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xc2::voice {
namespace {

const char* tag_label(SemanticTag t) {
    switch (t) {
        case SemanticTag::Break:        return "破防";
        case SemanticTag::Topple:       return "倒地";
        case SemanticTag::Launch:       return "浮空";
        case SemanticTag::Smash:        return "猛击";
        case SemanticTag::SpecialReady: return "必杀就绪";
        case SemanticTag::AggroWarning: return "仇恨警告";
        case SemanticTag::LowHPHelp:    return "危机";
        case SemanticTag::HealPotion:   return "恢复瓶";
        default:                        return nullptr;
    }
}

uint32_t tag_badge_color(SemanticTag t) {
    switch (t) {
        case SemanticTag::Break:        return 0xEC4899FFu;  // Magenta-pink
        case SemanticTag::Topple:       return 0x22C55EFFu;  // Emerald
        case SemanticTag::Launch:       return 0xF59E0BFFu;  // Amber
        case SemanticTag::Smash:        return 0xEF4444FFu;  // Crimson
        case SemanticTag::SpecialReady: return 0xF5C542FFu;  // Affinity Gold
        case SemanticTag::AggroWarning:
        case SemanticTag::LowHPHelp:    return 0xEF4444FFu;  // Warning Red
        case SemanticTag::HealPotion:   return 0x10B981FFu;  // Mint Green
        default:                        return 0x38BDF8FFu;
    }
}

}  // namespace

VoiceRenderer::SafeRect VoiceRenderer::compute_safe_rect(uint32_t vp_w, uint32_t vp_h) {
    SafeRect sr;
    const float fw = static_cast<float>(vp_w);
    const float fh = static_cast<float>(vp_h);
    sr.h = fh;
    sr.w = std::min(fw, fh * (16.0f / 9.0f));
    sr.x = (fw - sr.w) * 0.5f;
    sr.y = 0.0f;
    sr.scale = std::max(0.5f, sr.h / 1080.0f);
    return sr;
}

void VoiceRenderer::draw_diamond_avatar(const nxrt::gfx::HudCanvas& c, float cx, float cy,
                                        float half_diag, uint32_t fill_rgba, uint32_t border_rgba,
                                        const std::string& glyph, float alpha) {
    // 45-degree rotated Core Crystal diamond frame
    const float outer[8] = {
        cx,             cy - half_diag,
        cx + half_diag, cy,
        cx,             cy + half_diag,
        cx - half_diag, cy,
    };
    const float inner_r = half_diag * 0.80f;
    const float inner[8] = {
        cx,           cy - inner_r,
        cx + inner_r, cy,
        cx,           cy + inner_r,
        cx - inner_r, cy,
    };

    // Dark backing plate + colored core fill + Core Crystal facet highlight + crisp bevel border
    c.convex_poly_filled(outer, 4, with_alpha(0x0B121EE8u, alpha));
    c.convex_poly_filled(inner, 4, with_alpha(fill_rgba, alpha * 0.90f));
    // Upper-left Core Crystal specular facet for authentic XC2 crystal depth
    c.triangle_filled(cx, cy - inner_r, cx + inner_r * 0.55f, cy - inner_r * 0.15f,
                      cx - inner_r * 0.85f, cy, with_alpha(0xFFFFFF44u, alpha));
    c.polyline(outer, 4, with_alpha(border_rgba, alpha), true, std::max(1.2f, half_diag * 0.09f));
    c.polyline(inner, 4, with_alpha(0xFFFFFF48u, alpha), true, 1.0f);

    if (!glyph.empty()) {
        const float font_sz = std::max(11.0f, half_diag * 1.05f);
        float tw = 0.f, th = 0.f;
        c.measure_text(font_sz, glyph.c_str(), &tw, &th);
        c.text_outlined(cx - tw * 0.5f, cy - th * 0.5f, font_sz,
                        with_alpha(0xFFFFFFFFu, alpha),
                        with_alpha(0x000000D0u, alpha),
                        glyph.c_str());
    }
}

void VoiceRenderer::render_combat_strip(const nxrt::gfx::HudCanvas& c, const SafeRect& sr,
                                        const DirectorRenderState& st) const {
    if (st.combat_cards.empty()) return;

    const float base_x   = sr.x + sr.w * 0.022f;
    const float base_y   = sr.y + sr.h * 0.41f;
    const float card_w   = sr.w * 0.235f;
    const float row_h    = sr.h * 0.048f;
    const float gap      = sr.h * 0.010f;
    const float chamfer  = 8.0f * sr.scale;
    const float border_w = std::max(1.2f, 1.5f * sr.scale);

    // Layout active (non-exiting) cards from newest (slot 0) upward/downward cleanly
    // Spec: stationary slots starting at base_y, older card sits in upper slot (slot 0), newer in lower slot (slot 1)
    // or newest pushes older up by 1 slot. We stack from top (older) to bottom (newer).
    int active_idx = 0;
    float cur_y = base_y;
    for (const auto& card : st.combat_cards) {
        const float a = card.alpha * st.global_alpha;
        if (a <= 0.005f) continue;

        const bool merged = (card.partner_speaker_id != 0 && !card.text_line2.empty());
        const float card_h = merged ? (row_h * 1.48f) : row_h;
        const float y0 = cur_y + card.y_offset * (row_h + gap);
        const float y1 = y0 + card_h;
        const float x0 = base_x;
        const float x1 = base_x + card_w;

        // 45-degree chamfered top-left and bottom-right 6-vertex polygon
        const float poly[12] = {
            x0 + chamfer, y0,
            x1,           y0,
            x1,           y1 - chamfer,
            x1 - chamfer, y1,
            x0,           y1,
            x0,           y0 + chamfer,
        };

        // Horizontal linear gradient: rgba(12,18,28,0.86) -> rgba(12,18,28,0.0)
        const uint32_t bg_left  = with_alpha(0x0C121CDBu, a);
        const uint32_t bg_right = with_alpha(0x0C121C00u, a);
        c.convex_poly_hgrad(poly, 6, x0, x1, bg_left, bg_right);

        // Dynamic border color
        uint32_t stroke_col = 0x38BDF8A6u;  // Ether Cyan @ 65%
        if (card.semantic_tag == SemanticTag::AggroWarning ||
            card.semantic_tag == SemanticTag::LowHPHelp) {
            stroke_col = 0xEF4444F0u;       // Warning Red
        } else if (card.is_gold_affinity) {
            stroke_col = 0xF5C542F0u;       // Gold Affinity
        }
        // Left-side accent polyline (top-left chamfer + left edge + bottom-left corner, fading cleanly on right)
        const float accent_edge[8] = {
            x0 + card_w * 0.55f, y0,
            x0 + chamfer,        y0,
            x0,                  y0 + chamfer,
            x0,                  y1,
        };
        c.polyline(accent_edge, 4, with_alpha(stroke_col, a), false, border_w);
        // Subtle bottom-left to bottom-right hairline
        c.rect_filled_hgrad(x0, y1 - border_w, x1 - chamfer, y1,
                            with_alpha(stroke_col, a * 0.75f),
                            with_alpha(stroke_col, 0.f));

        // Left Core Crystal 45° diamond avatar
        const float diag = 16.0f * sr.scale;  // 32px full span
        const float av_cx = x0 + 24.0f * sr.scale;
        const float av_cy = (y0 + y1) * 0.5f - (merged ? 3.0f * sr.scale : 0.f);
        draw_diamond_avatar(c, av_cx, av_cy, diag, card.primary_color_rgba, stroke_col,
                            card.primary_glyph, a);

        if (merged) {
            const float sub_diag = diag * 0.65f;
            draw_diamond_avatar(c, av_cx + diag * 0.68f, av_cy + diag * 0.62f, sub_diag,
                                card.partner_color_rgba, 0xF5C542EEu, card.partner_glyph, a);
        }

        // Text column
        const float tx = x0 + (merged ? 54.0f : 48.0f) * sr.scale;
        const float name_sz = std::max(12.0f, 15.0f * sr.scale);
        const float body_sz = std::max(13.0f, 17.5f * sr.scale);
        const float header_y = y0 + 5.0f * sr.scale;

        // Speaker name (+ partner name if merged)
        std::string header = card.primary_name;
        if (merged && !card.partner_name.empty()) {
            header += " & " + card.partner_name;
        }
        c.text_outlined(tx, header_y, name_sz,
                        with_alpha(card.primary_color_rgba, a),
                        with_alpha(0x000000D8u, a),
                        header.c_str());

        // Optional semantic tag pill beside speaker name
        if (const char* tl = tag_label(card.semantic_tag)) {
            float nw = 0.f, nh = 0.f;
            c.measure_text(name_sz, header.c_str(), &nw, &nh);
            const float px = tx + nw + 8.0f * sr.scale;
            const float pill_sz = std::max(10.5f, 12.5f * sr.scale);
            float pw = 0.f, ph = 0.f;
            c.measure_text(pill_sz, tl, &pw, &ph);
            const float pad_x = 5.0f * sr.scale;
            const uint32_t bc = tag_badge_color(card.semantic_tag);
            c.rect_filled(px, header_y + 1.0f * sr.scale, px + pw + pad_x * 2.f,
                          header_y + ph + 2.0f * sr.scale, with_alpha(bc, a * 0.28f), 2.5f * sr.scale);
            c.rect(px, header_y + 1.0f * sr.scale, px + pw + pad_x * 2.f,
                   header_y + ph + 2.0f * sr.scale, with_alpha(bc, a * 0.85f), 2.5f * sr.scale, 1.0f);
            c.text(px + pad_x, header_y + 1.5f * sr.scale, pill_sz, with_alpha(0xFFFFFFFFu, a), tl);
        }

        // Line 1 text
        const float l1_y = y0 + 23.0f * sr.scale;
        c.text_outlined(tx, l1_y, body_sz,
                        with_alpha(0xF8FAFCFFu, a),
                        with_alpha(0x000000E0u, a),
                        card.text_line1.c_str());

        // Line 2 text (Driver + Blade merged response)
        if (merged) {
            const float l2_y = l1_y + 22.0f * sr.scale;
            c.text_outlined(tx, l2_y, body_sz * 0.94f,
                            with_alpha(0xFDE68AFFu, a),
                            with_alpha(0x000000E0u, a),
                            card.text_line2.c_str());
        }

        if (!card.exiting) {
            cur_y += card_h + gap;
            ++active_idx;
        }
    }
    (void)active_idx;
}

void VoiceRenderer::render_exploration_bar(const nxrt::gfx::HudCanvas& c, const SafeRect& sr,
                                           const DirectorRenderState& st) const {
    if (st.exploration_lines.empty()) return;

    const float cx = sr.x + sr.w * 0.5f;
    // Shift slightly upward if 3D nav skip-travel banner is occupying the bottom band
    const float base_y = sr.y + sr.h * (st.nav_bottom_busy ? 0.78f : 0.84f);
    const float name_sz = std::max(14.0f, 18.0f * sr.scale);
    const float body_sz = std::max(16.0f, 22.0f * sr.scale);

    // Measure widest line to size the soft protective backdrop & pale gold rule
    float max_w = sr.w * 0.34f;
    for (const auto& l : st.exploration_lines) {
        float bw = 0.f, bh = 0.f;
        c.measure_text(body_sz, l.text_line1.c_str(), &bw, &bh);
        if (bw + 80.f * sr.scale > max_w) max_w = bw + 80.f * sr.scale;
    }
    const float half_w = std::min(sr.w * 0.44f, max_w * 0.5f);

    const float top_alpha = st.exploration_lines.back().alpha * st.global_alpha;
    if (top_alpha <= 0.005f) return;

    // Soft center-weighted dark vignette mask (two opposing horizontal gradients)
    const float vig_top = base_y - (st.exploration_lines.size() > 1 ? 54.0f : 26.0f) * sr.scale;
    const float vig_bot = base_y + 42.0f * sr.scale;
    c.rect_filled_hgrad(cx - half_w * 1.18f, vig_top, cx, vig_bot,
                        with_alpha(0x060A1200u, top_alpha),
                        with_alpha(0x060A1288u, top_alpha));
    c.rect_filled_hgrad(cx, vig_top, cx + half_w * 1.18f, vig_bot,
                        with_alpha(0x060A1288u, top_alpha),
                        with_alpha(0x060A1200u, top_alpha));

    // 1px pale gold horizontal decorative rule (fading out on both ends)
    const float rule_y = base_y - 4.0f * sr.scale;
    const float rule_h = std::max(1.0f, 1.2f * sr.scale);
    c.rect_filled_hgrad(cx - half_w, rule_y, cx, rule_y + rule_h,
                        with_alpha(0xE8D28C00u, top_alpha),
                        with_alpha(0xE8D28CC8u, top_alpha));
    c.rect_filled_hgrad(cx, rule_y, cx + half_w, rule_y + rule_h,
                        with_alpha(0xE8D28CC8u, top_alpha),
                        with_alpha(0xE8D28C00u, top_alpha));

    // Render previous line (floating slightly above at 0.60 alpha) and current line (centered bright)
    const size_t n = st.exploration_lines.size();
    for (size_t i = 0; i < n; ++i) {
        const auto& line = st.exploration_lines[i];
        const bool is_prev = (n == 2 && i == 0);
        const float a = line.alpha * st.global_alpha * (is_prev ? 0.60f : 1.0f);
        if (a <= 0.005f) continue;

        const float y_off = is_prev ? (-34.0f * sr.scale) : (4.0f * sr.scale);
        const float cur_body_sz = is_prev ? (body_sz * 0.88f) : body_sz;
        const float cur_name_sz = is_prev ? (name_sz * 0.88f) : name_sz;

        if (!is_prev) {
            // Current speaker name centered just above the gold rule
            float nw = 0.f, nh = 0.f;
            c.measure_text(cur_name_sz, line.primary_name.c_str(), &nw, &nh);
            c.text_outlined(cx - nw * 0.5f, rule_y - nh - 4.0f * sr.scale, cur_name_sz,
                            with_alpha(line.primary_color_rgba, a),
                            with_alpha(0x000000E0u, a),
                            line.primary_name.c_str());

            float bw = 0.f, bh = 0.f;
            c.measure_text(cur_body_sz, line.text_line1.c_str(), &bw, &bh);
            c.text_outlined(cx - bw * 0.5f, base_y + y_off, cur_body_sz,
                            with_alpha(0xFFFFFFFFu, a),
                            with_alpha(0x000000E8u, a),
                            line.text_line1.c_str());
        } else {
            // Previous line: compact "Name: text" floating above
            const std::string combined = line.primary_name + "：" + line.text_line1;
            float bw = 0.f, bh = 0.f;
            c.measure_text(cur_body_sz, combined.c_str(), &bw, &bh);
            c.text_outlined(cx - bw * 0.5f, rule_y + y_off - bh * 0.5f, cur_body_sz,
                            with_alpha(0xE2E8F0FFu, a),
                            with_alpha(0x000000D0u, a),
                            combined.c_str());
        }
    }
}

void VoiceRenderer::render(const nxrt::gfx::HudCanvas& c, const DirectorRenderState& st) const {
    if (c.width == 0 || c.height == 0 || !c.draw_list) return;
    if (st.mode == UIVisibilityMode::Hidden || st.global_alpha <= 0.005f) return;

    const SafeRect sr = compute_safe_rect(c.width, c.height);
    // Render whichever track has active items so cards/banter never vanish across the 0.2s Combat<->Exploration transition.
    if (!st.combat_cards.empty()) {
        render_combat_strip(c, sr, st);
    }
    if (!st.exploration_lines.empty()) {
        render_exploration_bar(c, sr, st);
    }
}

void VoiceRenderer::render_voice_log(const nxrt::gfx::HudCanvas& c,
                                     const std::deque<VoiceLogEntry>& history) const {
    if (c.width == 0 || c.height == 0 || !c.draw_list) return;
    const SafeRect sr = compute_safe_rect(c.width, c.height);

    const float panel_w = sr.w * 0.30f;
    const float panel_h = sr.h * 0.56f;
    const float x1 = sr.x + sr.w * 0.978f;
    const float x0 = x1 - panel_w;
    const float y0 = sr.y + sr.h * 0.16f;
    const float y1 = y0 + panel_h;
    const float chamfer = 10.0f * sr.scale;

    const float poly[12] = {
        x0 + chamfer, y0,
        x1,           y0,
        x1,           y1 - chamfer,
        x1 - chamfer, y1,
        x0,           y1,
        x0,           y0 + chamfer,
    };
    c.convex_poly_filled(poly, 6, 0x0A101AE0u);
    c.polyline(poly, 6, 0x38BDF8C0u, true, std::max(1.2f, 1.5f * sr.scale));

    // Header
    const float hdr_sz = std::max(14.0f, 17.0f * sr.scale);
    const float pad_x = 14.0f * sr.scale;
    const float hdr_y = y0 + 10.0f * sr.scale;
    char hdr[64];
    std::snprintf(hdr, sizeof(hdr), "语音记录 Voice Log (%zu/30)  [Ctrl+F5 关闭]", history.size());
    c.text_outlined(x0 + pad_x, hdr_y, hdr_sz, 0xF5C542FFu, 0x000000E0u, hdr);

    const float div_y = hdr_y + 24.0f * sr.scale;
    c.rect_filled_hgrad(x0 + pad_x, div_y, x1 - pad_x, div_y + std::max(1.0f, 1.2f * sr.scale),
                        0x38BDF8B0u, 0x38BDF810u);

    const float row_sz = std::max(12.0f, 15.0f * sr.scale);
    const float row_h = 24.0f * sr.scale;
    float cur_y = div_y + 10.0f * sr.scale;

    if (history.empty()) {
        c.text(x0 + pad_x, cur_y, row_sz, 0x94A3B8CCu, "暂无已记录的队友语音。");
        return;
    }

    const size_t max_rows = static_cast<size_t>(std::max(1.0f, (y1 - cur_y - 12.0f * sr.scale) / row_h));
    const size_t start = (history.size() > max_rows) ? (history.size() - max_rows) : 0;
    for (size_t i = start; i < history.size(); ++i) {
        const auto& e = history[i];
        const std::string sp = e.speaker_name + "：";
        float sw = 0.f, sh = 0.f;
        c.measure_text(row_sz, sp.c_str(), &sw, &sh);
        c.text_outlined(x0 + pad_x, cur_y, row_sz, e.color_rgba, 0x000000D0u, sp.c_str());
        c.text_outlined(x0 + pad_x + sw + 2.0f * sr.scale, cur_y, row_sz,
                        0xF1F5F9FFu, 0x000000D0u, e.text.c_str());
        cur_y += row_h;
    }
}

}  // namespace xc2::voice

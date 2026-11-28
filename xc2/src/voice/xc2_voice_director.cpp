// SPDX-License-Identifier: GPL-3.0-or-later
#include "xc2_voice_director.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "nxrt/log.h"

namespace xc2::voice {
namespace {

constexpr const char* kTag = "voice";

float ease_out_cubic(float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    const float inv = 1.f - t;
    return 1.f - inv * inv * inv;
}

std::string truncate_utf8(const std::string& s, size_t max_chars) {
    size_t count = 0;
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0xE0u) == 0xC0u)      len = 2;
        else if ((c & 0xF0u) == 0xE0u) len = 3;
        else if ((c & 0xF8u) == 0xF0u) len = 4;
        if (count >= max_chars) return s.substr(0, i) + "...";
        i += std::min(len, s.size() - i);
        ++count;
    }
    return s;
}

}  // namespace

void VoiceDirector::push_event(const VoiceTriggerEvent& ev) {
    std::lock_guard lk(queue_mu_);
    if (pending_events_.size() >= 64) pending_events_.pop_front();
    pending_events_.push_back(ev);
}

void VoiceDirector::clear_active() {
    state_.combat_cards.clear();
    state_.exploration_lines.clear();
    exploration_backlog_.clear();
}

void VoiceDirector::record_history(uint64_t ts_ms, const SpeakerInfo* sp, SemanticTag tag, const std::string& text) {
    VoiceLogEntry e;
    e.timestamp_ms = ts_ms;
    e.speaker_id = sp ? sp->id : 0;
    e.color_rgba = sp ? sp->color_rgba : 0x38BDF8FFu;
    e.tag = tag;
    e.speaker_name = sp ? sp->name : "???";
    e.text = text;
    if (history_.size() >= kMaxHistory) history_.pop_front();
    history_.push_back(std::move(e));
}

void VoiceDirector::process_event(const VoiceTriggerEvent& ev, const GameStateSnapshot& gs, bool hidden_history_only) {
    if (!db_) return;
    const CueEntry* cue = db_->find_cue(ev.event_name, ev.cue_id);
    CueEntry synth{};
    if (!cue) {
        if (debug_log_missing_) {
            NXLOG_I(kTag, "[MissingCue] event='%s' cue_id=%u speaker=%u combat=%d",
                    ev.event_name, ev.cue_id, ev.speaker_id, gs.in_combat ? 1 : 0);
            int kk = -1;
            if (std::strncmp(ev.event_name, "voice_", 6) == 0) {
                const char* p = ev.event_name + 6;
                int digits = 0;
                while (p[digits] >= '0' && p[digits] <= '9' && digits < 12) ++digits;
                if (digits >= 8) {
                    const int kpos = digits - 6;
                    kk = (p[kpos] - '0') * 10 + (p[kpos + 1] - '0');
                }
            }
            if (kk == 6 || kk == 38 || kk == 50) return;
            synth.key = ev.event_name;
            synth.speaker_id = ev.speaker_id;
            synth.priority = (kk == 44) ? VoicePriority::P0_StoryAndBanter : VoicePriority::P1_Tactical;
            synth.tag = (kk == 44) ? SemanticTag::Banter : SemanticTag::None;
            synth.text_full = std::string("[待翻译] ") + ev.event_name;
            synth.text_compact = synth.text_full;
            cue = &synth;
        } else {
            return;
        }
    }

    // 1. Blacklist filter
    if (cue->priority == VoicePriority::P3_Blacklist) return;

    // 2. Player density filter
    if (density_ == VoiceDensity::Minimal && cue->priority != VoicePriority::P0_StoryAndBanter) return;
    if (density_ == VoiceDensity::Balanced && cue->priority == VoicePriority::P2_AmbientCombat) return;

    const uint16_t sp_id = cue->speaker_id ? cue->speaker_id : ev.speaker_id;
    const SpeakerInfo* sp = db_->find_speaker(sp_id);

    // If HUD is currently hidden (menus/cutscenes/chain attack), log into Ctrl+F5 history
    // without triggering stale overlay popups or dropping them into a post-menu burst.
    if (hidden_history_only) {
        const std::string& log_text = !cue->text_full.empty() ? cue->text_full : cue->text_compact;
        if (!log_text.empty()) {
            record_history(ev.timestamp_ms, sp, cue->tag, log_text);
        }
        return;
    }

    // 3. Per-speaker semantic deduplication (0.8s) so the same character never double-triggers,
    //    while teammate call-and-response (e.g. Driver Break -> Blade follow-up) is never swallowed.
    if (cue->tag != SemanticTag::None && cue->tag != SemanticTag::Banter) {
        const uint32_t key = (static_cast<uint32_t>(sp_id) << 8) | static_cast<uint32_t>(cue->tag);
        auto it = last_sp_tag_time_.find(key);
        if (it != last_sp_tag_time_.end() && (clock_sec_ - it->second) < kSemanticDedupSec) {
            if (debug_log_missing_) {
                NXLOG_I(kTag, "[DropDedup] event='%s' sp=%u tag=%u", ev.event_name, sp_id, static_cast<unsigned>(cue->tag));
            }
            return;
        }
        last_sp_tag_time_[key] = clock_sec_;
    }

    // 4. Per-speaker P2 ambient combat pacing (1.2s) so battle-start/encouragement back-and-forth
    //    between different teammates (e.g. Rex -> Nia) is never swallowed.
    if (cue->priority == VoicePriority::P2_AmbientCombat) {
        auto it = last_sp_p2_time_.find(sp_id);
        if (it != last_sp_p2_time_.end() && (clock_sec_ - it->second) < kAmbientP2CdSec) {
            if (debug_log_missing_) {
                NXLOG_I(kTag, "[DropP2Cd] event='%s' sp=%u", ev.event_name, sp_id);
            }
            return;
        }
        last_sp_p2_time_[sp_id] = clock_sec_;
    }

    const float dur_hint = (ev.duration_sec > 0.f) ? ev.duration_sec : cue->duration_sec;

    const std::string& log_text = !cue->text_full.empty() ? cue->text_full : cue->text_compact;
    record_history(ev.timestamp_ms, sp, cue->tag, log_text);

    // Route deterministically by track semantics + combat state so battle-start callouts (which often fire
    // 0.1-0.3s before GfGameBattle::isBattle() flips true) still appear on the left Combat tactical strip,
    // while P0 post-battle banter / victory lines always render on the bottom Exploration subtitle bar.
    const bool use_combat_strip =
        (cue->priority == VoicePriority::P1_Tactical || cue->priority == VoicePriority::P2_AmbientCombat) ||
        (last_non_hidden_mode_ == UIVisibilityMode::Combat && cue->priority != VoicePriority::P0_StoryAndBanter);
    if (use_combat_strip) {
        enqueue_combat(*cue, sp, dur_hint, gs);
    } else {
        enqueue_exploration(*cue, sp, dur_hint);
    }
}

void VoiceDirector::enqueue_combat(const CueEntry& cue, const SpeakerInfo* sp, float dur_hint, const GameStateSnapshot& gs) {
    // Always display the character's actual spoken line (text_full) rather than the generic tactical summary
    // (text_compact); semantic tactical tags (必杀就绪 / 破防 / 倒地 / 危机) are already shown as header badges.
    const std::string& raw_text = !cue.text_full.empty() ? cue.text_full : cue.text_compact;
    const size_t max_chars = (state_.ui_lang == VoiceUILang::En) ? 56 : 28;
    const std::string text = truncate_utf8(raw_text, max_chars);
    const float target_dur = std::max(dur_hint, 2.2f);

    // Check Driver + Blade partner merge within 2.0s
    if (sp && sp->partner_id != 0) {
        for (auto& card : state_.combat_cards) {
            if (!card.exiting && card.partner_speaker_id == 0 &&
                card.primary_speaker_id == sp->partner_id &&
                card.elapsed_sec <= kPartnerMergeSec) {
                card.partner_speaker_id = sp->id;
                card.partner_name = sp->name;
                card.partner_glyph = sp->short_glyph;
                card.partner_color_rgba = sp->color_rgba;
                if (sp->element_id != 0) card.element_id = sp->element_id;
                card.text_line2 = text;
                card.elapsed_sec = 0.f;
                card.target_duration_sec = std::max(card.target_duration_sec, target_dur + 0.6f);
                if (cue.tag == SemanticTag::AggroWarning || cue.tag == SemanticTag::LowHPHelp) {
                    card.semantic_tag = cue.tag;
                }
                return;
            }
        }
    }

    // Enforce max 2 active (non-exiting) cards; oldest exits immediately in 0.15s
    size_t active_cnt = 0;
    for (const auto& c : state_.combat_cards) {
        if (!c.exiting) ++active_cnt;
    }
    if (active_cnt >= kMaxCombatCards) {
        for (auto& c : state_.combat_cards) {
            if (!c.exiting) {
                c.exiting = true;
                c.exit_elapsed = 0.f;
                break;
            }
        }
    }

    // Slide existing active cards upward smoothly by 1 slot
    for (auto& c : state_.combat_cards) {
        if (!c.exiting) {
            c.slide_from = c.y_offset + 1.0f;
            c.slide_t = 0.f;
            c.y_offset = c.slide_from;
        }
    }

    ActiveVoiceCard card;
    card.uid = next_uid_++;
    card.primary_speaker_id = sp ? sp->id : cue.speaker_id;
    card.element_id = sp ? sp->element_id : 0;
    card.semantic_tag = cue.tag;
    card.is_gold_affinity = gs.affinity_gold[0] || gs.affinity_gold[1] || gs.affinity_gold[2];
    card.primary_color_rgba = sp ? sp->color_rgba : 0x38BDF8FFu;
    card.primary_name = sp ? sp->name : "???";
    card.primary_glyph = sp ? sp->short_glyph : "?";
    card.text_line1 = text;
    card.target_duration_sec = target_dur;
    card.alpha = 0.f;
    card.y_offset = 0.f;
    card.slide_from = 0.f;
    card.slide_t = 1.f;
    state_.combat_cards.push_back(std::move(card));
}

void VoiceDirector::enqueue_exploration(const CueEntry& cue, const SpeakerInfo* sp, float dur_hint) {
    const std::string& text = !cue.text_full.empty() ? cue.text_full : cue.text_compact;
    const float sec_per_cp = (state_.ui_lang == VoiceUILang::En) ? 0.075f : 0.16f;
    const float est = std::min(6.5f, static_cast<float>(count_utf8_codepoints(text)) * sec_per_cp + 1.4f);
    const float target_dur = std::max(dur_hint, est);

    ActiveVoiceCard line;
    line.uid = next_uid_++;
    line.primary_speaker_id = sp ? sp->id : cue.speaker_id;
    line.semantic_tag = cue.tag;
    line.primary_color_rgba = sp ? sp->color_rgba : 0xFACC15FFu;
    line.primary_name = sp ? sp->name : "???";
    line.primary_glyph = sp ? sp->short_glyph : "?";
    line.text_line1 = text;
    line.target_duration_sec = target_dur;
    line.alpha = 0.f;

    // If current line has been visible for at least 0.5s (or empty), promote immediately; otherwise queue up to 4
    const bool busy = !state_.exploration_lines.empty() &&
                      state_.exploration_lines.back().elapsed_sec < 0.55f;
    if (busy) {
        if (exploration_backlog_.size() < kMaxExplorationQ) {
            exploration_backlog_.push_back(std::move(line));
        }
        return;
    }

    if (state_.exploration_lines.size() >= 2) {
        state_.exploration_lines.erase(state_.exploration_lines.begin());
    }
    state_.exploration_lines.push_back(std::move(line));
}

void VoiceDirector::update(float dt_sec, const GameStateSnapshot& gs) {
    if (dt_sec < 0.f) dt_sec = 0.f;
    if (dt_sec > 0.1f) dt_sec = 0.1f;

    // 1. Determine visibility mode
    const bool should_hide = gs.in_cutscene || gs.in_menu_or_pause || gs.in_chain_attack ||
                             gs.in_special_cam || gs.hud_global_alpha < 0.1f;
    const UIVisibilityMode target_mode = should_hide
                                             ? UIVisibilityMode::Hidden
                                             : (gs.in_combat ? UIVisibilityMode::Combat
                                                             : UIVisibilityMode::Exploration);
    state_.nav_bottom_busy = gs.nav_bottom_busy;
    state_.rex_master_driver = gs.rex_master_driver;

    if (target_mode != UIVisibilityMode::Hidden) {
        // Mode transition between Combat and Exploration
        if (last_non_hidden_mode_ != target_mode) {
            last_non_hidden_mode_ = target_mode;
        }
        state_.mode = target_mode;
        state_.global_alpha = std::min(1.0f, state_.global_alpha + dt_sec / 0.12f) * gs.hud_global_alpha;
        clock_sec_ += dt_sec;
    } else {
        state_.global_alpha = std::max(0.0f, state_.global_alpha - dt_sec / 0.10f);
        if (state_.global_alpha <= 0.001f) {
            state_.mode = UIVisibilityMode::Hidden;
        }
        // Drain incoming queue to Ctrl+F5 history while hidden so stale menu/cutscene lines
        // never burst out onto the HUD when returning to gameplay.
        std::deque<VoiceTriggerEvent> hidden_batch;
        {
            std::lock_guard lk(queue_mu_);
            hidden_batch.swap(pending_events_);
        }
        for (const auto& ev : hidden_batch) {
            process_event(ev, gs, true);
        }
        return;
    }

    // 2. Drain thread-safe event queue
    std::deque<VoiceTriggerEvent> batch;
    {
        std::lock_guard lk(queue_mu_);
        batch.swap(pending_events_);
    }
    for (const auto& ev : batch) {
        process_event(ev, gs);
    }

    // 3. Advance Combat cards
    for (auto& c : state_.combat_cards) {
        if (c.slide_t < 1.f) {
            c.slide_t = std::min(1.f, c.slide_t + dt_sec / kSlideDurSec);
            c.y_offset = c.slide_from * (1.f - ease_out_cubic(c.slide_t));
        } else {
            c.y_offset = 0.f;
        }
        if (c.exiting) {
            c.exit_elapsed += dt_sec;
            c.alpha = std::max(0.f, 1.f - c.exit_elapsed / kExitFadeSec);
            c.y_offset -= dt_sec * 1.4f;
        } else {
            c.elapsed_sec += dt_sec;
            const float fade_in = std::min(1.f, c.elapsed_sec / 0.12f);
            const float rem = c.target_duration_sec - c.elapsed_sec;
            const float fade_out = (rem < 0.25f) ? std::max(0.f, rem / 0.25f) : 1.f;
            c.alpha = std::min(fade_in, fade_out);
            if (c.elapsed_sec >= c.target_duration_sec) {
                c.exiting = true;
                c.exit_elapsed = kExitFadeSec;
                c.alpha = 0.f;
            }
        }
    }
    state_.combat_cards.erase(
        std::remove_if(state_.combat_cards.begin(), state_.combat_cards.end(),
                       [](const ActiveVoiceCard& c) { return c.exiting && c.exit_elapsed >= kExitFadeSec; }),
        state_.combat_cards.end());

    // 4. Advance Exploration subtitle lines + backlog
    for (auto& l : state_.exploration_lines) {
        l.elapsed_sec += dt_sec;
        const float fade_in = std::min(1.f, l.elapsed_sec / 0.16f);
        const float rem = l.target_duration_sec - l.elapsed_sec;
        const float fade_out = (rem < 0.35f) ? std::max(0.f, rem / 0.35f) : 1.f;
        l.alpha = std::min(fade_in, fade_out);
    }
    state_.exploration_lines.erase(
        std::remove_if(state_.exploration_lines.begin(), state_.exploration_lines.end(),
                       [](const ActiveVoiceCard& l) { return l.elapsed_sec >= l.target_duration_sec; }),
        state_.exploration_lines.end());

    if (!exploration_backlog_.empty()) {
        const bool can_pop = state_.exploration_lines.empty() ||
                             state_.exploration_lines.back().elapsed_sec >=
                                 std::min(1.6f, state_.exploration_lines.back().target_duration_sec * 0.65f);
        if (can_pop) {
            if (state_.exploration_lines.size() >= 2) {
                state_.exploration_lines.erase(state_.exploration_lines.begin());
            }
            state_.exploration_lines.push_back(std::move(exploration_backlog_.front()));
            exploration_backlog_.pop_front();
        }
    }
}

}  // namespace xc2::voice

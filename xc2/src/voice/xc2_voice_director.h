// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic voice scheduling director: visibility state machine, priority filtering,
// 1.5s semantic dedup, 6.0s P2 cooldown, Driver+Blade partner merging, and 30-entry history.
#pragma once
#include <array>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "xc2_voice_db.h"
#include "xc2_voice_types.h"

namespace xc2::voice {

struct DirectorRenderState {
    UIVisibilityMode             mode              = UIVisibilityMode::Exploration;
    VoiceUILang                  ui_lang           = VoiceUILang::Zh;
    float                        global_alpha      = 1.0f;
    bool                         nav_bottom_busy   = false;
    bool                         rex_master_driver = false;
    std::vector<ActiveVoiceCard> combat_cards;       // Up to 3 active + exiting cards
    std::vector<ActiveVoiceCard> exploration_lines;  // Up to 2 lines (prev + current)
};

class VoiceDirector {
public:
    void set_database(const VoiceDatabase* db) { db_ = db; }
    void set_density(VoiceDensity d) { density_ = d; }
    VoiceDensity density() const { return density_; }
    void set_ui_lang(VoiceUILang lang) { state_.ui_lang = lang; }
    VoiceUILang ui_lang() const { return state_.ui_lang; }
    void set_debug_log_missing(bool v) { debug_log_missing_ = v; }

    // Thread-safe producer entry (callable from any thread).
    void push_event(const VoiceTriggerEvent& ev);

    // Per-frame update on the render/HUD thread.
    void update(float dt_sec, const GameStateSnapshot& gs);

    const DirectorRenderState& render_state() const { return state_; }
    const std::deque<VoiceLogEntry>& history() const { return history_; }

    void clear_active();

private:
    static constexpr size_t kMaxCombatCards    = 3;
    static constexpr size_t kMaxExplorationQ   = 6;
    static constexpr size_t kMaxHistory        = 30;
    static constexpr float  kSemanticDedupSec  = 0.8f;
    static constexpr float  kAmbientP2CdSec    = 1.2f;
    static constexpr float  kPartnerMergeSec   = 2.0f;
    static constexpr float  kExitFadeSec       = 0.15f;
    static constexpr float  kSlideDurSec       = 0.15f;

    void process_event(const VoiceTriggerEvent& ev, const GameStateSnapshot& gs, bool hidden_history_only = false);
    void enqueue_combat(const CueEntry& cue, const SpeakerInfo* sp, float dur_hint, const GameStateSnapshot& gs);
    void enqueue_exploration(const CueEntry& cue, const SpeakerInfo* sp, float dur_hint);
    void record_history(uint64_t ts_ms, const SpeakerInfo* sp, SemanticTag tag, const std::string& text);

    const VoiceDatabase* db_ = nullptr;
    VoiceDensity density_ = VoiceDensity::Full;
    bool debug_log_missing_ = false;

    std::mutex queue_mu_;
    std::deque<VoiceTriggerEvent> pending_events_;

    double clock_sec_ = 0.0;
    uint32_t next_uid_ = 1;
    std::array<double, 16> last_tag_time_{};
    std::unordered_map<uint32_t, double> last_sp_tag_time_;
    std::unordered_map<uint16_t, double> last_sp_p2_time_;

    UIVisibilityMode last_non_hidden_mode_ = UIVisibilityMode::Exploration;
    DirectorRenderState state_;
    std::deque<ActiveVoiceCard> exploration_backlog_;
    std::deque<VoiceLogEntry>   history_;
};

}  // namespace xc2::voice

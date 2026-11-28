// SPDX-License-Identifier: GPL-3.0-or-later
// Core cross-layer data structures for XC2 Native-Style Voice Visualization System.
#pragma once
#include <cstdint>
#include <string>

namespace xc2::voice {

enum class VoicePriority : uint8_t {
    P0_StoryAndBanter = 0,  // Post-battle banter, field chatter, field skills (always shown)
    P1_Tactical       = 1,  // Break/Topple/Launch/Smash, Special ready, Aggro/LowHP, Heal potion
    P2_AmbientCombat  = 2,  // Praise, heal thanks, encouragement (global cooldown)
    P3_Blacklist      = 3,  // Pure art names, attack grunts (dropped immediately)
};

enum class VoiceDensity : uint8_t {
    Minimal  = 0,  // P0 only
    Balanced = 1,  // P0 + P1
    Full     = 2,  // P0 + P1 + P2 (default)
};

enum class VoiceUILang : uint8_t {
    Zh = 0,  // zh_cn / zh_tw
    En = 1,  // en
    Ja = 2,  // ja
};

enum class SemanticTag : uint8_t {
    None = 0,
    Break,
    Topple,
    Launch,
    Smash,
    SpecialReady,
    AggroWarning,
    LowHPHelp,
    HealPotion,
    Banter,
};

enum class UIVisibilityMode : uint8_t {
    Hidden      = 0,  // Menus, cutscenes, special camera, chain attack, loading
    Exploration = 1,  // Field exploration & post-battle banter -> bottom subtitle bar
    Combat      = 2,  // Active combat -> top-left tactical strip below party HP
};

// Lightweight event pushed into the thread-safe queue (safe to construct on any thread).
struct VoiceTriggerEvent {
    char     event_name[64] = {};  // Primary Wwise ASCII event name (e.g. "voice_...")
    uint32_t cue_id         = 0;   // Optional numeric fallback ID / Wwise AkPlayingID
    uint16_t speaker_id     = 0;   // Optional speaker hint (0 = resolve from DB)
    float    duration_sec   = 0.f; // 0.0f = Director estimates from text / DB
    uint64_t timestamp_ms   = 0;
};

// Read-only per-frame snapshot of live game state.
struct GameStateSnapshot {
    bool  in_combat        = false;
    bool  in_cutscene      = false;
    bool  in_menu_or_pause = false;
    bool  in_chain_attack  = false;
    bool  in_special_cam   = false;
    float hud_global_alpha = 1.0f;
    bool  affinity_gold[3]  = {false, false, false};
    bool  nav_bottom_busy   = false;  // True when 3D nav banner occupies bottom area
    bool  rex_master_driver = false;  // True when Scenario >= 7060 (Ch.7 awakening / Ch.8+ Master Driver Rex)
};

struct SpeakerInfo {
    uint16_t    id          = 0;
    std::string name;
    std::string short_glyph;         // 1-char UTF-8 monogram for procedural diamond badge
    bool        is_blade    = false;
    uint16_t    partner_id  = 0;
    uint8_t     element_id  = 0;     // 0=none, 1=fire, 2=water, 3=wind, 4=earth, 5=elec, 6=ice, 7=light, 8=dark
    uint32_t    color_rgba  = 0x38BDF8FFu;
};

struct CueEntry {
    std::string   key;
    uint16_t      speaker_id   = 0;
    VoicePriority priority     = VoicePriority::P2_AmbientCombat;
    SemanticTag   tag          = SemanticTag::None;
    float         duration_sec = 0.f;
    std::string   text_full;
    std::string   text_compact;
};

// Render-ready card consumed by VoiceRenderer.
struct ActiveVoiceCard {
    uint32_t    uid                = 0;
    uint16_t    primary_speaker_id = 0;
    uint16_t    partner_speaker_id = 0;  // Non-zero = Driver + Blade merged card
    uint8_t     element_id         = 0;
    SemanticTag semantic_tag       = SemanticTag::None;
    bool        is_gold_affinity   = false;
    uint32_t    primary_color_rgba = 0x38BDF8FFu;
    uint32_t    partner_color_rgba = 0xEF4444FFu;
    std::string primary_name;
    std::string partner_name;
    std::string primary_glyph;
    std::string partner_glyph;
    std::string text_line1;
    std::string text_line2;
    float       elapsed_sec        = 0.f;
    float       target_duration_sec = 2.0f;
    float       alpha              = 0.f;  // 0..1 eased opacity
    float       y_offset           = 0.f;  // Ease-out vertical slide offset in normalized slot units
    float       slide_from         = 0.f;
    float       slide_t            = 1.f;  // 0..1 over 0.15s
    bool        exiting            = false;
    float       exit_elapsed       = 0.f;
};

struct VoiceLogEntry {
    uint64_t    timestamp_ms = 0;
    uint16_t    speaker_id   = 0;
    uint32_t    color_rgba   = 0xFFFFFFFFu;
    SemanticTag tag          = SemanticTag::None;
    std::string speaker_name;
    std::string text;
};

inline uint32_t with_alpha(uint32_t rgba, float alpha_mul) {
    if (alpha_mul <= 0.f) return rgba & 0xFFFFFF00u;
    if (alpha_mul >= 1.f) return rgba;
    const uint32_t a = static_cast<uint32_t>(static_cast<float>(rgba & 0xFFu) * alpha_mul + 0.5f);
    return (rgba & 0xFFFFFF00u) | (a > 255u ? 255u : a);
}

inline size_t count_utf8_codepoints(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0u) != 0x80u) ++n;
    }
    return n;
}

}  // namespace xc2::voice

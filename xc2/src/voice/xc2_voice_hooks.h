// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only Wwise SoundManager active-voice poller for XC2 (retail 3.0.1).
// Walks SoundManager + 0x110 (libc++ std::unordered_map active-playing node list)
// with zero sites-main.txt hooks, zero re-lift, and zero audio-thread interference.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "xc2_voice_types.h"

namespace xc2::voice {

class VoiceMemoryPoller {
public:
    using EmitFn = std::function<void(const VoiceTriggerEvent&)>;

    void reset();
    void set_debug_capture(bool enabled, const std::string& dump_path = "");

    // Poll guest SoundManager singleton once per frame (read-only via nxrt::mem::is_mapped).
    // Emits a VoiceTriggerEvent for every newly started Wwise voice event.
    void poll(uint64_t main_base, uint64_t now_ms, bool in_combat, const EmitFn& emit);

    uint32_t last_active_sound_count() const { return last_active_sounds_; }
    uint32_t total_voices_seen() const { return total_voices_seen_; }

private:
    struct ActiveTrack {
        std::string event_name;
        uint32_t    category   = 0;
        uint64_t    start_ms   = 0;
        bool        in_combat  = false;
        bool        is_voice   = false;
        uint64_t    last_seen_frame = 0;
    };

    static bool     is_voice_event_name(const char* name, uint32_t category);
    static uint16_t infer_speaker_from_event(const char* name);
    void            dump_finished_voice(uint32_t playing_id, const ActiveTrack& tr, uint64_t end_ms);

    bool        debug_capture_ = false;
    std::string dump_path_;
    uint64_t    frame_seq_ = 0;
    uint32_t    last_active_sounds_ = 0;
    uint32_t    total_voices_seen_ = 0;
    uint32_t    debug_log_budget_ = 0;
    std::unordered_map<uint32_t, ActiveTrack> active_;  // key: Wwise AkPlayingID
};

}  // namespace xc2::voice

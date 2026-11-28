// SPDX-License-Identifier: GPL-3.0-or-later
// Voice subtitle mapping database: O(1) lookup by Wwise event name or numeric CueID,
// with self-contained JSON loader and built-in Phase-1 mock dataset.
#pragma once
#include <string>
#include <unordered_map>

#include "xc2_voice_types.h"

namespace xc2::voice {

class VoiceDatabase {
public:
    void clear();
    // Load external JSON mapping file (schema: { "speakers": {...}, "cues": {...} }).
    // Returns true if loaded and parsed successfully.
    bool load_json_file(const std::string& path, std::string* err_out = nullptr);

    // Seed built-in mock speakers & demo cues for Phase 1 UI verification (never overwrites loaded keys).
    void seed_mock_entries();

    const SpeakerInfo* find_speaker(uint16_t id) const;
    const CueEntry*    find_cue(const char* event_name, uint32_t cue_id) const;

    size_t speaker_count() const { return speakers_.size(); }
    size_t cue_count() const { return cues_.size(); }

private:
    std::unordered_map<uint16_t, SpeakerInfo>  speakers_;
    std::unordered_map<std::string, CueEntry>  cues_;
};

}  // namespace xc2::voice

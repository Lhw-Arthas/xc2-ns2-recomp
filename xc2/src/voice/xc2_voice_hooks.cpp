// SPDX-License-Identifier: GPL-3.0-or-later
#include "xc2_voice_hooks.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_set>

#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace xc2::voice {
namespace {

constexpr const char* kTag = "voice";

// Verified retail 3.0.1 main-relative offsets (Phase 0 disassembly of 0x7295d0 / 0x728fa8 / 0x72a23c):
//   SoundManager* mgr = *(uint64_t*)(main_base + 0x11befc8)
//   mgr + 0x0F8 : std::unordered_map<uint64_t gameObjId, ActiveSoundEntry>
//   mgr + 0x110 : Node* head (__p1_.__value_.__next_)
//   Node layout :
//     +0x00 : Node* next
//     +0x10 : uint64_t gameObjId
//     +0x20 : uint32_t akPlayingId
//     +0x28 : char eventName[64]
//     +0x68 : uint32_t eventNameLen
//     +0x8c : uint32_t category (0..17)
namespace off301 {
constexpr uint64_t kSoundManagerSingleton = 0x11befc8;
constexpr uint64_t kActiveListHead        = 0x110;
constexpr uint64_t kNodeNext              = 0x00;
constexpr uint64_t kNodePlayingId         = 0x20;
constexpr uint64_t kNodeEventName         = 0x28;
constexpr uint64_t kNodeEventNameLen      = 0x68;
constexpr uint64_t kNodeCategory          = 0x8c;
}  // namespace off301

template <typename T>
bool safe_read(uint64_t addr, T& out) {
    if (addr < 0x10000ull || addr > UINT64_MAX - sizeof(T)) return false;
    if (!nxrt::mem::is_mapped(addr, sizeof(T))) return false;
    std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
    return true;
}

bool safe_read_bytes(uint64_t addr, void* dst, size_t n) {
    if (addr < 0x10000ull || addr > UINT64_MAX - n) return false;
    if (!nxrt::mem::is_mapped(addr, n)) return false;
    std::memcpy(dst, reinterpret_cast<const void*>(addr), n);
    return true;
}

bool starts_with(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

bool is_printable_ascii(const char* s, size_t len) {
    if (len == 0) return false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20u || c > 0x7Eu) return false;
    }
    return true;
}

}  // namespace

void VoiceMemoryPoller::reset() {
    active_.clear();
    frame_seq_ = 0;
    last_active_sounds_ = 0;
    total_voices_seen_ = 0;
    debug_log_budget_ = 0;
}

void VoiceMemoryPoller::set_debug_capture(bool enabled, const std::string& dump_path) {
    debug_capture_ = enabled;
    dump_path_ = dump_path;
}

bool VoiceMemoryPoller::is_voice_event_name(const char* name, uint32_t category) {
    if (!name || !*name) return false;
    // Verified from live 3.0.1 capture: all party/blade voices use Wwise category 2 and "voice_CCKKNNVV_Play"
    if (starts_with(name, "voice_") || starts_with(name, "blade_") ||
        starts_with(name, "Voice_") || starts_with(name, "evt_voice")) {
        // Exclude UI/system sound effects that happen to start with blade_
        if (starts_with(name, "blade_change") || starts_with(name, "blade_select") ||
            starts_with(name, "blade_hold") || starts_with(name, "blade_solo") ||
            starts_with(name, "blade_l_")) {
            return false;
        }
        return true;
    }
    return (category == 2u);
}

uint16_t VoiceMemoryPoller::infer_speaker_from_event(const char* name) {
    // Verified directly against XC2 3.0.1 common.bdat (CHR_Dr / CHR_Bl Voice column):
    //   8 digits: voice_CCKKNNVV_Play   -> speaker_id = CC  (1..80, exact BDAT VoiceID)
    //   9 digits: voice_CCCKKNNVV_Play  -> speaker_id = CCC (301..709, e.g. 501..506 DLC Blades, 701..709 Torna cast)
    //   Last 6 digits are always KK (2-digit category) + NNVV (4-digit line/variant).
    if (!name) return 0;
    const char* p = nullptr;
    if (starts_with(name, "voice_"))      p = name + 6;
    else if (starts_with(name, "blade_")) p = name + 6;
    if (!p) return 0;

    int digits = 0;
    while (p[digits] >= '0' && p[digits] <= '9' && digits < 12) ++digits;
    if (digits < 8) return 0;

    const int id_len = digits - 6;  // 2 for 8-digit, 3 for 9-digit
    int code = 0;
    for (int i = 0; i < id_len; ++i) code = code * 10 + (p[i] - '0');
    return (code > 0 && code < 1000) ? static_cast<uint16_t>(code) : 0;
}

void VoiceMemoryPoller::dump_finished_voice(uint32_t playing_id, const ActiveTrack& tr, uint64_t end_ms) {
    if (!debug_capture_ || !tr.is_voice || dump_path_.empty()) return;
    const float dur = (end_ms >= tr.start_ms) ? static_cast<float>(end_ms - tr.start_ms) * 0.001f : 0.f;
    std::ofstream f(dump_path_, std::ios::app);
    if (!f.is_open()) return;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"event\":\"%s\",\"playing_id\":%u,\"cat\":%u,\"in_combat\":%s,\"duration_sec\":%.2f,\"ts_ms\":%llu}\n",
                  tr.event_name.c_str(), playing_id, tr.category,
                  tr.in_combat ? "true" : "false", dur,
                  static_cast<unsigned long long>(tr.start_ms));
    f << buf;
}

void VoiceMemoryPoller::poll(uint64_t main_base, uint64_t now_ms, bool in_combat, const EmitFn& emit) {
    if (!main_base) return;
    ++frame_seq_;

    uint64_t mgr = 0;
    if (!safe_read(main_base + off301::kSoundManagerSingleton, mgr) || mgr < 0x10000ull) {
        last_active_sounds_ = 0;
        return;
    }

    uint64_t node = 0;
    if (!safe_read(mgr + off301::kActiveListHead, node)) {
        last_active_sounds_ = 0;
        return;
    }

    std::unordered_set<uint64_t> visited;
    uint32_t count = 0;
    constexpr uint32_t kMaxWalk = 256;

    while (node >= 0x10000ull && count < kMaxWalk) {
        if (!visited.insert(node).second) break;  // Cycle guard for lock-free snapshot

        uint64_t next = 0;
        uint32_t playing_id = 0;
        uint32_t name_len = 0;
        uint32_t category = 0;
        if (!safe_read(node + off301::kNodeNext, next) ||
            !safe_read(node + off301::kNodePlayingId, playing_id) ||
            !safe_read(node + off301::kNodeEventNameLen, name_len) ||
            !safe_read(node + off301::kNodeCategory, category)) {
            break;
        }

        if (playing_id != 0 && name_len > 0 && name_len < 64 && category <= 32) {
            char name_buf[64] = {};
            if (safe_read_bytes(node + off301::kNodeEventName, name_buf, name_len)) {
                name_buf[name_len] = '\0';
                if (is_printable_ascii(name_buf, name_len)) {
                    ++count;
                    auto it = active_.find(playing_id);
                    if (it != active_.end()) {
                        it->second.last_seen_frame = frame_seq_;
                    } else {
                        const bool is_v = is_voice_event_name(name_buf, category);
                        ActiveTrack tr;
                        tr.event_name = name_buf;
                        tr.category = category;
                        tr.start_ms = now_ms;
                        tr.in_combat = in_combat;
                        tr.is_voice = is_v;
                        tr.last_seen_frame = frame_seq_;
                        active_.emplace(playing_id, std::move(tr));

                        if (is_v) {
                            ++total_voices_seen_;
                            if (debug_capture_) {
                                NXLOG_I(kTag, "[VoiceLive] event='%s' playing_id=%u cat=%u combat=%d",
                                        name_buf, playing_id, category, in_combat ? 1 : 0);
                            }
                            if (emit) {
                                VoiceTriggerEvent ev{};
                                std::snprintf(ev.event_name, sizeof(ev.event_name), "%s", name_buf);
                                ev.cue_id = playing_id;
                                ev.speaker_id = infer_speaker_from_event(name_buf);
                                ev.duration_sec = 0.f;
                                ev.timestamp_ms = now_ms;
                                emit(ev);
                            }
                        } else if (debug_capture_ && debug_log_budget_ < 40) {
                            ++debug_log_budget_;
                            NXLOG_I(kTag, "[SoundLive] event='%s' playing_id=%u cat=%u combat=%d",
                                    name_buf, playing_id, category, in_combat ? 1 : 0);
                        }
                    }
                }
            }
        }

        node = next;
    }

    last_active_sounds_ = count;

    // Retire finished playing IDs and optionally dump exact measured duration
    for (auto it = active_.begin(); it != active_.end(); ) {
        if (it->second.last_seen_frame != frame_seq_) {
            dump_finished_voice(it->first, it->second, now_ms);
            it = active_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace xc2::voice

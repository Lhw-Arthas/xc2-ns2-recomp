// SPDX-License-Identifier: GPL-3.0-or-later
#include "xc2_voice_overlay.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/play.h"
#include "xc2_voice_db.h"
#include "xc2_voice_director.h"
#include "xc2_voice_hooks.h"
#include "xc2_voice_renderer.h"

namespace xc2::voice {
namespace {

constexpr const char* kTag = "voice";

// Verified retail 3.0.1 main-relative read-only offsets (shared with xc2_nav_overlay.cpp).
namespace off301 {
constexpr uint64_t kBattleSingleton = 0x10708e8;  // ptr -> +0x148 u8 bit 1 = GfGameBattle::isBattle()
constexpr uint64_t kEventSingleton  = 0x11a8db8;  // ptr -> +0x78  u32 bit 1 = GfGameEvent::isEvent()
}  // namespace off301

template <typename T>
bool safe_read(uint64_t addr, T& out) {
    if (addr < 0x10000ull || addr > UINT64_MAX - sizeof(T)) return false;
    if (!nxrt::mem::is_mapped(addr, sizeof(T))) return false;
    std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
    return true;
}

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

struct VoiceRuntime {
    std::atomic<uint64_t> main_base{0};
    std::atomic<bool>     enabled{true};
    std::atomic<bool>     debug{false};
    std::atomic<bool>     log_open{false};
    std::atomic<int>      mock_mode_override{0};  // 0 = auto live game state, 1 = force Exploration, 2 = force Combat
    std::atomic<int>      mock_banter_step{0};
    std::atomic<int>      mock_combat_step{0};
    std::atomic<uint64_t> toast_until_ms{0};
    std::string           toast_text;

    VoiceDatabase     db;
    VoiceDirector     director;
    VoiceRenderer     renderer;
    VoiceMemoryPoller poller;

    uint64_t last_frame_ms = 0;
};

VoiceRuntime& rt() {
    static VoiceRuntime s;
    return s;
}

void set_toast(const std::string& msg, uint64_t ms = 2200) {
    rt().toast_text = msg;
    rt().toast_until_ms.store(now_ms() + ms, std::memory_order_relaxed);
}

GameStateSnapshot read_game_state(uint64_t mb) {
    GameStateSnapshot gs;
    // Hide cleanly whenever F1 settings overlay or on-screen keyboard is up
    gs.in_menu_or_pause = nxrt::play::overlay_blocks_game();
    const int ov = rt().mock_mode_override.load(std::memory_order_relaxed);
    if (ov == 1) {
        gs.in_combat = false;
        return gs;
    }
    if (ov == 2) {
        gs.in_combat = true;
        gs.affinity_gold[0] = true;
        return gs;
    }

    if (!mb) return gs;
    uint64_t btl = 0;
    if (safe_read(mb + off301::kBattleSingleton, btl) && btl) {
        uint8_t flags = 0;
        if (safe_read(btl + 0x148, flags)) {
            gs.in_combat = (flags & 0x02u) != 0;
        }
    }
    uint64_t evt = 0;
    if (safe_read(mb + off301::kEventSingleton, evt) && evt) {
        uint32_t flags = 0;
        if (safe_read(evt + 0x78, flags)) {
            gs.in_cutscene = (flags & 0x02u) != 0;
        }
    }
    return gs;
}

void push_mock_event(const char* name) {
    VoiceTriggerEvent ev{};
    std::snprintf(ev.event_name, sizeof(ev.event_name), "%s", name);
    ev.timestamp_ms = now_ms();
    rt().director.push_event(ev);
}

}  // namespace

void init(uint64_t main_base) {
    auto& r = rt();
    r.main_base.store(main_base, std::memory_order_relaxed);
    r.enabled.store(nxrt::config_get_bool("xc2", "voice_subtitles", true), std::memory_order_relaxed);
    r.debug.store(nxrt::config_get_bool("xc2", "voice_debug", false), std::memory_order_relaxed);

    const std::string density_str = nxrt::config_get_str("xc2", "voice_density", "balanced");
    VoiceDensity density = VoiceDensity::Balanced;
    if (density_str == "minimal")   density = VoiceDensity::Minimal;
    else if (density_str == "full") density = VoiceDensity::Full;

    r.db.clear();
    // Search next to executable first, then current working directory
    const std::string lang = nxrt::config_get_str("xc2", "voice_lang", "zh_cn");
    std::vector<std::string> candidates;
    char exe_buf[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exe_buf, MAX_PATH) > 0) {
        const std::filesystem::path exe_dir = std::filesystem::path(exe_buf).parent_path();
        candidates.push_back((exe_dir / "voice_subtitles" / (lang + ".json")).string());
        candidates.push_back((exe_dir / "assets" / "voice_subtitles" / (lang + ".json")).string());
    }
    candidates.push_back("voice_subtitles\\" + lang + ".json");
    candidates.push_back("assets\\voice_subtitles\\" + lang + ".json");
    bool loaded_ext = false;
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            std::string err;
            if (r.db.load_json_file(p, &err)) {
                NXLOG_I(kTag, "loaded external voice DB '%s' (%zu speakers, %zu cues)",
                        p.c_str(), r.db.speaker_count(), r.db.cue_count());
                loaded_ext = true;
                break;
            } else {
                NXLOG_W(kTag, "failed to parse voice DB '%s': %s", p.c_str(), err.c_str());
            }
        }
    }
    r.db.seed_mock_entries();
    r.director.set_database(&r.db);
    r.director.set_density(density);
    r.director.set_debug_log_missing(r.debug.load(std::memory_order_relaxed));
    r.poller.reset();
    std::string dump_path = "voice_capture.jsonl";
    if (exe_buf[0]) {
        dump_path = (std::filesystem::path(exe_buf).parent_path() / "voice_capture.jsonl").string();
    }
    r.poller.set_debug_capture(r.debug.load(std::memory_order_relaxed), dump_path);
    r.last_frame_ms = now_ms();

    NXLOG_I(kTag, "voice overlay init (enabled=%d density=%s ext_db=%d total_cues=%zu)",
            r.enabled.load() ? 1 : 0, density_str.c_str(), loaded_ext ? 1 : 0, r.db.cue_count());
}

void shutdown() {
    rt().poller.reset();
    rt().director.clear_active();
    rt().main_base.store(0, std::memory_order_relaxed);
}

void toggle() {
    const bool next = !rt().enabled.load(std::memory_order_relaxed);
    rt().enabled.store(next, std::memory_order_relaxed);
    if (!next) rt().director.clear_active();
    set_toast(next ? "队友语音字幕：开启 (F5)" : "队友语音字幕：关闭 (F5)");
    NXLOG_I(kTag, "F5 toggle -> %s", next ? "ON" : "OFF");
}

void toggle_log() {
    const bool next = !rt().log_open.load(std::memory_order_relaxed);
    rt().log_open.store(next, std::memory_order_relaxed);
    NXLOG_I(kTag, "Ctrl+F5 Voice Log -> %s", next ? "OPEN" : "CLOSED");
}

void cycle_density() {
    auto& d = rt().director;
    const int next = (static_cast<int>(d.density()) + 1) % 3;
    d.set_density(static_cast<VoiceDensity>(next));
    static const char* const kLabels[] = {
        "语音字幕密度：精简 Minimal (仅剧情/战后 P0)",
        "语音字幕密度：平衡 Balanced (P0 + 战术 P1)",
        "语音字幕密度：完整 Full (P0 + P1 + 氛围 P2)",
    };
    set_toast(kLabels[next]);
    NXLOG_I(kTag, "Shift+F5 density -> %d", next);
}

bool enabled() {
    return rt().enabled.load(std::memory_order_relaxed);
}

void trigger_mock(int scenario) {
    auto& r = rt();
    if (!r.enabled.load(std::memory_order_relaxed)) {
        r.enabled.store(true, std::memory_order_relaxed);
    }
    if (scenario == 0) {
        // Exploration / Post-Battle P0 banter sequence
        r.mock_mode_override.store(1, std::memory_order_relaxed);
        const int step = r.mock_banter_step.fetch_add(1, std::memory_order_relaxed) % 3;
        static const char* const kBanter[] = {"mock_banter_1", "mock_banter_2", "mock_banter_3"};
        push_mock_event(kBanter[step]);
        set_toast("Mock F6: 探索/战后相声字幕 (P0)", 1600);
    } else if (scenario == 1) {
        // Combat P1 tactical + Driver/Blade merge sequence
        r.mock_mode_override.store(2, std::memory_order_relaxed);
        const int step = r.mock_combat_step.fetch_add(1, std::memory_order_relaxed) % 5;
        switch (step) {
            case 0: push_mock_event("mock_break_rex");   break;
            case 1: push_mock_event("mock_break_pyra");  break;  // Merges onto Rex's card within 2.0s!
            case 2: push_mock_event("mock_topple_nia");  break;
            case 3: push_mock_event("mock_aggro_tora");  break;  // Red warning border + pushes oldest out
            default: push_mock_event("mock_heal_byakko"); break;
        }
        set_toast("Mock F7: 战斗战术通讯条 (P1/合体)", 1600);
    }
}

void toggle_mock_combat_mode() {
    auto& r = rt();
    const int cur = r.mock_mode_override.load(std::memory_order_relaxed);
    const int next = (cur + 1) % 3;
    r.mock_mode_override.store(next, std::memory_order_relaxed);
    if (next == 0)      set_toast("语音模式：自动同步游戏状态 (F8)");
    else if (next == 1) set_toast("语音模式：Mock 探索底栏 (F8)");
    else                set_toast("语音模式：Mock 战斗战术条 (F8)");
}

void draw(const nxrt::gfx::HudCanvas& c) {
    auto& r = rt();
    const uint64_t t = now_ms();
    const float dt = (r.last_frame_ms > 0 && t >= r.last_frame_ms)
                         ? static_cast<float>(t - r.last_frame_ms) * 0.001f
                         : 0.016f;
    r.last_frame_ms = t;

    // Brief status toast (top-center)
    const uint64_t toast_end = r.toast_until_ms.load(std::memory_order_relaxed);
    if (t < toast_end && !r.toast_text.empty() && c.width > 0 && c.height > 0) {
        const float scale = std::max(0.6f, static_cast<float>(c.height) / 1080.0f);
        const float sz = 16.0f * scale;
        float tw = 0.f, th = 0.f;
        c.measure_text(sz, r.toast_text.c_str(), &tw, &th);
        const float px = (static_cast<float>(c.width) - tw) * 0.5f;
        const float py = 28.0f * scale;
        c.rect_filled(px - 12.f * scale, py - 5.f * scale, px + tw + 12.f * scale, py + th + 5.f * scale,
                      0x0C121CD0u, 4.f * scale);
        c.text_outlined(px, py, sz, 0xF8FAFCFFu, 0x000000E0u, r.toast_text.c_str());
    }

    if (!r.enabled.load(std::memory_order_relaxed)) return;

    const uint64_t mb = r.main_base.load(std::memory_order_relaxed);
    const GameStateSnapshot gs = read_game_state(mb);
    r.poller.poll(mb, t, gs.in_combat, [&](const VoiceTriggerEvent& ev) {
        r.director.push_event(ev);
    });
    r.director.update(dt, gs);
    r.renderer.render(c, r.director.render_state());
    if (r.log_open.load(std::memory_order_relaxed)) {
        r.renderer.render_voice_log(c, r.director.history());
    }
}

}  // namespace xc2::voice

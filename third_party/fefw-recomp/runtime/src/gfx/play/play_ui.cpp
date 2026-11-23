// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode settings menu: two-pane card-row UI (left option cards, right live description & status),
// bilingual Simplified Chinese / English, with XC2 Aegis Deep Ocean + Emerald / Gold styling.
//
// Two-pane card-row interaction & layout adapted from totk-pc-recomp (runtime/ui/option_rows.cpp & menu.cpp,
// MIT License, https://github.com/valentderah/totk-pc-recomp) and SavageWindRecomp (MIT License).
#include "play_ui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "nxrt/audio_sink.h"
#include "nxrt/extras.h"
#include "extras/temporal.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/input_source.h"
#include "nxrt/lowlat.h"
#include "nxrt/nvn_backend.h"
#include "nvn/nvn_mirror_cap.h"
#include "nxrt/overlay.h"
#include "nxrt/platform.h"
#include "play_game.h"

namespace nxrt::play {

namespace {

namespace hid = nxrt::hid;

// ---- XC2 Aegis palette --------------------------------------------------------------------------------------------------------------
const ImVec4 kGold(0.94f, 0.78f, 0.36f, 1.0f);
const ImVec4 kEmerald(0.28f, 0.86f, 0.76f, 1.0f);
const ImVec4 kOrange(0.95f, 0.64f, 0.28f, 1.0f);
const ImVec4 kDimText(0.60f, 0.68f, 0.76f, 1.0f);
const ImVec4 kPressed(0.18f, 0.56f, 0.46f, 1.0f);
const ImVec4 kReleased(0.14f, 0.20f, 0.28f, 1.0f);

const ImU32 kColGold = IM_COL32(240, 198, 92, 255);
const ImU32 kColGoldLine = IM_COL32(240, 198, 92, 115);
const ImU32 kColEmerald = IM_COL32(72, 220, 194, 255);
const ImU32 kColOrange = IM_COL32(242, 168, 78, 255);
const ImU32 kColWhite = IM_COL32(242, 246, 250, 255);
const ImU32 kColDim = IM_COL32(150, 170, 190, 255);
const ImU32 kColGrey = IM_COL32(105, 120, 136, 255);

// ---- Language state -----------------------------------------------------------------------------------------------------------------
bool g_zh = true;
const char* tr(const char* zh, const char* en) { return g_zh ? zh : en; }

// ---- Static option tables -----------------------------------------------------------------------------------------------------------
constexpr int kWindowModeCount = 3;
constexpr const char* kWindowModeKeys[kWindowModeCount] = {"windowed", "borderless", "exclusive"};
constexpr const char* kWindowModeZh[kWindowModeCount] = {"窗口模式", "无边框全屏", "独占全屏"};
constexpr const char* kWindowModeEn[kWindowModeCount] = {"Windowed", "Borderless", "Exclusive"};

constexpr int kSizePresets[][2] = {{0, 0}, {1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
constexpr const char* kSizeLabelsZh[] = {"自动 (显示器 / 上限 1440p)", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
constexpr const char* kSizeLabelsEn[] = {"Auto (Display / max 1440p)", "1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
constexpr int kSizePresetCount = static_cast<int>(sizeof kSizePresets / sizeof kSizePresets[0]);

constexpr int kFpsChoices[] = {-1, 0, 30, 60, 72, 90, 120, 144, 165, 240, 360};
constexpr int kFpsChoiceCount = static_cast<int>(sizeof kFpsChoices / sizeof kFpsChoices[0]);

constexpr const char* kAnisoZh[] = {"游戏默认", "2x", "4x", "8x", "16x"};
constexpr const char* kAnisoEn[] = {"Game default", "2x", "4x", "8x", "16x"};
constexpr int kAnisoValues[] = {0, 2, 4, 8, 16};
constexpr int kAnisoCount = static_cast<int>(sizeof kAnisoValues / sizeof kAnisoValues[0]);

constexpr const char* kAstcValues[] = {"fast", "high", "uncompressed"};
constexpr const char* kAstcZh[] = {"快速 (BC7)", "高质量 (后台重编码)", "无压缩 (RGBA8)"};
constexpr const char* kAstcEn[] = {"Fast (BC7)", "High (background)", "Uncompressed (RGBA8)"};
constexpr int kAstcCount = static_cast<int>(sizeof kAstcValues / sizeof kAstcValues[0]);

constexpr const char* kCornerZh[] = {"左上角", "右上角", "左下角", "右下角"};
constexpr const char* kCornerEn[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
constexpr int kCornerCount = 4;

constexpr const char* kFpsDetailZh[] = {
    "仅显示帧率",
    "帧率 + 帧时间",
    "简洁统计 (含 1% Low / 最慢帧)",
    "详细性能监控面板 (全参数 + 曲线图)",
};
constexpr const char* kFpsDetailEn[] = {
    "FPS only",
    "FPS & frame time",
    "Compact (1% low & worst)",
    "Detailed Performance HUD (+ Graph)",
};
constexpr int kFpsDetailCount = 4;

constexpr const char* kHdrModeZh[] = {"关闭 (SDR)", "自动", "强制开启", "Switch 2 原生 HDR"};
constexpr const char* kHdrModeEn[] = {"Off (SDR)", "Auto", "On", "Native HDR"};

constexpr const char* kHdrFmtZh[] = {"自动", "HDR10", "scRGB"};
constexpr const char* kHdrFmtEn[] = {"Auto", "HDR10", "scRGB"};

constexpr const char* kLatencyZh[] = {"关闭", "自动", "开启", "增强 (Boost)"};
constexpr const char* kLatencyEn[] = {"Off", "Auto", "On", "Boost"};

// The upscaler's mode (kRenderQuality in play_types.h, same order): the share of the output the 3D scene renders at, named the way the
// chosen upscaler names it - DLSS's top step is DLAA, FSR's and TAAU's is native AA, and without an upscaler the steps are plain shares.
constexpr const char* kModeDlssZh[] = {"DLAA (100%)", "质量 (67%)", "平衡 (58%)", "性能 (50%)", "超级性能 (33%)", "主机原版 (动态 720p)"};
constexpr const char* kModeDlssEn[] = {"DLAA (100%)", "Quality (67%)", "Balanced (58%)", "Performance (50%)", "Ultra Performance (33%)",
                                       "Console (dynamic 720p)"};
constexpr const char* kModeTemporalZh[] = {"原生抗锯齿 (100%)", "质量 (67%)", "平衡 (58%)", "性能 (50%)", "超级性能 (33%)", "主机原版 (动态 720p)"};
constexpr const char* kModeTemporalEn[] = {"Native AA (100%)", "Quality (67%)", "Balanced (58%)", "Performance (50%)", "Ultra Performance (33%)",
                                           "Console (dynamic 720p)"};
constexpr const char* kModeOffZh[] = {"原生 (100%)", "67%", "58%", "50%", "33%", "主机原版 (动态 720p)"};
constexpr const char* kModeOffEn[] = {"Native (100%)", "67%", "58%", "50%", "33%", "Console (dynamic 720p)"};
constexpr int kGameResCount = static_cast<int>(sizeof kModeDlssZh / sizeof kModeDlssZh[0]);
static_assert(kGameResCount == kRenderQualityCount, "one label per render quality");

constexpr const char* kGame3dUpZh[] = {
    "DLSS (NVIDIA)",
    "FSR 3.1.6 (推荐 60fps)",
    "FSR 4.1.2 (ML 高画质)",
    "FSR 2.3.4 (经典版)",
    "TAAU (内置时序)",
    "关闭 (不使用 DLSS / FSR)",
};
constexpr const char* kGame3dUpEn[] = {
    "DLSS (NVIDIA)",
    "FSR 3.1.6 (Rec. 60fps)",
    "FSR 4.1.2 (ML High)",
    "FSR 2.3.4 (Classic)",
    "TAAU (Built-in)",
    "Off (no DLSS / FSR)",
};
constexpr int kGame3dUpCount = static_cast<int>(sizeof kGame3dUpZh / sizeof kGame3dUpZh[0]);

// DLSS models offered in F1. L / M stay ini-only (Default picks them where they apply; on DLAA they drew black lines).
constexpr const char* kDlssPresetKeys[] = {"default", "K", "J"};
constexpr const char* kDlssPresetZh[] = {"默认 (由驱动按模式选择)", "模型 K (Transformer，推荐)", "模型 J (Transformer，拖影更少)"};
constexpr const char* kDlssPresetEn[] = {"Default (driver picks per mode)", "Model K (transformer, recommended)", "Model J (transformer, less ghosting)"};
constexpr int kDlssPresetCount = static_cast<int>(sizeof kDlssPresetKeys / sizeof kDlssPresetKeys[0]);

int dlss_preset_index(const std::string& k) {
    for (int i = 0; i < kDlssPresetCount; ++i)
        if (k == kDlssPresetKeys[i]) return i;
    return 0;
}

constexpr const char* kSpatialUpZh[] = {"关闭", "FSR 1 (空间锐化)", "NIS (空间锐化)"};
constexpr const char* kSpatialUpEn[] = {"Off", "FSR 1 (spatial)", "NIS (spatial)"};
constexpr extras::UpscaleMode kSpatialUpModes[] = {
    extras::UpscaleMode::Off,
    extras::UpscaleMode::Fsr1,
    extras::UpscaleMode::Nis,
};
constexpr int kSpatialUpCount = static_cast<int>(sizeof kSpatialUpModes / sizeof kSpatialUpModes[0]);

int game3d_up_index(const Game3dSettings& g) {
    if (g.upscaler == "dlss") return 0;
    if (g.upscaler == "taau") return 4;
    if (g.upscaler == "bilinear") return 5;
    if (!g.fsr_version.empty() && g.fsr_version[0] == '4') return 2;
    if (!g.fsr_version.empty() && g.fsr_version[0] == '2') return 3;
    return 1;
}

void set_game3d_up_index(Game3dSettings& g, int idx) {
    switch (idx) {
        case 0: g.upscaler = "dlss"; break;
        case 1: g.upscaler = "fsr3"; g.fsr_version = "3.1"; break;
        case 2: g.upscaler = "fsr3"; g.fsr_version = "4.1"; break;
        case 3: g.upscaler = "fsr3"; g.fsr_version = "2.3"; break;
        case 4: g.upscaler = "taau"; break;
        default: g.upscaler = "bilinear"; break;
    }
}

struct SwitchButton {
    const char* name;
    hid::NpadButtonBit bit;
};
constexpr SwitchButton kSwitchButtons[] = {
    {"A", hid::kBtnA},         {"B", hid::kBtnB},         {"X", hid::kBtnX},         {"Y", hid::kBtnY},
    {"L", hid::kBtnL},         {"R", hid::kBtnR},         {"ZL", hid::kBtnZL},       {"ZR", hid::kBtnZR},
    {"PLUS", hid::kBtnPlus},   {"MINUS", hid::kBtnMinus}, {"UP", hid::kBtnUp},       {"DOWN", hid::kBtnDown},
    {"LEFT", hid::kBtnLeft},   {"RIGHT", hid::kBtnRight}, {"LSTICK", hid::kBtnStickL}, {"RSTICK", hid::kBtnStickR},
};
constexpr int kSwitchButtonCount = static_cast<int>(sizeof kSwitchButtons / sizeof kSwitchButtons[0]);

// ---- Working state ------------------------------------------------------------------------------------------------------------------
struct Page {
    bool loaded = false;
    bool dirty = false;
    Settings edit;
    Ctx ctx = Ctx::Play;
    int rebind = -1;
    bool link_pitch = true;
    bool out_tab_open = false;
    char out_text[kActCount][128] = {};
    std::string out_err[kActCount];
    bool fps_capture = false;
    std::string notice;
    bool notice_warn = false;
    std::vector<std::string> monitors;
    double monitors_at = -1e9;
    std::vector<std::string> audio_devices;
    bool audio_loaded = false;
    int active_tab = 0;   // 0 Display, 1 Graphics, 2 Audio, 3 Controls, 4 Overlay
};

void mark(Page& p) { p.dirty = true; }
const char* cstr(const char* s) { return s ? s : ""; }
const char* act_label(Act a) { return a == Act::None ? tr("无 (禁用)", "none") : act_info(a).name; }
float px(float v) { return v * ImGui::GetFontSize() / 18.0f; }

void stop_rebind(UiHost& host, Page& p) {
    if (p.rebind >= 0 && host.capturing()) host.cancel_capture();
    p.rebind = -1;
}

std::vector<int> conflicts_of(const Page& p, int row) {
    std::vector<int> out;
    const Binding& a = p.edit.bindings[static_cast<std::size_t>(row)];
    if (a.in.kind == InKind::None) return out;
    for (std::size_t i = 0; i < p.edit.bindings.size(); ++i) {
        const Binding& b = p.edit.bindings[i];
        if (static_cast<int>(i) != row && b.ctx == a.ctx && b.in == a.in && b.trig == a.trig) out.push_back(static_cast<int>(i));
    }
    return out;
}

void set_notice(Page& p, const std::string& text, bool warn) {
    p.notice = text;
    p.notice_warn = warn;
}

void refresh_out_text(Page& p) {
    for (int a = 1; a < kActCount; ++a) {
        const std::string text = output_text(p.edit.out[a]);
        std::snprintf(p.out_text[a], sizeof p.out_text[a], "%s", text.c_str());
        p.out_err[a].clear();
    }
}

void sanitize(Settings& s) {
    std::vector<Binding>& v = s.bindings;
    v.erase(std::remove_if(v.begin(), v.end(), [](const Binding& b) { return b.in.kind == InKind::None; }), v.end());
    for (Binding& b : v) b.ms = std::clamp(b.ms, 0, 5000);
    s.display.width = s.display.width <= 0 ? 0 : std::clamp(s.display.width, 320, 16384);
    s.display.height = s.display.height <= 0 ? 0 : std::clamp(s.display.height, 320, 16384);
    s.display.render_width = s.display.render_width <= 0 ? 0 : std::clamp(s.display.render_width, 320, 16384);
    s.display.render_height = s.display.render_height <= 0 ? 0 : std::clamp(s.display.render_height, 320, 16384);
    extras::HdrSettings& h = s.extras.hdr;
    h.peak_nits = h.peak_nits <= 0 ? 0 : std::clamp(h.peak_nits, 200.0, 10000.0);
    h.paper_white_nits = h.paper_white_nits <= 0 ? 0 : std::clamp(h.paper_white_nits, 80.0, 500.0);
    h.highlight_boost = std::clamp(h.highlight_boost, 0.0, 2.0);
    s.extras.upscale.sharpness = std::clamp(s.extras.upscale.sharpness, 0.0, 1.0);
    s.extras.picture = extras::picture_clamped(s.extras.picture);
    s.audio.volume = std::clamp(s.audio.volume, 0.0, 1.0);
    s.display.monitor = std::clamp(s.display.monitor, 0, 32);
    s.game3d.res_preset = std::clamp(s.game3d.res_preset, 0, kGameResCount - 1);
    s.game3d.sharpness = std::clamp(s.game3d.sharpness, 0.0, 1.0);
    if (s.audio.device.empty()) s.audio.device = "default";
}

void apply_now(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit.fps = host.settings().fps;
    sanitize(p.edit);
    host.apply(p.edit);
    p.dirty = false;
    set_notice(p, tr("设置已应用并保存", "Settings applied and saved"), false);
}

void revert_to_saved(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.settings();
    p.edit.fps = host.settings().fps;
    extras::set_picture_live(p.edit.extras.picture);
    refresh_out_text(p);
    p.dirty = false;
    set_notice(p, tr("已放弃未保存的修改", "Changes discarded"), false);
}

void load_factory(UiHost& host, Page& p) {
    stop_rebind(host, p);
    p.edit = host.factory();
    p.edit.fps = host.settings().fps;
    extras::set_picture_live(p.edit.extras.picture);
    refresh_out_text(p);
    mark(p);
    set_notice(p, tr("已载入默认配置，点击「应用并保存」生效", "Factory defaults loaded; click Apply & Save"), false);
}

void set_fps_live(UiHost& host, const FpsSettings& f) {
    Settings cur = host.settings();
    cur.fps = f;
    host.apply(cur);
}

void poll_capture(UiHost& host, Page& p) {
    InputId id;
    if (host.capture_result(&id)) {
        const std::string name = input_name(id);
        const bool reserved = host.reserved_key(id);
        if (p.fps_capture) {
            if (reserved) {
                set_notice(p, name + tr(" 是菜单快捷键，无法绑定为帧率键（按 Esc 取消）", " opens the menu; press another key or Esc"), true);
                host.begin_capture();
                return;
            }
            p.fps_capture = false;
            if (id.kind == InKind::Key) {
                Settings cur = host.settings();
                cur.fps.key = id;
                host.apply(cur);
                set_notice(p, std::string(tr("帧率快捷键已改为 ", "FPS hotkey set to ")) + name, false);
            }
        } else if (p.rebind >= 0 && p.rebind < static_cast<int>(p.edit.bindings.size())) {
            const Settings cur = host.settings();
            if (reserved || (id.kind == InKind::Key && id == cur.fps.key)) {
                set_notice(p, name + tr(" 为系统保留键，请按其他键（Esc 取消）", " is reserved; press another key or Esc"), true);
                host.begin_capture();
                return;
            }
            p.edit.bindings[static_cast<std::size_t>(p.rebind)].in = id;
            mark(p);
            const std::vector<int> other = conflicts_of(p, p.rebind);
            if (other.empty()) {
                set_notice(p, std::string(tr("已绑定 ", "Bound ")) + name, false);
            } else {
                const Binding& o = p.edit.bindings[static_cast<std::size_t>(other[0])];
                set_notice(p, name + tr(" 与以下操作冲突：", " conflicts with: ") + act_label(o.act), true);
            }
        }
        p.rebind = -1;
    } else if ((p.rebind >= 0 || p.fps_capture) && !host.capturing()) {
        p.rebind = -1;
        p.fps_capture = false;
        set_notice(p, tr("已取消按键捕获", "Capture cancelled"), false);
    }
}

void refresh_monitors(Page& p) {
    const double now = ImGui::GetTime();
    if (p.monitors_at >= 0.0 && now - p.monitors_at < 2.0) return;
    p.monitors_at = now;
    p.monitors.clear();
    const int n = platform::display_count();
    for (int i = 1; i <= n; ++i) {
        char buf[192];
        if (platform::display_name(i, buf, sizeof buf)) p.monitors.emplace_back(buf);
    }
}

void load_audio_devices(Page& p) {
    p.audio_devices.clear();
    for (const audio::OutputDevice& d : audio::list_output_devices())
        if (!d.name.empty()) p.audio_devices.push_back(d.name);
    p.audio_loaded = true;
}

int window_mode_index(const std::string& mode) {
    for (int i = 0; i < kWindowModeCount; ++i)
        if (mode == kWindowModeKeys[i]) return i;
    return 0;
}

void fps_label_text(int fps, char* buf, std::size_t n) {
    if (fps < 0) std::snprintf(buf, n, "%s", tr("跟随显示器刷新率", "Refresh rate"));
    else if (fps == 0) std::snprintf(buf, n, "%s", tr("无限制", "Unlimited"));
    else std::snprintf(buf, n, "%d fps", fps);
}

// ---- Two-pane card-row engine (adapted from totk-pc-recomp option_rows.cpp, MIT) ----------------------------------------------------
struct DescInfo {
    std::string title;
    std::string body;
    bool restart = false;
    bool live = false;
    bool valid = false;
};

struct PaneState {
    DescInfo hover;
    DescInfo focus;
    DescInfo first;
    DescInfo shown;
    float bottom_reserve = 0.0f;
    bool held = false;
    bool focus_first = false;
};

PaneState g_pane;

void record_desc(const char* title, const char* body, bool restart, bool live, bool hov, bool foc) {
    DescInfo d;
    d.title = title ? title : "";
    d.body = body ? body : "";
    d.restart = restart;
    d.live = live;
    d.valid = true;
    if (hov) g_pane.hover = d;
    if (foc) g_pane.focus = d;
    if (!g_pane.first.valid) g_pane.first = d;
}

void pane_begin(const char* id, float bottom_reserve) {
    g_pane.bottom_reserve = bottom_reserve;
    g_pane.hover.valid = false;
    g_pane.focus.valid = false;
    g_pane.first.valid = false;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y = std::max(px(120.0f), avail.y - bottom_reserve);
    ImGui::PushID(id);
    ImGui::BeginChild("##rows_pane", ImVec2(avail.x * 0.59f, avail.y), ImGuiChildFlags_NavFlattened);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, px(7.0f)));
}

void pane_end(const LiveStatus& live) {
    ImGui::PopStyleVar();
    ImGui::EndChild();

    if (g_pane.hover.valid) g_pane.shown = g_pane.hover;
    else if (g_pane.focus.valid) g_pane.shown = g_pane.focus;
    else if (!g_pane.shown.valid && g_pane.first.valid) g_pane.shown = g_pane.first;

    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y = std::max(px(120.0f), avail.y - g_pane.bottom_reserve);
    const float gap = px(22.0f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + gap * 0.5f, p.y + px(4.0f)),
                                        ImVec2(p.x + gap * 0.5f, p.y + avail.y - px(4.0f)), kColGoldLine, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(p.x + gap, p.y));
    ImGui::BeginChild("##desc_pane", ImVec2(0.0f, avail.y), 0, ImGuiWindowFlags_NoNav);

    // Top: Option explanation card
    const float status_h = px(184.0f);
    const float desc_h = std::max(px(100.0f), avail.y - status_h - px(12.0f));
    {
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        const float cw = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(cp, ImVec2(cp.x + cw, cp.y + desc_h), IM_COL32(8, 16, 28, 150), px(10.0f));
        dl->AddRect(cp, ImVec2(cp.x + cw, cp.y + desc_h), IM_COL32(60, 125, 145, 95), px(10.0f), 0, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(14.0f), px(12.0f)));
        ImGui::BeginChild("##desc_inner", ImVec2(cw, desc_h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
        ImGui::PopStyleVar();
        if (g_pane.shown.valid) {
            ImGui::PushStyleColor(ImGuiCol_Text, kGold);
            ImGui::TextWrapped("%s", g_pane.shown.title.c_str());
            ImGui::PopStyleColor();
            if (g_pane.shown.restart) {
                ImGui::PushStyleColor(ImGuiCol_Text, kOrange);
                ImGui::TextUnformatted(tr("● 需重启游戏后生效", "● Applies after restart"));
                ImGui::PopStyleColor();
            } else if (g_pane.shown.live) {
                ImGui::PushStyleColor(ImGuiCol_Text, kEmerald);
                ImGui::TextUnformatted(tr("● 修改后实时生效", "● Applies live"));
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(g_pane.shown.body.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
    }

    ImGui::Dummy(ImVec2(0.0f, px(6.0f)));

    // Bottom: Live status card
    {
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        const float cw = ImGui::GetContentRegionAvail().x;
        const float ch = std::max(px(110.0f), ImGui::GetContentRegionAvail().y);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(cp, ImVec2(cp.x + cw, cp.y + ch), IM_COL32(8, 16, 28, 165), px(10.0f));
        dl->AddRect(cp, ImVec2(cp.x + cw, cp.y + ch), IM_COL32(72, 220, 194, 80), px(10.0f), 0, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(14.0f), px(10.0f)));
        ImGui::BeginChild("##status_inner", ImVec2(cw, ch), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
        ImGui::PopStyleVar();

        ImGui::PushStyleColor(ImGuiCol_Text, kEmerald);
        ImGui::TextUnformatted(tr("实时运行状态", "Live Status"));
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Text("%s: %d x %d  (%.1f fps)", tr("画面", "Output"), live.width, live.height, live.fps);
        ImGui::Text("%s: %.1f Hz  |  %s", tr("显示器", "Display"), live.refresh_hz, cstr(live.present_mode));
        const extras::HdrState hs = extras::hdr_state();
        ImGui::Text("HDR: %s  (%s)", hs.scene_hdr ? tr("开", "On") : tr("关", "Off"), extras::output_kind_name(hs.output));
        const nvn::DlssLiveInfo dli = nvn::dlss_live_info();
        const char* up_txt = !dli.active_name.empty() ? dli.active_name.c_str()
                           : (!dli.upscaler.empty() ? dli.upscaler.c_str() : "-");
        ImGui::Text("%s: %s", tr("3D 超分", "3D Upscaler"), up_txt);
        const nvn::VramLiveInfo vli = nvn::vram_live_info();
        if (vli.vram_total_mb > 0) {
            if (vli.budget_live) {
                ImGui::Text("VRAM: %llu / %llu MiB  (%s %llu / %llu MiB%s)",
                            static_cast<unsigned long long>(vli.vram_used_mb),
                            static_cast<unsigned long long>(vli.vram_total_mb),
                            tr("拷贝池", "Copies"),
                            static_cast<unsigned long long>(vli.pool_copy_mb),
                            static_cast<unsigned long long>(vli.pool_cap_mb),
                            vli.capture_on ? ", capture" : "");
            } else {
                ImGui::Text("VRAM: %llu MiB  (%s %llu / %llu MiB%s)",
                            static_cast<unsigned long long>(vli.vram_total_mb),
                            tr("拷贝池", "Copies"),
                            static_cast<unsigned long long>(vli.pool_copy_mb),
                            static_cast<unsigned long long>(vli.pool_cap_mb),
                            vli.capture_on ? ", capture" : "");
            }
        }
        const int backlog = gfx::shader_backlog();
        if (backlog > 0) ImGui::TextColored(kOrange, "%s: %d", tr("后台着色器编译剩余", "Shaders left"), backlog);
        else ImGui::TextColored(kDimText, "%s", tr("着色器缓存已就绪", "Shader cache ready"));
        if (live.pads > 0) ImGui::TextColored(kDimText, "%s: %s", tr("手柄", "Pad"), live.pad_name.c_str());
        else ImGui::TextColored(kDimText, "%s", tr("手柄: 未连接 (键鼠模式)", "Pad: None (KB/Mouse)"));

        ImGui::EndChild();
    }

    ImGui::EndChild();
    ImGui::PopID();
}

void card_group(const char* zh, const char* en) {
    ImGui::Spacing();
    const char* title = tr(zh, en);
    ImGui::PushStyleColor(ImGuiCol_Text, kGold);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - px(2.0f)), ImVec2(p.x + w, p.y - px(2.0f)),
                                        IM_COL32(240, 198, 92, 90), 1.0f);
    ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
}

// Core interactive option card. Returns: -1 step down, +1 step up, 2 clicked/A, 0 none.
int draw_card_raw(const char* id, const char* label, const char* value, const char* desc,
                  bool arrows, bool restart, bool live, bool accent_val, bool disabled, float progress = -1.0f) {
    const float fs = ImGui::GetFontSize();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = (restart || live) ? fs * 2.45f : fs * 2.15f;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##card", ImVec2(w, h));
    const ImGuiID item_id = ImGui::GetItemID();
    const bool hov = ImGui::IsItemHovered();
    bool foc = ImGui::IsItemFocused();
    ImGui::PopID();

    if (g_pane.focus_first && !disabled) {
        ImGui::SetFocusID(item_id, ImGui::GetCurrentWindow());
        if (ImGuiContext* ctx = ImGui::GetCurrentContext()) ctx->NavCursorVisible = true;
        g_pane.focus_first = false;
        foc = true;
    } else if (clicked && !disabled) {
        ImGui::SetFocusID(item_id, ImGui::GetCurrentWindow());
        foc = true;
    }

    int step = (clicked && !disabled) ? 2 : 0;
    g_pane.held = false;
    if (foc && !disabled) {
        static const ImGuiKey kStepKeys[] = {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight};
        for (ImGuiKey k : kStepKeys) {
            ImGui::SetKeyOwner(k, item_id, ImGuiInputFlags_LockThisFrame);
            if (ImGui::GetKeyData(k)->DownDuration > 0.7f) g_pane.held = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, ImGuiInputFlags_Repeat, item_id) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, ImGuiInputFlags_Repeat, item_id))
            step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, ImGuiInputFlags_Repeat, item_id) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, ImGuiInputFlags_Repeat, item_id))
            step = 1;
    }

    record_desc(label, desc, restart, live, hov, foc);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float r = px(9.0f);
    const float pad = px(15.0f);
    const bool hi = hov || foc;

    const ImU32 bg = disabled ? IM_COL32(10, 16, 26, 95)
                              : hi ? IM_COL32(28, 78, 92, 130) : IM_COL32(10, 20, 34, 155);
    const ImU32 brd = disabled ? IM_COL32(80, 95, 110, 45)
                               : hi ? kColGold : IM_COL32(72, 145, 168, 70);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, r);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), brd, r, 0, hi ? 1.8f : 1.0f);

    // Optional progress bar along bottom inside card (for sliders)
    if (progress >= 0.0f) {
        const float t = std::clamp(progress, 0.0f, 1.0f);
        const float by = p.y + h - px(4.5f);
        const float bx0 = p.x + pad;
        const float bx1 = p.x + w - pad;
        dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + px(2.5f)), IM_COL32(255, 255, 255, 28), px(2.0f));
        if (t > 0.001f)
            dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx0 + (bx1 - bx0) * t, by + px(2.5f)),
                              hi ? kColGold : kColEmerald, px(2.0f));
    }

    const float ty = p.y + ((restart || live) ? fs * 0.28f : (h - fs) * 0.5f);
    const float ny = p.y + fs * 1.42f;
    float right = p.x + w - pad;

    if (arrows && value && value[0]) {
        const float aw = fs * 0.36f;
        const float agap = fs * 0.45f;
        const float cy = ty + fs * 0.50f;
        const float val_w = ImGui::CalcTextSize(value).x;
        const float rx = right - aw;
        const float lx = rx - agap - val_w - agap - aw;
        const ImU32 ac = disabled ? kColGrey : hi ? kColGold : IM_COL32(150, 185, 200, 220);
        dl->AddTriangleFilled(ImVec2(lx, cy), ImVec2(lx + aw, cy - aw * 0.8f), ImVec2(lx + aw, cy + aw * 0.8f), ac);
        dl->AddTriangleFilled(ImVec2(rx + aw, cy), ImVec2(rx, cy - aw * 0.8f), ImVec2(rx, cy + aw * 0.8f), ac);
        if (clicked && !disabled) {
            const float mx = ImGui::GetIO().MousePos.x;
            if (mx >= lx - agap * 1.5f && mx <= lx + aw + agap) step = -1;
            else if (mx >= rx - agap && mx <= rx + aw + agap * 1.5f) step = 1;
        }
        right = rx - agap;
    }

    if (value && value[0]) {
        const float vw = ImGui::CalcTextSize(value).x;
        const ImU32 vc = disabled ? kColGrey : accent_val ? kColEmerald : kColWhite;
        dl->AddText(ImVec2(right - vw, ty), vc, value);
    }

    if (restart || live) {
        const float small = fs * 0.72f;
        const char* tag = restart ? tr("重启生效", "Restart") : tr("实时生效", "Live");
        const float tw = font->CalcTextSizeA(small, FLT_MAX, 0.0f, tag).x;
        dl->AddText(font, small, ImVec2(p.x + w - pad - tw, ny), restart ? kColOrange : kColDim, tag);
    }

    // Label on the left
    const ImU32 lc = disabled ? kColGrey : kColWhite;
    dl->AddText(ImVec2(p.x + pad, ty), lc, label);
    return step;
}

bool card_toggle(const char* id, const char* lzh, const char* len, const char* dzh, const char* den,
                 bool* val, bool restart = false, bool live = false) {
    const char* vtext = *val ? tr("开启", "On") : tr("关闭", "Off");
    const int s = draw_card_raw(id, tr(lzh, len), vtext, tr(dzh, den), true, restart, live, *val, false);
    if (s != 0) {
        *val = !(*val);
        return true;
    }
    return false;
}

bool card_choice(const char* id, const char* lzh, const char* len, const char* dzh, const char* den,
                 int* idx, const char* const* zh_names, const char* const* en_names, int count,
                 bool restart = false, bool live = false) {
    if (count <= 0) return false;
    *idx = std::clamp(*idx, 0, count - 1);
    const char* cur = g_zh ? zh_names[*idx] : en_names[*idx];
    const int s = draw_card_raw(id, tr(lzh, len), cur, tr(dzh, den), true, restart, live, false, false);
    if (s == -1) {
        *idx = (*idx + count - 1) % count;
        return true;
    }
    if (s == 1 || s == 2) {
        *idx = (*idx + 1) % count;
        return true;
    }
    return false;
}

bool card_choice_vec(const char* id, const char* lzh, const char* len, const char* dzh, const char* den,
                     int* idx, const std::vector<std::string>& items, bool restart = false) {
    const int count = static_cast<int>(items.size());
    if (count <= 0) return false;
    *idx = std::clamp(*idx, 0, count - 1);
    std::string disp = items[static_cast<std::size_t>(*idx)];
    if (disp.size() > 26) disp = disp.substr(0, 23) + "...";
    const int s = draw_card_raw(id, tr(lzh, len), disp.c_str(), tr(dzh, den), count > 1, restart, false, false, false);
    if (count > 1 && s == -1) {
        *idx = (*idx + count - 1) % count;
        return true;
    }
    if (count > 1 && (s == 1 || s == 2)) {
        *idx = (*idx + 1) % count;
        return true;
    }
    return false;
}

bool card_slider_f(const char* id, const char* lzh, const char* len, const char* dzh, const char* den,
                   double* val, double lo, double hi, double step, const char* fmt,
                   bool restart = false, bool live = false) {
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt, *val);
    const float prog = (hi > lo) ? static_cast<float>((*val - lo) / (hi - lo)) : 0.0f;
    const int s = draw_card_raw(id, tr(lzh, len), buf, tr(dzh, den), true, restart, live, false, false, prog);
    if (s == -1 || s == 1 || s == 2) {
        const double mult = g_pane.held ? 5.0 : 1.0;
        const double delta = (s == -1 ? -1.0 : 1.0) * step * mult;
        double nv = *val + delta;
        if (s == 2 && nv > hi + 1e-6) nv = lo;
        nv = std::clamp(nv, lo, hi);
        if (std::fabs(nv - *val) > 1e-7) {
            *val = nv;
            return true;
        }
    }
    return false;
}

bool card_slider_i(const char* id, const char* lzh, const char* len, const char* dzh, const char* den,
                   int* val, int lo, int hi, int step, const char* fmt,
                   bool restart = false, bool live = false, const char* special_zero = nullptr, int min_nonzero = 0) {
    char buf[64];
    if (special_zero && *val <= 0) std::snprintf(buf, sizeof buf, "%s", special_zero);
    else std::snprintf(buf, sizeof buf, fmt, *val);
    const float prog = (hi > lo) ? static_cast<float>(std::max(0, *val - lo)) / static_cast<float>(hi - lo) : 0.0f;
    const int s = draw_card_raw(id, tr(lzh, len), buf, tr(dzh, den), true, restart, live, false, false, prog);
    if (s == -1 || s == 1 || s == 2) {
        const int mult = g_pane.held ? 5 : 1;
        int nv = *val + (s == -1 ? -1 : 1) * step * mult;
        if (special_zero && min_nonzero > 0) {
            if (*val <= 0 && (s == 1 || s == 2)) nv = min_nonzero;
            else if (*val > 0 && nv < min_nonzero) nv = 0;
        }
        if (s == 2 && nv > hi) nv = lo;
        nv = std::clamp(nv, lo, hi);
        if (nv != *val) {
            *val = nv;
            return true;
        }
    }
    return false;
}

bool card_button(const char* id, const char* lzh, const char* len, const char* dzh, const char* den,
                 const char* right_text = "", bool disabled = false) {
    const int s = draw_card_raw(id, tr(lzh, len), right_text, tr(dzh, den), false, false, true, true, disabled);
    return s == 2 && !disabled;
}

// ---- Tab 0: Display -----------------------------------------------------------------------------------------------------------------
void tab_display(const LiveStatus& live, Page& p, float bottom_h) {
    DisplaySettings& d = p.edit.display;
    extras::LatencySettings& l = p.edit.extras.latency;

    pane_begin("tab_display", bottom_h);

    card_group("窗口与输出", "Window & Output");
    {   // the output resolution: what the game renders its final picture at (and is told its TV is, [am] resolution); the upscaler
        // modes on the Graphics page are shares of it. Not the window: a 4K output in a smaller window is scaled down to it.
        static constexpr int kOutH[] = {1080, 1440, 2160};
        static constexpr const char* kOutZh[] = {"1920x1080 (1080p)", "2560x1440 (1440p)", "3840x2160 (4K)"};
        static constexpr const char* kOutEn[] = {"1920x1080 (1080p)", "2560x1440 (1440p)", "3840x2160 (4K)"};
        int oi = 0;
        for (int i = 0; i < 3; ++i)
            if (std::abs(kOutH[i] - p.edit.game3d.out_height) < std::abs(kOutH[oi] - p.edit.game3d.out_height)) oi = i;
        if (card_choice("out_res", "输出分辨率", "Output resolution",
                        "游戏最终画面的分辨率（相当于主机所接电视的分辨率）。「画质」页的超分模式按它的百分比渲染 3D 场景。通常设为显示器的分辨率；它与窗口大小无关，窗口更小时画面会缩小显示。重启后生效。",
                        "The resolution the game renders its final picture at (the TV resolution the console would be connected to). The "
                        "upscaler modes on the Graphics page render the 3D scene at a share of it. Usually your monitor's resolution; it is "
                        "not the window size - a smaller window shows it scaled down. Takes effect after a restart.",
                        &oi, kOutZh, kOutEn, 3, true)) {
            p.edit.game3d.out_height = kOutH[oi];
            mark(p);
        }
    }

    int wmode = window_mode_index(d.window_mode);
    if (card_choice("wmode", "显示模式", "Window mode",
                    "无边框全屏可瞬间切换且支持 F11 / Alt+Enter 快捷切换；独占全屏会直接接管显示器。",
                    "Borderless fullscreen switches instantly (F11 / Alt+Enter). Exclusive takes over the display.",
                    &wmode, kWindowModeZh, kWindowModeEn, kWindowModeCount)) {
        d.window_mode = kWindowModeKeys[wmode];
        mark(p);
    }

    refresh_monitors(p);
    {
        std::vector<std::string> mons;
        mons.emplace_back(tr("自动 (当前窗口所在显示器)", "Auto (current display)"));
        for (const std::string& m : p.monitors) mons.push_back(m);
        int mon = std::clamp(d.monitor, 0, static_cast<int>(mons.size()) - 1);
        if (card_choice_vec("mon", "目标显示器", "Monitor",
                            "选择窗口和全屏模式显示的物理显示器。",
                            "Choose which physical monitor to display the game on.",
                            &mon, mons)) {
            d.monitor = mon;
            mark(p);
        }
    }

    int preset = 0;   // default: Auto (Display / max 1440p)
    for (int i = 0; i < kSizePresetCount; ++i)
        if (kSizePresets[i][0] == d.width && kSizePresets[i][1] == d.height) preset = i;
    if (card_choice("wsize", "窗口大小 (仅窗口模式)", "Window size (windowed mode only)",
                    "窗口模式下窗口在桌面上的大小（重启后会记住）；自动模式跟随显示器分辨率（上限 1440p）。全屏时不起作用；不改变游戏的输出分辨率，只改变画面显示的大小。",
                    "How large the window is on the desktop in windowed mode (remembered on restart); Auto follows display (capped at 1440p). No effect in fullscreen; it does not change the game's output resolution, only how large the picture is shown.",
                    &preset, kSizeLabelsZh, kSizeLabelsEn, kSizePresetCount)) {
        d.width = kSizePresets[preset][0];
        d.height = kSizePresets[preset][1];
        mark(p);
    }

    if (live.width > 0 && (live.width != d.width || live.height != d.height)) {
        char cur_sz[48];
        std::snprintf(cur_sz, sizeof cur_sz, "%d x %d", live.width, live.height);
        if (card_button("use_cur_sz", "使用当前窗口尺寸", "Use current window size",
                        "把当前实际窗口宽高直接填入上面的窗口尺寸设置。",
                        "Copy the current window dimensions into the setting above.", cur_sz)) {
            d.width = live.width;
            d.height = live.height;
            mark(p);
        }
    }

    if (card_toggle("keep_ar", "保持 16:9 画面比例", "Keep 16:9 aspect ratio",
                    "在非 16:9 屏幕上两侧/上下留黑边，避免画面拉伸变形。点击应用后实时生效。",
                    "Letterbox / pillarbox on non-16:9 displays instead of stretching. Applies live.", &d.keep_aspect)) {
        mark(p);
    }

    card_group("帧率与同步", "Frame Rate & Sync");
    if (card_toggle("vsync", "垂直同步 (VSync)", "VSync",
                    "开启（推荐）：使用 FIFO 呈现模式，消除画面撕裂并防止 AMD 显卡交换链竞态。关闭：使用 Immediate 模式以获得最低延迟。",
                    "On (recommended on AMD): FIFO presentation prevents tearing and swapchain races. Off: lowest latency.",
                    &d.vsync)) {
        mark(p);
    }

    {
        int fidx = 0;
        for (int i = 0; i < kFpsChoiceCount; ++i)
            if (kFpsChoices[i] == d.fps_cap) fidx = i;
        std::vector<std::string> flabels;
        flabels.reserve(kFpsChoiceCount);
        for (int i = 0; i < kFpsChoiceCount; ++i) {
            char b[48];
            fps_label_text(kFpsChoices[i], b, sizeof b);
            flabels.emplace_back(b);
        }
        if (card_choice_vec("fps_cap", "帧率上限", "Frame limit",
                            "限制游戏运行帧率。注：在 60Hz 启动下最高封顶 60 fps（保证游戏速度正常），向下限帧（如 30 fps）实时生效。",
                            "Caps frame rate. Note: capped at launch rate (60 fps) to preserve real-time game speed; lower caps apply live.",
                            &fidx, flabels)) {
            d.fps_cap = kFpsChoices[fidx];
            mark(p);
        }
    }

    if (live.refresh_hz > 0.0f) {
        char vrr[48];
        const int cap = static_cast<int>(std::floor(nxrt::suggest_vrr_cap(live.refresh_hz)));
        std::snprintf(vrr, sizeof vrr, "%d fps", cap);
        if (card_button("vrr_cap", "一键设为 VRR 最佳上限", "Set VRR-friendly cap",
                        "根据当前显示器刷新率自动设为略低于刷新率的帧数上限，配合 FreeSync / G-Sync 减少输入延迟。",
                        "Automatically sets the cap slightly below display refresh rate for FreeSync / G-Sync.", vrr)) {
            d.fps_cap = cap;
            mark(p);
        }
    }

    card_group("低延迟", "Low Latency");
    int lmode = static_cast<int>(l.mode);
    if (card_choice("lat_mode", "低延迟模式 (Anti-Lag 2 / Reflex)", "Low latency mode",
                    "启用 AMD Anti-Lag 2 或 NVIDIA Reflex 降低从手柄/键鼠输入到屏幕显示的延迟。",
                    "Reduces input-to-display latency via AMD Anti-Lag 2 or NVIDIA Reflex.",
                    &lmode, kLatencyZh, kLatencyEn, 4, true)) {
        l.mode = static_cast<extras::LatencyMode>(lmode);
        mark(p);
    }
    if (card_slider_i("lat_ahead", "预渲染帧数上限", "Frames ahead",
                      "低延迟开启时允许提前排队的帧数。-1 为不限制，1 兼顾流畅与延迟，0 延迟最低但可能略降帧率。",
                      "Maximum frames queued ahead when low latency is active (-1 = unlimited).",
                      &l.frames_ahead, -1, 3, 1, "%d", true, false, tr("不限制", "No limit"))) {
        mark(p);
    }

    pane_end(live);
}

// ---- Tab 1: Graphics ----------------------------------------------------------------------------------------------------------------
void tab_graphics(const LiveStatus& live, Page& p, float bottom_h) {
    DisplaySettings& d = p.edit.display;
    Game3dSettings& g3d = p.edit.game3d;
    extras::PictureSettings& pic = p.edit.extras.picture;
    extras::HdrSettings& hdr = p.edit.extras.hdr;
    extras::UpscaleSettings& up = p.edit.extras.upscale;

    pane_begin("tab_graphics", bottom_h);

    card_group("超分与抗锯齿", "Upscaling & Anti-Aliasing");
    {
        int gidx = game3d_up_index(g3d);
        if (card_choice("dlss_up", "超分器", "Upscaler",
                        "3D 画面使用的超分 / 抗锯齿技术，点「应用并保存」后实时切换并写入 nxrt.ini。「关闭」不使用 DLSS 或 FSR。",
                        "The upscaling / anti-aliasing the 3D picture uses. Switches live when applied and is saved to nxrt.ini. Off uses "
                        "neither DLSS nor FSR.",
                        &gidx, kGame3dUpZh, kGame3dUpEn, kGame3dUpCount, false, true)) {
            set_game3d_up_index(g3d, gidx);
            mark(p);
        }
        // The mode: the share of the output the 3D scene renders at, named after the upscaler (DLSS / FSR modes, plain shares when off)
        const bool is_dlss = g3d.upscaler == "dlss", is_off = g3d.upscaler == "bilinear";
        const char* const* mzh = is_dlss ? kModeDlssZh : is_off ? kModeOffZh : kModeTemporalZh;
        const char* const* men = is_dlss ? kModeDlssEn : is_off ? kModeOffEn : kModeTemporalEn;
        if (card_choice("game_res", is_dlss ? "DLSS 模式" : is_off ? "渲染分辨率" : "超分模式",
                        is_dlss ? "DLSS mode" : is_off ? "Render resolution" : "Upscaler mode",
                        "3D 场景按输出分辨率的百分比渲染，再放大到输出；百分比越低越快、越模糊。分辨率固定，不会动态下降。「主机原版」= 游戏自己的动态 720p。重启后生效。",
                        "The 3D scene renders at this share of the output and is brought up to it: lower is faster and softer. The resolution "
                        "stays fixed (no dynamic drops). Console = the game's own dynamic 720p. Takes effect after a restart.",
                        &g3d.res_preset, mzh, men, kGameResCount, true)) {
            mark(p);
        }
        {   // the size this mode renders at, so the percentages mean something
            const int out_h = render_output_height(), out_w = out_h * 16 / 9;
            const double share = kRenderQuality[std::clamp(g3d.res_preset, 0, kRenderQualityCount - 1)].share;
            char line[160];
            if (share > 0.0) {
                const int in_h = static_cast<int>(std::lround(out_h * share)), in_w = static_cast<int>(std::lround(out_w * share));
                std::snprintf(line, sizeof line, tr("渲染 %dx%d -> 输出 %dx%d（输出分辨率见「显示」页）", "Renders %dx%d -> output %dx%d (output resolution: Display page)"), in_w, in_h, out_w, out_h);
            } else {
                std::snprintf(line, sizeof line, "%s", tr("渲染 最高 1280x720（游戏动态分辨率）-> 1920x1080 -> 窗口",
                                                         "Renders up to 1280x720 (the game's dynamic resolution) -> 1920x1080 -> the window"));
            }
            ImGui::TextDisabled("%s", line);
        }
        // A hint, not a gate: native 4K plus the capture's copies is what runs out of video memory first on small cards.
        if (g3d.res_preset == 0 && render_output_height() >= 2160 && nvn::mirror_device_vram() < (10ull << 30)) {
            ImGui::PushStyleColor(ImGuiCol_Text, kGold);
            ImGui::TextWrapped("%s", tr("注意：这台显卡的显存不到 10 GB，4K 会与 mirror capture 的拷贝（约 2–3.5 GiB）叠加，"
                                        "附加内容场景最容易撑不住；较低的档位更稳。",
                                        "This card has under 10 GB of video memory: a 4K scene stacks with the mirror capture's copies "
                                        "(~2-3.5 GiB), and the add-on scenes are where it runs out first; a lower step is safer."));
            ImGui::PopStyleColor();
        }
        if (g3d.upscaler == "dlss") {
            // DLSS runs through Streamline, which has to be loaded before the Vulkan device: switching to it live works only when the
            // game started with it (or with auto and the DLLs there); otherwise the choice is saved and applies at the next start.
            if (!extras::temporal_dlss_started()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kGold);
                ImGui::TextWrapped("%s", extras::temporal_dlss_available()
                                             ? tr("DLSS 需要在启动时加载：已保存，重启游戏后生效。", "DLSS loads at start-up: saved, it applies after a restart.")
                                             : tr("找不到 DLSS 文件：可在启动器的「设置 → 超分」中从 NVIDIA 下载。", "The DLSS files are missing: the launcher downloads them from NVIDIA (Settings, under Upscaler)."));
                ImGui::PopStyleColor();
            }
            int pidx = dlss_preset_index(g3d.dlss_preset);
            if (card_choice("dlss_preset", "DLSS 模型", "DLSS model",
                            "DLSS 使用的 AI 模型（实时切换）。默认 = 驱动按模式自动选择；K 是 Transformer 模型，画质最好；J 拖影更少但更闪。",
                            "Which AI model DLSS uses (applies live). Default lets the driver choose per mode; K is the transformer model and "
                            "looks best; J ghosts less but flickers more.",
                            &pidx, kDlssPresetZh, kDlssPresetEn, kDlssPresetCount, false, true)) {
                g3d.dlss_preset = kDlssPresetKeys[pidx];
                mark(p);
            }
        }
        if (g3d.upscaler != "dlss" && card_slider_f("dlss_sharp", "3D 超分锐度 (FSR RCAS)", "3D Upscaler sharpness",
                          "控制 FSR 3.1.6 / 4.1.2 / 2.3.4 内置锐化强度（0.00 关闭，1.00 最强，推荐 0.40 ~ 0.60），点应用后实时生效且不卡顿。",
                          "FSR RCAS sharpening strength (0.00 off, 1.00 max, 0.40-0.60 recommended). Applies live.",
                          &g3d.sharpness, 0.0, 1.0, 0.05, "%.2f", false, true)) {
            mark(p);
        }
    }

    card_group("画面色调 (拖动实时生效)", "Colour Grading (Live)");
    if (card_button("pic_neutral", "恢复原版色调", "Preset: Neutral",
                    "将对比度、饱和度、黑位、伽马恢复为游戏原始输出，实时生效。",
                    "Resets contrast, saturation, black level and gamma to untouched game output.",
                    pic.neutral() ? tr("当前", "Active") : "")) {
        if (extras::picture_preset("neutral", &pic)) {
            extras::set_picture_live(pic);
            mark(p);
        }
    }
    if (card_button("pic_vivid", "鲜艳通透预设", "Preset: Vivid",
                    "适度提升通透度：对比度 1.10、饱和度 1.15、黑位 -0.03，可有效消除画面灰蒙感，实时生效。",
                    "Modest boost: contrast 1.10, saturation 1.15, black level -0.03. Live preview.")) {
        if (extras::picture_preset("vivid", &pic)) {
            extras::set_picture_live(pic);
            mark(p);
        }
    }
    if (card_slider_f("pic_con", "对比度", "Contrast",
                      "拉大明暗反差。1.00 为游戏原版默认值，修改时画面实时变化。",
                      "Spreads tones from mid-grey; 1.00 is untouched. Applies live.",
                      &pic.contrast, 0.50, 1.50, 0.02, "%.2f", false, true)) {
        extras::set_picture_live(pic);
        mark(p);
    }
    if (card_slider_f("pic_sat", "饱和度", "Saturation",
                      "调整画面色彩鲜艳程度。0.00 为黑白，1.00 为原版，推荐 1.00~1.20。",
                      "Colour intensity; 1.00 is untouched. Applies live.",
                      &pic.saturation, 0.00, 2.00, 0.05, "%.2f", false, true)) {
        extras::set_picture_live(pic);
        mark(p);
    }
    if (card_slider_f("pic_blk", "黑位调整", "Black level",
                      "设为负值（如 -0.03）可压暗发灰的暗部，让夜景和阴影更通透；0.00 为原版。",
                      "Negative pulls hazy shadows toward pure black; 0.00 is untouched.",
                      &pic.black, -0.25, 0.25, 0.01, "%+.2f", false, true)) {
        extras::set_picture_live(pic);
        mark(p);
    }
    if (card_slider_f("pic_gam", "伽马亮度", "Gamma",
                      "调整中间调亮度：大于 1.00 画面更亮，小于 1.00 更暗，不影响纯黑和纯白。",
                      "Mid-tone brightness; 1.00 is untouched.",
                      &pic.gamma, 0.50, 2.00, 0.05, "%.2f", false, true)) {
        extras::set_picture_live(pic);
        mark(p);
    }

    card_group("贴图与渲染优化", "Textures & Rendering");
    int aniso = 0;
    for (int i = 0; i < kAnisoCount; ++i)
        if (kAnisoValues[i] == d.anisotropy) aniso = i;
    if (card_choice("aniso", "各向异性过滤", "Anisotropic filtering",
                    "大幅提升地面、石板和远景斜向贴图的清晰度，几乎无性能损耗（推荐 8x 或 16x）。",
                    "Sharpens angled ground and distant textures at almost zero GPU cost (8x/16x recommended).",
                    &aniso, kAnisoZh, kAnisoEn, kAnisoCount)) {
        d.anisotropy = kAnisoValues[aniso];
        mark(p);
    }
    if (card_slider_f("lod_bias", "贴图 LOD 偏移", "Texture LOD bias",
                      "负值（如 -0.50）会优先选用更清晰的贴图细节，正值更柔和。原生 1440p / 4K 下 0.00 ~ -0.50 最佳。",
                      "Negative values select sharper mip levels; 0.00 to -0.50 is ideal at 1440p / 4K.",
                      &d.lod_bias, -2.0, 2.0, 0.25, "%+.2f")) {
        mark(p);
    }
    int astc = 1;
    for (int i = 0; i < kAstcCount; ++i)
        if (d.astc_quality == kAstcValues[i]) astc = i;
    if (card_choice("astc", "ASTC 贴图转码质量", "ASTC texture quality",
                    "PC 显卡不支持主机 ASTC 压缩，需转码为 BC7。「高质量」会在后台精编并缓存到磁盘，画质与显存最均衡。",
                    "Converts Switch ASTC textures to BC7. High re-encodes cleanly in background and caches to disk.",
                    &astc, kAstcZh, kAstcEn, kAstcCount, true)) {
        d.astc_quality = kAstcValues[astc];
        mark(p);
    }
    if (card_toggle("prewarm", "后台着色器预热", "Shader pipeline prewarm",
                    "启动时在后台提前编译历史记录的渲染管线，大幅减少跑图进新区域时的首次编译卡顿。",
                    "Precompiles previously seen pipelines on background threads to reduce area-load stutter.",
                    &d.shader_prewarm, true)) {
        mark(p);
    }
    if (card_toggle("fast_vram", "显存镜像加速", "Faster GPU memory",
                    "在独立显存保留常用缓冲副本（需额外最多 2 GiB 显存）。nxrt.ini 中的 [nvn] 设置优先于此项。",
                    "Keeps device-local copies of frequently read buffers. [nvn] in nxrt.ini takes priority.",
                    &d.fast_gpu_memory, true)) {
        mark(p);
    }
    if (card_toggle("barriers", "精简管线屏障", "Fewer GPU waits",
                    "减少冗余渲染屏障以略微提升性能；如遇老驱动花屏可关闭（使用完整屏障）。",
                    "Reduces redundant barriers for slightly higher speed; turn off if driver artifacts appear.",
                    &d.fewer_gpu_waits, true)) {
        mark(p);
    }

    card_group("后处理窗口缩放锐化 (可选)", "Post-Present Spatial Upscale (Optional)");
    {
        int sidx = 0;
        for (int i = 0; i < kSpatialUpCount; ++i)
            if (kSpatialUpModes[i] == up.mode) sidx = i;
        if (card_choice("up_mode", "窗口空间锐化器", "Spatial window sharpener",
                        "仅当游戏输出分辨率低于屏幕/窗口尺寸时触发（例如 1080p/1440p 游戏拉伸到 4K 全屏）；1:1 真原生输出时保持关闭即可。",
                        "Applies FSR 1 or NIS when upscaling a smaller game output to a larger window (no-op at 1:1 native).",
                        &sidx, kSpatialUpZh, kSpatialUpEn, kSpatialUpCount)) {
            up.mode = kSpatialUpModes[sidx];
            mark(p);
        }
    }
    if (up.mode == extras::UpscaleMode::Fsr1 || up.mode == extras::UpscaleMode::Nis) {
        if (card_slider_f("up_sharp", "空间锐化强度", "Spatial sharpness",
                          "调整 FSR 1 / NIS 窗口后处理锐化强度（0.00 关闭，1.00 最强）。",
                          "Sharpening strength for post-present FSR 1 / NIS.",
                          &up.sharpness, 0.0, 1.0, 0.05, "%.2f")) {
            mark(p);
        }
    }

    card_group("HDR 高动态范围输出", "HDR Output");
    int hmode = static_cast<int>(hdr.mode);
    if (card_choice("hdr_mode", "HDR 模式", "HDR mode",
                    "开启需显示器及 Windows 系统均开启 HDR。「Switch 2 原生 HDR」使用游戏原版 HDR 色调映射参数。",
                    "Requires an HDR display with Windows HDR enabled.",
                    &hmode, kHdrModeZh, kHdrModeEn, 4, true)) {
        hdr.mode = static_cast<extras::HdrMode>(hmode);
        mark(p);
    }
    int hfmt = static_cast<int>(hdr.format);
    if (card_choice("hdr_fmt", "HDR 信号格式", "HDR format",
                    "发送给显示器的信号格式：HDR10 或 scRGB，自动会优先选用显示器原生支持的 HDR10。",
                    "Signal format sent to display (HDR10 or scRGB).",
                    &hfmt, kHdrFmtZh, kHdrFmtEn, 3, true)) {
        hdr.format = static_cast<extras::HdrFormat>(hfmt);
        mark(p);
    }
    int peak = static_cast<int>(std::lround(hdr.peak_nits));
    if (card_slider_i("hdr_peak", "HDR 峰值亮度", "HDR peak brightness",
                      "高光能达到的最大亮度（尼特），设为自动则直接读取显示器上报的峰值亮度。",
                      "Maximum highlight brightness in nits (Auto reads display capability).",
                      &peak, 0, 2000, 50, "%d nits", false, false, tr("自动", "Auto"), 200)) {
        hdr.peak_nits = peak;
        mark(p);
    }
    int paper = static_cast<int>(std::lround(hdr.paper_white_nits));
    if (card_slider_i("hdr_paper", "HDR 纸白亮度", "HDR paper white",
                      "普通白色与 UI 的亮度（尼特），自动会跟随 Windows SDR 内容亮度设置。",
                      "Standard white & UI luminance in nits.",
                      &paper, 0, 500, 10, "%d nits", false, false, tr("自动", "Auto"), 80)) {
        hdr.paper_white_nits = paper;
        mark(p);
    }
    if (card_slider_f("hdr_boost", "HDR 高光增强", "HDR highlight boost",
                      "控制高光向峰值亮度扩展的幅度，0.00 接近 SDR，1.00 标准 HDR。",
                      "Expands highlights toward peak brightness.",
                      &hdr.highlight_boost, 0.0, 2.0, 0.10, "%.2f")) {
        mark(p);
    }

    // ---- Experimental: options of this PC version, not of the game, each with what it costs ----
    card_group("实验性功能", "Experimental");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));   // a plain note, not a warning
    ImGui::TextWrapped("%s", tr("实验性功能：并非在所有系统上效果都一样。",
                                "Experimental features: they may not work the same way on every system."));
    ImGui::PopStyleColor();
    {   // [nvn] mirror_capture: auto / on / off, with what it does and what it costs
        static constexpr const char* kCapKeys[] = {"auto", "on", "off"};
        static constexpr const char* kCapZh[] = {"自动 (显存足够时开启)", "开启", "关闭 (节省显存)"};
        static constexpr const char* kCapEn[] = {"Automatic (on when the card has room)", "On", "Off (saves video memory)"};
        int ci = 0;
        for (int i = 0; i < 3; ++i)
            if (g3d.capture == kCapKeys[i]) ci = i;
        if (card_choice("mirror_cap", "帧流水线 (Mirror Capture)", "Frame pipelining (mirror capture)",
                        "让游戏在当前画面还在绘制时就准备好下一帧，画面更流畅，也能消除部分显卡上的闪烁。代价：多占用约 2–3.5 GB 显存。重启后生效。",
                        "Lets the game get the next frame ready while the current one is still being drawn, for smoother play. It also "
                        "removes flicker on some graphics cards. The cost: about 2-3.5 GB of extra video memory. Takes effect after a restart.",
                        &ci, kCapZh, kCapEn, 3, true)) {
            g3d.capture = kCapKeys[ci];
            mark(p);
        }
    }

    pane_end(live);
}

// ---- Tab 2: Audio -------------------------------------------------------------------------------------------------------------------
void tab_audio(const LiveStatus& live, Page& p, float bottom_h) {
    AudioSettings& a = p.edit.audio;
    if (!p.audio_loaded) load_audio_devices(p);

    pane_begin("tab_audio", bottom_h);

    card_group("音量控制", "Volume");
    int vol_pct = static_cast<int>(std::lround(a.volume * 100.0));
    if (card_slider_i("vol", "主音量", "Master volume",
                      "控制游戏整体输出音量（0% 静音 ~ 100% 满音量）。",
                      "Master audio output gain (0% to 100%).",
                      &vol_pct, 0, 100, 5, "%d%%")) {
        a.volume = std::clamp(vol_pct, 0, 100) / 100.0;
        mark(p);
    }
    if (card_toggle("mute_bg", "切出窗口时自动静音", "Mute when unfocused",
                    "当你 Alt+Tab 切换到桌面或其他软件时自动静音，切回游戏自动恢复。",
                    "Automatically mutes game audio when the window loses focus.", &a.mute_unfocused)) {
        mark(p);
    }

    card_group("输出设备", "Output Device");
    {
        std::vector<std::string> devs;
        devs.emplace_back(tr("系统默认设备", "System default"));
        int cur = 0;
        for (std::size_t i = 0; i < p.audio_devices.size(); ++i) {
            devs.push_back(p.audio_devices[i]);
            if (a.device == p.audio_devices[i]) cur = static_cast<int>(i) + 1;
        }
        if (card_choice_vec("adev", "音频输出设备", "Output device",
                            "选择播放游戏声音的扬声器或耳机设备。更改输出设备需重启游戏生效。",
                            "Selects the audio output device (takes effect on next launch).",
                            &cur, devs, true)) {
            a.device = cur == 0 ? "default" : p.audio_devices[static_cast<std::size_t>(cur - 1)];
            mark(p);
        }
    }
    {
        char cnt[48];
        std::snprintf(cnt, sizeof cnt, tr("共 %d 个设备", "%d devices"), static_cast<int>(p.audio_devices.size()));
        if (card_button("refresh_audio", "刷新音频设备列表", "Refresh audio devices",
                        "如果刚插入耳机或切换了声卡，点击此项重新扫描可用输出设备。",
                        "Rescans available Windows audio output devices.", cnt)) {
            load_audio_devices(p);
            set_notice(p, tr("音频设备列表已刷新", "Audio device list refreshed"), false);
        }
    }

    pane_end(live);
}

// ---- Tab 3: Controls ----------------------------------------------------------------------------------------------------------------
void tab_controls(UiHost& host, const LiveStatus& live, Page& p, float bottom_h) {
    (void)host;
    PadSettings& pad = p.edit.pad;

    pane_begin("tab_controls", bottom_h);

    card_group("手柄参数设置 (应用后实时生效)", "Gamepad Tuning (Live)");
    if (card_toggle("abxy_lbl", "按键按标识映射 (Xbox A = Switch A)", "Buttons by label (A=A)",
                    "开启：Xbox 手柄下键 A 对应 Switch A（确认）、右键 B 对应取消。关闭：按任天堂物理位置，右键对应 A（确认）、下键对应 B（取消）。",
                    "On: Xbox bottom A acts as Switch A. Off: Nintendo positional layout (east button = Switch A).",
                    &pad.label_abxy, false, true)) {
        mark(p);
    }
    if (card_slider_f("ls_dz", "左摇杆死区", "Left stick dead zone",
                      "过滤左摇杆（移动）中心轻微漂移，数值越大需要推得越远才开始响应。",
                      "Filters minor left-stick drift near centre.", &pad.stick_deadzone, 0.0, 0.50, 0.02, "%.2f", false, true)) {
        mark(p);
    }
    if (card_slider_f("rs_dz", "右摇杆死区", "Right stick dead zone",
                      "过滤右摇杆（旋转视角）中心轻微漂移。",
                      "Filters minor right-stick drift near centre.", &pad.right_deadzone, 0.0, 0.50, 0.02, "%.2f", false, true)) {
        mark(p);
    }
    if (card_slider_f("look_s", "右摇杆视角灵敏度", "Right-stick look sensitivity",
                      "控制手柄右摇杆旋转镜头的速度倍率（0.25x ~ 3.00x，实时生效）。",
                      "Multiplier for right-stick camera rotation speed (0.25x - 3.00x).", &pad.look_sens, 0.25, 3.00, 0.05, "%.2fx", false, true)) {
        mark(p);
    }
    if (card_toggle("pad_invy", "反转手柄垂直视角", "Invert right-stick Y",
                    "开启后右摇杆向上推会向下看（实时生效）。",
                    "Pushing right stick up looks down (applies live).", &pad.invert_y, false, true)) {
        mark(p);
    }
    if (card_toggle("rumble", "手柄震动反馈", "Controller rumble",
                    "开启或关闭游戏内对手柄发送的震动反馈（实时生效）。",
                    "Enables or disables gamepad vibration (applies live).", &pad.rumble, false, true)) {
        mark(p);
    }
    if (card_slider_f("rumble_s", "震动强度", "Rumble strength",
                      "调整手柄震动的强弱（0.00x 静音 ~ 1.00x 标准强度，实时生效）。",
                      "Scales gamepad vibration intensity (0.00x - 1.00x).", &pad.rumble_scale, 0.0, 1.0, 0.10, "%.2fx", false, true)) {
        mark(p);
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader(tr("实时手柄 / 键盘输入状态测试", "Live Switch Pad Monitor"), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (int i = 0; i < kSwitchButtonCount; ++i) {
            const bool on = (live.out.buttons & hid::btn(kSwitchButtons[i].bit)) != 0;
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_Button, on ? kPressed : kReleased);
            ImGui::Button(kSwitchButtons[i].name, ImVec2(px(58.0f), 0.0f));
            ImGui::PopStyleColor();
            ImGui::PopID();
            if (i % 8 != 7) ImGui::SameLine();
        }
        ImGui::Text("LS (%.2f, %.2f)   RS (%.2f, %.2f)", live.out.lx, live.out.ly, live.out.rx, live.out.ry);
    }

    card_group("当前键位速查表 (实时映射)", "Active Controls Reference");
    card_button("kb_move", "角色移动 (左摇杆)", "Move (Left Stick)",
                "键盘 WASD 或方向键控制左摇杆移动；连接手柄后直接用左摇杆。",
                "Keyboard WASD or Arrow keys control Left Stick.", "WASD / 方向键");
    card_button("kb_cam", "旋转视角 (右摇杆)", "Camera (Right Stick)",
                "键盘 I / J / K / L 控制视角转动；连接手柄后直接用右摇杆。",
                "Keyboard I / J / K / L control Right Stick camera.", "I / J / K / L");
    card_button("kb_abxy", "确认 / 取消 / 功用键 (A B X Y)", "Face Buttons (A B X Y)",
                "空格或回车 = A（确认），退格或 Esc = B（取消），U = X（快传/菜单），O = Y。",
                "Space/Enter = A, Backspace/Esc = B, U = X, O = Y.", "Space / Back / U / O");
    card_button("kb_shld", "肩键与扳机 (L / R / ZL / ZR)", "Shoulders & Triggers",
                "Q = L，E = R，数字键 1 = ZL，数字键 2 = ZR。",
                "Q = L, E = R, 1 = ZL, 2 = ZR.", "Q / E / 1 / 2");
    card_button("kb_sys", "菜单 / 地图 / 摇杆下压", "PLUS / MINUS / L3 / R3",
                "Tab 或 + = PLUS（主菜单），- = MINUS，Shift = 左摇杆按下，Ctrl = 右摇杆按下；手柄同时按 - 与 + 可呼出本 F1 菜单。",
                "Tab/+ = PLUS, - = MINUS, Shift/Ctrl = L3/R3. Gamepad MINUS + PLUS toggles F1 menu.", "Tab / - / Shift / Ctrl");

    pane_end(live);
}

// ---- Tab 4: Overlay, Hotkeys & Language ---------------------------------------------------------------------------------------------
void tab_overlay(UiHost& host, const LiveStatus& live, Page& p, float bottom_h) {
    const FpsSettings fps = host.settings().fps;

    pane_begin("tab_overlay", bottom_h);

    card_group("帧率与性能监控 (实时生效)", "Performance Overlay (Live)");
    bool show = fps.show;
    if (card_toggle("fps_show", "显示帧率监控", "Show FPS counter",
                    "在屏幕角落实时显示当前帧率，也可随时按 F10 开关。",
                    "Shows a live frame rate counter in the corner (hotkey F10).", &show, false, true)) {
        FpsSettings f = fps;
        f.show = show;
        set_fps_live(host, f);
    }

    int corner = static_cast<int>(fps.corner);
    if (card_choice("fps_corner", "监控显示位置", "Overlay corner",
                    "选择帧率监控卡片停靠在屏幕的哪一个角落。",
                    "Selects which corner displays the FPS counter.",
                    &corner, kCornerZh, kCornerEn, kCornerCount, false, true)) {
        FpsSettings f = fps;
        f.corner = static_cast<Corner>(corner);
        set_fps_live(host, f);
    }

    int detail = std::clamp(fps.detail, 0, kFpsDetailCount - 1);
    if (card_choice("fps_detail", "监控信息详细度", "Counter detail",
                    "可选仅显示 FPS、简洁统计，或开启「详细性能监控面板」实时查看 GPU/显存/CPU/内存、帧时间曲线、渲染管线耗时与当前分辨率/超分状态。",
                    "Choose compact FPS-only counter or the full Detailed Performance HUD (GPU/VRAM/CPU/RAM, frame-time graph, pipeline timings, resolution & scaling).",
                    &detail, kFpsDetailZh, kFpsDetailEn, kFpsDetailCount, false, true)) {
        FpsSettings f = fps;
        f.detail = detail;
        set_fps_live(host, f);
    }

    bool in_shots = fps.in_screenshots;
    if (card_toggle("fps_shots", "截图时包含帧率监控", "Include counter in screenshots",
                    "关闭（推荐）：按 F12 截图时会保存纯净无遮挡的游戏画面。",
                    "Off (recommended) keeps F12 screenshots clean without the FPS counter.", &in_shots, false, true)) {
        FpsSettings f = fps;
        f.in_screenshots = in_shots;
        set_fps_live(host, f);
    }

    {
        const std::string key = input_name(fps.key);
        const char* right = p.fps_capture ? tr("请按键盘按键...", "Press a key...") : (key.empty() ? "F10" : key.c_str());
        if (card_button("fps_key", "更改帧率监控快捷键", "Change FPS hotkey",
                        "点击后按任意键盘按键即可替换默认的 F10 帧率开关快捷键，按 Esc 取消。",
                        "Click and press any keyboard key to rebind the FPS overlay toggle.", right)) {
            stop_rebind(host, p);
            p.fps_capture = true;
            host.begin_capture();
        }
    }

    card_group("菜单外观与语言", "Menu & Language");
    {
        static const char* const kLangs[] = {"简体中文", "English"};
        int lang = g_zh ? 0 : 1;
        if (card_choice("menu_lang", "菜单界面语言", "Menu language",
                        "在简体中文与英文之间实时切换当前 F1 设置菜单的显示语言，并自动保存到 controls.ini。",
                        "Switches the F1 settings menu between Simplified Chinese and English (saved automatically).",
                        &lang, kLangs, kLangs, 2, false, true)) {
            g_zh = (lang == 0);
            FpsSettings f = fps;
            f.menu_lang = g_zh ? "zh" : "en";
            set_fps_live(host, f);
        }
    }

    card_group("常用快捷键速查", "Hotkeys Reference");
    card_button("hk_f1", "打开 / 关闭系统设置菜单", "Toggle settings menu",
                "随时唤出或隐藏本设置菜单；菜单打开期间自动屏蔽游戏内按键输入，防止误操作。手柄按 - + + 也可呼出。",
                "Opens or closes this menu while blocking game input. Gamepad: Minus + Plus.", "F1 / Esc");
    card_button("hk_f10", "开关帧率性能监控", "Toggle FPS counter",
                "随时在右上角显示或隐藏实时帧率与帧时间监控。",
                "Toggles the live corner FPS overlay.", "F10");
    card_button("hk_f11", "切换全屏 / 窗口模式", "Toggle fullscreen",
                "在无边框全屏与窗口模式之间瞬间切换。",
                "Instantly toggles borderless fullscreen and windowed mode.", "F11 / Alt+Enter");
    card_button("hk_f12", "保存无损 PNG 截图", "Save PNG screenshot",
                "将当前原生高清画面保存为无损 PNG 到 xc2/build/boot/shots_manual/ 目录。",
                "Saves a lossless PNG screenshot into shots_manual/.", "F12");

    pane_end(live);
}

// ---- Tab bar helper -----------------------------------------------------------------------------------------------------------------
void draw_category_tabs(Page& p) {
    struct TabDef {
        const char* zh;
        const char* en;
    };
    static const TabDef kTabs[] = {
        {"显示", "Display"},
        {"画质", "Graphics"},
        {"音频", "Audio"},
        {"控制", "Controls"},
        {"界面与快捷键", "Overlay"},
    };
    constexpr int kTabCount = static_cast<int>(sizeof kTabs / sizeof kTabs[0]);

    const float total_w = ImGui::GetContentRegionAvail().x;
    const float gap = px(8.0f);
    const float btn_w = (total_w - gap * (kTabCount - 1)) / static_cast<float>(kTabCount);
    const float btn_h = px(38.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int i = 0; i < kTabCount; ++i) {
        if (i > 0) ImGui::SameLine(0.0f, gap);
        const bool sel = (p.active_tab == i);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##cat_tab", ImVec2(btn_w, btn_h));
        const bool hov = ImGui::IsItemHovered();
        const bool foc = ImGui::IsItemFocused();
        ImGui::PopID();
        if (clicked) {
            p.active_tab = i;
            g_pane.shown.valid = false;
            g_pane.focus_first = true;
        }
        const ImU32 bg = sel ? IM_COL32(28, 86, 102, 235)
                             : (hov || foc) ? IM_COL32(24, 64, 82, 195) : IM_COL32(12, 24, 40, 175);
        dl->AddRectFilled(pos, ImVec2(pos.x + btn_w, pos.y + btn_h), bg, px(8.0f));
        if (sel) {
            dl->AddRect(pos, ImVec2(pos.x + btn_w, pos.y + btn_h), kColGold, px(8.0f), 0, 1.5f);
            dl->AddRectFilled(ImVec2(pos.x + px(14.0f), pos.y + btn_h - px(3.0f)),
                              ImVec2(pos.x + btn_w - px(14.0f), pos.y + btn_h), kColGold, px(2.0f));
        } else {
            dl->AddRect(pos, ImVec2(pos.x + btn_w, pos.y + btn_h), IM_COL32(65, 130, 150, 65), px(8.0f), 0, 1.0f);
        }
        const char* lbl = tr(kTabs[i].zh, kTabs[i].en);
        const ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(pos.x + (btn_w - ts.x) * 0.5f, pos.y + (btn_h - ts.y) * 0.5f),
                    sel ? kColGold : kColWhite, lbl);
    }

    // L1 / R1 gamepad shoulder buttons switch tabs cleanly
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
        p.active_tab = (p.active_tab + kTabCount - 1) % kTabCount;
        g_pane.shown.valid = false;
        g_pane.focus_first = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
        p.active_tab = (p.active_tab + 1) % kTabCount;
        g_pane.shown.valid = false;
        g_pane.focus_first = true;
    }
}

}  // namespace

void draw_settings_page(UiHost& host) {
    static Page page;
    Page& p = page;
    static int last_frame = -2;
    const int frame = ImGui::GetFrameCount();
    if (frame != last_frame + 1) g_pane.focus_first = true;
    last_frame = frame;
    if (!p.loaded) {
        p.edit = host.settings();
        p.edit.fps = host.settings().fps;
        g_zh = (p.edit.fps.menu_lang != "en");
        refresh_out_text(p);
        p.loaded = true;
    }
    poll_capture(host, p);
    const LiveStatus live = host.status();

    draw_category_tabs(p);
    ImGui::Dummy(ImVec2(0.0f, px(6.0f)));

    const float bottom_h = px(56.0f);
    switch (p.active_tab) {
        case 0: tab_display(live, p, bottom_h); break;
        case 1: tab_graphics(live, p, bottom_h); break;
        case 2: tab_audio(live, p, bottom_h); break;
        case 3: tab_controls(host, live, p, bottom_h); break;
        default: tab_overlay(host, live, p, bottom_h); break;
    }

    // Pinned bottom action bar
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, px(3.0f)));

    if (p.dirty) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.50f, 0.46f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.64f, 0.58f, 1.0f));
    }
    if (ImGui::Button(tr("应用并保存", "Apply & Save"), ImVec2(px(138.0f), px(34.0f)))) apply_now(host, p);
    if (p.dirty) ImGui::PopStyleColor(2);

    ImGui::SameLine();
    if (ImGui::Button(tr("放弃修改", "Revert"), ImVec2(px(108.0f), px(34.0f)))) revert_to_saved(host, p);
    ImGui::SameLine();
    if (ImGui::Button(tr("恢复默认", "Defaults"), ImVec2(px(108.0f), px(34.0f)))) load_factory(host, p);

    ImGui::SameLine(0.0f, px(16.0f));
    if (host.capturing()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kGold, "%s", tr("请按下要绑定的按键，按 Esc 取消...", "Press a key to bind, Esc to cancel..."));
    } else if (p.dirty) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kOrange, "%s", tr("● 有未保存的修改", "● Unsaved changes"));
    } else if (!p.notice.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(p.notice_warn ? kOrange : kEmerald, "%s", p.notice.c_str());
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kDimText, "%s", tr("提示：方向键或悬停选择条目，左右微调，右侧查看说明", "Tip: D-pad or hover to select, Left/Right to adjust"));
    }
}

void draw_settings_menu(UiHost& host, gfx::Overlay& ov) {
    static bool lang_init = false;
    if (!lang_init) {
        lang_init = true;
        g_zh = (host.settings().fps.menu_lang != "en");
    }
    static int last_frame = -2;
    const int frame = ImGui::GetFrameCount();
    const bool opened = frame != last_frame + 1;
    last_frame = frame;
    if (!opened && !host.capturing() && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup) &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        ov.request_hide();
    }

    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0.0f, 0.0f), disp, IM_COL32(3, 8, 16, 155));

    const float em = ImGui::GetFontSize();
    const float w = std::min(disp.x - 2.5f * em, std::max(54.0f * em, disp.x * 0.74f));
    const float h = std::min(disp.y - 2.5f * em, std::max(34.0f * em, disp.y * 0.82f));
    ImGui::SetNextWindowPos(ImVec2(disp.x * 0.5f, disp.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    if (opened) ImGui::SetNextWindowFocus();

    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##xc2_settings_modal", nullptr, kFlags)) {
        // Custom sleek top header bar
        ImGui::PushStyleColor(ImGuiCol_Text, kGold);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(tr("异度神剑 2  ·  系统设置", "Xenoblade Chronicles 2  ·  Settings"));
        ImGui::PopStyleColor();

        const float close_w = px(92.0f);
        const float lang_w = px(96.0f);
        const float quit_w = px(104.0f);
        const float right_x = ImGui::GetWindowContentRegionMax().x;
        ImGui::SameLine(std::max(px(260.0f), right_x - close_w - lang_w - quit_w - px(18.0f)));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.46f, 0.14f, 0.16f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.20f, 0.22f, 1.0f));
        if (ImGui::Button(tr("退出游戏", "Quit game"), ImVec2(quit_w, 0.0f))) ImGui::OpenPopup("##quit_confirm");
        ImGui::PopStyleColor(2);
        if (ImGui::BeginPopupModal("##quit_confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            ImGui::TextUnformatted(tr("退出游戏？上次存档之后的进度将会丢失。", "Quit the game? Progress since your last save is lost."));
            ImGui::Dummy(ImVec2(0.0f, px(6.0f)));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.46f, 0.14f, 0.16f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.20f, 0.22f, 1.0f));
            if (ImGui::Button(tr("退出", "Quit"), ImVec2(px(104.0f), 0.0f))) {
                ImGui::CloseCurrentPopup();
                host.request_exit();
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            // Cancel, Esc, or a click anywhere outside the dialog closes it. Outside is where the press landed against the dialog's
            // rectangle, not ImGui's hover state: in play that reads "not hovered" even on the dialog's own buttons.
            const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize(), cp = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left];
            const bool outside = !ImGui::IsWindowAppearing() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                                 (cp.x < wp.x || cp.y < wp.y || cp.x >= wp.x + ws.x || cp.y >= wp.y + ws.y);
            if (ImGui::Button(tr("取消", "Cancel"), ImVec2(px(104.0f), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || outside)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(g_zh ? "EN / 中文" : "中文 / EN", ImVec2(lang_w, 0.0f))) {
            g_zh = !g_zh;
            FpsSettings f = host.settings().fps;
            f.menu_lang = g_zh ? "zh" : "en";
            set_fps_live(host, f);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("关闭 (F1)", "Close (F1)"), ImVec2(close_w, 0.0f))) ov.request_hide();

        ImGui::Dummy(ImVec2(0.0f, px(4.0f)));
        draw_settings_page(host);
    }
    ImGui::End();
}

}  // namespace nxrt::play

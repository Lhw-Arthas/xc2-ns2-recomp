# XC2 NS2 Recomp v0.2.1 — Quality Presets, Official DLSS/FSR Downloaders & 8 GB VRAM Support

This maintenance & community-PR update integrates **PR #30** (@UrsineBear) and **PR #36** (@nikolaybutnik) together with targeted startup, audio (#37), VRAM, and UI fixes. See **Upgrading to v0.2.1** below for the recommended clean migration steps.

## Which file do I download?

| file | what it is | do I need it? | SHA-256 |
|---|---|---|---|
| `XC2-package-dist-0.2.1.zip` | player package: launcher, runtime, lifter and shader tools | **yes** — play from this one | `038a8c2da19ffbbc77d14411b22b94e182ac65f263d310d196fe195f7b235e5a` |
| `xc2-launcher-0.2.1-src.zip` | launcher source code | no | `5f16bcbe67cd1cb2569a5d94cc09e1209ca85401884bd85840f6fdf536160fb1` |

## What's new

### Configurable render quality, output resolution & DLSS support (PR #36, @nikolaybutnik)

- **Display vs Graphics split in the F1 menu**: **Display** now selects the **Output resolution** (`1080p` / `1440p` / `4K`, `[am] resolution`) and windowed size (default `auto`, following your display up to 1440p); **Graphics** puts **Upscaler** (`DLSS`, `FSR 3.1.6`, `FSR 4.1.2`, `FSR 2.3.4`, `TAAU`, `Off`), **Mode / Render Quality** (`DLAA` / `Native AA` through `Ultra Performance` and `Console`), the live render -> output readout, and **DLSS Model** (`Default`, `K`, `J`) in one group.
- **Intent-based scaling (`[upscale] render_quality`, `[gfx] output_scale`)**: scene scales are computed at start-up from `[am] resolution`, and `[nvn] acquire_wait = auto` skips redundant acquire waits while `mirror_capture` is active.
- **Event Theater fix**: removed `AmericanEnglish` and `LatinAmericanSpanish` from `NXRT_TITLE_SUPPORTED_LANGUAGES` in setup / boot configuration.

### Optional official NVIDIA DLSS & AMD FSR downloaders in Launcher

- **Settings → Upscaler** lets players optionally download official prebuilt files directly onto their PC from **NVIDIA Streamline (`v2.14.16`)** (PR #36) or **AMD GPUOpen FidelityFX SDK (`v2.4.0` + `v1.1.4`)** into `upscalers/streamline/` or `upscalers/fsr3/`.
- Every download verifies exact byte size and SHA-256, places the upstream licence texts alongside the DLLs, and requires **explicit user confirmation of the upstream licence terms** beforehand. Neither this repository nor the release zip ships proprietary vendor DLLs.

### 8 GB VRAM support, live VRAM telemetry & menu ergonomics

- **Lower `mirror_capture = auto` floor (`3584 -> 1536 MiB`)**: 8 GB GPUs now enable frame pipelining automatically instead of falling back off.
- **Live VRAM visibility**: enabled `VK_EXT_memory_budget` when supported, added a `vram:` summary line to `nxrt.log` every 60 seconds, and surfaced live VRAM / pool-copy usage inside the F1 status card. Also added `[nvn] mirror_capture` (`auto` / `on` / `off`) to the F1 Experimental section.
- **F1 menu navigation & safe exit**: opening F1 or switching tabs focuses the first option card with clean D-pad Left/Right ownership ([#18](https://github.com/weile10000/xc2-ns2-recomp/issues/18)); the top-right **Quit game** button opens a confirmation dialog to avoid accidental exits.

### Renderer optimizations, audio fix & startup reliability (PR #30, @UrsineBear + fixes)

- **Audio crackling / static fix since v0.1.2 ([#37](https://github.com/weile10000/xc2-ns2-recomp/issues/37))**: fixed `xc2/src/x2_audio301.cpp` creating `nn::audio::OutputStream`'s release `SystemEvent` as `ManualClear` (`0`) instead of `EventClearMode_AutoClear` (`1`), and synced `watch_loop`'s queued water mark on writes — stopping the `guest-132` audio thread from busy-spinning on `GetOutputStreamFreeFrameCount` / `XAudio2Sink::GetState()` and eliminating audio lock contention / static crackling.
- Added `VK_AMD_shader_early_and_late_fragment_tests` (`early_fragment_tests = auto`), zero-copy colour feedback via `VK_EXT_attachment_feedback_loop_layout` (`[nvn] fb_reuse`), fixed a use-after-free in `nvn_pipeline.cpp`, tuned `nvn-texprep` priority, and auto-hid idle cursor in fullscreen/borderless modes.
- **FSR & fullscreen start-up hardening ([#18](https://github.com/weile10000/xc2-ns2-recomp/issues/18))**: prioritized standalone `amd_fidelityfx_upscaler_vk.dll` ahead of `amd_fidelityfx_vk.dll` when the FidelityFX loader is absent (preventing a first-dispatch `amdxc64.dll` crash when both sit in `upscalers/fsr3/`), and moved initial `Borderless` / `Exclusive` `set_fullscreen()` calls until after `Device::create()` / `Renderer::create()` so display/driver hooks cannot deadlock before Vulkan enumeration.
- **Faster first-time Set up (~34% shorter)**: parallelized `extract_embedded.py` zlib extraction, skipped redundant per-shader `spirv-val.exe` subprocesses (`--no-val`) and ~36,000 duplicate file copies in `build_cache.py`, lifted modules in one Python process at `bucket = 0x40000` with per-module PCH headers and `-Wl,--strip-debug` (no unused 469 MB PDB write/delete), and added activity-based early settle to headless shader prefill (~12s vs fixed 40s).
- **Startup & DLC path fixes**: `mem::alias_base()` probes a free 128 GiB host region when `0x2000000000` is occupied (`VammManager` `0xD401`, [#33](https://github.com/weile10000/xc2-ns2-recomp/issues/33)); `aoc_indices()` uses UTF-8 paths for non-ASCII DLC folders on Windows ([#25](https://github.com/weile10000/xc2-ns2-recomp/issues/25)).
- **CI hygiene**: removed a hardcoded user path from `tools/run/route.ps1` and scoped PR personal-path checks to changed files.

## Upgrading to v0.2.1

- **Recommended (Clean directory + migrate saves & shader cache)**: extract `v0.2.1` into a **fresh folder**, copy `save/`, `shader_cache/` (and `upscalers/` if you already downloaded DLSS/FSR) from your previous install, and run Set up once in the new launcher. Existing shaders are kept (no ~12,000-shader recompile), while `nxrt.ini`, `controls.ini`, `launch.env`, and `xc2_rt.dll` start completely clean with `v0.2.1` defaults (including Auto 1440p-capped window sizing and intent-based `render_quality`).
- **In-place upgrade (from `v0.2.0` only)**: if you extract directly over a `v0.2.0` folder, you **must launch the game from the new `xc2-launcher.exe`** (do not launch `xc2_boot.exe` directly beforehand, so the launcher copies the updated `runtime/xc2_rt.dll` next to the executable), and open **F1** once to re-save **Window size** and **Render quality / Upscaler mode** (otherwise old `controls.ini` `720p` window settings and legacy `nxrt.ini` scaling keys remain active). If any setting behaves unexpectedly, back up `save/` and `shader_cache/` and remove `nxrt.ini` and `controls.ini`. For `v0.1.x` installs, always use a fresh directory.

## Known issues

- **Lv. 130 Tyrannotitan Kurodil freeze on NVIDIA ([#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32))**: reproduces on both v0.1.4 and v0.2.0 even at 1080p with frame generation off (`nvoglv64.dll` wait / GPU TDR timeout around `fs be528292f3d04f1b`). Tracked for targeted shader/sync investigation in a future release.
- **Blinking / dialogue flicker** ([#15](https://github.com/weile10000/xc2-ns2-recomp/issues/15)): remains open. Opt-in `[nvn] view_reinterpret = 1` was tested and confirmed not to fix it (kept `0` by default).
- **Add-on scenes & first-run pipeline compile**: Torna / add-on scenes remain heavier than base game areas; first launch compiles Vulkan pipelines (~20 s pause).
- **Red edge** on the cloud sea silhouette in the opening battle background (cosmetic, single scene).

## Credits

Pull requests: [#30](https://github.com/weile10000/xc2-ns2-recomp/pull/30) (**@UrsineBear**), [#36](https://github.com/weile10000/xc2-ns2-recomp/pull/36) (**@nikolaybutnik**).
Issue reports that shaped this release: **@gao950912** ([#37](https://github.com/weile10000/xc2-ns2-recomp/issues/37) bisect + report).
Upstream: **fefw-recomp**, **nxrecomp**, **nxnative**, AMD **FidelityFX**, NVIDIA **Streamline**, **dav1d**, **SDL3**, **Dear ImGui**, **libvpx**, **libvorbis**, **FFmpeg**. Licences in `licenses/` and `THIRD_PARTY_NOTICES.md`.

---

# XC2 NS2 重编译 v0.2.1 —— 渲染质量预设、官方 DLSS/FSR 下载器与 8 GB 显存适配

本次更新整合了社区 **PR #30**（@UrsineBear）与 **PR #36**（@nikolaybutnik），并合入启动 `0xD401` 规避、非 ASCII DLC 路径修复、**#37 音频爆音修复**、8 GB 显存适配以及 F1 交互优化。升级方法请见下文「升级到 v0.2.1」建议。

## 我该下哪个文件？

| 文件 | 是什么 | 需要吗 | SHA-256 |
|---|---|---|---|
| `XC2-package-dist-0.2.1.zip` | 玩家包：启动器、运行时、提升工具与着色器工具 | **要** —— 直接用它玩 | `038a8c2da19ffbbc77d14411b22b94e182ac65f263d310d196fe195f7b235e5a` |
| `xc2-launcher-0.2.1-src.zip` | 启动器源码 | 不用 | `5f16bcbe67cd1cb2569a5d94cc09e1209ca85401884bd85840f6fdf536160fb1` |

## 本版变化

### 1. 可调 3D 渲染质量、输出分辨率与 DLSS 支持（PR #36，@nikolaybutnik）

- **F1 设置页分工重排**：「显示」页控制 **输出分辨率**（`1080p` / `1440p` / `4K`，`[am] resolution`）和 **窗口大小**（默认 `auto` 跟随显示器，上限 1440p）；「画质」页将 **超分器**（`DLSS`、`FSR 3.1.6`、`FSR 4.1.2`、`FSR 2.3.4`、`TAAU`、`关闭`）、**超分/渲染质量档位**（`DLAA / 原生抗锯齿` 到 `超级性能` 及 `主机原版`）、实时 `渲染 -> 输出` 尺寸预览与 **DLSS 模型**（`默认` / `K` / `J`）整合在同一分组。
- **意图驱动缩放（`[upscale] render_quality`、`[gfx] output_scale`）**：启动时按输出分辨率自动推算缩放倍率；同时引入 `[nvn] acquire_wait = auto`，在开启 `mirror_capture` 时跳过冗余等待。
- **修复过场剧场崩溃**：从语言白名单中移除引发崩溃的 `AmericanEnglish` 与 `LatinAmericanSpanish`。

### 2. 启动器内置官方 NVIDIA DLSS & AMD FSR 一键下载器

- 在「设置 → 超分」提供两家厂商的**官方直链可选下载**：NVIDIA Streamline / DLSS（`v2.14.16`，PR #36）以及 AMD GPUOpen FidelityFX SDK（`v2.4.0` + `v1.1.4`，自动安装到 `upscalers/fsr3/`）。
- 下载前必须由玩家**确认对应的上游厂商许可条款**，下载全程校对字节数与 SHA-256 哈希，并将上游许可文件一并保存至 DLL 同目录；**本仓库和发行包本身均不夹带任何厂商预编译 DLL**。

### 3. 8 GB 显存支持、显存占用监控与 F1 交互优化

- **下调 `mirror_capture = auto` 阈值（`3584 -> 1536 MiB`）**：8 GB 显卡现在也会默认自动开启帧流水线保护。
- **显存状态可见**：启用 `VK_EXT_memory_budget`，日志每 60 秒输出一次实际 `vram:` 用量，F1 右侧状态栏同步显示显存及池拷贝占用；画质页底部「实验性功能」可手动切换帧流水线（自动 / 开启 / 关闭）。
- **F1 手柄焦点与安全退出**：打开 F1 或切换标签页时自动聚焦首项、方向键左右稳定归属当前选项栏（[#18](https://github.com/weile10000/xc2-ns2-recomp/issues/18)）；顶部右上角 **退出游戏** 按钮带二次确认弹窗，防止误触直接关掉进程。

### 4. 渲染优化、音频爆音修复与启动修复（PR #30 @UrsineBear 及其他修复）

- **修复 v0.1.2 以来的音频带电爆音问题（[#37](https://github.com/weile10000/xc2-ns2-recomp/issues/37)）**：修正了 `xc2/src/x2_audio301.cpp` 将 `nn::audio::OutputStream` 释放事件误创建为 `ManualClear`（`0`）而非 `EventClearMode_AutoClear`（`1`）的问题，并在写入时同步唤醒线程的队列水位，彻底消除音频线程 100% 空转抢占 XAudio2 引擎锁引起的电流爆音与降速。
- 新增 `VK_AMD_shader_early_and_late_fragment_tests`（`early_fragment_tests = auto`）、`VK_EXT_attachment_feedback_loop_layout` 零拷贝色彩反馈（`[nvn] fb_reuse`）、修复 `nvn_pipeline.cpp` UAF、优化 `nvn-texprep` 线程优先级，以及全屏/无边框空闲鼠标自动隐藏。
- **FSR 加载优先级与无边框启动卡死修复（[#18](https://github.com/weile10000/xc2-ns2-recomp/issues/18)）**：缺失 loader 时优先选用可独立工作的 v2.4 `amd_fidelityfx_upscaler_vk.dll`（避免误选同目录 `amd_fidelityfx_vk.dll` 导致首帧崩溃）；同时将启动期的无边框/全屏切换延后到 Vulkan 设备与初始 Swapchain 建立之后，避免驱动/DWM 互锁黑屏。
- **大幅缩短首次 Set up 安装耗时（约提速 34%）**：`extract_embedded.py` 改为多线程并行 zlib 解压、`build_cache.py` 消除数万次冗余文件复制并支持发布包 `--no-val`、`xc2_setup.py` 合入单进程 lift + `bucket=0x40000` + 模块级 PCH 预编译头与免 PDB 链接，并改用自适应静止早停加速无头预热（实测同机从 ~11.7 分钟降至 ~7.6 分钟）。
- **启动与 DLC 路径**：`mem::alias_base()` 在默认 `0x2000000000` 被占用时自动探测空闲 128 GiB 窗口（[#33](https://github.com/weile10000/xc2-ns2-recomp/issues/33)）；`aoc_indices()` 改用 UTF-8 路径，修复中文等非 ASCII DLC 目录崩溃（[#25](https://github.com/weile10000/xc2-ns2-recomp/issues/25)）。
- **CI 检查**：清理 `tools/run/route.ps1` 硬编码本机路径，并将 PR 个人路径检查限定在改动文件范围内。

## 升级到 v0.2.1（推荐方案与注意事项）

- **最稳妥推荐（全新目录 + 迁移存档与缓存，零旧配置残留冲突）**：
  1. 将 `v0.2.1` 解压到一个**全新的干净目录**；
  2. 把旧目录下的 `save\` 与 `shader_cache\`（以及如有下载过的 `upscalers\`）复制到新目录中；
  3. 打开新目录的 `xc2-launcher.exe` 重新跑一遍 Set up（已有缓存会直接复用，无需重新耗时编译 12,137 个着色器）。这样既保留全部进度与缓存，又能确保 `nxrt.ini`、`controls.ini`、`launch.env` 与 `xc2_rt.dll` 均为纯净 `0.2.1` 状态，自动启用新版默认 **Auto（上限 1440p）窗口尺寸**与 `render_quality` 逻辑。
- **原地覆盖升级（仅限刚装过 `v0.2.0` 的安装）**：
  - 如直接覆盖解压进原 `v0.2.0` 同级根目录，务必做到两件事：**① 必须先从新版 `xc2-launcher.exe` 点 Play 启动**（不要先直接双击 `xc2_boot.exe`，让启动器自动把 `runtime\xc2_rt.dll` 刷新到根目录）；**② 进游戏按 F1 重新选一次窗口大小与渲染质量并点「应用并保存」**（否则旧 `controls.ini` 里的 `1280x720` 与旧 `nxrt.ini` 缩放键值会继续盖住新默认值）。如出现异常，保留 `save\` 与 `shader_cache\` 后删除 `nxrt.ini` / `controls.ini` 再启动即可。`v0.1.x` 老用户请务必使用全新目录升级。

## 已知问题

- **130 级暴虐巨神兽库洛戴伊尔战斗画面冻结（[#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32)）**：N 卡下即使 1080p 且关闭帧生成仍可能在特定高并发绘制处触发 `nvoglv64.dll` TDR 挂起（0.1.4 与 0.2.0 均存在），已列入后续版本专项待修复清单。
- **眨眼 / 对白眼嘴闪烁**（[#15](https://github.com/weile10000/xc2-ns2-recomp/issues/15)）：继续挂起排查（已实机验证 `[nvn] view_reinterpret = 1` 并非根因，默认保持关闭）。
- **附加内容重场景负载较高**，以及首次运行约 20 秒管线编译提示。
- 开场战斗背景云海红边（单场景纯视觉）。

## 致谢

本版合入 PR：[#30](https://github.com/weile10000/xc2-ns2-recomp/pull/30)（**@UrsineBear**）、[#36](https://github.com/weile10000/xc2-ns2-recomp/pull/36)（**@nikolaybutnik**）。
协助定位本次修复的反馈：**@gao950912**（[#37](https://github.com/weile10000/xc2-ns2-recomp/issues/37) 版本二分与报告）。
上游与依赖：**fefw-recomp**、**nxrecomp**、**nxnative**、AMD **FidelityFX**、NVIDIA **Streamline**、**dav1d**、**SDL3**、**Dear ImGui**、**libvpx**、**libvorbis**、**FFmpeg**，许可详见 `licenses/` 与 `THIRD_PARTY_NOTICES.md`。

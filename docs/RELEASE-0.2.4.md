# XC2 NS2 Recomp v0.2.4 — FSR 3 Frame Generation & Dynres Fixes, Intel Pipeline Cache, Setup & 1080p Native Baseline

This release ships all fixes verified after `v0.2.3`, including **#53 AMD FSR 3 Frame Generation (`HUDLessColor`) + HDR10 parity**, **FidelityFX FSR `3.1` default + automatic `FSR 4.*` dynamic-resolution slot isolation**, **#55/#56 dynamic-resolution & viewport scaling fixes**, **upgraded F10 dual FPS HUD + counter overflow fix**, **#49 Intel `vkGetPipelineCacheData` crash fix**, **#51/#54 opt-in `depth_bias_force_unorm`**, **#57 HDR highlight expansion slider**, **#52 `dav1d` software decoder & finalize fixes**, **#25 non-ASCII DLC folder handling**, **#58 PowerShell 5.1 Setup fallback & custom `game_dir` packaged mode**, **Launcher XeSS/120 Hz cleanup & upscaler download confirmation**, and a **clean 1080p Native TAAU docked baseline**.

> **Upgrading from `v0.2.3` (or `v0.2.2`)? Read this!**
> 1. **Existing `v0.2.3` / `v0.2.2` users can update in one click** via **About → Updates** inside `xc2-launcher.exe` (no `xc2_boot.exe` rebuild is required).
> 2. **New 1080p Native TAAU baseline (`nxrt.ini.in`)** applies automatically to **fresh setups** (`v0.2.4` preserves your existing `nxrt.ini` when upgrading). You can still apply **Reset defaults** in Launcher → Settings at any time.
> 3. **Still upgrading from `v0.2.2` or older?** Delete `shader_cache\spirv\` once after updating and click **Set up** so the `#15` `spirv_harness.exe` shader fix takes effect (`v0.2.3` users who already rebuilt `shader_cache\spirv\` do not need to wipe it).

## Which file do I download?

| file | what it is | do I need it? | SHA-256 |
|---|---|---|---|
| `XC2-package-dist-0.2.4.zip` | full player package: `0.2.4` launcher, updated `xc2_rt.dll`, `xc2_setup.py`, and toolchain | **yes** — new installs, manual upgrades, or `v0.2.2`/`v0.2.3` one-click **About → Updates** | `6a8529207f139521132c4a06184af261b4956f17bc8d41dccdc492c5f13798c1` |
| `XC2-hotfix-v0.2.4-tdr-rt.zip` | optional runtime hotfix (`xc2_rt.dll` from `010fc5e`): device-lost / `VK_EXT_device_fault` logging, descriptor `pool_set` deadlock fix, integer blit filter & UBO bounds | **optional (NVIDIA TDR #32/#41/#59 testers)** — extract over an existing `v0.2.4` install folder; does not affect launcher settings/saves | `eccd56cb71d4972f0e7d5a8ebedce37ed9c0566287c87d26f0a1e81727dd8cb6` |
| `xc2-launcher-0.2.4-src.zip` | launcher source code | no | `4d64c7a9205ca31dc89d72fc4118abd7acaecf64b1ec80cc0d947a370af5c57c` |

## What's new

### 1. Frame Generation (`FSR 3 FG`), Dynamic Resolution & F10 HUD (`#53`, `#55`, `#56`, `temporal_ffx`, `framegen` / `overlay`)

- **#53 AMD FSR 3 Frame Generation `HUDLessColor` + HDR parity (`nvn_framegen`, `framegen_ffx`)**: routed the game's HUDless scene buffer properly into FidelityFX Frame Generation (`game::Info::hudless_rt`) with matching SDR/HDR10 handling, fixing broken/green output when using FSR 3 FG.
- **FidelityFX dynamic-resolution isolation (`temporal_ffx.cpp`)**: switched the default FidelityFX SR mode cleanly to **FSR `3.1`** (`fsr_version`) and isolated dynamic-resolution context `slots_` automatically whenever **FSR `4.*`** is active, preventing input/motion-vector size mismatch issues across resolution steps.
- **#55 / #56 Dynamic resolution & viewport scaling fixes (`nvn_gpu`, `nvn_gpu_texture`, `nvn_replay`, `nvn_present`)**: bypassed XC2's internal compute upscale variants cleanly, fixed attachment scale caps, scaled HUD viewport info to output dimensions, and pinned dynamic-resolution state before `rt_rebuild`.
- **Upgraded F10 FPS overlay & overflow fix (`framegen`, `overlay.cpp`)**: when Frame Generation is active, **F10** now displays both rendered and presented frame rates (`base -> presented FPS`, e.g. `60 -> 118`) along with the active FG backend tag; also fixed an integer wrap/overflow bug in `shown fps`.

### 2. Renderer, Pipeline Cache & HDR Calibration (`#49`, `#51`, `#54`, `#57`, `#50`)

- **#49 Intel `igvk64.dll+0x79ddf3` `vkGetPipelineCacheData` crash fix (`nvn_pipeline`, `nvn_gpl`, `nvn_replay`)**: handles drivers returning empty/partial pipeline cache blobs or non-zero `VkResult` safely instead of dereferencing invalid cache data during prewarm/save.
- **#51 / #54 Depth-bias `FORCE_UNORM` regression gate (`config/keys.tsv`, `vk_device`, `nvn_replay`)**: gated `VK_EXT_depth_bias_control` (`FORCE_UNORM`) behind opt-in `[nvn] depth_bias_force_unorm` (`0` by default), resolving regressions introduced on non-RTX-40 GPUs while keeping the RTX 40-series workaround available via config.
- **#57 HDR display-referred highlight expansion (`output_pass`)**: added `OutputParams::expand_ratio` and a live slider in the F1 menu for tuning bright highlight expansion on HDR10 displays.
- **#50 F1 Overlay navigation**: isolated `##rows_pane` D-pad navigation & `ZR` save behavior (`play_ui.cpp`) and logs the active upscaler (`nvn_present.cpp`).

### 3. Media Playback, Setup & Localization Robustness (`#52`, `#58`, `#25`)

- **#52 `dav1d` software decoder & finalize fixes (`video_codec.cpp`, `nv12_convert.h`, `movie_vdec.cpp`)**: enabled `max_frame_delay = 0` with proper EOS draining, added SSE2 `i420_to_nv12` conversion, and resolved finalize thread join hangs at movie end.
- **#25 Non-ASCII / malformed UTF-16 DLC names (`services/misc.cpp`)**: safely handles unpaired UTF-16 surrogates and non-ASCII folder names inside `aoc_indices()`.
- **#58 Setup PowerShell 5.1 fallback & custom `game_dir` packaged mode (`tools/setup/doctor.ps1`, `xc2_setup.py`, `main.rs`)**: falls back cleanly to Windows PowerShell 5.1 when PowerShell 7 (`pwsh`) is unavailable, and supports custom `game_dir` layouts cleanly in packaged installs.

### 4. Launcher UX, 1080p Native Baseline & Updated Frame Generation Guidance

- **1080p Native TAAU Docked Baseline (`config/nxrt.ini.in`, `settings.rs`, `settings.js`)**: fresh setups now establish a verified baseline — **Docked mode** (`[am] operation_mode = docked`, `resolution = 1920x1080`), **1920×1080 window**, built-in **TAAU** (`[dlss] upscaler = taau`), **Native 100% render resolution** (`[upscale] render_quality = native`, `lock_render_size = 1`), **60 fps** (`[xc2] fps = 60`), **Frame Generation off** (`[dlss] frame_gen = 0`), and `[system] language = auto` (with default Japanese voice when unset, switchable in settings).
- **Upscaler download confirmation & option cleanup (`streamline.rs`, `fsr.rs`, `play.rs`, `settings.js`)**: added a confirmation prompt before downloading official NVIDIA / AMD upscaler packages, removed non-functional **Intel XeSS** options, removed the unviable native **120 Hz** option, and synced 30/60 Hz selection cleanly across Launcher & F1 Settings.
- **All-GPU Frame Generation notice & visual correctness hint**: updated UI copy so users on **NVIDIA, AMD, and Intel** GPUs know Frame Generation is available via **DLSS Frame Generation** or **AMD FSR 3 Frame Generation**, paired with an explicit warning that enabling Frame Generation can degrade **image correctness** (such as subtle mouth/eye/dialogue flicker in cutscenes, which disappears immediately when Frame Generation is disabled).

### 5. Optional Hotfix (`XC2-hotfix-v0.2.4-tdr-rt.zip`, `010fc5e`) — NVIDIA TDR Mitigation & Diagnostics

Added alongside the `v0.2.4` release assets specifically for users encountering NVIDIA GPU freezes / TDR (`#32`, `#41`, `#59`) — **the main `XC2-package-dist-0.2.4.zip` is unchanged, and Launcher auto-update (`About → Updates`) is unaffected**:
- **Shared `device_lost` guard & `VK_EXT_device_fault` watchdog reporting (`vk_device`, `vk_renderer`, `nvn_gpu`)**: stops `UINT64_MAX` (`-4`) timeline poison from prematurely destroying live GPU objects, breaks infinite fence/acquire/semaphore waits once lost, and records hardware fault details directly into `xc2_boot_freeze.txt`.
- **Descriptor `pool_set()` same-submission & deadlock fix (`nvn_pipeline`)**: prevents overwriting descriptor sets already bound in an open recording and eliminates an infinite spin waiting on unsubmitted `f.serial`.
- **Integer blit filter & UBO bounds fixes (`nvn_gpu_texture`, `nvn_replay`)**: enforces `VK_FILTER_NEAREST` for integer/depth blits (`VUID-vkCmdBlitImage-filter-02001`) and clamps unaligned UBO copies to `br.avail`.

## Upgrading to v0.2.4

- **From `v0.2.3` / `v0.2.2`**:
  1. Open `xc2-launcher.exe` → **About → Check now** → **Install update** (or extract `XC2-package-dist-0.2.4.zip` over your existing install folder). `xc2_boot.exe` does **not** need rebuilding.
  2. *(Only if updating from `v0.2.2` or older and you skipped `v0.2.3`)*: delete `shader_cache\spirv\` inside your install folder and run **Set up** once to re-translate shaders for `#15`.
- **New installs**:
  - Extract `XC2-package-dist-0.2.4.zip` into a clean folder, launch `xc2-launcher.exe`, select your own 3.0.1 dump, and click **Set up**.
- **Applying the optional TDR hotfix (`XC2-hotfix-v0.2.4-tdr-rt.zip`, for `v0.2.4` users hitting `#32`/`#41`/`#59`)**:
  1. Close the game and `xc2-launcher.exe`.
  2. Extract `XC2-hotfix-v0.2.4-tdr-rt.zip` directly into your existing **`v0.2.4` install folder** (the folder containing `xc2-launcher.exe`), choosing **Overwrite / Replace files** when prompted (this updates both `xc2_rt.dll` and `runtime\xc2_rt.dll`).
  3. *(Custom `game_dir` users only)*: also copy the extracted `xc2_rt.dll` over the copy next to `xc2_boot.exe` inside your `game_dir`.
  4. Launch the game normally — no rebuild or **Set up** is needed, and your `nxrt.ini`, saves, and `shader_cache` remain untouched. If a freeze still occurs on NVIDIA, please attach the newly generated `xc2_boot_freeze.txt` + `nxrt.log` on the issue tracker.

## Known issues

- **Lv. 130 Tyrannotitan Kurodil freeze on NVIDIA ([#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32))**
- **Minor cloud-sea visual bugs**: a very small number of visual artifacts remain in parts of the cloud sea (such as localized thin edge lines); they do **not** affect normal gameplay or progression.
- **Frame Generation side effects**: because frame interpolation relies on estimated motion vectors, enabling Frame Generation may cause subtle mouth/eye flicker in certain dialogue or cutscene shots (turning Frame Generation off restores clean output).
- **Add-on heavy scenes & first-run pipeline compile**: Torna / add-on scenes remain heavier than base game areas; first launch per GPU driver compiles Vulkan pipelines (~20 s pause).
- **Supported platforms**: the primary official target covered by release testing is native **Windows 11 x64**. **Windows 10 x64** and **Linux / SteamOS (Proton / Wine)** currently receive **best-effort compatibility** (e.g. PowerShell 5.1 fallback, `<CETCompat>false</CETCompat>`, SEH guards, Proton mount helpers) without guaranteed full regression support; community PRs and forks adapting other platforms are welcome.

---

# XC2 NS2 重编译 v0.2.4 —— FSR 3 帧生成与动态分辨率修复、Intel 管线缓存、安装兼容与 1080p 原生基准预设

本版本合入 `v0.2.3` 之后的全部集成修复与体验优化：修复 **#53 AMD FSR 3 帧生成（`HUDLessColor`）与 HDR10 兼容**、默认切换 **FSR `3.1` 并在启用 `FSR 4.*` 时自动隔离动态分辨率上下文**、修复 **#55/#56 动态分辨率与 HUD 视口缩放回归**、升级 **F10 双帧率监控与计数溢出修复**、修复 **#49 Intel `vkGetPipelineCacheData` 崩溃**、将 **#51/#54 深度偏移 `FORCE_UNORM` 改为可选项**、新增 **#57 HDR 高光扩展滑块**、改进 **#52 `dav1d` 软解与结束同步**、修复 **#25 非 ASCII DLC 路径崩溃**、完善 **#58 PowerShell 5.1 回退与自定义 `game_dir` 打包模式**、清理 **XeSS / 原生 120 Hz 选项并加入超分下载确认弹窗**，同时确立 **1080p 原生 TAAU 主机模式基准默认配置**。

> **升级必读（尤其是从 `v0.2.3` / `v0.2.2` 在线更新的用户）**：
> 1. **`v0.2.3` / `v0.2.2` 用户可直接在启动器「关于 → 检查更新 → 安装更新」一键升级**，无需重新编译 `xc2_boot.exe`。
> 2. **新的 1080p 原生 TAAU 基准预设**仅作用于新初始化的默认 `nxrt.ini`（在线升级会保留你原有的 `nxrt.ini` 配置）；如需一键采用新基准，可在启动器「设置」页点击恢复默认。
> 3. **若你从 `v0.2.2` 或更早版本跳过 `v0.2.3` 直接升级**：请在更新后删一次 `shader_cache\spirv\` 并在启动器点击 **Set up** 重译着色器，以应用 `#15` 闪烁修复（已升级过 `v0.2.3` 并重译过的用户无需删除）。

## 我该下哪个文件？

| 文件 | 是什么 | 需要吗 | SHA-256 |
|---|---|---|---|
| `XC2-package-dist-0.2.4.zip` | 完整玩家包：`0.2.4` 启动器、新版 `xc2_rt.dll`、`xc2_setup.py` 与完整工具链 | **要** —— 全新安装、手动覆盖升级，或 `v0.2.2`/`v0.2.3` 启动器内一键更新 | `6a8529207f139521132c4a06184af261b4956f17bc8d41dccdc492c5f13798c1` |
| `XC2-hotfix-v0.2.4-tdr-rt.zip` | 可选运行时热修复补丁（`010fc5e` `xc2_rt.dll`）：`device-lost` / `VK_EXT_device_fault` 诊断、描述符 `pool_set` 死锁修复、整数 blit 滤镜与 UBO 边界钳位 | **可选（针对 NVIDIA TDR `#32`/`#41`/`#59` 复测）** —— 直接覆盖解压到 `v0.2.4` 安装目录；不影响配置、存档与着色器缓存 | `eccd56cb71d4972f0e7d5a8ebedce37ed9c0566287c87d26f0a1e81727dd8cb6` |
| `xc2-launcher-0.2.4-src.zip` | 启动器源码 | 不用 | `4d64c7a9205ca31dc89d72fc4118abd7acaecf64b1ec80cc0d947a370af5c57c` |

## 本版变化

### 1. 帧生成（`FSR 3 FG`）、动态分辨率与 F10 监控增强（`#53`、`#55`、`#56`、`temporal_ffx`、`framegen` / `overlay`）

- **修复 #53 AMD FSR 3 帧生成 `HUDLessColor` 与 HDR10 输出（`nvn_framegen`、`framegen_ffx`）**：补齐无 UI 场景缓冲（`game::Info::hudless_rt`）路由并统一 SDR/HDR10 处理，解决开启 FSR 3 帧生成后绿屏或异常问题。
- **FidelityFX 动态分辨率上下文隔离（`temporal_ffx.cpp`）**：默认超分版本设为 **FSR `3.1`**；当选用 **FSR `4.*`** 时自动按动态分辨率挡位隔离 `slots_` 上下文，避免分辨率波动时尺寸不匹配崩溃。
- **修复 #55 / #56 动态分辨率与 HUD 视口缩放回归（`nvn_gpu`、`nvn_gpu_texture`、`nvn_replay`、`nvn_present`）**：绕过游戏内部计算超分变体、修正渲染目标缩放上限、正确缩放 HUD 视口信息，并在 `rt_rebuild` 前锁定当前动态分辨率状态。
- **F10 帧率显示升级与计数溢出修复（`framegen`、`overlay.cpp`）**：开启帧生成时，**F10** 现同屏显示原始渲染帧率与生成后帧率（例如 `60 -> 118`）及当前帧生成后端标识；同步修复了 `shown fps` 整数回绕溢出导致显示异常的问题。

### 2. 渲染管线、缓存与 HDR 调校（`#49`、`#51`、`#54`、`#57`、`#50`）

- **修复 #49 Intel `igvk64.dll+0x79ddf3` `vkGetPipelineCacheData` 崩溃（`nvn_pipeline`、`nvn_gpl`、`nvn_replay`）**：妥善处理驱动返回空缓存或非零 `VkResult` 的情况，避免预热或保存管线缓存时闪退。
- **#51 / #54 深度偏移 `FORCE_UNORM` 改为可选开关（`config/keys.tsv`、`vk_device`、`nvn_replay`）**：将 `VK_EXT_depth_bias_control`（`FORCE_UNORM`）收纳为默认关闭的 `[nvn] depth_bias_force_unorm` 选项，修复非 RTX 40 系显卡上的阴影异常回归，同时保留针对 RTX 40 系的按需修复开关。
- **新增 #57 HDR 高光扩展调节（`output_pass`）**：加入 `OutputParams::expand_ratio` 与 F1 实时菜单滑块，改善 HDR10 下高亮镜面高光动态范围。
- **优化 #50 F1 菜单交互**：隔离 `##rows_pane` 十字键导航与 `ZR` 保存行为（`play_ui.cpp`），并在日志中输出当前实际生效的超分后端（`nvn_present.cpp`）。

### 3. 影片解码、安装与路径兼容加固（`#52`、`#58`、`#25`）

- **#52 `dav1d` 软解与退出等待修复（`video_codec.cpp`、`nv12_convert.h`、`movie_vdec.cpp`）**：启用 `max_frame_delay = 0` 并完善流末尾清空（EOS drain），加入 SSE2 `i420_to_nv12` 转换，修复影片结束线程等待卡死。
- **#25 非 ASCII / 异常 UTF-16 DLC 目录名兼容（`services/misc.cpp`）**：`aoc_indices()` 现可安全跳过或处理含非 ASCII 与未配对 UTF-16 字符的目录。
- **#58 安装步骤 PowerShell 5.1 回退与自定义 `game_dir` 兼容（`tools/setup/doctor.ps1`、`xc2_setup.py`、`main.rs`）**：系统未安装 PowerShell 7（`pwsh`）时自动回退至自带 PowerShell 5.1，并修复打包模式下自定义 `game_dir` 的路径处理。

### 4. 启动器体验、1080p 原生基准默认设置与帧生成提示完善

- **确立 1080p 原生 TAAU 主机模式基准配置（`config/nxrt.ini.in`、`settings.rs`、`settings.js`）**：新初始化默认即采用稳定可复现的基准档位——**主机模式（Docked）**、`1920×1080` 分辨率与窗口、内建 **TAAU**、**100% 原生分辨率渲染**并 **锁定渲染分辨率**（`render_quality = native`、`lock_render_size = 1`）、**60 fps**、**关闭帧生成**（`frame_gen = 0`）、文本语言 **`auto`**（未单独指定时语音默认日语，可在设置中切换英语）。
- **超分下载确认弹窗与无效选项清理（`streamline.rs`、`fsr.rs`、`play.rs`、`settings.js`）**：下载 NVIDIA / AMD 官方超分运行库前新增确认弹窗；移除暂不可用的 **Intel XeSS** 选项与不成立的 **原生 120 Hz** 选项，并保持启动器与 F1 菜单的 30/60 Hz 同步。
- **完善全显卡帧生成说明与画面正确性提示**：在设置文案中明确 **NVIDIA / AMD / Intel 显卡均可通过 DLSS 帧生成或 AMD FSR 3 帧生成使用插帧**（不再局限于 RTX 40 系列），并明确提醒：**开启帧生成依赖运动矢量插帧，可能会影响部分画面正确性（例如人物对白/过场中嘴唇或眼部的轻微闪动，关闭帧生成即可恢复）。**

### 5. 可选热修复补丁（`XC2-hotfix-v0.2.4-tdr-rt.zip`，基于 `010fc5e`）—— NVIDIA TDR 缓解与故障诊断

针对遭遇 NVIDIA 显卡冻屏 / `VK_ERROR_DEVICE_LOST` TDR（`#32`、`#41`、`#59`）的玩家在 `v0.2.4` 资源旁额外提供（**不改动 `XC2-package-dist-0.2.4.zip` 主包，也不影响启动器「关于 → 检查更新」**）：
- **全局 `device_lost` 断链保护与 `VK_EXT_device_fault` 冻结采集（`vk_device`、`vk_renderer`、`nvn_gpu`）**：阻止 `UINT64_MAX`（`-4`）信号量污染 `g->completed` 误销毁活资源，在丢设备后中断无限 fence/acquire 等待，并在 `xc2_boot_freeze.txt` 中直接输出硬件故障描述与地址。
- **修复 `pool_set()` 同提交描述符覆写与自死锁（`nvn_pipeline`）**：禁止在未提交录制内篡改已绑定描述符集，消除全部副本处于开放提交 `f.serial` 时的永久自旋死锁。
- **修复整数格式线性 Blit 与 UBO 边界（`nvn_gpu_texture`、`nvn_replay`）**：强制整数与深度图像 Blit 使用 `VK_FILTER_NEAREST`（清零 `VUID-vkCmdBlitImage-filter-02001`），并对非对齐 UBO 拷贝补齐 `br.avail` 上界钳位。

## 如何升级到 v0.2.4

- **当前已在 `v0.2.3` / `v0.2.2`**：
  1. 在 `xc2-launcher.exe` 点击 **「关于 → 检查更新 → 安装更新」** 一键完成在线更新（无需重编 `xc2_boot.exe`）。
  2. *(仅当从 `v0.2.2` 或更早版本跳过 `v0.2.3` 直接升级时)*：删一次安装目录下的 `shader_cache\spirv\`，并在启动器点一次 **Set up** 重译着色器以生效 `#15` 修复。
- **全新安装**：
  - 解压 `XC2-package-dist-0.2.4.zip` 至干净文件夹，打开 `xc2-launcher.exe`，指定你自己的 3.0.1 游戏副本并点击 **Set up**。
- **如何安装可选 TDR 热修复补丁（`XC2-hotfix-v0.2.4-tdr-rt.zip`，供 `v0.2.4` 上复现 `#32`/`#41`/`#59` 的用户覆盖）**：
  1. 先完全关闭游戏和 `xc2-launcher.exe`。
  2. 将 `XC2-hotfix-v0.2.4-tdr-rt.zip` **直接解压到你现有的 `v0.2.4` 安装根目录**（即 `xc2-launcher.exe` 所在目录），提示文件冲突时选择 **覆盖 / 替换**（会自动同步更新根目录 `xc2_rt.dll` 与 `runtime\xc2_rt.dll`）。
  3. *(仅当在启动器使用了自定义 `game_dir` 时)*：请再手动把解压出的 `xc2_rt.dll` 覆盖复制到你的自定义 `game_dir` 内 `xc2_boot.exe` 同级目录。
  4. 直接打开启动器运行游戏即可——**无需重新编译、无需重新点击 Set up**，原有的 `nxrt.ini`、存档与 `shader_cache` 均保留不变；若在 N 卡上仍遇冻屏，请附上打补丁后新生成的 `xc2_boot_freeze.txt` 与 `nxrt.log`。

## 已知问题

- **130 级暴虐巨神兽库洛戴伊尔 NVIDIA 画面冻结 / TDR（[#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32)）**
- **云海部分仍有极少量视觉 BUG**（如局部边缘细线等，**不影响正常游戏流程与游玩体验**）。
- **帧生成插帧副作用**：开启帧生成后，极少数过场/对白镜头中人物嘴唇或眼部可能出现轻微闪动；关闭帧生成即可彻底消除。
- **附加内容重场景负载较高**，以及更换显卡驱动后首次启动约 20 秒管线编译提示。
- **支持平台**：目前以原生 **Windows 11 x64** 作为主要测试与官方支持目标；**Windows 10 x64** 与 **Linux / SteamOS（Proton / Wine）** 仅提供尽力适配（Best-effort，例如 PowerShell 5.1 回退、`<CETCompat>false</CETCompat>`、SEH 防护与 Proton 路径支持等），暂不保证完整回归测试，欢迎社区提交 PR 或自行 Fork 适配。

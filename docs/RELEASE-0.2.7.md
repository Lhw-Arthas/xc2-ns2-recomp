# XC2 NS2 Recomp v0.2.7 — Lip-Flicker & Cutscene Fix, Raw Dump/DLC Auto-Detection, Scene-Load Stutter Elimination & Post-TDR Memory Fix

This release brings together a major round of stability, visual, mod, and smoothness improvements verified since `v0.2.6`: **facial morph / lip-flicker fix with zero cutscene freezes ([#84](https://github.com/weile10000/xc2-ns2-recomp/pull/84), by @UrsineBear)**, **automatic detection of un-trimmed Switch 2 dumps & nested DLCs (zero manual folder sorting)**, **asynchronous shader patching/module creation & `uint_samplers` prewarm (eliminating ~1 s scene-load hitches)**, **post-device-ready Vulkan Video prewarm (eliminating first-cutscene cold-start hitches)**, **#81/#41 post-`VK_ERROR_DEVICE_LOST` runaway RAM fix**, **#87/#63 dual-GPU laptop Vulkan Video fix**, **#71 Atmosphère DLC & loose `romfs/` automatic mod overrides**, **#83 runtime-upgrade shader prefill fix**, and **#76/#78/#79/#80 16x AF, auto upscaler mip bias, and input polish**.

> **Upgrading from `v0.2.6` (or `v0.2.2+`)? Read this!**
> 1. **One-click update**: open `xc2-launcher.exe` → **About → Check now → Install update** (or extract `XC2-package-dist-0.2.7.zip` over your existing install folder).
> 2. **First Set up after upgrading**: the launcher will relink `xc2_boot.exe` once for the updated `v0.2.7` runtime and refresh the shader prefill marker (`#83` fixed!). Your `save/`, `nxrt.ini`, `controls.ini`, `mods/`, and translated `shader_cache/` are all preserved.
> 3. **Already have an un-trimmed raw 3.0.1 dump?** You no longer need to delete NS1 folders or manually move `0100e95004039062` / nested DLC `romfs` folders — just point the launcher at your raw dump folder.

## Which file do I download?

| file | what it is | do I need it? |
|---|---|---|
| `XC2-package-dist-0.2.7.zip` | full player package: `0.2.7` launcher, updated `xc2_rt.dll`, `xc2_setup.py`, and full toolchain | **yes** — new installs, manual upgrades, or `v0.2.2+` one-click **About → Updates** |
| `xc2-launcher-0.2.7-src.zip` | launcher source code | no |

*(Exact SHA-256 checksums are in `SHA256SUMS.txt` on the release page.)*

## What's new

### 1. Facial Morph / Lip-Flicker Fix Without Cutscene Freezes ([#84](https://github.com/weile10000/xc2-ns2-recomp/pull/84), by @UrsineBear)

- **Re-enabled `[nvn] mirror_gfx_subpage` by default**: replaces the earlier `memcmp` sub-page path (which froze movie playback) with lock-free per-vertex VB boundary write-back (`divisor == 0`), excluding instanced particle streams and active CPU-written interiors.
- **Verified**: lip/eyebrow flicker in close-ups (including with Frame Generation ON) is completely gone, and pre-rendered cutscenes play cleanly without freezing.

### 2. Scene-Load & Cutscene Stutter Elimination (`01a9fa6`, `7a24fe8`, `3aae40e`)

- **Asynchronous SPIR-V patching & `vkCreateShaderModule` (`nvn_gpl.cpp`)**: offloads uncached `resolve_stage_modules()` work onto `gpl_async` background worker threads and widens the `frames_flowing()` window (`100 ms → 1500 ms`) so a heavy scene-load upload frame no longer cascades into synchronous render-thread shader compiles.
- **`uint_samplers` prewarm persistence (`nvn_prewarm.cpp`, `NXPKEYS2`)**: player-local `pipeline_keys.bin` now records and prewarms stencil/integer-sampler patched pipelines (`uint_tex_patch`), cutting draw-time fragment part compilation by >99% on measured runs.
- **Background Vulkan Video prewarm (`vk_device.cpp`, `video_ffmpeg.cpp`)**: asynchronously prewarms FFmpeg's hardware video device right after windowed `gfx::Device::create()` finishes, removing the ~100 ms cold-start hitch on the first cutscene without racing driver init (`#42`).
- **Hot-path cleanup (`nvn_gpu_texture.cpp`, `hid.cpp`)**: removed the legacy `4k target` / `film texture` hot-path mutex+logging probe and downgraded periodic normal pad-state logs to debug.

### 3. Raw Un-Trimmed Dump & Nested DLC Auto-Detection (`cf651d9`)

- **Zero manual folder sorting (`dump.rs`, `xc2_setup.py`, `settings.rs`)**:
  - Automatically detects the Switch 2 Edition 3.0.1 Upgrade Pack (`0100e95004039062`) full ~24 GB `romfs` (even when NS1 `exefs`/`romfs` are still present) and excludes `0100e95004039062` from add-on mounting.
  - Recursively locates nested DLC `romfs` folders (both under `<root>/dlc/<id>/...` and directly under `<root>/<id>/...`) and creates per-DLC directory junctions under `<install>/game/dlc-301/<id>/romfs` — keeping your original dump 100% read-only with zero file copies.

### 4. Stability, Dual-GPU Cutscene Decoding & Setup Fixes ([#81](https://github.com/weile10000/xc2-ns2-recomp/issues/81), [#41](https://github.com/weile10000/xc2-ns2-recomp/issues/41), [#87](https://github.com/weile10000/xc2-ns2-recomp/issues/87), [#63](https://github.com/weile10000/xc2-ns2-recomp/issues/63), [#83](https://github.com/weile10000/xc2-ns2-recomp/issues/83))

- **#81 / #41 Post-`VK_ERROR_DEVICE_LOST` runaway RAM fix (`nvn_gpu`, `nvn_gpu_pool`, `nvn_texprep`)**: drains deferred destroys, frees cached arenas, and stops command recording / descriptor-pool growth immediately once `device_lost` is latched, preventing the 24 GB `MEMORY LIMIT EXCEEDED` crash cascade after a GPU disconnect.
- **#87 / #63 Dual-GPU laptop cutscene crash fix (`video_ffmpeg`, `vk_device`)**: explicitly binds FFmpeg Vulkan Video to the active render GPU index instead of defaulting to GPU 0, and routes Intel GPUs to D3D11VA under `auto` to avoid `igvkMedia64.dll` crashes on Optimus laptops.
- **#83 Runtime-upgrade shader setup fix (`xc2_setup.py`)**: checks the real `build/prefilled` marker after a runtime upgrade instead of `build/prefilled.stale`.
- **Diagnostic report improvements (`tools/report.py`)**: avoids false 60 s stall kills, includes `controls.ini`, and emits quick triage flags.

### 5. Mods, Image Clarity & Input Polish ([#71](https://github.com/weile10000/xc2-ns2-recomp/issues/71), [#76](https://github.com/weile10000/xc2-ns2-recomp/issues/76), [#78](https://github.com/weile10000/xc2-ns2-recomp/issues/78), [#79](https://github.com/weile10000/xc2-ns2-recomp/issues/79), [#80](https://github.com/weile10000/xc2-ns2-recomp/issues/80))

- **#71 Mod loading overhaul**: supports Atmosphère DLC layouts, fixes `.ard/.rec` subdirectory matching via `bf2.arh` trie decoding, and automatically overrides `bf2.ard` entries from loose `romfs/` files so multiple NS2-compatible mods no longer clash over `bf2.arh`.
- **#79 Sharper textures by default**: defaults anisotropic filtering to **16x AF** and applies automatic negative mip LOD bias when temporal upscaling is active.
- **#76 / #78 / #80 Input improvements**: adds live mouse-capture toggle, stick outer-range calibration, and missing keyboard/mouse D-pad bindings in F1.

## Upgrading to v0.2.7

- **From `v0.2.2` – `v0.2.6`**: open `xc2-launcher.exe` → **About → Check now → Install update** (or extract `XC2-package-dist-0.2.7.zip` over your install folder), then click **Set up** once. Saves, settings, mods, and translated shaders are kept.
- **New installs**: extract `XC2-package-dist-0.2.7.zip` into a clean folder, launch `xc2-launcher.exe`, select your own 3.0.1 dump (trimmed or raw), and click **Set up**.

## Known issues

- **NVIDIA DLSS black lines ([#74](https://github.com/weile10000/xc2-ns2-recomp/issues/74))**: caused by the **NVIDIA app's DLSS model override** (models **M** and **L**), not this build. Clear the DLSS model override for this game to restore clean output.
- **NVIDIA `VK_ERROR_DEVICE_LOST` in certain scenes/combat ([#41](https://github.com/weile10000/xc2-ns2-recomp/issues/41), [#81](https://github.com/weile10000/xc2-ns2-recomp/issues/81), [#89](https://github.com/weile10000/xc2-ns2-recomp/issues/89))**: `v0.2.7` stops the secondary RAM explosion after device loss, while the underlying NVIDIA driver TDR is still under investigation (local test hardware is AMD RX 6800 XT).
- **Above-60 fps (90 / 120 fps) mode is experimental**: may affect certain UI/QTE animation cadence ([#85](https://github.com/weile10000/xc2-ns2-recomp/issues/85)). For daily play we recommend **60 fps + Frame Generation to 120 fps**.
- **Minor cloud-sea edge artifacts**: a few localized thin edge lines remain in parts of the cloud sea (cosmetic only).
- **Supported platforms**: primary target is native **Windows 11 x64**; **Windows 10 x64** and **Linux / SteamOS (Proton / Wine)** receive best-effort compatibility.

## Credits & Legal Notice

- **Contributors in this release**: **[@UrsineBear](https://github.com/UrsineBear)** (PR [#84](https://github.com/weile10000/xc2-ns2-recomp/pull/84) facial morph / sub-page fix), plus everyone who submitted logs, diagnostics, and feedback in the issue tracker and community.
- **Upstream & References**: **[nxrecomp](https://github.com/drakolordx7/nxrecomp)** (ARM64 → C++ static recompiler), `Goldoire/fefw-recomp` *(upstream removed; vendored fork preserved in `third_party/fefw-recomp/`)*, **Ryujinx** (MIT, Maxwell shader translator), **[@capibawa](https://github.com/capibawa)** (`nxnative`), **[AksharDP/xc2-recomp](https://github.com/AksharDP/xc2-recomp)**, and **[Lichii](https://github.com/zortos293/Lichii)**. Third-party libraries (SDL3, Vulkan-Headers, VMA, dav1d, FFmpeg, Dear ImGui, AMD FidelityFX, NVIDIA Streamline) retain their respective licences (see `licenses/` and `THIRD_PARTY_NOTICES.md`).
- **Licence**: Project code is licensed under **GPL-3.0-or-later**. **No licence here grants any rights to Nintendo's copyrighted game code or assets; lifted user binaries remain the property of their respective rights holders.**
- **Legal Notice**: Independent, non-commercial fan project. Not affiliated with, sponsored by, or endorsed by Nintendo or MONOLITH SOFT. *Xenoblade Chronicles*, *Xenoblade Chronicles 2*, *Nintendo Switch*, and *Nintendo Switch 2* are trademarks of Nintendo.

---

# XC2 NS2 重编译 v0.2.7 —— 嘴唇闪烁与播片修复、原始 Dump/DLC 自动识别、切场景卡顿消除与丢设备内存修复

本版本汇总了 `v0.2.6` 之后全部经实测验证的稳定性、画质、Mod 与流畅度改进：**修复面部形变/嘴唇闪烁且播片不冻结（[#84](https://github.com/weile10000/xc2-ns2-recomp/pull/84)，by @UrsineBear）**、**自动识别未精简的 Switch 2 原始 Dump 与多层嵌套 DLC（无需玩家手工移动/删文件）**、**SPIR-V 补丁与着色器模块异步下沉 + `uint_samplers` 本地预热（大幅消除切场景近 1 秒级卡顿）**、**设备就绪后后台异步预热 Vulkan Video（消除首段播片冷启动卡顿）**、**修复 #81/#41 丢设备后内存暴涨崩溃**、**修复 #87/#63 双显卡笔记本过场硬解崩溃**、**支持 #71 Atmosphère DLC 与松散 `romfs/` 自动覆盖 Mod**、**修复 #83 升级运行时后着色器预热报错**，以及 **#76/#78/#79/#80 默认 16x 各向异性过滤、自动超分 mip 偏移与键鼠/摇杆打磨**。

> **从 `v0.2.6`（或 `v0.2.2+`）升级必读**：
> 1. **一键在线更新**：打开 `xc2-launcher.exe` → **「关于 → 检查更新 → 安装更新」**（或下载 `XC2-package-dist-0.2.7.zip` 解压覆盖原目录）。
> 2. **升级后首次「开始构建」**：启动器会为新版运行时自动重链一次 `xc2_boot.exe` 并刷新预热标记（`#83` 已修）。你的 `save/`、`nxrt.ini`、`controls.ini`、`mods/` 和已翻译的 `shader_cache/` 都会保留。
> 3. **使用未精简原始 3.0.1 Dump 的玩家**：现在不再需要手动删除 NS1 文件或搬运 `0100e95004039062` / 嵌套 DLC 的 `romfs`，直接选择原始 Dump 根目录即可。

## 我该下哪个文件？

| 文件 | 是什么 | 需要吗 |
|---|---|---|
| `XC2-package-dist-0.2.7.zip` | 完整玩家包：`0.2.7` 启动器、新版 `xc2_rt.dll`、`xc2_setup.py` 与完整工具链 | **要** —— 全新安装、手动覆盖升级，或 `v0.2.2+` 启动器内一键更新 |
| `xc2-launcher-0.2.7-src.zip` | 启动器源码 | 不用 |

*（精确 SHA-256 校验值见发布页的 `SHA256SUMS.txt`。）*

## 本版变化

### 1. 彻底修复嘴唇/面部形变闪烁且播片不冻结（[#84](https://github.com/weile10000/xc2-ns2-recomp/pull/84)，by @UrsineBear）

- **默认重新开启 `[nvn] mirror_gfx_subpage`**：用无锁逐顶点顶点缓冲（`divisor == 0`）边界精准回写替代旧版 `memcmp` 方案，自动排除实例化粒子流与活跃 CPU 写入区域。
- **实测验证**：开启帧生成下特写镜头嘴唇/眉毛闪烁彻底消失，预渲染过场播片流畅不冻结。

### 2. 切场景与播片普惠卡顿优化（`01a9fa6`、`7a24fe8`、`3aae40e`）

- **SPIR-V 补丁与着色器模块创建全面异步化（`nvn_gpl.cpp`）**：将未缓存的 `resolve_stage_modules()` 下沉至 `gpl_async` 后台工作线程并行完成，并将 `frames_flowing()` 判定窗口从 `100 ms` 放宽至 `1500 ms`，避免切场景大纹理上传后引发渲染线程同步编译雪崩。
- **`uint_samplers` 纳入本地预热缓存（`nvn_prewarm.cpp`，`NXPKEYS2`）**：玩家本地 `pipeline_keys.bin` 现完整记录并预热模板/整数采样补丁管线，实测绘制期片元同步编译耗时降幅超 99%。
- **Vulkan Video 后台异步预热（`vk_device.cpp`、`video_ffmpeg.cpp`）**：在窗口模式图形设备就绪后自动在后台线程预热 FFmpeg 硬件解码设备，彻底消除首段播片 ~100 ms 的冷启动卡顿（同时避开 `#42` 启动期驱动竞态）。
- **热路径清理（`nvn_gpu_texture.cpp`、`hid.cpp`）**：移除遗留的 `4k target` / `film texture` 热路径锁+统计探针，并将手柄正常周期日志降级为 Debug。

### 3. 未精简原始 Dump 与多层嵌套 DLC 自动识别（`cf651d9`）

- **全程只读、零拷贝、免手工整理（`dump.rs`、`xc2_setup.py`、`settings.rs`）**：
  - 自动识别升级包 `0100e95004039062` 内的完整 24 GB NS2 3.0.1 `romfs`（即使原目录保留了 NS1 的 `exefs`/`romfs` 也不会误选），并自动排除 `0100e95004039062` 不再作为普通 DLC 挂载。
  - 同时扫描 `<root>/dlc/<id>` 与 `<root>/<id>`，递归定位任意深度的嵌套 DLC `romfs`（黄金国 + 扩展包 + 日配），在 `<install>/game/dlc-301/<id>/romfs` 按项建立目录软链（Junction），不改动玩家原始文件、不占额外磁盘空间。

### 4. 稳定性、双显卡笔记本播片崩溃与升级流程修复（[#81](https://github.com/weile10000/xc2-ns2-recomp/issues/81)、[#41](https://github.com/weile10000/xc2-ns2-recomp/issues/41)、[#87](https://github.com/weile10000/xc2-ns2-recomp/issues/87)、[#63](https://github.com/weile10000/xc2-ns2-recomp/issues/63)、[#83](https://github.com/weile10000/xc2-ns2-recomp/issues/83)）

- **修复 #81 / #41 丢设备后内存暴涨误报 `MEMORY LIMIT EXCEEDED`**：`VK_ERROR_DEVICE_LOST` 发生后立即清空延迟销毁队列、释放缓存内存并停止继续录制与扩张描述符池，杜绝黑屏后内存一路飙升至 24 GB 撞墙崩溃。
- **修复 #87 / #63 双显卡笔记本过场硬解崩溃**：Vulkan Video 显式绑定当前渲染 GPU 序号（不再默认落到 GPU 0 核显），Intel 渲染 GPU 在 `auto` 下自动走 D3D11VA，避免 `igvkMedia64.dll` 崩溃。
- **修复 #83 升级运行时后着色器预热失败**：正确校验真实 `build/prefilled` 标记而非 `build/prefilled.stale`。
- **诊断报告改进（`tools/report.py`）**：修复 60 秒误判卡死、附带 `controls.ini` 并输出快速分诊标记。

### 5. Mod 加载增强、画质锐度与输入细节（[#71](https://github.com/weile10000/xc2-ns2-recomp/issues/71)、[#76](https://github.com/weile10000/xc2-ns2-recomp/issues/76)、[#78](https://github.com/weile10000/xc2-ns2-recomp/issues/78)、[#79](https://github.com/weile10000/xc2-ns2-recomp/issues/79)、[#80](https://github.com/weile10000/xc2-ns2-recomp/issues/80)）

- **#71 Mod 加载大幅增强**：支持 Atmosphère DLC 目录结构、修复 `.ard/.rec` 子目录路径匹配，并支持松散 `romfs/` 文件自动覆盖 `bf2.ard`，多个 NS2 兼容 Mod 不再因争夺 `bf2.arh` 而冲突。
- **#79 默认画质更清晰**：默认开启 **16x 各向异性过滤**，并在开启时域超分时自动应用负 mip 偏移保持贴图清晰度。
- **#76 / #78 / #80 输入打磨**：新增实时鼠标捕获开关、摇杆外圈校准与 F1 键鼠十字键补全。

## 如何升级到 v0.2.7

- **从 `v0.2.2` – `v0.2.6` 升级**：打开 `xc2-launcher.exe` → **「关于 → 检查更新 → 安装更新」**（或解压 `XC2-package-dist-0.2.7.zip` 覆盖原目录），再点一次 **「开始构建」** 即可。存档、配置、Mod 与着色器缓存全部保留。
- **全新安装**：解压 `XC2-package-dist-0.2.7.zip` 到干净文件夹，打开 `xc2-launcher.exe`，指定你自己的 3.0.1 游戏 Dump（已精简或未精简原始文件夹均可），点击 **「开始构建」**。

## 已知问题

- **N 卡 DLSS 黑线（[#74](https://github.com/weile10000/xc2-ns2-recomp/issues/74)）**：由 **NVIDIA app 的 DLSS 模型覆盖（模型 M / L）** 引起，并非本版 Bug；清除该游戏的 DLSS 模型覆盖即可恢复干净画面。
- **部分场景/战斗中 N 卡偶发 `VK_ERROR_DEVICE_LOST`（[#41](https://github.com/weile10000/xc2-ns2-recomp/issues/41)、[#81](https://github.com/weile10000/xc2-ns2-recomp/issues/81)、[#89](https://github.com/weile10000/xc2-ns2-recomp/issues/89)）**：`v0.2.7` 已根治丢设备后的次生内存暴涨，底层 N 卡驱动 TDR 仍在持续排查（本地测试硬件为 AMD RX 6800 XT）。
- **高于 60 帧模式（90 / 120 fps）属实验性质**：可能与部分 UI / QTE 步进存在耦合（[#85](https://github.com/weile10000/xc2-ns2-recomp/issues/85)）；日常游玩推荐 **60 帧模式 + 帧生成到 120 帧**。
- **云海局部仍有极少量细边缘瑕疵**（纯视觉，不影响游玩）。
- **支持平台**：主要官方测试目标为原生 **Windows 11 x64**；**Windows 10 x64** 与 **Linux / SteamOS（Proton / Wine）** 为尽力适配（Best-effort）。

## 致谢与法律声明

- **本版贡献者**：感谢 **[@UrsineBear](https://github.com/UrsineBear)**（PR [#84](https://github.com/weile10000/xc2-ns2-recomp/pull/84) 面部形变与子页回写修复），以及在 Issue 区和社区积极提交日志、诊断报告与反馈的每一位玩家。
- **上游与参考致谢**：**[nxrecomp](https://github.com/drakolordx7/nxrecomp)**（静态 ARM64 → C++ 重编译器）、`Goldoire/fefw-recomp` *（原上游已下架，独立分支留存于 `third_party/fefw-recomp/`）*、**Ryujinx**（MIT，Maxwell 着色器翻译器）、**[@capibawa](https://github.com/capibawa)**（`nxnative`）、**[AksharDP/xc2-recomp](https://github.com/AksharDP/xc2-recomp)** 与 **[Lichii](https://github.com/zortos293/Lichii)**。随包第三方库（SDL3、Vulkan-Headers、VMA、dav1d、FFmpeg、Dear ImGui、AMD FidelityFX、NVIDIA Streamline）保留其各自开源许可，详见 `licenses/` 与 `THIRD_PARTY_NOTICES.md`。
- **开源许可**：本项目自有代码采用 **GPL-3.0-or-later**。**此处任何许可均不授予任天堂游戏代码或资源的任何权利；由玩家自行构建产出的举升二进制文件不属于本仓库分发内容，权利仍归原权利方所有。**
- **法律声明**：本项目为独立、非商业性的技术爱好者项目，与任天堂或 MONOLITH SOFT 没有任何关联，亦未获得其赞助或认可。*Xenoblade Chronicles*、*Xenoblade Chronicles 2*、*Nintendo Switch* 与 *Nintendo Switch 2* 均为任天堂公司的注册商标。

# XC2 NS2 Recomp v0.3.0 — Native 3D Navigation, Party Voice Subtitles & Redesigned Mods Manager

> **Built out of love. Please support the official release and the future of the Xenoblade series.**

*Xenoblade Chronicles 2* has long had two notorious quality-of-life pain points since the original Switch 1 release: **getting lost on multi-layered Titan maps without a 3D guide line**, and **missing out on unsubtitled combat & post-battle party banter**. Thanks to native static recompilation, **`v0.3.0` brings what we believe are the first publicly released native 3D Field Navigation and real-time Party Voice Subtitle systems for XC2**, alongside a completely redesigned Mods Manager and Launcher enhancements.

> **Note:** Both 3D Navigation and Party Voice Subtitles are **brand-new v1 implementations**. Edge cases in complex terrain, specific quests, or individual voice lines may still occur — bug reports and feedback are very welcome!

> **Upgrading from `v0.2.x`? Clean install strongly recommended!**
> `v0.3.0` introduces a major runtime, shader, and launcher overhaul. To avoid mixed-binary issues (`old xc2_boot.exe` + `new xc2_rt.dll`), we **strongly recommend extracting `XC2-package-dist-0.3.0.zip` into a fresh folder**:
> 1. Extract into a new clean folder, launch `xc2-launcher.exe`, select your 3.0.1 dump, and click **Set up**.
> 2. Carry over your progress cleanly: click **Play → Import saves** and pick your old `save/` folder (you can also copy your old `shader_cache/` and `mods/` folders into the new install folder to skip shader re-translation).
> *(In-place update via **About → Check now → Install update** is still available, but if you use it you must run **Set up** once afterward to relink `xc2_boot.exe`.)*

## Which file do I download?

| file | what it is | do I need it? |
|---|---|---|
| `XC2-package-dist-0.3.0.zip` | full player package: `0.3.0` launcher, updated `xc2_rt.dll`, `xc2_setup.py`, and full toolchain | **yes** — new installs, manual upgrades, or `v0.2.2+` one-click **About → Updates** |
| `xc2-launcher-0.3.0-src.zip` | launcher source code | no |

*(Exact SHA-256 checksums are in `SHA256SUMS.txt` on the release page.)*

## What's new

### 1. Native Vulkan 3D Field Navigation (Base Game + Torna DLC)

- **Depth-occluded 3D Ground Ribbon**: Reads the game's own NavMesh in real time and runs A* pathfinding directly to your active quest objective, rendered as a native Vulkan sky-blue conveyor ribbon (`#56C8FA`) with proper world depth occlusion.
- **Torna DLC Island Stitching**: Automatically stitches disconnected NavMesh islands across ladders and cliffs in *Torna – The Golden Country* (`ma40a`) with reachable-edge fallback guidance.
- **3-Level Cross-Map Skip-Travel Guide**: Automatically resolves cross-Titan objectives to `Titan > Sub-Area > Nearest Landmark` in your game language, and fades out smoothly during combat and cutscenes.
- **F4 Searchable Landmark & NPC Custom Navigation**: Press **`F4`** (or **`ZL + R3`** on gamepad) to search and navigate to any Landmark or talkable NPC on the current map — with full gamepad navigation (`D-Pad / Stick`, `A`, `L/R`, `X`, `B`).
- **Quick Toggle**: Press **`F3`** (or **`ZL + L3`**) anytime to cycle/toggle the 3D navigation overlay.

### 2. Native-Style Party Voice Visualization System (Framework + Local Pack Importer)

- **Zero-Hook Read-Only Detection**: Monitors all 91 playable Drivers, Story/Rare/DLC Blades, and Torna heroes directly from active Wwise sound state — compatible with **both Japanese and English voice tracks**.
- **XC2-Native Dual-Track HUD**:
  - **Combat Tactical Strip (Left)**: Displays real-time spoken lines with semantic badges (`BREAK / TOPPLE / LAUNCH / SMASH / SPECIAL / AGGRO / LOW HP / POTION`), Driver + Blade partner merging, and per-speaker cooldowns.
  - **Exploration & Post-Battle Banter Bar (Bottom)**: Classic XC2 45° chamfered obsidian plate with sky-blue / pale-gold rails for field chatter and post-battle banter.
  - **Voice Log (`Ctrl + F5`)**: Review the last 30 party voice lines anytime.
- **Runtime Native Character Portraits (Zero Copyrighted Assets Shipped)**: Lazily decodes Tegra Block-Linear BC3 portraits directly from your own local `bf2.arh` (`face.wilay`, `btl_face.wilay`, `dlc3_btl_face.wilay`). Spoiler-safe by design — including **automatic Salvager → Master Driver portrait switching for Rex** upon reaching the Chapter 7 awakening progression.
- **Multi-Language Ready (Simplified Chinese / Traditional Chinese / English / Japanese)**: UI badges, speaker nameplates, line wrapping, and reading pacing adapt automatically to your chosen subtitle language (`zh_hans` / `zh_hant` / `en` / `ja`).
- **Local Subtitle Pack Importer**: This release ships **only the voice framework and local importer** (no copyrighted audio, textures, or scripts). Use **"Import Subtitle Pack (.zip / .json)"** on the Launcher's **Play** page to load community-made plain-JSON packs (check *Xenoblade Chronicles 2* modding communities such as GameBanana or GBAtemp).
- **Hotkeys**: `F5` toggle subtitles · `Shift + F5` cycle density (`Full / Balanced / Minimal`) · `Ctrl + F5` Voice Log.

### 3. Redesigned Launcher Mods Manager & Play Page

- **Wide Importer & Traffic-Light Diagnostics**: Import folders, `.zip`/`.7z` archives, or loose `.txt`/`.ips`/`.pchtxt`/`.bdat`/`.wismt` files with automatic NS2 3.0.1 Build ID validation, NS1 `common.bdat` safety blocking, conflict detection, and Safe Mode.
- **Launcher Play & Settings Updates**: Added an **In-Game Features** quick-reference card with one-click local voice subtitle pack import, plus voice subtitle toggles, density, and language selectors in **Settings**.

### 4. Stability Fix ([#91](https://github.com/weile10000/xc2-ns2-recomp/issues/91))

- **Fixed black-screen freeze on boot (`#91`, `vk_device.cpp`)**: reverted the `v0.2.7` startup background Vulkan Video prewarm (`prewarm_hwdec_async()`) which raced initial swapchain and NVN pipeline creation inside NVIDIA/AMD Vulkan drivers. Hardware video decode now initializes lazily when the first cutscene opens, restoring clean startup without requiring manual `hwdec_backend = d3d11va` workarounds.

## Installing / Upgrading to v0.3.0

- **Clean install (Recommended for all users, including `v0.2.x` upgraders)**:
  1. Extract `XC2-package-dist-0.3.0.zip` into a **new, empty folder**.
  2. Launch `xc2-launcher.exe`, select your own 3.0.1 dump (trimmed or raw), and click **Set up**.
  3. On the **very first launch**, the window may stay black for ~20–30 seconds while the GPU driver compiles compute shaders for the first time — please wait briefly without force-closing; subsequent launches enter the title screen normally.
  4. If migrating from `v0.2.x`: use **Play → Import saves** to bring over your saves cleanly, and optionally copy `shader_cache/` and `mods/` from your old folder.
- **In-place update (optional)**: if you update via **About → Check now → Install update**, make sure to run **Set up** once afterward so `xc2_boot.exe` is relinked against the `v0.3.0` runtime.

## Known issues

- **New v0.3.0 features (3D Navigation & Voice Subtitles)**: First public release — complex multi-level terrain paths or rare voice cues may still have edge cases; please report them with logs/screenshots.
- **NVIDIA DLSS black lines ([#74](https://github.com/weile10000/xc2-ns2-recomp/issues/74))**: caused by the **NVIDIA app's DLSS model override** (models **M** and **L**), not this build. Clear the DLSS model override for this game to restore clean output.
- **NVIDIA `VK_ERROR_DEVICE_LOST` in certain scenes/combat ([#41](https://github.com/weile10000/xc2-ns2-recomp/issues/41), [#81](https://github.com/weile10000/xc2-ns2-recomp/issues/81), [#89](https://github.com/weile10000/xc2-ns2-recomp/issues/89))**: underlying NVIDIA driver TDR is still under investigation (local test hardware is AMD RX 6800 XT).
- **Above-60 fps (90 / 120 fps) mode is experimental**: may affect certain UI/QTE animation cadence ([#85](https://github.com/weile10000/xc2-ns2-recomp/issues/85)). For daily play we recommend **60 fps + Frame Generation to 120 fps**.
- **Minor cloud-sea edge artifacts**: a few localized thin edge lines remain in parts of the cloud sea (cosmetic only).
- **Supported platforms**: primary target is native **Windows 11 x64**; **Windows 10 x64** and **Linux / SteamOS (Proton / Wine)** receive best-effort compatibility.

## Credits & Legal Notice

- **Community Contributors**: Special thanks to third-party community contributors creating and maintaining plain-JSON party voice subtitle packs compatible with this interface, and to everyone who submitted logs, diagnostics, and feedback in the issue tracker and community.
- **Upstream & References**: **[nxrecomp](https://github.com/drakolordx7/nxrecomp)** (ARM64 → C++ static recompiler), `Goldoire/fefw-recomp` *(upstream removed; vendored fork preserved in `third_party/fefw-recomp/`)*, **Ryujinx** (MIT, Maxwell shader translator), **[@capibawa](https://github.com/capibawa)** (`nxnative`), **[AksharDP/xc2-recomp](https://github.com/AksharDP/xc2-recomp)**, and **[Lichii](https://github.com/zortos293/Lichii)**. Third-party libraries (SDL3, Vulkan-Headers, VMA, dav1d, FFmpeg, Dear ImGui, AMD FidelityFX, NVIDIA Streamline) retain their respective licences (see `licenses/` and `THIRD_PARTY_NOTICES.md`).
- **Licence**: Project code is licensed under **GPL-3.0-or-later**. **No licence here grants any rights to Nintendo's copyrighted game code or assets; lifted user binaries remain the property of their respective rights holders.**
- **Legal Notice**: Independent, non-commercial fan project. Not affiliated with, sponsored by, or endorsed by Nintendo or MONOLITH SOFT. *Xenoblade Chronicles*, *Xenoblade Chronicles 2*, *Nintendo Switch*, and *Nintendo Switch 2* are trademarks of Nintendo.

---

# XC2 NS2 重编译 v0.3.0 —— 原生 3D 场景导航、队友语音可视化系统与启动器模组管理器重构

> **始于热爱，请支持正版，为了《异度神剑》系列的未来。**

自 2017 年 NS1 原版发售以来，《异度之刃 2》一直有两个困扰玩家多年的老痛点：**多层立体的巨神兽地图没有 3D 引导线容易迷路**，以及**战斗与战后队友语音/相声没有字幕**。依托原生静态重编译的底层能力，**`v0.3.0` 带来了可能是社区首个公开发布实现的 XC2 原生 3D 场景导航与战斗/战后队友语音可视化系统**，并同步重做了启动器模组管理器。

> **说明**：3D 导航与队友语音字幕均为**首次上线的新功能**，在极端复杂地形、个别支线或特定台词上可能仍有待打磨之处，非常欢迎大家游玩后通过 Issue 反馈问题与建议！

> **从 `v0.2.x` 升级必读：强烈推荐全新安装！**
> `v0.3.0` 涉及运行时、着色器与启动器的大幅度更新，为避免新旧文件混用（如旧 `xc2_boot.exe` 配新 `xc2_rt.dll`）引发异常，**强烈建议将 `XC2-package-dist-0.3.0.zip` 解压到全新空文件夹安装**：
> 1. 解压到新目录，打开 `xc2-launcher.exe`，指定你的 3.0.1 Dump 并点击 **「开始构建」**；
> 2. 迁移旧进度：点击 **「游玩 → 导入存档」** 选择旧目录的 `save/` 文件夹即可无损导入存档（也可手动复制旧目录的 `shader_cache/` 与 `mods/` 文件夹以省去重新编译着色器）。
> *（启动器内 **「关于 → 检查更新 → 安装更新」** 仍可用，但覆盖升级后务必点一次 **「开始构建」** 重链 `xc2_boot.exe`。）*

## 我该下哪个文件？

| 文件 | 是什么 | 需要吗 |
|---|---|---|
| `XC2-package-dist-0.3.0.zip` | 完整玩家包：`0.3.0` 启动器、新版 `xc2_rt.dll`、`xc2_setup.py` 与完整工具链 | **要** —— 全新安装、手动覆盖升级，或 `v0.2.2+` 启动器内一键更新 |
| `xc2-launcher-0.3.0-src.zip` | 启动器源码 | 不用 |

*（精确 SHA-256 校验值见发布页的 `SHA256SUMS.txt`。）*

## 本版变化

### 1. 原生 Vulkan 3D 场景导航（支持本篇 + 黄金国 DLC）

- **真·深度遮挡 3D 地面引导带**：实时读取游戏原生 NavMesh 执行 A* 寻路，自动绑定当前主/支线任务目标，以启动器同款天空蓝（`#56C8FA`）流光传送带贴地渲染并具备完整场景遮挡。
- **黄金国 DLC 断岛缝合**：针对《黄金之国伊拉》(`ma40a`) 梯子/悬崖导致的 NavMesh 断岛自动缝合与边缘降级引导，稳定可用。
- **跨图 3 级快速传送指引**：跨巨神兽目标自动解析为 `巨神兽 > 子区域 > 最近传送点` 三级中文/多语路径，进入战斗与过场自动平滑隐藏。
- **F4 地标与 NPC 自定义搜索导航**：按 **`F4`**（手柄 **`ZL + R3`**）可随时搜索当前地图任意地标或可对话 NPC 发起导航，支持全手柄操作（`十字键/摇杆`、`A` 确认、`L/R` 切页、`X` 清除、`B` 关闭）。
- **快捷开关**：随时按 **`F3`**（手柄 **`ZL + L3`**）切换显示模式。

### 2. XC2 原生美术风队友语音可视化系统（框架 + 本地字幕包导入）

- **零 Hook 纯只读监听**：直接轮询游戏活跃语音状态，覆盖全量 91 位主角、剧情/稀有/DLC 异刃及黄金国角色，**同时兼容日语与英语配音**。
- **原生双轨 HUD**：
  - **左侧战斗战术通讯卡**：优先展示角色实际说出口的原台词，搭配语义战术徽章（`破防 / 倒地 / 浮空 / 猛击 / 必杀就绪 / 仇恨警告 / 危机 / 恢复瓶`）、御刃者与异刃合体展示及按说话人独立冷却。
  - **底部探索/战后相声框**：45° 斜切深海板 + 天蓝/淡金双光轨，还原原生观感。
  - **语音记录（`Ctrl + F5`）**：随时回看最近 30 条队友语音。
- **运行时直读原生头像（零版权资源入库）**：直接从玩家本地 `bf2.arh` 实时解码 Tegra 块线性 BC3 头像上传 Vulkan；全程防剧透设计，并支持**莱克斯在第 7 章末觉醒（第 8 章起）自动从打捞员装切换为剑主装头像**。
- **多语言适配（简体中文 / 繁體中文 / English / 日本語）**：战术徽章、角色名牌、断行与停留节奏均随所选字幕语言自动适配（`zh_hans` / `zh_hant` / `en` / `ja`）。
- **本地字幕包导入**：本发行包**仅包含语音可视化框架与本地导入器**，不内置任何游戏音频、贴图或字幕文件。可在启动器「游玩」页点击 **「导入本地字幕包（.zip / .json）」** 加载社区制作的纯 JSON 字幕包（可在 GameBanana / GBAtemp 或国内社区获取）。
- **快捷键**：`F5` 开关字幕 · `Shift + F5` 切换密度（`完整 / 平衡 / 精简`）· `Ctrl + F5` 语音记录。

### 3. 启动器模组管理器重构 & 游玩页升级

- **宽口径导入与红绿灯诊断**：支持文件夹、`.zip`/`.7z` 与散文件导入，内置 NS2 3.0.1 Build ID 校验、NS1 `common.bdat` 拦截、冲突检测与安全模式。
- **游玩页与设置页增强**：新增「游戏内增强」快捷键速查与一键导入本地字幕包入口，设置页新增语音字幕开关、密度与语言选项。

### 4. 稳定性修复（[#91](https://github.com/weile10000/xc2-ns2-recomp/issues/91)）

- **修复启动即黑屏卡死（`#91`，`vk_device.cpp`）**：回退了 `v0.2.7` 引入的启动期后台 Vulkan Video 异步预热（`prewarm_hwdec_async()`），该后台线程在初始化交换链与首帧管线编译时并发创建第二个 Vulkan 设备，导致 N 卡 / A 卡 / Steam 环境在启动期驱动死锁黑屏。现改为在首个过场视频打开时按需初始化硬解，无需手动改 `hwdec_backend = d3d11va`。

## 如何安装 / 升级到 v0.3.0

- **全新安装（强烈推荐，含 `v0.2.x` 老玩家）**：
  1. 将 `XC2-package-dist-0.3.0.zip` 解压到**全新的空文件夹**；
  2. 打开 `xc2-launcher.exe`，指定你自己的 3.0.1 游戏 Dump（已精简或未精简原始文件夹均可），点击 **「开始构建」**；
  3. 全新安装后的**首次启动**可能会在黑屏处停留约 20–30 秒（显卡驱动首次编译计算着色器），请稍等片刻切勿强关，之后每次启动都会秒进标题画面；
  4. 老玩家迁移：点 **「游玩 → 导入存档」** 无损迁入旧存档，也可顺手把旧目录里的 `shader_cache/` 和 `mods/` 复制过来免去重复编译。
- **原地覆盖升级（可选）**：如使用启动器 **「关于 → 检查更新 → 安装更新」**，升级完成后请务必点击一次 **「开始构建」** 重新链接 `xc2_boot.exe`。

## 已知问题

- **v0.3.0 两大新功能（3D 导航 & 队友语音字幕）首次上线**：在复杂立体地形寻路或极个别语音条目上可能仍有边界情况，欢迎附日志/截图反馈。
- **N 卡 DLSS 黑线（[#74](https://github.com/weile10000/xc2-ns2-recomp/issues/74)）**：由 **NVIDIA app 的 DLSS 模型覆盖（模型 M / L）** 引起，并非本版 Bug；清除该游戏的 DLSS 模型覆盖即可恢复干净画面。
- **部分场景/战斗中 N 卡偶发 `VK_ERROR_DEVICE_LOST`（[#41](https://github.com/weile10000/xc2-ns2-recomp/issues/41)、[#81](https://github.com/weile10000/xc2-ns2-recomp/issues/81)、[#89](https://github.com/weile10000/xc2-ns2-recomp/issues/89)）**：底层 N 卡驱动 TDR 仍在持续排查（本地测试硬件为 AMD RX 6800 XT）。
- **高于 60 帧模式（90 / 120 fps）属实验性质**：可能与部分 UI / QTE 步进存在耦合（[#85](https://github.com/weile10000/xc2-ns2-recomp/issues/85)）；日常游玩推荐 **60 帧模式 + 帧生成到 120 帧**。
- **云海局部仍有极少量细边缘瑕疵**（纯视觉，不影响游玩）。
- **支持平台**：主要官方测试目标为原生 **Windows 11 x64**；**Windows 10 x64** 与 **Linux / SteamOS（Proton / Wine）** 为尽力适配（Best-effort）。

## 致谢与法律声明

- **社区贡献致谢**：特别感谢第三方社区创作者为本框架适配并维护纯 JSON 队友语音字幕包，以及在 Issue 区和社区积极提交日志、诊断报告与反馈的每一位玩家。
- **上游与参考致谢**：**[nxrecomp](https://github.com/drakolordx7/nxrecomp)**（静态 ARM64 → C++ 重编译器）、`Goldoire/fefw-recomp` *（原上游已下架，独立分支留存于 `third_party/fefw-recomp/`）*、**Ryujinx**（MIT，Maxwell 着色器翻译器）、**[@capibawa](https://github.com/capibawa)**（`nxnative`）、**[AksharDP/xc2-recomp](https://github.com/AksharDP/xc2-recomp)** 与 **[Lichii](https://github.com/zortos293/Lichii)**。随包第三方库（SDL3、Vulkan-Headers、VMA、dav1d、FFmpeg、Dear ImGui、AMD FidelityFX、NVIDIA Streamline）保留其各自开源许可，详见 `licenses/` 与 `THIRD_PARTY_NOTICES.md`。
- **开源许可**：本项目自有代码采用 **GPL-3.0-or-later**。**此处任何许可均不授予任天堂游戏代码或资源的任何权利；由玩家自行构建产出的举升二进制文件不属于本仓库分发内容，权利仍归原权利方所有。**
- **法律声明**：本项目为独立、非商业性的技术爱好者项目，与任天堂或 MONOLITH SOFT 没有任何关联，亦未获得其赞助或认可。*Xenoblade Chronicles*、*Xenoblade Chronicles 2*、*Nintendo Switch* 与 *Nintendo Switch 2* 均为任天堂公司的注册商标。

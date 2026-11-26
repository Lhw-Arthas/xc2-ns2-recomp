# XC2 NS2 Recomp v0.2.6 — Keyboard & Mouse Controls, Scene HDR, and a Build Chain That Cannot Go Stale

This release ships everything verified since `v0.2.5`: **keyboard and mouse play with rebindable keys ([#69](https://github.com/weile10000/xc2-ns2-recomp/issues/69), [#76](https://github.com/weile10000/xc2-ns2-recomp/issues/76))**, **scene HDR with Accurate/Vivid styles and a live switch ([#68](https://github.com/weile10000/xc2-ns2-recomp/pull/68), by @Revelans)**, **the lightning purple-stripe fix ([#77](https://github.com/weile10000/xc2-ns2-recomp/pull/77), by @Revelans)**, a launcher that is translated everywhere including the build page, and a build chain that relinks a stale `xc2_boot.exe` instead of trusting that the file exists.

> **Upgrading from `v0.2.5` or older? Read this!**
> 1. **One click**: `xc2-launcher.exe` → **About → Check now → Install update**, or extract `XC2-package-dist-0.2.6.zip` over your install folder.
> 2. **The first Set up after upgrading rebuilds `xc2_boot.exe` once** (5–10 minutes on a normal desktop). The build chain now relinks whenever the runtime changed, because an old exe next to a new `xc2_rt.dll` is a mixed build — that is the class of bug behind "it looked broken until I rebuilt it". Later runs skip the step.
> 3. **Your `nxrt.ini`, `controls.ini`, saves and `shader_cache` are kept.** A `controls.ini` from an older build has `menu_cursor` migrated to `0` automatically, so the mouse is the camera and the left button is a game button instead of a menu pointer.

## Which file do I download?

| file | what it is | do I need it? |
|---|---|---|
| `XC2-package-dist-0.2.6.zip` | full player package: `0.2.6` launcher, updated `xc2_rt.dll`, `xc2_setup.py`, toolchain | **yes** — new installs and manual upgrades |
| `xc2-launcher-0.2.6-src.zip` | launcher source code | no |

*(See `SHA256SUMS.txt` on the release page for exact file hashes.)*

## What's new

### 1. Keyboard & mouse play (`#69`, `#76`, `play_engine`, `play_ui`, `xc2_boot`)
- **Mouse look**: raw relative input, no acceleration, captured while the game has focus and released when the F1 page opens. Gain is `[mouse] stick_scale` in `controls.ini`, exposed as a live slider in F1.
- **Bindable keys through the runtime's play-mode engine** instead of a hardcoded table: WASD move, IJKL look, `E` / left click / `Return` confirm, `Space` / `Backspace` / `Esc` cancel, right click lock-on, `1`–`4` = Y / A / B / X, `Q` / `R` = ZL / ZR, `F` / `G` = L / R, `Tab` = `+`, `M` = left-stick click, wheel = d-pad up/down.
- **F1 rebind page**: every action lists its keys as buttons — click one to capture a new key, `×` removes it, `+` adds another. A key that already drives something else **unbinds the other action** and the notice names it, so one input never does two things. A separate **Reset keys** button restores the factory bindings without touching any other setting.
- **Stick calibration (`#76`)**: both sticks show the raw value they report at rest, with "Set centre here" and two fine sliders. The measured centre is subtracted from the raw axis **before** the dead zone — a dead zone cannot fix a stick that rests off zero, which is what turns "push back" into a slow walk. Defaults are 0, so a healthy pad is unaffected.

### 2. 90 / 120 fps, and a 60 fps cutscene cap (`7485963`, `c0dac09`, `7a6fbd8`, `93e36cf`)
- The game's own limiter can be rewritten to **90 or 120 fps** with a dual clock: game logic keeps its 30 Hz step while presents double or quadruple, and pre-rendered cutscenes stay capped at 60 fps so they keep their timing.
- Resource-bank teardown now scales with the rate (`(cur_fps + 59) / 60`), which is what stopped the crashes when leaving a cutscene or teleporting at 120 fps.
- F1 defaults the upscaler to **TAAU at 100 % (native)** with the frame limit on `auto`, and the launcher marks **60 fps as recommended** with 90 / 120 fps as experimental.

### 3. Boot shader preparation and a faster replay path (`3b54fc3`, `3198860`, `7a6fbd8`, `2272f83`, `3823a03`)
- Setup now extracts the game's paired `programs.bin` from **your own `bf2.ard`**, and the runtime builds its graphics pipelines **before the first frame**, sharing the prepared library with the draw-time cache — a new area no longer stutters while pipelines compile.
- `key.color_count == 0` no longer prunes fragment outputs, which used to make prewarmed depth/shadow variants miss and fall back to a draw-time build.
- Command replay, mirror-pool dirty-range tracking, pipeline lookup, the dispatcher, hook slots and write-watch were all made cheaper; on our machine the dev build measured about 45 → 58 fps at 1080p with no visual change.

- The tone map is patched to follow the game's **own scene values** above mid grey instead of an SDR shoulder, and the window plus the upscaler output are promoted to `RGBA16F` while the patch runs.
- New `[hdr] style` (`accurate` / `vivid`) and `[hdr] keep_highlights`, and the HDR switch now applies **without restarting**.
- `[hdr] mode` still defaults to `off`: nothing changes for players who do not use HDR.

### 4. Scene HDR, Accurate/Vivid styles, live toggle (`#68`, by @Revelans)
### 5. Visual fixes (`#77`, `#72`, by @Revelans and @UrsineBear)
- **Lightning purple stripes (`#77`, by @Revelans)**: the level-0 block depth of non-sparse 3D textures is sized to the slice count instead of always 16, and a packaged layout's byte 0 supplies the 3D block shape. XC2's `256x128x8` BC4 lightning flipbook was laid out over 16 slices instead of 8, so the GPU read the wrong texels.
- **Facial morph flicker under mirror capture (`#72`, by @UrsineBear)**: when `[nvn] mirror_gfx_storage_writeback = 0`, graphics-stage SSBO writes stay in the device-local mirror, so host RAM still holds the pre-morph bytes. An SSBO whose head or tail shares a 4 KiB page with a per-frame CPU-written UBO had those bytes overwritten with stale host memory on the next capture. The page range is now split around the byte ranges the GPU owns, and that arithmetic has its own test group (`gfx_nvn_capture_spans`, seven cases).

### 6. Launcher
- **Language switch in the top-right corner**, the same idea as the F1 menu: the label is the language it switches to, and it keeps your scroll position. The Settings row stays.
- **The build page is translated**: ten of its sixteen step titles were still English, plus the two progress lines for the long phases.
- **Picture mode** replaces "operation mode": docked = full quality, handheld = lighter. That is what the game actually uses it for.
- **Joy-Con 2 mouse rows removed** — an upstream feature; XC2 drives the camera from the real mouse.
- The **Vivid** colour-grading preset now shows its "active" badge like Neutral does, and the output resolution hint says what to do about a soft picture (1440p or higher plus DLSS/FSR).
- **5.1 surround** is selectable in the launcher (`[audio] channels`, [#60](https://github.com/weile10000/xc2-ns2-recomp/issues/60)).

### 7. Build and install chain
- **A stale exe is no longer kept.** Both link paths now relink when the runtime changed (the package path records the runtime DLL signature in `build/link-stamp.json`).
- **The shader prefill marker carries the runtime signature**, so a new runtime re-runs only the prefill and keeps the translated cache (which is keyed by shader hash).
- **`play.ps1` takes the retail switches from `GAME_ENV`** instead of the dev profile keeping a copy, and a new gate (`check_profiles.py`) fails when a profile contradicts them.
- **A fresh `controls.ini` sets `menu_cursor = 0`**, so a clean install no longer starts with the left button eaten by the menu pointer.
- **The `nxrt.ini` template no longer seeds keys the F1 page owns** (`[hdr] mode`, the window size), and setup clears a shadowed key that still holds the old default — the F1 changes were being reverted on the next launch ([#57](https://github.com/weile10000/xc2-ns2-recomp/issues/57), [#73](https://github.com/weile10000/xc2-ns2-recomp/issues/73)).
- **The log reports the real build** (`git describe`, e.g. `v0.2.6`) instead of a hardcoded `0.2.0`, so a report identifies the version it came from.

### 8. Diagnostics
- **Freeze and device-lost dumps now report the pipeline prewarm counters**, including whether the workers were still building when the device went away — the one fact [#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32) could not show.
- The mirror-cap test asserted the upstream `2048 MiB` ceiling that our fork raised to `8192 MiB` (XC2's mesh pools need about 2.6 GiB of whole-pool copies); it had been failing on main and hiding real failures.

## Upgrading to v0.2.6

- **From `v0.2.5` or older**: open `xc2-launcher.exe` → **About → Check now → Install update**, or extract `XC2-package-dist-0.2.6.zip` over your existing install folder. Then press **Set up** once: it relinks `xc2_boot.exe` (5–10 minutes) and leaves your saves, `nxrt.ini`, `controls.ini` and `shader_cache` alone.
- **New installs**: extract into a clean folder, launch `xc2-launcher.exe`, point it at your own 3.0.1 dump, and press **Set up**.

## Known issues

- **NVIDIA black lines with DLSS/Streamline ([#74](https://github.com/weile10000/xc2-ns2-recomp/issues/74))**: still open. Reported on 40/50-series cards; a reproduction attempt on an RTX 3080 Laptop found no lines and 0 NaN/Inf in 16 dumps, so it looks driver- or generation-specific. FSR 3 and TAAU are unaffected — use one of those on NVIDIA if you see them.
- **Device loss on NVIDIA ([#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32), [#41](https://github.com/weile10000/xc2-ns2-recomp/issues/41))**: the failures happen inside `nvoglv64.dll`; we only have an AMD card here and cannot reproduce them. This release adds the diagnostics needed to correlate them with the pipeline prewarm, and `[nvn] shader_prepare = off` / `[nvn] pool_mirror_mb` remain available as A/B switches.
- **Frame Generation side effects**: frame interpolation may cause subtle mouth/eye flicker in close-ups, and a cutscene can freeze under FG without producing a freeze report (the interpolated frames keep the watchdog satisfied).
- **Add-on heavy scenes and first-run pipeline compile**: Torna / add-on areas stay heavier than base game areas, and the first launch per driver compiles Vulkan pipelines.
- **Cloud sea**: a very small number of thin edge artifacts remain in parts of the cloud sea.
---

# XC2 NS2 重编译 v0.2.6 —— 键鼠操作、场景 HDR，以及一条不会「陈旧」的构建链

本版包含 `v0.2.5` 之后验证过的全部改动：**键鼠操作与可改键（[#69](https://github.com/weile10000/xc2-ns2-recomp/issues/69)、[#76](https://github.com/weile10000/xc2-ns2-recomp/issues/76)）**、**场景 HDR 与「准确 / 鲜艳」风格、免重启切换（[#68](https://github.com/weile10000/xc2-ns2-recomp/pull/68)，by @Revelans）**、**闪电特效紫纹修复（[#77](https://github.com/weile10000/xc2-ns2-recomp/pull/77)，by @Revelans）**、全面汉化的启动器（含构建页），以及**会主动重链接陈旧 `xc2_boot.exe` 的构建链**。

> **从 `v0.2.5` 或更早升级？请先看这里**
> 1. **一键更新**：`xc2-launcher.exe` → **关于 → 检查更新 → 安装更新**；或把 `XC2-package-dist-0.2.6.zip` 解压覆盖到安装目录。
> 2. **升级后第一次「开始构建」会重新链接一次 `xc2_boot.exe`**（普通台式机约 5–10 分钟）。构建链现在只要运行时变了就重链接——旧的 exe 配新的 `xc2_rt.dll` 是混合构建，这正是「我重新构建了一下就好了」那类问题的根源。之后不再重复。
> 3. **你的 `nxrt.ini`、`controls.ini`、存档和 `shader_cache` 都会保留。** 旧版 `controls.ini` 里的 `menu_cursor` 会自动迁移为 `0`，鼠标才是镜头、左键才是游戏按键，而不是菜单指针。

## 我该下哪个文件？

| 文件 | 是什么 | 需要吗 |
|---|---|---|
| `XC2-package-dist-0.2.6.zip` | 完整玩家包：`0.2.6` 启动器、更新后的 `xc2_rt.dll`、`xc2_setup.py`、工具链 | **需要** —— 新装与手动升级 |
| `xc2-launcher-0.2.6-src.zip` | 启动器源码 | 不需要 |

*（文件哈希见发布页的 `SHA256SUMS.txt`。）*

## 本版变化

### 1. 键鼠操作（`#69`、`#76`、`play_engine`、`play_ui`、`xc2_boot`）
- **鼠标转视角**：原始相对位移、无加速度；游戏获得焦点时捕获指针，打开 F1 时释放。灵敏度是 `controls.ini` 里的 `[mouse] stick_scale`，F1 里有实时滑块。
- **按键走运行时的 play 模式绑定引擎**，不再是硬编码表：WASD 移动、IJKL 视角、`E`/左键/回车=确认、空格/退格/Esc=取消、右键=锁定、`1`–`4`=Y/A/B/X、`Q`/`R`=ZL/ZR、`F`/`G`=L/R、`Tab`=加号、`M`=左摇杆按下、滚轮=十字键上下。
- **F1 改键页**：每个动作把它所有的键列成按钮——点一下就进入捕获，`×` 删除，`+` 再加一个。若按下的键已经被别的动作占用，**那条旧绑定会被解绑**，提示里会点名，保证一个输入不会同时干两件事。另有独立的**「恢复默认按键」**，只重置键位、不动其他设置。
- **摇杆校准（`#76`）**：左右两杆都会显示静止时上报的原始值，配「以此为中位」和两个微调滑块。测得的中位**在死区之前**从原始轴减掉——死区治不了「静止时轴心偏移」，那正是「往后走变成慢走」的原因。默认值为 0，健康手柄完全不受影响。

### 2. 90 / 120 fps 与过场 60 fps 封顶（`7485963`、`c0dac09`、`7a6fbd8`、`93e36cf`）
- 游戏自身的限帧可以改写为 **90 或 120 fps**，采用双时钟：游戏逻辑仍按 30 Hz 步进，而 present 翻倍或翻四倍；预渲染过场封顶 60 fps，保持原有节奏。
- 资源 bank 的销毁节奏改为随帧率缩放（`(cur_fps + 59) / 60`），这正是修掉「120 fps 下退出过场或传送时崩溃」的那一笔。
- F1 默认超分器为 **TAAU 100%（原生）**、帧率上限 `auto`；启动器把 **60 fps 标为推荐**，90 / 120 fps 标为实验性。

### 3. 启动期着色器预热与更快的回放路径（`3b54fc3`、`3198860`、`7a6fbd8`、`2272f83`、`3823a03`）
- 安装时会从**你自己的 `bf2.ard`** 提取成对的 `programs.bin`，运行时在**首帧之前**就把图形管线建好，并与绘制期的缓存共享——新场景不再因为现编译管线而卡顿。
- `key.color_count == 0` 不再剪枝片元输出；以前正是它让预热的深度/阴影变体对不上、退回绘制期现建。
- 命令回放、镜像池脏区跟踪、管线查找、调度器、hook 槽位与 write-watch 都做了提速；在本机上 dev 构建 1080p 实测约 **45 → 58 fps**，画面无变化。

- 色调映射改为在中间灰以上**跟随游戏自己的场景数值**，而不是压到 SDR 肩部；补丁运行期间窗口与超分输出提升为 `RGBA16F`。
- 新增 `[hdr] style`（`accurate` / `vivid`）与 `[hdr] keep_highlights`，HDR 开关**不再需要重启**。
- `[hdr] mode` 默认仍是 `off`：不用 HDR 的玩家完全不受影响。

### 4. 场景 HDR、「准确 / 鲜艳」风格与免重启切换（`#68`，by @Revelans）
### 5. 画面修复（`#77`、`#72`，by @Revelans 与 @UrsineBear）
- **闪电特效紫色条纹（`#77`，by @Revelans）**：非 sparse 3D 纹理的 level-0 块深度改为按切片数取（原来恒为 16），packaged layout 的 byte 0 提供 3D 块形状。XC2 那张 `256x128x8` 的 BC4 闪电 flipbook 被按 16 层排布而不是 8 层，GPU 读到了错位的纹素。
- **镜像捕获下的面部形变闪烁（`#72`，by @UrsineBear）**：当 `[nvn] mirror_gfx_storage_writeback = 0` 时，图形阶段的 SSBO 写入留在设备本地镜像里，主机内存仍持有形变前的字节。若该 SSBO 的首部或尾部与一个每帧被 CPU 写入的 UBO 共享同一个 4 KiB 页，下次捕获就会用陈旧的主机内存覆盖这些字节。现在会按 GPU 拥有的字节区间切分页范围，这段算术有独立测试组（`gfx_nvn_capture_spans`，7 个用例）。

### 6. 启动器
- **右上角语言切换**，和 F1 里那个同一个思路：按钮显示的是「要切过去的语言」，并且不会把你弹回页面顶部。设置页的语言项保留。
- **构建页汉化**：16 个步骤里有 10 个标题还是英文，加上两条长耗时阶段的进度行，现在都是中文。
- **「画质模式」取代「运行模式」**：主机 = 高画质，掌机 = 省性能。游戏本来就是拿它这么用的。
- **删掉 Joy-Con 2 鼠标两项**——那是上游的功能，XC2 用真实鼠标。
- 「鲜艳通透」预设现在会像「恢复原版色调」一样显示**「当前」**标记；输出分辨率提示也改成告诉玩家怎么解决「糊」（1440p 或更高 + DLSS/FSR）。
- **5.1 环绕声**可在启动器里选择（`[audio] channels`，[#60](https://github.com/weile10000/xc2-ns2-recomp/issues/60)）。

### 7. 构建与安装链
- **不再保留陈旧的 exe**：两条链接路径都会在运行时变化后重链接（包模式把运行时 DLL 的签名记进 `build/link-stamp.json`）。
- **shader prefill 的完成标记带上运行时签名**：换了运行时就只重跑 prefill，已翻译的缓存（按 shader 哈希索引）保留。
- **`play.ps1` 直接从 `GAME_ENV` 取零售开关**，不再在 dev profile 里抄一份；新增闸门 `check_profiles.py`，profile 与它冲突就失败。
- **新生成的 `controls.ini` 会写 `menu_cursor = 0`**，全新安装不会再出现「左键被菜单指针吃掉」。
- **`nxrt.ini` 模板不再种 F1 页拥有的键**（`[hdr] mode`、窗口尺寸），安装时还会清理「仍是旧默认值」的被遮蔽键——以前在 F1 改完下次启动会被顶回（[#57](https://github.com/weile10000/xc2-ns2-recomp/issues/57)、[#73](https://github.com/weile10000/xc2-ns2-recomp/issues/73)）。
- **日志报告真实版本**（`git describe`，例如 `v0.2.6`），不再写死 `0.2.0`，玩家发来的报告能直接看出是哪个版本。

### 8. 诊断
- **冻结与设备丢失报告现在会带上管线预热计数**，包括「设备丢失时 worker 是否还在建管线」——这正是 [#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32) 的报告里看不出来的那一条。
- mirror 上限测试断言的是上游的 `2048 MiB`，而我们早已提到 `8192 MiB`（XC2 的 mesh 池需要约 2.6 GiB 整池拷贝）；它一直在 main 上失败，会掩盖真正的回归，已修正。

## 如何升级到 v0.2.6

- **从 `v0.2.5` 或更早**：打开 `xc2-launcher.exe` → **关于 → 检查更新 → 安装更新**；或把 `XC2-package-dist-0.2.6.zip` 解压覆盖到原安装目录。然后按一次**「开始构建」**：它会重链接 `xc2_boot.exe`（5–10 分钟），存档、`nxrt.ini`、`controls.ini`、`shader_cache` 都不动。
- **全新安装**：解压到干净目录，运行 `xc2-launcher.exe`，指向你自己的 3.0.1 dump，按**「开始构建」**。

## 已知问题

- **N 卡 DLSS/Streamline 下的黑线（[#74](https://github.com/weile10000/xc2-ns2-recomp/issues/74)）**：仍未解决。报告集中在 40/50 系；在 RTX 3080 Laptop 上的复现尝试未出现黑线，16 份 dump 里 0 个 NaN/Inf，因此更像是驱动或特定代际的问题。FSR 3 与 TAAU 不受影响，N 卡上遇到就用这两个。
- **N 卡设备丢失（[#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32)、[#41](https://github.com/weile10000/xc2-ns2-recomp/issues/41)）**：故障发生在 `nvoglv64.dll` 内部，本地只有 A 卡，无法复现。本版加入了把它与管线预热关联起来所需的诊断；`[nvn] shader_prepare = off` 与 `[nvn] pool_mirror_mb` 仍可作为 A/B 开关使用。
- **帧生成的副作用**：帧插值可能让近景的嘴部/眼部出现轻微闪烁；开启帧生成时过场有可能冻住且不产生冻结报告（插值帧让 watchdog 一直满足）。
- **追加内容重场景与首次管线编译**：黄金之国等追加场景仍比本篇更重；每换一次驱动，首次启动会编译 Vulkan 管线。
- **云海**：云海局部仍有极少量细边缘瑕疵。
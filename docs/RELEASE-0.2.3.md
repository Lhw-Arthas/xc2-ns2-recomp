# XC2 NS2 Recomp v0.2.3 — #15 Flicker Root Fix, Movie Freeze Fixes, M4 Mod Manager & #44/#47 Rendering

This release ships all integration fixes verified after `v0.2.2`, including the **Issue #15 blinking / dialogue flicker root fix**, **cutscene & movie freeze fixes** (`movie_adec` coalesced-wakeup / Vorbis backpressure + CPU `dav1d` clock recovery + `#42` 9070 XT prewarm + PR `#48` NVDEC), **Issue #44 RTX 40-series 3D shadow stripes fix** + **Issue #47/#39 4K `DLAA`/`Quality` scaling restoration**, **M4 Mods UI / Options / Conflicts / DMNT Cheats VM**, **Issue #45 older Windows 10 `spirv_harness.exe` compatibility**, and a **`spirv_uint_samplers` SPIR-V ordering fix for `amdvlk64.dll`**.

> **Upgrading from `v0.2.2`? Read this!**
> 1. **`v0.2.2` users can update in one click** via **About → Updates** inside `xc2-launcher.exe` (no `xc2_boot.exe` rebuild is required).
> 2. **To apply the Issue #15 blinking/dialogue flicker fix**: because `#15` is fixed in the offline SPIR-V shader translator (`spirv_harness.exe`), **delete your existing `shader_cache\spirv\` folder** (or rename it as a backup) and click **Set up** in the launcher once after updating. `Set up` will skip `xc2_boot.exe` (already built) and re-translate the shaders with the fixed `spirv_harness.exe`.

## Which file do I download?

| file | what it is | do I need it? | SHA-256 |
|---|---|---|---|
| `XC2-package-dist-0.2.3.zip` | full player package: `0.2.3` launcher (M4 Mods UI + Cheats), `xc2_rt.dll`, fixed `spirv_harness.exe`, lifter & shader tools | **yes** — new installs, `v0.2.0`/`v0.2.1` manual upgrades, or `v0.2.2` one-click **About → Updates** | `86009357da754dd18a72cb7546115bd841481f47c8f8945f36157a731bd5ca4a` |
| `xc2-launcher-0.2.3-src.zip` | launcher source code | no | `198d4be6c445af03a61562eeea465b9f66bb0fb2f2cebfeee40ac71b54aa0c20` |

## What's new

### 1. Issue #15 blinking / dialogue flicker root fix ([`01df5fd`](https://github.com/weile10000/xc2-ns2-recomp/commit/01df5fd))

- Fixed Ryujinx `StructuredIr/AstOptimizer` (`spirv-call-args.patch` in `spirv_harness.exe`) so single-use mutable `StorageBuffer` loads (`LDG` before `STG` in vertex morph compute shader `c83ff5fc5d458f3a`) are no longer folded past subsequent stores, eliminating the false motion vectors behind blinking/mouth flicker in motion blur.
- Added runtime uint-sampler rewriting for float samplers targeting `R8_UINT` attachments (`nvn_spirv_patch.cpp`) and fixed SPIR-V type/variable declaration ordering in `spirv_uint_samplers` so `d2767845f04b8ef2` (`usampler variant`) validates cleanly under `spirv-val` and no longer crashes `amdvlk64.dll`.

### 2. RTX 40-series 3D shadow stripes (#44) & 4K DLAA / `Quality` scaling (#47 / #39)

- Enabled `VK_EXT_depth_bias_control` (`leastRepresentableValueForceUnormRepresentation`) and `vkCmdSetDepthBias2EXT` (`VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT`) on `D32_SFLOAT_S8_UINT` shadow passes so depth bias matches console `UNORM` stepping without dark stripes on RTX 40-series GPUs ([#44](https://github.com/weile10000/xc2-ns2-recomp/issues/44)).
- Restored `[upscale] render_quality` default routing to the NVN render-target scaling path (`nvn_gpu_texture.cpp::quality_scales()` + `xc2_lock_render_size()`) instead of forcing guest `native_res = 2`, restoring full `3840×2160` internal rendering on 4K `Native` / `DLAA` ([#47](https://github.com/weile10000/xc2-ns2-recomp/issues/47)). Set `[xc2] native_res > 0` explicitly only if you want guest-side tier selection.

### 3. Movie & cutscene freeze fixes + video decoding (`17d3012`..`3607075`, `#42`, `#35`, PR `#48`)

- **Cutscene mid-video freeze root fixes (`movie_adec.cpp`, `audio_out.cpp`)**: fixed intermittent ~35s freezes on Torna / base-game 4K cutscenes by re-signaling coalesced `AudioDecoder` output frames (`17d3012`), unregistering finalized decoders, fixing `kResultNoFrame` to `12` and serializing output event clears (`3ba71c6`), widening the audio gate past pre-roll with wakeup on video advance (`98a3389`), and capping Vorbis decode backlog with `in_ready` backpressure (`3607075`).
- **CPU `dav1d` cutscene picture freeze recovery (`movie_trace.cpp`, `#35`)**: automatically re-anchors `RenderThread`'s movie clock (`player[+644] = 1`) when software decoding drops frames with no decoded backlog, preventing permanent 100%-drop freezes while audio keeps playing.
- **#42 RX 9070 XT frame-0 startup deadlock**: deferred `ffdec_prewarm()` from `install_video_codec_hle()` (`t = 0.001 s`) to movie `worker_loop` creation so `av_hwdevice_ctx_create(VULKAN)` never races `gfx::Device::create()` in `amdvlk64.dll`.
- **PR #48 (`video_nvdec.cpp`, @nikolaybutnik)**: fixed NVDEC `CUdeviceptr` NV12 plane transfer to host memory via `cuCtxPushCurrent` + `cuMemcpy2D` (`cuCtxPopCurrent`).

### 4. M4 Mod Manager, Options, Conflicts & DMNT Cheats VM

- **Launcher Mods page (M4b)**: drag/reorder RomFS mods, configure declarative `.mod_layers.tsv` option sub-layers, inspect file conflicts, enable ExeFS patches (`.pchtxt` / `.ips` via M3 `exefs_patch.py`) and DMNT cheat entries (`.cheats_enabled.tsv`), and gate `Set up` rebuilds cleanly (`[mods] pending_rebuild`).
- **Runtime DMNT Cheat VM (M4c)**: executes Atmosphère DMNT dynamic cheats (opcodes `0`–`9` with `VirtualQuery` / `mem::is_mapped` bounds checks) per presented frame (`cheats.cpp`).

### 5. Platform & startup hardening

- **#45 Older Windows 10 `spirv_harness.exe` exit `0xC000000D` (`2148734214`)**: disabled `<CETCompat>` in `SpirvHarness.csproj` (`ExtendedDLLCharacteristics = 0x0`) for Windows 10 builds without full CET shadow-stack support.
- **#35 stackwalk guard (`stackwalk.cpp`)**: guards `capture_stack()` with `VirtualQuery` stack readability checks and SEH (`__try / __except`).
- **#43 / #33 / #25**: auto-skips incompatible v3.0.0 DLC 98 (#43), pre-reserves an alternative 128 GiB alias window on host address collisions (#33), and clears `HIGH_ENTROPY_VA` (#25).

## Upgrading to v0.2.3

- **From `v0.2.2`**:
  1. Open `xc2-launcher.exe` → **About → Check now** → **Install update** (or extract `XC2-package-dist-0.2.3.zip` into your `v0.2.2` folder). `xc2_boot.exe` does **not** need rebuilding.
  2. **Important for #15 (blinking / dialogue flicker)**: delete `shader_cache\spirv\` inside your install folder and run **Set up** once to re-translate shaders using the updated `spirv_harness.exe`.
- **From `v0.2.1` or older / New installs**:
  - Extract `XC2-package-dist-0.2.3.zip` cleanly (or over `v0.2.1` + delete `shader_cache\spirv\` and click **Set up**; copy `save/` and `upscalers/` if moving to a fresh folder).

## Known issues

- **Lv. 130 Tyrannotitan Kurodil freeze on NVIDIA ([#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32))**
- **Add-on scenes & first-run pipeline compile**: Torna / add-on scenes remain heavier than base game areas; first launch compiles Vulkan pipelines (~20 s pause).
- **Platform support**: Initial releases only officially support native **Windows 11 x64**. Windows 10 and Linux / SteamOS / Wine / Proton are not officially supported in this phase — future support is not ruled out, and community forks adapting other platforms are welcome.

---

# XC2 NS2 重编译 v0.2.3 —— 根修 #15 闪烁、影片冻结修复、M4 模组管理与 #44/#47 渲染

本版本合入 `v0.2.2` 之后的全部集成修复：根治 **#15 号角色眨眼/对白闪烁**，彻底修复 **过场影片中途冻结**（`movie_adec` 合并唤醒/Vorbis 背压 + CPU `dav1d` 时钟恢复 + `#42` 9070 XT 预热防死锁 + PR `#48` NVDEC 拷贝），修复 **#44 RTX 40 系 3D 阴影黑条纹** 并恢复 **#47/#39 4K `DLAA`/`Quality` 缩放链路**，正式内置 **M4 模组管理 / 选项层 / 冲突检测 / DMNT 作弊码引擎**，修复 **#45 旧版 Win10 `spirv_harness.exe` 启动报错**，以及 **`spirv_uint_samplers` 前向引用导致的 `amdvlk64.dll` 崩溃**。

> **升级必读（尤其是从 `v0.2.2` 在线更新的用户）**：
> 1. **`v0.2.2` 用户可直接在启动器「关于 → 检查更新 → 安装更新」一键升级**，无需重新编译 `xc2_boot.exe`。
> 2. **要让 #15 眨眼/对白闪烁修复生效**：因为根修位于离线着色器翻译器 `spirv_harness.exe`，升级后请**删除安装目录下的 `shader_cache\spirv\` 文件夹**（或先改名备份），然后在启动器再点一次 **Set up**。Set up 会跳过已编好的 `xc2_boot.exe`，仅使用新版 `spirv_harness.exe` 重译着色器。

## 我该下哪个文件？

| 文件 | 是什么 | 需要吗 | SHA-256 |
|---|---|---|---|
| `XC2-package-dist-0.2.3.zip` | 完整玩家包：含 M4 模组 UI 的 `0.2.3` 启动器、新版 `xc2_rt.dll`、修复后的 `spirv_harness.exe` 与提升/着色器工具 | **要** —— 全新安装、手动覆盖升级，或 `v0.2.2` 启动器内一键更新 | `86009357da754dd18a72cb7546115bd841481f47c8f8945f36157a731bd5ca4a` |
| `xc2-launcher-0.2.3-src.zip` | 启动器源码 | 不用 | `198d4be6c445af03a61562eeea465b9f66bb0fb2f2cebfeee40ac71b54aa0c20` |

## 本版变化

### 1. 根修 #15 角色眨眼 / 对白闪烁（[`01df5fd`](https://github.com/weile10000/xc2-ns2-recomp/commit/01df5fd)）

- 修复 Ryujinx `StructuredIr/AstOptimizer`（`spirv_harness.exe` 内 `spirv-call-args.patch`）：禁止把单次使用的可变 `StorageBuffer` 加载折叠越过后续 `STG` 写入（修复 vertex morph 计算着色器 `c83ff5fc5d458f3a` 注入虚假运动矢量导致 motion blur 里眨眼/口型糊成灰褐噪点）。
- 修复运行时 `spirv_uint_samplers` 替换 uint 采样器时未重排后置类型与变量声明导致的非法 SPIR-V 前向引用（`d2767845f04b8ef2` 通过 `spirv-val` 全检，不再触发 Windows `amdvlk64.dll` 编译管线崩溃）。

### 2. 修复 #44 RTX 40 系 3D 阴影条纹 & 恢复 #47/#39 4K `DLAA` / `Quality` 缩放

- 在 `vk_device.cpp` / `nvn_replay.cpp` 启用 `VK_EXT_depth_bias_control`（`FORCE_UNORM`）并使用 `vkCmdSetDepthBias2EXT`，使 `D32_SFLOAT_S8_UINT` 深度偏移遵循固定 `UNORM` 步长，根除 RTX 40 系 3D 阴影条纹（[#44](https://github.com/weile10000/xc2-ns2-recomp/issues/44)）。
- 默认恢复 `[upscale] render_quality` 走 PR #36 的 NVN 渲染目标缩放路径（[#47](https://github.com/weile10000/xc2-ns2-recomp/issues/47)），4K `Native` / `DLAA` 恢复真 `3840×2160` 内部分辨率（仅当手动配置 `[xc2] native_res > 0` 时才改由客体档位接管）。

### 3. 过场影片冻结与视频解码修复（`17d3012`..`3607075`、`#42`、`#35`、PR `#48`）

- **根治 CG 过场中途冻结（`movie_adec.cpp`、`audio_out.cpp`）**：针对伊拉/本体 4K 过场在 ~35 秒处偶发画面冻结，修复 `AudioDecoder` 合并输出帧重发事件（`17d3012`）与释放注销、修正 `kResultNoFrame` 返回值 (`12`) 与清除事件串行化（`3ba71c6`）、预缓冲后放宽音频门控并由视频推进唤醒（`98a3389`），并通过 `in_ready` 背压限制 Vorbis 解码队列积压（`3607075`）。
- **CPU `dav1d` 软解过场丢帧时钟重锚定（`movie_trace.cpp`，`#35`）**：当无预解积压且发生丢帧时自动置 `player[+644] = 1` 重锚定 `RenderThread` 时钟，防止陷入 100% 永久丢帧冻帧。
- **#42 RX 9070 XT 点 Play 卡死在第 0 帧**：将 `ffdec_prewarm()` 从启动早期（`t = 0.001 s`）延后至影片解码线程首次创建时执行，避免与主线程 `gfx::Device::create()` 在 `amdvlk64.dll` 内并发建设备死锁。
- **PR #48（`video_nvdec.cpp`，@nikolaybutnik）**：通过 `cuCtxPushCurrent` + `cuMemcpy2D` 修复 NVDEC 显存到宿主内存拷贝。

### 4. M4 模组管理、选项层、冲突检测与 DMNT 作弊码引擎

- **启动器 Mods 页面（M4b）**：支持 RomFS 模组拖拽排序、声明式 `.mod_layers.tsv` 选项子层切换、文件冲突预览、ExeFS 补丁（M3 `.pchtxt` / `.ips`）与 `.cheats_enabled.tsv` 作弊开关及重编门控（`[mods] pending_rebuild`）。
- **运行时 DMNT 虚拟机（M4c）**：按呈递帧执行 Atmosphère DMNT 动态作弊码（Opcode `0`–`9`，接入 `VirtualQuery` / `mem::is_mapped` 边界安全校验）。

### 5. 平台兼容性与启动加固

- **#45 旧版 Win10 `spirv_harness.exe` 启动报 `0xC000000D` (`2148734214`)**：在 `SpirvHarness.csproj` 关闭 `<CETCompat>false</CETCompat>`（PE `ExtendedDLLCharacteristics = 0x0`）。
- **#35 栈回溯 SEH 保护（`stackwalk.cpp`）**：为 `capture_stack()` 增加 `VirtualQuery` 栈可读性检查与 `__try / __except` 保护。
- **#43 / #33 / #25**：自动跳过不兼容的 v3.0.0 DLC 98（#43）、主机地址碰撞时自动探测备用 128 GiB 别名窗口（#33）、关闭 `HIGH_ENTROPY_VA`（#25）。

## 如何升级到 v0.2.3

- **当前已在 `v0.2.2`**：
  1. 在 `xc2-launcher.exe` 点击 **「关于 → 检查更新 → 安装更新」** 一键完成在线更新（无需重编 `xc2_boot.exe`）。
  2. **让 #15 闪烁修复生效**：删掉安装目录下的 `shader_cache\spirv\`，在启动器再点一次 **Set up** 重译着色器缓存即可。
- **当前在 `v0.2.1` / 更早版本或全新安装**：
  - 解压 `XC2-package-dist-0.2.3.zip`（建议先删 `shader_cache\spirv\` 或使用干净目录迁移 `save\` 与 `upscalers\` 后跑一次 **Set up**）。

## 已知问题

- **130 级暴虐巨神兽库洛戴伊尔 NVIDIA 画面冻结 / TDR（[#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32)）**
- **附加内容重场景负载较高**，以及首次运行约 20 秒管线编译提示。
- **支持平台**：项目初期仅官方支持原生 **Windows 11 x64**，暂不支持 Windows 10 及 Linux / SteamOS / Wine / Proton；未来不排除扩展支持，也欢迎社区自行 Fork 适配其他平台或系统。

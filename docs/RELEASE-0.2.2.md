# XC2 NS2 Recomp v0.2.2 — #39 4K/Quality Rendering Fix & In-Launcher Auto-Updater

This hotfix & quality-of-life release resolves **Issue #39** (the `v0.2.1` 3D dot-grid / sub-resolution scaling regression affecting `Quality` presets and `4K Native`), enables **in-launcher update checks & one-click updates for `v0.2.2+`**, and unifies public documentation.

> **Important upgrade note for `v0.2.1` / `v0.2.0` users**: Earlier launchers (`<= v0.2.1`) still had auto-update checks disabled on the frontend (`update.js`) and upstream placeholder paths in the backend (`update.rs`). Therefore, upgrading *to* `v0.2.2` must be done manually once (either via the full zip or the small `v0.2.1 -> v0.2.2` `xc2_rt.dll` hotfix zip). Starting with **`v0.2.2`**, **About → Updates** checks GitHub releases (including `Pre-release` builds) and updates in place for future versions!

## Which file do I download?

| file | what it is | do I need it? | SHA-256 |
|---|---|---|---|
| `XC2-package-dist-0.2.2.zip` | full player package: updated `0.2.2` launcher (with auto-updater), `xc2_rt.dll`, lifter & shader tools | **yes** — new installs, `v0.2.0` upgrades, or `v0.2.1` users who want `0.2.2+` in-launcher updates | `f970a1841b08d08244dddff3595f0ff482fdade3ca0bf8f9e9c8c27bf662c703` |
| `XC2-hotfix-v0.2.2-from-v0.2.1-rt.zip` | runtime DLL hotfix (`runtime/xc2_rt.dll` + `xc2_rt.dll`) for existing `v0.2.1` installs | optional — `v0.2.1` users who only want the `#39` rendering fix immediately (~12 MB) | `48d100499862539ae4d7063a36d6ba4a351bb711c23e5f791692a6d070643c64` |
| `xc2-launcher-0.2.2-src.zip` | launcher source code | no | `6019cf22671141e20b1b745e6c6be5e0abff945e516ec4cd0e7e58d493a61b6` |

## What's new

### 1. Fix 3D dot-grid / sub-resolution regression in v0.2.1 ([#39](https://github.com/weile10000/xc2-ns2-recomp/issues/39))

- **Root cause**: `v0.2.1` applied `[gfx] output_scale` directly at the swapchain/output layer without updating XC2's internal `native_res` tier, causing both scaled upscaling modes (`Quality` / `Balanced` / `Performance`) and `4K Native` / `DLAA` to sample coarse internal buffers with visible dot-grid artifacts.
- **Fix**: routed `render_quality` through the in-engine `native_res` path (`runtime/include/nxrt/gfx_tuning.h`, `nvn_gpu_texture.cpp`, `xc2_boot.cpp`):
  - **4K + `Native` / `DLAA`**: renders cleanly at full `3840×2160` internal resolution.
  - **4K + `Quality`**: renders internally at `2560×1440` (`1440p -> 4K`) cleanly without checkerboard/dot-grid artifacts.

### 2. Working in-launcher auto-updater (`v0.2.2+`)

- Enabled **Check now** and startup update checks (`launcher/src/update.js`) and adapted `launcher/src-tauri/src/update.rs` & `restart_launcher` (`main.rs`) to `weile10000/xc2-ns2-recomp`:
  - Queries GitHub `/releases?per_page=20` so `Pre-release` (`0.x`) releases are discovered accurately (instead of `/releases/latest`, which ignores pre-releases).
  - Selects `XC2-package-dist-*.zip`, verifies SHA-256 against the GitHub asset digest before unpacking, supports both flat-root and single-folder zip layouts, preserves `save/`, `shader_cache/`, `upscalers/`, `nxrt.ini`, `controls.ini`, `xc2_boot.exe`, and custom `locales/`, rolls back cleanly on error, restarts directly into `xc2-launcher.exe`, and checks whether hook tables / stub sources changed (`rebuild_needed`).

### 3. Documentation & CI hygiene

- Consolidated shader patch counts under [`tools/shader/PATCHES.md`](../tools/shader/PATCHES.md), aligned `README.md` and `BENCHMARK-301.md` preview labels with clean screenshot captures, and added an automated maintainer-path guard in CI.

## Upgrading to v0.2.2

- **Already on `v0.2.1` (Fastest: no Set up rebuild or shader recompile needed)**:
  - **Recommended (Full package + enables future auto-updates)**: extract `XC2-package-dist-0.2.2.zip` over your `v0.2.1` directory (or extract to a fresh folder and copy `save/`, `shader_cache/`, `upscalers/`), then **start the game from `xc2-launcher.exe`** (it copies `runtime\xc2_rt.dll` next to `xc2_boot.exe` automatically). Neither `xc2_boot.exe` nor the shader cache needs rebuilding.
  - **Quick DLL-only hotfix (~12 MB)**: close the game & launcher, extract `XC2-hotfix-v0.2.2-from-v0.2.1-rt.zip` directly into your `v0.2.1` install folder (replacing `runtime\xc2_rt.dll` and root `xc2_rt.dll`). Note that the launcher itself will remain at `0.2.1` until you overwrite it with `0.2.2`.
- **On `v0.2.0` or `v0.1.x` / New installs**:
  - Extract `XC2-package-dist-0.2.2.zip` into a **clean folder**, copy `save/`, `shader_cache/` (and `upscalers/` if downloaded) from your old folder if upgrading, and run Set up once in `xc2-launcher.exe`.

## Known issues

- **Lv. 130 Tyrannotitan Kurodil freeze on NVIDIA ([#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32))**
- **Blinking / dialogue flicker ([#15](https://github.com/weile10000/xc2-ns2-recomp/issues/15))**
- **Add-on scenes & first-run pipeline compile**: Torna / add-on scenes remain heavier than base game areas; first launch compiles Vulkan pipelines (~20 s pause).
- **Red edge** on the cloud sea silhouette in the opening battle background (cosmetic, single scene).

---

# XC2 NS2 重编译 v0.2.2 —— 修复 #39 4K/画质档网格失真 & 启用启动器内自动热更新

本次维护更新重点修复 **#39 号回归问题**（`v0.2.1` 中 4K 原生与 `Quality` 等画质档出现 3D 渲染网格噪点/内部分辨率偏低的问题），同时正式修通并启用了 **`v0.2.2+` 启动器内置检测更新与一键在线更新**，并统一了公开文档口径。

> **`v0.2.1` / `v0.2.0` 老用户升级必读**：早期版本的启动器（`<= v0.2.1`）前端禁用了更新检查（`update.js`），后端仍残留上游占位路径（`update.rs`）。因此**从旧版升到 `v0.2.2` 需要最后一次手动更新**（下载完整包覆盖，或使用仅 12 MB 的 `v0.2.1 -> v0.2.2` 运行时热修小包）；**升级到 `v0.2.2` 之后**，今后发布新版（`v0.2.3+`）即可直接在启动器 **「关于 → 更新」** 内自动检测（含 `Pre-release`）并一键原地更新！

## 我该下哪个文件？

| 文件 | 是什么 | 需要吗 | SHA-256 |
|---|---|---|---|
| `XC2-package-dist-0.2.2.zip` | 完整玩家包：含已修复自动更新的 `0.2.2` 启动器、新版 `xc2_rt.dll`、提升与着色器工具 | **要** —— 全新安装、从 `v0.2.0` 升级，或希望 `v0.2.1` 获得后续启动器一键更新能力的用户 | `f970a1841b08d08244dddff3595f0ff482fdade3ca0bf8f9e9c8c27bf662c703` |
| `XC2-hotfix-v0.2.2-from-v0.2.1-rt.zip` | `v0.2.1` 专用轻量热修补丁（`runtime/xc2_rt.dll` + `xc2_rt.dll`） | 可选 —— 仅需快速修复 `#39` 的 `v0.2.1` 玩家（约 12 MB） | `48d100499862539ae4d7063a36d6ba4a351bb711c23e5f791692a6d070643c64` |
| `xc2-launcher-0.2.2-src.zip` | 启动器源码 | 不用 | `6019cf22671141e20b1b745e6c6be5e0abff945e516ec4cd0e7e58d493a61b6` |

## 本版变化

### 1. 修复 v0.2.1 4K / 渲染质量档网格失真（[#39](https://github.com/weile10000/xc2-ns2-recomp/issues/39)）

- **问题原因**：`v0.2.1` 仅在输出缩放层调整 `[gfx] output_scale`，未同步经由游戏内部 `native_res` 档位驱动内部分辨率，导致 `Quality / Balanced / Performance` 以及部分 `4K Native / DLAA` 组合下仍以较低内部缓冲渲染并放大，产生明显的像素网格感。
- **修复内容**：打通 `render_quality` 与游戏原生 `native_res` 的联动并修正交换链/纹理尺寸处理（`gfx_tuning.h`、`nvn_gpu_texture.cpp`、`xc2_boot.cpp`）：
  - **4K + `Native / DLAA`**：实机完整以 `3840×2160` 原生渲染。
  - **4K + `Quality`**：实机稳定以 `2560×1440 -> 3840×2160` 渲染输出，网格失真消除。

### 2. 启用启动器内在线检测与一键更新（`v0.2.2+`）

- 取消 `launcher/src/update.js` 的早退占位，适配 `launcher/src-tauri/src/update.rs` 及 `main.rs`（`restart_launcher`）为本仓库发行版格式：
  - 通过 GitHub `/releases?per_page=20` 拉取发布列表，正常识别标记为 `Pre-release` 的 `0.x` 版本（不再受 `/releases/latest` 跳过预发布限制）。
  - 自动匹配 `XC2-package-dist-*.zip` 并严格校验 GitHub 官方 SHA-256 摘要，兼容平铺与单顶层目录压缩结构，自动跳过 `save/`、`shader_cache/`、`upscalers/`、`nxrt.ini`、`controls.ini`、`xc2_boot.exe` 与用户自定义语言表，失败可回滚，升级完毕可直接一键重启至新 `xc2-launcher.exe`，并自动检测 `xc2/` hook 表是否变化以提示是否需重跑 Set up。

### 3. 公开文档与隐私守卫规范化

- 将着色器补丁状态统一以 [`tools/shader/PATCHES.md`](../tools/shader/PATCHES.md) 为单一事实来源，修正首页与性能报告预览图尺寸说明，并在 CI 与本地提交检查中加入维护者私有路径守卫。

## 如何升级到 v0.2.2

- **当前已在 `v0.2.1`（无需重新编译 `xc2_boot.exe` 或重编着色器）**：
  - **推荐方案（同步获得 `0.2.2+` 一键更新能力）**：下载完整包 `XC2-package-dist-0.2.2.zip`，直接解压覆盖原 `v0.2.1` 目录（或解压到新目录并复制 `save\`、`shader_cache\`、`upscalers\`），随后**从新版 `xc2-launcher.exe` 点 Play 启动**，启动器会自动将新 `runtime\xc2_rt.dll` 同步到根目录。
  - **极简热修（约 12 MB）**：先完全退出游戏与启动器，下载 `XC2-hotfix-v0.2.2-from-v0.2.1-rt.zip` 解压覆盖进原 `v0.2.1` 根目录（替换 `runtime\xc2_rt.dll` 与根目录 `xc2_rt.dll`）即可；该方式下启动器本体仍停留在 `0.2.1`。
- **当前在 `v0.2.0` / `v0.1.x` 或新用户**：
  - 请解压 `XC2-package-dist-0.2.2.zip` 到**全新干净目录**，从旧目录迁移 `save\`、`shader_cache\`（以及如有下载过的 `upscalers\`），在启动器运行一遍 Set up 即可复用既有着色器缓存。

## 已知问题

- **130 级暴虐巨神兽库洛戴伊尔战斗画面冻结（[#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32)）**
- **眨眼 / 对白眼嘴闪烁（[#15](https://github.com/weile10000/xc2-ns2-recomp/issues/15)）**
- **附加内容重场景负载较高**，以及首次运行约 20 秒管线编译提示。
- 开场战斗背景云海红边（单场景纯视觉）。

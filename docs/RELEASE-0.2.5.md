# XC2 NS2 Recomp v0.2.5 — Minor Maintenance & NVIDIA/Intel GPU Diagnostic Update

`v0.2.5` is a **minor maintenance update** focused primarily on **strengthening GPU fault / `VK_ERROR_DEVICE_LOST` diagnostics for NVIDIA and Intel GPUs**, hardening device-lost / descriptor synchronization paths, and **permanently fixing the NVIDIA 3D oblique black-stripes/hatching regression (`#54` / `#61` / `#62` / `#64`) at runtime**.

> **Upgrading from `v0.2.2`, `v0.2.3`, or `v0.2.4`?**
> 1. **One-click auto-update in the launcher**: Open `xc2-launcher.exe` → **About → Check now → Install update** (`XC2-package-dist-0.2.5.zip`). **No `xc2_boot.exe` rebuild or shader cache wipe is needed** when upgrading from `v0.2.3` or `v0.2.4`!
> 2. *(Upgrading from `v0.2.2` or earlier?)* Still wipe `shader_cache\spirv\` once after updating and click **Set up** so the `#15` shader harness translation fix takes effect.
> 3. **Optional small hotfix for existing `v0.2.4` installs**: If you already run `v0.2.4` and prefer a ~13 MB manual drop-in instead of the full package, extract `XC2-hotfix-v0.2.5-from-v0.2.4-rt.zip` directly over your `v0.2.4` install folder (note that Launcher **About** will keep showing `0.2.4` and `tools/report.py` stays at `0.2.4` unless you update via Launcher or `XC2-package-dist-0.2.5.zip`).

## Which file do I download?

| file | what it is | do I need it? |
|---|---|---|
| `XC2-package-dist-0.2.5.zip` | full player package: `0.2.5` launcher, updated `xc2_rt.dll`, updated `tools/report.py`, `xc2_setup.py`, and toolchain | **yes** — new installs, manual upgrades, or `v0.2.2`–`v0.2.4` one-click **About → Updates** |
| `XC2-hotfix-v0.2.5-from-v0.2.4-rt.zip` | lightweight runtime-only hotfix (`xc2_rt.dll` from `v0.2.5`) | **optional (`v0.2.4` manual quick patch)** — extract over an existing `v0.2.4` folder; checksums in `SHA256SUMS.txt` |
| `xc2-launcher-0.2.5-src.zip` | launcher source code | no |

*(See `SHA256SUMS.txt` on the release page for exact file hashes.)*

## What's new in v0.2.5

### 1. Stronger NVIDIA / Intel GPU Fault & TDR Diagnostics (`010fc5e`, `94cb026`)
- **`#41` `pVendorBinaryData` nullptr crash fix (`vk_device.cpp`)**: guarded vendor binary payload reads in `VK_EXT_device_fault` handling when drivers return `vendorBinarySize > 0` with `pVendorBinaryData == nullptr` (seen on RTX 50-series).
- **NVIDIA Breadcrumb Checkpoints & Expanded Device-Lost Snapshots (`vk_device`, `vk_renderer`, `nvn_replay`, `crash`, `watchdog`)**: added `VK_NV_device_diagnostic_checkpoints` markers around NVN replay draws/submits, richer descriptor/UBO/pool context in `*_devicelost.txt`, fault details on freeze/crash dumps, and automatic collection of `*_devicelost.txt` inside **`Play → Record a report`** (`tools/report.py`) to pinpoint `#32`, `#41`, and Intel Arc (`#63`) GPU errors.
- **Global `device_lost` Guard & Sync Fixes (`nvn_gpu`, `nvn_pipeline`, `nvn_gpu_texture`, `nvn_replay`)**: prevents `UINT64_MAX` (`-4`) timeline values from corrupting `g->completed` and destroying live resources, breaks infinite waits on lost devices, fixes `pool_set()` same-submit descriptor overwrites/deadlock, forces `VK_FILTER_NEAREST` on integer/depth blits (`VUID-vkCmdBlitImage-filter-02001`), and clamps unaligned UBO copies to `br.avail`.

### 2. Permanent Runtime Fix for NVIDIA 3D Black Stripes / Hatching (`#54`, `#44`, `#39`, `#61`, `#62`, `#64`)
- **Extended `[nvn] uint_tex_patch` for Depth-Stencil Samplers (`nvn_replay.cpp`, `nvn_spirv_patch.cpp`)**: at runtime, fragment float samplers reading the stencil plane of `DEPTH32F_STENCIL8` / `DEPTH24_STENCIL8` textures (except explicit `ds_depth_shaders`) are automatically rewritten to integer `usampler` with `VK_FILTER_NEAREST` / `maxAnisotropy = 1.0`.
- Clean installs no longer depend on headless `prefill` timing hitting a stencil-sampling frame or deleting `spirv.preload.pack` manually—the diagonal/horizontal moiré and hatching across 3D terrain and characters on NVIDIA GPUs is fixed automatically on launch.
- **`Op::Barrier` Feedback Sync Fix (`nvn_replay.cpp`) & SPIR-V Validation Cleanup (`nvn_spirv_patch.cpp`, `nvn_pipeline.cpp`)**: preserves non-color pending write records across barrier fast paths, fixes signed integer FS output types, and prunes unattached FS outputs (`Undefined-Value-*` warnings cleared under Vulkan validation).

### 3. F1 In-Game Menu Navigation Fix (`#50`, `play_ui.cpp`)
- Fixed D-pad / analog Up/Down boundary wrapping in the **F1** in-game settings menu so pressing Up at the top row or Down at the bottom row behaves reliably on controllers.

---

# XC2 NS2 重编译 v0.2.5 —— 小幅维护与 N 卡 / I 卡图形诊断增强版

`v0.2.5` 是一次**小幅维护更新**，核心目标在于**加强 NVIDIA / Intel 显卡的图形故障与 `VK_ERROR_DEVICE_LOST`（TDR）诊断收集能力**、加固设备丢失与同步防护，并**在运行时彻底修复 `v0.2.4` 新装后仍可能出现的 NVIDIA 3D 斜向黑条/摩尔纹（`#54` / `#61` / `#62` / `#64`）**。

> **升级提示（针对 `v0.2.2`、`v0.2.3`、`v0.2.4` 用户）：**
> 1. **启动器内一键自动升级**：在 `xc2-launcher.exe` 中点击 **「关于 → 检查更新 → 安装更新」** 即可升级到 `v0.2.5`（`XC2-package-dist-0.2.5.zip`）。从 `v0.2.3` 或 `v0.2.4` 升级**无需重新编译 `xc2_boot.exe`，也无需删除着色器缓存**。
> 2. *（若从 `v0.2.2` 或更早版本升级）*：更新后请删除一次 `shader_cache\spirv\` 并在启动器点击 **Set up** 重译，以生效 `#15` 修复。
> 3. **给 `v0.2.4` 用户的轻量手工补丁（可选）**：不想下载完整包的 `v0.2.4` 用户，也可直接下载 ~13 MB 的 `XC2-hotfix-v0.2.5-from-v0.2.4-rt.zip` 解压覆盖到安装根目录（手工覆盖后启动器“关于”页面版本号与 `tools/report.py` 仍为 `0.2.4`；推荐直接用启动器自动更新）。

## 本次主要更新内容

### 1. 加强 N 卡 / Intel 显卡图形错误与 TDR 诊断能力（`010fc5e`、`94cb026`）
- **修复 `#41` `pVendorBinaryData` 空指针二次崩溃**：防护驱动返回 `vendorBinarySize > 0` 但 `pVendorBinaryData == nullptr` 时的越界崩溃。
- **NVIDIA Breadcrumb 检查点与 `*_devicelost.txt` 自动打包**：接入 `VK_NV_device_diagnostic_checkpoints`、增强 `VK_EXT_device_fault` 故障详情与卡死前的描述符/UBO 上下文快照，并更新 `tools/report.py` 在生成 **Report zip** 时自动收录 `*_devicelost.txt`，便于继续精准定位 `#32`、`#41` 与 Intel Arc（`#63`）图形故障。
- **全局 `device_lost` 防护与同步加固**：阻断 `UINT64_MAX`（`-4`）污染存活资源队列、修复 `pool_set()` 同提交描述符覆写/死锁、强制整数与深度 Blit 使用 `VK_FILTER_NEAREST` 并补齐非对齐 UBO 边界钳位。

### 2. 运行时根治 NVIDIA 3D 斜向黑条/摩尔纹（`#54`、`#44`、`#39`、`#61`、`#62`、`#64`）
- **扩展 `[nvn] uint_tex_patch` 支持深度-模板平面浮点采样器**：运行期自动将读取模板平面的采样器重写为 `usampler` 并强制 `NEAREST` 过滤，**不再依赖安装阶段 `prefill` 是否碰巧跑到游戏内模板帧**，也无需用户再手动执行脚本或删 `spirv.preload.pack`。
- 同步修复 `Op::Barrier` 非颜色待决写入保留与 SPIR-V 签名/悬空输出校验告警。

### 3. 修复 F1 菜单手柄上下边界导航（`#50`）
- 修复游戏内按 **F1** 打开设置面板时，手柄方向键/摇杆在首行和末行边界处的导航跳转问题。

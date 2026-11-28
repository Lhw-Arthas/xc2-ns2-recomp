# Compatibility

**Xenoblade Chronicles 2 – Nintendo Switch 2 Edition**, retail **3.0.1** — title id `0100E95004038000`,
program id `0400E95004038001`.
You need your own dump: the ExeFS of the 3.0.1 program and its romfs. No game data is distributed here.

## Requirements

- **Native Windows 11 x86-64** with AVX2 *(primary official release target covered by regression testing — **Windows 10 x64** and **Linux / SteamOS / Proton / Wine** receive best-effort compatibility without guaranteed full regression support; community PRs and forks adapting other environments are welcome)*
- a GPU with Vulkan 1.3 (`VK_EXT_external_memory_host`)
- 16 GB of RAM; the 3.0.1 romfs alone is 25.6 GB, and the build needs roughly as much again for the generated
  C++ and objects, so keep about 60 GB free during the build
- your own copy of the game and its 3.0.1 update

### GPU & Platform Matrix

| Hardware / Environment | Coverage | Current Notes & Open Issues |
|---|---|---|
| **AMD RDNA 2 / RDNA 4** (RX 6800 XT / 9070 XT) | Primary dev & benchmark | 59–60 fps typical, 43–52 fps open plains (`1440p` HDR10); mirror-capture fixes cloud-sea rim & motion shimmer; `v0.2.3` defers video prewarm (`#42`) and fixes `spirv_uint_samplers` declaration order for `amdvlk64.dll` |
| **NVIDIA** (RTX series) | Community & partial repro | `#15` blinking/dialogue flicker fixed (`v0.2.3`, requires shader cache rebuild); `#54`/`#44`/`#39`/`#61`/`#62`/`#64` 3D oblique black stripes/hatching fixed permanently at runtime via `[nvn] uint_tex_patch` (`v0.2.5`); `#51` dark contour bands fixed via opt-in `[nvn] depth_bias_force_unorm` (`v0.2.4`); `#47`/`#39` 4K `DLAA`/`Quality` scaling restored (`v0.2.4`); **dedicated TDR blocker** tracking `nvoglv64.dll` hangs / `VK_ERROR_DEVICE_LOST` (`#32` Kurodil, `#41` Brighid cutscene, `#59` fast-travel load — `v0.2.5` adds `VK_NV_device_diagnostic_checkpoints`, `VK_EXT_device_fault`, `pVendorBinaryData` null-crash fix, and `*_devicelost.txt` snapshot collection) |
| **Intel GPU / Other environments** | Best-effort compatibility | Primary target is native Windows 11 x64; `#49` fixes Intel `igvk64.dll` `vkGetPipelineCacheData` crash (`v0.2.4`); `v0.2.5` expands `device_lost` / fault reporting & `report.py` collection for Intel/NVIDIA GPU error investigation (`#63`); `#25` / `#33` startup & setup fixes, `#45` `<CETCompat>false</CETCompat>` and `#58` PowerShell 5.1 fallback for Win 10; `#35` / `#52` CPU `dav1d` recovery & SEH stackwalk guard |

## Measured frame rate

Ryzen 7 5800X + Radeon RX 6800 XT, Adrenalin 26.10.1, 1440p window:

| area | fps | note |
|---|---:|---|
| port and most areas | **59–60** | the game's own 60 Hz step |
| the open plain (heaviest) | **43–52** | ~4,400 draws/frame, frame time 18–23 ms |

4K output costs no more than 1440p: the internal chain is fixed at 720p → 1440p and the last step is a blit.

## What works

- boots to the title screen, loads a 2022 clear save, plays in the field
- 60 fps recommended (`tools\run\play-301.cmd -60`, launcher default); the game's logic, timers and animations follow it cleanly. Experimental 90 / 120 fps dual-clock modes are selectable in the launcher (cutscenes and movies stay capped at 60 fps; frame-time swings under VSync/VRR may cause minor flicker)
- the upscaler slot is served by the built-in **TAAU** by default; **FSR 4.1.2** and **FSR 3.1.6** are selectable
  at run time (`[dlss] upscaler = fsr3` + `[upscale] fsr_version`), and FSR 4.1.2 measured no slower than TAAU here
- dynamic resolution is handled: one FidelityFX context per input size (6 in a long session, 0 crashes)
- 1080p / 1440p / 4K window sizes; **HDR10 output**, verified on an HDR display
- **VRR opt-in**: `[gfx] present_mode = fifo_relaxed` — a late frame is presented at once instead of being
  quantised to the next vblank, which turns 43–52 fps into even pacing. Needs VRR on the display and in the
  driver; the default stays `fifo` because most players have no VRR.
- keyboard/mouse and gamepad, with remapping
- add-on content through `NXRT_AOC_DIR` (the 3.0.1 launcher points it at `game/dlc-301`; the files are yours)
- mods through the emulator layout: a `mods` folder next to the executable

## What does not work

| | why |
|---|---|
| **add-on content shipped inside the game** | the retail 3.0.1 program links no `nn::aoc` symbols of its own; add-ons are served by the runtime's `aoc:u` from the directory in `NXRT_AOC_DIR` (the 3.0.1 launcher points it at `game/dlc-301`), and the DLC files themselves are not distributed |

## Known issues

- **Red edge on the cloud sea in the opening battle background**: one thin red line along a cloud-sea silhouette,
  in the same 640×360 volumetric-fog layer the blue-grey rim came from. Low priority - it is one scene, it does not
  move, and it does not affect play.
- The **RDNA 2 blue-grey rim** where objects meet the cloud sea and the **cloud-sea shimmer** in motion are fixed:
  both came from the runtime letting the GPU read pool bytes the CPU had already rewritten, which the mirror-capture
  work in pull requests 19 and 27 captures per submission (verified on an RX 6800 XT, 2026-11-22).
- **Frame generation is not fully reliable yet.** With `[dlss] frame_gen` on (FSR 3 or DLSS-G) two things have been seen
  and are not fixed: facial detail can still micro-flicker in close-ups (lips, eyebrows) even though the same scene is
  clean with frame generation off, and a pre-rendered cutscene can stall with the picture frozen while audio keeps
  playing. That stall leaves **no `xc2_boot_freeze.txt`** — frame generation keeps presenting interpolated frames, so the
  "no frame presented in 10 s" watchdog never fires and the usual report shows nothing. Measured 2026-11-26 on an RX 6800
  XT: `v0.2.5` played the DLC opening with frame generation on, while a build carrying the retail-3.0.1 frame-rate hook
  (`[xc2] fps > 0`) froze it; the same build with `[xc2] fps = 0` played it. Turn frame generation off before reporting a
  visual bug so the two are not confused.
- **First run prepares shader cache (`shader_prepare`)**: boot-time shader preparation builds GPL pipeline libraries ahead of gameplay from embedded program pairs extracted during setup, eliminating steady-play and multi-map compile hitches once prepared.
- **Slow frames on area transitions** (texture uploads and memory-pool imports), not in steady play.
- **Shader cache & NVIDIA validation state**: stencil-plane sampler fixes (`VUID-vkCmdDrawIndexed-format-07753`) are
  reproduced automatically on the player's own shader cache by
  [`tools/shader/patch_ds_samplers.py`](../tools/shader/patch_ds_samplers.py), and the retired `2649526f56d9b5b6`
  volumetric-fog workaround was removed on 2026-11-17. [`tools/shader/PATCHES.md`](../tools/shader/PATCHES.md) is the
  single source of truth for the remaining unautomated/per-shader items.
- **Dedicated blocker — NVIDIA GPU TDR / `VK_ERROR_DEVICE_LOST` ([#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32), [#41](https://github.com/weile10000/xc2-ns2-recomp/issues/41), [#59](https://github.com/weile10000/xc2-ns2-recomp/issues/59))**:
  reproduces on NVIDIA GPUs under heavy particle overdraw or map-transition concurrency (logs consistently show `nvn-render` stalled inside `nvoglv64.dll` with `timeline value = UINT64_MAX` / `fs be528292f3d04f1b` in the final draw ring). Tracked as a dedicated blocker for root-cause investigation.

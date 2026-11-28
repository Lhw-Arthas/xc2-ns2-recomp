# STATUS — dated milestones

A dated record of verified milestones and active limits. Detailed public records live in
[CHANGELOG.md](CHANGELOG.md), [docs/RELEASE-0.2.1.md](docs/RELEASE-0.2.1.md), [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md),
and [docs/reports/](docs/reports/).

| date | milestone | public reference |
|---|---|---|
| 2026-11-09 | First static lift of a Switch title (4 modules, 90,691 functions) and NVN → Vulkan replay rendering a frame | [LIFT-REPORT.md](docs/reports/LIFT-REPORT.md) · [ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| 2026-11-10 | Models & materials: block-linear layouts, tile modes, texture formats, deferred pass | [ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| 2026-11-11 | Lighting pipeline: light lists, probe volumes, integer render-target logic op | [ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| 2026-11-12 | Volumetric fog pipeline identified; initial 60 Hz pacing experiments | [PATCHES.md](tools/shader/PATCHES.md) |
| 2026-11-13 | Early-Z mask fix, 1080p output, replay phase split, 60/120 Hz pacing | [MODIFICATIONS.md](third_party/fefw-recomp/MODIFICATIONS.md) |
| 2026-11-14 | Frame-rate census, Switch 2 add-on (`pkg7`) analysis, DLC mount chain | [COMPATIBILITY.md](docs/COMPATIBILITY.md) |
| 2026-11-15 | **Retail 3.0.1 route boots**: 128 GiB alias region (`VammManager`), Opus + `nn::audio` HLE, tensor-core upscaler translated (Ampere SASS), 30.0 fps in-game; shader cache 21,171; `earlyz_mask`, logic-op fix, `quad_split` | [LIFT-REPORT.md](docs/reports/LIFT-REPORT.md) · [BUILD.md](docs/BUILD.md) |
| 2026-11-16 | **Dynamic resolution FidelityFX fix & replay hot-path optimization**: one LRU FidelityFX context per input size (6 contexts across `960x540`..`1280x720`, 0 crashes), replay trimmed from `9.7–15.4 ms` to `5.3–6.8 ms`; 1080p/1440p **60 Hz** (`59.7 fps`, 1% low `51.3` on TAAU, `59.7 / 50.8` on FSR 4.1.2; heaviest open plain ~4,400 draws at `43–52 fps`) | [COMPATIBILITY.md](docs/COMPATIBILITY.md) · [BENCHMARK-301.md](docs/reports/BENCHMARK-301.md) |
| 2026-11-17 – 2026-11-22 | **Portable releases (`v0.1.0`–`v0.2.1`)**: Tauri launcher, self-contained setup, official DLSS & FSR downloaders, 4K AV1 hardware video, RDNA 2 cloud-sea rim & shimmer fix, 8 GB VRAM `mirror_capture`, #37 audio crackling fix | [CHANGELOG.md](CHANGELOG.md) · [RELEASE-0.2.1.md](docs/RELEASE-0.2.1.md) · [#30](https://github.com/weile10000/xc2-ns2-recomp/pull/30) · [#36](https://github.com/weile10000/xc2-ns2-recomp/pull/36) |
| 2026-11-23 | **Mod support M2/M2b verified**: RomFS file overlay, `bf2.ard` inline record patching, and `ard_tail` appended replacement with patched `bf2.arh` (`xbc1` 48-byte headers) | [MODS.md](docs/MODS.md) |
| 2026-11-24 | **`v0.2.3` release**: M4 Mods UI / DMNT Cheats VM, `#15` morph flicker root fix (`spirv_harness`), cutscene freeze fixes (`movie_adec` coalesced wakeup + `#35` CPU recovery + `#42` 9070 XT prewarm + PR `#48`), `#44` RTX 40 shadow bias fix, `#47`/`#39` 4K `DLAA`/`Quality` restore, `#45` CETCompat, `spirv_uint_samplers` ordering fix | [RELEASE-0.2.3.md](docs/RELEASE-0.2.3.md) · [COMPATIBILITY.md](docs/COMPATIBILITY.md) |
| 2026-11-24 | **`v0.2.4` release**: `#53` FSR 3 FG `HUDLessColor` + HDR parity, `temporal_ffx` FSR `3.1` default + `4.*` dynres `slots_` isolation, F10 dual FPS HUD + wrap fix, `#49` Intel `vkGetPipelineCacheData` fix, `#51`/`#54` opt-in `depth_bias_force_unorm`, `#52` `dav1d` EOS/SSE2, `#55`/`#56` dynres fixes, `#57` HDR highlight expansion, `#58` PS 5.1 fallback + custom `game_dir`, 1080p native TAAU docked baseline, Frame Gen all-GPU & visual notice | [RELEASE-0.2.4.md](docs/RELEASE-0.2.4.md) · [COMPATIBILITY.md](docs/COMPATIBILITY.md) |
| 2026-11-25 | **`v0.2.5` maintenance & diagnostic release**: runtime `#54`/`#44`/`#39`/`#61`/`#62`/`#64` depth-stencil float→`usampler` rewrite + `Op::Barrier` feedback fix (`6242d23`), SPIR-V unsigned/unattached output validation cleanup (`8048d6e`), `#41` `pVendorBinaryData` null-crash fix + `VK_NV_device_diagnostic_checkpoints` / `VK_EXT_device_fault` / `*_devicelost.txt` diagnostics & `report.py` collection (`010fc5e`, `94cb026`), `#50` F1 controller Top/Bottom boundary nav | [RELEASE-0.2.5.md](docs/RELEASE-0.2.5.md) · [COMPATIBILITY.md](docs/COMPATIBILITY.md) |

## Current limits

* **Default window (`1920×1080`)**: as of `v0.2.4`, fresh setups default to a clean docked **1080p Native TAAU (`lock_render_size = 1`, `60 fps`, `frame_gen = 0`)** baseline; `1440p` / `4K` output modes and upscalers (FSR 3/4, DLSS) remain selectable in Launcher / F1 Settings.
* **Open plain GPU/CPU load**: reaches ~4,400 draws/frame (`~9 ms` replay CPU time, `18–23 ms` total frame time = `43–52 fps` on RX 6800 XT); VRR (`fifo_relaxed`) or future draw-batching optimizations target closing the `1.5–6 ms` gap to locked 60 fps.
* **Texture uploads & pool imports**: area transitions can still introduce occasional hitch frames while streaming pools import and pipelines warm.
* **GPU vendor coverage & NVIDIA TDR blocker**: primary verification is on AMD RDNA 2; NVIDIA has a dedicated tracking blocker for driver TDR / `VK_ERROR_DEVICE_LOST` hangs in `nvoglv64.dll` (`nvn-render` stalling under high-concurrency particle/transition loads ending around `fs be528292f3d04f1b`, tracked across [#32](https://github.com/weile10000/xc2-ns2-recomp/issues/32), [#41](https://github.com/weile10000/xc2-ns2-recomp/issues/41), and [#59](https://github.com/weile10000/xc2-ns2-recomp/issues/59)). See [COMPATIBILITY.md](docs/COMPATIBILITY.md).

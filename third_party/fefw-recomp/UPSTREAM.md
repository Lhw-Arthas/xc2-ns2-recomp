# Upstream provenance

This directory is a fork of **Goldoire/fefw-recomp** (the upstream GitHub repository has since been removed / returns **404**), vendored into this project (it is tracked by the outer
repository, not a git submodule).

| | |
|---|---|
| upstream | `Goldoire/fefw-recomp` *(removed on GitHub / 404; `third_party/fefw-recomp/` is the self-contained fork base)* |
| the fork is based on | an upstream state before **v0.2.0** (0.1-era runtime, 380 source files under runtime/src) |
| latest upstream release | **v0.2.0** (2026-11-13) — `runtime/src` has **394** files |
| snapshot of v0.2.0 | **kept outside the repository** (558 files of vendored upstream, not worth the tree): moved to `work/ref/fefw-recomp-ref-0.2.0/`, which git ignores; the pristine extract also sits at `<downloads>\fefw-recomp-src-0.2.0` |
| archive | `<downloads>\fefw-recomp src-v0.2.0.zip` — 7,280,826 bytes, SHA-256 `d5199b6be980f3dd936941c043e18a9b0b43a62c1916732d57f1982d9d554242` |
| tag object | `aff2c65c72fd8f448542df27141dae9f84a39077` |
| licence | see `LICENSE` and the upstream `LEGAL.md` (kept here) |

**Why this file exists**: the upstream repository may be deleted at any time. If it goes, the vendored copy under
`third_party/fefw-recomp` is the canonical source of everything this build uses, and the snapshot above is the last
upstream state we have. The runtime itself is never fetched at build time; the *other* build inputs are: CMake's
`FetchContent` pulls SDL3, Vulkan-Headers, VMA, ImGui and zstd (see `runtime/cmake/gfx_deps.cmake`), and
nxrecomp is cloned into `third_party/nxrecomp` by the user. See `docs/BUILD.md`.

## What v0.2.0 has that this fork does not (assessed 2026-11-14)

New files: `gfx/nvn/vn_astc_gpu.cpp/.h`, `vn_astc_alpha.cpp`, `vn_bc1.cpp`, `vn_residency.cpp/.h`,
`texture_upload_readiness.h`, `gfx/haptic.cpp`, `gfx/extras/temporal_xess.cpp`, `ffx_files.cpp/.h`,
`gfx/menu_i18n.cpp`, `gfx/render_status.cpp`, `gfx/vk_layer_filter.cpp/.h`.

53 new configuration keys, in four groups:

* **texture / ASTC on the GPU**: `[gfx] astc_gpu, astc_gpu_cache, astc_gpu_cache_read, astc_gpu_ms, astc_gpu_upgrade,
  astc_bc, texture_detail, texture_eco, budget_detail_gib, pageable_memory, pressure_detail_percent, amd_overallocation`,
  `[nvn] astc_gpu_fail_test, astc_gpu_probe, astc_gpu_slow, lean_draws`
* **residency / pool**: `[gfx] residency, residency_dim, residency_percent, residency_idle_s, residency_restore_*,
  residency_test_budget_mb`, `[nvn] mirror_hot_copy, pool_mirror_pressure_percent, depth_pool_readiness`
* **replay hot path**: `[nvn] ubo_only_rebind, lean_ab, storage_read_copy, gfx_ssbo_readonly, push_uniforms,
  pipeline_cache_lock, int_fetch_retype`
* **diagnostics / extras**: `[gfx] disable_overwolf_layers, ui_font, ui_language`, `[diag] fail_small_pool_import,
  fail_small_vk_allocation, load_census`, `[test] crash_after_s, crash_kind`, `[dlss] xess_*`, `[fs] romfs_files, mods`

This fork has 31 keys upstream does not have, all diagnostics of ours (`earlyz`, `earlyz_mask`, `earlyz_bit`,
`cull_off_all`, `depth_off_fs`, `gob_switch1`, `barrier_keeps_pass`, `draw_occ`, …) plus `[boot] guest_relinks`.

# tools/ — debugging and analysis

Reusable tools only. One-off scripts from a single investigation live in the (git-ignored) `work\` tree; anything
here should still be useful to the next person.

All scripts resolve their paths relative to themselves (`os.path.dirname(__file__)`, `$PSScriptRoot`,
`%~dp0`), so the whole directory can be copied to another machine.

## ci/

| file | what |
|---|---|
| `check_invariants.py` | the project rules: no private paths, no game data, ASCII build inputs, CRLF in .cmd files |
| `check_links.py` | every relative link in the tracked markdown resolves |
| `config_table.py` | keeps `config/keys.tsv`, `docs/CONFIG.md` and the code in step: `--emit` writes the page, `--check` fails on a new undocumented key |
| `install-git-hooks.ps1` | copies `hooks/pre-commit` into `.git/hooks`; the hook runs the two checks above on every commit |
| `hooks/pre-commit` | the hook itself (needs `python` on PATH) |

## setup/

| file | what |
|---|---|
| `xc2_setup.py` | the first-run program: doctor, sources, runtime build, lift, link, ingest a `.nup` |
| `doctor.ps1` | what this machine is missing, checked against `deps.lock.json`; `-Json` for the setup page |
| `check-env.ps1` | whether the toolchain is complete, and repairs the Python packages it can copy locally |
| `deps.lock.json` | the pinned external sources and their hashes |

## shader/

| file | what |
|---|---|
| `spv_rename.py` | renames the UBO instance names a glslang round-trip produces back to `fp_c3`/`fp_c4`/`support_buffer`; NVN binds by name, so a shader installed without this does nothing |
| `patch_ds_samplers.py` | retypes the depth-stencil samplers the console leaves implicit (see `PATCHES.md`) |
| `cache\` | the archive scan and the cache builder: `ard_index.py`, `extract_embedded.py`, `build_cache.py`, `pack.py`; `cache/README.md` has the numbers |

The loop for a hand-patched shader:

```
glslangValidator -V x.frag -o x.spv
python tools\shader\spv_rename.py x.spv x_r.spv support_buffer_1=support_buffer fp_c1_1=fp_c1 fp_c3_1=fp_c3
copy x_r.spv xc2\build\boot\shader_cache\spirv\<hash>.spv
```

With `NXRT_NVN_SPV_HOT` set (see `run\hot.cmd`) the replacement takes effect in about 0.25 s. Drop
`spirv.preload.pack` first, or the pack keeps serving the old shader.

## run/

| file | what |
|---|---|
| `play.ps1` | starts the game from `config/profiles/<name>.ini`: paths, switches and frame rate are data, not set lines. `-Headless`, `-Fps`, `-Dump`, `-Shots`, `-Seconds` for scripted runs |
| `config/profiles/dev.ini` | the 3.0.1 development build: exe, working directory, romfs/dlc, save, cache and the 20 switches |
| `play-301.cmd` | the 3.0.1 route: `-60` for 60 fps, `-headless`, `-dump` to collect shaders, language switches |
| `play60-301.cmd` | the same at 60 fps, double-clickable |
| `hot.cmd` | launch with shader hot-swap on |
| `ab.cmd` | launch with the "skip this draw" switch on (`NXRT_NVN_SKIP_FS_FILE`), 0.5 s to take effect |
| `up.cmd` | switch the upscaler at run time (1 TAAU / 2 spatial / 3 bilinear / 4 FSR) |
| `ab-mode.cmd`, `ab-am.cmd` | A/B the operation mode and the `[am]` resolution/scale |
| `capture_win2.ps1` | PrintWindow capture of the game window, DPI aware, no focus needed |
| `play-ns1.cmd` | the older 3.0.0 route (a Switch 1 program as the guest), kept for comparison |

## analysis/

### reading PFM captures (linear HDR, F12)

`pfm_probe.py`, `pfm_rows.py`, `pfm_cols.py`, `pfm_win.py`, `pfm_cmp.py`, `pfm_dist.py`, `pfm_rg.py`.
The scale is `raw = PFM / 2.78`; a tonemapped PNG is not a number.

### trace and draw analysis

| file | what |
|---|---|
| `draws.py`, `draw_table.py` | pull the draw list out of a log |
| `order.py` | draw order of one shader |
| `trace_query.py`, `texdump_scan.py` | query trace entries, scan a texture dump |
| `trace_depth.py` | group a frame's transparent draws by target, shader, blend and effective depth state |
| `snap_list.py` | per-draw history of a render target or texture from `trace_snap_rt` / `trace_snap_tex` |
| `ubodump.py` | print one draw's uniform and storage buffers as floats |
| `ab_read.py`, `fsdiff2.py`, `ubodiff.py` | offline A/B of captures, shader diffs, UBO diffs |
| `crop.py`, `stretch.py`, `glsl.py` | small image and GLSL helpers |

### runtime measurements

| file | what |
|---|---|
| `replay_stats.py` | reads the `nvn.rstats` lines (`[nvn] replay_stats = N`): per-phase µs to ms per frame, reconciled with the `setup` column, and the remaining margin against a 60 fps budget |
| `session_census.py` | scans `work\logs\play-*.log`: sessions, total time and crashes grouped by the upscaler in use |
| `pe_rva.py` | maps `module+0x...` from a crash stack to the containing function (PE sections, exports, `.pdata`) |
| `vk_entry_map.py` | names a Vulkan ICD's entry points: loads the DLL as an instance and enumerates every prototype, giving an RVA to name table |

### archives and symbols

| file | what |
|---|---|
| `nso_seg.py` | decompresses NSO segments with a pure-Python LZ4 decoder |
| `symat.py` | address to `module!symbol+offset` (MOD0/.dynsym); `--dump` writes a symbol table |
| `dis.py` | disassembles a range with `llvm-objdump` and labels `bl`/`b` targets |
| `bdat_cols.py`, `bdat_table.py`, `bdat_scan.py` | bdat column names, one table, or a scan of an archive |
| `ard_get.py` | extract a record from `bf2.ard` by name |
| `nup_extract.py` | list, verify or unpack a `.nup` package |

## build/

| file | what |
|---|---|
| `make-package.ps1` | assembles the portable tree: launcher, `xc2_rt.dll` + import library, the tools that compile the lifted code (Python, Zig, spirv-val, harness), and the setup program. Contains no game file. |
| `PACKAGE-README.txt` | the readme that ships inside that tree |

## The rest of build/

| file | what |
|---|---|
| `build.ps1` | lift + compile the modules (the lift is the long part) |
| `build-boot.ps1` | link `xc2_boot.exe` against the lifted libraries |
| `build-release.ps1` | the release split: `xc2_rt.dll` plus a Zig-linked exe |
| `build-census.ps1` | the frame-counter census build, in directories of its own |
| `build-shaders.ps1` | make the shader cache and the preload pack complete (`-Check` reports only) |
| `patch-shaders.ps1` | check or install the hand-patched modules (`-Check` exits 0 when the cache is consistent) |
| `make-portable.ps1` | copy a built tree somewhere else |
| `verify-package.ps1` | check a package tree, or an install made from one: no debug leftovers, the files the release promises, the shader cache count (`-ExpectedSpirv`) |

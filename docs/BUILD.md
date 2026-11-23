# Building

Windows x86-64 only. The build has three parts: the runtime libraries (C++/Vulkan), the lifted game code
(the NSO modules translated to C++), and the boot executable that links them.

The reference toolchain is described in [docs/ENVIRONMENT.md](ENVIRONMENT.md); this page is the shortest path.

> In a hurry? [QUICKSTART.md](QUICKSTART.md) has the five steps and the common failures. This page is the full
> build, including the shader cache.

## 1. Toolchain

| what | version used here | why |
|---|---|---|
| Visual Studio Build Tools (MSVC + Windows SDK) | 2022+, MSVC 14.44, SDK 10.0.26100 | clang-cl takes its headers and import libraries |
| LLVM | 23.1.5 | `clang-cl`, `llvm-lib`, `llvm-objdump`, `llvm-cxxfilt` |
| CMake + Ninja | 3.25+ | the runtime build |
| .NET SDK | 10 | the shader harness (Ryujinx's translator) |
| Vulkan SDK | 1.4.363+ | `spirv-as`, `spirv-dis`, `spirv-val`, `glslc` |
| Python | 3.12, with `lz4` and `capstone` | the lifter (`third_party/nxrecomp`) |
| Git | any | |
| PowerShell | **7+** (`pwsh`) | the build uses `ForEach-Object -Parallel`, which Windows PowerShell 5.1 does not have; run the scripts with `pwsh`, not `powershell.exe` |

Either put all of them on `PATH`, or set **`XC2_ENV`** to a PowerShell script that does; the build scripts
dot-source it when it is set. **`XC2_PYTHON`** names the interpreter if `python` on `PATH` is not the one
with `lz4`/`capstone`.

```powershell
$env:XC2_ENV    = 'C:\path\to\toolchain\env.ps1'
$env:XC2_PYTHON = 'C:\path\to\python.exe'
```

## 2. Sources that are not checked in

| what | how |
|---|---|
| `third_party/nxrecomp` (the recompiler) | from the repository root, unpack [nxrecomp-e7a5a54-xc2.tar.gz](https://github.com/weile10000/xc2-ns2-recomp/releases/download/v0.1.0/nxrecomp-e7a5a54-xc2.tar.gz) so that `third_party/nxrecomp/` appears (4.6 MB, sha256 `6b140959ada4d74b1843783736569db03ab307d8b84fb376da9c77cdc0729419`) |
| `third_party/deps` (SDL3, Vulkan-Headers, volk, VMA, ImGui, zstd, dav1d) | fetched by CMake's `FetchContent` on the first configure (`runtime/cmake/gfx_deps.cmake`) |
| Ryujinx's shader translator | `pwsh -File third_party/fefw-recomp/tools/shaders/scripts/setup_ryujinx.ps1`, then `dotnet build third_party/fefw-recomp/tools/shaders/harness/SpirvHarness.csproj -c Release` |

**Do not `git clone` the recompiler.** The upstream repository rewrote its history on 2026-11-16: the commit this
build needs (`e7a5a54`) is no longer fetchable — git answers `not our ref`, the API returns 422 — and the current
default branch has a different layout (`base/` and `games/` where this build expects `recomp/`, `runtime/` and
`tools/`). A fresh clone therefore produces a tree nothing here can use. The release asset above is that exact
commit (GPL-3.0-or-later) **plus this project's three recompiler patches and the `xc2ns2` game profile**, which live
in `third_party/nxrecomp` and are git-ignored, so they would otherwise be lost with the upstream commit.

The translator script needs no arguments. It stages the three projects it uses (Ryujinx.Common, Spv.Generator,
Ryujinx.Graphics.Shader) from a local checkout if you have one at `third_party/fefw-recomp/refs/ryubing`, and
otherwise downloads the Ryubing fork's own archive — `git.ryujinx.app/projects/Ryubing`, commit `a82350bb77`,
MIT, 12.2 MB, no account. That archive is the only external source besides the recompiler and CMake's deps; pass
`-Ryubing <checkout>` to use a different one.

Verified from an empty tree: the script downloads 12.2 MB in about 5 s and the harness then builds in about 15 s
with 0 errors.

## 3. Your game files

The recompiler reads the game's own ExeFS; nothing is downloaded by this repository.

```
xc2/game/xc2ns301/       main, sdk, subsdk0, subsdk1, subsdk2, subsdk3, rtld, main.npdm   (the 3.0.1 program)
<your romfs 3.0.1>/      bf2.ard, bf2.arh, stream/            (NXRT_FS_ROMFS at run time)
<your dlc>/              optional                               (NXRT_AOC_DIR at run time)
```

That is the **folder** layout. If your dump is a single `.nup` package instead — the format a55uka's tool writes —
unpack it first; the two parts land exactly where the paths above expect them:

```powershell
# the program (exefs): main, main.npdm, sdk, subsdk0..3, rtld
python tools\analysis\nup_extract.py --nup <your dump>.nup --pkg 0 --part exefs --out xc2\game\xc2ns301

# the romfs: bf2.ard, bf2.arh, stream/
python tools\analysis\nup_extract.py --nup <your dump>.nup --pkg 0 --part romfs --out <your romfs 3.0.1>

# the add-on content, if you have it (package 7)
python tools\analysis\nup_extract.py --nup <your dump>.nup --pkg 7 --out <your dlc>
```

`--list` prints the packages a `.nup` holds and `--verify` checks every extent and SHA-256 first, which is
worth doing once: it catches a truncated dump before the build blames itself. It needs `zstandard`
(`pip install zstandard`) only for packages that are actually compressed.

## 4. Build

### 4.1 the runtime (once, and after changing `third_party/fefw-recomp/runtime/**`)

The graphics shell links an ASTC-to-BC7 transcoder, so its two source dependencies have to be present first. This
fetches astc-encoder 5.7.0, bc7enc_rdo and the ISPC compiler into `tools/textures/third_party` (git-ignored):

```powershell
pwsh -File third_party\fefw-recomp\tools\textures\scripts\fetch_third_party.ps1
```

Then the two builds. Pass the compilers explicitly: a `clang.exe` on `PATH` otherwise wins over `clang-cl` and the
build dies on MSVC-only flags.

```powershell
# the graphics shell
cmake -S third_party/fefw-recomp/runtime -B third_party/fefw-recomp/build/runtime-gfx `
      -G Ninja -DCMAKE_BUILD_TYPE=Release -DNXRT_BUILD_GFX=ON -DNXRT_BUILD_GAMES=OFF -DNXRT_BUILD_TESTS=OFF `
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build third_party/fefw-recomp/build/runtime-gfx --target nxrt nxrt_gfx

# the game module the boot executable links against (nxrt_game_xc2ns2_core / _data)
# -DNXRT_GAMES=xc2ns2 is NOT optional: without it NXRT_GAMES defaults to "fefw", the configure succeeds, and
# nothing named nxrt_game_xc2ns2_* exists, so the link below fails with "unknown target".
cmake -S third_party/fefw-recomp/runtime -B third_party/fefw-recomp/build/xc2-game-probe `
      -G Ninja -DCMAKE_BUILD_TYPE=Release -DNXRT_BUILD_GFX=ON -DNXRT_BUILD_GAMES=ON `
      -DNXRT_GAMES=xc2ns2 -DNXRT_TEST_GAME=xc2ns2 -DNXRT_BUILD_TESTS=OFF `
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build third_party/fefw-recomp/build/xc2-game-probe `
      --target nxrt_game_xc2ns2_core nxrt_game_xc2ns2_data
```

The second configure is what `build-boot.ps1` expects: it links `build/xc2-game-probe/games/xc2ns2/*.lib`. It is a
separate build directory because the game module needs the runtime built with `NXRT_BUILD_GAMES=ON`, which the
graphics shell above deliberately turns off.

### 4.2 lift and compile the guest modules (13 s + 4 s for `main`, longer for the rest)

The lifter also needs the hook sites: `xc2/sites/sites-<module>.txt` ship with the repository and are picked up
automatically. Put your own additions in `xc2/build/sites-<module>.txt`, which takes precedence — see
[xc2/sites/README.md](../xc2/sites/README.md).

`-LiftDir`, `-ObjDir` and `-LibDir` choose where the lift, the objects and the per-module libraries go. The
defaults (`build\lift`, `build\obj`, `build\lib`) are the four-module development route; the retail command above
keeps its own `-301` directories so the two never overwrite each other.

```powershell
.\tools\build\build.ps1 -Exefs xc2\game\xc2ns301 -Run:$false `
    -Modules 'main,sdk,subsdk0,subsdk1,subsdk2,subsdk3,rtld' `
    -LiftDir xc2\build\301\lift -ObjDir xc2\build\301\obj -LibDir xc2\build\301\lib
```

The stub link at the end of `build.ps1` always fails — that is expected, `build-boot.ps1` does the real link.

### 4.3 the boot executable (~50 s)

```powershell
.\tools\build\build-boot.ps1 -Exefs xc2\game\xc2ns301 -OutDir xc2\build\301\boot `
    -LibDir xc2\build\301\lib -MainLib xc2\build\301\lib\main.lib -Defines XC2_MODULES_7
```

Produces `xc2\build\301\boot\xc2_boot.exe`, which is what `tools\run\play-301.cmd` runs. Without the flags the
script links the older four-module route into `xc2\build\boot\`.

One library comes from outside the tree: **libvpx** (BSD-3, the host VP9/VP8 decoders) is compiled into `nxrt` and
therefore linked into this executable even though nothing in the boot stub mentions it. The script finds it through
`VCPKG_ROOT` and the usual vcpkg install roots (`vcpkg install libvpx:x64-windows-static`); set `VCPKG_ROOT` if
yours lives elsewhere. A runtime configured without libvpx still links, but leaves VP9 movies out.

### 4.4 shaders

Shaders are translated offline into `xc2\build\boot\shader_cache\spirv` (SPIR-V keyed by the shader hash).

**The cache has two sources, and a fresh install ends at 12,137.** Scanning the romfs alone is not enough, and the
number to remember is **12,137** = 12,132 translated from the archive + 5 that exist in no file:

| source | count | tool |
|---|---|---|
| the embedded groups in `bf2.ard` (12,126) plus `ndsr.pck`'s 28 = 12,154 archive blocks | **12,132** translated | `tools\shader\cache\extract_embedded.py` |
| blocks that exist in **no file at all** (the game builds them at run time) | +5 = **12,137** | only `NXRT_NVN_DUMP_MISSING` |

The 22-block gap between 12,154 and 12,132 is misaligned slices the translator rejects; the scan missed nothing. A
cache with only the embedded groups (12,104) renders a **black title screen** — that is the failure this section
exists to prevent. Measured on the portable package: the archive scan yields 12,132 `.spv` in 187 s at 10 jobs.

**12,154 is an upper bound, not the target** (corrected 2026-11-19, handover 163). `ndsr.pck` is the game's own
tensor-core upscaler (NDSR) container. Its control table names 12 further compute variants that the retail title
**never dispatches**: 0 hits across 8,502 recorded requests on the docked machine and 2,184 on a fresh handheld
session (234 s, 60 fps, 0 missing-shader lines). They are deliberately **not** installed — adding them would put
dead entries in the cache, and the four NDSR kernels the game does run every frame are already in 12,132. A shipped
setup therefore stops at 12,137, and that is a complete cache.

```powershell
<python> tools\shader\cache\build_cache.py --ard <romfs>\bf2.ard --cache <install>\shader_cache\spirv `
    --harness <tools>\spirv_harness\spirv_harness.exe --work <install>\build\shader-cache --jobs 10
```

**First launch.** Whatever the archive cannot name is collected by running the game **headless for about 24 seconds**
(`NXRT_NVN_DUMP_MISSING`), translating those blocks, and only then opening the window. Progress is printed as
`PREFILL:` lines and written to `work\prefill.status` (`phase`, `elapsed`, `budget`, `collected`, `note`) so a
front end can show it. Collection runs once per cache (`work\.prefilled`); `-noprefill` skips it, and a collector
that dies early leaves the marker unwritten so the next launch tries again.

**Later launches** need nothing. New areas keep asking for shaders the cache has never seen: those are collected when
the game exits and translated, so the *next* launch is complete. The first time you enter an area with unseen
shaders, that area renders with gaps until you restart.

**Rebuilding offline.** `tools\shader\cache\` walks your own dump and needs nothing else:

```powershell
python tools\shader\cache\ard_index.py        --ard <romfs>\bf2.ard --out work\files.tsv
python tools\shader\cache\extract_embedded.py --ard <romfs>\bf2.ard --tsv work\files.tsv --out work\blocks
python tools\shader\cache\build_cache.py      --blocks work\blocks `
       --cache xc2\build\boot\shader_cache\spirv --harness <spirv_harness.exe>
```

That finds **12,154** blocks in 3.0.1 in about five minutes. It is not the whole set: some shaders the game uses are
not in any file — they exist only at run time — so the launcher's collection is what closes the last gap. Neither
path alone is complete.

**Stencil samplers.** `format-07753` (a float sampler reading a stencil plane as an integer) is undefined behaviour
on NVIDIA and can end in a lost device. `tools\shader\patch_ds_samplers.py` retypes the affected samplers; the
launcher runs it automatically after a session that reported them, using your own log, so nothing derived from the
game is distributed. The runtime names each of them once per (shader, binding) as `ds_debug: fs <hash> ...` in every
log — the `[nvn] ds_debug` switch used to gate that, which meant the one run that mattered, the one that lost the
device, recorded nothing to patch from (issue #10, 2026-11-19).

## 5. Run

```powershell
.\tools\run\play-301.cmd            # the 3.0.1 route; see the header of that file for every switch
```

Logs go to `work/logs/play-301-<time>.log`.

## 6. Notes

* Never commit `xc2/game/**`, `xc2/build/**` or `work/**`: they hold game data and multi-GB build products
  (`.gitignore` already covers them).
* The shader cache is keyed by shader hash: do not delete it. If a translation changes, translate the affected
  hashes and let `tools/build/patch-shaders.ps1` (or `tools/shader/cache/build_cache.py`) replace those entries.
* The single-instance rule: `xc2_boot.exe` maps its data files exclusively — after killing it, wait ~25 s before
  starting it again, and close the game before linking.

# xc2ns301 — Xenoblade Chronicles 2 (Switch 2 Edition), the game-specific half

This directory holds everything that is specific to this game: the boot glue, the module table and the lift
configuration. The build, link and shader scripts live in `../tools/`. The general runtime lives in `../third_party/fefw-recomp`; the recompiler
in `../third_party/nxrecomp` (clone it there yourself, it is not vendored).

## Build

```powershell
# lift the seven 3.0.1 modules and compile them (from the checkout root)
.\tools\build\build.ps1 -Exefs xc2\game\xc2ns301 -Run:$false -Modules main,sdk,subsdk0,subsdk1,subsdk2,subsdk3,rtld

# link the executable the launcher runs
.\tools\build\build-boot.ps1 -Exefs xc2\game\xc2ns301 -OutDir xc2\build\301\boot -LibDir xc2\build\lib `
    -MainLib build\lib\main.lib -Defines XC2_MODULES_7
```

The stub link at the end of `build.ps1` always fails; that is expected. See `../docs/BUILD.md`.

## Layout

| path | what |
|---|---|
| `src/` | boot harness (`xc2_boot.cpp`), the Opus and `nn::audio` shims, ABI stubs (`xc2_stub.cpp`), and smoke stubs (`stub_main.cpp`, `rt_stub.cpp`) |
| `sites/` | hook/patch/inline site tables (`sites-main.txt`, `sites-sdk.txt`, `sites-rtld.txt`) |
| `release/` | CMake build & `.def` exports for the split `xc2_rt.dll` package runtime |
| `recomp/xc2ns2/config.py` | the module table and build ids the lifter reads |
| `tests/` | input scripts for the smoke test |

## Input

The ExeFS of the 3.0.1 program, from your own dump, in `game/xc2ns301/` (`main`, `sdk`, `subsdk0..3`, `rtld`,
`main.npdm`). `-Exefs` or `$env:XC2_EXEFS` overrides the path. Nothing from the game is committed.

## Licence

GPL-3.0-or-later; see `LICENSE` and `PROVENANCE.md`.

# Contributing

## Reporting a problem

Open an issue with:

- the build you ran (the release tag or the commit),
- your GPU, driver and Windows version,
- the log from `work\logs\` (one file per launch, written by `tools\run\play-301.cmd`; it is timestamped so it
  survives the next start),
- and, if the problem is visual, the area of the game and what you expected to see.

If you can, say whether the problem also happens with `[dlss] upscaler = taau` and with
`[gfx] window_width/height` set to 1280x720 — that separates renderer problems from upscaler problems.

## Building

See [docs/BUILD.md](docs/BUILD.md). In short: a Windows toolchain (MSVC + LLVM clang-cl, CMake, Ninja,
.NET SDK, Vulkan SDK, Python 3.12), your own game dump in `xc2/game/`, then

```powershell
.\tools\build\build.ps1 -Exefs xc2\game\xc2ns301 -Run:$false `
    -Modules 'main,sdk,subsdk0,subsdk1,subsdk2,subsdk3,rtld' `
    -LiftDir xc2\build\301\lift -ObjDir xc2\build\301\obj -LibDir xc2\build\301\lib      # lift and compile the seven modules
.\tools\build\build-boot.ps1 -Exefs xc2\game\xc2ns301 -OutDir xc2\build\301\boot `
    -LibDir xc2\build\301\lib -MainLib xc2\build\301\lib\main.lib -Defines XC2_MODULES_7
```

## Before you open a pull request

- Read [docs/INVARIANTS.md](docs/INVARIANTS.md) — the three rules above all else.
- Keep the guest data out of the diff. Nothing under `xc2/game/` may be committed.
- If you change a shader patch, run `tools\build\patch-shaders.ps1 -Check` and make sure it exits 0.
- If you change a file under `third_party/fefw-recomp/`, add the change to that directory's
  `MODIFICATIONS.md` (GPL-3.0 requires a modification notice).
- If you add or change runtime config keys (`config_get_*`), F1 settings (`play_config.cpp`), or `config/nxrt.ini.in`,
  update `config/keys.tsv`, regenerate `CONFIG.md` with `python tools/ci/config_table.py --emit`, and run the local CI
  checks (`check_invariants.py`, `check_links.py`, `config_table.py --check`, `check_play_settings.py`,
  `check_install_template.py`, plus `python -m unittest discover -s tools/tests -p "test_*.py" -v`).
- For pre-release versions (`0.x`), tag releases as **pre-release** and publish a `SHA256SUMS` table for all attached
  zip/tarball assets alongside the release notes.
- Say which build you tested on, and what you measured. Numbers with a scene and a build are worth more than
  "it feels faster".

## AI assistance

The maintainer works with an AI pair-programmer (credited in the README). Issues and pull requests may be
answered with its help; every reply is reviewed before it is posted, and the maintainer stands behind its content.

## Licence

The project is **GPL-3.0-or-later**; by contributing you agree to license your work under the same terms.
Third-party components keep their own licences — see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and
`third_party/fefw-recomp/MODIFICATIONS.md`.

# Working on this repository

A note for whoever is reading the code next — a person, or an assistant with no memory of the last session.
It says what the project is, what must not break, and where to look. Everything else is in [docs/](docs/).

## What this is

Static recompilation of *Xenoblade Chronicles 2 — Nintendo Switch 2 Edition* (retail 3.0.1) to native Windows
and Vulkan. The game's NSO modules are lifted to C++ by [nxrecomp](https://github.com/drakolordx7/nxrecomp) and
linked against the runtime in `third_party/fefw-recomp`, which replays the game's NVN/NVN2 command stream on
Vulkan. It is not an emulator: there is no interpretation at run time.

[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) has the picture; [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) has
the measured frame rates and the known issues.

## Ground rules

These are not style preferences. Each one exists because breaking it cost a session.

1. **No game data, ever.** No NSO modules, no romfs, no keys, no lifted C++, no shader cache, and nothing
   derived from the game's own assets (a hand-edited shader counts — it is still the game's code). The rule of
   thumb: *if it cannot be reconstructed without the game files, it does not belong here.* CI checks the
   obvious cases; `docs/INVARIANTS.md` has the reasoning.
2. **The shader cache is append-only.** Never rebuild or delete it. It holds 21,171 translated programs, and
   `spirv.preload.pack` is read before the loose `.spv` files, so a stale pack silently wins over an edited
   shader. If you change a shader patch, run `tools\build\patch-shaders.ps1 -Check` and expect exit 0.
   **A cache built from the romfs alone is never complete:** the archive yields 12,154 blocks (12,126 embedded
   groups + 28 in `ndsr.pck`), 12,132 of them translate, and 5 more exist in no file at all — the game builds
   them at run time, so they only arrive through `NXRT_NVN_DUMP_MISSING`. A fresh install therefore ends at
   **12,137**, which is what a complete cache has; a cache of 12,104 renders a black title screen. Any new build,
   package or setup path must run the archive scan **and** the prefill; see handover 139 for the chain and the
   checklist. `ndsr.pck` also names 12 compute variants in its control table — those are never dispatched (zero
   hits in 8,502 docked and 2,184 handheld requests, measured 2026-11-19), so no setup path may add them; handover
   163 has the numbers.
3. **The game runs in one instance, started by the operator.** Automation must not terminate or restart it.
   Reading logs and snapshots at any time is fine. After it exits, the next launch waits about 25 seconds.
4. **Absolute paths in scripts and commands.** Relative paths break as soon as the working directory moves,
   and several build scripts change directory part-way through.

## Layout

The checkout holds only what cannot be rebuilt elsewhere. Everything heavy has one home outside it, reached by a
junction, so no script has to know about a second copy:

| path | what |
|---|---|
| `xc2\` | the game-specific half: `src`, `sites`, `tests`, the release CMake project, the module table |
| `tools\` | `build` (build and package), `run` (start the game), `shader` (cache and patches), `analysis`, `ci` (the gates), `setup` |
| `launcher\` | the launcher sources; its built exe is what `make-package.ps1` copies |
| `config\` | the machine-readable settings table (`keys.tsv`) and the undocumented backlog |
| `docs\`, `licenses\`, `third_party\` | reference, licences, upstream code |
| `work\` → external `work` dir (junction) | logs, screenshots, scratch |
| `xc2\game\` → external `exefs` dir (junction) | the user's own dump; `xc2\build\boot\game\*` likewise for romfs and DLC |

`python tools/ci/check_invariants.py` fails when a new top-level entry appears, so the shape above is a rule and not
a habit. The rules that go with it:

1. **One entry point per intent**: `tools/build/build*.ps1`, `tools/run/play*.cmd`, `tools/build/make-package.ps1`.
   A new script is a subcommand of one of them, not a new name at the root.
2. **Directory names carry no date and no version.** Versions go in arguments, file names or `build.json`.
3. **One-off scripts never go in `tools\`.** Keep them outside the repository (e.g. an external archive directory); `tools\` is for what the next person uses.
4. **Data has one physical copy.** The junctions above are the only second names for it.
5. **Build outputs stay out of the checkout**: `xc2\build\` holds `boot` (runtime cwd and shader cache), `301`,
   `census`, `release` — four directories, nothing else.

## Build and run

[docs/QUICKSTART.md](docs/QUICKSTART.md) is the five-step version with the failures people actually hit.
[docs/BUILD.md](docs/BUILD.md) is the full build, including the shader cache.

```powershell
pwsh -File third_party\fefw-recomp\tools\textures\scripts\fetch_third_party.ps1   # once
# two cmake configures, see QUICKSTART section 3
.\tools\build\build.ps1 -Exefs xc2\game\xc2ns301 -Run:$false `
    -Modules 'main,sdk,subsdk0,subsdk1,subsdk2,subsdk3,rtld' `
    -LiftDir xc2\build\301\lift -ObjDir xc2\build\301\obj -LibDir xc2\build\301\lib
.\tools\build\build-boot.ps1 -Exefs xc2\game\xc2ns301 -OutDir xc2\build\301\boot `
    -LibDir xc2\build\301\lib -MainLib xc2\build\301\lib\main.lib -Defines XC2_MODULES_7
.\tools\run\play-301.cmd -60
```

## Where to look

| question | file |
|---|---|
| how does the lift, the runtime and the shader pipeline fit together | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| what runs, what does not, how fast | [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) |
| it builds but the screen is wrong, or the driver crashes | [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) |
| the toolchain, and the switches that print diagnostics | [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md) |
| the rules, and why they exist | [docs/INVARIANTS.md](docs/INVARIANTS.md) |
| how films are decoded and why the end-of-stream protocol looks inverted | [docs/MEDIA.md](docs/MEDIA.md) |
| the mod path: asset catalog (M0), what a mod may replace, where its tools and output live | [tools/mods/README.md](tools/mods/README.md) and [docs/MODS.md](docs/MODS.md) |
| the hook sites the lifter uses | [xc2/sites/README.md](xc2/sites/README.md) |
| third-party components and their licences | [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) |

## Before you open a pull request

```powershell
python tools/ci/check_links.py          # every relative link resolves
python tools/ci/check_invariants.py     # no private paths, no game data, ASCII build inputs
```

The second one also runs as a git hook (`pwsh -File tools/ci/install-git-hooks.ps1` installs it) and in CI.
If you change a file under `third_party/fefw-recomp/`, add the change to that directory's `MODIFICATIONS.md`
— GPL-3.0 requires a modification notice.

## If you have the maintainer's local tree (`docs/internal/`)

This repository is developed alongside git-ignored local notes: `docs/internal/` holds the working state, the
decision log, and round-by-round history, while `work/` holds logs and scratch.

**Mandatory cross-session protocol (applies to every harness/agent, even if started mid-conversation without an onboarding prompt):**

1. **At session start**: read `docs/internal/AGENTS.md` and `docs/internal/STATE.md` (and any parent workspace onboarding card reported below), then run:

```powershell
pwsh -File tools/agent-brief.ps1
```

   Do **not** scan all of `docs/internal/handover/` upfront — grep only inside `docs/internal/handover/` when needed. Full maintainer rules live in `docs/internal/AGENTS.md`.

2. **Before finishing any code/doc task**: follow the closeout checklist in `docs/internal/AGENTS.md`:
   - Overwrite `docs/internal/STATE.md` (current status, verified facts, next steps)
   - Append newly settled decisions with `§` numbers to `docs/internal/DECISIONS.md`
   - Write `docs/internal/handover/NN-<date>-<topic>.md` and update `docs/internal/handover/_index.json`
   - Update any external onboarding card (keeping its hardlink to the maintenance copy intact if used) and run external maintenance sync if configured
   - Run the closeout verification & commit tracked files cleanly by path:

```powershell
pwsh -File tools/agent-brief.ps1 -VerifyCloseout
```

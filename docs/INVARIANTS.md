# Project invariants

Three rules the project holds itself to. They are not style preferences: breaking one has cost a session's
work, a corrupted cache or a lost build in the past.

## 1. The game runs in a single instance, owned by the operator

The game runs on the operator's machine, in one instance, and the operator decides when it starts and stops.
Automation must **not** terminate or restart it. Read-only access to logs and snapshots is unrestricted at any
time. Any experiment that needs a different launch configuration is prepared as a command and run by the
operator; after the process exits, the next launch waits about 25 seconds (the runtime releases the mapping
lazily).

## 2. The shader cache is append-only

The translated shader cache is only ever added to. It is not rebuilt or deleted: a rebuild costs the whole
corpus (21,171 programs) and has silently shadowed hand-edited `.spv` files before (`spirv.preload.pack` takes
precedence over loose files). When a shader patch is edited, run `tools\build\patch-shaders.ps1 -Check` and
expect exit code 0.

The three rules below are checked, not just written: `python tools/ci/check_invariants.py` runs locally, a git
hook refuses a commit that breaks them (`pwsh -File tools/ci/install-git-hooks.ps1`), and CI runs the same checks.

## 3. No game data, lifted code, shader cache or keys are committed or released

The repository contains tooling, runtime code and documentation only. The game's files, the C++ the lift
produces from them, the shader cache built from them and any key material stay on the machine that owns the
game. Game-derived builds (`xc2_boot.exe` and lifted module libraries) are not distributed: portable releases
ship only the launcher, generic runtime, and build toolchain, while anything embedding lifted game code is
built locally from the player's own dump.

### Where that rule has an edge: the hook tables

`xc2/sites/sites-<module>.txt` is tracked, and it lists addresses inside the game. Those addresses cannot be
found without the game's files, so by the letter of the rule they would be private. They stay public because
the alternative is worse: without them the lifter emits no hook at all, every `hook_add_entry` in
`xc2_boot.cpp` silently does nothing, and a fresh clone builds a binary that runs but cannot be diagnosed or
tuned.

The line is **bytes**. An `entryhook` or `midhook` line is an address and a name — no game bytes. A `patch`
line carries the instruction word it expects and the one it writes, which is the game's own code, so those
belong in the untracked `xc2/build/sites-<module>.txt`. The shipped file states this itself
("patch 71002a8828 is NOT shipped"), and `check_invariants.py` now enforces it: a private override carrying one
patch shadowed the shipped list for two days and stopped a from-zero build in 0.8 s (2026-11-17).

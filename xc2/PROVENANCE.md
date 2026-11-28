# Provenance and licences

The project is **GPL-3.0-or-later**. Upstream code it uses:

| project | used for | licence | version / snapshot |
|---|---|---|---|
| [drakolordx7/nxrecomp](https://github.com/drakolordx7/nxrecomp) | the AArch64 → C++ static recompiler | GPL-3.0-or-later | commit `de23ddc8e2447fe6b4ec626807a31dab7ab4b807` (2026-11-02), cloned to `third_party/nxrecomp` |
| `Goldoire/fefw-recomp` *(upstream removed / 404)* | the runtime fork base (kernel/SVCs, IPC services, NVN2 → Vulkan, diagnostics) | GPL-3.0-or-later | snapshot 2026-11-09 (`6f1d964`), vendored & maintained self-contained at `third_party/fefw-recomp`; **the list of changes is `third_party/fefw-recomp/MODIFICATIONS.md`** |
| [seedds/nxnative](https://github.com/seedds/nxnative) (branch `xc2-ns2-profile`) | the Opus HLE path: `dm/host/hle_codec.c` → `xc2/src/x2_opus301.cpp` | MIT (see `LICENSE.nxnative.MIT`) | ported 2026-11-16 |
| [AksharDP/xc2-recomp](https://github.com/AksharDP/xc2-recomp) (Switch 1) | **read-only reference**: findings cross-checked by file and line, no code copied | MIT | from 2026-11-09 |

`fefw-recomp` is itself built on nxrecomp; its `recomp/THIRD_PARTY.md` records the finer sources (smo-recomp,
SuperMarioWonderRecomp, ssbu-recomp and others). This project inherits those notices through the fork and
**removes no copyright header**.

## Game data

- This repository contains no game code, no keys and no assets. The ExeFS is extracted by the user from their own
  dump and placed in `xc2\game\xc2ns301\` (or `xc2ns2` for the older 3.0.0 route); that directory is
  git-ignored, and `tools\run\play.ps1` and `tools\build\build-boot.ps1` default to it.
- The lifted C++ under `work/` is derived from the game (recompiled output) and is likewise not committed.
- Only sources and build scripts are distributed.

## What this project adds

- the scaffolding under `src/`, `sites/` and the build scripts under `tools/`;
- `xc2/recomp/xc2ns2/config.py`, the module table and build ids the lifter reads. `third_party/nxrecomp/` is a
  local, git-ignored dependency, so the lift configuration lives in this repository and is read from here at build
  time; a fresh clone has to put nxrecomp into `third_party/nxrecomp` itself.

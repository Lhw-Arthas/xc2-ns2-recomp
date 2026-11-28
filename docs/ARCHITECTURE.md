# Architecture

How a retail Switch 2 build of *Xenoblade Chronicles 2* becomes a native Windows program.

```
ExeFS (NSO modules)  ──nxrecomp lift──▶  C++  ──clang-cl──▶  main.lib  ─┐
                                                                        ├──▶  xc2_boot.exe
runtime (fefw-recomp: kernel, services, NVN/NVN2 → Vulkan)  ───────────┘
        │
        └─ NVN calls ──▶ command replay ──▶ Vulkan ──▶ SPIR-V (offline shader harness)
```

## The lift

The game's NSO modules are translated to C++ ahead of time by [nxrecomp](https://github.com/drakolordx7/nxrecomp)
(ARM64 → C++). The retail 3.0.1 build lifts completely (numbers from the 3.0.1 exefs; the older development build measured
differently — 49,908 functions in `main` — see `docs/reports/LIFT-REPORT.md`):

| module | functions | instructions | untranslated |
|---|---:|---:|---:|
| `main` | 48,965 | 3,031,179 | 0 |
| `sdk` | 45,922 | 2,148,015 | 0 |
| `subsdk0` | 2,033 | 97,245 | 0 |
| `subsdk1` | 2,154 | — | 0 |
| `subsdk2` | 9,970 | — | 0 |
| `subsdk3` | 1,533 | — | 0 |
| `rtld` | 46 | 2,408 | 0 |

The generated C++ compiles into ~537 translation units and links into a **144.5 MB** executable.

## The runtime

`third_party/fefw-recomp` (a self-contained vendored fork of the removed `Goldoire/fefw-recomp` Switch runtime, see its
[MODIFICATIONS.md](../third_party/fefw-recomp/MODIFICATIONS.md) and [UPSTREAM.md](../third_party/fefw-recomp/UPSTREAM.md)) provides everything the console would:

- **kernel**: SVCs, memory (the 39-bit address space with the Switch 2 128 GiB alias region), threads, handles.
- **services**: `fsp-srv` (romfs, saves), `am`/`vi` (display), `hid` (input), `aud`/`hwopus`, `aoc`, `time`.
- **NVN / NVN2**: the game's graphics API is **replayed**, not re-implemented. The guest's command stream is
  recorded and executed against Vulkan, with the game's own render passes, barriers and presents preserved.

## The shaders

Two sources, one pipeline:

- **Maxwell** (`.wishp`, control block stride 0x848) — translated from SASS to SPIR-V by Ryujinx's translator.
- **Ampere / NVN2** (control block stride 0xd8) — the same translator plus our own SASS front end.

Translation runs **offline**, at build time, into a shader cache (21,171 programs). The game never waits for a
translation at run time; only the driver's pipeline compile happens on first use.

## Where the frame time goes (measured)

At 1440p, in the heaviest area (the open plain, ~4,400 draws/frame):

| | ms/frame |
|---|---:|
| replay (CPU: resolve, descriptor sets, draw calls) | 9.0 |
| GPU (all render passes) | 7.3 |
| present / sync | the rest |

(The same scene measured 6.7 ms of GPU at 4K, which is the point of the next paragraph: the output size is not
what costs.)

The output resolution is nearly free: 4K costs no more than 1440p, because the internal chain is fixed at
720p → 1440p and the final step to the window is a single blit.

## Repository layout

| path | what |
|---|---|
| `xc2/` | the game-specific half: boot glue, lift configuration, scripts, the shader patch harness |
| `third_party/fefw-recomp/` | the runtime fork (GPL-3.0), see its MODIFICATIONS.md |
| `tools/` | analysis and run tools (`run/`, `analysis/`, `env/`, `probe/`, `shader/`) |
| `docs/` | build, environment, architecture, compatibility, and the reports under `docs/reports/` |

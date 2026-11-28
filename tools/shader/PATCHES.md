# tools/shader/PATCHES.md

Every shader this project modifies, how it is made, and whether a clean build can reproduce it.

**Why this file exists**: a patched shader is a modified copy of the game's own module, so it cannot be published.
A clean build therefore gets **none** of them, and its picture is not the maintainer's picture. That difference has
already cost us once - the `format-07753` fix only exists as patches, so a fresh clone still shows the NVIDIA
black screen until the tool below is run.

## Made by a tool (a clean build can reproduce it)

### The stencil-plane samplers - `patch_ds_samplers.py`

Five fragment shaders read a depth-stencil texture's **stencil plane** through a **float** sampler. The console lets
the 8-bit value land in the float register with its bits unchanged; Vulkan cannot, and an `S8_UINT` view against a
float sampler is `VUID-vkCmdDrawIndexed-format-07753` (AMD tolerates it, NVIDIA does not - that is the black screen).

```
pwsh -File tools/run/report.ps1          # or: $env:NXRT_NVN_DS_DEBUG = 1; .\tools\run\play-301.cmd
python tools/shader/patch_ds_samplers.py --cache xc2\build\boot\shader_cache\spirv --log work\logs\play-301-<stamp>.log
```

The runtime names the shaders (`ds_debug:` lines with the set and binding), the tool declares the sampler `uint` and
bitcasts the fetched value back to float, so the bits are identical and the descriptor is legal.

Measured on the retail 3.0.1 cache: five shaders, one sample instruction each.

## Hand-made (a clean build does NOT get these)

| shader | what it changes | why it is still manual |
|---|---|---|
| `87d073eae1cc6f14` | also its **location 1** output: `vec4` + `intBitsToFloat(_150)` becomes `uvec4` + `uint(_150)` | it is a per-shader edit with no rule behind it |

**Verified necessary on 2026-11-17.** With the module at 23,088 bytes (stencil sampler only) the validation layer reports
`ShaderFragmentOutputMismatch` at **location 1 and location 0**; at 23,120 bytes (with the output fix) only
**location 0** remains, which is a different shader. So this is not dead weight the way the froxel patch was.

The remaining location 0 belongs to `d3aa3541df429da5` (the light list) and **cannot be a static patch**: the same
shader also renders into `R32G32_SFLOAT` elsewhere, so declaring its output integer breaks that case - tried, and the
VUID simply changed direction. Fixing it properly means teaching `spirv_outputs_to_uint` to bitcast sint to uint,
which touches a 400-line patcher and the source of early-Z's light list (handover 47, section 101.9).

### Retired: the froxel patch, `2649526f56d9b5b6`

It forced a guard in the volumetric-fog raymarch (`_248 = _192`) to `true`, because the stepping block was being
skipped whole and the fallback constant 1.0 covered the screen with a flat blue haze — visible as a hard-edged band
just above the horizon when looking up (HANDOVER 05 section 39.1, 06 section 40.4).

**Removed on 2026-11-17**: with the module back to the game's own, the band does not appear. Something between
2026-11-12 and now fixed the cause, so the workaround is dead weight. The retired module is kept in an external
backup (`shader-patch-retired/` outside the repo) in case a scene turns up where it still matters — it was
verified at one place, not everywhere.

That is the rule for every entry above: a workaround outlives its reason silently unless someone puts the original
back and looks.

Both are kept in `xc2/game/shader-patch/` (git-ignored, never published). If the picture of a clean build has to
match this one, they have to become tools too - and the second one needs a per-draw decision, not a static patch
(see handover 47, section 101.9).

## Not a patch at all

`tools\run\skip_uvdiv.txt` is a **skip list** consumed by `[nvn] skip_fs_file`: those shaders are not run rather
than modified. An empty file means nothing is skipped.

## The rule

Before adding a hand-made patch, ask whether the change can be expressed as a **rule over the player's own cache**
(like `patch_ds_samplers.py`) instead of a file. A rule can be published; a file cannot.

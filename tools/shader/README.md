# tools/shader

Working on the game's own shaders: the modules in `xc2/build/boot/shader_cache/spirv`.

Nothing here contains game data. Each tool reads the player's own cache and writes it back.

`cache/` holds the other half of the pipeline: the archive scan and the translator that build
`shader_cache/spirv` from the player's own dump. See `cache/README.md` for the numbers.

## patch_ds_samplers.py

Fixes the one thing the console does and Vulkan cannot: a **float sampler reading a depth-stencil texture's
stencil plane**. On Maxwell the 8-bit value lands in the float register with its bits unchanged, and the
deferred lighting's class mask reads it back with `floatBitsToInt(x) & 24`. Vulkan has no such format, so the
runtime binds an `S8_UINT` view while the shader declares a float sampler, which is
`VUID-vkCmdDrawIndexed-format-07753` and undefined behaviour. AMD tolerates it; NVIDIA does not.

The tool declares the sampler as `uint` and bitcasts the fetched value back to float, so the bits are identical
and the descriptor is legal.

```powershell
# 1. once, with the probe on: the runtime names every shader that takes that path
$env:NXRT_NVN_DS_DEBUG = 1
.\tools\run\play-301.cmd

# 2. rewrite those modules in your own cache
python tools/shader/patch_ds_samplers.py --cache xc2\build\boot\shader_cache\spirv --log work\logs\play-301-<stamp>.log
```

Which samplers need it cannot be known statically (it depends on what the game binds at draw time), which is why
the runtime reports it and the tool reads that report. Each module is backed up as `<hash>.spv.orig` the first
time it is patched.

Measured on the retail 3.0.1 cache: five shaders, one sample instruction each, and afterwards no shader takes the
stencil path and the VUID is gone.

### What it does to the module

```
OpTypeImage %float ...            ->  a private copy with %uint, so sibling samplers of the same type are untouched
%r = OpImageSample* %v4float ...  ->  %r = OpImageSample* %v4uint ...
                                      %f = OpBitcast %v4float %r   (new; every use of %r now uses %f)
```

Downstream code is untouched: it still sees a `v4float` whose bits are the stencil byte.

## spv_rename.py

Renames `OpName` strings. glslang names a uniform block's instance `<block>_1` when the block and the instance
share a name, and NVN finds a descriptor by the name the translator baked in, so a hand-built module has to be
renamed back before the runtime will use it. See `tools/build/patch-shaders.ps1` for installing rebuilt shaders into a
cache (including dropping `spirv.preload.pack` so loose files are not shadowed).

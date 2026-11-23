# SPDX-License-Identifier: GPL-3.0-or-later
# Assembles the portable tree: the launcher, the runtime as a DLL, and the two tools that compile the lifted code, so
# a player unpacks it and needs nothing installed - no MSVC, no Windows SDK, no Python, no .NET, no Zig.
#
#   pwsh tools\build\make-package.ps1 -Out E:\XC2-package
#
# What it does NOT contain: any game file. The exe is built on the player's PC from their own dump (that is the whole
# point of the recompilation), and the shader cache is translated from their own shaders.
param(
    [string]$Out = '',
    [string]$Launcher = '',   # default: <checkout>\launcher
    [string]$Zig = 'E:\Tools\dev\zig',
    [switch]$SkipHarness,
    [switch]$SkipPython,
    [switch]$AllowNoFFmpeg    # build a package whose 4K openings fall back to the CPU decoders, on purpose
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # the checkout
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $root) 'XC2-package' }

function Say($text) { Write-Host ("  " + $text) }
function Need($path, $what) { if (-not (Test-Path $path)) { throw "missing $what : $path" } }

# robocopy, not Copy-Item: the Zig distribution carries symlinks and Copy-Item leaves a half-built tree behind.
function CopyTree($src, $dst) {
    New-Item -ItemType Directory -Force (Split-Path $dst -Parent) | Out-Null
    $null = robocopy $src $dst /E /SL /NFL /NDL /NJH /NJS /NP
    if ($LASTEXITCODE -ge 8) { throw ("robocopy failed ($($LASTEXITCODE)) copying " + $src) }
}
function RemoveTree($p) {
    if (-not (Test-Path $p)) { return }
    try { [System.IO.Directory]::Delete($p, $true) } catch { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }
}
function Size($path) {
    try {
        $s = (Get-ChildItem $path -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        if ($s) { return [math]::Round($s / 1MB, 1) }
    } catch { }
    return 0
}

Write-Host "package -> $Out"
if (Test-Path $Out) { Say 'removing the previous one'; RemoveTree $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

# --- 1. the launcher ---------------------------------------------------------------------------------------------
if (-not $Launcher) { $Launcher = Join-Path $root 'launcher' }
if (-not (Test-Path (Join-Path $Launcher 'package.json'))) { throw "no launcher sources at $Launcher (the checkout carries them in launcher\)" }
$exe = Join-Path $Launcher 'src-tauri\target\release\xc2-launcher.exe'
Need $exe 'the built launcher'
# The launcher must be built with `npm run tauri build -- --no-bundle`, never with a bare `cargo build`: the CLI is
# what embeds the frontend, and a cargo build produces an exe that looks for the dev server on 127.0.0.1:1420 instead
# (2026-11-18: shipped like that once, every player saw ERR_CONNECTION_REFUSED). The frontend bundle it embeds must be
# newer than its sources, which is what the CLI guarantees and a hand-run cargo build does not.
# The frontend bundle the CLI embeds: inside the launcher tree (launcher\dist) since the sources moved into the
# checkout; older layouts kept it beside the checkout, which is why both are looked at.
$dist = Join-Path $Launcher 'dist\index.html'
if (-not (Test-Path $dist)) { $dist = Join-Path (Split-Path -Parent $Launcher) 'dist\index.html' }
if ($dist -and (Test-Path $dist)) {
    if ((Get-Item $exe).LastWriteTime -lt (Get-Item $dist).LastWriteTime) {
        throw "the launcher ($exe) is older than its frontend ($dist): rebuild it with 'npm run tauri build -- --no-bundle'"
    }
}
# The version the About page shows comes from tauri.conf.json at build time, so an exe built before the version was bumped
# reports the old release - 0.1.4 shipped a launcher that said 0.1.3 that way (issue #24, 2026-11-21). Compare the two numbers,
# not just the timestamps.
$conf = Join-Path $Launcher 'src-tauri\tauri.conf.json'
if (Test-Path $conf) {
    $want = (Get-Content -LiteralPath $conf -Raw | ConvertFrom-Json).version
    $have = (Get-Item $exe).VersionInfo.ProductVersion
    if ($want -and $have -and ($have -notlike "$want*")) {
        throw "the launcher ($exe) reports $have while tauri.conf.json says $want - rebuild it with 'npm run tauri build -- --no-bundle', or the About page shows the wrong release"
    }
    Say ("launcher version {0} (tauri.conf.json {1})" -f $have, $want)
}
Copy-Item $exe $Out -Force
Say ("launcher      {0:N1} MB" -f ((Get-Item $exe).Length / 1MB))

# --- 2. the runtime, as a DLL and the import library to link against ---------------------------------------------
$dll = Join-Path $root 'xc2\build\release\dll\xc2_rt.dll'
$lib = Join-Path $root 'xc2\build\release\dll\xc2_rt.lib'
Need $dll 'xc2_rt.dll (pwsh tools\build\build-release.ps1)'
Need $lib 'xc2_rt.lib'
New-Item -ItemType Directory -Force (Join-Path $Out 'runtime') | Out-Null
Copy-Item $dll, $lib (Join-Path $Out 'runtime') -Force
Say ("runtime       {0:N1} MB  (xc2_rt.dll + the import library)" -f (((Get-Item $dll).Length + (Get-Item $lib).Length) / 1MB))

# FFmpeg (GPL-3.0-or-later shared build): the film's hardware decoders (libavcodec + D3D11VA). The runtime loads them from
# its own directory, so they go in runtime and the setup copies every runtime*.dll next to xc2_boot.exe. A package without
# them still runs - every 4K opening falls back to the CPU decoders (8-17 fps against 30).
$ffmpeg = ''
foreach ($cand in @($env:NXRT_FFMPEG_DIR, 'E:\XC2\lan\ffmpeg\ffmpeg-master-latest-win64-gpl-shared')) {
    if ($cand -and (Test-Path (Join-Path $cand 'bin\avcodec-63.dll'))) { $ffmpeg = $cand; break }
}
if ($ffmpeg) {
    $ffDlls = @('avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll', 'swresample-7.dll', 'swscale-10.dll')
    $ffBytes = 0
    foreach ($n in $ffDlls) {
        $srcDll = Join-Path $ffmpeg ('bin\' + $n)
        if (Test-Path $srcDll) { Copy-Item $srcDll (Join-Path $Out 'runtime') -Force; $ffBytes += (Get-Item $srcDll).Length }
    }
    $lic = Join-Path $Out 'licenses'
    New-Item -ItemType Directory -Force $lic | Out-Null
    if (Test-Path (Join-Path $ffmpeg 'LICENSE.txt')) { Copy-Item (Join-Path $ffmpeg 'LICENSE.txt') (Join-Path $lic 'FFMPEG-LICENSE.txt') -Force }
    Say ("ffmpeg        {0:N1} MB  (GPL-3.0 shared build, the film's hardware decoders)" -f ($ffBytes / 1MB))
} else {
    # The first 0.1.4 package went out like this and nobody noticed until a player's log said "built without FFmpeg"
    # (handover 64, §237): a warning is not enough for the one thing that decides whether 4K openings play.
    if (-not $AllowNoFFmpeg) {
        throw ('FFmpeg not found: point NXRT_FFMPEG_DIR at a GPL shared build (bin\avcodec-63.dll), or pass ' +
               '-AllowNoFFmpeg to ship a package that decodes 4K openings on the CPU')
    }
    Write-Warning 'FFmpeg not found: the package ships no hardware film decoder, so 4K openings run on the CPU'
}

# --- 3. the tools: an embeddable Python with the lifter in it, the shader harness, Zig -----------------------------
$py = Join-Path $root 'xc2\build\release\tools\python'
if (-not $SkipPython) {
    Need (Join-Path $py 'python.exe') 'the embeddable Python (pwsh tools\build\build-release.ps1)'
    CopyTree $py (Join-Path $Out 'tools\python')
    Say ("python        {0:N1} MB  (capstone, lz4, zstandard, nxrecomp)" -f (Size (Join-Path $Out 'tools\python')))
}
$harness = Join-Path $root 'third_party\fefw-recomp\tools\shaders\build\publish\win-x64'
if (-not $SkipHarness) {
    Need (Join-Path $harness 'spirv_harness.exe') 'the published shader harness'
    CopyTree $harness (Join-Path $Out 'tools\spirv_harness')
    Say ("harness       {0:N1} MB  (self-contained, no .NET)" -f (Size (Join-Path $Out 'tools\spirv_harness')))
}
# spirv-val: the harness validates every translation with it, and a machine without the Vulkan SDK has none - the
# shader step then died with an unhandled Win32Exception (issue #5, 2026-11-18). SPIRV-Tools, Apache-2.0.
if (-not $SkipHarness) {
    $val = ''
    if ($env:VULKAN_SDK) { $val = Join-Path $env:VULKAN_SDK 'Bin\spirv-val.exe' }
    if (-not $val -or -not (Test-Path $val)) {
        $cand = Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending |
                ForEach-Object { Join-Path $_.FullName 'Bin\spirv-val.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1
        if ($cand) { $val = $cand }
    }
    if (-not $val -or -not (Test-Path $val)) {
        $cmd = Get-Command spirv-val.exe -ErrorAction SilentlyContinue
        if ($cmd) { $val = $cmd.Source }
    }
    Need $val 'spirv-val.exe (the Vulkan SDK installs it; the package ships it so players need none)'
    New-Item -ItemType Directory -Force (Join-Path $Out 'tools\spirv-val') | Out-Null
    Copy-Item $val (Join-Path $Out 'tools\spirv-val\spirv-val.exe') -Force
    # Apache-2.0 section 4 wants a copy of the licence with the binary, not a pointer to it.
    $lic = Join-Path $root 'licenses\Apache-2.0.txt'
    Need $lic 'licenses\Apache-2.0.txt'
    $head = @'
spirv-val.exe - part of SPIRV-Tools, https://github.com/KhronosGroup/SPIRV-Tools
Copyright (c) The Khronos Group Inc. Licensed under the Apache License, Version 2.0.
This is an unmodified copy of the binary that ships with the Vulkan SDK (LunarG), redistributed so that the shader
harness can validate translated shaders on a machine that has no Vulkan SDK installed.
The full licence text follows.

--------------------------------------------------------------------------------

'@
    Set-Content -LiteralPath (Join-Path $Out 'tools\spirv-val\LICENSE.txt') -Value $head -Encoding utf8
    Add-Content -LiteralPath (Join-Path $Out 'tools\spirv-val\LICENSE.txt') -Value (Get-Content $lic -Raw) -Encoding utf8
    Say ("spirv-val     {0:N1} MB  (Apache-2.0, validates every translated shader)" -f ((Get-Item $val).Length / 1MB))
}
Need (Join-Path $Zig 'zig.exe') 'zig.exe'
CopyTree $Zig (Join-Path $Out 'tools\build\zig')
Say ("zig           {0:N1} MB" -f (Size (Join-Path $Out 'tools\build\zig')))

# --- 3b. what this folder is, so a player who unzips it has somewhere to start --------------------------------
Copy-Item (Join-Path $PSScriptRoot 'PACKAGE-README.txt') (Join-Path $Out 'README.txt') -Force
$notes = Join-Path $root 'docs\RELEASE-0.2.1.md'
if (Test-Path $notes) { Copy-Item $notes (Join-Path $Out 'RELEASE-NOTES.md') -Force }

# --- 3c. the support tools a player is asked to run: the report collector and the dev run scripts ---------------
New-Item -ItemType Directory -Force (Join-Path $Out 'tools\run') | Out-Null
Copy-Item (Join-Path $root 'tools\report.py') (Join-Path $Out 'tools\report.py') -Force
# Only what a player may be asked to run. The rest of tools/run is the maintainer's A/B and probe scripts.
foreach ($f in @('report.ps1', 'play-301.cmd', 'prefill.ps1', 'fill-cache.ps1')) {
    $s = Join-Path $root ('tools\run\' + $f)
    if (Test-Path $s) { Copy-Item $s (Join-Path $Out 'tools\run') -Force }
}
Say ("report        tools\report.py + tools\run (python, no PowerShell 7 needed)")

# --- 4. the engine, the shader corpus extractor, and what the lift needs --------------------------------------
New-Item -ItemType Directory -Force (Join-Path $Out 'engine') | Out-Null
Copy-Item (Join-Path $root 'tools\setup\xc2_setup.py') (Join-Path $Out 'engine') -Force
Copy-Item (Join-Path $root 'config\nxrt.ini.in') (Join-Path $Out 'engine') -Force   # the setup renders it for a fresh install
# The shader chain: the game's shaders are embedded in the romfs's bf2.ard, which these read and translate. Without
# them a package has an empty cache and the first launch renders with gaps.
New-Item -ItemType Directory -Force (Join-Path $Out 'tools\shader-cache') | Out-Null
Copy-Item (Join-Path $root 'tools\shader\cache\*.py') (Join-Path $Out 'tools\shader-cache') -Force
# The stencil-plane retype (format-07753): the prefill runs with ds_debug and this rewrites the samplers it names.
# Without it in the package a first run renders characters and the background untextured.
New-Item -ItemType Directory -Force (Join-Path $Out 'tools\shader') | Out-Null
Copy-Item (Join-Path $root 'tools\shader\patch_ds_samplers.py') (Join-Path $Out 'tools\shader') -Force
# The engine's --nup path runs this with the bundled Python (zstandard is in it), for a player whose dump is a .nup.
New-Item -ItemType Directory -Force (Join-Path $Out 'tools\analysis') | Out-Null
Copy-Item (Join-Path $root 'tools\analysis\nup_extract.py') (Join-Path $Out 'tools\analysis') -Force
Say 'engine        xc2_setup.py + the shader chain + nup_extract.py'
New-Item -ItemType Directory -Force (Join-Path $Out 'xc2\sites') | Out-Null
Copy-Item (Join-Path $root 'xc2\sites\*.txt') (Join-Path $Out 'xc2\sites') -Force
New-Item -ItemType Directory -Force (Join-Path $Out 'xc2\src') | Out-Null
Copy-Item (Join-Path $root 'xc2\src\xc2_stub.cpp'), (Join-Path $root 'xc2\src\xc2_modules.h') (Join-Path $Out 'xc2\src') -Force
CopyTree (Join-Path $root 'third_party\nxrecomp\recomp\include') (Join-Path $Out 'third_party\nxrecomp\recomp\include')
Say ("sources       {0:N1} MB  (hook tables, recompiler headers, the exe stub)" -f ((Size (Join-Path $Out 'xc2')) + (Size (Join-Path $Out 'third_party'))))

# --- 5. licences -------------------------------------------------------------------------------------------------
$lic = Join-Path $Out 'licenses'
New-Item -ItemType Directory -Force $lic | Out-Null
foreach ($f in @('LICENSE', 'THIRD_PARTY_NOTICES.md')) {
    $src = Join-Path $root $f
    if (Test-Path $src) { Copy-Item $src $lic -Force }
}
$notes = @"
XC2 NS2 recomp - portable package
=================================

Run xc2-launcher.exe. It asks for your own legally dumped copy of the game, recompiles it on this PC and plays it.

Nothing here is derived from the game. The exe is built on your PC from your own dump; the shader cache is
translated from your own shaders. The runtime DLL and the tools below are our own code or third-party code.

Bundled tools, each with its own licence:
  tools/python         Python 3.13.17 (PSF) with capstone (BSD-3), lz4 (BSD-2), zstandard (BSD-3), nxrecomp (GPL-3.0)
  tools/spirv_harness  Ryujinx's shader translator (MIT) in a self-contained .NET 10 runtime (MIT)
  tools/build/zig      Zig 0.17.0 (MIT); its bundled clang, lld and libc++ carry Apache-2.0 WITH LLVM-exception
  runtime/xc2_rt.dll   this project and the fefw-recomp runtime (GPL-3.0-or-later)
  runtime/av*.dll      FFmpeg shared GPL build (GPL-3.0-or-later): the film's hardware decoders; see licenses/FFMPEG-LICENSE.txt
"@
Set-Content -LiteralPath (Join-Path $lic 'BUNDLED_TOOLS.txt') -Value $notes -Encoding utf8NoBOM

Say ("TOTAL         {0:N1} MB" -f (Size $Out))

# --- 6. the tree just written, held to the same rules as a shipped one ---------------------------------------------------
# In a child process: verify-package.ps1 exits with a status, which would otherwise end this script mid-way.
& pwsh -NoProfile -File (Join-Path $PSScriptRoot 'verify-package.ps1') -Path $Out -AllowNoFFmpeg:$AllowNoFFmpeg
if ($LASTEXITCODE -ne 0) { throw "the package failed verify-package.ps1: $Out" }
Write-Host "package ready: $Out"

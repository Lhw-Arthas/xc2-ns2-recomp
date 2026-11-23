# SPDX-License-Identifier: GPL-3.0-or-later
# Build xc2_boot.exe: maps the four modules, registers them with nxrt, starts the NVN back end and enters rtld.
#
#   .\tools\build\build-boot.ps1                 # build
#   .\tools\build\build-boot.ps1 -Run            # build and run (window)
#   .\tools\build\build-boot.ps1 -Run -Gfx headless
#
# Prerequisites (built once; see docs/BUILD.md):
#   third_party\fefw-recomp\build\runtime-gfx\{nxrt.lib,nxrt_gfx.lib,...}   (graphics shell)
#   third_party\fefw-recomp\build\xc2-game-probe\games\xc2ns2\*.lib         (game module)
#   xc2\build\lib\{main,sdk,subsdk0,rtld}.lib                               (tools\build\build.ps1)
#   .\tools\build\build-boot.ps1 -MainLib build\lib-census\main.lib -OutDir build\boot-census   # the census build
param(
    [string]$Exefs = '',
    [string]$Link = 'host',
    [string]$Gfx = 'window',
    [switch]$Run,
    [string]$MainLib = '',      # the main.lib to link; default build\lib\main.lib
    [string]$OutDir = '',       # where xc2_boot.exe and its object go; default build\boot
    [string]$LibDir = '',       # directory holding the per-module libs build.ps1 writes; default build\lib
    [string]$Defines = ''       # extra /D defines for xc2_boot.cpp, space separated (HANDOVER 89: XC2_MODULES_7)
)
$ErrorActionPreference = 'Stop'
$root = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'xc2'   # tools\build -> the checkout's xc2
# The four NSOs now live in the project (xc2\game\xc2ns2), copied from <fefw-recomp>\work\xc2ns2.
if (-not $Exefs) { $Exefs = Join-Path $root 'game\xc2ns2' }
elseif (-not [System.IO.Path]::IsPathRooted($Exefs)) { $Exefs = Join-Path (Get-Location).Path $Exefs }
if (-not (Test-Path $Exefs)) { throw "ExeFS not found: $Exefs (copy main, sdk, subsdk0, rtld, main.npdm into xc2\game\xc2ns2)" }
# Optional: a script that puts clang-cl / llvm-lib / dotnet on PATH. A machine that already has them needs nothing.
if ($env:XC2_ENV) {
    if (-not (Test-Path $env:XC2_ENV)) { throw "XC2_ENV points at a file that does not exist: $env:XC2_ENV" }
    . $env:XC2_ENV | Out-Null
}

$repo = Split-Path -Parent $root
$rt = Join-Path $repo 'third_party\fefw-recomp'
# Not $gfx: PowerShell variable names are case-insensitive, so a variable called $gfx would overwrite the -Gfx parameter.
$gfxDir = Join-Path $rt 'build\runtime-gfx'
$game = Join-Path $rt 'build\xc2-game-probe\games\xc2ns2'
$nxinc = Join-Path $repo 'third_party\nxrecomp\recomp\include'
# Everything below switches directories for the link, so these have to be absolute from here on.
$bd = if ($OutDir) { $OutDir } else { Join-Path $root 'build\boot' }
if (-not [System.IO.Path]::IsPathRooted($bd)) { $bd = Join-Path (Get-Location).Path $bd }
$mainlib = if ($MainLib) { $MainLib } else { Join-Path $root 'build\lib\main.lib' }
if (-not [System.IO.Path]::IsPathRooted($mainlib)) { $mainlib = Join-Path (Get-Location).Path $mainlib }
$libdir = if ($LibDir) { $LibDir } else { Join-Path $root 'build\lib' }
if (-not [System.IO.Path]::IsPathRooted($libdir)) { $libdir = Join-Path (Get-Location).Path $libdir }
# The retail 3.0.1 ExeFS has three more modules (HANDOVER 89). They are linked only when their libs are in $LibDir, so the
# 4-module dev/playable build links exactly what it always did.
$extraLibs = @()
foreach ($m in 'subsdk1', 'subsdk2', 'subsdk3') { $p = Join-Path $libdir ($m + '.lib'); if (Test-Path $p) { $extraLibs += $p } }
# libvpx (BSD-3): the host VP9/VP8 decoders sit in nxrt.lib (runtime/src/media/video_codec.cpp), so linking the runtime
# needs the library even though nothing in this exe mentions vpx - without it the link stops at nine vpx_codec_* symbols.
# A runtime configured without libvpx (NXRT_HAVE_LIBVPX=0, see runtime/cmake/media_deps.cmake) references none of them,
# which is why a missing library is a warning and not an error. Same search order as tools/build/build-release.ps1.
$vpxRoots = @($env:VCPKG_ROOT, 'E:\Tools\dev\vcpkg', 'C:\vcpkg') | Where-Object { $_ }
$vpxLibs = @()
foreach ($vpxRoot in $vpxRoots) {
    $cand = Join-Path $vpxRoot 'installed\x64-windows-static\lib\vpx.lib'
    if (Test-Path $cand) { $vpxLibs += $cand; break }
}
if (-not $vpxLibs) {
    Write-Warning ("libvpx not found (looked in: {0}). If the runtime was built with it, this link fails on vpx_codec_*: vcpkg install libvpx:x64-windows-static, or set VCPKG_ROOT" -f ($vpxRoots -join ', '))
}
# libvorbis (BSD-3) decodes the films' sound (runtime/src/media/movie_adec.cpp); xc2/release links the same two libraries, and
# without them the runtime is configured with NXRT_HAVE_VORBIS=0 and the movie player has no sound. Same search order.
$vorbisLibs = @()
foreach ($vorbisRoot in $vpxRoots) {
    $vorbisDir = Join-Path $vorbisRoot 'installed\x64-windows-static\lib'
    $vorbisLib = Join-Path $vorbisDir 'vorbis.lib'
    $oggLib = Join-Path $vorbisDir 'ogg.lib'
    if ((Test-Path $vorbisLib) -and (Test-Path $oggLib)) { $vorbisLibs = @($vorbisLib, $oggLib); break }
}
if (-not $vorbisLibs) {
    Write-Warning ("libvorbis not found (looked in: {0}). If the runtime was built with it, this link fails on vorbis_*: vcpkg install libvorbis:x64-windows-static" -f ($vpxRoots -join ', '))
}
New-Item -ItemType Directory -Force -Path $bd | Out-Null

$need = @(
    "$gfxDir\nxrt.lib", "$gfxDir\nxrt_gfx.lib", "$gfxDir\nxrt_imgui.lib", "$gfxDir\nxrt_volk.lib",
    "$gfxDir\nxrt_tiling.lib", "$gfxDir\nxrt_astc2bc7.lib", "$gfxDir\nxrt_zstd.lib",
    "$gfxDir\_deps\sdl3-build\SDL3-static.lib", "$gfxDir\third_party\dav1d\nxrt_dav1d.lib",
    "$game\nxrt_game_xc2ns2_core.lib", "$game\nxrt_game_xc2ns2_data.lib",
    $mainlib, (Join-Path $libdir 'sdk.lib'),
    (Join-Path $libdir 'subsdk0.lib'), (Join-Path $libdir 'rtld.lib')
)
foreach ($f in $need) { if (-not (Test-Path $f)) { throw "missing $f (build it first: docs\BUILD.md, then tools\build\build.ps1)" } }

$obj = Join-Path $bd 'xc2_boot.obj'
$objOpus = Join-Path $bd 'x2_opus301.obj'
$objAudio = Join-Path $bd 'x2_audio301.obj'
# The headers of the pinned third-party libraries. CMake's FetchContent extracts them into the build directory
# (build\runtime-gfx\_deps\<name>-src); an older layout kept them under third_party\deps\src. Look in both, and say
# what was expected when neither is there: a clean clone that skipped the runtime configure used to fail here with a
# bare "cannot open include file".
function Find-Dep([string]$what, [string[]]$candidates) {
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    $msg = "missing the $what headers. Looked in:" + [Environment]::NewLine
    foreach ($c in $candidates) { $msg += "  " + $c + [Environment]::NewLine }
    throw ($msg + "Run the runtime configure first (docs/BUILD.md 4.1).")
}
$depsDir = Join-Path $gfxDir '_deps'
$volk = Find-Dep 'volk' @(
    (Join-Path $depsDir 'volk-src'),
    (Join-Path $repo 'third_party\deps\src\volk\volk-vulkan-sdk-1.4.363.0'))
$vkinc = Find-Dep 'Vulkan-Headers' @(
    (Join-Path $depsDir 'vulkan_headers-src\include'),
    (Join-Path $repo 'third_party\deps\src\vkheaders\Vulkan-Headers-vulkan-sdk-1.4.363.0\include'))
$vma = Find-Dep 'VulkanMemoryAllocator' @(
    (Join-Path $depsDir 'vma-src\include'),
    (Join-Path $repo 'third_party\deps\src\vma\VulkanMemoryAllocator-3.4.0\include'))
$sdlinc = Find-Dep 'SDL3' @(
    (Join-Path $depsDir 'sdl3-src\include'),
    (Join-Path $repo 'third_party\deps\src\sdl3\SDL3-3.4.18\include'))
$defArgs = @()
if ($Defines) { foreach ($d in $Defines.Split(' ')) { if ($d) { $defArgs += "/D$d" } } }
& clang-cl.exe /nologo /std:c++20 /O2 /MT /c (Join-Path $root 'src\xc2_boot.cpp') "/Fo$obj" @defArgs `
    "/I$nxinc" "/I$rt\runtime\include" "/I$rt\runtime\games" "/I$volk" "/I$vkinc" "/I$vma" "/I$sdlinc"
if ($LASTEXITCODE -ne 0) { throw 'compile failed: xc2_boot.cpp' }
& clang-cl.exe /nologo /std:c++20 /O2 /MT /c (Join-Path $root 'src\x2_opus301.cpp') "/Fo$objOpus" @defArgs `
    "/I$nxinc" "/I$rt\runtime\include" "/I$rt\runtime\games" "/I$volk" "/I$vkinc" "/I$vma" "/I$sdlinc"
if ($LASTEXITCODE -ne 0) { throw 'compile failed: x2_opus301.cpp' }
& clang-cl.exe /nologo /std:c++20 /O2 /MT /c (Join-Path $root 'src\x2_audio301.cpp') "/Fo$objAudio" @defArgs `
    "/I$nxinc" "/I$rt\runtime\include" "/I$rt\runtime\games" "/I$volk" "/I$vkinc" "/I$vma" "/I$sdlinc"
if ($LASTEXITCODE -ne 0) { throw 'compile failed: x2_audio301.cpp' }

# FFmpeg's hardware movie decoder (runtime/src/media/video_ffmpeg.cpp): nxrt.lib references avcodec/avutil when the
# runtime was configured with NXRT_FFMPEG_DIR, so the link needs the same two import libraries.
$ffLibs = @()
$ffCache = Join-Path $gfxDir 'CMakeCache.txt'
if (Test-Path -LiteralPath $ffCache) {
    $ffLine = Select-String -LiteralPath $ffCache -Pattern '^NXRT_FFMPEG_DIR:PATH=.?' | Select-Object -First 1
    if ($ffLine -and $ffLine.Line -match '=(.+)$') {
        $ffDir = $Matches[1].Trim()
        if ($ffDir -and (Test-Path -LiteralPath (Join-Path $ffDir 'lib\avcodec.lib'))) {
            $ffLibs = @((Join-Path $ffDir 'lib\avcodec.lib'), (Join-Path $ffDir 'lib\avutil.lib'))
            Write-Host ('ffmpeg    : ' + $ffDir)
        }
    }
}
# Optional local Windows application icon: if a user places xc2_icon.ico / mythra_ns2_301.ico / mythra_icon.ico
# in the boot output directory (or xc2\build\boot), compile a tiny .res with llvm-rc/rc and embed it as IDI_ICON1 (1)
# so the Windows 11 taskbar, Alt+Tab, Task Manager and Explorer show it natively. Never requires committing any art.
$iconRes = @()
$iconCandidates = @()
foreach ($dir in @($bd, (Join-Path $root 'build\boot'))) {
    foreach ($nm in 'xc2_icon.ico', 'mythra_ns2_301.ico', 'mythra_icon.ico') {
        $iconCandidates += (Join-Path $dir $nm)
    }
}
$foundIco = $iconCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if ($foundIco) {
    $rcTool = (Get-Command llvm-rc.exe, rc.exe -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($rcTool) {
        $rcFile = Join-Path $bd 'xc2_icon.rc'
        $resFile = Join-Path $bd 'xc2_icon.res'
        $icoEsc = $foundIco.Replace('\', '\\')
        Set-Content -LiteralPath $rcFile -Value "1 ICON `"$icoEsc`"" -Encoding Ascii
        if ($rcTool.Name -ieq 'llvm-rc.exe') {
            & $rcTool.Source /FO $resFile $rcFile
        } else {
            & $rcTool.Source /nologo /fo $resFile $rcFile
        }
        if ($LASTEXITCODE -eq 0 -and (Test-Path -LiteralPath $resFile)) {
            $iconRes = @($resFile)
            Write-Host ('icon      : ' + $foundIco)
        } else {
            Write-Warning ("icon resource compile failed for $foundIco (continuing without embedded icon)")
        }
    }
}

$exe = Join-Path $bd 'xc2_boot.exe'
# A linker map next to the exe: the runtime reads it (diag/symbols.cpp) and names the frames of every thread dump and crash
# backtrace instead of printing bare offsets. Without it a dump says "xc2_boot+0x8383d" and nothing else - and the PDB of this
# exe is 492 MB and only exists for the MSVC release build, never for this one (2026-11-22).
$mapFile = Join-Path $bd 'xc2_boot.map'
& clang-cl.exe /nologo /MT "/Fe:$exe" $obj $objOpus $objAudio @iconRes `
    "$gfxDir\nxrt_gfx.lib" "$gfxDir\nxrt.lib" "$game\nxrt_game_xc2ns2_core.lib" "$game\nxrt_game_xc2ns2_data.lib" `
    $mainlib (Join-Path $libdir 'sdk.lib') `
    (Join-Path $libdir 'subsdk0.lib') (Join-Path $libdir 'rtld.lib') @extraLibs @vpxLibs @vorbisLibs @ffLibs `
    "$gfxDir\nxrt_imgui.lib" "$gfxDir\nxrt_volk.lib" "$gfxDir\nxrt_tiling.lib" "$gfxDir\nxrt_astc2bc7.lib" "$gfxDir\nxrt_zstd.lib" `
    "$gfxDir\_deps\sdl3-build\SDL3-static.lib" "$gfxDir\third_party\dav1d\nxrt_dav1d.lib" `
    kernel32.lib user32.lib gdi32.lib winmm.lib imm32.lib ole32.lib oleaut32.lib version.lib uuid.lib advapi32.lib `
    setupapi.lib shell32.lib dinput8.lib psapi.lib /link "/MAP:$mapFile"
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
Write-Host ("boot exe: {0} ({1:N1} MB)" -f $exe, ((Get-Item $exe).Length / 1MB))

# The guest names for the map's f_<VA> entries, next to the exe so symbols_load_map finds them (runtime/src/diag/
# symbols.cpp). build.ps1 writes it from the lift; a boot build from a lib dir that predates the sidecar is fine -
# the dump then keeps the f_ names.
$namesSrc = Join-Path $libdir 'guest-symbols.names'
$namesDst = Join-Path $bd 'xc2_boot.names'
if (Test-Path -LiteralPath $namesSrc) {
    Copy-Item -LiteralPath $namesSrc -Destination $namesDst -Force
    Write-Host ("guest names: {0}" -f $namesDst)
} else {
    Write-Warning ("no guest names at {0}: lifted frames stay f_<VA> in dumps (run toolsuilduild.ps1 to write it)" -f $namesSrc)
}

# build.json: what this exe is. The folder name is a human convention; this is the thing a log or a check can read.
$head = (git -C $repo rev-parse HEAD 2>$null)
$dirty = @(git -C $repo status --porcelain 2>$null | Where-Object { $_ }).Count
$manifest = [ordered]@{
    profile   = 'dev-boot'
    commit    = $head
    dirty     = $dirty
    built_at  = (Get-Date).ToString('o')
    exe       = $exe
    exe_bytes = (Get-Item $exe).Length
    sha256    = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    defines   = $Defines
    main_lib  = $mainlib
    lib_dir   = $libdir
}
$manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $bd 'build.json') -Encoding utf8
Write-Host ('build.json: {0} {1}' -f $head.Substring(0, 7), $(if ($dirty) { 'dirty' } else { 'clean' }))

if ($Run) {
    Push-Location $bd
    try { & $exe --exefs $Exefs --link $Link --gfx $Gfx --log (Join-Path $bd 'xc2_boot.log') } finally { Pop-Location }
}


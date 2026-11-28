# SPDX-License-Identifier: GPL-3.0-or-later
# Build the release split: xc2_rt.dll (runtime + boot code, a C ABI, no game code) and xc2_boot.exe (a stub plus
# the libraries lifted from the player's own dump, compiled by the bundled Zig).
#
#   pwsh tools\build\build-release.ps1                       # everything, into xc2\build\release
#   pwsh tools\build\build-release.ps1 -SkipDll              # only compile the lift and link the exe
#   pwsh tools\build\build-release.ps1 -Zig D:\zig\zig.exe -Jobs 6
#
# The lift has to exist first (tools\build\build.ps1). Nothing from the game is in the DLL; nothing from the
# runtime is in the exe. Upstream ships the same arrangement as fefw_rt.dll + fefw.exe.
param(
    [string]$Zig = '',
    [int]$Jobs = 0,
    [string]$OutDir = '',
    [string]$DllBuildDir = '',
    [string]$LiftDir = '',
    [switch]$SkipDll,
    [switch]$SkipExe,
    [switch]$SkipHarness,
    [switch]$SkipPython,
    [ValidateSet('auto', 'fma', 'compat')][string]$Cpu = 'auto'
)
$ErrorActionPreference = 'Stop'
$root = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'xc2'   # tools\build -> the checkout's xc2
$repo = Split-Path -Parent $root                    # the checkout
$rt = Join-Path $repo 'third_party\fefw-recomp'

if ($env:XC2_ENV) { if (-not (Test-Path $env:XC2_ENV)) { throw "XC2_ENV points at a file that does not exist: $env:XC2_ENV" }; . $env:XC2_ENV | Out-Null }
if (-not $Jobs -or $Jobs -lt 1) { $Jobs = (Get-CimInstance Win32_ComputerSystem).NumberOfLogicalProcessors - 1; if ($Jobs -lt 2) { $Jobs = 2 } }
if (-not $OutDir) { $OutDir = Join-Path $root 'build\release' }
if (-not $DllBuildDir) { $DllBuildDir = Join-Path $root 'build\release\dll' }
if (-not $LiftDir) {
    # The release layout keeps its own lift (build\lift-301). A checkout that only ever ran the development route has
    # build\301\lift instead, which is the same seven modules in the same shape, so the exe step works from either one -
    # measured 2026-11-19, a release build stopped at "no lifted sources for main" until this fallback existed.
    $releaseLift = Join-Path $root 'build\lift-301'
    $devLift = Join-Path $root 'build\301\lift'
    if (Test-Path (Join-Path $releaseLift 'main')) { $LiftDir = $releaseLift }
    elseif (Test-Path (Join-Path $devLift 'main')) {
        $LiftDir = $devLift
        Write-Host ("lift: using the development lift at {0} (no release-layout lift here)" -f $devLift)
    } else { $LiftDir = $releaseLift }
}

# The bundled Zig is what makes this build need no MSVC and no Windows SDK: it carries its own headers and CRT.
if (-not $Zig) {
    $zigBundled = Join-Path $repo 'third_party\fefw-recomp\game-build\release\zig\zig.exe'
    $onPath = Get-Command zig.exe -ErrorAction SilentlyContinue
    foreach ($cand in @($env:XC2_ZIG, $zigBundled, $(if ($onPath) { $onPath.Source } else { '' }))) {
        if ($cand -and (Test-Path $cand)) { $Zig = $cand; break }
    }
}
if (-not $Zig -or -not (Test-Path $Zig)) { throw 'no zig.exe: pass -Zig, set $env:XC2_ZIG, or put it on PATH' }
Write-Host ("zig: {0} ({1})" -f $Zig, (& $Zig version))

$modules = @('main', 'sdk', 'subsdk0', 'subsdk1', 'subsdk2', 'subsdk3', 'rtld')
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# ---- 1. xc2_rt.dll --------------------------------------------------------------------------------------
$dll = Join-Path $DllBuildDir 'xc2_rt.dll'
if (-not $SkipDll) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    $cfg = @('-S', (Join-Path $root 'release'), '-B', $DllBuildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
             '-DCMAKE_C_COMPILER=clang-cl', '-DCMAKE_CXX_COMPILER=clang-cl')
    # Reuse the runtime's fetched third-party sources instead of downloading them again.
    $deps = Join-Path $rt 'build\runtime-gfx\_deps'
    if (Test-Path $deps) { $cfg += "-DFETCHCONTENT_BASE_DIR=$deps" }
    # libvpx (BSD-3): the host VP9/VP8 decoders. Static from a vcpkg install, which CMake finds by itself only on a
    # fresh configure - a cached build keeps its first NXRT_LIBVPX_LIB-NOTFOUND answer, and the runtime then says
    # "built without libvpx" and leaves VP9 movies out (measured 2026-11-18).
    foreach ($vcpkg in @($env:VCPKG_ROOT, 'C:\vcpkg')) {
        if (-not $vcpkg) { continue }
        $vpx = Join-Path $vcpkg 'installed\x64-windows-static\lib\vpx.lib'
        if (Test-Path $vpx) {
            $cfg += "-DNXRT_LIBVPX_ROOT=" + (Split-Path -Parent (Split-Path -Parent $vpx)).Replace('\', '/')
            break
        }
    }
    # FFmpeg (GPL-3.0-or-later shared build): the film's hardware decoders (libavcodec + D3D11VA). Without this the runtime
    # says "built without FFmpeg" and every 4K opening runs on the CPU - measured 2026-11-21 on the packaged DLL, which had
    # [media] hwdec on and no FFmpeg at all, so the option did nothing. The DLLs travel in the package next to xc2_boot.exe.
    $ffmpeg = ''
    $wsFFmpeg = Join-Path (Split-Path -Parent $repo) 'lan\ffmpeg\ffmpeg-master-latest-win64-gpl-shared'
    foreach ($ff in @($env:NXRT_FFMPEG_DIR, $wsFFmpeg)) {
        if ($ff -and (Test-Path (Join-Path $ff 'lib\avcodec.lib'))) { $ffmpeg = $ff; break }
    }
    if ($ffmpeg) { $cfg += '-DNXRT_FFMPEG_DIR=' + $ffmpeg.Replace('\', '/') }
    else { Write-Warning 'FFmpeg not found: this build has no hardware film decoder' }
    & cmake @cfg 2>&1 | ForEach-Object { if ($_ -match 'error|Error') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw 'the release project could not be configured' }
    & cmake --build $DllBuildDir --target xc2_rt 2>&1 | ForEach-Object { if ($_ -match 'error|Linking|undefined') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw 'xc2_rt.dll did not build' }
    Write-Host ("xc2_rt.dll: {0:N1} MB in {1:N0} s" -f ((Get-Item $dll).Length / 1MB), $t.Elapsed.TotalSeconds)
} elseif (-not (Test-Path $dll)) { throw "no $dll and -SkipDll was given" }

# ---- 0. the shader translator, published self-contained -----------------------------------------------
# Framework-dependent would need the .NET runtime on the player's machine; this carries its own. Nothing in it is
# derived from the game: it is Ryujinx's translator (MIT) plus our own harness code, and it reads the shader
# bytecode from the dump at run time.
if (-not $SkipHarness) {
    $proj = Join-Path $rt 'tools\shaders\harness\SpirvHarness.csproj'
    $pub = Join-Path $rt 'tools\shaders\build\publish\win-x64'
    # Prefer the SDK the environment script points at: a bare 'dotnet' on PATH is often a runtime-only install and
    # then 'dotnet publish' answers "No .NET SDKs were found".
    $dn = $null
    foreach ($cand in @((Join-Path $env:DOTNET_ROOT 'dotnet.exe'), 'dotnet')) {
        if (-not $cand) { continue }
        $exe = if ($cand -eq 'dotnet') { (Get-Command dotnet -ErrorAction SilentlyContinue).Source } else { $cand }
        if ($exe -and (Test-Path $exe) -and (& $exe --list-sdks 2>$null)) { $dn = $exe; break }
    }
    if (-not $dn) { throw 'no .NET SDK found: set $env:DOTNET_ROOT or put the SDK on PATH' }
    $t = [Diagnostics.Stopwatch]::StartNew()
    & $dn publish $proj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=false -o $pub 2>&1 |
        ForEach-Object { if ($_ -match 'error|-> ') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw 'the shader harness did not publish' }
    $h = Join-Path $pub 'spirv_harness.exe'
    $mb = ((Get-ChildItem $pub -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
    Write-Host ("spirv_harness: {0:N0} MB self-contained in {1:N0} s -> {2}" -f $mb, $t.Elapsed.TotalSeconds, $pub)
}

# ---- 0b. an embeddable Python, so the lift does not need one installed ------------------------------------
# Same shape as upstream's package: the interpreter, the wheels its tools import, and the lifter itself inside
# site-packages. PYTHONPATH cannot be used for that last part - a python*._pth file makes the interpreter ignore
# it - so nxrecomp and its games\ folder are copied in and found by name.
if (-not $SkipPython) {
    $pyver = '3.13.17'
    $pydir = Join-Path $OutDir 'tools\python'
    $zip = Join-Path $env:TEMP "python-$pyver-embed-amd64.zip"
    if (-not (Test-Path $zip)) {
        Write-Host "python: downloading python-$pyver-embed-amd64.zip"
        Invoke-WebRequest -Uri "https://www.python.org/ftp/python/$pyver/python-$pyver-embed-amd64.zip" -OutFile $zip -UseBasicParsing
    }
    if (Test-Path $pydir) { Remove-Item $pydir -Recurse -Force }
    New-Item -ItemType Directory -Force $pydir | Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $pydir)
    $pth = Get-ChildItem $pydir -Filter 'python*._pth' | Select-Object -First 1
    if (-not ((Get-Content $pth.FullName -Raw) -match 'Lib\\site-packages')) { Add-Content $pth.FullName 'Lib\site-packages' }
    $site = Join-Path $pydir 'Lib\site-packages'
    New-Item -ItemType Directory -Force $site | Out-Null
    # The wheels are for the embedded interpreter's version and platform, not for the one running pip here.
    $whl = Join-Path $env:TEMP "xc2-wheels-$pyver"
    New-Item -ItemType Directory -Force $whl | Out-Null
    $pipPy = if ($env:XC2_PYTHON) { $env:XC2_PYTHON } else { (Get-Command python -ErrorAction SilentlyContinue).Source }
    if (-not $pipPy) { throw 'no python to run pip with: set $env:XC2_PYTHON' }
    & $pipPy -m pip download --only-binary=:all: --python-version 3.13 --platform win_amd64 --implementation cp -d $whl capstone lz4 zstandard 2>&1 |
        ForEach-Object { if ($_ -match 'Saved|ERROR') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw 'pip download failed' }
    foreach ($w in Get-ChildItem $whl -Filter *.whl) { [IO.Compression.ZipFile]::ExtractToDirectory($w.FullName, $site, $true) }
    # The lifter, inside site-packages: -m nxrecomp then works with no PYTHONPATH at all.
    $nxsrc = Join-Path $repo 'third_party\nxrecomp\recomp'
    foreach ($part in @('nxrecomp', 'games')) {
        $from = Join-Path $nxsrc $part
        if (-not (Test-Path $from)) { throw "no $from (the recompiler sources are missing)" }
        Copy-Item $from $site -Recurse -Force
    }
    $mb = ((Get-ChildItem $pydir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
    Write-Host ("python: {0:N0} MB embeddable + capstone/lz4/zstandard + nxrecomp -> {1}" -f $mb, $pydir)
}

if ($SkipExe) { return }

# ---- 2. compile the lift with Zig ------------------------------------------------------------------------
$inc = Join-Path $repo 'third_party\nxrecomp\recomp\include'
$objRoot = Join-Path $root 'build\release\obj-zig'

# The processor variant. '+fma' needs FMA3 (Haswell / Piledriver or newer); an older CPU executing it dies with an
# illegal instruction, so 'auto' reads what this processor actually has from zig itself and picks the slower set
# when FMA3 is missing. Same two variants as upstream's fefw_build.py.
$variants = @{
    fma    = @{ mcpu = 'x86_64+cx16+fma';          needs = @('cx16', 'sse4_2', 'avx', 'fma') }
    compat = @{ mcpu = 'x86_64+cx16+sse4_2+popcnt'; needs = @('cx16', 'sse4_2', 'popcnt') }
}
function Get-HostCpuFeatures {
    try {
        $out = (& $Zig targets 2>$null) -join "`n"
    } catch { return $null }
    $i = $out.IndexOf('.native')
    $j = if ($i -ge 0) { $out.IndexOf('.features', $i) } else { -1 }
    if ($j -lt 0) { return $null }
    $end = $out.IndexOf('}', $j)
    $seg = $out.Substring($j, $end - $j)
    $feats = @([regex]::Matches($seg, '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
    if (-not $feats.Count) { return $null }
    return $feats
}
if ($Cpu -eq 'auto') {
    $feats = Get-HostCpuFeatures
    if (-not $feats) { $Cpu = 'fma'; Write-Host 'cpu: cannot read the processor features, assuming fma' }
    elseif (@($variants.fma.needs | Where-Object { $feats -notcontains $_ }).Count -eq 0) { $Cpu = 'fma' }
    else { $Cpu = 'compat' }
}
$missing = @($variants[$Cpu].needs | Where-Object { $feats -and $feats -notcontains $_ })
Write-Host ("cpu: {0} (-mcpu={1}){2}" -f $Cpu, $variants[$Cpu].mcpu,
    $(if ($missing.Count) { ': this processor lacks ' + ($missing -join ', ') } else { '' }))

$flags = @('-target', 'x86_64-windows-gnu', "-mcpu=$($variants[$Cpu].mcpu)", '-std=c++20', '-O1', '-w',
           '-fno-exceptions', '-fno-rtti', '-fno-strict-aliasing', '-ffp-contract=off', '-fno-math-errno',
           '-fno-threadsafe-statics', '-DNX_INLINE_INDIRECT')
$todo = @()
foreach ($m in $modules) {
    $dir = Join-Path $LiftDir $m
    if (-not (Test-Path $dir)) { throw "no lifted sources for $m in $dir (run tools\build\build.ps1 first)" }
    $od = Join-Path $objRoot $m
    New-Item -ItemType Directory -Force -Path $od | Out-Null
    foreach ($s in Get-ChildItem $dir -Filter '*.cpp') {
        $o = Join-Path $od ($s.BaseName + '.o')
        if ((Test-Path $o) -and ((Get-Item $o).LastWriteTime -gt $s.LastWriteTime)) { continue }
        $todo += [pscustomobject]@{ Src = $s.FullName; Obj = $o; Dir = $dir }
    }
}
Write-Host ("compile: {0} translation unit(s) with zig, {1} at a time" -f $todo.Count, $Jobs)
if ($todo.Count) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    $res = $todo | ForEach-Object -ThrottleLimit $Jobs -Parallel {
        $o = & $using:Zig c++ @using:flags "-I$($using:inc)" "-I$($_.Dir)" -c $_.Src "-o$($_.Obj)" 2>&1 | Out-String
        [pscustomobject]@{ Src = $_.Src; Exit = $LASTEXITCODE; Log = $o }
    }
    $bad = @($res | Where-Object { $_.Exit -ne 0 })
    Write-Host ("compile: {0} ok, {1} failed, {2:N1}s" -f ($res.Count - $bad.Count), $bad.Count, $t.Elapsed.TotalSeconds)
    foreach ($b in $bad) {
        Write-Host ("  {0}: exit {1}" -f (Split-Path $b.Src -Leaf), $b.Exit)
        $lines = @($b.Log -split "`n" | Where-Object { $_ -match 'error:|fatal' } | Select-Object -First 5)
        if (-not $lines.Count) { $lines = @($b.Log -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 5) }
        foreach ($l in $lines) { Write-Host ('    ' + $l.TrimEnd()) }
    }
    if ($bad.Count) { throw 'the lift did not compile' }
}

# ---- 3. link the exe: the stub + every lifted object + the DLL's import library ---------------------------
$objs = Get-ChildItem $objRoot -Recurse -Filter '*.o' | ForEach-Object { $_.FullName }
$exe = Join-Path $OutDir 'xc2_boot.exe'
# 537 object paths are about 40 kB, past the 32,767-character limit on a Windows command line, so they go in a
# response file (zig expands @file) and everything else stays on the command line.
$rsp = Join-Path $objRoot 'objects.rsp'
Set-Content -LiteralPath $rsp -Value ($objs | ForEach-Object { '"' + $_ + '"' }) -Encoding ascii
$t = [Diagnostics.Stopwatch]::StartNew()
$o = & $Zig c++ -target x86_64-windows-gnu -O1 -w -fno-exceptions -fno-rtti -DXC2_MODULES_7 "@$rsp" `
        (Join-Path $root 'src\xc2_stub.cpp') "-I$inc" "-I$(Join-Path $root 'src')" `
        (Join-Path $DllBuildDir 'xc2_rt.lib') -o $exe 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    ($o -split "`n") | Where-Object { $_ -match 'error|undefined' } | Select-Object -First 10 | ForEach-Object { Write-Host ('  ' + $_.Trim()) }
    throw 'the exe did not link'
}
# Clear HIGH_ENTROPY_VA (0x20) from DllCharacteristics: Zig/LLD leaves high-entropy ASLR enabled by default, which can
# place bottom-up Windows host allocations inside the guest's [0x2000000000, 0x4000000000) alias window (issue #25).
$peBytes = [System.IO.File]::ReadAllBytes($exe)
$peOff = [BitConverter]::ToInt32($peBytes, 0x3C)
if ($peBytes[$peOff + 24] -eq 0x0B -and $peBytes[$peOff + 25] -eq 0x02) {
    $dcOff = $peOff + 24 + 70
    $dc = [BitConverter]::ToUInt16($peBytes, $dcOff)
    if ($dc -band 0x0020) {
        $dc = $dc -band (-bnot 0x0020)
        $dcBytes = [BitConverter]::GetBytes([uint16]$dc)
        $peBytes[$dcOff] = $dcBytes[0]
        $peBytes[$dcOff + 1] = $dcBytes[1]
        [System.IO.File]::WriteAllBytes($exe, $peBytes)
    }
}
Copy-Item -LiteralPath $dll -Destination $OutDir -Force
Write-Host ("exe: {0:N1} MB + xc2_rt.dll, linked in {1:N0} s -> {2}" -f ((Get-Item $exe).Length / 1MB), $t.Elapsed.TotalSeconds, $OutDir)

# build.json: the release exe is linked on the player's PC, so the manifest travels with it.
$head = (git -C $repo rev-parse HEAD 2>$null)
$dirty = @(git -C $repo status --porcelain 2>$null | Where-Object { $_ }).Count
[ordered]@{
    profile   = 'release'
    commit    = $head
    dirty     = $dirty
    built_at  = (Get-Date).ToString('o')
    exe       = $exe
    exe_bytes = (Get-Item $exe).Length
    sha256    = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    dll       = (Join-Path $OutDir 'xc2_rt.dll')
    dll_bytes = (Get-Item (Join-Path $OutDir 'xc2_rt.dll')).Length
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutDir 'build.json') -Encoding utf8

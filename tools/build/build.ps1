# SPDX-License-Identifier: GPL-3.0-or-later
# xc2ns2 scaffold build: lift the four modules with nxrecomp, compile every shard with clang-cl, link xc2ns2_stub.exe.
#
#   . .\tools\build\build.ps1                 # everything (lift if stale, compile, link, run the smoke test)
#   . .\tools\build\build.ps1 -Run:$false     # build only
#   . .\tools\build\build.ps1 -SkipLift       # reuse build\lift (the C++ is current)
#   . .\tools\build\build.ps1 -Clean          # drop build\obj and build\bin first
#
# Inputs: the ExeFS modules (default <repo>\game\xc2ns2, override with -Exefs or $env:XC2_EXEFS).
# Hook sites: build\sites-<module>.txt if present, else the shipped xc2\sites\sites-<module>.txt.
# Toolchain: clang-cl and llvm-lib on PATH (Visual Studio Build Tools + LLVM), or point $env:XC2_ENV at a script
# that sets them up. Python: $env:XC2_PYTHON, else `python` on PATH.
param(
    [string]$Exefs = $(if ($env:XC2_EXEFS) { $env:XC2_EXEFS } else { Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'xc2\game\xc2ns2' }),
    [string[]]$Modules = @('main', 'sdk', 'subsdk0', 'rtld'),   # accepts -Modules main,sdk and -Modules 'main,sdk'
    [switch]$SkipLift,
    [switch]$Clean,
    [switch]$Run = $true,
    [int]$Jobs = 10,
    # Where the lift, the objects and the per-module libraries go. The defaults are the four-module development
    # route; the retail 3.0.1 route keeps its own (build\lift-301, build\lib-301) so the two never overwrite
    # each other. Relative paths resolve against the directory you run the script from.
    [string]$LiftDir = '',
    [string]$ObjDir = '',
    [string]$LibDir = '',
    [string]$ModsDir = '',
    [string]$IniPath = ''
)
$ErrorActionPreference = 'Stop'
$root = Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'xc2'   # tools\build -> the checkout's xc2
$nx = (Resolve-Path (Join-Path $root '..\third_party\nxrecomp\recomp')).Path
$build = Join-Path $root 'build'
function Resolve-OutDir([string]$value, [string]$fallback) {
    $p = if ($value) { $value } else { $fallback }
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path (Get-Location).Path $p }
    return $p
}
$liftDir = Resolve-OutDir $LiftDir (Join-Path $build 'lift')
$objDir  = Resolve-OutDir $ObjDir  (Join-Path $build 'obj')
$libDir  = Resolve-OutDir $LibDir  (Join-Path $build 'lib')

# Optional: a script that puts clang-cl/llvm-lib/python/dotnet on PATH. It is loaded first, because it may be what
# puts python there; a machine that already has the tools needs nothing here.
if ($env:XC2_ENV) {
    if (-not (Test-Path $env:XC2_ENV)) { throw "XC2_ENV points at a file that does not exist: $env:XC2_ENV" }
    . $env:XC2_ENV | Out-Null
}
# The interpreter the lifter runs under. The packaged one comes first: a release carries tools\python with the
# wheels and nxrecomp inside it, so nothing has to be installed. Each candidate is asked for its version rather than
# trusted - the Windows Store alias answers "which python" and then does nothing.
function Test-Python([string]$p) {
    if (-not $p -or $p -match '\\WindowsApps\\' -or -not (Test-Path $p)) { return $false }
    try { & $p --version *> $null; return ($LASTEXITCODE -eq 0) } catch { return $false }
}
$py = $null
foreach ($cand in @($env:XC2_PYTHON,
                    (Join-Path $root 'build\release\tools\python\python.exe'),
                    (Join-Path (Split-Path -Parent $root) 'tools\python\python.exe'))) {
    if (Test-Python $cand) { $py = $cand; break }
}
if (-not $py) {
    foreach ($name in 'python', 'python3', 'py') {
        $c = Get-Command $name -ErrorAction SilentlyContinue
        if ($c -and (Test-Python $c.Source)) { $py = $c.Source; break }
    }
}
if (-not $py) { throw 'no working python: set $env:XC2_PYTHON, or build the package python with tools\build\build-release.ps1' }
Write-Host ("python: " + $py)

# The lift runs with the nxrecomp directory as the working directory, so a relative -Exefs has to be resolved here,
# against the directory the caller used.
if (-not [System.IO.Path]::IsPathRooted($Exefs)) { $Exefs = Join-Path (Get-Location).Path $Exefs }

if ($ModsDir -and (Test-Path $ModsDir)) {
    $patchedDir = Join-Path $build 'exefs-patched'
    $patchScript = Join-Path $root '..\tools\mods\exefs_patch.py'
    $pArgs = @($patchScript, '--exefs', $Exefs, '--mods', $ModsDir, '--out', $patchedDir)
    if ($IniPath -and (Test-Path $IniPath)) { $pArgs += @('--ini', $IniPath) }
    $outJson = & $py @pArgs
    if ($LASTEXITCODE -ne 0) { throw 'M3 ExeFS code patching failed' }
    if ((Test-Path (Join-Path $patchedDir 'm3_state.json')) -and (Test-Path (Join-Path $patchedDir 'main'))) {
        $Exefs = $patchedDir
        Write-Host "m3: using patched ExeFS copy at $Exefs"
    }
}

if ($Clean) { Remove-Item -Recurse -Force (Join-Path $build 'obj'), (Join-Path $build 'bin') -ErrorAction SilentlyContinue }

$mods = $Modules | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ }
# Guest load addresses. main is the console's fixed 0x7100000000; the other modules are placed by the runtime in the
# SDK region (nxrt/memory.h: kSdkBase..kHeapBase) and the FEFW fork puts them at these addresses (recomp/games/fefw).
$bases = @{ 'main' = '0x7100000000'; 'sdk' = '0x7500000000'; 'subsdk0' = '0x7600000000'; 'subsdk1' = '0x7640000000'; 'subsdk2' = '0x7680000000'; 'subsdk3' = '0x76C0000000'; 'rtld' = '0x7700000000' }
$env:PYTHONPATH = $nx
$sw = [Diagnostics.Stopwatch]::StartNew()

# ---- 1. lift ------------------------------------------------------------------------------------------
if (-not $SkipLift) {
    Push-Location $nx
    try {
        foreach ($m in $mods) {
            $src = Join-Path $Exefs $m
            if (-not (Test-Path $src)) { throw "missing module: $src" }
            if (-not $bases.ContainsKey($m)) { throw "no guest base for module $m (add it to `$bases)" }
            $base = $bases[$m]
            $out = Join-Path $liftDir $m
            $stampFile = Join-Path $out '.base'
            # Site list (entry/mid hooks compiled into the lifted code). build\sites-<module>.txt wins if it exists, so a
            # local experiment can add hooks without touching the repository; otherwise the shipped xc2\sites\ copy is
            # used. Without --sites the lifter emits no nx_hook_entry, and every hook_add_entry in xc2_boot.cpp silently
            # does nothing.
            #
            # An override OLDER than the shipped file is a leftover, not an experiment. It used to win anyway, so the
            # patches it carries were either missing without a word or aimed at bytes that had since moved. Measured
            # 2026-11-17: a sites-main.txt from two days earlier stopped a from-zero build in 0.8 s ("the code has
            # a94257f6, the patch expects 7100731f"), and an sdk and an rtld override were quietly lifting with half
            # their patches. Say so and use the shipped copy; touch the override to mean it.
            $sitesArg = @()
            $override = Join-Path $build "sites-$m.txt"
            $shipped = Join-Path $root "sites\sites-$m.txt"
            $useSites = $null
            if (Test-Path $override) {
                if ((Test-Path $shipped) -and ((Get-Item -LiteralPath $override).LastWriteTime -lt (Get-Item -LiteralPath $shipped).LastWriteTime)) {
                    Write-Warning ("sites-$m.txt: build\sites-$m.txt is older than xc2\sites\sites-$m.txt ({0} < {1}) - using the shipped copy. Touch build\sites-$m.txt to override anyway." -f (Get-Item -LiteralPath $override).LastWriteTime.ToString('yyyy-MM-dd HH:mm'), (Get-Item -LiteralPath $shipped).LastWriteTime.ToString('yyyy-MM-dd HH:mm'))
                } else { $useSites = $override }
            }
            if (-not $useSites -and (Test-Path $shipped)) { $useSites = $shipped }
            if ($useSites) { $sitesArg = @('--sites', $useSites) }
            $firstMod = Get-ChildItem $out -Filter 'module_*.cpp' -ErrorAction SilentlyContinue | Select-Object -First 1
            $fresh = (Test-Path $out) -and $firstMod -and
                     ($firstMod.LastWriteTime -gt (Get-Item $src).LastWriteTime) -and
                     (-not $useSites -or ($firstMod.LastWriteTime -gt (Get-Item -LiteralPath $useSites).LastWriteTime)) -and
                     ((Test-Path $stampFile) -and ((Get-Content $stampFile -Raw).Trim() -eq $base))
            if ($fresh) { Write-Host "lift $m : up to date (base $base)"; continue }
            $t = [Diagnostics.Stopwatch]::StartNew()
            # No frame-counter instrumentation, deliberately. The retail 3.0.1 main paces itself by real time, not
            # by a fixed 1/30 s a frame: measured 2026-11-17, at 59.1 fps with the counters effectively off (45 of the
            # 46 were dropped, the list having been scanned from the 4-module dev dump) the game ran at the right
            # speed on screen. The scaling this build used to apply had nothing to correct, and it was the only way
            # the game could die inside fps_fs (0xC0000005 reading address 0x3, eight seconds in), so the path is
            # gone rather than repaired. xc2_boot.cpp no longer carries the helpers or the census either.
            & $py -m nxrecomp lift $src $out --game xc2ns2 --name $m --base $base --jobs $Jobs @sitesArg 2>&1 |
                ForEach-Object { Write-Host "  $_" }
            if ($LASTEXITCODE -ne 0) { throw "lift failed for $m" }
            Set-Content -LiteralPath $stampFile -Value $base -Encoding ascii
            Write-Host ("lift {0} : {1:N1}s (base {2})" -f $m, $t.Elapsed.TotalSeconds, $base)
        }
    } finally { Pop-Location }
}

# ---- 2. compile ----------------------------------------------------------------------------------------
$inc = Join-Path $nx 'include'
$ccJobs = @()
foreach ($m in $mods) {
    $dir = Join-Path $liftDir $m
    $obj = Join-Path $objDir $m
    New-Item -ItemType Directory -Force -Path $obj | Out-Null
    $sources = Get-ChildItem $dir -Filter '*.cpp'
    foreach ($s in $sources) {
        $o = Join-Path $obj ($s.BaseName + '.obj')
        if ((Test-Path $o) -and ((Get-Item $o).LastWriteTime -gt $s.LastWriteTime)) { continue }
        $ccJobs += [pscustomobject]@{ Src = $s.FullName; Obj = $o; Mod = $m; Inc = $inc; Dir = $dir }
    }
}
Write-Host ("compile: {0} translation unit(s) to build" -f $ccJobs.Count)
if ($ccJobs.Count) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    $res = $ccJobs | ForEach-Object -ThrottleLimit $Jobs -Parallel {
        $o = & clang-cl.exe /nologo /std:c++20 /O2 /c "/I$($_.Inc)" "/I$($_.Dir)" $_.Src "/Fo$($_.Obj)" 2>&1 | Out-String
        [pscustomobject]@{ Src = $_.Src; Exit = $LASTEXITCODE; Errors = ([regex]::Matches($o, 'error:')).Count; Log = $o }
    }
    $bad = @($res | Where-Object { $_.Exit -ne 0 })
    Write-Host ("compile: {0} ok, {1} failed, {2:N1}s" -f ($res.Count - $bad.Count), $bad.Count, $t.Elapsed.TotalSeconds)
    # A compiler that is killed (memory, a cancelled job) exits non-zero without writing an "error:" line, and this
    # used to print nothing at all: measured 2026-11-17, ten failed translation units and a blank space in the log,
    # with each one compiling fine by hand afterwards. Always show the exit code and whatever it did say.
    foreach ($b in $bad) {
        $lines = @($b.Log -split "`n" | Where-Object { $_ -match 'error:|fatal|Internal error|Killed|out of memory' } | Select-Object -First 5)
        if (-not $lines.Count) { $lines = @($b.Log -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 5) }
        Write-Host ("  {0}: exit {1}" -f (Split-Path $b.Src -Leaf), $b.Exit)
        if (-not $lines.Count) { Write-Host '    (no output at all: the compiler was most likely killed)' }
        else { foreach ($l in $lines) { Write-Host ('    ' + $l.TrimEnd()) } }
    }
    if ($bad.Count) { throw 'compile failed' }
}

# ---- 3. one static library per module ------------------------------------------------------------------
$libs = @()
foreach ($m in $mods) {
    $obj = Join-Path $objDir $m
    $lib = Join-Path $libDir "$m.lib"
    New-Item -ItemType Directory -Force -Path (Split-Path $lib) | Out-Null
    $objs = Get-ChildItem $obj -Filter '*.obj' | ForEach-Object { '"' + $_.FullName + '"' }
    $rsp = Join-Path $objDir "$m.rsp"
    Set-Content -LiteralPath $rsp -Value $objs -Encoding ascii
    if ((Test-Path $lib) -and ((Get-Item $lib).LastWriteTime -gt (Get-ChildItem $obj -Filter '*.obj' | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime)) {
        Write-Host "lib $m : up to date"
    } else {
        & llvm-lib.exe /nologo "/OUT:$lib" "@$rsp"
        if ($LASTEXITCODE -ne 0) { throw "llvm-lib failed for $m" }
        Write-Host ("lib {0} : {1:N1} MB" -f $m, ((Get-Item $lib).Length / 1MB))
    }
    $libs += $lib
}

# ---- 3b. guest names: the dynsym tables of the lift, for the runtime's symbolisation -------------------
# The map names every lifted function "f_<guest VA>"; this sidecar turns those into the names the lift exported
# (movie::MediaDecoder::Stop). build-boot.ps1 copies it next to the exe. Best effort: a missing names file only
# means dumps keep the f_ names, so a lifter/sidecar problem must not fail the build.
$namesFile = Join-Path $libDir 'guest-symbols.names'
try {
    & (Join-Path $PSScriptRoot 'guest-symbols.ps1') -LiftDir $liftDir -OutFile $namesFile
    if ($LASTEXITCODE -ne 0) { throw "guest-symbols.ps1 exited $LASTEXITCODE" }
} catch {
    Write-Warning ("guest names not written: {0}. Thread dumps will show f_<VA> for lifted frames." -f $_.Exception.Message)
}

# ---- 4. stub main + link -------------------------------------------------------------------------------
$stubObj = Join-Path $build 'obj\stub'
New-Item -ItemType Directory -Force -Path $stubObj | Out-Null
foreach ($s in Get-ChildItem (Join-Path $root 'src') -Filter '*.cpp' | Where-Object { $_.Name -in @('stub_main.cpp', 'rt_stub.cpp') }) {
    $o = Join-Path $stubObj ($s.BaseName + '.obj')
    if ((Test-Path $o) -and ((Get-Item $o).LastWriteTime -gt $s.LastWriteTime)) { continue }
    & clang-cl.exe /nologo /std:c++20 /O2 /c "/I$inc" $s.FullName "/Fo$o"
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $($s.Name)" }
}
$binDir = Join-Path $build 'bin'
New-Item -ItemType Directory -Force -Path $binDir | Out-Null
$exe = Join-Path $binDir 'xc2ns2_stub.exe'
$linkRsp = Join-Path $build 'obj\link.rsp'
$lines = @()
$lines += (Get-ChildItem $stubObj -Filter '*.obj' | ForEach-Object { '"' + $_.FullName + '"' })
$lines += $libs | ForEach-Object { '"' + $_ + '"' }
Set-Content -LiteralPath $linkRsp -Value $lines -Encoding ascii
& clang-cl.exe /nologo "/Fe:$exe" "@$linkRsp"
if ($LASTEXITCODE -ne 0) { throw 'link failed' }
Write-Host ("link: {0} ({1:N1} MB)" -f $exe, ((Get-Item $exe).Length / 1MB))

$sw.Stop()
Write-Host ("total: {0:N1}s" -f $sw.Elapsed.TotalSeconds)
if ($Run) {
    Write-Host ''
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "smoke test failed (exit $LASTEXITCODE)" }
}


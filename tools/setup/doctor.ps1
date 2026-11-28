#Requires -Version 5.1
<#
.SYNOPSIS
  Say what this machine is missing to build the project, and nothing else.

.DESCRIPTION
  Checks the toolchain versions and every external source in deps.lock.json. Reports only; it never installs
  anything unless -Install is given, because a silent install turns "the build is not reproducible" into "the
  install failed halfway".

.EXAMPLE
  pwsh -File tools/setup/doctor.ps1
  pwsh -File tools/setup/doctor.ps1 -CacheDir D:\deps -Install
#>
param(
    # The archive cache CMake uses (gfx_deps.cmake: NXRT_DEPS_DIR), so a working machine reports "ok" rather than
    # "not in the cache" for sources that are already there.
    [string]$CacheDir = (Join-Path $env:LOCALAPPDATA 'nxrt\deps'),
    [switch]$Install,
    [switch]$Json
)
$ErrorActionPreference = 'Stop'

# The toolchain lives in a directory that env.ps1 puts on PATH, so a machine that HAS cmake and ninja reports them as
# missing unless that script ran first - which is what a first-time user hits, and the advice ("install cmake") is then
# wrong. Source it here when XC2_ENV names it, so the report describes the machine rather than the shell.
if ($env:XC2_ENV -and (Test-Path $env:XC2_ENV)) {
    . $env:XC2_ENV | Out-Null
}
$lockPath = Join-Path $PSScriptRoot 'deps.lock.json'
if (-not (Test-Path $lockPath)) { throw "no deps.lock.json next to this script" }
$lock = Get-Content $lockPath -Raw | ConvertFrom-Json

$problems = @()
$report = @()

# Two sources can share a cache file name (the two Vulkan archives are both vulkan-sdk-1.4.363.0.zip), in which case
# the CMake download dir holds one file for two of them and a hash comparison proves nothing.
$leafOf = @{}
foreach ($s in $lock.sources) {
    if ($s.dest -or -not $s.url) { continue }
    $leaf = ($s.url -split '/')[-1]
    if (-not $leafOf.ContainsKey($leaf)) { $leafOf[$leaf] = @() }
    $leafOf[$leaf] += $s.name
}
function Add-Row([string]$name, [string]$state, [string]$detail) {
    $script:report += [pscustomobject]@{ name = $name; state = $state; detail = $detail }
    if ($state -in @('missing', 'version', 'empty', 'hash')) { $script:problems += "$name : $detail" }
}

# --- the toolchain: versions, not hashes ---------------------------------------------------------------
$commands = @{ 'clang-cl' = 'clang-cl'; 'cmake' = 'cmake'; 'ninja' = 'ninja'; 'dotnet' = 'dotnet'; 'python' = 'python'; 'rustc' = 'rustc' }
foreach ($p in $lock.toolchain.PSObject.Properties) {
    $name = $p.Name
    if ($name -eq 'note') { continue }
    $want = $p.Value.version
    $cmd = $commands[$name]
    $exe = Get-Command $cmd -ErrorAction SilentlyContinue
    if (-not $exe) {
        Add-Row $name 'missing' "not on PATH (want $want)"
        continue
    }
    $out = & $exe.Source --version 2>&1 | Select-Object -First 1
    if ($out -match [regex]::Escape($want)) { Add-Row $name 'ok' $want }
    else { Add-Row $name 'version' "found '$out', lock says $want" }
}

# --- the sources ---------------------------------------------------------------------------------------
# The cache is keyed by the file name the URL ends with (CMake stores it that way), so look for that first and the
# lock's own name second.
function Get-Cached($src) {
    if (-not $CacheDir -or -not (Test-Path $CacheDir)) { return $null }
    $leaf = ($src.url -split '/')[-1]
    foreach ($n in @($leaf, $src.name)) {
        $p = Join-Path $CacheDir $n
        if ($n -and (Test-Path $p)) { return $p }
    }
    return $null
}

# A checkout that is already on disk is checked against the lock's commit: that is the only way to know the bytes
# being compiled are the bytes that were verified, and it is the check a reporter can run for us.
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
foreach ($s in $lock.sources) {
    if (-not $s.checkout) { continue }
    $dir = Join-Path $root $s.checkout
    if (-not (Test-Path (Join-Path $dir '.git'))) { continue }
    $head = (& git -C $dir rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) { continue }
    if ($head -eq $s.commit) { Add-Row ($s.name + ' (checkout)') 'ok' ($head.Substring(0, 7) + ' matches the lock') }
    else { Add-Row ($s.name + ' (checkout)') 'version' ("on disk " + $head.Substring(0, 7) + ", lock says " + $s.commit.Substring(0, 7)) }
}

foreach ($s in $lock.sources) {
    if ($s.kind -eq 'git') {
        if (-not $s.commit) {
            Add-Row $s.name 'unpinned' "no commit recorded: a build today and a build next month can differ ($($s.url))"
            continue
        }
        $null = git ls-remote --heads $s.url 2>$null
        if ($LASTEXITCODE -eq 0) { Add-Row $s.name 'ok' "$($s.commit), reachable" }
        else { Add-Row $s.name 'unreachable' "$($s.url) does not answer; $($s.risk)" }
        continue
    }
    # Sources that a script of ours downloads to its own place are checked there, not in the CMake cache.
    if ($s.dest) {
        $d = Join-Path $root $s.dest
        if (Test-Path $d) {
            $n = (Get-ChildItem $d -Recurse -File -ErrorAction SilentlyContinue | Measure-Object).Count
            if ($n -gt 0) { Add-Row $s.name 'ok' ("present at " + $s.dest + " ($n files)") }
            else { Add-Row $s.name 'empty' ($s.dest + " exists but holds no files") }
        } else {
            Add-Row $s.name $(if ($s.optional) { 'note' } else { 'missing' }) ("not at " + $s.dest + "; run the script that fetches it (BUILD.md section 2)")
        }
        continue
    }
    $cached = Get-Cached $s
    if ($cached) {
        # A zero-byte file in the cache is a download that failed and was left behind; the hash check would only say
        # "wrong hash", which reads like a supply-chain problem rather than a broken cache.
        $leaf = ($s.url -split '/')[-1]
        if ($leafOf[$leaf].Count -gt 1) {
            Add-Row $s.name 'note' ("shares the cache file $leaf with " + (($leafOf[$leaf] | Where-Object { $_ -ne $s.name }) -join ', ') +
                                           "; the hash cannot be attributed, check the CMake configure output instead")
            continue
        }
        if ((Get-Item $cached).Length -eq 0) {
            Add-Row $s.name 'empty' "$cached is 0 bytes: a failed download is sitting in the cache, delete it and rebuild"
            continue
        }
        if ($s.sha256) {
            $h = (Get-FileHash $cached -Algorithm SHA256).Hash.ToLower()
            if ($h -eq $s.sha256) { Add-Row $s.name 'ok' "cached, sha256 matches" }
            else { Add-Row $s.name 'hash' "$cached is $h, lock says $($s.sha256)" }
        } else { Add-Row $s.name 'ok' "cached (no hash in the lock)" }
        continue
    }
    if (-not $s.sha256) {
        Add-Row $s.name 'unpinned' "no sha256 in the lock ($($s.url))"
        continue
    }
    if ($Install -and $CacheDir) {
        New-Item -ItemType Directory -Force $CacheDir | Out-Null
        $ext = if ($s.url -match '\.(zip|tar\.gz|tgz)$') { $Matches[0] } else { '' }
        $dest = Join-Path $CacheDir ($s.name + $ext)
        Write-Host "  fetching $($s.name)"
        & curl.exe -sSL --fail --retry 3 --retry-delay 2 -o $dest $s.url
        if ($LASTEXITCODE -ne 0) { Add-Row $s.name 'fetch' "download failed: $($s.url)"; continue }
        $h = (Get-FileHash $dest -Algorithm SHA256).Hash.ToLower()
        if ($h -eq $s.sha256) { Add-Row $s.name 'ok' "fetched, sha256 matches" }
        else { Add-Row $s.name 'hash' "fetched $h, lock says $($s.sha256)" }
        continue
    }
    Add-Row $s.name 'missing' "not in the cache; fetch: curl -L -o <CacheDir>/$($s.name) $($s.url)"
}

# --- what a clean build cannot get from this repository ------------------------------------------------
foreach ($g in $lock.generated_artifacts) {
    $report += [pscustomobject]@{ name = $g.name; state = 'note'; detail = $g.action }
}

if ($Json) {
    $report | ConvertTo-Json -Depth 4
} else {
    foreach ($r in $report) {
        $mark = switch ($r.state) { 'ok' { '  ok  ' } 'note' { ' note ' } default { ' FAIL ' } }
        '{0} {1,-18} {2}' -f $mark, $r.name, $r.detail
    }
    ''
    if ($problems.Count -eq 0) { Write-Host 'nothing missing' }
    else {
        Write-Host ($problems.Count.ToString() + ' problem(s):')
        foreach ($p in $problems) { Write-Host ('  - ' + $p) }
    }
}
if ($problems.Count -gt 0) { exit 1 }

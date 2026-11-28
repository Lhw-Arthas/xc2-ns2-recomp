# SPDX-License-Identifier: GPL-3.0-or-later
# Check a package tree, or an install made from it, against the layout the release promises.
#
#   pwsh tools\build\verify-package.ps1 -Path D:\dist\xc2-package-0.1.3
#   pwsh tools\build\verify-package.ps1 -Path D:\dist\xc2-test\XC2-package-dist-0.1.3 -ExpectedSpirv 12137
#
# Exit 0 when every check passes, 1 when one fails. make-package.ps1 runs it on the tree it just wrote;
# run it again on the folder a player ends up with, which is where debug leftovers accumulate.
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [ValidateSet('auto', 'package', 'installed')][string]$Mode = 'auto',
    [int]$ExpectedSpirv = 0,
    [string]$ExpectCommit = '',
    [switch]$AllowNoFFmpeg    # make-package's own switch: a package built without the film decoders on purpose
)
$ErrorActionPreference = 'Stop'
$fails = New-Object System.Collections.ArrayList
function Fail([string]$what) { [void]$fails.Add($what); Write-Host ("  FAIL  " + $what) }
function Ok([string]$what) { Write-Host ("  ok    " + $what) }
function Need([string]$rel) {
    if (Test-Path -LiteralPath (Join-Path $Path $rel)) { Ok $rel } else { Fail ("missing: " + $rel) }
}
function Absent([string]$rel) {
    if (Test-Path -LiteralPath (Join-Path $Path $rel)) { Fail ("must not be here: " + $rel) } else { Ok ("absent: " + $rel) }
}

if (-not (Test-Path -LiteralPath $Path)) { throw "no such tree: $Path" }
$Path = (Resolve-Path -LiteralPath $Path).Path
if ($Mode -eq 'auto') {
    $Mode = if ((Test-Path -LiteralPath (Join-Path $Path 'setup_state.tsv')) -or (Test-Path -LiteralPath (Join-Path $Path 'xc2_boot.exe'))) { 'installed' } else { 'package' }
}
Write-Host ("verify " + $Mode + ": " + $Path)

# ---- what every tree carries ---------------------------------------------------------------------------------------
Need 'README.txt'
Need 'RELEASE-NOTES.md'
Need 'licenses'
Need 'xc2-launcher.exe'
Need (Join-Path 'runtime' 'xc2_rt.dll')
# The film's hardware decoders. The runtime loads them from its own directory and the setup copies every runtime DLL
# beside the exe; a package that lost them still runs, but every 4K opening drops to the CPU decoders (§237, issue #2).
$ffDlls = @('avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll', 'swresample-7.dll', 'swscale-10.dll')
function MaybeNeed([string]$rel) {
    if ($AllowNoFFmpeg) { Write-Host ("  skip  " + $rel + " (no FFmpeg, on purpose)") } else { Need $rel }
}
if ($Mode -eq 'package') {
    foreach ($n in $ffDlls) { MaybeNeed (Join-Path 'runtime' $n) }
} elseif (Test-Path -LiteralPath (Join-Path $Path 'runtime\avcodec-63.dll')) {
    # An install made from a package that carries them must have them beside the exe: that is where Windows looks,
    # and an in-place upgrade used to leave them behind (0xC0000135, issue #2).
    foreach ($n in $ffDlls) { MaybeNeed $n }
}
Need (Join-Path 'engine' 'xc2_setup.py')

# ---- what must never be in either ----------------------------------------------------------------------------------
# Our own binaries never ship with symbols. The bundled tools may keep theirs: a .pdb under tools\ is the
# publisher's, not a debugging leftover of this project (tools\spirv_harness ships one per assembly).
# A played install writes nxrt.log by itself; that is the runtime's log, not a leftover. It still must not be in a
# package that has never been run.
$logAllowed = ($Mode -eq 'installed')
$forbidden = Get-ChildItem -LiteralPath $Path -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object {
    $rel = $_.FullName.Substring($Path.Length + 1)
    ($_.Name -like '*.pdb' -and $rel -notlike 'tools\*') -or ($_.Name -like 'nxrt.log*' -and -not $logAllowed) -or
    $_.Name -like 'stall_*.txt' -or $_.Name -like '*.bak-*' -or $_.Name -like '*.ilk' -or $_.Name -like '*.dmp' -or $_.Name -eq 'core'
}
if ($forbidden) {
    foreach ($f in $forbidden) { Fail ("debug leftover: " + $f.FullName.Substring($Path.Length + 1) + " (" + [math]::Round($f.Length / 1MB, 1) + " MB)") }
} else { Ok 'no pdb, log, stall or backup files' }

if ($Mode -eq 'package') {
    foreach ($d in 'setup_state.tsv', 'xc2_boot.exe', 'shader_cache', 'nup_dump', 'exefs', 'romfs', 'save', 'work', 'setup.log', 'nxrt.ini') {
        Absent $d
    }
    Ok 'package mode: no install artefacts'
} else {
    Need 'xc2_boot.exe'
    Need 'setup_state.tsv'
    Need 'nxrt.ini'
    $state = Get-Content -LiteralPath (Join-Path $Path 'setup_state.tsv') -Raw
    if ($state -match 'finished\s+1') { Ok 'setup finished' } else { Fail "setup_state.tsv does not say finished: $($state.Trim())" }
    # build.json is the only thing that ties an exe to a commit: check it when the caller names one.
$manifest = Join-Path $Path 'build.json'
if (-not (Test-Path -LiteralPath $manifest)) { $manifest = Join-Path $Path 'build.json' }
if ($ExpectCommit) {
    if (-not (Test-Path -LiteralPath $manifest)) { Fail "no build.json in $Path, so the commit cannot be checked" }
    else {
        $j = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
        $want = $ExpectCommit.ToLower(); $got = ([string]$j.commit).ToLower()
        if ($got.StartsWith($want)) { Ok ("build.json: " + $got.Substring(0, [Math]::Min(7, $got.Length)) + " " + $j.profile) }
        else { Fail ("build.json says " + $got + ", expected " + $want) }
    }
} elseif (Test-Path -LiteralPath $manifest) {
    $j = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    Write-Host ("  note  build.json: " + ([string]$j.commit).Substring(0, 7) + " " + $j.profile)
}
$spvDir = Join-Path $Path 'shader_cache\spirv'
    if (Test-Path -LiteralPath $spvDir) {
        $n = (Get-ChildItem -LiteralPath $spvDir -Filter '*.spv' -File -ErrorAction SilentlyContinue).Count
        if ($ExpectedSpirv -gt 0) {
            if ($n -eq $ExpectedSpirv) { Ok ("shader cache: $n modules") } else { Fail ("shader cache: $n modules, expected $ExpectedSpirv") }
        } else { Ok ("shader cache: $n modules") }
    } else { Fail 'missing: shader_cache\spirv' }
    foreach ($d in 'work', 'save', 'nup_dump') {
        if (Test-Path -LiteralPath (Join-Path $Path $d)) { Write-Host ("  note  " + $d + " is present (install data, not shipped)") }
    }
}

Write-Host ''
if ($fails.Count) {
    Write-Host ("FAILED: " + $fails.Count + " check(s)")
    exit 1
}
Write-Host 'verify: all checks pass'
exit 0

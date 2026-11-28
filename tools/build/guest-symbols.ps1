# SPDX-License-Identifier: GPL-3.0-or-later
# The lift's dynsym tables as a name sidecar, so a thread dump can say
# "subsdk0 movie::MediaDecoder::Stop" instead of "f_760001be70".
#
#   tools\build\build.ps1     writes <LibDir>\guest-symbols.names from <LiftDir>
#   tools\build\build-boot.ps1 copies it next to the exe as xc2_boot.names
#   runtime\src\diag\symbols.cpp reads it: the linker map already carries one entry per lifted function, named
#   f_<guest VA>; the sidecar gives that same address a real name, so host frames and guest chains both get one.
#
# Format, one line per symbol, ascending address (the runtime binary-searches it):
#
#   0x000000760001be70<TAB>subsdk0<TAB>movie::MediaDecoder::Stop
#
# Parameters are dropped (--no-params): a dump line is a chain of a dozen names, and class::method identifies the
# frame; the mangled name stays in the lift and the map when the exact overload is needed.
#
#   pwsh -File tools\build\guest-symbols.ps1 -LiftDir xc2\build\301\lift -OutFile xc2\build\301\lib\guest-symbols.names
param(
    [Parameter(Mandatory = $true)][string]$LiftDir,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [string]$Demangler = '',   # llvm-cxxfilt; found on PATH or in the dev toolchain when empty
    [string[]]$Modules = @('main', 'sdk', 'subsdk0', 'subsdk1', 'subsdk2', 'subsdk3', 'rtld')
)
$ErrorActionPreference = 'Stop'
if (-not [System.IO.Path]::IsPathRooted($LiftDir)) { $LiftDir = Join-Path (Get-Location).Path $LiftDir }
if (-not [System.IO.Path]::IsPathRooted($OutFile)) { $OutFile = Join-Path (Get-Location).Path $OutFile }
if (-not (Test-Path -LiteralPath $LiftDir)) { throw "lift dir not found: $LiftDir" }

if (-not $Demangler) {
    foreach ($cand in @('llvm-cxxfilt.exe', 'llvm-cxxfilt') + @(
            (Join-Path $env:ProgramFiles 'LLVM\bin\llvm-cxxfilt.exe'))) {
        $c = Get-Command $cand -ErrorAction SilentlyContinue
        if ($c) { $Demangler = $c.Source; break }
        if ($cand -and (Test-Path -LiteralPath $cand)) { $Demangler = $cand; break }
    }
}
if (-not $Demangler) { Write-Warning 'no llvm-cxxfilt: the sidecar keeps mangled names (_ZN5movie...), readable but noisier' }

# One entry per symbol: { "mangled", 0x<guest VA>ULL, <flags> },  0x0 means "imported, defined elsewhere".
$entryRe = [regex]'^\s*\{\s*"((?:[^"\\]|\\.)*)",\s*0x([0-9a-fA-F]+)ULL,\s*(\d+)\s*\},?\s*$'
$syms = [System.Collections.Generic.List[object]]::new()
$seen = [System.Collections.Generic.HashSet[string]]::new()
foreach ($m in $Modules) {
    $file = Join-Path (Join-Path $LiftDir $m) "module_$m.cpp"
    if (-not (Test-Path -LiteralPath $file)) { continue }
    $n = 0
    foreach ($line in [System.IO.File]::ReadLines($file)) {
        $mt = $entryRe.Match($line)
        if (-not $mt.Success) { continue }
        $addr = [Convert]::ToUInt64($mt.Groups[2].Value, 16)
        if ($addr -eq 0) { continue }
        $name = $mt.Groups[1].Value
        if (-not $name) { continue }
        if (-not $seen.Add(('{0:x}-{1}' -f $addr, $name))) { continue }
        $syms.Add([pscustomobject]@{ Addr = $addr; Module = $m; Name = $name })
        $n++
    }
    Write-Host ('{0,-8} {1} names' -f $m, $n)
}
if (-not $syms.Count) { throw "no dynsym entries under $LiftDir (do the module_*.cpp files exist?)" }

# Demangle in one batch: tens of thousands of names through a single process, not one each.
$mangled = @($syms | Where-Object { $_.Name.StartsWith('_Z') } | ForEach-Object { $_.Name } | Sort-Object -Unique)
$demangled = @{}
if ($Demangler -and $mangled.Count) {
    $out = @($mangled | & $Demangler --no-params)
    if ($LASTEXITCODE -ne 0) { throw "demangler failed (exit $LASTEXITCODE): $Demangler" }
    if ($out.Count -ne $mangled.Count) { throw "demangler returned $($out.Count) lines for $($mangled.Count) names" }
    for ($i = 0; $i -lt $mangled.Count; $i++) { $demangled[$mangled[$i]] = ([string]$out[$i]).Trim() }
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutFile) | Out-Null
$tab = [char]9
$sorted = $syms | Sort-Object Addr
$sw = [System.IO.StreamWriter]::new($OutFile, $false, [System.Text.Encoding]::ASCII)
try {
    foreach ($s in $sorted) {
        $name = if ($demangled.ContainsKey($s.Name)) { $demangled[$s.Name] } else { $s.Name }
        # A tab or a newline inside a name would break the three-column format; take them out rather than crash.
        $name = $name -replace '[\t\r\n]', ' '
        $sw.Write(('0x{0:x16}' -f $s.Addr) + $tab + $s.Module + $tab + $name)
        $sw.WriteLine()
    }
} finally { $sw.Dispose() }
$kb = [math]::Round((Get-Item -LiteralPath $OutFile).Length / 1KB, 1)
Write-Host ("guest symbols: {0} names -> {1} ({2} KB)" -f $sorted.Count, $OutFile, $kb)

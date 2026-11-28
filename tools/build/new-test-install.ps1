# SPDX-License-Identifier: GPL-3.0-or-later
# Make a test install from a release zip, and hold it to the same checks a shipped tree gets.
#
#   pwsh tools\build\new-test-install.ps1 -Zip D:\dist\XC2-package-dist-0.1.3.zip
#   pwsh tools\build\new-test-install.ps1 -Zip ... -Nup "D:\dump\xc2 3.0.1.nup"     # runs the setup too (~12 min)
#
# Without -Nup the tree is only unpacked and checked as a package (no game data, no exe): that is what a player
# downloads. With -Nup the package's own setup ingests the dump, lifts and links it, builds the shader cache, and the
# result is checked as an install (exe, setup_state, 12,137 modules).
param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [string]$Version = '',
    [string]$Root = '',
    [string]$Nup = '',
    [int]$ExpectedSpirv = 12137,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Root) { $Root = Join-Path (Split-Path -Parent $repoRoot) 'test' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
if (-not (Test-Path -LiteralPath $Zip)) { throw "no zip at $Zip" }
if (-not $Version) {
    $m = [regex]::Match((Split-Path $Zip -Leaf), '(\d+\.\d+\.\d+)')
    if (-not $m.Success) { throw "cannot read a version from the file name: pass -Version" }
    $Version = $m.Groups[1].Value
}
$target = Join-Path $Root $Version
if (Test-Path -LiteralPath $target) {
    if (-not $Force) { throw "$target exists: pass -Force to replace it, or another -Version" }
    Remove-Item -LiteralPath $target -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $target | Out-Null
$t0 = Get-Date
Write-Host ("unpack  {0} -> {1}" -f (Split-Path $Zip -Leaf), $target)
[System.IO.Compression.ZipFile]::ExtractToDirectory($Zip, $target)
Write-Host ("        {0:N1} s, {1:N0} MB" -f ((Get-Date) - $t0).TotalSeconds, ((Get-ChildItem $target -Recurse -File | Measure-Object Length -Sum).Sum / 1MB))

$verify = Join-Path $PSScriptRoot 'verify-package.ps1'
Write-Host 'check   the package as a player receives it'
& pwsh -NoProfile -File $verify -Path $target
if ($LASTEXITCODE -ne 0) { throw "the package failed verify-package.ps1: $target" }

if ($Nup) {
    if (-not (Test-Path -LiteralPath $Nup)) { throw "no dump at $Nup" }
    $py = Join-Path $target 'tools\python\python.exe'
    # The setup shells out to "python" for its helpers: put the package's own interpreter first, or a machine with
    # another python on PATH (this one has the DeepSeek runtime's) runs the helpers without zstandard.
    $env:PATH = (Join-Path $target 'tools\python') + ';' + $env:PATH
    $env:XC2_PYTHON = $py
    $engine = Join-Path $target 'engine\xc2_setup.py'
    $t1 = Get-Date
    Write-Host ("setup   {0} (this is the long one)" -f (Split-Path $Nup -Leaf))
    & $py -u $engine --repo $target --install $target --nup $Nup
    if ($LASTEXITCODE -ne 0) { throw "the setup failed (see $target\setup.log)" }
    Write-Host ("        {0:N0} s" -f ((Get-Date) - $t1).TotalSeconds)
    Write-Host 'check   the install'
    & pwsh -NoProfile -File $verify -Path $target -ExpectedSpirv $ExpectedSpirv
    if ($LASTEXITCODE -ne 0) { throw "the install failed verify-package.ps1: $target" }
}

Write-Host ''
Write-Host ("ready: {0}" -f $target)
Write-Host ("play:  {0}" -f (Join-Path $target 'xc2-launcher.exe'))

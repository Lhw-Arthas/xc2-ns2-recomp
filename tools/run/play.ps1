# SPDX-License-Identifier: GPL-3.0-or-later
# Start the game from a profile: config/profiles/<name>.ini holds every path and switch, this script applies them.
#
#   pwsh tools\run\play.ps1 -Profile dev
#   pwsh tools\run\play.ps1 -Profile dev -Fps 30 -Headless -Seconds 40
#
# The profile is data: the same engine can start the development build, a packaged install or an A/B run without
# another .cmd file full of set lines. tools\run\play-301.cmd is now a shim over this script.
param(
    [string]$Profile = 'dev',
    [string]$ProfileFile = '',
    [int]$Fps = 0,
    [switch]$Headless,
    [switch]$NoPrefill,
    [switch]$Dump,
    [string]$Shots = '',
    [int]$Seconds = 0,
    [string]$Exe = '',
    [string]$Log = '',
    [int]$Retries = 2,
    [int]$RetryDelay = 2,
    [hashtable]$Env = @{}
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # tools\run -> the checkout
if (-not $ProfileFile) { $ProfileFile = Join-Path $repo ("config\\profiles\\" + $Profile + ".ini") }
if (-not (Test-Path -LiteralPath $ProfileFile)) { throw "no profile at $ProfileFile" }

# A small ini reader: [section], key = value, # and ; comments. Paths resolve against the checkout.
$cfg = @{}
$section = ''
foreach ($line in Get-Content -LiteralPath $ProfileFile) {
    $l = $line.Trim()
    if (-not $l -or $l.StartsWith('#') -or $l.StartsWith(';')) { continue }
    if ($l -match '^\[(.+)\]$') { $section = $matches[1].Trim(); continue }
    $i = $l.IndexOf('=')
    if ($i -lt 1) { continue }
    $cfg[$section + '/' + $l.Substring(0, $i).Trim()] = $l.Substring($i + 1).Trim()
}
function Get-Cfg([string]$key, [string]$fallback = '') {
    if ($cfg.ContainsKey($key)) { return $cfg[$key] }
    return $fallback
}
function Resolve-Under([string]$value) {
    if (-not $value) { return '' }
    if ([System.IO.Path]::IsPathRooted($value)) { return $value }
    return (Join-Path $repo $value)
}

$exe = if ($Exe) { $Exe } else { Resolve-Under (Get-Cfg 'run/exe') }
$cwd = Resolve-Under (Get-Cfg 'run/cwd')
$exefs = Resolve-Under (Get-Cfg 'run/exefs')
$romfs = Resolve-Under (Get-Cfg 'run/romfs')
$dlc = Resolve-Under (Get-Cfg 'run/dlc')
$save = Resolve-Under (Get-Cfg 'run/save')
$cache = Resolve-Under (Get-Cfg 'run/shader_cache')
$missing = Resolve-Under (Get-Cfg 'run/missing')
$logs = Resolve-Under (Get-Cfg 'run/logs')
$gf = if ($Headless) { 'headless' } else { Get-Cfg 'run/gfx' 'window' }
$fps = if ($Fps -gt 0) { $Fps } else { [int](Get-Cfg 'run/fps' '60') }
$hz = [int](Get-Cfg 'run/hz' '60')
if (-not (Test-Path -LiteralPath $exe)) { throw "no exe at $exe (build it first: tools\\build\\build-boot.ps1)" }
if (-not (Test-Path -LiteralPath $cwd)) { throw "no working directory at $cwd" }
foreach ($d in $logs, $missing, $save) { if ($d) { New-Item -ItemType Directory -Force -Path $d | Out-Null } }

# The retail 3.0.1 switches come from tools/setup/xc2_setup.py (GAME_ENV), the list a package writes into
# launch.env, so a dev run cannot drift from a package run. The profile's [env] applies afterwards and is for
# dev-only switches (NXRT_NVN_DS_DEBUG, a wider language list) - never for a copy of GAME_ENV.
$gameEnvPy = if ($env:XC2_PYTHON) { $env:XC2_PYTHON } else { 'python' }
try {
    $gameEnv = & $gameEnvPy (Join-Path $repo 'tools/setup/xc2_setup.py') --emit-env 2>$null
    foreach ($line in $gameEnv) {
        if (-not $line) { continue }
        $eq = $line.IndexOf('=')
        if ($eq -gt 0) { Set-Item -Path ('env:' + $line.Substring(0, $eq)) -Value $line.Substring($eq + 1) }
    }
} catch {
    Write-Warning "play.ps1: could not read GAME_ENV from tools/setup/xc2_setup.py ($_); the launch may differ from a package"
}
foreach ($k in $cfg.Keys) {
    if ($k.StartsWith('env/')) { Set-Item -Path ('env:' + $k.Substring(4)) -Value $cfg[$k] }
}
foreach ($k in $Env.Keys) { Set-Item -Path ('env:' + $k) -Value $Env[$k] }
$env:NXRT_XC2_FPS = "$fps"
$env:NXRT_NVN_PRESENT_INTERVAL_HZ = "$hz"
$env:NXRT_FS_ROMFS = $romfs
$env:NXRT_FS_SAVE_DIR = $save
$env:NXRT_AOC_DIR = $dlc
$env:NXRT_NVN_SHADER_CACHE = $cache
$env:NXRT_NVN_DUMP_MISSING = $missing
$env:NXRT_NVN_SKIP_FS_FILE = Join-Path $repo 'work\\skip_fs.txt'
$env:NXRT_NVN_HOT_STATE_FILE = Join-Path $repo 'work\\hot_state.txt'
if (-not (Test-Path -LiteralPath $env:NXRT_NVN_SKIP_FS_FILE)) { New-Item -ItemType File -Path $env:NXRT_NVN_SKIP_FS_FILE -Force | Out-Null }
if (-not (Test-Path -LiteralPath $env:NXRT_NVN_HOT_STATE_FILE)) { Set-Content -LiteralPath $env:NXRT_NVN_HOT_STATE_FILE -Value ('front_face=' + (Get-Cfg 'env/NXRT_NVN_FRONT_FACE_MODE' '2')) }
if ($Dump) { New-Item -ItemType Directory -Force -Path $missing | Out-Null; $env:NXRT_NVN_DUMP_ALL = '1' }

# The upscaler hot file: start every run empty so [dlss] upscaler from nxrt.ini takes effect.
$hot = Join-Path $repo 'work\\dlss_hot.txt'
Set-Content -LiteralPath $hot -Value '' -NoNewline -Encoding ascii
$env:NXRT_DLSS_HOT_FILE = $hot

# The preload pack is read before the loose .spv files, so a pack older than a replaced shader hides it.
$pack = Join-Path (Split-Path $cache -Parent) 'spirv.preload.pack'
if (Test-Path -LiteralPath $pack) { Remove-Item -LiteralPath $pack -Force }

if (-not $Log) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $Log = Join-Path $logs ('play-' + $stamp + '.log')
}
if ($Shots) {
    $dir = Join-Path $repo ('work\\shots\\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $env:NXRT_NVN_SHOT_FRAMES = $Shots
    $env:NXRT_NVN_SHOT_DIR = $dir
    Write-Host "shots: $dir"
}

if (-not $NoPrefill -and -not $env:XC2_NOPREFILL) {
    & pwsh -NoProfile -File (Join-Path $repo 'tools\\run\\prefill.ps1') -Exe $exe -Exefs $exefs -Cache $cache -Work (Join-Path $repo 'work')
}
if (-not $exefs -or -not (Test-Path -LiteralPath $exefs)) { throw "no exefs at $exefs" }
if (-not (Test-Path -LiteralPath $romfs)) { throw "no romfs at $romfs" }

& pwsh -NoProfile -File (Join-Path $repo 'tools\\run\\build-stamp.ps1') -Exe $exe
Write-Host ("profile {0}: {1} at {2} fps" -f $Profile, $exe, $fps)
# A launch sometimes dies before the first frame, in the address allocator's second 60 GiB pass: MapPhysicalMemory
# answers 0xD401 ("range holds other memory") and svcBreak ends the process (HANDOVER 171). It is a collision between
# the two setup passes, not a broken build: the same exe starts on the next attempt. Retrying here is what the operator
# does by hand, and it keeps an experiment from being lost to a launch that says nothing about the change under test.
function Test-VammCollision([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    $held = Select-String -LiteralPath $path -Pattern 'MapPhysicalMemory \(range holds other memory\) returned 0xD401' -Quiet
    if (-not $held) { return $false }
    return [bool](Select-String -LiteralPath $path -Pattern 'svc: svcBreak' -Quiet)
}

# One line per launch attempt, so the 0xD401 rate has a denominator instead of an impression (work\logs is ignored).
function Add-LaunchRecord([int]$n, $run, [bool]$collision, [string]$logFile) {
    $dir = Join-Path $repo 'work\logs'
    if (-not (Test-Path -LiteralPath $dir)) { return }
    $file = Join-Path $dir 'launch-attempts.tsv'
    if (-not (Test-Path -LiteralPath $file)) {
        Set-Content -LiteralPath $file -Value (@('stamp', 'profile', 'exe', 'attempt', 'exit', 'vamm_d401', 'log') -join [char]9) -Encoding ascii
    }
    $row = @((Get-Date -Format 'yyyy-MM-ddTHH:mm:ss'), $Profile, (Split-Path -Leaf $exe), $n, $run.ExitCode,
             $(if ($collision) { 1 } else { 0 }), (Split-Path -Leaf $logFile))
    Add-Content -LiteralPath $file -Value ($row -join [char]9) -Encoding ascii
}

# One launch. ExitedOnItsOwn is false when the timer stopped a healthy run, which says nothing about the launch and
# must not be retried (it would just restart the clock).
function Invoke-Launch([string]$logFile) {
    $argv = @('--exefs', $exefs, '--link', (Get-Cfg 'run/link' 'host'), '--gfx', $gf, '--log', $logFile)
    Push-Location $cwd
    try {
        if ($Seconds -gt 0) {
            $proc = Start-Process -FilePath $exe -ArgumentList $argv -PassThru -NoNewWindow
            Write-Host ("pid {0}; stopping after {1}s" -f $proc.Id, $Seconds)
            $deadline = (Get-Date).AddSeconds($Seconds)
            while (-not $proc.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
            if (-not $proc.HasExited) {
                $proc.Kill()
                $proc.WaitForExit(5000) | Out-Null
                return [pscustomobject]@{ ExitedOnItsOwn = $false; ExitCode = 0 }
            }
            return [pscustomobject]@{ ExitedOnItsOwn = $true; ExitCode = $proc.ExitCode }
        }
        & $exe @argv
        return [pscustomobject]@{ ExitedOnItsOwn = $true; ExitCode = $LASTEXITCODE }
    } finally { Pop-Location }
}

$attempt = 0
$currentLog = $Log
while ($true) {
    $attempt++
    $run = Invoke-Launch $currentLog
    $collision = $run.ExitedOnItsOwn -and (Test-VammCollision $currentLog)
    Add-LaunchRecord -n $attempt -run $run -collision $collision -logFile $currentLog
    if (-not $collision) { break }
    if ($attempt -gt $Retries) { break }
    Write-Host ("launch {0} died in the VAMM 0xD401 path (HANDOVER 171); starting again in {1}s (retry {2} of {3})" -f
        $attempt, $RetryDelay, $attempt, $Retries)
    Start-Sleep -Seconds $RetryDelay
    # Every attempt keeps its own log: the crash one is the evidence for the retry, and the run under test is the next.
    $currentLog = Join-Path ([IO.Path]::GetDirectoryName($Log)) (
        [IO.Path]::GetFileNameWithoutExtension($Log) + '-r' + ($attempt + 1) + '.log')
    Write-Host "retry log: $currentLog"
}
Write-Host "log: $currentLog"
if ($run.ExitedOnItsOwn -and $run.ExitCode -ne 0) { Write-Host ("launch exited {0}" -f $run.ExitCode) }

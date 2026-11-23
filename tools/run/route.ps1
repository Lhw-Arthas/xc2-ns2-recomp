# tools\run\route.ps1 - record, replay and A/B one input route (HANDOVER 228).
#
# A route is the input of one run, written down by the runtime (XC2_ROUTE_OUT) and replayed by it (XC2_ROUTE):
#
#     pwsh -File tools\run\route.ps1 -Record E:\XC2\dev\work\routes\opening.txt -Seconds 120
#         play; every button and stick change is written with its millisecond
#     pwsh -File tools\run\route.ps1 -Play E:\XC2\dev\work\routes\opening.txt -Seconds 60
#         the same run again, from the file
#     pwsh -File tools\run\route.ps1 -Ab opening.txt -A NXRT_NVN_PUSH_UNIFORMS=1 -B NXRT_NVN_PUSH_UNIFORMS=0
#         both configurations on the same route, compared window by window
#
# The comparison uses [nvn] replay_stats = 60 (one line a second with the replay cost). Windows are paired by
# position and kept only when the two runs drew the same number of frames within -Tolerance, because a route does
# not make the game frame-exact: it makes it land in the same scene, and the filter keeps the comparison honest.
# Raw windows go to <route>.ab.csv next to the route.
param(
    [string]$Record,
    [string]$Play,
    [string]$Ab,
    [string[]]$A = @(),
    [string[]]$B = @(),
    [string]$Press,
    [int]$Repeat = 1,
    [int]$Seconds = 40,
    [string]$Profile = 'dev',
    [double]$Tolerance = 0.02,
    [int]$MinWindows = 4
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$playScript = Join-Path $root 'tools\run\play.ps1'
if (-not (Test-Path -LiteralPath $playScript)) { throw "no play.ps1 at $playScript" }
if (-not $env:XC2_PYTHON) {
    $pyCmd = Get-Command python -ErrorAction SilentlyContinue
    if ($pyCmd) { $env:XC2_PYTHON = $pyCmd.Source }
}

function Clear-RouteEnv {
    foreach ($k in 'XC2_ROUTE', 'XC2_ROUTE_OUT', 'XC2_ROUTE_DELAY', 'XC2_PRESS') {
        Remove-Item ("Env:\" + $k) -ErrorAction SilentlyContinue
    }
}

function Set-Pairs([string[]]$pairs) {
    foreach ($p in $pairs) {
        if (-not $p) { continue }
        $i = $p.IndexOf('=')
        if ($i -lt 1) { throw "not KEY=VALUE: $p" }
        Set-Item ("Env:\" + $p.Substring(0, $i)) $p.Substring($i + 1)
    }
}

function Run-Once([string]$tag) {
    $out = & pwsh -NoProfile -File $playScript -Profile $Profile -Seconds $Seconds 2>&1
    $line = $out | Where-Object { $_ -match '^log: ' } | Select-Object -Last 1
    if (-not $line) { throw "run $tag produced no log line" }
    $log = ($line -replace '^log: ', '').Trim()
    $w = @(Select-String -Path $log -Pattern 'replay ([0-9.]+) ms/frame .*?\| (\d+) draws/frame' -Encoding utf8 |
        ForEach-Object { [pscustomobject]@{ ms = [double]$_.Matches[0].Groups[1].Value; draws = [int]$_.Matches[0].Groups[2].Value } })
    [pscustomobject]@{ tag = $tag; log = $log; windows = $w }
}

function Median([double[]]$v) {
    if (-not $v -or $v.Count -eq 0) { return 0 }
    $s = $v | Sort-Object
    $n = $s.Count
    if ($n % 2) { return $s[[int](($n - 1) / 2)] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2
}

$work = 'E:\XC2\dev\work\routes'
if (-not (Test-Path $work)) { New-Item -ItemType Directory -Path $work -Force | Out-Null }

if ($Record) {
    Clear-RouteEnv
    Set-Item Env:\XC2_ROUTE_OUT $Record
    # -Press drives the recording with a script instead of a person: the presses go through the same poll() the game
    # sees, so they land in the route file like any other input.
    if ($Press) { Set-Item Env:\XC2_PRESS $Press }
    Write-Output "recording to $Record for $Seconds s - play now"
    & pwsh -NoProfile -File $playScript -Profile $Profile -Seconds $Seconds
    if (Test-Path -LiteralPath $Record) {
        $n = (Get-Content -LiteralPath $Record | Where-Object { $_ -notmatch '^#' -and $_.Trim() }).Count
        Write-Output "recorded $n events"
    } else {
        Write-Output "no route file: nothing changed the input (a run that only sat on a screen records nothing)"
    }
    return
}

if ($Play) {
    Clear-RouteEnv
    Set-Item Env:\XC2_ROUTE $Play
    & pwsh -NoProfile -File $playScript -Profile $Profile -Seconds $Seconds
    return
}

if (-not $Ab) {
    throw 'usage: route.ps1 -Record <file> | -Play <file> | -Ab <file> [-A k=v ...] [-B k=v ...]'
}
if (-not (Test-Path -LiteralPath $Ab)) { throw "no route at $Ab" }

Set-Item Env:\NXRT_NVN_REPLAY_STATS '60'
$runs = @()
foreach ($i in 1..$Repeat) {
    Clear-RouteEnv
    Set-Item Env:\XC2_ROUTE $Ab
    Set-Pairs $A
    Set-Item Env:\NXRT_NVN_REPLAY_STATS '60'
    $runs += Run-Once ("A" + $i)
    Clear-RouteEnv
    Set-Item Env:\XC2_ROUTE $Ab
    Set-Pairs $B
    Remove-Item Env:\NXRT_NVN_REPLAY_STATS -ErrorAction SilentlyContinue
    Set-Item Env:\NXRT_NVN_REPLAY_STATS '60'
    $runs += Run-Once ("B" + $i)
}

$as = $runs | Where-Object { $_.tag -like 'A*' }
$bs = $runs | Where-Object { $_.tag -like 'B*' }
$pairs = @()
for ($i = 0; $i -lt $Repeat; $i++) {
    $wa = $as[$i].windows
    $wb = $bs[$i].windows
    $n = [Math]::Min($wa.Count, $wb.Count)
    for ($k = 0; $k -lt $n; $k++) {
        $tol = [Math]::Max(20, $wa[$k].draws * $Tolerance)
        if ([Math]::Abs($wa[$k].draws - $wb[$k].draws) -le $tol) {
            $pairs += [pscustomobject]@{ pair = $i; window = $k; drawsA = $wa[$k].draws; drawsB = $wb[$k].draws; msA = $wa[$k].ms; msB = $wb[$k].ms }
        }
    }
}
Write-Output ''
$na = 0; foreach ($r in $as) { $na += $r.windows.Count }
$nb = 0; foreach ($r in $bs) { $nb += $r.windows.Count }
Write-Output ("route {0}: {1} paired windows (of {2} and {3} recorded)" -f (Split-Path $Ab -Leaf), $pairs.Count, $na, $nb)
if ($pairs.Count -lt $MinWindows) {
    Write-Output ("too few comparable windows ({0} < {1}): the route does not reach the same scene twice - record a longer one" -f $pairs.Count, $MinWindows)
}
$ma = Median ([double[]]@($pairs | ForEach-Object { $_.msA }))
$mb = Median ([double[]]@($pairs | ForEach-Object { $_.msB }))
$da = Median ([double[]]@($pairs | ForEach-Object { [double]$_.drawsA }))
Write-Output ("A: replay {0:N2} ms/frame (median, {1:N0} draws/frame)" -f $ma, $da)
$db = Median ([double[]]@($pairs | ForEach-Object { [double]$_.drawsB }))
Write-Output ("B: replay {0:N2} ms/frame (median, {1:N0} draws/frame)" -f $mb, $db)
if ($ma -gt 0) { Write-Output ("B - A: {0:N2} ms ({1:N1} %)" -f ($mb - $ma), (100.0 * ($mb - $ma) / $ma)) }
$csv = [IO.Path]::ChangeExtension($Ab, '.ab.csv')
$pairs | Export-Csv -LiteralPath $csv -NoTypeInformation -Encoding UTF8
Write-Output ("windows: {0}" -f $csv)

<#
.SYNOPSIS
  The flythrough measurement matrix: one scene and one camera path, flown at every resolution
  asked for with occlusion culling on and off (docs/plan/09-testing-profiling.md §9.4,
  docs/experiments/flythrough-desert-overlook.md).

.DESCRIPTION
  tools/flythrough.ps1 [-Preset msvc-release] [-Set substitute|overlay]
                       [-Resolutions 1920x1080,2560x1440,3840x2160,11520x2160]
                       [-Occlusion on,off] [-Raster hw] [-Shadows off|rt|csm|auto] [-Repeat 3] [-Warmup 240]
                       [-Frames 0] [-PageBudgetPct 0] [-Census] [-Verify] [-Markers]
                       [-Out <dir>] [-Overlay <manifest>] [-WaitQuiet 600] [-RequireQuiet]
                       [-Exe <engine-view>] [-Scene <scene.json>] [-CameraPath <path.json>]

  Runs `engine-view --benchmark` once per (resolution, occlusion) pair and keeps each run's
  per-frame `.jsonl` in -Out, then writes `summary.json` (every run's summary line) and
  `summary.md` (one table: the frame's median, p95 and p99 from the GPU timers, the passes'
  medians, the visible pairs, the wall time, and the machine state each run recorded). A width of
  11520 is drawn as a three-monitor surround (`--views surround3`), which is what that resolution
  is on the machine it describes (plan 04 §4.1, experiment E9).

  -Set overlay adds `--overlay` with the owner's landmark manifest (-Overlay, default
  D:/workspace/game_engine_local/scenes/desert-overlook/landmarks.json); the scene falls back to
  its committed substitutes for anything the manifest does not name, and every run's summary says
  which mesh came from where.

  -Markers instead reads the path's marker frames the way the scene's README reports them: a
  `--census-pixels` run at 640x360 with occlusion culling on and one with it off, and one line per
  marker with each landmark's pixels and its visible pairs on/off.

  Measure under the machine-wide GPU lock, never beside another agent's GPU work:

    tools/gpu-lock.ps1 run -Purpose "flythrough" -Exec "pwsh tools/flythrough.ps1 -Set substitute"

  `gpu-lock.ps1 run` marks the lock as held for everything it starts, so engine-view's own
  machine-state check sees the lock as its own (docs/subsystems/bench.md, "The GPU lock").
  -WaitQuiet (seconds, default 600) waits for a quiet machine before each run and then runs either
  way; -RequireQuiet refuses to measure on a busy one (engine-view exits 4 and the run is marked).

  Exit code: 0 when every run succeeded, 1 otherwise.
#>
[CmdletBinding()]
param(
  [string]$Preset = 'msvc-release',
  [ValidateSet('substitute', 'overlay')] [string]$Set = 'substitute',
  [string[]]$Resolutions = @('1920x1080', '2560x1440', '3840x2160', '11520x2160'),
  [string[]]$Occlusion = @('on', 'off'),
  [string]$Raster = 'hw',
  [string]$Shadows = 'off',
  [int]$Repeat = 3,
  [int]$Warmup = 240,
  [int]$Frames = 0,
  [int]$PageBudgetPct = 0,
  [switch]$Census,
  [switch]$Verify,
  [switch]$Markers,
  [string]$Out = '',
  [string]$Overlay = 'D:/workspace/game_engine_local/scenes/desert-overlook/landmarks.json',
  [int]$WaitQuiet = 600,
  [switch]$RequireQuiet,
  [string]$Exe = '',
  [string]$Scene = '',
  [string]$CameraPath = '',
  [string[]]$Extra = @()
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
if (-not $Exe) {
  $Exe = Join-Path $Root ("build/$Preset/bin/engine-view" + $(if ($IsWin) { '.exe' } else { '' }))
}
if (-not (Test-Path $Exe)) { throw "flythrough: no engine-view at $Exe; build the preset first" }
if (-not $Scene) { $Scene = Join-Path $Root 'content/test-scenes/desert-overlook/scene.json' }
if (-not $CameraPath) { $CameraPath = Join-Path $Root 'content/test-scenes/desert-overlook/camera-path.json' }
if (-not $Out) {
  $Out = Join-Path $Root ("build/$Preset/flythrough/" + (Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Set")
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$common = @('--scene', $Scene, '--camera-path', $CameraPath, '--raster', $Raster)
# engine-view takes off, rt or csm; auto is what no flag means (rt where the device can trace, the
# cascaded shadow maps where it cannot).
if ($Shadows -ne 'auto') { $common += @('--shadows', $Shadows) }
if ($Set -eq 'overlay') {
  if (-not (Test-Path $Overlay)) { throw "flythrough: no overlay manifest at $Overlay" }
  $common += @('--overlay', $Overlay)
}
if ($PageBudgetPct -gt 0) { $common += @('--page-budget-pct', "$PageBudgetPct") }
$common += $Extra

# One engine-view run; the last line of its stdout is the summary.
function Invoke-View([string[]]$Arguments) {
  $output = & $Exe @Arguments 2>&1
  $code = $LASTEXITCODE
  $lines = @($output | ForEach-Object { "$_" })
  $summary = $null
  for ($i = $lines.Count - 1; $i -ge 0; $i--) {
    if ($lines[$i].StartsWith('{')) { $summary = $lines[$i] | ConvertFrom-Json; break }
  }
  $warnings = @($lines | Where-Object { $_ -like 'WARNING:*' })
  return [pscustomobject]@{ Code = $code; Summary = $summary; Warnings = $warnings; Output = $lines }
}

function Format-Ms([double]$v) { '{0:N3}' -f $v }

# The worst "others" CPU and GPU utilization either machine-state sample saw: what a reader needs
# beside every row to know whether it is a cost or an upper bound.
function Get-State($summary) {
  $ms = $summary.machine_state
  if ($null -eq $ms) { return 'not recorded' }
  $cpu = @($ms.start.cpu_others_pct, $ms.end.cpu_others_pct) | Where-Object { $null -ne $_ } | Measure-Object -Maximum
  $gpu = @($ms.start.gpu_util_pct) | Where-Object { $null -ne $_ } | Measure-Object -Maximum
  $quiet = if ($summary.quiet) { 'quiet' } else { 'WARNING' }
  return '{0}: others {1:N1}% CPU, GPU {2}% before' -f $quiet, $cpu.Maximum, $gpu.Maximum
}

if ($Markers) {
  $names = @('skyscraper', 'office-tower', 'cathedral', 'mosque', 'wreck', 'palms')
  $runs = @{}
  foreach ($o in @('on', 'off')) {
    $jsonl = Join-Path $Out "markers-$o.jsonl"
    $argv = $common + @('--benchmark', $jsonl, '--repeat', '1', '--warmup', '30', '--census-pixels',
                        '--width', '640', '--height', '360')
    if ($o -eq 'off') { $argv += '--no-occlusion' }
    $r = Invoke-View $argv
    if ($r.Code -ne 0) { $r.Output | Select-Object -Last 5; throw "flythrough: the $o census failed ($($r.Code))" }
    # Not $frames: PowerShell names are case-insensitive, and that one is the [int] -Frames parameter.
    $byFrame = @{}
    foreach ($l in Get-Content $jsonl) {
      $j = $l | ConvertFrom-Json
      if ($null -eq $j.format) { $byFrame[[int]$j.frame] = $j }
    }
    $runs[$o] = $byFrame
  }
  $path = Get-Content $CameraPath -Raw | ConvertFrom-Json
  foreach ($m in $path.markers) {
    $on = $runs['on'][[int]$m.frame]
    $off = $runs['off'][[int]$m.frame]
    $cells = for ($k = 0; $k -lt $names.Count; $k++) {
      $px = [int]$on.pixels[$k]; $a = [int]$on.meshes[$k]; $b = [int]$off.meshes[$k]
      $state = if ($b -eq 0) { 'out of frame' } elseif ($px -eq 0) { 'hidden' } else { "$px px" }
      '{0} {1} ({2}/{3})' -f $names[$k], $state, $a, $b
    }
    '{0,5} {1,-16} pairs {2}/{3} | {4}' -f $m.frame, $m.name, $on.visible_pairs, $off.visible_pairs, ($cells -join '; ')
  }
  Write-Host "results: $Out"
  exit 0
}

$results = @()
$failed = 0
# A list given on a `pwsh -File` command line (as `gpu-lock.ps1 run -Exec` passes it) arrives as one
# string, so commas split it here as well as in PowerShell.
$Resolutions = @($Resolutions | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$Occlusion = @($Occlusion | ForEach-Object { $_ -split "," } | Where-Object { $_ })
foreach ($o in $Occlusion) { if ($o -notin @("on", "off")) { throw "flythrough: -Occlusion takes on and off, not '$o'" } }
foreach ($res in $Resolutions) {
  if ($res -notmatch '^(\d+)x(\d+)$') { throw "flythrough: '$res' is not WIDTHxHEIGHT" }
  $w = [int]$Matches[1]; $h = [int]$Matches[2]
  foreach ($o in $Occlusion) {
    $tag = "$Set-$res-occlusion-$o"
    $jsonl = Join-Path $Out "$tag.jsonl"
    $argv = $common + @('--benchmark', $jsonl, '--repeat', "$Repeat", '--warmup', "$Warmup",
                        '--width', "$w", '--height', "$h")
    if ($w -eq 11520) { $argv += @('--views', 'surround3') }
    if ($o -eq 'off') { $argv += '--no-occlusion' }
    if ($Frames -gt 0) { $argv += @('--frames', "$Frames") }
    if ($Census) { $argv += '--census' }
    if ($Verify) { $argv += '--verify-occlusion' }
    if ($WaitQuiet -gt 0) { $argv += @('--wait-quiet', "$WaitQuiet") }
    if ($RequireQuiet) { $argv += '--require-quiet' }
    Write-Host "== $tag" -ForegroundColor Cyan
    $r = Invoke-View $argv
    if ($r.Code -ne 0 -or $null -eq $r.Summary) {
      $failed++
      Write-Warning "flythrough: $tag exited $($r.Code)"
      $r.Output | Select-Object -Last 5 | ForEach-Object { Write-Warning $_ }
      continue
    }
    $r.Warnings | ForEach-Object { Write-Host $_ -ForegroundColor Yellow }
    $results += [pscustomobject]@{ Tag = $tag; Resolution = $res; Occlusion = $o; Summary = $r.Summary; Warnings = $r.Warnings }
  }
}

$results | ForEach-Object { $_.Summary } | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $Out 'summary.json')
$md = @(
  "# Flythrough: $Set, raster $Raster, shadows $Shadows",
  '',
  "engine-view $Exe; scene $Scene; path $CameraPath. Milliseconds are GPU time from gfx::GpuTimer, per frame: each frame's median over the repeats, then the median, p95 and p99 over the path.",
  '',
  '| resolution | views | occlusion | raster | shadows | frames x repeats | frame median | p95 | p99 | cull | raster pass | hiz | resolve | rt chain | shadow maps | visible pairs (median) | shadow pairs (median) | wall ms/frame | deterministic | machine state |',
  '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
)
foreach ($r in $results) {
  $s = $r.Summary
  $raster = [double]$s.gpu_ms.hw.median + [double]$s.gpu_ms.sw.median
  # The shadow maps' column is every pass that draws them; an older engine-view's summary has none.
  $shadow = if ($null -ne $s.gpu_ms.shadow) { Format-Ms $s.gpu_ms.shadow.median } else { '-' }
  $shadowPairs = if ($null -ne $s.shadow_pairs) { '{0:N0}' -f [double]$s.shadow_pairs.median } else { '-' }
  $md += '| {0} | {1} | {2} | {3} | {4} | {5} x {6} | {7} | {8} | {9} | {10} | {11} | {12} | {13} | {14} | {15} | {16:N0} | {17} | {18:N3} | {19} | {20} |' -f `
    $r.Resolution, $s.views, $r.Occlusion, $s.raster, $s.shadows, $s.frames, $s.repeats,
    (Format-Ms $s.gpu_ms.total.median), (Format-Ms $s.gpu_ms.total.p95), (Format-Ms $s.gpu_ms.total.p99),
    (Format-Ms $s.gpu_ms.cull.median), (Format-Ms $raster), (Format-Ms $s.gpu_ms.hiz.median),
    (Format-Ms $s.gpu_ms.resolve.median), (Format-Ms $s.gpu_ms.rt.median), $shadow,
    $s.visible_pairs.median, $shadowPairs, $s.wall_ms_per_frame, $s.deterministic, (Get-State $s)
}
if ($results.Count -gt 0) {
  $first = $results[0].Summary
  $md += ''
  $md += "Scene ``$($first.scene)`` $($first.scene_hash), path ``$($first.path)`` $($first.path_hash), identity $($first.identity); $($first.instances) instances, $($first.pairs) pairs, $($first.clusters) clusters."
  $md += ''
  $md += '| mesh | source | hash | clusters | triangles |'
  $md += '|---|---|---|---|---|'
  foreach ($m in $first.meshes) { $md += "| $($m.name) | $($m.source) | $($m.hash) | $($m.clusters) | $($m.triangles) |" }
}
$md -join "`n" | Set-Content (Join-Path $Out 'summary.md')
Get-Content (Join-Path $Out 'summary.md')
Write-Host "results: $Out"
exit $(if ($failed -gt 0) { 1 } else { 0 })

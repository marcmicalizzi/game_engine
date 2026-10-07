#!/usr/bin/env pwsh
<#
.SYNOPSIS
  The frame-budget gate (ADR-0018): flies the budget file's path scenes, compares each run's GPU
  milliseconds per pass with its budget, and fails naming the pass that went over.

.DESCRIPTION
  tools/frame-budget.ps1 [-Preset msvc-release] [-Budget content/budgets/frame-budgets.json]
                         [-Run <id>,...] [-WaitQuiet 600] [-Out <dir>] [-Exe <engine-view>]
                         [-FromSummaries <dir>] [-AnyDevice]
                         [-Rebaseline -Reason "<why>" [-By <who>] [-AllowBusy]]

  For every run of the budget file (or those -Run names) it takes the machine-wide GPU lock, runs
  `engine-view --offscreen --benchmark` with the run's scene, path, resolution and flags and
  `--wait-quiet` (the lock first, then the wait for a quiet machine while holding it; bench.md,
  "The GPU lock"), releases the lock, and compares the summary's `gpu_ms` medians and p99s (and
  `cpu_ms`, where a run budgets it) with the budget plus the file's tolerance
  (tools/lib/FrameBudget.psm1 has the rule). The per-frame `.jsonl`, `report.md` and
  `report.json` go in -Out (default build/<preset>/frame-budget/<time>/).

  The budgets hold on one machine, the file's `machine.device`: elsewhere the script measures
  nothing and exits 3 (nvidia-smi names the adapter before anything runs; the summary's `device`
  is checked after each run too). -AnyDevice compares anyway, to read a number, never to gate.

  -FromSummaries <dir> compares the `<id>.jsonl` files a previous run left in <dir> instead of
  flying anything: re-reading a run against a changed budget, or a tolerance, costs no GPU.

  -Rebaseline replaces each run's budget with what this run measured and records who (-By, default
  the git user), when, why (-Reason, required), the commit, the adapter and the machine's state in
  its `baseline`; the old numbers stay in git's history of the file. It refuses a run measured on a
  busy machine unless -AllowBusy, because a busy number is an upper bound and would loosen the
  budget. Commit the file with the change that moved the numbers, and say why in the message.

  Exit codes: 0 every run within budget (or re-baselined); 1 a run over budget on a quiet machine,
  a run that failed, or a summary missing a budgeted pass; 3 not the budget's machine; 4 the only
  runs over budget ran on a busy machine (inconclusive: run again when it is quiet).

  Where it runs and what it costs: docs/ci/what-to-run.md, "Frame budgets".
#>
[CmdletBinding()]
param(
  [string]$Preset = 'msvc-release',
  [string]$Budget = '',
  [string[]]$Run = @(),
  [int]$WaitQuiet = 600,
  [string]$Out = '',
  [string]$Exe = '',
  [string]$FromSummaries = '',
  [switch]$AnyDevice,
  [switch]$Rebaseline,
  [string]$Reason = '',
  [string]$By = '',
  [switch]$AllowBusy
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
Import-Module (Join-Path $PSScriptRoot 'lib/FrameBudget.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'lib/MachineLock.psm1') -Force

if (-not $Budget) { $Budget = Join-Path $Root 'content/budgets/frame-budgets.json' }
$plan = Read-FrameBudget -Path $Budget
if ($Rebaseline -and -not $Reason.Trim()) { throw 'frame-budget: -Rebaseline needs -Reason: the budget file records why its numbers moved' }
$Run = @($Run | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$runs = @($plan.runs | Where-Object { $Run.Count -eq 0 -or $_.id -in $Run })
foreach ($id in $Run) { if (-not ($plan.runs | Where-Object { $_.id -eq $id })) { throw "frame-budget: no run '$id' in $Budget" } }

if (-not $Out) {
  $Out = if ($FromSummaries) { $FromSummaries } else { Join-Path $Root ("build/$Preset/frame-budget/" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
}
New-Item -ItemType Directory -Force -Path $Out | Out-Null

if (-not $FromSummaries) {
  if (-not $Exe) { $Exe = Join-Path $Root ("build/$Preset/bin/engine-view" + $(if ($IsWin) { '.exe' } else { '' })) }
  if (-not (Test-Path $Exe)) { throw "frame-budget: no engine-view at $Exe; build the preset first (tools/dev.ps1 build -Preset $Preset)" }
  # The adapter, before anything is flown: a machine the budgets are not for measures nothing.
  $smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
  if ($smi -and -not $AnyDevice) {
    $names = @(& $smi.Source --query-gpu=name --format=csv,noheader 2>$null | ForEach-Object { "$_".Trim() } | Where-Object { $_ })
    if ($names.Count -gt 0 -and -not ($names | Where-Object { $_ -eq $plan.machine.device })) {
      Write-Host "frame-budget: skipped: the budgets are $($plan.machine.device)'s and this machine has $($names -join ', ')" -ForegroundColor Yellow
      exit 3
    }
  }
}

function Get-Measured($summary, [string]$stat) { if ($summary.gpu_ms.total) { [double]$summary.gpu_ms.total.$stat } else { 0.0 } }

$results = @()
$summaries = @{}
foreach ($r in $runs) {
  $jsonl = Join-Path $Out "$($r.id).jsonl"
  if (-not $FromSummaries) {
    Write-Host "== $($r.id)" -ForegroundColor Cyan
    $argv = Get-FrameBudgetArgs -Run $r -Root $Root -Jsonl $jsonl -WaitQuiet $WaitQuiet
    # One lock per run, as `tools/dev.ps1 bench -GpuLock` takes one per executable: another agent
    # waiting for the card gets it between two runs rather than after all of them.
    $session = Open-MachineLockSession -Kind gpu -Purpose "frame budget $($r.id)"
    $started = Get-Date
    try {
      $output = @(& $Exe @argv 2>&1 | ForEach-Object { "$_" })
      $code = $LASTEXITCODE
    } finally {
      Close-MachineLockSession -Session $session
    }
    $output | Set-Content (Join-Path $Out "$($r.id).log")
    Write-Host ('   {0:N0} s, exit {1}' -f ((Get-Date) - $started).TotalSeconds, $code)
    if ($code -eq 3) {
      $results += [pscustomobject]@{ Id = $r.id; Verdict = 'device'; Device = ''; Quiet = $false; Rows = @(); Missing = @(); MachineState = 'engine-view cannot draw here' }
      continue
    }
    if ($code -ne 0) {
      $output | Select-Object -Last 5 | ForEach-Object { Write-Warning $_ }
      $results += [pscustomobject]@{ Id = $r.id; Verdict = 'failed'; Device = ''; Quiet = $false; Rows = @(); Missing = @(); MachineState = "engine-view exited $code" }
      continue
    }
  }
  if (-not (Test-Path $jsonl)) {
    $results += [pscustomobject]@{ Id = $r.id; Verdict = 'failed'; Device = ''; Quiet = $false; Rows = @(); Missing = @(); MachineState = "no $jsonl" }
    continue
  }
  $summary = Get-Content $jsonl | Select-Object -Last 1 | ConvertFrom-Json
  $summaries[$r.id] = $summary
  $result = Test-FrameBudgetRun -Budget $plan -Run $r -Summary $summary
  if ($AnyDevice -and $result.Verdict -eq 'device') {
    $saved = $plan.machine.device
    $plan.machine.device = $result.Device
    $result = Test-FrameBudgetRun -Budget $plan -Run $r -Summary $summary
    $plan.machine.device = $saved
  }
  $results += $result
  $overRows = @($result.Rows | Where-Object { $_.Over })
  $line = '   {0}: {1}; total {2:N3} / {3:N3} ms (median / p99) against {4:N3} / {5:N3}; {6}' -f $r.id, $result.Verdict,
    (Get-Measured $summary 'median'), (Get-Measured $summary 'p99'), [double]$r.gpu_ms.total.median, [double]$r.gpu_ms.total.p99, $result.MachineState
  Write-Host $line -ForegroundColor $(if ($result.Verdict -eq 'pass') { 'Green' } elseif ($result.Verdict -eq 'busy') { 'Yellow' } else { 'Red' })
  foreach ($row in $overRows) {
    Write-Host ('     over: {0} {1} {2:N3} ms, budget {3:N3}, limit {4:N3}' -f $row.Line, $row.Stat, $row.Measured, $row.Budget, $row.Limit) -ForegroundColor Red
  }
}

$report =@("# Frame budgets: $(Split-Path -Leaf $Budget) on $($plan.machine.device)", '',
            "engine-view $Exe, $(Get-Date -Format 'yyyy-MM-dd HH:mm'). Tolerance: GPU median +$($plan.tolerance.gpu.median.pct)% +$($plan.tolerance.gpu.median.ms) ms, p99 +$($plan.tolerance.gpu.p99.pct)% +$($plan.tolerance.gpu.p99.ms) ms; CPU median +$($plan.tolerance.cpu.median.pct)% +$($plan.tolerance.cpu.median.ms) ms, p99 +$($plan.tolerance.cpu.p99.pct)% +$($plan.tolerance.cpu.p99.ms) ms.", '')
$report += Format-FrameBudgetReport -Results $results -Budget $plan
$report -join "`n" | Set-Content (Join-Path $Out 'report.md')
$results | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $Out 'report.json')

if ($Rebaseline) {
  if (-not $By) {
    $By = "$(& git -C $Root config user.name 2>$null)".Trim()
    if (-not $By) { $By = Get-MachineLockOwner }
  }
  $commit = "$(& git -C $Root rev-parse --short HEAD 2>$null)".Trim()
  if ("$(& git -C $Root status --porcelain 2>$null)".Trim()) { $commit += ' (with uncommitted changes)' }
  $changed = 0
  foreach ($r in $runs) {
    $res = $results | Where-Object { $_.Id -eq $r.id } | Select-Object -First 1
    if (-not $summaries.ContainsKey($r.id) -or $res.Verdict -in @('failed', 'device', 'malformed')) {
      Write-Warning "frame-budget: $($r.id) not re-baselined: $($res.Verdict)"
      continue
    }
    if (-not $res.Quiet -and -not $AllowBusy) {
      Write-Warning "frame-budget: $($r.id) not re-baselined: the machine was busy ($($res.MachineState)); -AllowBusy takes it anyway"
      continue
    }
    Set-FrameBudgetBaseline -Run $r -Summary $summaries[$r.id] -By $By -Reason $Reason -Commit $commit | Out-Null
    $changed++
  }
  $plan | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Budget -Encoding utf8NoBOM
  Write-Host "frame-budget: re-baselined $changed of $($runs.Count) runs in $Budget; commit it with the reason" -ForegroundColor Cyan
  Write-Host "results: $Out"
  exit $(if ($changed -eq $runs.Count) { 0 } else { 1 })
}

$code = Get-FrameBudgetExitCode -Results $results
$word = @{ 0 = 'within budget'; 1 = 'OVER BUDGET or failed'; 3 = 'not the budget''s machine: skipped'; 4 = 'over budget on a busy machine: inconclusive, run again when it is quiet' }[$code]
Write-Host "frame-budget: $word ($(@($results | Where-Object { $_.Verdict -eq 'pass' }).Count) of $($results.Count) runs pass)"
Write-Host "results: $Out"
exit $code

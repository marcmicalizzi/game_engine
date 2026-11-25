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

  A run holds a budget per Windows session state, `budgets.locked` and `budgets.unlocked` (T161):
  a locked session idles the card's clocks, so a flight is compared only with the set of the state
  its summary's `machine_state` recorded, and with nothing when the run holds no set for that state
  or the state is unknown or changed during the flight (bench.md, "Frame budgets").

  -Rebaseline replaces each run's budget for the session state it was flown in with what this run
  measured and records who (-By, default the git user), when, why (-Reason, required), the commit,
  the adapter, the session state and the machine's state in its `baseline`; the other state's set is
  left alone, and the old numbers stay in git's history of the file. It refuses a run measured on a
  busy machine unless -AllowBusy, because a busy number is an upper bound and would loosen the
  budget, and a run whose session state is unknown or changed. Commit the file with the change
  that moved the numbers, and say why in the message.

  Exit codes: 0 every run within budget (or re-baselined); 1 a run over budget on a quiet machine,
  a run that failed, or a summary missing a budgeted pass; 3 not the budget's machine; 4 the only
  runs over budget ran on a busy machine (inconclusive: run again when it is quiet); 5 a summary
  that is not a native measurement (ADR-0064: its `reconstruction` block names a provider or an
  internal size below the output's, or its `gpu_ms` is null and its times are under
  `reconstructed`): refused before any number of it is read, never compared and never a baseline;
  6 a run compared with nothing because it holds no budget for its session state, or the state was
  unknown or changed during the flight (inconclusive: fly it in a state it holds, or take one).

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

# No material pack: a budget is a function of the tree, never of whether this checkout has fetched
# or generated one (T103; docs/subsystems/materials.md, "Where the hosts find it"). The variable is
# the one default lookup's switch, inherited by every engine-view below; each summary's
# `load.materials` says what the flight resolved, and one that drew with a pack fails. None of the
# budget scenes has a generated mesh that falls back to a pack, so the milliseconds do not move.
$env:ENGINE_DEFAULT_MATERIALS = '0'

$results = @()
$summaries = @{}
foreach ($r in $runs) {
  # A re-reading reads the records where they are and writes its report to -Out (T221: a kept
  # flight is judged again without writing into the directory that keeps it).
  $jsonl = Join-Path $(if ($FromSummaries) { $FromSummaries } else { $Out }) "$($r.id).jsonl"
  if (-not $FromSummaries) {
    Write-Host "== $($r.id)" -ForegroundColor Cyan
    # A run whose scene names files the tree does not commit says how to write them: `prepare` is
    # a program of the preset's bin/ and its arguments, run from the repository root before the
    # lock is taken (the static world's kit: `engine-content static-kit`, roadmap T130).
    if ($r.PSObject.Properties['prepare'] -and $r.prepare) {
      $tool = Join-Path (Split-Path $Exe) ("$(@($r.prepare)[0])" + $(if ($IsWin) { '.exe' } else { '' }))
      $prep = @(@($r.prepare) | Select-Object -Skip 1 | ForEach-Object { "$_" })
      Push-Location $Root
      try { $prepared = @(& $tool @prep 2>&1 | ForEach-Object { "$_" }); $pcode = $LASTEXITCODE } finally { Pop-Location }
      if ($pcode -ne 0) {
        $prepared | Select-Object -Last 5 | ForEach-Object { Write-Warning $_ }
        $results += [pscustomobject]@{ Id = $r.id; Verdict = 'failed'; Device = ''; Quiet = $false; Rows = @(); Missing = @(); MachineState = "prepare exited $pcode" }
        continue
      }
    }
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
  $pack = if ($summary.PSObject.Properties['load'] -and $summary.load -and $summary.load.PSObject.Properties['materials']) { "$($summary.load.materials)" } else { '' }
  if ($pack) {
    $results += [pscustomobject]@{ Id = $r.id; Verdict = 'failed'; Device = ''; Quiet = $false; Rows = @(); Missing = @(); MachineState = "drew with the material pack $pack; a budget draws with none" }
    continue
  }
  $summaries[$r.id] = $summary
  # The frame records beside the summary: the frame thread's median is judged by its fast mode
  # (T221; tools/lib/FrameBudget.psm1, Get-CpuModes).
  $frames = Read-FrameCpu -Path $jsonl
  $result = Test-FrameBudgetRun -Budget $plan -Run $r -Summary $summary -Frames $frames
  if ($AnyDevice -and $result.Verdict -eq 'device') {
    $saved = $plan.machine.device
    $plan.machine.device = $result.Device
    $result = Test-FrameBudgetRun -Budget $plan -Run $r -Summary $summary -Frames $frames
    $plan.machine.device = $saved
  }
  $results += $result
  if ($result.Verdict -eq 'reconstructed') {
    Write-Host "   $($r.id): refused: $($result.MachineState)" -ForegroundColor Red
    continue
  }
  if ($result.Verdict -in @('nobaseline', 'session', 'device')) {
    # Compared with nothing (T161): the measured total is said, and no limit beside it.
    Write-Host ('   {0}: {1}; total {2:N3} / {3:N3} ms (median / p99); {4}' -f $r.id, $result.Verdict,
      (Get-Measured $summary 'median'), (Get-Measured $summary 'p99'), $result.MachineState) -ForegroundColor Yellow
    continue
  }
  $set = Get-SessionBudget $r $result.Session
  $overRows = @($result.Rows | Where-Object { $_.Over })
  $line = '   {0}: {1}; total {2:N3} / {3:N3} ms (median / p99) against {4:N3} / {5:N3}, {6}; {7}' -f $r.id, $result.Verdict,
    (Get-Measured $summary 'median'), (Get-Measured $summary 'p99'), [double]$set.gpu_ms.total.median, [double]$set.gpu_ms.total.p99, $result.Compared, $result.MachineState
  Write-Host $line -ForegroundColor $(if ($result.Verdict -eq 'pass') { 'Green' } elseif ($result.Verdict -eq 'busy') { 'Yellow' } else { 'Red' })
  foreach ($row in $overRows) {
    $how = switch ($row.Judged) { 'fast' { ' (its fast mode)' } 'unsplit' { ' (the whole flight: modes not told apart, inconclusive)' } default { '' } }
    Write-Host ('     over: {0} {1}{5} {2:N3} ms, budget {3:N3}, limit {4:N3}' -f $row.Line, $row.Stat, $row.Measured, $row.Budget, $row.Limit, $how) -ForegroundColor Red
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
    if (-not $summaries.ContainsKey($r.id) -or $res.Verdict -in @('failed', 'device', 'malformed', 'reconstructed', 'session')) {
      Write-Warning "frame-budget: $($r.id) not re-baselined: $($res.Verdict)$(if ($res.Verdict -eq 'session') { " ($($res.MachineState))" })"
      continue
    }
    if (-not $res.Quiet -and -not $AllowBusy) {
      Write-Warning "frame-budget: $($r.id) not re-baselined: the machine was busy ($($res.MachineState)); -AllowBusy takes it anyway"
      continue
    }
    # The flight's own session state's set, and only that one: the other state's stays as it was.
    $recordsAt = Join-Path $(if ($FromSummaries) { $FromSummaries } else { $Out }) "$($r.id).jsonl"
    try {
      Set-FrameBudgetBaseline -Run $r -Summary $summaries[$r.id] -By $By -Reason $Reason -Commit $commit -Frames (Read-FrameCpu -Path $recordsAt) | Out-Null
    } catch {
      Write-Warning "frame-budget: $($r.id) not re-baselined: $($_.Exception.Message)"
      continue
    }
    Write-Host "   $($r.id): the $($res.Session) baseline taken; the run now holds $(Format-RunSessions $r)" -ForegroundColor Cyan
    $changed++
  }
  $plan | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Budget -Encoding utf8NoBOM
  Write-Host "frame-budget: re-baselined $changed of $($runs.Count) runs in $Budget; commit it with the reason" -ForegroundColor Cyan
  Write-Host "results: $Out"
  exit $(if ($changed -eq $runs.Count) { 0 } else { 1 })
}

$code = Get-FrameBudgetExitCode -Results $results
$word = @{ 0 = 'within budget'; 1 = 'OVER BUDGET or failed'; 3 = 'not the budget''s machine: skipped'; 4 = 'over budget on a busy machine: inconclusive, run again when it is quiet'
           5 = 'REFUSED: a summary is not a native measurement (ADR-0064); a budget is measured with reconstruction off at the output''s size'
           6 = 'INCONCLUSIVE: a run was compared with nothing, because it holds no budget for the session state it was flown in, or its state was unknown or changed during the flight (T161); fly it in a state it holds, or take that state''s baseline' }[$code]
Write-Host "frame-budget: $word ($(@($results | Where-Object { $_.Verdict -eq 'pass' }).Count) of $($results.Count) runs pass)"
Write-Host "results: $Out"
exit $code

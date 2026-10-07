<#
  Frame budgets (ADR-0018; docs/plan/11-performance-principles.md §11.1; docs/subsystems/bench.md,
  "Frame budgets"): the rules tools/frame-budget.ps1 applies, kept apart from the runs so that
  tools/frame-budget.Tests.ps1 can hold them with no GPU.

  A budget file (content/budgets/frame-budgets.json) names one machine and a set of runs — a path
  scene, its camera path, a resolution and the flags it is flown with — and for each run the GPU
  milliseconds per pass, at the median and the p99 of the path's frames, that the run is allowed,
  with where each number came from and who set it. A run's flythrough summary
  (`engine.scene.FlythroughSummary`, what engine-view --benchmark writes last) is compared line by
  line: a number is over when it passes `budget * (1 + pct / 100) + ms` for its statistic's
  tolerance. The relative part is for the passes that cost milliseconds, the absolute part for the
  ones that cost hundredths, where a timer's own jitter is the whole difference.

  A run that is over on a quiet machine fails the gate. A run that is over on a busy one says so
  and is inconclusive — its numbers are upper bounds (bench.md, "Measuring on a shared machine") —
  and a run within budget passes whatever the machine was doing, since an upper bound under the
  limit is under it.
#>

Set-StrictMode -Version Latest

$script:Format = 'engine.frame-budgets.v1'
$script:Stats = @('median', 'p99')

# The budget file, checked for the shape the rest of this module reads. Throws naming the problem.
function Read-FrameBudget {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [string]$Path)
  if (-not (Test-Path -LiteralPath $Path)) { throw "frame-budget: no budget file at $Path" }
  $budget = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
  Assert-FrameBudget $budget
  return $budget
}

function Assert-FrameBudget($budget) {
  # A missing field reads as null here, so the sentence below names it instead of strict mode's.
  Set-StrictMode -Off
  if ($budget.format -ne $script:Format) { throw "frame-budget: format is '$($budget.format)', not '$script:Format'" }
  if (-not $budget.machine -or -not $budget.machine.device) { throw 'frame-budget: no machine.device to hold the budgets on' }
  foreach ($kind in @('gpu', 'cpu')) {
    $t = $budget.tolerance.$kind
    if ($null -eq $t) { throw "frame-budget: no tolerance.$kind" }
    foreach ($s in $script:Stats) {
      if ($null -eq $t.$s -or $null -eq $t.$s.pct -or $null -eq $t.$s.ms) { throw "frame-budget: tolerance.$kind.$s needs pct and ms" }
    }
  }
  $seen = @{}
  foreach ($run in @($budget.runs)) {
    if (-not $run.id) { throw 'frame-budget: a run without an id' }
    if ($seen.ContainsKey($run.id)) { throw "frame-budget: run id '$($run.id)' appears twice" }
    $seen[$run.id] = $true
    foreach ($field in @('scene', 'path', 'width', 'height')) {
      if ($null -eq $run.$field) { throw "frame-budget: run '$($run.id)' has no $field" }
    }
    if ($null -eq $run.gpu_ms -or $null -eq $run.gpu_ms.total) { throw "frame-budget: run '$($run.id)' budgets no gpu_ms.total" }
    foreach ($line in (Get-BudgetLines $run)) {
      foreach ($s in $script:Stats) {
        if ($null -eq $line.Value.$s) { throw "frame-budget: run '$($run.id)' $($line.Name) has no $s" }
      }
    }
    if ($null -eq $run.baseline -or -not $run.baseline.by -or -not $run.baseline.reason -or -not $run.baseline.date) {
      throw "frame-budget: run '$($run.id)' does not say who set its budget, when and why (baseline.by, .date, .reason)"
    }
  }
}

# The budgeted lines of a run: every pass under gpu_ms, and cpu_ms when it has one.
function Get-BudgetLines($run) {
  $lines = @()
  foreach ($p in $run.gpu_ms.PSObject.Properties) {
    $lines += [pscustomobject]@{ Kind = 'gpu'; Name = "gpu_ms.$($p.Name)"; Pass = $p.Name; Value = $p.Value }
  }
  if ($null -ne $run.PSObject.Properties['cpu_ms'] -and $null -ne $run.cpu_ms) {
    $lines += [pscustomobject]@{ Kind = 'cpu'; Name = 'cpu_ms'; Pass = ''; Value = $run.cpu_ms }
  }
  return $lines
}

# engine-view's arguments for a run, after the executable: the run's scene and path from the
# repository root, offscreen, its resolution and views, its own flags, and the benchmark's.
function Get-FrameBudgetArgs {
  [CmdletBinding()]
  param([Parameter(Mandatory)] $Run, [Parameter(Mandatory)] [string]$Root,
        [Parameter(Mandatory)] [string]$Jsonl, [int]$WaitQuiet = 0)
  $argv = @('--scene', [IO.Path]::Combine($Root, $Run.scene), '--camera-path', [IO.Path]::Combine($Root, $Run.path),
            '--offscreen', '--benchmark', $Jsonl,
            '--width', "$($Run.width)", '--height', "$($Run.height)")
  if ($Run.PSObject.Properties['views'] -and $Run.views -and $Run.views -ne 'single') { $argv += @('--views', "$($Run.views)") }
  $repeat = if ($Run.PSObject.Properties['repeat'] -and $Run.repeat) { [int]$Run.repeat } else { 1 }
  $warmup = if ($Run.PSObject.Properties['warmup'] -and $null -ne $Run.warmup) { [int]$Run.warmup } else { 60 }
  $argv += @('--repeat', "$repeat", '--warmup', "$warmup")
  if ($Run.PSObject.Properties['args']) { $argv += @($Run.args | ForEach-Object { "$_" }) }
  if ($WaitQuiet -gt 0) { $argv += @('--wait-quiet', "$WaitQuiet") }
  return $argv
}

function Get-FrameBudgetLimit([double]$Budget, [double]$Pct, [double]$Ms) {
  return $Budget * (1.0 + $Pct / 100.0) + $Ms
}

# The summary's number for a budgeted line, or $null when the summary has no such line.
function Get-MeasuredValue($summary, $line, [string]$stat) {
  $parent = if ($line.Kind -eq 'cpu') { $summary } else { $summary.PSObject.Properties['gpu_ms'].Value }
  $name = if ($line.Kind -eq 'cpu') { 'cpu_ms' } else { $line.Pass }
  if ($null -eq $parent) { return $null }
  $node = $parent.PSObject.Properties[$name]
  if ($null -eq $node -or $null -eq $node.Value) { return $null }
  $v = $node.Value.PSObject.Properties[$stat]
  if ($null -eq $v -or $null -eq $v.Value) { return $null }
  return [double]$v.Value
}

<#
  One run's summary against its budget. Returns the run's verdict and a row per budgeted number:
    Verdict  'pass'       every number within its limit
             'over'       a number over its limit on a quiet machine: the gate fails
             'busy'       a number over its limit on a busy machine: inconclusive
             'device'     the summary was drawn on another adapter than the budget's: skipped
             'malformed'  the summary lacks a budgeted line: the gate fails
#>
function Test-FrameBudgetRun {
  [CmdletBinding()]
  param([Parameter(Mandatory)] $Budget, [Parameter(Mandatory)] $Run, [Parameter(Mandatory)] $Summary)
  $rows = @()
  $over = $false
  $missing = @()
  foreach ($line in (Get-BudgetLines $Run)) {
    foreach ($s in $script:Stats) {
      $t = $Budget.tolerance.($line.Kind).$s
      $b = [double]$line.Value.$s
      $limit = Get-FrameBudgetLimit $b ([double]$t.pct) ([double]$t.ms)
      $m = Get-MeasuredValue $Summary $line $s
      if ($null -eq $m) { $missing += "$($line.Name).$s"; continue }
      $isOver = $m -gt $limit
      $over = $over -or $isOver
      $rows += [pscustomobject]@{ Line = $line.Name; Stat = $s; Budget = $b; Measured = $m; Limit = $limit; Over = $isOver }
    }
  }
  $device = if ($Summary.PSObject.Properties['device']) { "$($Summary.device)" } else { '' }
  $quiet = [bool]($Summary.PSObject.Properties['quiet'] -and $Summary.quiet)
  $verdict = if ($device -ne $Budget.machine.device) { 'device' }
             elseif ($missing.Count -gt 0) { 'malformed' }
             elseif (-not $over) { 'pass' }
             elseif ($quiet) { 'over' }
             else { 'busy' }
  return [pscustomobject]@{
    Id = $Run.id; Verdict = $verdict; Device = $device; Quiet = $quiet; Rows = $rows; Missing = $missing
    MachineState = (Format-MachineState $Summary)
  }
}

# The gate's exit code over every run's result: 1 when any run is over on a quiet machine, failed
# or is malformed; 4 when the only overs were on a busy machine (measure again later); 3 when no
# run was drawn on the budget's machine at all; 0 otherwise.
function Get-FrameBudgetExitCode {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [AllowEmptyCollection()] [object[]]$Results)
  $verdicts = @($Results | ForEach-Object { $_.Verdict })
  if ($verdicts | Where-Object { $_ -in @('over', 'malformed', 'failed') }) { return 1 }
  if ($verdicts | Where-Object { $_ -eq 'busy' }) { return 4 }
  if ($verdicts.Count -gt 0 -and -not ($verdicts | Where-Object { $_ -ne 'device' })) { return 3 }
  return 0
}

# One line for the machine's state around a run: what a reader needs beside a number.
function Format-MachineState($summary) {
  if (-not $summary.PSObject.Properties['machine_state'] -or $null -eq $summary.machine_state) { return 'not recorded' }
  $ms = $summary.machine_state
  $part = {
    param($s)
    if ($null -eq $s) { return '?' }
    $cpu = if ($null -ne $s.cpu_others_pct) { '{0:N1}% others' -f [double]$s.cpu_others_pct } else { 'cpu ?' }
    $gpu = if ($null -ne $s.gpu_util_pct) { 'GPU {0}%' -f $s.gpu_util_pct } else { 'GPU ?' }
    "$cpu, $gpu"
  }
  $quiet = if ($summary.quiet) { 'quiet' } else { 'busy' }
  return '{0} (start {1}; end {2})' -f $quiet, (& $part $ms.start), (& $part $ms.end)
}

function Format-FrameBudgetReport {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [AllowEmptyCollection()] [object[]]$Results, $Budget)
  $out = @()
  foreach ($r in $Results) {
    $out += "## $($r.Id): $($r.Verdict)"
    $out += ''
    $out += "Device: $($r.Device). Machine: $($r.MachineState)."
    if ($r.Missing.Count -gt 0) { $out += "Missing from the summary: $($r.Missing -join ', ')." }
    if ($r.Rows.Count -gt 0) {
      $out += ''
      $out += '| line | stat | budget | limit | measured | |'
      $out += '|---|---|---|---|---|---|'
      foreach ($row in $r.Rows) {
        $mark = if ($row.Over) { '**over**' } else { '' }
        $out += '| {0} | {1} | {2:N3} | {3:N3} | {4:N3} | {5} |' -f $row.Line, $row.Stat, $row.Budget, $row.Limit, $row.Measured, $mark
      }
    }
    $out += ''
  }
  return $out
}

<#
  A run's budget set from a summary: every GPU pass the summary timed (a median or a p99 of at
  least 0.001 ms) and the total, and the frame thread's cpu_ms, rounded to the microsecond; and the
  baseline record — who, when, why, from which commit, on which device, and the machine's state.
  The old numbers are in git's history of the file; the record says why they moved.
#>
function Set-FrameBudgetBaseline {
  [CmdletBinding()]
  param([Parameter(Mandatory)] $Run, [Parameter(Mandatory)] $Summary, [Parameter(Mandatory)] [string]$By,
        [Parameter(Mandatory)] [string]$Reason, [string]$Commit = '', [string]$Date = (Get-Date -Format 'yyyy-MM-dd'))
  if (-not $Reason.Trim()) { throw 'frame-budget: a re-baseline says why (-Reason)' }
  $gpu = [ordered]@{}
  foreach ($p in $Summary.gpu_ms.PSObject.Properties) {
    $median = [double]$p.Value.median
    $p99 = [double]$p.Value.p99
    if ($p.Name -eq 'total' -or $median -ge 0.001 -or $p99 -ge 0.001) {
      $gpu[$p.Name] = [ordered]@{ median = [math]::Round($median, 3); p99 = [math]::Round($p99, 3) }
    }
  }
  $Run.gpu_ms = [pscustomobject]$gpu
  $cpu = [pscustomobject][ordered]@{ median = [math]::Round([double]$Summary.cpu_ms.median, 3); p99 = [math]::Round([double]$Summary.cpu_ms.p99, 3) }
  if ($Run.PSObject.Properties['cpu_ms']) { $Run.cpu_ms = $cpu } else { $Run | Add-Member -NotePropertyName cpu_ms -NotePropertyValue $cpu }
  $baseline = [pscustomobject][ordered]@{
    date = $Date; by = $By; reason = $Reason; commit = $Commit; device = "$($Summary.device)"
    quiet = [bool]$Summary.quiet; machine_state = (Format-MachineState $Summary)
    identity = "$($Summary.identity)"; frames = $Summary.frames; repeats = $Summary.repeats
  }
  if ($Run.PSObject.Properties['baseline']) { $Run.baseline = $baseline } else { $Run | Add-Member -NotePropertyName baseline -NotePropertyValue $baseline }
  return $Run
}

Export-ModuleMember -Function Read-FrameBudget, Assert-FrameBudget, Get-FrameBudgetArgs, Get-FrameBudgetLimit,
  Test-FrameBudgetRun, Get-FrameBudgetExitCode, Format-FrameBudgetReport, Set-FrameBudgetBaseline, Format-MachineState

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

  **A run holds a budget per session state** (roadmap T161; bench.md, "Frame budgets"): with the
  Windows session locked the card idles its clocks and a light scene reads two to nine times slower,
  so a locked flight and an unlocked one are two different cards. `budgets.locked` and
  `budgets.unlocked` sit side by side under each run, a summary is compared only with the set of
  the state its `machine_state` recorded, and a state with no set is a verdict of its own
  ('nobaseline') rather than a comparison with the other state's numbers.
#>

Set-StrictMode -Version Latest

$script:Format = 'engine.frame-budgets.v2'
$script:Stats = @('median', 'p99')
$script:Sessions = @('locked', 'unlocked')

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
    if ($null -ne $run.PSObject.Properties['gpu_ms'] -or $null -ne $run.PSObject.Properties['baseline']) {
      throw "frame-budget: run '$($run.id)' has gpu_ms or baseline outside budgets.<session>: a budget belongs to the session state it was flown in (budgets.locked, budgets.unlocked)"
    }
    if ($null -eq $run.budgets) { throw "frame-budget: run '$($run.id)' has no budgets (budgets.locked or budgets.unlocked)" }
    $names = @($run.budgets.PSObject.Properties | ForEach-Object { $_.Name })
    if ($names.Count -eq 0) { throw "frame-budget: run '$($run.id)' has no budgets (budgets.locked or budgets.unlocked)" }
    foreach ($name in $names) {
      if ($name -notin $script:Sessions) { throw "frame-budget: run '$($run.id)' budgets.$name is not a session state ($($script:Sessions -join ', '))" }
      $set = $run.budgets.$name
      $where = "run '$($run.id)' budgets.$name"
      if ($null -eq $set.gpu_ms -or $null -eq $set.gpu_ms.total) { throw "frame-budget: $where budgets no gpu_ms.total" }
      foreach ($line in (Get-BudgetLines $set)) {
        foreach ($s in $script:Stats) {
          if ($null -eq $line.Value.$s) { throw "frame-budget: $where $($line.Name) has no $s" }
        }
      }
      if ($null -eq $set.baseline -or -not $set.baseline.by -or -not $set.baseline.reason -or -not $set.baseline.date) {
        throw "frame-budget: $where does not say who set its budget, when and why (baseline.by, .date, .reason)"
      }
    }
  }
}

# The budgeted lines of one session's set: every pass under gpu_ms, and cpu_ms when it has one.
function Get-BudgetLines($set) {
  $lines = @()
  foreach ($p in $set.gpu_ms.PSObject.Properties) {
    $lines += [pscustomobject]@{ Kind = 'gpu'; Name = "gpu_ms.$($p.Name)"; Pass = $p.Name; Value = $p.Value }
  }
  if ($null -ne $set.PSObject.Properties['cpu_ms'] -and $null -ne $set.cpu_ms) {
    $lines += [pscustomobject]@{ Kind = 'cpu'; Name = 'cpu_ms'; Pass = ''; Value = $set.cpu_ms }
  }
  return $lines
}

<#
  The session state a summary was flown in, from its `machine_state` (bench.md, "Measuring on a
  shared machine": `session_locked` at the start and at the end, null where it is unknown):
    'locked'    locked at both ends, or at one with the other unknown
    'unlocked'  unlocked at both ends, or at one with the other unknown
    'changed'   locked at one end and unlocked at the other: the flight measured two cards
    'unknown'   neither end says (a machine with no session to read, or no machine_state)
#>
function Get-SessionState {
  [CmdletBinding()]
  param([Parameter(Mandatory)] $Summary)
  if (-not $Summary.PSObject.Properties['machine_state'] -or $null -eq $Summary.machine_state) { return 'unknown' }
  $ends = @()
  foreach ($end in @('start', 'end')) {
    $s = if ($Summary.machine_state.PSObject.Properties[$end]) { $Summary.machine_state.$end } else { $null }
    if ($null -eq $s -or -not $s.PSObject.Properties['session_locked'] -or $null -eq $s.session_locked) { continue }
    $ends += $(if ([bool]$s.session_locked) { 'locked' } else { 'unlocked' })
  }
  $distinct = @($ends | Select-Object -Unique)
  if ($distinct.Count -eq 0) { return 'unknown' }
  if ($distinct.Count -gt 1) { return 'changed' }
  return $distinct[0]
}

# A run's budget set for one session state, or $null when the run holds none for it.
function Get-SessionBudget($Run, [string]$Session) {
  if ($null -eq $Run.PSObject.Properties['budgets'] -or $null -eq $Run.budgets) { return $null }
  $p = $Run.budgets.PSObject.Properties[$Session]
  if ($null -eq $p) { return $null }
  return $p.Value
}

# The session states a run holds budgets for, with each one's date: "locked (2026-10-12)".
function Format-RunSessions($Run) {
  $held = @()
  foreach ($name in $script:Sessions) {
    $set = Get-SessionBudget $Run $name
    if ($null -ne $set) { $held += "$name ($($set.baseline.date))" }
  }
  if ($held.Count -eq 0) { return 'none' }
  return ($held -join ', ')
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
  **The frame thread's two modes, said and never judged** (roadmap T221; bench.md, "The frame
  thread's two modes"). On this machine a flight's `cpu_ms` is often two-moded: most frames in a
  fast mode and a share in a slow mode 2.3 to 3 times dearer, every section of the frame by the same
  factor. A verdict prints the share and the two modes' medians beside the whole median so that a
  reader of a green gate sees a share that jumped; **nothing here decides a verdict**: the frame
  thread's median is judged whole, as it always was. A judge by the fast mode alone was tried and
  refused (T221's review): every fast frame is at or under the split, so its median can cross a limit
  only when the p10 itself is over half of it, and a cost on every second or third frame, or a change
  that makes some frames cheaper while the rest grow, would pass.

  The split is twice the flight's 10th percentile: a frame over it is counted in the slow mode.
#>
$script:SlowSplit = 2.0

# The frame thread's `cpu_ms` of every frame record in a flight's .jsonl (the lines with a `frame`;
# the summary, which has none, is left out). An empty array for a file of summaries alone.
function Read-FrameCpu {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [string]$Path)
  $out = [System.Collections.Generic.List[double]]::new()
  if (-not (Test-Path -LiteralPath $Path)) { return , $out.ToArray() }
  foreach ($text in [IO.File]::ReadLines($Path)) {
    if ([string]::IsNullOrWhiteSpace($text)) { continue }
    $doc = $null
    try { $doc = [System.Text.Json.JsonDocument]::Parse($text) } catch { continue }
    try {
      $root = $doc.RootElement
      if ($root.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) { continue }
      $frame = [System.Text.Json.JsonElement]::new()
      $cpu = [System.Text.Json.JsonElement]::new()
      if ($root.TryGetProperty([string]'frame', [ref]$frame) -and $root.TryGetProperty([string]'cpu_ms', [ref]$cpu) -and
          $cpu.ValueKind -eq [System.Text.Json.JsonValueKind]::Number) {
        $out.Add($cpu.GetDouble())
      }
    } finally { $doc.Dispose() }
  }
  return , $out.ToArray()
}

# The two modes of a flight's per-frame `cpu_ms`, as numbers: the frames, the p10 and the split, the
# fast mode's median, the slow share and the slow mode's median (null with no slow frame), and the
# whole flight's median. $null for a flight with no frame records.
function Get-CpuModes {
  [CmdletBinding()]
  param([AllowEmptyCollection()] [double[]]$Frames = @())
  $n = @($Frames).Count
  if ($n -eq 0) { return $null }
  $sorted = [double[]]@($Frames)
  [Array]::Sort($sorted)
  $p10 = $sorted[[int][math]::Floor(0.1 * $n)]
  $split = $script:SlowSplit * $p10
  $fast = 0
  while ($fast -lt $n -and $sorted[$fast] -le $split) { $fast++ }
  $slow = $n - $fast
  return [pscustomobject][ordered]@{
    Frames = $n; P10 = $p10; Split = $split
    FastMedian = $(if ($fast -gt 0) { $sorted[[int][math]::Floor($fast / 2)] } else { $null })
    SlowShare = $slow / $n
    SlowMedian = $(if ($slow -gt 0) { $sorted[$fast + [int][math]::Floor($slow / 2)] } else { $null })
    WholeMedian = $sorted[[int][math]::Floor($n / 2)]
  }
}

# One line for a flight's modes, said beside the verdict and deciding nothing.
function Format-CpuModes($Modes) {
  if ($null -eq $Modes) { return 'cpu_ms modes: no frame records' }
  $slowMode = if ($null -ne $Modes.SlowMedian) { ', slow mode {0:N3} ms' -f $Modes.SlowMedian } else { '' }
  # The share as N1 and a sign, not P1: P1's pattern is the culture's ("53.2 %" in the invariant one).
  return 'cpu_ms modes (said, not judged): slow share {0:N1}%, fast mode {1:N3} ms{2}; whole median {3:N3} ms judged (split {4:N3})' -f
    (100.0 * $Modes.SlowShare), $Modes.FastMedian, $slowMode, $Modes.WholeMedian, $Modes.Split
}

<#
  One run's summary against its budget. Returns the run's verdict and a row per budgeted number:
    Verdict  'pass'       every number within its limit
             'over'       a number over its limit on a quiet machine: the gate fails
             'busy'       a number over its limit on a busy machine: inconclusive
             'device'     the summary was drawn on another adapter than the budget's: skipped
             'malformed'  the summary lacks a budgeted line: the gate fails
             'reconstructed'  the summary is not a native measurement (ADR-0064): refused before
                          any number of it is read, whatever it measured and on whatever adapter
             'nobaseline' the summary's session state is known and the run holds no budget for
                          it: compared with nothing, never with the other state's numbers (T161)
             'session'    the summary's session state is unknown, or changed during the flight:
                          compared with nothing
  `Session` is the summary's state (Get-SessionState) and `Compared` the baseline it was held to.
  `-Frames` (Read-FrameCpu) gives `Modes` (Get-CpuModes), which is said and never judged: the
  verdict and every row are what they are without it.
#>
function Test-FrameBudgetRun {
  [CmdletBinding()]
  param([Parameter(Mandatory)] $Budget, [Parameter(Mandatory)] $Run, [Parameter(Mandatory)] $Summary,
        [AllowEmptyCollection()] [double[]]$Frames = @())
  $device = if ($Summary.PSObject.Properties['device']) { "$($Summary.device)" } else { '' }
  $session = Get-SessionState -Summary $Summary
  $refusal = Get-ReconstructionRefusal -Summary $Summary
  if ($refusal) {
    return [pscustomobject]@{
      Id = $Run.id; Verdict = 'reconstructed'; Device = $device; Session = $session; Compared = ''
      Quiet = $false; Rows = @(); Missing = @(); MachineState = $refusal
    }
  }
  $quiet = [bool]($Summary.PSObject.Properties['quiet'] -and $Summary.quiet)
  $state = Format-MachineState $Summary
  if ($device -ne $Budget.machine.device) {
    return [pscustomobject]@{ Id = $Run.id; Verdict = 'device'; Device = $device; Session = $session; Compared = ''; Quiet = $quiet; Rows = @(); Missing = @(); MachineState = $state }
  }
  # The session state before any number: a locked card and an unlocked one are two cards, so a
  # summary is held to its own state's set or to nothing (T161; bench.md, "Frame budgets").
  if ($session -in @('unknown', 'changed')) {
    $why = if ($session -eq 'changed') { 'the session was locked at one end of the flight and unlocked at the other, so it measured two cards' }
           else { 'the summary does not record whether the session was locked (machine_state.*.session_locked)' }
    return [pscustomobject]@{
      Id = $Run.id; Verdict = 'session'; Device = $device; Session = $session; Compared = ''; Quiet = $quiet; Rows = @(); Missing = @()
      MachineState = "$state; compared with nothing: $why; fly it again in one state"
    }
  }
  $set = Get-SessionBudget $Run $session
  if ($null -eq $set) {
    return [pscustomobject]@{
      Id = $Run.id; Verdict = 'nobaseline'; Device = $device; Session = $session; Compared = ''; Quiet = $quiet; Rows = @(); Missing = @()
      MachineState = "$state; compared with nothing: flown with the session $session and the run has no $session baseline (it holds: $(Format-RunSessions $Run)); fly it in a state it holds, or take a $session baseline with -Rebaseline -Reason ""<why>"" in this state"
    }
  }
  $rows = @()
  $over = $false
  $missing = @()
  foreach ($line in (Get-BudgetLines $set)) {
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
  $verdict = if ($missing.Count -gt 0) { 'malformed' }
             elseif (-not $over) { 'pass' }
             elseif ($quiet) { 'over' }
             else { 'busy' }
  return [pscustomobject]@{
    Id = $Run.id; Verdict = $verdict; Device = $device; Session = $session
    Compared = "the $session baseline of $($set.baseline.date)"; Quiet = $quiet; Rows = $rows; Missing = $missing
    MachineState = $state; Modes = (Get-CpuModes -Frames $Frames)
  }
}

# The gate's exit code over every run's result: 5 when any run was not a native measurement
# (ADR-0064: nothing of such a gate is a verdict, so it wins over every other code); 1 when any run
# is over on a quiet machine, failed or is malformed; 6 when a run was compared with nothing because
# the run holds no budget for the session state it was flown in, or its state is unknown or changed
# during the flight (T161: fly it in a state the run holds, or take that state's baseline); 4 when
# the only overs were on a busy machine (measure again later); 3 when no run was drawn on the
# budget's machine at all; 0 otherwise.
function Get-FrameBudgetExitCode {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [AllowEmptyCollection()] [object[]]$Results)
  $verdicts = @($Results | ForEach-Object { $_.Verdict })
  if ($verdicts | Where-Object { $_ -eq 'reconstructed' }) { return 5 }
  if ($verdicts | Where-Object { $_ -in @('over', 'malformed', 'failed') }) { return 1 }
  if ($verdicts | Where-Object { $_ -in @('nobaseline', 'session') }) { return 6 }
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
    # An end sample read again once nvidia-smi's period had passed: the first reading was the
    # flight's own last frames, and saying so keeps the discount visible (bench.md, "The end
    # sample is read after the run's own work").
    if ($s.PSObject.Properties['gpu_own_tail_pct'] -and $null -ne $s.gpu_own_tail_pct) { $gpu += ' (own tail {0}%)' -f $s.gpu_own_tail_pct }
    "$cpu, $gpu"
  }
  $quiet = if ($summary.quiet) { 'quiet' } else { 'busy' }
  return '{0}, session {1} (start {2}; end {3})' -f $quiet, (Get-SessionState -Summary $summary), (& $part $ms.start), (& $part $ms.end)
}

function Format-FrameBudgetReport {
  [CmdletBinding()]
  param([Parameter(Mandatory)] [AllowEmptyCollection()] [object[]]$Results, $Budget)
  $out = @()
  foreach ($r in $Results) {
    $out += "## $($r.Id): $($r.Verdict)"
    $out += ''
    $out += "Device: $($r.Device). Machine: $($r.MachineState)."
    if ($r.PSObject.Properties['Compared'] -and $r.Compared) { $out += "Compared with $($r.Compared)." }
    if ($r.PSObject.Properties['Modes'] -and $null -ne $r.Modes) { $out += "$(Format-CpuModes $r.Modes)." }
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
  A run's budget set from a summary, for the session state the summary was flown in: every GPU
  pass the summary timed (a median or a p99 of at least 0.001 ms) and the total, and the frame
  thread's cpu_ms, rounded to the microsecond; and the baseline record — who, when, why, from which
  commit, on which device, the session state and the machine's state. The other state's set is left
  as it was. It refuses a summary whose session state is unknown or changed during the flight: a
  number that belongs to neither card is a baseline for neither. The old numbers are in git's
  history of the file; the record says why they moved.
#>
function Set-FrameBudgetBaseline {
  [CmdletBinding()]
  param([Parameter(Mandatory)] $Run, [Parameter(Mandatory)] $Summary, [Parameter(Mandatory)] [string]$By,
        [Parameter(Mandatory)] [string]$Reason, [string]$Commit = '', [string]$Date = (Get-Date -Format 'yyyy-MM-dd'))
  if (-not $Reason.Trim()) { throw 'frame-budget: a re-baseline says why (-Reason)' }
  $refusal = Get-ReconstructionRefusal -Summary $Summary
  if ($refusal) { throw "frame-budget: a budget is never set from a summary that is $refusal" }
  $session = Get-SessionState -Summary $Summary
  if ($session -notin $script:Sessions) {
    throw "frame-budget: a budget is never set from a flight whose session state is $($session): it is a baseline for neither the locked card nor the unlocked one"
  }
  $gpu = [ordered]@{}
  foreach ($p in $Summary.gpu_ms.PSObject.Properties) {
    $median = [double]$p.Value.median
    $p99 = [double]$p.Value.p99
    if ($p.Name -eq 'total' -or $median -ge 0.001 -or $p99 -ge 0.001) {
      $gpu[$p.Name] = [ordered]@{ median = [math]::Round($median, 3); p99 = [math]::Round($p99, 3) }
    }
  }
  $cpu = [pscustomobject][ordered]@{ median = [math]::Round([double]$Summary.cpu_ms.median, 3); p99 = [math]::Round([double]$Summary.cpu_ms.p99, 3) }
  $baseline = [pscustomobject][ordered]@{
    date = $Date; by = $By; reason = $Reason; commit = $Commit; device = "$($Summary.device)"
    session = $session; quiet = [bool]$Summary.quiet; machine_state = (Format-MachineState $Summary)
    identity = "$($Summary.identity)"; frames = $Summary.frames; repeats = $Summary.repeats
  }
  $set = [pscustomobject][ordered]@{ gpu_ms = [pscustomobject]$gpu; cpu_ms = $cpu; baseline = $baseline }
  if ($null -eq $Run.PSObject.Properties['budgets'] -or $null -eq $Run.budgets) {
    if ($Run.PSObject.Properties['budgets']) { $Run.budgets = [pscustomobject]@{} } else { $Run | Add-Member -NotePropertyName budgets -NotePropertyValue ([pscustomobject]@{}) }
  }
  if ($Run.budgets.PSObject.Properties[$session]) { $Run.budgets.$session = $set } else { $Run.budgets | Add-Member -NotePropertyName $session -NotePropertyValue $set }
  return $Run
}

<#
  **The measurement rule** (ADR-0064; docs/subsystems/renderer.md, "Sampling over time: the
  measurement rule"): whether a summary or a frame record is a native measurement, and so a cost.
  It is not when its `reconstruction` block names a provider other than "off" or an internal size
  other than the output's, or when it carries `reconstructed` or has a null `gpu_ms`: any one of
  them refuses it, so a writer that forgot one is still refused. A line with no `reconstruction`
  block and a `gpu_ms` was written before the block existed and is native.
  Returns $null for a native measurement, else the sentence that says why not: every tool that
  reads a cost asks this first (tools/frame-budget.ps1, tools/flythrough.ps1).
#>
function Get-ReconstructionRefusal {
  [CmdletBinding()]
  param([Parameter(Mandatory)] $Summary)
  $block = if ($Summary.PSObject.Properties['reconstruction']) { $Summary.reconstruction } else { $null }
  if ($null -ne $block) {
    $provider = if ($block.PSObject.Properties['provider']) { "$($block.provider)" } else { 'off' }
    $sizes = '{0}x{1} -> {2}x{3}' -f $block.internal_width, $block.internal_height, $block.output_width, $block.output_height
    if ($provider -and $provider -ne 'off') { return "reconstructed ($provider, $sizes): not a native measurement, so not a cost (ADR-0064)" }
    if ([int64]$block.internal_width -ne [int64]$block.output_width -or [int64]$block.internal_height -ne [int64]$block.output_height) {
      return "reconstructed (off, $sizes): drawn below the output's size, so not a cost (ADR-0064)"
    }
  }
  if ($Summary.PSObject.Properties['reconstructed'] -and $null -ne $Summary.reconstructed) {
    return 'reconstructed: its times are under `reconstructed`, so it has no cost to read (ADR-0064)'
  }
  if ($Summary.PSObject.Properties['gpu_ms'] -and $null -eq $Summary.gpu_ms -and $null -ne $block) {
    return 'reconstructed: its `gpu_ms` is null, so it has no cost to read (ADR-0064)'
  }
  return $null
}

Export-ModuleMember -Function Read-FrameBudget, Assert-FrameBudget, Get-FrameBudgetArgs, Get-FrameBudgetLimit,
  Test-FrameBudgetRun, Get-FrameBudgetExitCode, Format-FrameBudgetReport, Set-FrameBudgetBaseline, Format-MachineState,
  Get-ReconstructionRefusal, Get-SessionState, Get-SessionBudget, Format-RunSessions,
  Read-FrameCpu, Get-CpuModes, Format-CpuModes

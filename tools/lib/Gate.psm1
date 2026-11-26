# The merge gate's judgement (roadmap T217; docs/ci/gate.md): reading a
# kept CTest log record by record, a suite's verdict from its logs and not from a console, the
# integrity read, the filters a batch and a follow-up use, the litter check, and starting a leg as
# a process of its own. tools/gate.ps1 drives it; tools/gate.Tests.ps1 tests it over a throwaway
# repository with stand-in commands.

Set-StrictMode -Version 3.0

# The release suite's set: the apps' end-to-end tests, a test's parts (engine_view.1 to .4, T77) and
# its device half (engine_cli.gpu, T170). CTest's regex has no \d.
$script:ReleaseFilter = '^engine_(view|editor|host|cli|content)(\.([0-9]+|gpu))?$'
# The weaker card's set (T162): the renderer's parts, the water and the CLI's device half.
$script:WeakerCardFilter = '^(renderer\.[0-9]+|water|engine_cli\.gpu)$'

function Get-GateFilters {
  <#
  .SYNOPSIS
    The CTest filter each Windows suite runs with: everything (empty) for a batch's gate, or the
    follow-up's reach anchored whole in the three wave-one suites. The release suite always runs the
    apps' end-to-end set, which is what it is in the gate for.
  #>
  param([string]$Reach = '')
  $wave1 = if ($Reach) { "^($Reach)$" } else { '' }
  [ordered]@{
    'msvc-debug'   = $wave1
    'msvc-minimal' = $wave1
    'msvc-no-ecs'  = $wave1
    'msvc-release' = $script:ReleaseFilter
  }
}

function Get-WeakerCardFilter { $script:WeakerCardFilter }

function ConvertTo-CommandLine {
  <#
  .SYNOPSIS
    One Windows command line from an argument list, each argument quoted the way CommandLineToArgvW
    reads it back (Start-Process joins its -ArgumentList with spaces and quotes nothing, so a path
    or a regex with a space or a quote would arrive as two arguments).
  #>
  param([string[]]$Arguments)
  $parts = foreach ($a in $Arguments) {
    if ($a -ne '' -and $a -notmatch '[\s"]') { $a; continue }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('"')
    $slashes = 0
    foreach ($ch in $a.ToCharArray()) {
      if ($ch -eq '\') { $slashes++; continue }
      if ($ch -eq '"') { [void]$sb.Append('\', 2 * $slashes + 1); [void]$sb.Append('"'); $slashes = 0; continue }
      if ($slashes) { [void]$sb.Append('\', $slashes); $slashes = 0 }
      [void]$sb.Append($ch)
    }
    if ($slashes) { [void]$sb.Append('\', 2 * $slashes) }
    [void]$sb.Append('"')
    $sb.ToString()
  }
  $parts -join ' '
}

function Read-CTestLog {
  <#
  .SYNOPSIS
    Every test record of one kept LastTest.log: its name and what the record itself says.
  .DESCRIPTION
    Status is the record's footer, read with the test's own output beside it, because the footer
    alone lies once: CTest writes "Test Passed." for a test its console calls ***Skipped (exit 75,
    SKIP_RETURN_CODE: the test's processes gave up waiting for the GPU lock; 2026-11-21). So:
      passed      the footer says Passed and the output did not give up without a summary
      gave-up     the output says "gave up waiting for the GPU lock" and carries no doctest summary
      failed      the footer says Failed
      incomplete  no footer (the run was cut off inside the record)
    The footer has two spellings. A test CTest judges by its exit code ends "Test Passed." or
    "Test Failed."; one it judges by a regular expression (PASS_REGULAR_EXPRESSION, which the hosts'
    generators tests carry) ends "Test Pass Reason:" or "Test Fail Reason:" with the reason on the
    next line, and never the first form. Batch 73's first gate read the four generators tests of
    three suites as cut off for knowing only the first.
    NoCase is a doctest summary of zero test cases; Summary says whether a doctest summary is there.
  #>
  param([Parameter(Mandatory)][string]$Path)
  $raw = [IO.File]::ReadAllText($Path)
  $records = New-Object System.Collections.Generic.List[object]
  foreach ($blk in [regex]::Split($raw, '(?m)^(?=\d+/\d+ Testing: )')) {
    if ($blk -notmatch '^\d+/\d+ Testing: (\S+)') { continue }
    $name = $Matches[1]
    $summary = $blk -match '\[doctest\] test cases:'
    $gaveUp = $blk -match 'gave up waiting for the GPU lock'
    $noCase = $blk -match '\[doctest\] test cases:\s+0 \|'
    $status = if ($blk -match '(?m)^Test (Failed\.|Fail Reason:)') { 'failed' }
              elseif ($blk -match '(?m)^Test (Passed\.|Pass Reason:)') { if ($gaveUp -and -not $summary) { 'gave-up' } else { 'passed' } }
              else { 'incomplete' }
    $records.Add([pscustomobject]@{ Name = $name; Status = $status; GaveUp = $gaveUp; Summary = $summary; NoCase = $noCase; Log = $Path })
  }
  , $records.ToArray()
}

function Test-NoCaseByDesign {
  <#
  .SYNOPSIS
    The three records that run no doctest case by design: the NPC replay's cases need the ECS
    (minimal and no-ecs), and the frame loop's need the capabilities the minimal build switches off.
  #>
  param([string]$Name, [string]$Preset)
  ($Name -eq 'engine_cli.npc_replay' -and $Preset -in 'msvc-minimal', 'msvc-no-ecs') -or
    ($Name -eq 'engine_view.frame_loop' -and $Preset -eq 'msvc-minimal')
}

function Get-SuiteState {
  <#
  .SYNOPSIS
    A suite's tests by name, each at its last record across the suite's kept logs in the order they
    were written (a rerun's log after the pass it reruns), with what is wrong with the suite.
  .OUTPUTS
    Tests (name -> last record), Failed, GaveUp, Incomplete (names), Records (count).
  #>
  param([string[]]$Logs = @())
  $last = [ordered]@{}
  $count = 0
  foreach ($log in $Logs) {
    foreach ($r in (Read-CTestLog -Path $log)) { $last[$r.Name] = $r; $count++ }
  }
  $by = { param($s) @($last.Values | Where-Object { $_.Status -eq $s } | ForEach-Object { $_.Name }) }
  [pscustomobject]@{
    Tests = $last; Records = $count
    # @() again: a scriptblock's one-element array reaches its caller as the element itself.
    Failed = @(& $by 'failed'); GaveUp = @(& $by 'gave-up'); Incomplete = @(& $by 'incomplete')
  }
}

function Get-RerunPlan {
  <#
  .SYNOPSIS
    What a suite earns a second look at, once, alone: every test that gave up waiting for the card,
    and engine_editor when it is the suite's only failure (its end-to-end cases depend on which
    window the desktop has in front; beside other suites' windows they failed and alone they passed,
    A40). Nothing else is rerun: a failure is a verdict.
  #>
  param([Parameter(Mandatory)]$State)
  $names = @($State.GaveUp)
  if (@($State.Failed).Count -eq 1 -and $State.Failed[0] -eq 'engine_editor' -and @($State.Incomplete).Count -eq 0) { $names += 'engine_editor' }
  $names
}

function Test-RunExplained {
  <#
  .SYNOPSIS
    Whether one `tools/dev.ps1 test` run's exit code is what its own kept logs say: 0 is; 75 only when
    a log of that run holds a record that gave up waiting for the card (its give-ups are then judged
    by the suite's state); anything else only when a log of that run holds a failed or cut-off record.
    A run that exited 1 having kept nothing, 8 over all-passed records, or 75 over records none of
    which gave up, is a fault even when a later rerun passed: the code and the log disagree. dev.ps1
    exits 75 for any "(Skipped)" line, and a skipped record whose give-up line is missing reads as
    passed (Read-CTestLog), so a 75 taken on trust would be a test that never ran read as a pass
    (T217-M3).
  #>
  param([int]$Exit, [string[]]$Logs = @())
  if ($Exit -eq 0) { return $true }
  $own = Get-SuiteState -Logs $Logs
  if ($Exit -eq 75) { return [bool]@($own.GaveUp).Count }
  [bool](@($own.Failed).Count + @($own.Incomplete).Count)
}

function Read-LegVerdict {
  <#
  .SYNOPSIS
    The gate's reading of the Linux leg's verdict file at the end: no verdict when the file is
    missing, unreadable, not finished (`done`: a leg that died part-way never writes it), or names
    other than -Expected containers; otherwise a line per container and the red ones by name.
  #>
  param([Parameter(Mandatory)][string]$Path, [int]$Expected = 3)
  $v = $null
  if (Test-Path -LiteralPath $Path) { try { $v = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop } catch { $v = $null } }
  $lines = New-Object System.Collections.Generic.List[string]
  $reds = New-Object System.Collections.Generic.List[string]
  $none = $null
  $done = $v -and $v.PSObject.Properties['done'] -and $v.done -and $v.PSObject.Properties['presets']
  if (-not $done) { $none = 'the linux leg did not finish its verdict file' }
  else {
    $presets = @($v.presets.PSObject.Properties)
    foreach ($p in $presets) {
      $lines.Add("LINUX $($p.Name) exit $($p.Value.exit): $($p.Value.line)")
      if (-not $p.Value.ok) { $reds.Add("linux $($p.Name)") }
    }
    if ($presets.Count -ne $Expected) { $none = "the linux leg reported $($presets.Count) containers, not $Expected" }
  }
  [pscustomobject]@{ Verdict = $v; Lines = $lines.ToArray(); Red = $reds.ToArray(); NoVerdict = $none }
}

function Get-WorktreeDirt {
  <#
  .SYNOPSIS
    What makes a worktree other than the commit at its HEAD: every line of `git status --porcelain`
    (tracked changes and unmerged paths, and with -TrackedOnly not given, untracked files git does
    not ignore) and an unfinished merge, cherry-pick or revert. A gate's verdict names a commit, so
    its tree must be exactly that commit (T217-H1). A git that fails is a fault, never a clean tree.
  #>
  param([Parameter(Mandatory)][string]$Root, [switch]$TrackedOnly)
  $mode = if ($TrackedOnly) { 'no' } else { 'normal' }
  $st = @(& git -C $Root status --porcelain "--untracked-files=$mode" 2>$null)
  if ($LASTEXITCODE -ne 0) { throw "git status failed in $Root (exit $LASTEXITCODE)" }
  $dirt = @($st | ForEach-Object { "$_" } | Where-Object { $_ })
  foreach ($ref in 'MERGE_HEAD', 'CHERRY_PICK_HEAD', 'REVERT_HEAD') {
    $null = & git -C $Root rev-parse -q --verify $ref 2>$null
    if ($LASTEXITCODE -eq 0) { $dirt += "$ref exists: an unfinished operation" }
  }
  # Unrolled, one line an object: callers collect it with @(), and `, $dirt` would reach them as
  # one element holding the list.
  $dirt
}

function Get-IntegrityFindings {
  <#
  .SYNOPSIS
    Every kept log, record by record: CTest's word against the test's own.
  .DESCRIPTION
    Logs is a list of @{ Preset; Path } in the order they were written. Findings, each a line:
      GAVE-UP   a record that gave up with no summary, and no later record of the same test in the
                same preset has a verdict of its own (a doctest summary; for a test that never prints
                one, a footer without the give-up line)
      NO-CASE   a doctest summary of zero cases, except the three by design (Test-NoCaseByDesign),
                which are listed as "(by design)" and are not faults
      INCOMPLETE a record with no footer
  #>
  param([object[]]$Logs = @())
  $all = New-Object System.Collections.Generic.List[object]
  foreach ($l in $Logs) {
    foreach ($r in (Read-CTestLog -Path $l.Path)) { $all.Add([pscustomobject]@{ Preset = $l.Preset; Record = $r; File = (Split-Path $l.Path -Leaf) }) }
  }
  $lines = New-Object System.Collections.Generic.List[string]
  $bad = 0
  for ($i = 0; $i -lt $all.Count; $i++) {
    $e = $all[$i]; $r = $e.Record
    if ($r.GaveUp -and -not $r.Summary) {
      $resolved = $false
      for ($j = $i + 1; $j -lt $all.Count; $j++) {
        $later = $all[$j]
        if ($later.Preset -ne $e.Preset -or $later.Record.Name -ne $r.Name) { continue }
        if ($later.Record.Summary -or ($later.Record.Status -in 'passed', 'failed' -and -not $later.Record.GaveUp)) { $resolved = $true; break }
      }
      if ($resolved) { $lines.Add("GAVE-UP (answered later) $($e.File) $($r.Name)") }
      else { $lines.Add("GAVE-UP $($e.File) $($r.Name): no later record has a verdict of its own"); $bad++ }
    }
    if ($r.NoCase) {
      if (Test-NoCaseByDesign -Name $r.Name -Preset $e.Preset) { $lines.Add("NO-CASE (by design) $($e.File) $($r.Name)") }
      else { $lines.Add("NO-CASE $($e.File) $($r.Name)"); $bad++ }
    }
    if ($r.Status -eq 'incomplete') { $lines.Add("INCOMPLETE $($e.File) $($r.Name)"); $bad++ }
  }
  [pscustomobject]@{ Records = $all.Count; Faults = $bad; Lines = $lines.ToArray() }
}

function Read-ContainerVerdict {
  <#
  .SYNOPSIS
    One Linux container's verdict from its whole transcript and its exit code: green only when the
    script exited 0 and CTest's own summary line says no test failed out of more than none.
  #>
  param([string]$Text = '', [int]$Exit)
  $m = [regex]::Match("$Text", '(?m)\b(\d+)% tests passed, (\d+) tests? failed out of (\d+)')
  if (-not $m.Success) { return [pscustomobject]@{ Ok = $false; Line = 'no CTest summary in the transcript'; Exit = $Exit } }
  $ok = ($Exit -eq 0) -and ([int]$m.Groups[2].Value -eq 0) -and ([int]$m.Groups[3].Value -gt 0)
  [pscustomobject]@{ Ok = $ok; Line = $m.Value; Exit = $Exit }
}

function Get-TreeLitter {
  <#
  .SYNOPSIS
    What appeared in a worktree that nothing should have written: a new entry at its root other than
    the build and derived-data directories, or a new untracked file anywhere that git does not ignore.
    Sunday 2026-11-22's gate left a file named `$null` at the gate worktree's root; this names one.
  #>
  param([Parameter(Mandatory)][string]$Root, [string[]]$Before = @(), [string[]]$Allowed = @('build', 'ddc'))
  $now = Get-TreeEntries -Root $Root
  , @($now | Where-Object { $_ -notin $Before -and ($_ -split '/')[0] -notin $Allowed })
}

function Get-TreeEntries {
  # The root's own entries, and every untracked path git reports (not the ignored ones). A git that
  # fails throws: an empty list from a failed status would blind the litter check (T217-L7; a
  # module's functions run under the session's $ErrorActionPreference, not gate.ps1's Stop).
  param([Parameter(Mandatory)][string]$Root)
  $names = @(Get-ChildItem -LiteralPath $Root -Force | ForEach-Object { $_.Name })
  $st = @(& git -C $Root status --porcelain --untracked-files=all 2>$null)
  if ($LASTEXITCODE -ne 0) { throw "Get-TreeEntries: git status failed in $Root (exit $LASTEXITCODE); the litter check cannot see the tree" }
  $untracked = @($st | Where-Object { $_ -like '?? *' } | ForEach-Object { $_.Substring(3).Trim('"') })
  , @(@($names) + @($untracked) | Sort-Object -Unique)
}

# The steps a verdict holds a state for (T220), each 'green', 'red', 'inconclusive', 'skipped' (by
# the caller's -Skip) or 'not run' (not earned: the suites were not green). The core steps are what
# a follow-up can never run again and must find green; the earned ones are what it completes.
$script:CoreSteps = @('checks', 'builds', 'suites', 'integrity', 'tree')
$script:EarnedSteps = @('listeners', 'budgets', 'weaker-card', 'linux')

function Get-GateSteps { [pscustomobject]@{ Core = $script:CoreSteps; Earned = $script:EarnedSteps } }

function Get-BudgetStepState {
  <#
  .SYNOPSIS
    The frame budgets' step from tools/frame-budget.ps1's exit codes (T220): green, red or
    inconclusive, never a pass read from a retry of a failure.
  .DESCRIPTION
    First is the whole flight's exit; Busy the runs its report.json holds as over on a busy machine;
    Again the exit of those runs flown once more by themselves (-Run), or $null when they were not.
      0                     green
      1, 3, 5, anything else red: over on a quiet machine, a failed run, not the budget's machine, a
                            summary that is not a native measurement. No second flight changes it.
      4  (only busy overs)  the busy runs are flown again alone: 0 green, 4 or 6 inconclusive, else red.
                            A 4 whose report names no busy run is red: the code and the report disagree.
      6  (compared with nothing: no budget for the session state) inconclusive; when it also had busy
                            runs they are flown again, and a 1 there is red.
  #>
  param([Parameter(Mandatory)][int]$First, [string[]]$Busy = @(), $Again = $null)
  if ($First -eq 0) { return 'green' }
  if ($First -notin 4, 6) { return 'red' }
  if ($First -eq 4 -and @($Busy).Count -eq 0) { return 'red' }
  if ($null -eq $Again) { return 'inconclusive' }
  $again = [int]$Again
  if ($again -eq 0) { return $(if ($First -eq 4) { 'green' } else { 'inconclusive' }) }
  if ($again -in 4, 6) { return 'inconclusive' }
  'red'
}

function Read-BudgetBusyRuns {
  <#
  .SYNOPSIS
    The ids of the runs a frame-budget report.json holds as over on a busy machine (Verdict 'busy'),
    or none when the report is missing or unreadable.
  #>
  param([Parameter(Mandatory)][string]$Path)
  if (-not (Test-Path -LiteralPath $Path)) { return @() }
  try { $r = Get-Content -LiteralPath $Path -Raw -ErrorAction Stop | ConvertFrom-Json -ErrorAction Stop } catch { return @() }
  @(@($r) | Where-Object { $_ -and $_.PSObject.Properties['Verdict'] -and $_.Verdict -eq 'busy' } | ForEach-Object { "$($_.Id)" })
}

function Get-FoldPlan {
  <#
  .SYNOPSIS
    Whether an earlier verdict can be completed by a follow-up at this tip, and which steps the
    follow-up runs (T220; docs/ci/gate.md, "A follow-up that completes a verdict").
  .DESCRIPTION
    Refusal is a sentence when it cannot: a verdict that records no step states, one for another
    commit, one whose tree changed or gained litter while it ran, one that holds any step red or whose
    own red or no_verdict list is not empty (a red step is a verdict: no follow-up turns it green), or one whose core steps (checks, builds, suites,
    integrity, tree) are not all green. Otherwise Run is every earned step (listeners, budgets,
    weaker-card, linux) it holds inconclusive, not run or skipped, less those this run -Skips, and
    a plan with nothing to run is refused too. Sources is the earlier verdict's own per-step sources
    when it was itself folded, else every step it holds named to it.
  #>
  param([Parameter(Mandatory)]$Earlier, [Parameter(Mandatory)][string]$EarlierPath, [Parameter(Mandatory)][string]$Tip, [string[]]$Skip = @())
  $no = { param($why) [pscustomobject]@{ Refusal = $why; Run = @(); States = $null; Sources = $null } }
  $has = { param($o, $n) $o -and $o.PSObject.Properties[$n] }
  if (-not (& $has $Earlier 'states') -or -not $Earlier.states) { return & $no "$EarlierPath records no step states (a verdict from before T220), so there is nothing to fold it with" }
  if ("$($Earlier.tip)" -ne $Tip) { return & $no "$EarlierPath is for $($Earlier.tip) and the tree is at ${Tip}: nothing folds across two HEADs" }
  $dirty = @(@($Earlier.tree_changed) + @($Earlier.litter) | Where-Object { $_ })
  if ($dirty) { return & $no "$EarlierPath ran on a tree that changed or gained litter ($($dirty -join '; ')): nothing folds across a dirty tree" }
  $states = [ordered]@{}
  foreach ($s in $script:CoreSteps + $script:EarnedSteps) {
    $states[$s] = if (& $has $Earlier.states $s) { "$($Earlier.states.$s)" } else { 'missing' }
  }
  $red = @($states.Keys | Where-Object { $states[$_] -eq 'red' })
  if ($red) { return & $no "$EarlierPath holds $($red -join ', ') red: a red step is a verdict, and no follow-up turns it green" }
  # Its own red and no-verdict lists are read as well as its states (T220-L2): every red today maps to
  # a state, but a red added later without one must still refuse the fold rather than slip past it.
  $ownRed = @(if (& $has $Earlier 'red') { @($Earlier.red) | Where-Object { $_ } })
  if ($ownRed) { return & $no "$EarlierPath is red ($($ownRed -join '; ')): a red verdict is a verdict, and no follow-up turns it green" }
  $noVerdict = @(if (& $has $Earlier 'no_verdict') { @($Earlier.no_verdict) | Where-Object { $_ } })
  if ($noVerdict) { return & $no "$EarlierPath holds no verdict for $($noVerdict -join ', '): a leg that left none is red, and no follow-up turns it green" }
  $core = @($script:CoreSteps | Where-Object { $states[$_] -ne 'green' })
  if ($core) { return & $no "$EarlierPath holds $(@($core | ForEach-Object { "$_ $($states[$_])" }) -join ', '): a follow-up runs no check, build or suite, so it completes only a verdict whose are green" }
  $run = @($script:EarnedSteps | Where-Object { $states[$_] -in 'inconclusive', 'not run', 'skipped', 'missing' -and $_ -notin $Skip })
  if (-not $run) { return & $no "$EarlierPath holds nothing this follow-up would run (its states: $(@($script:EarnedSteps | ForEach-Object { "$_ $($states[$_])" }) -join ', '))" }
  $sources = [ordered]@{}
  $own = @([ordered]@{ verdict = $EarlierPath; batch = "$($Earlier.batch)"; started = "$($Earlier.started)"; ended = "$($Earlier.ended)" })
  foreach ($s in $states.Keys) {
    # Assigned whole, never through an `if` expression, which would unroll a list of one.
    $sources[$s] = $own
    if ((& $has $Earlier 'sources') -and $Earlier.sources -and (& $has $Earlier.sources $s)) { $sources[$s] = @($Earlier.sources.$s) }
  }
  [pscustomobject]@{ Refusal = $null; Run = $run; States = $states; Sources = $sources }
}

function Get-OpenStepsText {
  <#
  .SYNOPSIS
    The steps of a verdict's states that are neither green nor skipped, grouped by state
    ("inconclusive: budgets; not run: listeners"), or '' when there are none (T220-L1): what an exit 4
    and after-gate's refusal name, so neither names nothing.
  #>
  param($States)
  if (-not $States) { return '' }
  $pairs = if ($States -is [System.Collections.IDictionary]) { @($States.Keys | ForEach-Object { , @("$_", "$($States[$_])") }) }
           else { @($States.PSObject.Properties | ForEach-Object { , @($_.Name, "$($_.Value)") }) }
  $by = [ordered]@{}
  foreach ($p in $pairs) {
    if ($p[1] -in 'green', 'skipped') { continue }
    if (-not $by.Contains($p[1])) { $by[$p[1]] = New-Object System.Collections.Generic.List[string] }
    $by[$p[1]].Add($p[0])
  }
  @($by.Keys | ForEach-Object { "${_}: $($by[$_] -join ', ')" }) -join '; '
}

function Start-GateProcess {
  <#
  .SYNOPSIS
    A pwsh script as a process of its own, its whole stdout and stderr in two files: a leg's verdict
    is read from what it wrote, never from a redirect in this process that caught one line (b71's
    weaker-card leg: dev.ps1 sends CTest's lines to the host, so `*>` here kept dev.ps1's one line).
    -NoWait returns the process at once (the suites of a wave, the Linux leg); -Detached does too,
    and the process shares neither this one's console nor any handle of it, so it outlives it and
    holds nothing a reader of this process's output waits on (the server leg; T231). On Linux a
    detached leg is started as -NoWait is.
    Otherwise this waits and returns the exit code. Every path it is given is absolute: nothing a
    gate starts writes where its working directory happens to be.
  #>
  param([Parameter(Mandatory)][string]$Script, [string[]]$Arguments = @(), [Parameter(Mandatory)][string]$Log,
        [Parameter(Mandatory)][string]$WorkingDirectory, [switch]$NoWait, [switch]$Detached)
  foreach ($p in $Script, $Log, $WorkingDirectory) {
    if (-not [IO.Path]::IsPathRooted($p)) { throw "Start-GateProcess: '$p' is not an absolute path" }
  }
  $pwsh = (Get-Process -Id $PID).Path
  $line = ConvertTo-CommandLine (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Script) + $Arguments)
  $sp = @{ FilePath = $pwsh; ArgumentList = $line; WorkingDirectory = $WorkingDirectory; PassThru = $true
           RedirectStandardOutput = $Log; RedirectStandardError = "$Log.err" }
  if ($Detached -and $IsWindows) {
    # **A detached leg inherits no handle of this process** (T231). A redirect makes Start-Process
    # create the child with handle inheritance on (CreateProcess's bInheritHandles, which .NET sets
    # whenever a stream is redirected), so the leg and everything it starts held every inheritable
    # handle this process had, the launching shell's redirected stdout among them, and a shell
    # reading that redirection waited three hours past a verdict already written (batch 75). A
    # hidden window does not change it, and neither does -NoNewWindow. Start-Process with no
    # redirect and no -NoNewWindow goes through ShellExecuteEx, which creates the process with
    # inheritance off; that process, a pwsh of its own with a hidden console, then starts the leg
    # with the two log files and waits for it, so the leg inherits only its handles, and the id
    # returned is alive exactly as long as the leg, whose exit code it exits with.
    $q = { param($v) "'" + ("$v" -replace "'", "''") + "'" }
    $inner = "`$p = Start-Process -FilePath $(& $q $pwsh) -ArgumentList $(& $q $line) -WorkingDirectory $(& $q $WorkingDirectory) " +
             "-RedirectStandardOutput $(& $q $Log) -RedirectStandardError $(& $q "$Log.err") -NoNewWindow -PassThru; " +
             "`$null = `$p.Handle; `$p.WaitForExit(); exit `$p.ExitCode"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($inner))
    $sp = @{ FilePath = $pwsh; ArgumentList = "-NoProfile -ExecutionPolicy Bypass -EncodedCommand $encoded"
             WorkingDirectory = $WorkingDirectory; PassThru = $true; WindowStyle = 'Hidden' }
  } else { $sp.NoNewWindow = $true }
  $proc = Start-Process @sp
  $null = $proc.Handle   # without the handle taken now, ExitCode reads empty once the process is gone
  if ($Detached -or $NoWait) { return $proc }
  $proc.WaitForExit()
  $proc.ExitCode
}

Export-ModuleMember -Function Get-GateFilters, Get-WeakerCardFilter, ConvertTo-CommandLine, Read-CTestLog,
  Test-NoCaseByDesign, Get-SuiteState, Get-RerunPlan, Test-RunExplained, Get-IntegrityFindings, Read-ContainerVerdict,
  Read-LegVerdict, Get-WorktreeDirt, Get-TreeLitter, Get-TreeEntries, Start-GateProcess, Get-GateSteps, Get-BudgetStepState,
  Read-BudgetBusyRuns, Get-FoldPlan, Get-OpenStepsText

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
    $status = if ($blk -match '(?m)^Test Failed\.') { 'failed' }
              elseif ($blk -match '(?m)^Test Passed\.') { if ($gaveUp -and -not $summary) { 'gave-up' } else { 'passed' } }
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
    Whether one `tools/dev.ps1 test` run's exit code is what its own kept logs say: 0 or 75 is (a 75's
    give-ups are judged by the suite's state), anything else only when a log of that run holds a
    failed or cut-off record. A run that exited 1 having kept nothing, or 8 over all-passed records,
    is a fault even when a later rerun passed: the code and the log disagree.
  #>
  param([int]$Exit, [string[]]$Logs = @())
  if ($Exit -in 0, 75) { return $true }
  $own = Get-SuiteState -Logs $Logs
  [bool](@($own.Failed).Count + @($own.Incomplete).Count)
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
  # The root's own entries, and every untracked path git reports (not the ignored ones).
  param([Parameter(Mandatory)][string]$Root)
  $names = @(Get-ChildItem -LiteralPath $Root -Force | ForEach-Object { $_.Name })
  $untracked = @(& git -C $Root status --porcelain --untracked-files=all 2>$null | Where-Object { $_ -like '?? *' } | ForEach-Object { $_.Substring(3).Trim('"') })
  , @(@($names) + @($untracked) | Sort-Object -Unique)
}

function Start-GateProcess {
  <#
  .SYNOPSIS
    A pwsh script as a process of its own, its whole stdout and stderr in two files: a leg's verdict
    is read from what it wrote, never from a redirect in this process that caught one line (b71's
    weaker-card leg: dev.ps1 sends CTest's lines to the host, so `*>` here kept dev.ps1's one line).
    -NoWait returns the process at once (the suites of a wave, the Linux leg); -Detached does too,
    and the process does not share this one's console, so it outlives it (the server leg).
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
  # A detached leg outlives this process, so it does not share its console; a hidden one is no window.
  if ($Detached -and $IsWindows) { $sp.WindowStyle = 'Hidden' } else { $sp.NoNewWindow = $true }
  $proc = Start-Process @sp
  $null = $proc.Handle   # without the handle taken now, ExitCode reads empty once the process is gone
  if ($Detached -or $NoWait) { return $proc }
  $proc.WaitForExit()
  $proc.ExitCode
}

Export-ModuleMember -Function Get-GateFilters, Get-WeakerCardFilter, ConvertTo-CommandLine, Read-CTestLog,
  Test-NoCaseByDesign, Get-SuiteState, Get-RerunPlan, Test-RunExplained, Get-IntegrityFindings, Read-ContainerVerdict,
  Get-TreeLitter, Get-TreeEntries, Start-GateProcess

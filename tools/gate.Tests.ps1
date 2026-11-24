#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/gate.ps1, tools/after-gate.ps1 and tools/lib/Gate.psm1. Runs under CTest as two
  tests: `tools.gate` (-Part unit: the module's functions and the refusals that start one process,
  in every configuration) and `tools.gate_scenarios` (-Part scenarios: the real scripts over a
  throwaway repository, registered in msvc-debug only, since the scripts are the same in every
  configuration and the scenarios are most of the time).

.DESCRIPTION
  Nothing here builds, tests, starts a container, opens a device, pushes to a real remote or installs
  anything. A throwaway repository in a fresh directory under the system temp directory stands in for
  the gate worktree, and its `tools/` holds stand-ins for every command the gate runs (`dev.ps1`,
  `linux-build.ps1`, `remote-build.ps1`, `ci/check-no-listeners.ps1`, `frame-budget.ps1`,
  `export-game-sdk.ps1`, `structure-check.ps1`): one small script that does what a scenario file
  says (a test's records in a kept CTest log, an exit code, a build that fails some attempts, a line
  on stdout that may lie, a stray write) and writes every call to a calls log. The real
  tools/gate.ps1 is then run over it with -Root, and its exit code, its markers and its verdict file
  are held to the rules (docs/ci/gate.md):
    - a test that gave up waiting for the card is run once more alone, and the verdict names it;
    - a suite whose only failure is engine_editor gets that test alone once, and no other suite does;
    - the summary fails when a leg left no verdict;
    - a suite's verdict is read from its kept CTest log, not from a console line, and an exit code
      its own logs do not explain (1 over passed records, 75 with no give-up) is red;
    - the integrity read: a record that gave up needs a later one with a verdict, and a record of no
      doctest case is a fault except the three that are so by design;
    - the waves (three suites together, then release), the follow-up's -Reach and -Skip, the
      listener check and the budgets only after green suites and red when they fail, the weaker
      card red when it fails, the server leg never holding the verdict, and a file left at the gate
      worktree's root (Sunday's `$null`) or a tracked file changed during the run failing the gate;
    - every build attempt in the verdict, a pass on a retry named, a build failing every attempt red;
    - a gate worktree that is not clean (a tracked change, an untracked file, an unfinished merge) is
      refused, and the last verdict of the batch is set aside first;
    - after-gate: a fast-forward and never a merge commit, binaries copied (the copy cleared first)
      and installed, a file in use reported and left, the SDK exported outside the gate worktree,
      the fold committed, no push without -Push nor after a failed step (the push case pushes to a
      bare repository in the same scratch directory), and refusals of a red or stale verdict, a dirty
      gate worktree, a main that is not on main and a main the gated tip does not descend from.

      pwsh tools/gate.Tests.ps1 [-Part all|unit|scenarios] [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp, [ValidateSet('all', 'unit', 'scenarios')][string]$Part = 'all')

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/Gate.psm1') -Force
$gateScript = Join-Path $PSScriptRoot 'gate.ps1'
$afterScript = Join-Path $PSScriptRoot 'after-gate.ps1'
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
$base = Join-Path ([IO.Path]::GetTempPath()) "engine-gate-$([guid]::NewGuid().ToString('N'))"
$clock = [Diagnostics.Stopwatch]::StartNew()

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { $ok = $false; Write-Host "       ($($_.Exception.Message))" }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

function Invoke-TGit([string]$dir, [string[]]$gitArgs) {
  $o = & git -C $dir -c user.name=t -c user.email=t@example.com -c commit.gpgsign=false @gitArgs 2>&1
  if ($LASTEXITCODE -ne 0) { throw "git $gitArgs failed: $o" }
  return ($o | Out-String).Trim()
}

function Write-Text([string]$path, [string]$text) {
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
  [IO.File]::WriteAllText($path, $text.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding($false)))
}

# A kept CTest log in CTest's own layout. A record is "name:status": passed, failed, gaveup (exit 75:
# the footer still says Passed, as CTest writes it), nocase (a doctest summary of zero cases), plain
# (a passing test that prints no doctest summary), incomplete (cut off before its footer).
$recordWriter = @'
function Format-CTestLog([string[]]$records) {
  $out = New-Object System.Text.StringBuilder
  [void]$out.AppendLine('Start testing: Nov 22 14:26 Eastern Standard Time')
  [void]$out.AppendLine('----------------------------------------------------------')
  $i = 0
  foreach ($r in $records) {
    $i++
    $name, $status = $r -split ':', 2
    [void]$out.AppendLine("$i/$($records.Count) Testing: $name")
    [void]$out.AppendLine("$i/$($records.Count) Test: $name")
    [void]$out.AppendLine('Command: "stand-in"')
    [void]$out.AppendLine("`"$name`" start time: Nov 22 14:26 Eastern Standard Time")
    [void]$out.AppendLine('Output:')
    [void]$out.AppendLine('----------------------------------------------------------')
    switch ($status) {
      { $_ -in 'passed', 'regex-passed' } { [void]$out.AppendLine('[doctest] test cases:    2 |    2 passed | 0 failed | 0 skipped') }
      { $_ -in 'failed', 'regex-failed' } { [void]$out.AppendLine('[doctest] test cases:    2 |    1 passed | 1 failed | 0 skipped') }
      'nocase'     { [void]$out.AppendLine('[doctest] test cases:    0 |    0 passed | 0 failed | 4 skipped') }
      'gaveup'     { [void]$out.AppendLine('gpu-lock: gave up waiting for the GPU lock after 1800 s (exit 75)') }
      'plain'      { [void]$out.AppendLine('stand-in: 3 checks passed') }
      'incomplete' { [void]$out.AppendLine('[doctest] running') }
    }
    if ($status -eq 'incomplete') { break }
    [void]$out.AppendLine('<end of output>')
    [void]$out.AppendLine('Test time =   1.00 sec')
    [void]$out.AppendLine('----------------------------------------------------------')
    # A test judged by a regular expression (PASS_REGULAR_EXPRESSION) has a footer of its own: the
    # reason on two lines, and neither "Test Passed." nor "Test Failed.".
    switch ($status) {
      'failed'       { [void]$out.AppendLine('Test Failed.') }
      'regex-passed' { [void]$out.AppendLine('Test Pass Reason:'); [void]$out.AppendLine('Required regular expression found. Regex=[test cases: +[0-9]+ \| +([2-9]|[1-9][0-9]+) passed \| +0 failed]') }
      'regex-failed' { [void]$out.AppendLine('Test Fail Reason:'); [void]$out.AppendLine('Required regular expression not found. Regex=[test cases: +[0-9]+ \| +([2-9]|[1-9][0-9]+) passed \| +0 failed]') }
      default        { [void]$out.AppendLine('Test Passed.') }
    }
    [void]$out.AppendLine("`"$name`" end time: Nov 22 14:27 Eastern Standard Time")
    [void]$out.AppendLine('----------------------------------------------------------')
  }
  $out.ToString()
}
'@
. ([scriptblock]::Create($recordWriter))

# The one stand-in, copied under every command's name; what it does is the scenario's.
$standIn = $recordWriter + @'

$ErrorActionPreference = 'Stop'
$me = [IO.Path]::GetFileNameWithoutExtension($PSCommandPath)
$sc = Get-Content -LiteralPath $env:GATE_STANDIN_SCENARIO -Raw | ConvertFrom-Json -AsHashtable
$state = $env:GATE_STANDIN_STATE
$root = Split-Path -Parent $PSScriptRoot
if ($me -eq 'check-no-listeners') { $root = Split-Path -Parent $root }
# One file a line: the legs run side by side, and two appends to one file at once can lose one.
function Log([string]$what) { Set-Content -LiteralPath (Join-Path $state "call-$([guid]::NewGuid().ToString('N')).log") ("{0} {1}" -f (Get-Date).ToString('HH:mm:ss.fff'), $what) }
function Next([string]$key) {
  $n = Join-Path $state "$key.count"
  $i = if (Test-Path -LiteralPath $n) { [int](Get-Content -LiteralPath $n) } else { 0 }
  Set-Content -LiteralPath $n ($i + 1); $i
}
$preset = ''; $filter = ''
for ($i = 0; $i -lt $args.Count; $i++) {
  if ($args[$i] -eq '-Preset') { $preset = $args[$i + 1] }
  if ($args[$i] -eq '-Filter') { $filter = $args[$i + 1] }
}
Log "$me $($args -join ' ') | device=$env:ENGINE_GFX_TEST_DEVICE"
switch ($me) {
  'dev' {
    $cmd = $args[0]
    if ($cmd -in 'docs', 'lint') {
      if ($sc.ContainsKey("$cmd-writes")) { foreach ($w in @($sc["$cmd-writes"])) { Set-Content -LiteralPath (Join-Path $root $w) 'stray' } }
      exit $(if ($sc.ContainsKey($cmd)) { $sc[$cmd] } else { 0 })
    }
    if ($cmd -eq 'configure') { New-Item -ItemType Directory -Force -Path (Join-Path $root "build/$preset") | Out-Null; Set-Content -LiteralPath (Join-Path $root "build/$preset/build.ninja") ''; exit 0 }
    if ($cmd -eq 'build') {
      # 'build-fails': { preset: n } fails the first n attempts of that preset's build.
      $k = Next "build-$preset"
      $fails = if ($sc.ContainsKey('build-fails') -and $sc['build-fails'].ContainsKey($preset)) { [int]$sc['build-fails'][$preset] } else { 0 }
      if ($k -lt $fails) { Log "build-failed $preset $k"; exit 1 }
      exit 0
    }
    if ($cmd -eq 'test') {
      $k = Next "test-$preset"
      $runs = @($sc.test[$preset])
      $run = if ($k -lt $runs.Count) { $runs[$k] } else { @{ records = @('extra:passed') } }
      Log "test-start $preset $k"
      if ($run.ContainsKey('barrier')) {
        # The wave's barrier: this run ends only once every suite named has started, so a wave run
        # one after another, not side by side, shows as a barrier-timeout and not as a slow clock.
        Set-Content -LiteralPath (Join-Path $state "started-$preset") ''
        $deadline = (Get-Date).AddSeconds(60)
        while (@($run.barrier | Where-Object { -not (Test-Path -LiteralPath (Join-Path $state "started-$_")) }).Count) {
          if ((Get-Date) -gt $deadline) { Log "barrier-timeout $preset"; break }
          Start-Sleep -Milliseconds 50
        }
      }
      if (-not $run.ContainsKey('nolog')) {
        $dir = Join-Path $root "build/$preset/Testing/Temporary"
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $n = Next "log-$preset"
        Set-Content -LiteralPath (Join-Path $dir ('LastTest-20261122-{0:000000}-filter.log' -f $n)) (Format-CTestLog @($run.records)) -NoNewline
      }
      if ($run.ContainsKey('stdout')) { $run.stdout }
      Log "test-end $preset $k"
      $code = if ($run.ContainsKey('exit')) { $run.exit } elseif (@($run.records | Where-Object { $_ -like '*:failed' })) { 8 } elseif (@($run.records | Where-Object { $_ -like '*:gaveup' })) { 75 } else { 0 }
      exit $code
    }
    exit 0
  }
  { $_ -in 'linux-build', 'remote-build' } {
    $code = if ($sc.linux -and $sc.linux.ContainsKey($preset)) { $sc.linux[$preset] } else { 0 }
    if ($sc.ContainsKey('linux-silent') -and $sc['linux-silent'] -eq $preset) { exit 0 }
    if ($code -eq 0) { '100% tests passed, 0 tests failed out of 3' } else { '67% tests passed, 1 tests failed out of 3' }
    exit $code
  }
  'check-no-listeners' { exit $(if ($sc.ContainsKey('listeners')) { $sc.listeners } else { 0 }) }
  'frame-budget' {
    # 'budgets': an exit code for every flight, or a list of { exit; busy = [run ids] }, one a flight
    # in order. Each flight writes report.json in its -Out as the real one does: two runs, r1 and r2,
    # those named busy over on a busy machine and the rest within budget.
    $out = ''; for ($i = 0; $i -lt $args.Count; $i++) { if ($args[$i] -eq '-Out') { $out = $args[$i + 1] } }
    $k = Next 'budgets'
    $b = if ($sc.ContainsKey('budgets')) { $sc.budgets } else { 0 }
    $flight = if ($b -is [System.Collections.IList]) { if ($k -lt $b.Count) { $b[$k] } else { @{ exit = 0 } } } else { @{ exit = [int]$b } }
    $busy = if ($flight.ContainsKey('busy')) { @($flight.busy) } else { @() }
    if ($out) {
      New-Item -ItemType Directory -Force -Path $out | Out-Null
      $report = @('r1', 'r2' | ForEach-Object { [ordered]@{ Id = $_; Verdict = $(if ($_ -in $busy) { 'busy' } else { 'pass' }) } })
      Set-Content -LiteralPath (Join-Path $out 'report.json') (ConvertTo-Json -InputObject $report -Depth 4)
    }
    exit $flight.exit
  }
  'export-game-sdk' {
    $out = ''; for ($i = 0; $i -lt $args.Count; $i++) { if ($args[$i] -eq '-Out') { $out = $args[$i + 1] } }
    New-Item -ItemType Directory -Force -Path $out | Out-Null; Set-Content -LiteralPath (Join-Path $out 'sdk.json') '{}'
    # 'sdk-writes': a relative write, which lands in whatever directory the export was started in.
    if ($sc.ContainsKey('sdk-writes')) { Set-Content -LiteralPath (Join-Path (Get-Location).Path $sc['sdk-writes']) 'stray' }
    exit $(if ($sc.ContainsKey('sdk')) { $sc.sdk } else { 0 })
  }
  'structure-check' {
    Get-ChildItem -LiteralPath (Join-Path $root 'tools/structure-raises') -File -Filter '*.json' | Remove-Item
    Set-Content -LiteralPath (Join-Path $root 'tools/structure-ceilings.json') '{"folded":true}'; exit 0
  }
}
exit 0
'@

function New-GateRepo([string]$dir) {
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  Invoke-TGit $dir @('init', '-q', '-b', 'main') | Out-Null
  Write-Text (Join-Path $dir '.gitignore') "/build/`n"
  Write-Text (Join-Path $dir 'README.md') 'stand-in tree'
  foreach ($n in 'tools/dev.ps1', 'tools/linux-build.ps1', 'tools/remote-build.ps1', 'tools/ci/check-no-listeners.ps1',
                 'tools/frame-budget.ps1', 'tools/export-game-sdk.ps1', 'tools/structure-check.ps1') {
    Write-Text (Join-Path $dir $n) $standIn
  }
  Write-Text (Join-Path $dir 'tools/structure-ceilings.json') '{}'
  Invoke-TGit $dir @('add', '.') | Out-Null
  Invoke-TGit $dir @('commit', '-q', '-m', 'stand-in tree') | Out-Null
}

function Set-Scenario([string]$name, [hashtable]$scenario) {
  $state = Join-Path $base "state-$name"
  New-Item -ItemType Directory -Force -Path $state | Out-Null
  $scenarioFile = Join-Path $state 'scenario.json'
  Write-Text $scenarioFile ($scenario | ConvertTo-Json -Depth 8)
  $env:GATE_STANDIN_SCENARIO = $scenarioFile
  $env:GATE_STANDIN_STATE = $state
  $state
}

function Invoke-Gate([string]$name, [hashtable]$scenario, [string[]]$extra = @()) {
  $state = Set-Scenario $name $scenario
  # Each scenario's suites keep logs of their own: the last scenario's are not this one's.
  foreach ($d in 'msvc-debug', 'msvc-minimal', 'msvc-no-ecs', 'msvc-release') { Remove-Item -LiteralPath (Join-Path $script:repo "build/$d/Testing") -Recurse -Force -ErrorAction SilentlyContinue }
  $logs = Join-Path $base "logs-$name"
  $out = (& pwsh -NoProfile -File $gateScript -Root $script:repo -Batch $name -Logs $logs -MemoryWaitMinutes 0 @extra *>&1 | Out-String)
  $code = $LASTEXITCODE
  $vf = Join-Path $logs "$name-gate.verdict.json"
  [pscustomobject]@{
    Code = $code; Out = $out; Logs = $logs
    Verdict = $(if (Test-Path -LiteralPath $vf) { Get-Content -LiteralPath $vf -Raw | ConvertFrom-Json } else { $null })
    Calls = @(Get-ChildItem -LiteralPath $state -Filter 'call-*.log' | ForEach-Object { (Get-Content -LiteralPath $_.FullName -Raw).Trim() } | Sort-Object)
  }
}

function Invoke-GateRaw([string]$batchName, [string]$logs, [string[]]$extra = @()) {
  # A run whose refusal comes before any step: its code and its words.
  $out = (& pwsh -NoProfile -File $gateScript -Root $script:repo -Batch $batchName -Logs $logs -MemoryWaitMinutes 0 @extra *>&1 | Out-String)
  [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}

function Get-CallTime([object[]]$calls, [string]$pattern, [switch]$Last) {
  $hits = @($calls | Where-Object { $_ -match $pattern } | ForEach-Object { [datetime]::ParseExact(($_ -split ' ')[0], 'HH:mm:ss.fff', $null) })
  if (-not $hits) { return $null }
  if ($Last) { ($hits | Sort-Object)[-1] } else { ($hits | Sort-Object)[0] }
}

$suiteGreen = @{
  'msvc-debug'   = @(@{ records = @('a:passed') })
  'msvc-minimal' = @(@{ records = @('b:passed') })
  'msvc-no-ecs'  = @(@{ records = @('c:passed') })
  'msvc-release' = @(@{ records = @('engine_view.1:passed') })
}

try {
  Write-Host "gate tests ($Part)"
  New-Item -ItemType Directory -Force -Path $base | Out-Null

  if ($Part -in 'all', 'unit') {
  # ---- the module's pure parts ------------------------------------------------------------------
  $fx = Join-Path $base 'fixtures'
  New-Item -ItemType Directory -Force -Path $fx | Out-Null
  $logA = Join-Path $fx 'a.log'
  Write-Text $logA (Format-CTestLog @('t.pass:passed', 't.fail:failed', 't.gave:gaveup', 't.none:nocase', 't.plain:plain', 't.cut:incomplete'))
  $recs = Read-CTestLog -Path $logA
  Test-That 'a kept log reads record by record, and a skipped test''s "Test Passed." footer reads as gave-up' {
    ($recs | ForEach-Object { "$($_.Name)=$($_.Status)" }) -join ',' -eq 't.pass=passed,t.fail=failed,t.gave=gave-up,t.none=passed,t.plain=passed,t.cut=incomplete'
  }
  $logRegex = Join-Path $fx 'regex.log'
  Write-Text $logRegex (Format-CTestLog @('g.pass:regex-passed', 'g.fail:regex-failed', 'g.cut:incomplete'))
  Test-That 'a test judged by a regular expression reads by its reason footer, passed or failed, and is not cut off' {
    $state = Get-SuiteState -Logs @($logRegex)
    $read = Read-CTestLog -Path $logRegex   # the function returns its array whole: name it, then pipe
    (($read | ForEach-Object { "$($_.Name)=$($_.Status)" }) -join ',') -eq 'g.pass=passed,g.fail=failed,g.cut=incomplete' -and
      ($state.Failed -join ',') -eq 'g.fail' -and ($state.Incomplete -join ',') -eq 'g.cut'
  }
  Test-That 'the three records of no case by design, and no others' {
    (Test-NoCaseByDesign 'engine_cli.npc_replay' 'msvc-minimal') -and (Test-NoCaseByDesign 'engine_cli.npc_replay' 'msvc-no-ecs') -and
      (Test-NoCaseByDesign 'engine_view.frame_loop' 'msvc-minimal') -and -not (Test-NoCaseByDesign 'engine_view.frame_loop' 'msvc-no-ecs') -and
      -not (Test-NoCaseByDesign 'engine_cli.npc_replay' 'msvc-debug') -and -not (Test-NoCaseByDesign 'renderer.1' 'msvc-minimal')
  }
  $logB = Join-Path $fx 'b.log'
  Write-Text $logB (Format-CTestLog @('t.gave:passed'))
  Test-That 'integrity: a give-up answered by a later record is not a fault, an unanswered one is, and so is a cut-off record' {
    $one = Get-IntegrityFindings -Logs @(@{ Preset = 'msvc-debug'; Path = $logA }, @{ Preset = 'msvc-debug'; Path = $logB })
    $two = Get-IntegrityFindings -Logs @(@{ Preset = 'msvc-debug'; Path = $logA }, @{ Preset = 'msvc-release'; Path = $logB })
    # one: t.none (no case) and t.cut; two: those and t.gave, whose later record is another preset's.
    $one.Faults -eq 2 -and $two.Faults -eq 3 -and ($one.Lines -match '^GAVE-UP \(answered later\)') -and ($two.Lines -match '^GAVE-UP a\.log t\.gave')
  }
  Test-That 'a suite''s rerun plan: every give-up, and engine_editor only as the only failure' {
    $l1 = Join-Path $fx 'p1.log'; Write-Text $l1 (Format-CTestLog @('x:gaveup', 'engine_editor:failed', 'y:passed'))
    $l2 = Join-Path $fx 'p2.log'; Write-Text $l2 (Format-CTestLog @('engine_editor:failed', 'z:failed'))
    ((Get-RerunPlan -State (Get-SuiteState -Logs @($l1))) -join ',') -eq 'x,engine_editor' -and
      @(Get-RerunPlan -State (Get-SuiteState -Logs @($l2))).Count -eq 0
  }
  $passedLog = Join-Path $fx 'passed.log'; Write-Text $passedLog (Format-CTestLog @('p:passed', 'q:plain'))
  $gaveLog = Join-Path $fx 'gave.log'; Write-Text $gaveLog (Format-CTestLog @('p:passed', 'g:gaveup'))
  $failLog = Join-Path $fx 'fail.log'; Write-Text $failLog (Format-CTestLog @('p:passed', 'f:failed'))
  Test-That 'an exit code is explained only by its own run''s logs: 75 needs a give-up, 1 or 8 a failed or cut-off record' {
    (Test-RunExplained -Exit 0 -Logs @($passedLog)) -and (Test-RunExplained -Exit 75 -Logs @($gaveLog)) -and (Test-RunExplained -Exit 8 -Logs @($failLog)) -and
      -not (Test-RunExplained -Exit 75 -Logs @($passedLog)) -and -not (Test-RunExplained -Exit 75 -Logs @()) -and
      -not (Test-RunExplained -Exit 1 -Logs @($passedLog)) -and -not (Test-RunExplained -Exit 1 -Logs @())
  }
  Test-That 'a container is green only on exit 0 with CTest''s own summary of no failure out of more than none' {
    (Read-ContainerVerdict -Text "x`n100% tests passed, 0 tests failed out of 184`n" -Exit 0).Ok -and
      -not (Read-ContainerVerdict -Text '100% tests passed, 0 tests failed out of 184' -Exit 1).Ok -and
      -not (Read-ContainerVerdict -Text '' -Exit 0).Ok -and
      -not (Read-ContainerVerdict -Text '0% tests passed, 0 tests failed out of 0' -Exit 0).Ok
  }
  $legDir = Join-Path $fx 'legs'
  $leg = { param($name, $text) $p = Join-Path $legDir $name; if ($null -ne $text) { Write-Text $p $text }; Read-LegVerdict -Path $p -Expected 3 }
  $three = '{"leg":"linux","done":true,"presets":{"a":{"ok":true,"exit":0,"line":"x"},"b":{"ok":false,"exit":1,"line":"y"},"c":{"ok":true,"exit":0,"line":"z"}}}'
  Test-That 'the Linux leg: no verdict when it died before finishing its file, wrote half a file, or reported fewer than three containers' {
    $missing = & $leg 'missing.json' $null
    $died = & $leg 'died.json' '{"leg":"linux","done":false,"presets":{"a":{"ok":true,"exit":0,"line":"x"}}}'
    $torn = & $leg 'torn.json' '{"leg":"linux","done":tr'
    $two = & $leg 'two.json' '{"leg":"linux","done":true,"presets":{"a":{"ok":true,"exit":0,"line":"x"},"b":{"ok":true,"exit":0,"line":"y"}}}'
    $whole = & $leg 'three.json' $three
    $missing.NoVerdict -and $died.NoVerdict -match 'did not finish' -and $torn.NoVerdict -and $two.NoVerdict -match 'reported 2 containers' -and
      -not $whole.NoVerdict -and (@($whole.Red) -join ',') -eq 'linux b' -and @($whole.Lines).Count -eq 3
  }
  Test-That 'a git that fails is a fault, never a clean tree or an empty list of untracked files' {
    $broken = Join-Path $fx 'broken'
    Write-Text (Join-Path $broken '.git') "gitdir: $(Join-Path $fx 'no-such-gitdir')"
    $t1 = $false; try { [void](Get-TreeEntries -Root $broken) } catch { $t1 = $_.Exception.Message -match 'git status failed' }
    $t2 = $false; try { [void](Get-WorktreeDirt -Root $broken) } catch { $t2 = $_.Exception.Message -match 'git status failed' }
    $t1 -and $t2
  }
  Test-That 'a follow-up''s reach anchors the three wave-one suites, and release keeps the apps'' set' {
    $f = Get-GateFilters -Reach 'world|docs_check'
    $f['msvc-debug'] -eq '^(world|docs_check)$' -and $f['msvc-no-ecs'] -eq '^(world|docs_check)$' -and
      $f['msvc-release'] -eq '^engine_(view|editor|host|cli|content)(\.([0-9]+|gpu))?$' -and (Get-GateFilters)['msvc-minimal'] -eq ''
  }
  $echo = Join-Path $fx 'echo.ps1'
  Write-Text $echo '$args | ConvertTo-Json -Compress'
  Test-That 'arguments reach a leg whole: spaces, quotes, a trailing backslash and an empty one' {
    $want = @('a b', 'say "hi"', 'C:\dir with space\', '', '^(x|y)$')
    $log = Join-Path $fx 'echo out.log'
    $code = Start-GateProcess -Script $echo -Arguments $want -Log $log -WorkingDirectory $fx
    $got = @(Get-Content -LiteralPath $log -Raw | ConvertFrom-Json)
    $code -eq 0 -and $got.Count -eq $want.Count -and -not @(0..($want.Count - 1) | Where-Object { $got[$_] -ne $want[$_] })
  }
  Test-That 'a leg is never started with a relative path' {
    $threw = $false
    try { [void](Start-GateProcess -Script 'echo.ps1' -Log (Join-Path $fx 'x.log') -WorkingDirectory $fx) } catch { $threw = $true }
    $threw
  }
  Test-That 'an unknown -Skip name is refused with exit 2' {
    $o = (& pwsh -NoProfile -File $gateScript -Root $fx -Batch x -Logs (Join-Path $base 'skip-logs') -Skip 'linux,budget' *>&1 | Out-String)
    $LASTEXITCODE -eq 2 -and $o -match 'not budget' -and -not (Test-Path -LiteralPath (Join-Path $base 'skip-logs'))
  }
  # ---- T220: the budgets' three states and the follow-up's plan ---------------------------------
  Test-That 'the budgets: 0 green; a quiet over (1) or any other failure red, whatever a second flight says' {
    (Get-BudgetStepState -First 0) -eq 'green' -and (Get-BudgetStepState -First 1) -eq 'red' -and (Get-BudgetStepState -First 3) -eq 'red' -and
      (Get-BudgetStepState -First 5) -eq 'red' -and (Get-BudgetStepState -First 1 -Busy @('r1') -Again 0) -eq 'red'
  }
  Test-That 'the budgets: a busy over flown again alone is green only if it passes then, inconclusive if busy again, red if over quietly' {
    (Get-BudgetStepState -First 4 -Busy @('r1') -Again 0) -eq 'green' -and (Get-BudgetStepState -First 4 -Busy @('r1') -Again 4) -eq 'inconclusive' -and
      (Get-BudgetStepState -First 4 -Busy @('r1') -Again 1) -eq 'red' -and (Get-BudgetStepState -First 4 -Busy @('r1')) -eq 'inconclusive' -and
      (Get-BudgetStepState -First 4) -eq 'red' -and (Get-BudgetStepState -First 6) -eq 'inconclusive' -and
      (Get-BudgetStepState -First 6 -Busy @('r1') -Again 0) -eq 'inconclusive' -and (Get-BudgetStepState -First 6 -Busy @('r1') -Again 1) -eq 'red'
  }
  Test-That 'the busy runs are read from a flight''s report.json, one run or many, and none from a missing or torn file' {
    $one = Join-Path $fx 'report-one.json'; Write-Text $one '{"Id":"r1","Verdict":"busy"}'
    $many = Join-Path $fx 'report-many.json'; Write-Text $many '[{"Id":"r1","Verdict":"pass"},{"Id":"r2","Verdict":"busy"},{"Id":"r3","Verdict":"busy"}]'
    $torn = Join-Path $fx 'report-torn.json'; Write-Text $torn '[{"Id":"r1","Verd'
    ((Read-BudgetBusyRuns -Path $one) -join ',') -eq 'r1' -and ((Read-BudgetBusyRuns -Path $many) -join ',') -eq 'r2,r3' -and
      @(Read-BudgetBusyRuns -Path $torn).Count -eq 0 -and @(Read-BudgetBusyRuns -Path (Join-Path $fx 'no-report.json')).Count -eq 0
  }
  $foldBase = @{ batch = 'b1'; tip = 'abc'; started = 's'; ended = 'e'; tree_changed = @(); litter = @()
                 states = [ordered]@{ checks = 'green'; builds = 'green'; suites = 'green'; integrity = 'green'; tree = 'green'
                                      listeners = 'green'; budgets = 'inconclusive'; 'weaker-card' = 'skipped'; linux = 'green' } }
  $asVerdict = { param($changes) $h = $foldBase.Clone(); $h.states = [ordered]@{}
    foreach ($k in $foldBase.states.Keys) { $h.states[$k] = $foldBase.states[$k] }
    foreach ($k in $changes.Keys) { if ($k -like 'states.*') { $h.states[$k.Substring(7)] = $changes[$k] } else { $h[$k] = $changes[$k] } }
    $h | ConvertTo-Json -Depth 6 | ConvertFrom-Json }
  Test-That 'a follow-up''s plan: the inconclusive and skipped earned steps, less its own -Skip, each kept step named to its verdict' {
    $p = Get-FoldPlan -Earlier (& $asVerdict @{}) -EarlierPath 'v1.json' -Tip 'abc'
    $q = Get-FoldPlan -Earlier (& $asVerdict @{}) -EarlierPath 'v1.json' -Tip 'abc' -Skip @('weaker-card')
    -not $p.Refusal -and ($p.Run -join ',') -eq 'budgets,weaker-card' -and ($q.Run -join ',') -eq 'budgets' -and
      $p.Sources['suites'][0].verdict -eq 'v1.json' -and $p.Sources['suites'][0].batch -eq 'b1'
  }
  Test-That 'a follow-up is refused across two HEADs, over a dirty tree, over any red step, over core steps not green, and with nothing to run' {
    $why = { param($changes, $tip = 'abc') (Get-FoldPlan -Earlier (& $asVerdict $changes) -EarlierPath 'v1.json' -Tip $tip).Refusal }
    (& $why @{} 'def') -match 'two HEADs' -and (& $why @{ tree_changed = @('README.md') }) -match 'dirty tree' -and
      (& $why @{ litter = @('$null') }) -match 'dirty tree' -and (& $why @{ 'states.budgets' = 'red' }) -match 'budgets red.*no follow-up turns it green' -and
      (& $why @{ 'states.suites' = 'not run' }) -match 'suites not run' -and
      (& $why @{ 'states.budgets' = 'green'; 'states.weaker-card' = 'green' }) -match 'nothing this follow-up would run' -and
      (Get-FoldPlan -Earlier ([pscustomobject]@{ tip = 'abc'; green = $false }) -EarlierPath 'old.json' -Tip 'abc').Refusal -match 'records no step states'
  }
  Test-That 'a follow-up is refused over an earlier verdict whose own red or no-verdict list is not empty, whatever its states say (T220-L2)' {
    $why = { param($changes) (Get-FoldPlan -Earlier (& $asVerdict $changes) -EarlierPath 'v1.json' -Tip 'abc').Refusal }
    (& $why @{ red = @('something new') }) -match 'v1\.json is red \(something new\)' -and
      (& $why @{ no_verdict = @('linux') }) -match 'no verdict for linux' -and -not (& $why @{ red = @(); no_verdict = @() })
  }
  Test-That 'the steps an exit 4 or a refusal names: every state but green and skipped, grouped by state, from either form of the states' {
    $d = [ordered]@{ checks = 'green'; budgets = 'inconclusive'; linux = 'skipped'; listeners = 'not run'; 'weaker-card' = 'inconclusive' }
    $o = [pscustomobject]$d
    (Get-OpenStepsText $d) -eq 'inconclusive: budgets, weaker-card; not run: listeners' -and (Get-OpenStepsText $o) -eq (Get-OpenStepsText $d) -and
      (Get-OpenStepsText ([ordered]@{ checks = 'green'; linux = 'skipped' })) -eq ''
  }
  Test-That '-Completes takes no -Reach' {
    $o = (& pwsh -NoProfile -File $gateScript -Root $fx -Batch x -Logs (Join-Path $base 'completes-logs') -Completes 'v.json' -Reach 'x' *>&1 | Out-String)
    $LASTEXITCODE -eq 2 -and $o -match 'takes no -Reach' -and -not (Test-Path -LiteralPath (Join-Path $base 'completes-logs'))
  }
  Write-Host ("unit part: {0:N1} s" -f $clock.Elapsed.TotalSeconds)
  }

  if ($Part -in 'all', 'scenarios') {
  $scenarioClock = [Diagnostics.Stopwatch]::StartNew()
  # ---- a batch's gate, green, with each second look ----------------------------------------------
  $repo = Join-Path $base 'repo'
  New-GateRepo $repo
  $wave = @('msvc-debug', 'msvc-minimal', 'msvc-no-ecs')
  $green = @{
    test = @{
      'msvc-debug'   = @(@{ barrier = $wave; records = @('a:passed', 'engine_view.3:gaveup') }, @{ records = @('engine_view.3:passed') },
                         @{ records = @('renderer.1:passed', 'water:passed') })
      'msvc-minimal' = @(@{ barrier = $wave; records = @('engine_cli.npc_replay:nocase', 'engine_view.frame_loop:nocase', 'b:passed', 'tools.lint:plain') })
      'msvc-no-ecs'  = @(@{ barrier = $wave; records = @('engine_cli.npc_replay:nocase', 'engine_editor:failed', 'c:passed') }, @{ records = @('engine_editor:passed') })
      'msvc-release' = @(@{ records = @('engine_view.1:passed', 'engine_cli.gpu:passed') })
    }
  }
  $g = Invoke-Gate 'batchA' $green @('-Server', 'stand-in-host')
  Test-That 'a green batch exits 0 and its verdict file says green at the tip' {
    $g.Code -eq 0 -and $g.Verdict.green -and $g.Verdict.tip -eq (Invoke-TGit $repo @('rev-parse', 'HEAD')) -and $g.Out -match 'GATE batchA exit 0'
  }
  Test-That 'each build is in the verdict with its one attempt and its log' {
    $b = $g.Verdict.steps.builds.'msvc-debug'
    $b.ok -and -not $b.passed_on_retry -and @($b.attempts).Count -eq 1 -and $b.attempts[0].exit -eq 0 -and (Test-Path -LiteralPath $b.attempts[0].log) -and
      @($g.Verdict.retried_builds).Count -eq 0
  }
  Test-That 'a test that gave up on the card is run once more, alone, and the verdict names it' {
    $g.Out -match 'RERUN msvc-debug engine_view\.3: passed' -and ($g.Calls -match 'dev test -Preset msvc-debug -Filter \^engine_view\\\.3\$').Count -eq 1 -and
      $g.Verdict.suites.'msvc-debug'.reruns -contains 'engine_view.3: passed'
  }
  Test-That 'a suite whose only failure is engine_editor gets that test alone once' {
    $g.Out -match 'RERUN msvc-no-ecs engine_editor: passed' -and ($g.Calls -match 'dev test -Preset msvc-no-ecs -Filter \^engine_editor\$').Count -eq 1
  }
  Test-That 'the integrity read: the give-up is answered, the by-design records of no case are named and are no fault' {
    $g.Verdict.integrity.faults -eq 0 -and ($g.Out -match 'GAVE-UP \(answered later\)') -and
      ($g.Out -match 'NO-CASE \(by design\) .*msvc-minimal.* engine_view\.frame_loop') -and ($g.Out -match 'NO-CASE \(by design\) .*msvc-no-ecs.* engine_cli\.npc_replay')
  }
  Test-That 'the waves: debug, minimal and no-ecs side by side (each waited at a barrier for the other two), then release once they have all ended' {
    # The barrier is the proof of side by side: a wave run one suite at a time would leave its first
    # suite at the barrier until the deadline and log barrier-timeout. The order after it is the
    # gate's own sequencing (release starts only after every wave-one process has exited).
    $lastEnd = Get-CallTime $g.Calls 'test-end msvc-(debug|minimal|no-ecs) [01]$' -Last   # the suites and their second looks; the weaker card comes later
    $release = Get-CallTime $g.Calls 'test-start msvc-release 0$'
    ($g.Calls -match 'barrier-timeout').Count -eq 0 -and $release -gt $lastEnd -and
      @(Get-ChildItem -LiteralPath (Join-Path $base 'state-batchA') -Filter 'started-*').Count -eq 3
  }
  Test-That 'the off-configurations run with -SkipInvariantGpu, every suite with -Jobs 3, release with the apps'' set' {
    ($g.Calls -match 'dev test -Preset msvc-minimal -Jobs 3 -SkipInvariantGpu').Count -eq 1 -and
      ($g.Calls -match 'dev test -Preset msvc-debug -Jobs 3 \|').Count -eq 1 -and
      ($g.Calls -match [regex]::Escape('dev test -Preset msvc-release -Filter ^engine_(view|editor|host|cli|content)(\.([0-9]+|gpu))?$ -Jobs 3')).Count -eq 1
  }
  Test-That 'green suites earn the listener check, the budgets and the weaker card''s set as that card' {
    ($g.Calls -match 'check-no-listeners -Preset msvc-release').Count -eq 1 -and ($g.Calls -match 'frame-budget -Preset msvc-release -WaitQuiet 120').Count -eq 1 -and
      ($g.Calls -match [regex]::Escape('dev test -Preset msvc-debug -Filter ^(renderer\.[0-9]+|water|engine_cli\.gpu)$ -Jobs 3 | device=titanxp')).Count -eq 1
  }
  Test-That 'the three containers ran, each judged from its transcript' {
    @('linux-gcc-release', 'linux-clang-debug', 'linux-clang-minimal' | Where-Object { $g.Out -match "LINUX $_ exit 0: 100% tests passed" }).Count -eq 3
  }
  $serverPid = if ($g.Out -match 'server leg started detached \((\d+)\)') { [int]$Matches[1] } else { 0 }
  if ($serverPid) { Wait-Process -Id $serverPid -Timeout 120 -ErrorAction SilentlyContinue }
  Test-That 'the server leg ran detached, wrote a verdict of its own and is not part of the gate''s' {
    $sv = Get-Content -LiteralPath (Join-Path $g.Logs 'batchA-server.verdict.json') -Raw | ConvertFrom-Json
    $serverPid -gt 0 -and $sv.done -and $sv.presets.'linux-server'.ok -and ($g.Calls -match 'remote-build -Host stand-in-host -Preset linux-server -Test -Fetch').Count -eq 1 -and
      -not ($g.Verdict.red)
  }
  Test-That 'every log the gate keeps is outside the gate worktree, and the worktree gained no stray file and changed nothing' {
    @($g.Verdict.litter).Count -eq 0 -and @($g.Verdict.tree_changed).Count -eq 0 -and (Test-Path -LiteralPath (Join-Path $g.Logs 'batchA-msvc-debug-suite.log')) -and
      @(Get-ChildItem -LiteralPath $g.Logs -Filter 'batchA-msvc-debug-*LastTest-*.log').Count -eq 3
  }

  # ---- verdicts that cannot lie ------------------------------------------------------------------
  $red = @{
    test = @{
      'msvc-debug'   = @(@{ records = @('a:passed', 'engine_view.1:gaveup') }, @{ records = @('engine_view.1:gaveup') })
      'msvc-minimal' = @(@{ records = @('b:passed', 'b2:failed'); exit = 0; stdout = '100% tests passed, 0 tests failed out of 2' })
      'msvc-no-ecs'  = @(@{ records = @('engine_editor:failed', 'd:failed') })
      'msvc-release' = @(@{ records = @('engine_view.1:nocase') })
    }
    'linux-silent' = 'linux-clang-debug'
  }
  $r = Invoke-Gate 'batchB' $red
  Test-That 'a red gate exits 1 and its verdict file says so' { $r.Code -eq 1 -and -not $r.Verdict.green -and $r.Out -match 'GATE batchB exit 1' }
  Test-That 'a test that gives up twice is a red suite named for it, and an unanswered give-up is an integrity fault' {
    $r.Out -match 'SUITE msvc-debug FAILED \(gave up on the card twice: engine_view\.1\)' -and ($r.Out -match 'GAVE-UP .*engine_view\.1: no later record')
  }
  Test-That 'a suite is judged by its kept log: a console that says "100% tests passed" with exit 0 over a failed record is red' {
    $r.Out -match 'SUITE msvc-minimal FAILED \(failed: b2\)'
  }
  Test-That 'engine_editor beside another failure is not run again' {
    ($r.Calls -match 'dev test -Preset msvc-no-ecs -Filter').Count -eq 0 -and $r.Out -match 'SUITE msvc-no-ecs FAILED \(failed: engine_editor, d\)'
  }
  Test-That 'a record of no case outside the three by design is an integrity fault' { $r.Out -match 'NO-CASE \S+msvc-release\S+ engine_view\.1' -and $r.Verdict.integrity.faults -ge 2 }
  Test-That 'a container that printed no CTest summary is not green, though it exited 0' { $r.Out -match 'LINUX linux-clang-debug exit 0: no CTest summary' -and ($r.Verdict.red -contains 'linux linux-clang-debug') }
  Test-That 'red suites earn no listener check, no budgets and no weaker card' {
    ($r.Calls -match 'check-no-listeners|frame-budget|device=titanxp').Count -eq 0 -and $r.Out -match 'not run: the suites are not all green'
  }
  Test-That 'the Linux leg, which runs beside red suites, records what it did: red here, never "not run" (T220-L3)' {
    $r.Verdict.states.linux -eq 'red' -and $r.Verdict.states.listeners -eq 'not run' -and $r.Verdict.states.budgets -eq 'not run'
  }

  # ---- exit codes the logs do not explain --------------------------------------------------------
  $codes = @{
    test = @{
      'msvc-debug'   = @(@{ records = @('a:passed'); exit = 1 })
      'msvc-minimal' = @(@{ records = @('b:passed', 'b3:plain'); exit = 75 })
      'msvc-no-ecs'  = $suiteGreen['msvc-no-ecs']
      'msvc-release' = $suiteGreen['msvc-release']
    }
  }
  $e = Invoke-Gate 'batchE' $codes @('-Skip', 'linux')
  Test-That 'an exit of 1 over passed records is red, and so is a 75 whose logs hold no give-up (a test that never ran is no pass)' {
    $e.Code -eq 1 -and $e.Out -match 'SUITE msvc-debug FAILED \(dev\.ps1 exited 1 where its own logs hold no record that explains it\)' -and
      $e.Out -match 'SUITE msvc-minimal FAILED \(dev\.ps1 exited 75 where' -and ($e.Calls -match 'dev test -Preset msvc-minimal -Filter').Count -eq 0 -and
      $e.Out -match 'SUITE msvc-no-ecs OK' -and ($e.Verdict.red -contains 'wave 1 msvc-minimal')
  }

  # ---- builds: a pass on a retry is named, a build that fails every attempt is red ---------------
  $b = Invoke-Gate 'batchF' @{ 'build-fails' = @{ 'msvc-debug' = 1; 'msvc-minimal' = 3 }; test = @{} } @('-Skip', 'linux')
  Test-That 'a build that passed on its second attempt is green and named as a retry, with both attempts and their logs in the verdict' {
    $d = $b.Verdict.steps.builds.'msvc-debug'
    $d.ok -and $d.passed_on_retry -and @($d.attempts).Count -eq 2 -and $d.attempts[0].exit -eq 1 -and $d.attempts[1].exit -eq 0 -and
      (Test-Path -LiteralPath $d.attempts[0].log) -and ($b.Verdict.retried_builds -contains 'msvc-debug') -and $b.Out -match 'BUILD msvc-debug OK on attempt 2'
  }
  Test-That 'a build that fails all three attempts is red, and no suite runs' {
    $m = $b.Verdict.steps.builds.'msvc-minimal'
    $b.Code -eq 1 -and -not $m.ok -and @($m.attempts).Count -eq 3 -and ($b.Verdict.red -contains 'build msvc-minimal') -and
      $b.Out -match 'SUITES not run' -and ($b.Calls -match 'dev test').Count -eq 0
  }

  # ---- what only green suites earn, red when it fails ---------------------------------------------
  $after = @{
    test = @{
      'msvc-debug'   = @(@{ records = @('a:passed') }, @{ records = @('renderer.1:failed', 'water:passed') })
      'msvc-minimal' = $suiteGreen['msvc-minimal']; 'msvc-no-ecs' = $suiteGreen['msvc-no-ecs']; 'msvc-release' = $suiteGreen['msvc-release']
    }
    listeners = 1; budgets = 3
  }
  $w = Invoke-Gate 'batchG' $after @('-Skip', 'linux')
  Test-That 'a listener check, budgets or weaker card that fails is red by name, and the suites stay green' {
    $w.Code -eq 1 -and ($w.Verdict.red -contains 'listeners') -and ($w.Verdict.red -contains 'budgets') -and ($w.Verdict.red -contains 'weaker card msvc-debug') -and
      -not $w.Verdict.weaker_card.ok -and $w.Verdict.suites.'msvc-debug'.ok -and $w.Out -match 'SUITE msvc-debug FAILED \(failed: renderer\.1\)'
  }

  # ---- T220: the budgets inconclusive, flown again alone, and a follow-up that completes them -----
  $busyGreen = @{ test = @{
      'msvc-debug' = @(@{ records = @('a:passed') }, @{ records = @('renderer.1:passed', 'water:passed') })
      'msvc-minimal' = $suiteGreen['msvc-minimal']; 'msvc-no-ecs' = $suiteGreen['msvc-no-ecs']; 'msvc-release' = $suiteGreen['msvc-release'] } }
  $h = Invoke-Gate 'batchH' ($busyGreen + @{ budgets = @(@{ exit = 4; busy = @('r2') }, @{ exit = 4; busy = @('r2') }) }) @('-Skip', 'linux', '-BudgetWaitQuiet', '300')
  Test-That 'budgets over only on a busy machine: the busy run is flown once more by itself, and busy again is inconclusive, neither red nor green (exit 4)' {
    $h.Code -eq 4 -and -not $h.Verdict.green -and @($h.Verdict.red).Count -eq 0 -and ($h.Verdict.inconclusive -join ',') -eq 'budgets' -and
      $h.Verdict.states.budgets -eq 'inconclusive' -and $h.Verdict.states.listeners -eq 'green' -and $h.Verdict.states.linux -eq 'skipped' -and
      ($h.Calls -match 'frame-budget -Preset msvc-release -WaitQuiet 300 -Out').Count -eq 1 -and
      ($h.Calls -match 'frame-budget -Preset msvc-release -Run r2 -WaitQuiet 300 -Out').Count -eq 1 -and @($h.Verdict.budgets.flights).Count -eq 2 -and
      $h.Out -match 'GATE batchH exit 4'
  }
  Test-That 'after-gate refuses an inconclusive verdict and names what is open' {
    $o = (& pwsh -NoProfile -File $afterScript -Gate $repo -Main $repo -Verdict (Join-Path $h.Logs 'batchH-gate.verdict.json') -CopyTo (Join-Path $base 'h-copy') *>&1 | Out-String)
    $LASTEXITCODE -eq 2 -and $o -match 'inconclusive: budgets'
  }
  $hVerdict = Join-Path $h.Logs 'batchH-gate.verdict.json'
  $i = Invoke-Gate 'batchI' @{ budgets = 0 } @('-Skip', 'linux', '-Completes', $hVerdict)
  Test-That 'a follow-up completes the verdict: only the budgets fly, and the folded verdict is green with each step''s source' {
    $i.Code -eq 0 -and $i.Verdict.green -and $i.Verdict.tip -eq $h.Verdict.tip -and $i.Verdict.completes -eq $hVerdict -and
      ($i.Calls -match 'frame-budget').Count -eq 1 -and ($i.Calls -match 'dev (docs|lint|test)|check-no-listeners|linux-build').Count -eq 0 -and
      ($i.Calls -match 'dev build -Preset msvc-release \| ').Count -eq 1 -and @($i.Calls -match 'dev build -Preset (?!msvc-release)').Count -eq 0 -and
      (Get-CallTime $i.Calls 'dev build -Preset msvc-release') -lt (Get-CallTime $i.Calls 'frame-budget') -and $i.Verdict.states.builds -eq 'green' -and
      (@($i.Verdict.sources.builds | ForEach-Object { $_.batch }) -join '+') -eq 'batchH+batchI' -and $i.Verdict.sources.linux[0].batch -eq 'batchI' -and
      $i.Verdict.sources.budgets[0].batch -eq 'batchI' -and $i.Verdict.sources.suites[0].batch -eq 'batchH' -and $i.Verdict.sources.listeners[0].batch -eq 'batchH' -and
      $i.Verdict.sources.suites[0].ended -eq $h.Verdict.ended -and $i.Verdict.states.linux -eq 'skipped' -and
      (@($i.Verdict.sources.tree | ForEach-Object { $_.batch }) -join '+') -eq 'batchH+batchI' -and $i.Verdict.suites.'msvc-debug'.ok
  }
  Test-That 'after-gate reads the folded verdict like any other, and says where each step came from' {
    Write-Text (Join-Path $repo 'build/msvc-release/bin/engine-view.exe') 'bytes'
    $mainI = Join-Path $base 'main-i'
    & git clone -q $repo $mainI 2>$null
    $o = (& pwsh -NoProfile -File $afterScript -Gate $repo -Main $mainI -Verdict (Join-Path $i.Logs 'batchI-gate.verdict.json') -CopyTo (Join-Path $base 'i-copy') -Install (Join-Path $base 'i-install') -Sdk (Join-Path $base 'i-sdk') *>&1 | Out-String)
    $LASTEXITCODE -eq 0 -and $o -match 'batchI completes .*budgets from batchI' -and $o -match 'MAIN already holds' -and (Test-Path -LiteralPath (Join-Path $base 'i-install/engine-view.exe'))
  }
  $j = Invoke-Gate 'batchJ' ($busyGreen + @{ budgets = @(@{ exit = 1 }) }) @('-Skip', 'linux')
  Test-That 'a budget over on a quiet machine is red, is not flown again, and no follow-up completes it' {
    $again = Invoke-GateRaw 'batchJ2' (Join-Path $base 'logs-batchJ2') @('-Completes', (Join-Path $j.Logs 'batchJ-gate.verdict.json'))
    $j.Code -eq 1 -and ($j.Verdict.red -contains 'budgets') -and $j.Verdict.states.budgets -eq 'red' -and ($j.Calls -match 'frame-budget').Count -eq 1 -and
      $again.Code -eq 2 -and $again.Out -match 'budgets red: a red step is a verdict'
  }
  $k = Invoke-Gate 'batchK' ($busyGreen + @{ budgets = @(@{ exit = 4; busy = @('r1') }, @{ exit = 0 }) }) @('-Skip', 'linux')
  Test-That 'a busy over that is within budget when flown again by itself is green, both flights in the verdict' {
    $k.Code -eq 0 -and $k.Verdict.green -and $k.Verdict.states.budgets -eq 'green' -and ($k.Calls -match 'frame-budget -Preset msvc-release -Run r1 ').Count -eq 1 -and
      (@($k.Verdict.budgets.flights | ForEach-Object { $_.exit }) -join ',') -eq '4,0'
  }
  Test-That 'a follow-up is refused for a verdict of another commit or of a tree that changed while it ran' {
    $hv = Get-Content -LiteralPath $hVerdict -Raw | ConvertFrom-Json
    $other = Join-Path $base 'other-tip.json'; $hv.tip = '0000000000000000000000000000000000000000'; Write-Text $other ($hv | ConvertTo-Json -Depth 8)
    $hv = Get-Content -LiteralPath $hVerdict -Raw | ConvertFrom-Json
    $dirt = Join-Path $base 'dirty-run.json'; $hv.tree_changed = @('README.md'); Write-Text $dirt ($hv | ConvertTo-Json -Depth 8)
    $a = Invoke-GateRaw 'batchL' (Join-Path $base 'logs-batchL') @('-Completes', $other)
    $b2 = Invoke-GateRaw 'batchL' (Join-Path $base 'logs-batchL') @('-Completes', $dirt)
    $a.Code -eq 2 -and $a.Out -match 'nothing folds across two HEADs' -and $b2.Code -eq 2 -and $b2.Out -match 'nothing folds across a dirty tree'
  }
  $i2 = Invoke-Gate 'batchI2' @{ budgets = 0; 'build-fails' = @{ 'msvc-release' = 3 } } @('-Skip', 'linux', '-Completes', $hVerdict)
  Test-That 'a follow-up whose release build fails is red, and flies nothing over what the build left (T220-L4)' {
    $i2.Code -eq 1 -and ($i2.Verdict.red -contains 'build msvc-release') -and $i2.Verdict.states.builds -eq 'red' -and
      $i2.Verdict.states.budgets -eq 'not run' -and ($i2.Calls -match 'frame-budget|check-no-listeners').Count -eq 0 -and
      ($i2.Calls -match 'dev build -Preset msvc-release').Count -eq 3 -and -not $i2.Verdict.steps.follow_up_build.ok
  }
  $m = Invoke-Gate 'batchM' @{ budgets = 1 } @('-Skip', 'linux', '-Completes', $hVerdict)
  Test-That 'a follow-up whose own budgets come back over on a quiet machine is red (exit 1), and after-gate refuses it (T220-L6)' {
    $o = (& pwsh -NoProfile -File $afterScript -Gate $repo -Main $repo -Verdict (Join-Path $m.Logs 'batchM-gate.verdict.json') -CopyTo (Join-Path $base 'm-copy') *>&1 | Out-String)
    $LASTEXITCODE -eq 2 -and $o -match 'not green \(red: budgets; steps red: budgets' -and -not (Test-Path -LiteralPath (Join-Path $base 'm-copy')) -and
      $m.Code -eq 1 -and -not $m.Verdict.green -and ($m.Verdict.red -contains 'budgets') -and $m.Verdict.states.budgets -eq 'red' -and
      $m.Verdict.sources.budgets[0].batch -eq 'batchM' -and ($m.Calls -match 'frame-budget').Count -eq 1
  }
  $n = Invoke-Gate 'batchN' @{} @('-Skip', 'budgets', '-Completes', $hVerdict)
  Test-That 'a follow-up that -Skips an inconclusive step keeps it inconclusive, never promoted, and says so in its exit 4 (T220-L1)' {
    $n.Code -eq 4 -and -not $n.Verdict.green -and $n.Verdict.states.budgets -eq 'inconclusive' -and $n.Verdict.sources.budgets[0].batch -eq 'batchH' -and
      $n.Verdict.states.linux -eq 'green' -and $n.Verdict.sources.linux[0].batch -eq 'batchN' -and ($n.Calls -match 'frame-budget|dev ').Count -eq 0 -and
      $n.Out -match 'GATE batchN exit 4 .*not green: inconclusive: budgets'
  }
  $hv = Get-Content -LiteralPath $hVerdict -Raw | ConvertFrom-Json
  $hv.states.PSObject.Properties.Remove('weaker-card')
  $missing = Join-Path $base 'missing-step.json'; Write-Text $missing ($hv | ConvertTo-Json -Depth 8)
  $o2 = Invoke-Gate 'batchO' @{ budgets = 0 } @('-Skip', 'linux,weaker-card', '-Completes', $missing)
  Test-That 'a step the earlier verdict lacks and the follow-up -Skips is recorded skipped, as a plain gate records it (T220-L1)' {
    $o2.Code -eq 0 -and $o2.Verdict.green -and $o2.Verdict.states.'weaker-card' -eq 'skipped' -and $o2.Verdict.sources.'weaker-card'[0].batch -eq 'batchO' -and
      $o2.Verdict.states.budgets -eq 'green' -and ($o2.Calls -match 'device=titanxp').Count -eq 0
  }
  $rv = Invoke-Gate 'batchR' $busyGreen @('-Reach', 'a', '-Skip', 'linux,listeners,budgets,weaker-card')
  $r2 = Invoke-Gate 'batchR2' @{ budgets = 0 } @('-Skip', 'linux,listeners,weaker-card', '-Completes', (Join-Path $rv.Logs 'batchR-gate.verdict.json'))
  Test-That 'a fold of a -Reach verdict records that reach, and says so as it starts (T220-M1)' {
    $rv.Code -eq 0 -and $rv.Verdict.reach -eq 'a' -and $r2.Code -eq 0 -and $r2.Verdict.reach -eq 'a' -and
      $r2.Out -match "its suites under -Reach 'a'" -and $r2.Verdict.states.listeners -eq 'skipped' -and $r2.Verdict.sources.listeners[0].batch -eq 'batchR2'
  }
  Test-That 'a -Completes naming the verdict this run would write is refused before anything is set aside (T220-L5)' {
    $self = Invoke-GateRaw 'batchH' $h.Logs @('-Completes', $hVerdict)
    $self.Code -eq 2 -and $self.Out -match 'names the verdict this run would write' -and $self.Out -match 'a -Batch of its own \(not batchH\)' -and
      (Test-Path -LiteralPath $hVerdict) -and @(Get-ChildItem -LiteralPath $h.Logs -Filter 'batchH-gate.verdict.previous-*').Count -eq 0
  }

  # ---- a follow-up, and a leg that leaves no verdict ---------------------------------------------
  $follow = @{
    test = @{
      'msvc-debug'   = @(@{ nolog = $true; exit = 0; stdout = '100% tests passed, 0 tests failed out of 4' })
      'msvc-minimal' = @(@{ records = @('world:passed') })
      'msvc-no-ecs'  = @(@{ records = @('world:passed') })
      'msvc-release' = @(@{ records = @('engine_view.1:passed') })
    }
  }
  $f = Invoke-Gate 'batchC' $follow @('-Reach', 'world|docs_check', '-Skip', 'linux,listeners,budgets,weaker-card')
  Test-That 'a follow-up runs wave one with its reach anchored, and nothing it skipped' {
    ($f.Calls -match [regex]::Escape('dev test -Preset msvc-no-ecs -Filter ^(world|docs_check)$ -Jobs 3 -SkipInvariantGpu')).Count -eq 1 -and
      ($f.Calls -match 'linux-build|check-no-listeners|frame-budget|remote-build').Count -eq 0
  }
  Test-That 'a suite that kept no CTest log has no verdict, and the summary fails for it whatever its console said' {
    $f.Code -eq 1 -and ($f.Verdict.no_verdict -contains 'suite msvc-debug') -and ($f.Verdict.red -contains 'no verdict: suite msvc-debug')
  }

  # ---- a failed check, a stray file at the root (Sunday's `$null`), a tracked file changed --------
  $d = Invoke-Gate 'batchD' @{ lint = 1; 'docs-writes' = @('$null', 'README.md'); test = @{} } @('-Skip', 'linux')
  Test-That 'a failed lint runs no suite and fails the gate' { $d.Code -eq 1 -and $d.Out -match 'LINT exit 1' -and $d.Out -match 'SUITES not run' -and ($d.Calls -match 'dev test').Count -eq 0 }
  Test-That 'a file a step left at the gate worktree''s root is named and fails the gate' { ($d.Verdict.litter -contains '$null') -and ($d.Verdict.red -contains 'litter') }
  Test-That 'a tracked file a step changed during the gate is named and fails the gate' {
    ($d.Verdict.red -contains 'tree changed') -and (@($d.Verdict.tree_changed) -match 'README\.md').Count -eq 1 -and $d.Out -match 'TREE CHANGED during the gate'
  }
  Remove-Item -LiteralPath (Join-Path $repo '$null') -Force -ErrorAction SilentlyContinue
  Invoke-TGit $repo @('checkout', '--', 'README.md') | Out-Null

  # ---- refusals: a log directory inside the worktree, a worktree that is not its HEAD -------------
  Test-That 'the gate refuses a log directory inside the gate worktree, and makes nothing there' {
    $o = Invoke-GateRaw 'x' (Join-Path $repo 'logs')
    $o.Code -eq 2 -and $o.Out -match 'inside the gate worktree' -and -not (Test-Path -LiteralPath (Join-Path $repo 'logs'))
  }
  $dirtyLogs = Join-Path $base 'logs-dirty'
  Write-Text (Join-Path $dirtyLogs 'dirty-gate.verdict.json') (@{ batch = 'dirty'; tip = 'x'; green = $true; red = @() } | ConvertTo-Json)
  Write-Text (Join-Path $repo 'README.md') 'changed by hand'
  $t = Invoke-GateRaw 'dirty' $dirtyLogs
  Invoke-TGit $repo @('checkout', '--', 'README.md') | Out-Null
  Test-That 'the gate refuses a worktree with a tracked change, and the last green verdict of the batch is set aside first' {
    $t.Code -eq 2 -and $t.Out -match 'not clean at its HEAD' -and $t.Out -match 'README\.md' -and
      -not (Test-Path -LiteralPath (Join-Path $dirtyLogs 'dirty-gate.verdict.json')) -and @(Get-ChildItem -LiteralPath $dirtyLogs -Filter 'dirty-gate.verdict.previous-*.json').Count -eq 1
  }
  Write-Text (Join-Path $repo 'stray.txt') 'left by someone'
  $u = Invoke-GateRaw 'dirty' $dirtyLogs
  Remove-Item -LiteralPath (Join-Path $repo 'stray.txt') -Force
  Test-That 'the gate refuses a worktree with an untracked file git does not ignore' { $u.Code -eq 2 -and $u.Out -match 'stray\.txt' }
  $mergeHead = Join-Path $repo (Invoke-TGit $repo @('rev-parse', '--git-path', 'MERGE_HEAD'))
  Write-Text $mergeHead (Invoke-TGit $repo @('rev-parse', 'HEAD'))
  $mh = Invoke-GateRaw 'dirty' $dirtyLogs
  Remove-Item -LiteralPath $mergeHead -Force
  Test-That 'the gate refuses a worktree with an unfinished merge' { $mh.Code -eq 2 -and $mh.Out -match 'MERGE_HEAD exists' }

  # ---- after the gate ----------------------------------------------------------------------------
  $afterState = Set-Scenario 'after' @{ 'sdk-writes' = 'sdk-stray.txt' }
  $origin = Join-Path $base 'origin.git'
  $main = Join-Path $base 'main'
  & git init -q --bare -b main $origin
  & git clone -q $origin $main 2>$null
  Invoke-TGit $main @('pull', '-q', $repo, 'main') | Out-Null
  Invoke-TGit $main @('push', '-q', 'origin', 'main') | Out-Null
  $gateTree = Join-Path $base 'gate'
  Invoke-TGit $main @('worktree', 'add', '-q', '-b', 'gate', $gateTree) | Out-Null
  Write-Text (Join-Path $gateTree 'tools/structure-raises/T1.json') '{"row":"T1"}'
  Write-Text (Join-Path $gateTree 'feature.txt') 'the batch'
  Invoke-TGit $gateTree @('add', '.') | Out-Null
  Invoke-TGit $gateTree @('commit', '-q', '-m', 'the batch') | Out-Null
  $gtip = Invoke-TGit $gateTree @('rev-parse', 'HEAD')
  $bin = Join-Path $gateTree 'build/msvc-release/bin'
  foreach ($n in 'engine-view.exe', 'engine-view.pdb', 'engine-cli.exe', 'schemac.exe', 'game/sandbox.dll') { Write-Text (Join-Path $bin $n) "bytes of $n" }
  Write-Text (Join-Path $gateTree 'build/msvc-release/shell/index.html') '<html></html>'
  $av = Join-Path $base 'after-logs'
  $verdictA = Join-Path $av 'bA-gate.verdict.json'
  Write-Text $verdictA (@{ batch = 'bA'; tip = $gtip; green = $true; red = @() } | ConvertTo-Json)
  $install = Join-Path $base 'install'
  $copy = Join-Path $base 'bin-main'
  $sdk = Join-Path $base 'sdk'
  Write-Text (Join-Path $install 'engine-cli.exe') 'the old one, open in a session'
  # A running session's executable: on Windows a file opened with no sharing cannot be replaced, which
  # is what a running engine-view is to an install; elsewhere a lock is advisory and the case is not made.
  $held = if ($IsWindows) { [IO.File]::Open((Join-Path $install 'engine-cli.exe'), 'Open', 'Read', 'None') } else { $null }
  try {
    $a = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict $verdictA -CopyTo $copy -Install $install -Sdk $sdk -Trailer 'Co-Authored-By: t' *>&1 | Out-String)
    $acode = $LASTEXITCODE
  } finally { if ($held) { $held.Dispose() } }
  $mainLog = @((Invoke-TGit $main @('log', '--format=%H %P %s', '-3')) -split "`n")
  Test-That 'after-gate fast-forwards main to the gated tip (no merge commit) and folds the raises in a commit of its own' {
    $mainLog[1] -like "$gtip *" -and (Invoke-TGit $main @('rev-list', '--merges', 'HEAD')) -eq '' -and $mainLog[0] -match "Fold bA's raises into the structure ceilings" -and
      -not (Test-Path -LiteralPath (Join-Path $main 'tools/structure-raises/T1.json')) -and (Invoke-TGit $main @('log', '-1', '--format=%B')) -match 'Co-Authored-By: t'
  }
  Test-That 'after-gate copies the release binaries out, installs what is not in use, and reports and leaves what is' {
    (Test-Path -LiteralPath (Join-Path $copy 'schemac.exe')) -and (Test-Path -LiteralPath (Join-Path $install 'engine-view.exe')) -and
      (Test-Path -LiteralPath (Join-Path $install 'game/sandbox.dll')) -and (Test-Path -LiteralPath (Join-Path $install 'shell/index.html')) -and
      -not (Test-Path -LiteralPath (Join-Path $install 'schemac.exe')) -and
      $(if ($IsWindows) { $a -match 'NOT INSTALLED engine-cli\.exe' -and (Get-Content -LiteralPath (Join-Path $install 'engine-cli.exe') -Raw) -match 'the old one' -and $acode -eq 1 }
        else { $acode -eq 0 })
  }
  Test-That 'after-gate exports the SDK in a directory of its own, never the gate worktree, and does not push without -Push' {
    (Test-Path -LiteralPath (Join-Path $sdk 'sdk.json')) -and $a -match 'PUSH not taken' -and (& git -C $origin rev-parse main) -ne $gtip -and
      -not (Test-Path -LiteralPath (Join-Path $gateTree 'sdk-stray.txt')) -and (Test-Path -LiteralPath (Join-Path $av 'bA-sdk-export.cwd/sdk-stray.txt')) -and
      $a -match 'SDK export left sdk-stray\.txt' -and @(Get-WorktreeDirt -Root $gateTree).Count -eq 0
  }
  Set-Scenario 'after2' @{} | Out-Null
  Test-That 'after-gate refuses a red verdict and a verdict for another commit, and does nothing' {
    Write-Text (Join-Path $av 'red-gate.verdict.json') (@{ batch = 'red'; tip = $gtip; green = $false; red = @('suite msvc-debug') } | ConvertTo-Json)
    Write-Text (Join-Path $av 'old-gate.verdict.json') (@{ batch = 'old'; tip = '0000000000000000000000000000000000000000'; green = $true; red = @() } | ConvertTo-Json)
    $before = Invoke-TGit $main @('rev-parse', 'HEAD')
    $o1 = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict (Join-Path $av 'red-gate.verdict.json') -CopyTo $copy -Install $install -Sdk $sdk *>&1 | Out-String); $c1 = $LASTEXITCODE
    $o2 = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict (Join-Path $av 'old-gate.verdict.json') -CopyTo $copy -Install $install -Sdk $sdk *>&1 | Out-String); $c2 = $LASTEXITCODE
    $c1 -eq 2 -and $o1 -match 'was not green' -and $c2 -eq 2 -and $o2 -match 'the verdict is for' -and (Invoke-TGit $main @('rev-parse', 'HEAD')) -eq $before
  }
  Test-That 'after-gate refuses a gate worktree that is not clean at the gated commit' {
    Write-Text (Join-Path $gateTree 'feature.txt') 'changed after the gate'
    $o = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict $verdictA -CopyTo $copy -Install $install -Sdk $sdk *>&1 | Out-String); $c = $LASTEXITCODE
    Invoke-TGit $gateTree @('checkout', '--', 'feature.txt') | Out-Null
    $c -eq 2 -and $o -match 'is not clean at its HEAD' -and $o -match 'feature\.txt'
  }
  Test-That 'with -Push, after-gate does not push when an earlier step failed (a -CopyTo it did not make is refused and left as it was)' {
    $foreign = Join-Path $base 'foreign'
    Write-Text (Join-Path $foreign 'keep.txt') 'someone else''s'
    $o = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict $verdictA -CopyTo $foreign -Install $install -Sdk $sdk -Push *>&1 | Out-String); $c = $LASTEXITCODE
    $c -eq 1 -and $o -match 'COPY failed: .*not a copy this script made' -and $o -match 'PUSH not taken: an earlier step' -and
      (& git -C $origin rev-parse main) -ne (Invoke-TGit $main @('rev-parse', 'HEAD')) -and (Test-Path -LiteralPath (Join-Path $foreign 'keep.txt')) -and
      @(Get-ChildItem -LiteralPath $foreign -Force).Count -eq 1
  }
  Test-That 'with -Push, after-gate clears its own last copy and pushes main (here to a bare repository in the scratch directory)' {
    Write-Text (Join-Path $copy 'stale-from-an-earlier-batch.txt') 'old'
    $o = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict $verdictA -CopyTo $copy -Install $install -Sdk $sdk -Push *>&1 | Out-String)
    $LASTEXITCODE -eq 0 -and $o -match 'PUSH main to origin' -and (& git -C $origin rev-parse main) -eq (Invoke-TGit $main @('rev-parse', 'HEAD')) -and
      -not (Test-Path -LiteralPath (Join-Path $copy 'stale-from-an-earlier-batch.txt')) -and (Test-Path -LiteralPath (Join-Path $copy 'schemac.exe'))
  }
  Test-That 'after-gate refuses a main checkout that is not on main' {
    Invoke-TGit $main @('switch', '-q', '-c', 'side') | Out-Null
    $o = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict $verdictA -CopyTo $copy -Install $install -Sdk $sdk *>&1 | Out-String); $c = $LASTEXITCODE
    Invoke-TGit $main @('switch', '-q', 'main') | Out-Null
    $c -eq 2 -and $o -match "is on 'side', not main"
  }
  Test-That 'after-gate refuses a gated tip that main cannot fast-forward to' {
    Write-Text (Join-Path $gateTree 'second.txt') 'the next batch'
    Invoke-TGit $gateTree @('add', '.') | Out-Null
    Invoke-TGit $gateTree @('commit', '-q', '-m', 'the next batch') | Out-Null
    $tip2 = Invoke-TGit $gateTree @('rev-parse', 'HEAD')
    Write-Text (Join-Path $main 'hand.txt') 'a commit on main the gate never saw'
    Invoke-TGit $main @('add', 'hand.txt') | Out-Null
    Invoke-TGit $main @('commit', '-q', '-m', 'by hand') | Out-Null
    $vb = Join-Path $av 'bB-gate.verdict.json'
    Write-Text $vb (@{ batch = 'bB'; tip = $tip2; green = $true; red = @() } | ConvertTo-Json)
    $before = Invoke-TGit $main @('rev-parse', 'HEAD')
    $o = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict $vb -CopyTo $copy -Install $install -Sdk $sdk *>&1 | Out-String); $c = $LASTEXITCODE
    $c -eq 2 -and $o -match 'main is not an ancestor' -and (Invoke-TGit $main @('rev-parse', 'HEAD')) -eq $before
  }
  [void]$afterState
  Write-Host ("scenarios part: {0:N1} s" -f $scenarioClock.Elapsed.TotalSeconds)
  }
}
finally {
  Remove-Item Env:GATE_STANDIN_SCENARIO, Env:GATE_STANDIN_STATE -ErrorAction SilentlyContinue
  if ($failures.Count -gt 0 -or $KeepTemp) { Write-Host "kept: $base" }
  else { Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host ''
if ($failures.Count -gt 0) { Write-Host "gate: $($failures.Count) of $checks checks failed" -ForegroundColor Red; exit 1 }
Write-Host ("gate: $checks checks passed ({0:N1} s)" -f $clock.Elapsed.TotalSeconds)
exit 0

#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/gate.ps1, tools/after-gate.ps1 and tools/lib/Gate.psm1. Runs under CTest as
  `tools.gate`.

.DESCRIPTION
  Nothing here builds, tests, starts a container, opens a device, pushes to a real remote or installs
  anything. A throwaway repository in a fresh directory under the system temp directory stands in for
  the gate worktree, and its `tools/` holds stand-ins for every command the gate runs (`dev.ps1`,
  `linux-build.ps1`, `remote-build.ps1`, `ci/check-no-listeners.ps1`, `frame-budget.ps1`,
  `export-game-sdk.ps1`, `structure-check.ps1`): one small script that does what a scenario file
  says (a test's records in a kept CTest log, an exit code, a line on stdout that may lie) and writes
  every call to a calls log. The real tools/gate.ps1 is then run over it with -Root, and its exit
  code, its markers and its verdict file are held to the rules (docs/ci/gate.md):
    - a test that gave up waiting for the card is run once more alone, and the verdict names it;
    - a suite whose only failure is engine_editor gets that test alone once, and no other suite does;
    - the summary fails when a leg left no verdict;
    - a suite's verdict is read from its kept CTest log, not from a console line;
    - the integrity read: a record that gave up needs a later one with a verdict, and a record of no
      doctest case is a fault except the three that are so by design;
    - the waves (three suites together, then release), the follow-up's -Reach and -Skip, the
      listener check and the budgets only after green suites, the server leg never holding the
      verdict, and a file left at the gate worktree's root (Sunday's `$null`) failing the gate;
    - after-gate: a fast-forward and never a merge commit, binaries copied and installed, a file in
      use reported and left, the SDK, the fold committed, and no push without -Push (the push case
      pushes to a bare repository in the same scratch directory).

      pwsh tools/gate.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/Gate.psm1') -Force
$gateScript = Join-Path $PSScriptRoot 'gate.ps1'
$afterScript = Join-Path $PSScriptRoot 'after-gate.ps1'
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
$base = Join-Path ([IO.Path]::GetTempPath()) "engine-gate-$([guid]::NewGuid().ToString('N'))"

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
      'passed'     { [void]$out.AppendLine('[doctest] test cases:    2 |    2 passed | 0 failed | 0 skipped') }
      'failed'     { [void]$out.AppendLine('[doctest] test cases:    2 |    1 passed | 1 failed | 0 skipped') }
      'nocase'     { [void]$out.AppendLine('[doctest] test cases:    0 |    0 passed | 0 failed | 4 skipped') }
      'gaveup'     { [void]$out.AppendLine('gpu-lock: gave up waiting for the GPU lock after 1800 s (exit 75)') }
      'plain'      { [void]$out.AppendLine('stand-in: 3 checks passed') }
      'incomplete' { [void]$out.AppendLine('[doctest] running') }
    }
    if ($status -eq 'incomplete') { break }
    [void]$out.AppendLine('<end of output>')
    [void]$out.AppendLine('Test time =   1.00 sec')
    [void]$out.AppendLine('----------------------------------------------------------')
    [void]$out.AppendLine($(if ($status -eq 'failed') { 'Test Failed.' } else { 'Test Passed.' }))
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
function Opt([string]$name) { $i = [array]::IndexOf($args, $name); if ($i -ge 0 -and $i + 1 -lt $args.Count) { $args[$i + 1] } else { '' } }
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
      if ($sc.ContainsKey("$cmd-writes")) { Set-Content -LiteralPath (Join-Path $root $sc["$cmd-writes"]) 'stray' }
      exit $(if ($sc.ContainsKey($cmd)) { $sc[$cmd] } else { 0 })
    }
    if ($cmd -eq 'configure') { New-Item -ItemType Directory -Force -Path (Join-Path $root "build/$preset") | Out-Null; Set-Content -LiteralPath (Join-Path $root "build/$preset/build.ninja") ''; exit 0 }
    if ($cmd -eq 'build') { exit 0 }
    if ($cmd -eq 'test') {
      $k = Next "test-$preset"
      $runs = @($sc.test[$preset])
      $run = if ($k -lt $runs.Count) { $runs[$k] } else { @{ records = @('extra:passed') } }
      Log "test-start $preset $k"
      if ($run.ContainsKey('sleep')) { Start-Sleep -Milliseconds $run.sleep }
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
  'frame-budget' { exit $(if ($sc.ContainsKey('budgets')) { $sc.budgets } else { 0 }) }
  'export-game-sdk' {
    $out = ''; for ($i = 0; $i -lt $args.Count; $i++) { if ($args[$i] -eq '-Out') { $out = $args[$i + 1] } }
    New-Item -ItemType Directory -Force -Path $out | Out-Null; Set-Content -LiteralPath (Join-Path $out 'sdk.json') '{}'; exit 0
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

function Invoke-Gate([string]$name, [hashtable]$scenario, [string[]]$extra = @()) {
  $state = Join-Path $base "state-$name"
  New-Item -ItemType Directory -Force -Path $state | Out-Null
  $scenarioFile = Join-Path $state 'scenario.json'
  Write-Text $scenarioFile ($scenario | ConvertTo-Json -Depth 8)
  $env:GATE_STANDIN_SCENARIO = $scenarioFile
  $env:GATE_STANDIN_STATE = $state
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

function Get-CallTime([object[]]$calls, [string]$pattern, [switch]$Last) {
  $hits = @($calls | Where-Object { $_ -match $pattern } | ForEach-Object { [datetime]::ParseExact(($_ -split ' ')[0], 'HH:mm:ss.fff', $null) })
  if (-not $hits) { return $null }
  if ($Last) { ($hits | Sort-Object)[-1] } else { ($hits | Sort-Object)[0] }
}

try {
  Write-Host 'gate tests'

  # ---- the module's pure parts ------------------------------------------------------------------
  $fx = Join-Path $base 'fixtures'
  New-Item -ItemType Directory -Force -Path $fx | Out-Null
  $logA = Join-Path $fx 'a.log'
  Write-Text $logA (Format-CTestLog @('t.pass:passed', 't.fail:failed', 't.gave:gaveup', 't.none:nocase', 't.plain:plain', 't.cut:incomplete'))
  $recs = Read-CTestLog -Path $logA
  Test-That 'a kept log reads record by record, and a skipped test''s "Test Passed." footer reads as gave-up' {
    ($recs | ForEach-Object { "$($_.Name)=$($_.Status)" }) -join ',' -eq 't.pass=passed,t.fail=failed,t.gave=gave-up,t.none=passed,t.plain=passed,t.cut=incomplete'
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
  Test-That 'a container is green only on exit 0 with CTest''s own summary of no failure out of more than none' {
    (Read-ContainerVerdict -Text "x`n100% tests passed, 0 tests failed out of 184`n" -Exit 0).Ok -and
      -not (Read-ContainerVerdict -Text '100% tests passed, 0 tests failed out of 184' -Exit 1).Ok -and
      -not (Read-ContainerVerdict -Text '' -Exit 0).Ok -and
      -not (Read-ContainerVerdict -Text '0% tests passed, 0 tests failed out of 0' -Exit 0).Ok
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

  # ---- a batch's gate, green, with each second look ----------------------------------------------
  $repo = Join-Path $base 'repo'
  New-GateRepo $repo
  $green = @{
    test = @{
      'msvc-debug'   = @(@{ sleep = 1500; records = @('a:passed', 'engine_view.3:gaveup') }, @{ records = @('engine_view.3:passed') },
                         @{ records = @('renderer.1:passed', 'water:passed') })
      'msvc-minimal' = @(@{ sleep = 1500; records = @('engine_cli.npc_replay:nocase', 'engine_view.frame_loop:nocase', 'b:passed', 'tools.lint:plain') })
      'msvc-no-ecs'  = @(@{ sleep = 1500; records = @('engine_cli.npc_replay:nocase', 'engine_editor:failed', 'c:passed') }, @{ records = @('engine_editor:passed') })
      'msvc-release' = @(@{ records = @('engine_view.1:passed', 'engine_cli.gpu:passed') })
    }
  }
  $g = Invoke-Gate 'batchA' $green @('-Server', 'stand-in-host')
  Test-That 'a green batch exits 0 and its verdict file says green at the tip' {
    $g.Code -eq 0 -and $g.Verdict.green -and $g.Verdict.tip -eq (Invoke-TGit $repo @('rev-parse', 'HEAD')) -and $g.Out -match 'GATE batchA exit 0'
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
  Test-That 'the waves: debug, minimal and no-ecs together, then release once they have all ended' {
    $starts = foreach ($p in 'msvc-debug', 'msvc-minimal', 'msvc-no-ecs') { Get-CallTime $g.Calls "test-start $p 0$" }
    $firstEnd = Get-CallTime $g.Calls 'test-end msvc-(debug|minimal|no-ecs) 0$'
    $lastEnd = Get-CallTime $g.Calls 'test-end msvc-(debug|minimal|no-ecs) [01]$' -Last   # the suites and their second looks; the weaker card comes later
    $release = Get-CallTime $g.Calls 'test-start msvc-release 0$'
    -not @($starts | Where-Object { $_ -gt $firstEnd }) -and $release -gt $lastEnd
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
  Test-That 'every log the gate keeps is outside the gate worktree, and the worktree gained no stray file' {
    @($g.Verdict.litter).Count -eq 0 -and (Test-Path -LiteralPath (Join-Path $g.Logs 'batchA-msvc-debug-suite.log')) -and
      @(Get-ChildItem -LiteralPath $g.Logs -Filter 'batchA-msvc-debug-*LastTest-*.log').Count -eq 3
  }

  # ---- verdicts that cannot lie ------------------------------------------------------------------
  foreach ($d in 'msvc-debug', 'msvc-minimal', 'msvc-no-ecs', 'msvc-release') { Remove-Item -LiteralPath (Join-Path $repo "build/$d/Testing") -Recurse -Force -ErrorAction SilentlyContinue }
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

  # ---- a follow-up, and a leg that leaves no verdict ---------------------------------------------
  foreach ($d in 'msvc-debug', 'msvc-minimal', 'msvc-no-ecs', 'msvc-release') { Remove-Item -LiteralPath (Join-Path $repo "build/$d/Testing") -Recurse -Force -ErrorAction SilentlyContinue }
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

  # ---- a failed check, and a stray file at the root (Sunday's `$null`) ---------------------------
  $d = Invoke-Gate 'batchD' @{ lint = 1; 'docs-writes' = '$null'; test = @{} } @('-Skip', 'linux')
  Test-That 'a failed lint runs no suite and fails the gate' { $d.Code -eq 1 -and $d.Out -match 'LINT exit 1' -and $d.Out -match 'SUITES not run' -and ($d.Calls -match 'dev test').Count -eq 0 }
  Test-That 'a file a step left at the gate worktree''s root is named and fails the gate' { ($d.Verdict.litter -contains '$null') -and ($d.Verdict.red -contains 'litter') }
  Remove-Item -LiteralPath (Join-Path $repo '$null') -Force -ErrorAction SilentlyContinue
  Test-That 'the gate refuses a log directory inside the gate worktree' {
    $o = (& pwsh -NoProfile -File $gateScript -Root $repo -Batch x -Logs (Join-Path $repo 'logs') -MemoryWaitMinutes 0 *>&1 | Out-String)
    $LASTEXITCODE -eq 2 -and $o -match 'inside the gate worktree'
  }
  Remove-Item -LiteralPath (Join-Path $repo 'logs') -Recurse -Force -ErrorAction SilentlyContinue

  # ---- after the gate ----------------------------------------------------------------------------
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
  Write-Text (Join-Path $av 'bA-gate.verdict.json') (@{ batch = 'bA'; tip = $gtip; green = $true; red = @() } | ConvertTo-Json)
  $install = Join-Path $base 'install'
  $copy = Join-Path $base 'bin-main'
  $sdk = Join-Path $base 'sdk'
  Write-Text (Join-Path $install 'engine-cli.exe') 'the old one, open in a session'
  # A running session's executable: on Windows a file opened with no sharing cannot be replaced, which
  # is what a running engine-view is to an install; elsewhere a lock is advisory and the case is not made.
  $held = if ($IsWindows) { [IO.File]::Open((Join-Path $install 'engine-cli.exe'), 'Open', 'Read', 'None') } else { $null }
  try {
    $a = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict (Join-Path $av 'bA-gate.verdict.json') -CopyTo $copy -Install $install -Sdk $sdk -Trailer 'Co-Authored-By: t' *>&1 | Out-String)
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
  Test-That 'after-gate exports the SDK from the gate worktree and does not push without -Push' {
    (Test-Path -LiteralPath (Join-Path $sdk 'sdk.json')) -and $a -match 'PUSH not taken' -and
      (& git -C $origin rev-parse main) -ne $gtip
  }
  Test-That 'after-gate refuses a red verdict and a verdict for another commit, and does nothing' {
    Write-Text (Join-Path $av 'red-gate.verdict.json') (@{ batch = 'red'; tip = $gtip; green = $false; red = @('suite msvc-debug') } | ConvertTo-Json)
    Write-Text (Join-Path $av 'old-gate.verdict.json') (@{ batch = 'old'; tip = '0000000000000000000000000000000000000000'; green = $true; red = @() } | ConvertTo-Json)
    $before = Invoke-TGit $main @('rev-parse', 'HEAD')
    $o1 = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict (Join-Path $av 'red-gate.verdict.json') -CopyTo $copy -Install $install -Sdk $sdk *>&1 | Out-String); $c1 = $LASTEXITCODE
    $o2 = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict (Join-Path $av 'old-gate.verdict.json') -CopyTo $copy -Install $install -Sdk $sdk *>&1 | Out-String); $c2 = $LASTEXITCODE
    $c1 -eq 2 -and $o1 -match 'was not green' -and $c2 -eq 2 -and $o2 -match 'the verdict is for' -and (Invoke-TGit $main @('rev-parse', 'HEAD')) -eq $before
  }
  Test-That 'with -Push, after-gate pushes main (here to a bare repository in the scratch directory)' {
    $o = (& pwsh -NoProfile -File $afterScript -Gate $gateTree -Main $main -Verdict (Join-Path $av 'bA-gate.verdict.json') -CopyTo $copy -Install $install -Sdk $sdk -Push *>&1 | Out-String)
    $LASTEXITCODE -eq 0 -and $o -match 'PUSH main to origin' -and (& git -C $origin rev-parse main) -eq (Invoke-TGit $main @('rev-parse', 'HEAD'))
  }
}
finally {
  Remove-Item Env:GATE_STANDIN_SCENARIO, Env:GATE_STANDIN_STATE -ErrorAction SilentlyContinue
  if ($failures.Count -gt 0 -or $KeepTemp) { Write-Host "kept: $base" }
  else { Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue }
}
Write-Host ''
if ($failures.Count -gt 0) { Write-Host "gate: $($failures.Count) of $checks checks failed" -ForegroundColor Red; exit 1 }
Write-Host "gate: $checks checks passed"
exit 0

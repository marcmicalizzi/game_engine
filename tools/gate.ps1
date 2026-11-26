#!/usr/bin/env pwsh
<#
.SYNOPSIS
  The merge gate over a gate worktree, as one command (roadmap T217; docs/ci/gate.md).

.DESCRIPTION
  A batch's gate:

      pwsh tools/gate.ps1 -Root <gate worktree> -Batch b73 -Logs <dir> [-Server <ssh alias>]

  A follow-up gated by what it changes (AGENTS.md, "What to run, and when"), the same command with
  the tests it can reach, and the checks its change cannot touch left out by name:

      pwsh tools/gate.ps1 -Root <gate worktree> -Batch b73b -Logs <dir> -Reach '<regex>' [-Skip listeners,budgets,weaker-card]

  In order, every step a process of its own with its whole output in -Logs:
    1. the Linux leg (the three containers, one after another) started beside everything below, and
       the GPU server leg (-Server) started detached: it writes its own verdict file and never holds
       this one;
    2. the documentation check and the lint;
    3. the four Windows builds (configured if the preset has no build tree, three attempts each,
       each after a short wait for memory to spare);
    4. the suites in two waves through the tree's own `tools/dev.ps1 test`, `-Jobs` each:
       msvc-debug, msvc-minimal and msvc-no-ecs together (the two off-configurations with
       -SkipInvariantGpu), then msvc-release's end-to-end set alone: four suites at once put long
       parts past the GPU lock's 30-minute deadline (b72). -Reach anchors wave one's filter;
    5. each suite's second look, once and alone: a test that gave up waiting for the card (exit 75)
       and engine_editor when it is the suite's only failure;
    6. only when every suite is green: the listener check, the frame budgets and the weaker card's
       leg (the renderer's set as a Titan Xp, T162);
    7. the Linux leg waited for; the integrity read of every kept CTest log; the litter check of the
       gate worktree; the summary and `<Logs>/<Batch>-gate.verdict.json`.

  Verdicts are read from what a leg kept, never from a console line: a suite's from the CTest logs
  `tools/dev.ps1` keeps per pass (copied into -Logs), a container's from its whole transcript. The
  summary fails when any leg that ran left no verdict. Every verdict holds each step's state and its
  source (`states`, `sources`). Exit 0 green, 1 red, 4 nothing red and a step inconclusive (the
  budgets on a busy machine: complete it with -Completes), 2 refused: an unknown
  -Skip name, a -Root that is not a git worktree, a -Logs inside it, or a -Root that is not clean at
  its HEAD (a tracked change, an untracked file git does not ignore, an unfinished merge), since the
  verdict names that commit. A tracked change or a moved HEAD during the run is red. The last
  `<Batch>-gate.verdict.json` in -Logs is set aside first, so no older green outlives a failed run.

  A follow-up that completes an earlier verdict of the same HEAD (T220), when that verdict is green
  in its checks, builds, suites, integrity and tree and holds a step inconclusive, not run or skipped:

      pwsh tools/gate.ps1 -Root <gate worktree> -Batch b73c -Logs <dir> -Completes <dir>/b73b-gate.verdict.json

  It runs no check, build or suite: only the earned steps (listeners, budgets, weaker card, Linux)
  the earlier verdict lacks or holds as inconclusive, less what -Skip names, and writes one folded
  verdict naming each step's source verdict and time. It refuses (exit 2) a verdict of another
  commit, of a tree that changed while it ran, or holding any step red: a red step is a verdict.

  The budgets (-BudgetWaitQuiet, default 120 s, is frame-budget.ps1's -WaitQuiet) keep their own
  states: exit 0 green; 4 (over only on a busy machine) flies the busy runs once more by themselves
  (-Run), green if they pass then, inconclusive if they come back busy; 6 inconclusive; anything
  else red, and nothing flown again turns a quiet over green. Exit 4 of the gate: nothing red, a
  step inconclusive.

  What it never does: push, merge, install, copy binaries out (tools/after-gate.ps1 does those, after
  the verdict is read), take or wait on the GPU lock itself (every test takes it, ADR-0049), kill a
  process, or write inside -Root other than through the tree's own tools and the one copy of
  -SamplesFrom into the git-ignored content/samples: -Logs must be outside it.
#>
[CmdletBinding(DefaultParameterSetName = 'gate')]
param(
  [Parameter(ParameterSetName = 'gate', Mandatory)][string]$Root,
  [Parameter(ParameterSetName = 'gate', Mandatory)][string]$Batch,
  [Parameter(ParameterSetName = 'gate', Mandatory)][string]$Logs,
  [Parameter(ParameterSetName = 'gate')][string]$Reach = '',
  [Parameter(ParameterSetName = 'gate')][string[]]$Skip = @(),
  [Parameter(ParameterSetName = 'gate')][string]$Server = '',
  [Parameter(ParameterSetName = 'gate')][int]$Jobs = 3,
  [Parameter(ParameterSetName = 'gate')][string]$WeakerCard = 'titanxp',
  [Parameter(ParameterSetName = 'gate')][int]$MemoryWaitMinutes = 15,
  [Parameter(ParameterSetName = 'gate')][string]$SamplesFrom = '',
  [Parameter(ParameterSetName = 'gate')][int]$BudgetWaitQuiet = 120,
  [Parameter(ParameterSetName = 'gate')][string]$Completes = '',
  [Parameter(ParameterSetName = 'leg', Mandatory)][ValidateSet('linux', 'server')][string]$Leg,
  [Parameter(ParameterSetName = 'leg', Mandatory)][string]$Spec
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/Gate.psm1') -Force

function Write-Json([string]$path, $value) {
  [IO.File]::WriteAllText($path, ($value | ConvertTo-Json -Depth 8), (New-Object System.Text.UTF8Encoding($false)))
}
function Stamp { (Get-Date).ToString('HH:mm:ss') }

# ---- a leg: a process of its own, started by the gate, writing its own verdict file ---------------
if ($PSCmdlet.ParameterSetName -eq 'leg') {
  $s = Get-Content -LiteralPath $Spec -Raw | ConvertFrom-Json
  $verdict = [ordered]@{ leg = $Leg; done = $false; started = (Get-Date).ToString('s'); presets = [ordered]@{} }
  Write-Json $s.Verdict $verdict
  $runs = if ($Leg -eq 'linux') { @($s.Presets | ForEach-Object { @{ Preset = $_; Script = 'tools/linux-build.ps1'; Args = @('-Preset', $_, '-Test') } }) }
          else { , @{ Preset = 'linux-server'; Script = 'tools/remote-build.ps1'; Args = @('-Host', $s.Host, '-Preset', 'linux-server', '-Test', '-Fetch', $s.Fetch) } }
  foreach ($r in $runs) {
    $log = Join-Path $s.Logs "$($s.Batch)-$Leg-$($r.Preset).log"
    Write-Host "$Leg $($r.Preset) start $(Stamp)"
    $code = Start-GateProcess -Script (Join-Path $s.Root $r.Script) -Arguments $r.Args -Log $log -WorkingDirectory $s.Root
    $text = if (Test-Path -LiteralPath $log) { [IO.File]::ReadAllText($log) } else { '' }
    $v = Read-ContainerVerdict -Text $text -Exit $code
    Write-Host "$($Leg.ToUpper()) $($r.Preset) exit $code$(if ($v.Ok) { ' OK' } else { ' NOT GREEN' }): $($v.Line) $(Stamp)"
    $verdict.presets[$r.Preset] = [ordered]@{ ok = $v.Ok; exit = $code; line = $v.Line; log = $log }
    Write-Json $s.Verdict $verdict
  }
  $verdict.done = $true
  $verdict.ended = (Get-Date).ToString('s')
  Write-Json $s.Verdict $verdict
  exit 0
}

# ---- the gate ---------------------------------------------------------------------------------------
# -Skip a,b from `pwsh -File` arrives as one string; either form is split and every name checked.
$Skip = @($Skip | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$unknown = @($Skip | Where-Object { $_ -notin 'linux', 'listeners', 'budgets', 'weaker-card' })
if ($unknown) { Write-Host "gate: -Skip takes linux, listeners, budgets, weaker-card; not $($unknown -join ', ')"; exit 2 }
if ($Completes -and $Reach) { Write-Host 'gate: -Completes runs no suite, so it takes no -Reach'; exit 2 }
if (-not (Test-Path -LiteralPath (Join-Path $Root '.git'))) { Write-Host "gate: $Root is not a git worktree"; exit 2 }
$Root = (Resolve-Path -LiteralPath $Root).Path
# -Logs is checked before it is made, so a refused run has made nothing inside the gate worktree (T217-L1).
$Logs = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Logs)
$sep = [IO.Path]::DirectorySeparatorChar
if (($Logs.TrimEnd($sep) + $sep).StartsWith($Root.TrimEnd($sep) + $sep, [StringComparison]::OrdinalIgnoreCase)) {
  Write-Host "gate: -Logs ($Logs) is inside the gate worktree; a gate writes nothing there of its own"; exit 2
}
New-Item -ItemType Directory -Force -Path $Logs | Out-Null
$Logs = (Resolve-Path -LiteralPath $Logs).Path
# The last verdict of this batch name is set aside before anything else can fail, so a run that is
# refused or stops part-way never leaves an older green where after-gate.ps1 would read it (T217-L2).
$verdictPath = Join-Path $Logs "$Batch-gate.verdict.json"
# A -Completes naming the verdict this run would write is refused before that file is set aside, so
# the verdict the caller meant to complete keeps its name (T220-L5).
$completesPath = ''
if ($Completes) {
  $completesPath = [IO.Path]::GetFullPath($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Completes))
  if ([string]::Equals($completesPath, [IO.Path]::GetFullPath($verdictPath), [StringComparison]::OrdinalIgnoreCase)) {
    Write-Host "gate: -Completes names the verdict this run would write ($verdictPath); give the follow-up a -Batch of its own (not $Batch); nothing set aside"; exit 2
  }
}
if (Test-Path -LiteralPath $verdictPath) {
  $aside = Join-Path $Logs ("$Batch-gate.verdict.previous-{0}.json" -f (Get-Date).ToString('yyyyMMdd-HHmmss-fff'))
  Move-Item -LiteralPath $verdictPath -Destination $aside
  Write-Host "gate: the last verdict of $Batch is set aside as $aside"
}
# The verdict names the commit at HEAD, so the tree must be that commit and nothing else: no tracked
# change, no untracked file git does not ignore, no unfinished merge (T217-H1). The primary checkout
# stood in that state on the morning of batch 73 (two unmerged pages and a stray directory).
try { $dirtAtStart = @(Get-WorktreeDirt -Root $Root) } catch { Write-Host "gate: $($_.Exception.Message); refused"; exit 2 }
if ($dirtAtStart) {
  Write-Host "gate: $Root is not clean at its HEAD, so a verdict could not name what it tested; refused:"
  $dirtAtStart | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
  exit 2
}
# Git for Windows' Unix tools on PATH for the tests that need them (tools.docs_gate: dirname, mktemp),
# found beside git rather than named (docs/ci/what-to-run.md, "Windows shell-test environment").
$git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($IsWindows -and $git) {
  $usr = Join-Path (Split-Path (Split-Path $git.Source)) 'usr/bin'
  if ((Test-Path -LiteralPath $usr) -and ($env:PATH -notlike "*$usr*")) { $env:PATH = "$usr;$env:PATH" }
}

$tip = (& git -C $Root rev-parse HEAD).Trim()
# A follow-up that completes an earlier verdict (T220): read and judged before anything runs.
$fold = $null
if ($Completes) {
  $earlier = $null
  if (Test-Path -LiteralPath $completesPath) { try { $earlier = Get-Content -LiteralPath $completesPath -Raw | ConvertFrom-Json } catch { $earlier = $null } }
  if (-not $earlier) { Write-Host "gate: no readable verdict at $completesPath; refused"; exit 2 }
  $fold = Get-FoldPlan -Earlier $earlier -EarlierPath $completesPath -Tip $tip -Skip $Skip
  if ($fold.Refusal) { Write-Host "gate: $($fold.Refusal); refused"; exit 2 }
}
$entriesBefore = Get-TreeEntries -Root $Root
$dev = Join-Path $Root 'tools/dev.ps1'
$started = Get-Date
$result = [ordered]@{ batch = $Batch; tip = $tip; reach = $Reach; skip = @($Skip); started = $started.ToString('s'); green = $false
                      steps = [ordered]@{}; suites = [ordered]@{}; no_verdict = @(); litter = @(); tree_changed = @()
                      retried_builds = @(); memory_wait_timeouts = @(); states = [ordered]@{}; sources = [ordered]@{}; inconclusive = @() }
if ($fold) {
  # The folded verdict carries the earlier one's record of what this run does not do again.
  foreach ($k in 'steps', 'suites') {
    if ($earlier.PSObject.Properties[$k] -and $earlier.$k) { foreach ($p in $earlier.$k.PSObject.Properties) { $result[$k][$p.Name] = $p.Value } }
  }
  # A step this run flies again overwrites its record below.
  foreach ($k in 'linux', 'weaker_card', 'budgets') { if ($earlier.PSObject.Properties[$k]) { $result[$k] = $earlier.$k } }
  # The suites it keeps ran under the earlier verdict's reach, so the folded verdict says that reach
  # (T220-M1): a fold of a -Reach verdict holds the reach set's suites, never the whole ones.
  $result.reach = if ($earlier.PSObject.Properties['reach']) { "$($earlier.reach)" } else { '' }
  $result.completes = $completesPath
}
$red = New-Object System.Collections.Generic.List[string]
$keptLogs = New-Object System.Collections.Generic.List[object]   # @{ Preset; Path } in the order written
"gate $Batch over $(& git -C $Root log --oneline -1); the tree is clean at its HEAD; $(Stamp)"
if ($Reach) { "follow-up: wave one runs ^($Reach)`$" }
if ($fold) { "follow-up completing $completesPath ($($earlier.batch), the same tip, its suites $(if ($result.reach) { "under -Reach '$($result.reach)'" } else { 'whole' })): runs $($fold.Run -join ', '), the release build first when one of them flies its binaries, and no check or suite" }
# Whether this run does a step: a batch's gate does every step it does not skip; a follow-up that
# completes a verdict does only the earned steps its plan names.
function Test-Runs([string]$step) { if ($fold) { $step -in $fold.Run } else { $step -notin $Skip } }
if ($Skip) { "skipped by the caller: $($Skip -join ', ')" }

function Wait-Memory([double]$commit, [double]$physical, [string]$what) {
  # Commit to spare and physical memory free, in GB, at most -MemoryWaitMinutes; a timeout proceeds,
  # says so and is kept in the verdict (a build that fails for memory is retried anyway). Write-Host,
  # because a string on the pipeline of a function whose result is tested is swallowed by the test
  # (the first gate's lesson).
  if (-not $IsWindows) { return }
  $deadline = (Get-Date).AddMinutes($MemoryWaitMinutes)
  while ($true) {
    $o = Get-CimInstance Win32_OperatingSystem
    $spare = $o.FreeVirtualMemory / 1MB; $free = $o.FreePhysicalMemory / 1MB
    if ($spare -ge $commit -and $free -ge $physical) { return }
    if ((Get-Date) -ge $deadline) {
      $line = "MEMORY-WAIT TIMED OUT before $what (commit to spare {0:N1} GB, physical free {1:N1} GB); proceeding" -f $spare, $free
      Write-Host $line
      $script:result.memory_wait_timeouts += $line
      return
    }
    Start-Sleep -Seconds 30
  }
}

function Get-KeptNames([string]$preset) {
  $dir = Join-Path $Root "build/$preset/Testing/Temporary"
  if (-not (Test-Path -LiteralPath $dir)) { return @() }
  @(Get-ChildItem -LiteralPath $dir -File -Filter 'LastTest-*.log' | ForEach-Object { $_.Name })
}

function Start-DevTest([string]$preset, [string]$filter, [string]$tag, [hashtable]$environment = @{}) {
  # tools/dev.ps1 test as a process of its own; the pass logs it keeps are collected by Complete-DevTest.
  $a = @('test', '-Preset', $preset)
  if ($filter) { $a += @('-Filter', $filter) }
  if ($Jobs -gt 0) { $a += @('-Jobs', "$Jobs") }
  if ($preset -in 'msvc-minimal', 'msvc-no-ecs') { $a += '-SkipInvariantGpu' }
  $saved = @{}
  $environment['ENGINE_GPU_LOCK_LOG'] = Join-Path $Logs "$Batch-$preset-$tag-holds.jsonl"
  foreach ($k in $environment.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $environment[$k]) }
  try {
    $before = Get-KeptNames $preset
    $p = Start-GateProcess -Script $dev -Arguments $a -Log (Join-Path $Logs "$Batch-$preset-$tag.log") -WorkingDirectory $Root -NoWait
  } finally { foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) } }
  [pscustomobject]@{ Preset = $preset; Tag = $tag; Process = $p; Before = $before }
}

function Complete-DevTest($run) {
  $run.Process.WaitForExit()
  $code = $run.Process.ExitCode
  $dir = Join-Path $Root "build/$($run.Preset)/Testing/Temporary"
  $new = @(Get-KeptNames $run.Preset | Where-Object { $_ -notin $run.Before } | Sort-Object)
  $copied = @()
  foreach ($n in $new) {
    $dst = Join-Path $Logs "$Batch-$($run.Preset)-$($run.Tag)-$n"
    Copy-Item -LiteralPath (Join-Path $dir $n) -Destination $dst -Force
    $copied += $dst
    $keptLogs.Add(@{ Preset = $run.Preset; Path = $dst })
  }
  [pscustomobject]@{ Exit = $code; Logs = $copied }
}

function Invoke-Wave([string[]]$presets, [hashtable]$filters, [string]$label, [hashtable]$environment = @{}) {
  "$label start $(Stamp): $($presets -join ', ')"
  $runs = @($presets | ForEach-Object { Start-DevTest $_ $filters[$_] 'suite' $environment.Clone() })
  # Every suite of the wave ends before any second look starts, so a rerun is alone on the card's line.
  $dones = @($runs | ForEach-Object { Complete-DevTest $_ })
  for ($i = 0; $i -lt $runs.Count; $i++) {
    $done = $dones[$i]
    $preset = $runs[$i].Preset
    $suite = [ordered]@{ ok = $false; filter = $filters[$preset]; exits = @($done.Exit); logs = @($done.Logs); reruns = @(); failed = @(); gave_up = @(); incomplete = @() }
    $unexplained = @(if (-not (Test-RunExplained -Exit $done.Exit -Logs $done.Logs)) { $done.Exit })
    $state = Get-SuiteState -Logs $suite.logs
    foreach ($name in (Get-RerunPlan -State $state)) {
      "RERUN $preset $name alone $(Stamp)"
      $again = Complete-DevTest (Start-DevTest $preset "^$([regex]::Escape($name))$" "rerun-$name" $environment.Clone())
      $suite.exits += $again.Exit
      $suite.logs += $again.Logs
      if (-not (Test-RunExplained -Exit $again.Exit -Logs $again.Logs)) { $unexplained += $again.Exit }
      $after = Get-SuiteState -Logs $again.Logs
      $now = if ($after.Tests.Contains($name)) { $after.Tests[$name].Status } else { 'no record' }
      "RERUN $preset ${name}: $now (exit $($again.Exit))"
      $suite.reruns += "$name`: $now"
    }
    $state = Get-SuiteState -Logs $suite.logs
    $suite.failed = @($state.Failed); $suite.gave_up = @($state.GaveUp); $suite.incomplete = @($state.Incomplete)
    $why = @()
    if ($state.Records -eq 0) { $why += 'no CTest log: no verdict'; $script:result.no_verdict += "suite $preset" }
    if ($state.Failed) { $why += "failed: $($state.Failed -join ', ')" }
    if ($state.GaveUp) { $why += "gave up on the card twice: $($state.GaveUp -join ', ')" }
    if ($state.Incomplete) { $why += "cut off: $($state.Incomplete -join ', ')" }
    if ($unexplained) { $why += "dev.ps1 exited $($unexplained -join ',') where its own logs hold no record that explains it" }
    $suite.ok = ($why.Count -eq 0)
    $script:result.suites[$preset] = $suite
    if ($suite.ok) { "SUITE $preset OK ($($state.Tests.Count) tests$(if ($suite.reruns) { '; ' + ($suite.reruns -join '; ') }))" }
    else { "SUITE $preset FAILED ($($why -join '; '))"; $red.Add("$($label.ToLower()) $preset") }
  }
  "$label end $(Stamp)"
}

function Invoke-Step([string]$name, [string]$script, [string[]]$arguments) {
  # Writes its marker and nothing else: the caller reads the code from $result.steps, since a string
  # and a value on one function's pipeline reach the caller's `if` together.
  $code = Start-GateProcess -Script $script -Arguments $arguments -Log (Join-Path $Logs "$Batch-$name.log") -WorkingDirectory $Root
  $script:result.steps[$name] = $code
  "$($name.ToUpper()) exit $code $(Stamp)"
  if ($code -ne 0) { $red.Add($name) }
}

function Invoke-Budgets {
  # The frame budgets as a step with three states (T220): the whole flight, and when it came back
  # over only on a busy machine (exit 4), the busy runs once more by themselves before the step is
  # called inconclusive. A quiet over (exit 1) is red and is never flown again. Each flight's
  # report.json and per-frame records go in -Logs, never under -Root.
  $script_ = Join-Path $Root 'tools/frame-budget.ps1'
  $out1 = Join-Path $Logs "$Batch-budgets-1"
  $first = Start-GateProcess -Script $script_ -Arguments @('-Preset', 'msvc-release', '-WaitQuiet', "$BudgetWaitQuiet", '-Out', $out1) -Log (Join-Path $Logs "$Batch-budgets.log") -WorkingDirectory $Root
  $flights = @([ordered]@{ runs = 'all'; exit = $first; out = $out1 })
  "BUDGETS exit $first $(Stamp)"
  $busy = @()
  $again = $null
  if ($first -in 4, 6) {
    $busy = @(Read-BudgetBusyRuns -Path (Join-Path $out1 'report.json'))
    if ($busy) {
      $out2 = Join-Path $Logs "$Batch-budgets-2"
      "BUDGETS again, by themselves: $($busy -join ', ') $(Stamp)"
      $again = Start-GateProcess -Script $script_ -Arguments @('-Preset', 'msvc-release', '-Run', ($busy -join ','), '-WaitQuiet', "$BudgetWaitQuiet", '-Out', $out2) -Log (Join-Path $Logs "$Batch-budgets-2.log") -WorkingDirectory $Root
      $flights += [ordered]@{ runs = ($busy -join ','); exit = $again; out = $out2 }
      "BUDGETS again exit $again $(Stamp)"
    } elseif ($first -eq 4) { 'BUDGETS exit 4 with no busy run in its report: the code and the report disagree' }
  }
  $state = Get-BudgetStepState -First $first -Busy $busy -Again $again
  $script:result.steps['budgets'] = $(if ($null -ne $again) { $again } else { $first })
  $script:result.budgets = [ordered]@{ state = $state; wait_quiet_s = $BudgetWaitQuiet; flights = $flights }
  "BUDGETS $($state.ToUpper())"
}

# 1. The legs beside everything: Linux (waited for at the end) and the server (never waited for).
$legs = @{}
if (Test-Runs 'linux') {
  $legSpec = Join-Path $Logs "$Batch-linux.spec.json"
  Write-Json $legSpec @{ Root = $Root; Logs = $Logs; Batch = $Batch; Verdict = (Join-Path $Logs "$Batch-linux.verdict.json")
                         Presets = @('linux-gcc-release', 'linux-clang-debug', 'linux-clang-minimal') }
  Remove-Item -LiteralPath (Join-Path $Logs "$Batch-linux.verdict.json") -ErrorAction SilentlyContinue
  $legs.linux = Start-GateProcess -Script $PSCommandPath -Arguments @('-Leg', 'linux', '-Spec', $legSpec) -Log (Join-Path $Logs "$Batch-linux.log") -WorkingDirectory $Root -NoWait
  "linux leg started ($($legs.linux.Id))"
}
$serverVerdict = Join-Path $Logs "$Batch-server.verdict.json"
if ($Server) {
  $legSpec = Join-Path $Logs "$Batch-server.spec.json"
  Write-Json $legSpec @{ Root = $Root; Logs = $Logs; Batch = $Batch; Verdict = $serverVerdict; Host = $Server; Fetch = (Join-Path $Logs "$Batch-server-fetch") }
  Remove-Item -LiteralPath $serverVerdict -ErrorAction SilentlyContinue
  $sp = Start-GateProcess -Script $PSCommandPath -Arguments @('-Leg', 'server', '-Spec', $legSpec) -Log (Join-Path $Logs "$Batch-server.log") -WorkingDirectory $Root -Detached
  "server leg started detached ($($sp.Id)); its verdict: $serverVerdict"
}

# 2. and 3. The checks and the builds; none of 2. to 5. in a follow-up that completes a verdict,
# whose earlier verdict holds them green at this very commit.
$builds = [ordered]@{}
function Invoke-Build([string]$preset) {
  # Every attempt and its log is kept in the verdict, and a build that passed on a retry is named
  # there: still green, never folded into a plain pass (T217-M2).
  $log = Join-Path $Logs "$Batch-build-$preset.log"
  $record = [ordered]@{ ok = $false; configure = $null; attempts = @(); passed_on_retry = $false }
  $ok = $true
  if (-not (Test-Path -LiteralPath (Join-Path $Root "build/$preset/build.ninja"))) {
    Wait-Memory 10 4 "configure $preset"
    $configureLog = Join-Path $Logs "$Batch-configure-$preset.log"
    $configureExit = Start-GateProcess -Script $dev -Arguments @('configure', '-Preset', $preset) -Log $configureLog -WorkingDirectory $Root
    $record.configure = [ordered]@{ exit = $configureExit; log = $configureLog }
    $ok = $configureExit -eq 0
    if (-not $ok) { "CONFIGURE $preset exit $configureExit ($configureLog)" }
  }
  $attempts = New-Object System.Collections.Generic.List[object]
  if ($ok) {
    foreach ($attempt in 1..3) {
      Wait-Memory 10 4 "build $preset attempt $attempt"
      $attemptLog = "$log.$attempt"
      $buildExit = Start-GateProcess -Script $dev -Arguments @('build', '-Preset', $preset) -Log $attemptLog -WorkingDirectory $Root
      $attempts.Add([ordered]@{ attempt = $attempt; exit = $buildExit; log = $attemptLog })
      if ($buildExit -eq 0) { $record.ok = $true; break }
      "BUILD $preset attempt $attempt failed (exit $buildExit; $attemptLog) $(Stamp)"
    }
  }
  $record.attempts = $attempts.ToArray()
  $record.passed_on_retry = $record.ok -and $attempts.Count -gt 1
  $script:builds[$preset] = $record
  if ($record.passed_on_retry) {
    "BUILD $preset OK on attempt $($attempts.Count), after $($attempts.Count - 1) failed: a pass on a retry, named in the verdict $(Stamp)"
    $script:result.retried_builds += $preset
  } elseif ($record.ok) { "BUILD $preset OK $(Stamp)" }
  else { "BUILD $preset FAILED after $($attempts.Count) attempt(s)"; $red.Add("build $preset") }
}
if (-not $fold) {
Invoke-Step 'docs' $dev @('docs')
Invoke-Step 'lint' $dev @('lint')
$checksOk = ($result.steps.docs -eq 0) -and ($result.steps.lint -eq 0)
foreach ($preset in 'msvc-debug', 'msvc-minimal', 'msvc-no-ecs', 'msvc-release') { Invoke-Build $preset }
$result.steps.builds = $builds

# 4. and 5. The suites in two waves, each with its second looks.
$filters = @{}
(Get-GateFilters -Reach $Reach).GetEnumerator() | ForEach-Object { $filters[$_.Key] = $_.Value }
$buildsOk = -not @($builds.Values | Where-Object { -not $_.ok })
if ($checksOk -and $buildsOk) {
  Invoke-Wave @('msvc-debug', 'msvc-minimal', 'msvc-no-ecs') $filters 'WAVE 1'
  Invoke-Wave @('msvc-release') $filters 'WAVE 2'
} else { 'SUITES not run: a check or a build failed' }
$suitesGreen = $checksOk -and $buildsOk -and $result.suites.Count -eq 4 -and -not @($result.suites.Values | Where-Object { -not $_.ok })
} else {
  # A follow-up flies the binaries of the tip it names (T220-L4): before a step that runs a release
  # binary (the listener check, the budgets), the release build the plain gate runs, a no-op when
  # build/msvc-release is current and a rebuild of the tip when it is not; a build that fails is red
  # and nothing earned is flown over what it left.
  $checksOk = $true; $buildsOk = $true
  if (@($fold.Run | Where-Object { $_ -in 'listeners', 'budgets' })) {
    Invoke-Build 'msvc-release'
    $result.steps.follow_up_build = $builds['msvc-release']
    $buildsOk = [bool]$builds['msvc-release'].ok
  }
  $suitesGreen = $buildsOk
  if (-not $buildsOk) { 'LISTENERS, BUDGETS, WEAKER CARD not run: the follow-up''s release build failed' }
}

# 6. What only a green set of suites earns.
if ($suitesGreen) {
  if (Test-Runs 'listeners') { Invoke-Step 'listeners' (Join-Path $Root 'tools/ci/check-no-listeners.ps1') @('-Preset', 'msvc-release') }
  if (Test-Runs 'budgets') {
    # The one write the gate makes inside -Root itself: the budgets' sample models into the
    # git-ignored content/samples, the folder's entries copied into it whether or not it exists
    # yet (T217-L3: copying the folder made content/samples/<leaf> when content/samples existed).
    $samplesDir = Join-Path $Root 'content/samples'
    if ($SamplesFrom -and -not (Test-Path -LiteralPath (Join-Path $samplesDir 'Lantern/Lantern.glb'))) {
      New-Item -ItemType Directory -Force -Path $samplesDir | Out-Null
      Get-ChildItem -LiteralPath $SamplesFrom -Force | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $samplesDir -Recurse -Force }
      "copied the entries of $SamplesFrom into content/samples"
    }
    Invoke-Budgets
  }
  if (Test-Runs 'weaker-card') {
    $w = @{ 'msvc-debug' = (Get-WeakerCardFilter) }
    $saved = $result.suites['msvc-debug']
    Invoke-Wave @('msvc-debug') $w 'WEAKER CARD' @{ ENGINE_GFX_TEST_DEVICE = $WeakerCard }
    $result.weaker_card = $result.suites['msvc-debug']; $result.suites['msvc-debug'] = $saved
  }
} else { 'LISTENERS, BUDGETS, WEAKER CARD not run: the suites are not all green' }

# 7. The Linux leg, the integrity read, the litter, the summary.
if ($legs.linux) {
  "waiting for the linux leg $(Stamp)"
  $legs.linux.WaitForExit()
  $lr = Read-LegVerdict -Path (Join-Path $Logs "$Batch-linux.verdict.json") -Expected 3
  $result.linux = $lr.Verdict
  $lr.Lines
  foreach ($n in $lr.Red) { $red.Add($n) }
  if ($lr.NoVerdict) { "NO VERDICT: $($lr.NoVerdict)"; $result.no_verdict += 'linux' }
}
'---- integrity'
$integrity = Get-IntegrityFindings -Logs $keptLogs.ToArray()
$integrity.Lines
"integrity: $($integrity.Records) test records read, $($integrity.Faults) faults"
$result.integrity = @{ records = $integrity.Records; faults = $integrity.Faults; lines = @($integrity.Lines) }
if ($integrity.Faults -gt 0) { $red.Add('integrity') }
try {
  $litter = Get-TreeLitter -Root $Root -Before $entriesBefore   # already one list: @() here would nest it
  # A tracked file changed, a merge begun or HEAD moved while the gate ran: the verdict would name a
  # commit whose tree it did not test (T217-H1; the litter check sees only new untracked entries).
  $changed = @(Get-WorktreeDirt -Root $Root -TrackedOnly)
  $headNow = (& git -C $Root rev-parse HEAD).Trim()
  if ($headNow -ne $tip) { $changed += "HEAD moved from $tip to $headNow" }
} catch {
  $litter = @(); $changed = @("the tree could not be read at the end: $($_.Exception.Message)")
}
$result.litter = @($litter)
if ($litter) { "LITTER in the gate worktree: $($litter -join ', ')"; $red.Add('litter') }
$result.tree_changed = @($changed)
if ($changed) { "TREE CHANGED during the gate: $($changed -join '; ')"; $red.Add('tree changed') }
if ($Server) {
  $sv = if (Test-Path -LiteralPath $serverVerdict) { Get-Content -LiteralPath $serverVerdict -Raw | ConvertFrom-Json } else { $null }
  $result.server = if ($sv -and $sv.done) { $sv } else { "running; its verdict will be $serverVerdict" }
  if ($sv -and $sv.done) { foreach ($p in $sv.presets.PSObject.Properties) { "SERVER $($p.Name) exit $($p.Value.exit): $($p.Value.line) (not part of this verdict)" } }
  else { "SERVER still running; it does not hold this verdict: $serverVerdict" }
}
foreach ($n in $result.no_verdict) { $red.Add("no verdict: $n") }

# Each step's state and the verdict it comes from (T220): what a follow-up reads to know what is left
# to complete, and the folded verdict's record of where every green came from.
$st = $result.states
$own = [ordered]@{ verdict = $verdictPath; batch = $Batch; started = $result.started; ended = '' }
function Set-State([string]$step, [string]$state, [switch]$Kept, [switch]$AndOwn) {
  $st[$step] = $state
  # Always a list, one entry a verdict (assigned whole: an `if` expression would unroll a list of one).
  $list = New-Object System.Collections.Generic.List[object]
  if ($Kept -or $AndOwn -or ($fold -and $step -in 'integrity', 'tree')) { foreach ($e in @($fold.Sources[$step])) { $list.Add($e) } }
  if (-not $Kept) { $list.Add($own) }
  $result.sources[$step] = $list.ToArray()
}
$earned = { param($step, $isRed)
  if ($fold -and $step -notin $fold.Run) {
    # Not run again: green or inconclusive as the earlier verdict holds it, an inconclusive never
    # promoted; any other state is a step this follow-up -Skips, recorded as a plain gate records a
    # skipped step (T220-L1), so no verdict ends holding a state nothing names.
    $was = $fold.States[$step]
    if ($was -in 'green', 'inconclusive') { Set-State $step $was -Kept } else { Set-State $step 'skipped' }
  }
  elseif ($step -in $Skip) { Set-State $step 'skipped' }
  elseif (-not $suitesGreen) { Set-State $step 'not run' }
  else { Set-State $step $(if ($isRed) { 'red' } else { 'green' }) }
}
if ($fold) {
  foreach ($s in 'checks', 'suites') { Set-State $s 'green' -Kept }
  # The follow-up's own release build, when it ran one, is part of the builds' record (T220-L4).
  if (-not $result.steps.Contains('follow_up_build')) { Set-State 'builds' 'green' -Kept }
  elseif ($buildsOk) { Set-State 'builds' 'green' -AndOwn }
  else { Set-State 'builds' 'red' }
} else {
  Set-State 'checks' $(if ($checksOk) { 'green' } else { 'red' })
  Set-State 'builds' $(if ($buildsOk) { 'green' } else { 'red' })
  Set-State 'suites' $(if (-not ($checksOk -and $buildsOk)) { 'not run' } elseif ($suitesGreen) { 'green' } else { 'red' })
}
& $earned 'listeners' ($red -contains 'listeners')
& $earned 'weaker-card' ($red -contains 'weaker card msvc-debug')
# The Linux leg runs beside everything, so its state is what it did whether or not the suites were
# green: red or green when it ran, `not run` only when it did not (T220-L3).
$linuxRed = @($red | Where-Object { $_ -like 'linux *' }).Count -or $result.no_verdict -contains 'linux'
if ($legs.linux) { Set-State 'linux' $(if ($linuxRed) { 'red' } else { 'green' }) }
else { & $earned 'linux' $linuxRed }
if ($fold -and 'budgets' -notin $fold.Run) { & $earned 'budgets' $false }
elseif ($result.Contains('budgets') -and $result.budgets -is [System.Collections.IDictionary]) { Set-State 'budgets' $result.budgets.state }
else { & $earned 'budgets' $false }
Set-State 'integrity' $(if ($integrity.Faults -gt 0) { 'red' } else { 'green' })
Set-State 'tree' $(if ($litter -or $changed) { 'red' } else { 'green' })
if ($st['budgets'] -eq 'red') { $red.Add('budgets') }
$result.inconclusive = @($st.Keys | Where-Object { $st[$_] -eq 'inconclusive' })
$result.red = @($red | Sort-Object -Unique)
$result.green = ($red.Count -eq 0) -and -not @($st.Values | Where-Object { $_ -notin 'green', 'skipped' })
$result.ended = (Get-Date).ToString('s')
foreach ($s in $result.sources.Values) { foreach ($e in $s) { if ($e.verdict -eq $verdictPath) { $e.ended = $result.ended } } }
if ($fold) { "folded with ${completesPath}: $(@($st.Keys | ForEach-Object { "$_ $($st[$_]) ($(@($result.sources[$_] | ForEach-Object { $_.batch }) -join '+'))" }) -join ', ')" }
Write-Json $verdictPath $result
# The last line says the verdict is written, and whether the server leg trails it (T231): a reader of
# this output knows the gate is done even while the leg runs on, which holds no handle of this one.
$after = " | verdict written: $verdictPath"
if ($Server -and $sp -and -not $sp.HasExited) { $after += " | the server leg trails ($($sp.Id)), its verdict will be $serverVerdict" }
if ($result.green) { "GATE $Batch exit 0 $(Stamp): green at $tip$after"; exit 0 }
if ($result.red.Count) { "GATE $Batch exit 1 $(Stamp): $($result.red -join '; ')$after"; exit 1 }
# Exit 4 is reached only with a state that is neither green nor skipped, so the list is never empty.
"GATE $Batch exit 4 $(Stamp): nothing red, and not green: $(Get-OpenStepsText $st)$after"
exit 4

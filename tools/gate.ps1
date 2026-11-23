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
  summary fails when any leg that ran left no verdict. Exit 0 green, 1 red, 2 refused.

  What it never does: push, merge, install, copy binaries out (tools/after-gate.ps1 does those, after
  the verdict is read), take or wait on the GPU lock itself (every test takes it, ADR-0049), kill a
  process, or write inside -Root other than through the tree's own tools: -Logs must be outside it.
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
if (-not (Test-Path -LiteralPath (Join-Path $Root '.git'))) { Write-Host "gate: $Root is not a git worktree"; exit 2 }
$Root = (Resolve-Path -LiteralPath $Root).Path
New-Item -ItemType Directory -Force -Path $Logs | Out-Null
$Logs = (Resolve-Path -LiteralPath $Logs).Path
$sep = [IO.Path]::DirectorySeparatorChar
if (($Logs.TrimEnd($sep) + $sep).StartsWith($Root.TrimEnd($sep) + $sep, [StringComparison]::OrdinalIgnoreCase)) {
  Write-Host "gate: -Logs ($Logs) is inside the gate worktree; a gate writes nothing there of its own"; exit 2
}
# Git for Windows' Unix tools on PATH for the tests that need them (tools.docs_gate: dirname, mktemp),
# found beside git rather than named (docs/ci/what-to-run.md, "Windows shell-test environment").
$git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if ($IsWindows -and $git) {
  $usr = Join-Path (Split-Path (Split-Path $git.Source)) 'usr/bin'
  if ((Test-Path -LiteralPath $usr) -and ($env:PATH -notlike "*$usr*")) { $env:PATH = "$usr;$env:PATH" }
}

$tip = (& git -C $Root rev-parse HEAD).Trim()
$entriesBefore = Get-TreeEntries -Root $Root
$dev = Join-Path $Root 'tools/dev.ps1'
$started = Get-Date
$result = [ordered]@{ batch = $Batch; tip = $tip; reach = $Reach; skip = @($Skip); started = $started.ToString('s'); green = $false
                      steps = [ordered]@{}; suites = [ordered]@{}; no_verdict = @(); litter = @() }
$red = New-Object System.Collections.Generic.List[string]
$keptLogs = New-Object System.Collections.Generic.List[object]   # @{ Preset; Path } in the order written
"gate $Batch over $(& git -C $Root log --oneline -1); untracked at start: $(@(& git -C $Root status --porcelain --untracked-files=normal).Count); $(Stamp)"
if ($Reach) { "follow-up: wave one runs ^($Reach)`$" }
if ($Skip) { "skipped by the caller: $($Skip -join ', ')" }

function Wait-Memory([double]$commit, [double]$physical) {
  # Commit to spare and physical memory free, in GB, at most -MemoryWaitMinutes; a timeout proceeds and
  # says so (a build that fails for memory is retried anyway). Write-Host, because a string on the
  # pipeline of a function whose result is tested is swallowed by the test (the first gate's lesson).
  if (-not $IsWindows) { return }
  $deadline = (Get-Date).AddMinutes($MemoryWaitMinutes)
  while ($true) {
    $o = Get-CimInstance Win32_OperatingSystem
    $spare = $o.FreeVirtualMemory / 1MB; $free = $o.FreePhysicalMemory / 1MB
    if ($spare -ge $commit -and $free -ge $physical) { return }
    if ((Get-Date) -ge $deadline) { Write-Host ("MEMORY-WAIT TIMED OUT (commit to spare {0:N1} GB, physical free {1:N1} GB); proceeding" -f $spare, $free); return }
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
    if ($unexplained) { $why += "dev.ps1 exited $($unexplained -join ',') where its own logs hold no failed record" }
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

# 1. The legs beside everything: Linux (waited for at the end) and the server (never waited for).
$legs = @{}
if ('linux' -notin $Skip) {
  $spec = Join-Path $Logs "$Batch-linux.spec.json"
  Write-Json $spec @{ Root = $Root; Logs = $Logs; Batch = $Batch; Verdict = (Join-Path $Logs "$Batch-linux.verdict.json")
                      Presets = @('linux-gcc-release', 'linux-clang-debug', 'linux-clang-minimal') }
  Remove-Item -LiteralPath (Join-Path $Logs "$Batch-linux.verdict.json") -ErrorAction SilentlyContinue
  $legs.linux = Start-GateProcess -Script $PSCommandPath -Arguments @('-Leg', 'linux', '-Spec', $spec) -Log (Join-Path $Logs "$Batch-linux.log") -WorkingDirectory $Root -NoWait
  "linux leg started ($($legs.linux.Id))"
}
$serverVerdict = Join-Path $Logs "$Batch-server.verdict.json"
if ($Server) {
  $spec = Join-Path $Logs "$Batch-server.spec.json"
  Write-Json $spec @{ Root = $Root; Logs = $Logs; Batch = $Batch; Verdict = $serverVerdict; Host = $Server; Fetch = (Join-Path $Logs "$Batch-server-fetch") }
  Remove-Item -LiteralPath $serverVerdict -ErrorAction SilentlyContinue
  $sp = Start-GateProcess -Script $PSCommandPath -Arguments @('-Leg', 'server', '-Spec', $spec) -Log (Join-Path $Logs "$Batch-server.log") -WorkingDirectory $Root -Detached
  "server leg started detached ($($sp.Id)); its verdict: $serverVerdict"
}

# 2. and 3. The checks and the builds.
Invoke-Step 'docs' $dev @('docs')
Invoke-Step 'lint' $dev @('lint')
$checksOk = ($result.steps.docs -eq 0) -and ($result.steps.lint -eq 0)
$builds = [ordered]@{}
foreach ($preset in 'msvc-debug', 'msvc-minimal', 'msvc-no-ecs', 'msvc-release') {
  $log = Join-Path $Logs "$Batch-build-$preset.log"
  $ok = $true
  if (-not (Test-Path -LiteralPath (Join-Path $Root "build/$preset/build.ninja"))) {
    Wait-Memory 10 4
    $ok = (Start-GateProcess -Script $dev -Arguments @('configure', '-Preset', $preset) -Log (Join-Path $Logs "$Batch-configure-$preset.log") -WorkingDirectory $Root) -eq 0
  }
  $built = $false
  if ($ok) {
    foreach ($attempt in 1..3) {
      Wait-Memory 10 4
      if ((Start-GateProcess -Script $dev -Arguments @('build', '-Preset', $preset) -Log "$log.$attempt" -WorkingDirectory $Root) -eq 0) { $built = $true; break }
      "BUILD $preset attempt $attempt failed $(Stamp)"
    }
  }
  $builds[$preset] = $built
  if ($built) { "BUILD $preset OK $(Stamp)" } else { "BUILD $preset FAILED"; $red.Add("build $preset") }
}
$result.steps.builds = $builds

# 4. and 5. The suites in two waves, each with its second looks.
$filters = @{}
(Get-GateFilters -Reach $Reach).GetEnumerator() | ForEach-Object { $filters[$_.Key] = $_.Value }
$buildsOk = -not @($builds.Values | Where-Object { -not $_ })
if ($checksOk -and $buildsOk) {
  Invoke-Wave @('msvc-debug', 'msvc-minimal', 'msvc-no-ecs') $filters 'WAVE 1'
  Invoke-Wave @('msvc-release') $filters 'WAVE 2'
} else { 'SUITES not run: a check or a build failed' }
$suitesGreen = $checksOk -and $buildsOk -and $result.suites.Count -eq 4 -and -not @($result.suites.Values | Where-Object { -not $_.ok })

# 6. What only a green set of suites earns.
if ($suitesGreen) {
  if ('listeners' -notin $Skip) { Invoke-Step 'listeners' (Join-Path $Root 'tools/ci/check-no-listeners.ps1') @('-Preset', 'msvc-release') }
  if ('budgets' -notin $Skip) {
    if ($SamplesFrom -and -not (Test-Path -LiteralPath (Join-Path $Root 'content/samples/Lantern/Lantern.glb'))) {
      Copy-Item -LiteralPath $SamplesFrom -Destination (Join-Path $Root 'content/samples') -Recurse -Force
      "copied content/samples from $SamplesFrom"
    }
    Invoke-Step 'budgets' (Join-Path $Root 'tools/frame-budget.ps1') @('-Preset', 'msvc-release', '-WaitQuiet', '120')
  }
  if ('weaker-card' -notin $Skip) {
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
  $lv = Join-Path $Logs "$Batch-linux.verdict.json"
  $v = if (Test-Path -LiteralPath $lv) { Get-Content -LiteralPath $lv -Raw | ConvertFrom-Json } else { $null }
  $result.linux = $v
  if (-not $v -or -not $v.done) { 'NO VERDICT: the linux leg did not finish its verdict file'; $result.no_verdict += 'linux' }
  else {
    foreach ($p in $v.presets.PSObject.Properties) { "LINUX $($p.Name) exit $($p.Value.exit): $($p.Value.line)"; if (-not $p.Value.ok) { $red.Add("linux $($p.Name)") } }
    if (@($v.presets.PSObject.Properties).Count -ne 3) { 'NO VERDICT: the linux leg reported fewer than three containers'; $result.no_verdict += 'linux' }
  }
}
'---- integrity'
$integrity = Get-IntegrityFindings -Logs $keptLogs.ToArray()
$integrity.Lines
"integrity: $($integrity.Records) test records read, $($integrity.Faults) faults"
$result.integrity = @{ records = $integrity.Records; faults = $integrity.Faults; lines = @($integrity.Lines) }
if ($integrity.Faults -gt 0) { $red.Add('integrity') }
$litter = Get-TreeLitter -Root $Root -Before $entriesBefore
$result.litter = @($litter)
if ($litter) { "LITTER in the gate worktree: $($litter -join ', ')"; $red.Add('litter') }
if ($Server) {
  $sv = if (Test-Path -LiteralPath $serverVerdict) { Get-Content -LiteralPath $serverVerdict -Raw | ConvertFrom-Json } else { $null }
  $result.server = if ($sv -and $sv.done) { $sv } else { "running; its verdict will be $serverVerdict" }
  if ($sv -and $sv.done) { foreach ($p in $sv.presets.PSObject.Properties) { "SERVER $($p.Name) exit $($p.Value.exit): $($p.Value.line) (not part of this verdict)" } }
  else { "SERVER still running; it does not hold this verdict: $serverVerdict" }
}
foreach ($n in $result.no_verdict) { $red.Add("no verdict: $n") }
$result.red = @($red | Sort-Object -Unique)
$result.green = ($red.Count -eq 0)
$result.ended = (Get-Date).ToString('s')
Write-Json (Join-Path $Logs "$Batch-gate.verdict.json") $result
if ($result.green) { "GATE $Batch exit 0 $(Stamp): green at $tip"; exit 0 }
"GATE $Batch exit 1 $(Stamp): $($result.red -join '; ')"
exit 1

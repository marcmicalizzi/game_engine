#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for the frame-budget gate's rules (tools/lib/FrameBudget.psm1, tools/frame-budget.ps1).
  Runs under CTest as `tools.frame_budget`.

.DESCRIPTION
  The gate decides whether a change merges (ADR-0018), so its arithmetic and its verdicts are held
  as rules over summaries written here, with no GPU: a number within its tolerance passes and one
  past it fails naming the pass; an over on a busy machine is inconclusive and a pass on one is a
  pass; another adapter's run is skipped; a summary missing a budgeted pass fails; a re-baseline
  records who and why and refuses no reason. The Windows session's state is injected into the
  summaries (New-Summary -session): a flight is held only to its own state's budget, a state the run
  holds no budget for is compared with nothing (exit 6), and a re-baseline writes its own state's
  set beside the other (T161). Then the committed budget file is read, so a malformed
  edit to it fails here before it fails a merge, and the script compares a summary directory
  without flying anything (-FromSummaries).

      pwsh tools/frame-budget.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Import-Module (Join-Path $PSScriptRoot 'lib/FrameBudget.psm1') -Force

$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { Write-Host "       threw: $($_.Exception.Message)"; $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

# A session's budget set: the total and the cull, the frame thread, and who set it.
function New-Set([double]$total = 2.0, [double]$cull = 0.3, [string]$date = '2026-10-07') {
  [pscustomobject]@{
    gpu_ms = [pscustomobject]@{
      total = [pscustomobject]@{ median = $total; p99 = 2.0 * $total }
      cull = [pscustomobject]@{ median = $cull; p99 = 0.4 }
    }
    cpu_ms = [pscustomobject]@{ median = 0.5; p99 = 1.0 }
    baseline = [pscustomobject]@{ date = $date; by = 'test'; reason = 'the test''s own' }
  }
}
# One run holding a locked budget only, as the committed file did when T161 landed.
function New-Budget {
  [pscustomobject]@{
    format = 'engine.frame-budgets.v2'
    machine = [pscustomobject]@{ device = 'Test GPU 9000'; name = 'the test machine'; preset = 'msvc-release' }
    tolerance = [pscustomobject]@{
      gpu = [pscustomobject]@{ median = [pscustomobject]@{ pct = 10; ms = 0.05 }; p99 = [pscustomobject]@{ pct = 20; ms = 0.1 } }
      cpu = [pscustomobject]@{ median = [pscustomobject]@{ pct = 25; ms = 0.1 }; p99 = [pscustomobject]@{ pct = 50; ms = 0.5 } }
    }
    runs = @([pscustomobject]@{
      id = 'scene-1080p'; scene = 'content/test-scenes/x/scene.json'; path = 'content/test-scenes/x/camera-path.json'
      width = 1920; height = 1080; views = 'single'; repeat = 1; warmup = 30; args = @('--shadows', 'csm')
      budgets = [pscustomobject]@{ locked = (New-Set) }
    })
  }
}
# A summary as engine-view writes it. -session injects the Windows session's state at the two ends
# of the flight ('locked', 'unlocked', 'changed' for locked then unlocked, 'unknown' for nulls):
# the harness reads it from the machine (LogonUI.exe), so this is the only way a test reaches a
# state the machine running it is not in.
function New-Summary([double]$total = 2.0, [double]$totalP99 = 4.0, [double]$cull = 0.3, [bool]$quiet = $true,
                     [string]$device = 'Test GPU 9000', [double]$cpu = 0.5, [string]$session = 'locked') {
  $ends = switch ($session) { 'locked' { @($true, $true) } 'unlocked' { @($false, $false) } 'changed' { @($true, $false) } default { @($null, $null) } }
  [pscustomobject]@{
    format = 'engine.flythrough.v1'; device = $device; quiet = $quiet; identity = 'abc'; frames = 100; repeats = 1
    gpu_ms = [pscustomobject]@{
      total = [pscustomobject]@{ median = $total; p99 = $totalP99 }
      cull = [pscustomobject]@{ median = $cull; p99 = 0.4 }
      hw = [pscustomobject]@{ median = 0.05; p99 = 0.07 }
      sw = [pscustomobject]@{ median = 0.0; p99 = 0.0 }
    }
    cpu_ms = [pscustomobject]@{ median = $cpu; p99 = 1.0 }
    machine_state = [pscustomobject]@{
      start = [pscustomobject]@{ cpu_others_pct = 3.0; gpu_util_pct = 2; session_locked = $ends[0] }
      end = [pscustomobject]@{ cpu_others_pct = 4.0; gpu_util_pct = 5; session_locked = $ends[1] }
    }
  }
}
function Row($result, [string]$line, [string]$stat) { $result.Rows | Where-Object { $_.Line -eq $line -and $_.Stat -eq $stat } | Select-Object -First 1 }

Write-Host 'the limit'
Test-That 'is the budget plus its percentage plus its milliseconds' { [math]::Abs((Get-FrameBudgetLimit 2.0 10 0.05) - 2.25) -lt 1e-12 }
Test-That 'a tiny pass is held by the milliseconds, not the percentage' { [math]::Abs((Get-FrameBudgetLimit 0.01 10 0.05) - 0.061) -lt 1e-12 }

Write-Host 'a run against its budget'
$b = New-Budget
$run = $b.runs[0]
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary)
Test-That 'at its budget it passes, with a row per budgeted number' { $r.Verdict -eq 'pass' -and $r.Rows.Count -eq 6 }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -total 2.24)
Test-That 'inside the tolerance it passes' { $r.Verdict -eq 'pass' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -total 2.26)
Test-That 'past it on a quiet machine it is over, and the row says which pass and statistic' {
  $r.Verdict -eq 'over' -and (Row $r 'gpu_ms.total' 'median').Over -and -not (Row $r 'gpu_ms.total' 'p99').Over
}
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -totalP99 4.95)
Test-That 'a p99 past its own looser tolerance is over' { $r.Verdict -eq 'over' -and (Row $r 'gpu_ms.total' 'p99').Over }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -totalP99 4.85)
Test-That 'a p99 inside it is not' { $r.Verdict -eq 'pass' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -cull 0.5)
Test-That 'a pass other than the total goes over by itself' { $r.Verdict -eq 'over' -and (Row $r 'gpu_ms.cull' 'median').Over }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -cpu 0.8)
Test-That 'the frame thread''s cpu_ms has its own tolerance' { $r.Verdict -eq 'over' -and (Row $r 'cpu_ms' 'median').Over }

Write-Host "other programs' share of the GPU through a flight (T230): sampled on request, deciding nothing"
# The counters' instances as the WMI class names them. The flight (4242) has its device on adapter
# c3a1; another adapter's engines and a video decode engine do not count; the others on the 3D,
# compute and copy engines of c3a1 are summed per engine and the busiest is the share.
$looks = @(
  @{ Path = 'pid_4242_luid_0x00000000_0x0000c3a1_phys_0_eng_0_engtype_3D'; CookedValue = 90.0 },
  @{ Path = 'pid_100_luid_0x00000000_0x0000c3a1_phys_0_eng_0_engtype_3D'; CookedValue = 3.0 },
  @{ Path = 'pid_200_luid_0x00000000_0x0000c3a1_phys_0_eng_0_engtype_3D'; CookedValue = 1.5 },
  @{ Path = 'pid_300_luid_0x00000000_0x0000c3a1_phys_0_eng_2_engtype_Copy'; CookedValue = 4.0 },
  @{ Path = 'pid_100_luid_0x00000000_0x0000c3a1_phys_0_eng_3_engtype_VideoDecode'; CookedValue = 30.0 },
  @{ Path = 'pid_500_luid_0x00000000_0x0000ffff_phys_0_eng_0_engtype_3D'; CookedValue = 50.0 }
) | ForEach-Object { [pscustomobject]$_ }
Test-That 'a look sums the others on each 3D, compute and copy engine of the flight''s adapter and takes the busiest' {
  [math]::Abs((ConvertFrom-GpuEngineSamples -Samples $looks -ExcludePid 4242) - 4.5) -lt 1e-9
}
Test-That 'a video decode engine and another adapter''s engines never count' {
  (ConvertFrom-GpuEngineSamples -Samples @($looks[0], $looks[4], $looks[5]) -ExcludePid 4242) -eq 0
}
Test-That 'no engine instance, or a flight with none yet (its adapter unknown), is no reading' {
  $null -eq (ConvertFrom-GpuEngineSamples -Samples @([pscustomobject]@{ Path = 'processor(_total)'; CookedValue = 9 })) -and
    $null -eq (ConvertFrom-GpuEngineSamples -Samples @($looks[1], $looks[2]) -ExcludePid 4242)
}
# Batch 75's figures: 4 to 5 % in the desktop's hands while three p99 lines went over.
$desktop = Get-GpuShareSummary -Samples @(4.2, 4.8, 4.5, 5.0, 4.4)
Test-That 'the share is summarized against the provisional threshold and decides nothing' {
  $desktop.PastThreshold -and $desktop.Provisional -and -not $desktop.Decides -and $desktop.Count -eq 5 -and $desktop.ThresholdPct -eq 2.0
}
$medianOver = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -total 2.26) -GpuShare $desktop
$p99Over = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -totalP99 4.95) -GpuShare $desktop
$cpuOver = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -cpu 0.8) -GpuShare $desktop
$passShare = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary) -GpuShare $desktop
Test-That 'with a share past the threshold an over on a median, a p99 or cpu_ms stays over, and a pass stays a pass' {
  $medianOver.Verdict -eq 'over' -and $p99Over.Verdict -eq 'over' -and $cpuOver.Verdict -eq 'over' -and $passShare.Verdict -eq 'pass' -and
    (Get-FrameBudgetExitCode -Results @($p99Over)) -eq 1
}
Test-That 'the share is said beside the verdict and kept in the record' {
  $j = @($p99Over) | ConvertTo-Json -Depth 8 | ConvertFrom-Json
  $p99Over.MachineState -match 'GPU share through the flight 4\.6% mean.*decides nothing' -and @($j.GpuShare.Samples).Count -eq 5
}
# The switch: off by default, and every look of the flight's loop is inside it.
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'frame-budget.ps1'), [ref]$null, [ref]$null)
$switch = $ast.ParamBlock.Parameters | Where-Object { $_.Name.VariablePath.UserPath -eq 'GpuShare' }
$reads = @($ast.FindAll({ param($n) $n -is [System.Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Read-GpuOthersShare' }, $true))
function Test-InsideSwitch($node) {
  for ($p = $node.Parent; $p; $p = $p.Parent) {
    if ($p -is [System.Management.Automation.Language.IfStatementAst] -and ($p.Clauses | Where-Object { $_.Item1.Extent.Text -eq '$GpuShare' })) { return $true }
  }
  return $false
}
Test-That 'the sampling is a switch, off by default, and with it off nothing is sampled' {
  $switch -and $switch.StaticType -eq [switch] -and $null -eq $switch.DefaultValue -and $reads.Count -ge 1 -and
    -not ($reads | Where-Object { -not (Test-InsideSwitch $_) })
}
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -total 3.0 -quiet $false)
Test-That 'over on a busy machine is inconclusive, not a failure' { $r.Verdict -eq 'busy' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -quiet $false)
Test-That 'within budget on a busy machine is a pass: an upper bound under the limit is under it' { $r.Verdict -eq 'pass' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -total 9.0 -device 'Another GPU')
Test-That 'another adapter''s run is skipped whatever it measured' { $r.Verdict -eq 'device' }
$s = New-Summary
$s.gpu_ms.PSObject.Properties.Remove('cull')
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary $s
Test-That 'a summary without a budgeted pass is malformed, and says which' { $r.Verdict -eq 'malformed' -and ($r.Missing -contains 'gpu_ms.cull.median') }
Test-That 'the machine state is one line with the session and both ends' { (Format-MachineState (New-Summary)) -match '^quiet, session locked \(start 3\.0% others, GPU 2%; end 4\.0% others, GPU 5%\)$' }
# The end sample engine-view reads again after nvidia-smi's period when its first reading was the
# flight's own last frames (bench.md, "The end sample is read after the run's own work"): the
# summary's `quiet` is engine-view's, and the line names the reading it set aside.
$s = New-Summary
$s.machine_state.end | Add-Member -NotePropertyName gpu_own_tail_pct -NotePropertyValue 97
Test-That 'an end sample read past the flight''s own tail says what it set aside' { (Format-MachineState $s) -match '^quiet, session locked \(start 3\.0% others, GPU 2%; end 4\.0% others, GPU 5% \(own tail 97%\)\)$' }

Write-Host 'the session state (T161): a locked card and an unlocked one are two cards'
Test-That 'locked at both ends is locked' { (Get-SessionState -Summary (New-Summary -session 'locked')) -eq 'locked' }
Test-That 'unlocked at both ends is unlocked' { (Get-SessionState -Summary (New-Summary -session 'unlocked')) -eq 'unlocked' }
Test-That 'locked at one end and unlocked at the other is changed' { (Get-SessionState -Summary (New-Summary -session 'changed')) -eq 'changed' }
Test-That 'nulls at both ends are unknown' { (Get-SessionState -Summary (New-Summary -session 'unknown')) -eq 'unknown' }
$s = New-Summary -session 'unlocked'
$s.machine_state.end.session_locked = $null
Test-That 'one end known and the other null is the known one' { (Get-SessionState -Summary $s) -eq 'unlocked' }
$s = New-Summary
$s.PSObject.Properties.Remove('machine_state')
Test-That 'a summary with no machine state is unknown' { (Get-SessionState -Summary $s) -eq 'unknown' }
$s = New-Summary
$s.machine_state.start.PSObject.Properties.Remove('session_locked')
$s.machine_state.end.PSObject.Properties.Remove('session_locked')
Test-That 'and so is one written before the field existed' { (Get-SessionState -Summary $s) -eq 'unknown' }

$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -session 'locked')
Test-That 'a locked flight is held to the locked budget and says so' { $r.Verdict -eq 'pass' -and $r.Session -eq 'locked' -and $r.Compared -eq 'the locked baseline of 2026-10-07' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -session 'unlocked' -total 1.0)
Test-That 'an unlocked flight of a run with no unlocked budget is compared with nothing, and says what the run holds' {
  $r.Verdict -eq 'nobaseline' -and $r.Rows.Count -eq 0 -and $r.MachineState -match 'no unlocked baseline \(it holds: locked \(2026-10-07\)\)' -and $r.MachineState -match '-Rebaseline'
}
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -session 'unlocked' -total 9.0)
Test-That 'however far over the locked numbers it reads: never held to the other state''s budget' { $r.Verdict -eq 'nobaseline' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -session 'changed')
Test-That 'a flight whose session changed is compared with nothing' { $r.Verdict -eq 'session' -and $r.MachineState -match 'two cards' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -session 'unknown')
Test-That 'and so is one that does not record it' { $r.Verdict -eq 'session' -and $r.MachineState -match 'does not record' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -session 'unlocked' -device 'Another GPU')
Test-That 'another adapter is skipped before the session is asked' { $r.Verdict -eq 'device' }

# Both states side by side: the locked card's 3.0 ms and the unlocked card's 1.0 ms.
$both = New-Budget
$both.runs[0].budgets = [pscustomobject]@{ locked = (New-Set -total 3.0 -cull 0.4); unlocked = (New-Set -total 1.0 -cull 0.05 -date '2026-11-17') }
$r2 = $both.runs[0]
Test-That 'a file holding both states reads' { try { Assert-FrameBudget $both; $true } catch { Write-Host "       $($_.Exception.Message)"; $false } }
$r = Test-FrameBudgetRun -Budget $both -Run $r2 -Summary (New-Summary -session 'locked' -total 2.0 -cull 0.3)
Test-That 'a locked 2.0 ms passes the locked 3.0' { $r.Verdict -eq 'pass' -and $r.Compared -match 'locked' }
$r = Test-FrameBudgetRun -Budget $both -Run $r2 -Summary (New-Summary -session 'unlocked' -total 2.0 -cull 0.3)
Test-That 'the same numbers unlocked are over the unlocked 1.0, naming its baseline' {
  $r.Verdict -eq 'over' -and (Row $r 'gpu_ms.total' 'median').Over -and (Row $r 'gpu_ms.total' 'median').Budget -eq 1.0 -and $r.Compared -eq 'the unlocked baseline of 2026-11-17'
}
$r = Test-FrameBudgetRun -Budget $both -Run $r2 -Summary (New-Summary -session 'unlocked' -total 1.0 -totalP99 2.0 -cull 0.05)
Test-That 'and the unlocked card''s own numbers pass it' { $r.Verdict -eq 'pass' }

Write-Host 'the measurement rule (ADR-0064): only a native measurement is a cost'
# A summary as engine-view writes it since the rule: the block, and for anything but a native
# measurement a null `gpu_ms` with the times under `reconstructed`.
function Add-Block($summary, [string]$provider = 'off', [int]$iw = 1920, [int]$ih = 1080, [bool]$inputs = $false) {
  $summary | Add-Member -NotePropertyName reconstruction -NotePropertyValue ([pscustomobject]@{
    provider = $provider; mode = $(if ($iw -eq 1920) { 'native' } else { 'ratio' }); inputs = $inputs
    internal_width = $iw; internal_height = $ih; output_width = 1920; output_height = 1080 })
  $summary
}
function Move-Times($summary) {
  $summary | Add-Member -NotePropertyName reconstructed -NotePropertyValue ([pscustomobject]@{ gpu_ms = $summary.gpu_ms })
  $summary.gpu_ms = $null
  $summary
}
Test-That 'a summary written before the block existed is native' { $null -eq (Get-ReconstructionRefusal -Summary (New-Summary)) }
Test-That 'a native summary with the block is native' { $null -eq (Get-ReconstructionRefusal -Summary (Add-Block (New-Summary))) }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (Add-Block (New-Summary))
Test-That 'and is compared as it always was' { $r.Verdict -eq 'pass' -and $r.Rows.Count -eq 6 }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (Add-Block (New-Summary) -inputs $true)
Test-That 'the inputs on with no provider is still a native measurement: the motion pass is in its cost' { $r.Verdict -eq 'pass' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (Move-Times (Add-Block (New-Summary -total 0.1) -provider 'engine'))
Test-That 'a provider on is refused, though its numbers would pass' { $r.Verdict -eq 'reconstructed' -and $r.Rows.Count -eq 0 -and $r.MachineState -match 'engine, 1920x1080 -> 1920x1080' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (Move-Times (Add-Block (New-Summary -total 0.1) -iw 1280 -ih 720))
Test-That 'an internal size below the output''s is refused with no provider named' { $r.Verdict -eq 'reconstructed' -and $r.MachineState -match '1280x720 -> 1920x1080' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (Add-Block (New-Summary -total 0.1) -provider 'engine')
Test-That 'a writer that left the times in `gpu_ms` beside a provider is refused all the same' { $r.Verdict -eq 'reconstructed' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (Move-Times (Add-Block (New-Summary)))
Test-That 'and one whose block says native but whose times are under `reconstructed`' { $r.Verdict -eq 'reconstructed' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary (Move-Times (Add-Block (New-Summary -device 'Another GPU') -provider 'engine'))
Test-That 'the refusal comes before the adapter''s skip' { $r.Verdict -eq 'reconstructed' }
Test-That 'a budget is never set from one' {
  try { Set-FrameBudgetBaseline -Run (New-Budget).runs[0] -Summary (Move-Times (Add-Block (New-Summary) -provider 'engine')) -By 'me' -Reason 'a reason' | Out-Null; $false } catch { $_.Exception.Message -match 'ADR-0064' }
}

Write-Host 'the gate over every run'
function V([string]$v) { [pscustomobject]@{ Verdict = $v } }
Test-That 'a refused summary is 5, its own code, whatever else the gate holds' {
  (Get-FrameBudgetExitCode -Results @((V 'pass'), (V 'reconstructed'))) -eq 5 -and (Get-FrameBudgetExitCode -Results @((V 'over'), (V 'reconstructed'), (V 'device'))) -eq 5
}
Test-That 'all pass is 0' { (Get-FrameBudgetExitCode -Results @((V 'pass'), (V 'pass'))) -eq 0 }
Test-That 'one over is 1' { (Get-FrameBudgetExitCode -Results @((V 'pass'), (V 'over'))) -eq 1 }
Test-That 'a failed run is 1' { (Get-FrameBudgetExitCode -Results @((V 'failed'))) -eq 1 }
Test-That 'over on a quiet machine wins over busy' { (Get-FrameBudgetExitCode -Results @((V 'busy'), (V 'over'))) -eq 1 }
Test-That 'only busy overs are 4' { (Get-FrameBudgetExitCode -Results @((V 'pass'), (V 'busy'))) -eq 4 }
Test-That 'nothing on the budget''s machine is 3' { (Get-FrameBudgetExitCode -Results @((V 'device'), (V 'device'))) -eq 3 }
Test-That 'a skipped run beside passing ones does not fail the gate' { (Get-FrameBudgetExitCode -Results @((V 'device'), (V 'pass'))) -eq 0 }
Test-That 'a run compared with nothing is 6, its own code' {
  (Get-FrameBudgetExitCode -Results @((V 'pass'), (V 'nobaseline'))) -eq 6 -and (Get-FrameBudgetExitCode -Results @((V 'session'))) -eq 6
}
Test-That 'below a failure and a refusal, above a busy over' {
  (Get-FrameBudgetExitCode -Results @((V 'nobaseline'), (V 'over'))) -eq 1 -and (Get-FrameBudgetExitCode -Results @((V 'nobaseline'), (V 'reconstructed'))) -eq 5 -and
  (Get-FrameBudgetExitCode -Results @((V 'busy'), (V 'nobaseline'))) -eq 6
}

Write-Host 'engine-view''s arguments'
$argv = Get-FrameBudgetArgs -Run $run -Root 'R:' -Jsonl 'out.jsonl' -WaitQuiet 600
Test-That 'offscreen, the benchmark, the size, the repeats, the run''s flags and the wait' {
  ($argv -join ' ') -eq ('--scene ' + [IO.Path]::Combine('R:', $run.scene) + ' --camera-path ' + [IO.Path]::Combine('R:', $run.path) +
                         ' --offscreen --benchmark out.jsonl --width 1920 --height 1080 --repeat 1 --warmup 30 --shadows csm --wait-quiet 600')
}
$wide = $run.PSObject.Copy()
$wide.views = 'surround3'
Test-That 'a surround run names its views' { ((Get-FrameBudgetArgs -Run $wide -Root 'R:' -Jsonl 'o').IndexOf('surround3')) -gt 0 }

Write-Host 'a re-baseline'
$copy = (New-Budget).runs[0]
Test-That 'refuses no reason' { try { Set-FrameBudgetBaseline -Run $copy -Summary (New-Summary) -By 'me' -Reason ' ' | Out-Null; $false } catch { $true } }
$after = Set-FrameBudgetBaseline -Run $copy -Summary (New-Summary -total 2.4567 -totalP99 4.4444) -By 'me' -Reason 'the sky got a second pass' -Commit 'abc1234' -Date '2026-10-08'
$set = $after.budgets.locked
Test-That 'takes the measured numbers to the microsecond' { $set.gpu_ms.total.median -eq 2.457 -and $set.gpu_ms.total.p99 -eq 4.444 }
Test-That 'takes every pass the run timed and leaves out the ones it did not' { $null -ne $set.gpu_ms.hw -and $null -eq $set.gpu_ms.PSObject.Properties['sw'] }
Test-That 'records who, when, why, the commit, the device, the session and the machine' {
  $set.baseline.by -eq 'me' -and $set.baseline.reason -eq 'the sky got a second pass' -and $set.baseline.commit -eq 'abc1234' -and
  $set.baseline.date -eq '2026-10-08' -and $set.baseline.device -eq 'Test GPU 9000' -and $set.baseline.session -eq 'locked' -and $set.baseline.machine_state -match '^quiet, session locked'
}
$b2 = New-Budget
$b2.runs = @($after)
Test-That 'and the result is a budget the gate reads' { try { Assert-FrameBudget $b2; $true } catch { $false } }
$beside = Set-FrameBudgetBaseline -Run (New-Budget).runs[0] -Summary (New-Summary -session 'unlocked' -total 1.1 -totalP99 1.9 -cull 0.04) -By 'me' -Reason 'the unlocked card' -Date '2026-11-17'
Test-That 'an unlocked flight takes the unlocked budget beside the locked one and leaves the locked alone' {
  $beside.budgets.unlocked.gpu_ms.total.median -eq 1.1 -and $beside.budgets.unlocked.baseline.session -eq 'unlocked' -and
  $beside.budgets.locked.gpu_ms.total.median -eq 2.0 -and $beside.budgets.locked.baseline.date -eq '2026-10-07'
}
Test-That 'a flight whose session changed is never a baseline' {
  try { Set-FrameBudgetBaseline -Run (New-Budget).runs[0] -Summary (New-Summary -session 'changed') -By 'me' -Reason 'a reason' | Out-Null; $false } catch { $_.Exception.Message -match 'changed' }
}
Test-That 'nor one that does not record it' {
  try { Set-FrameBudgetBaseline -Run (New-Budget).runs[0] -Summary (New-Summary -session 'unknown') -By 'me' -Reason 'a reason' | Out-Null; $false } catch { $_.Exception.Message -match 'unknown' }
}

Write-Host 'the frame thread''s two modes: said, never judged (T221)'
# A flight's per-frame cpu_ms as this machine draws them: a fast mode spread over 15% above `fast`,
# and `share` of the frames in stretches of a slow mode 2.5 to 2.85 times dearer (bench.md, "The
# frame thread's two modes"). Deterministic: the spread is a function of the frame's index. `add`
# is a cost on the frames `regressed` names (a frame index to a bool), on top of either mode.
function New-Frames([double]$fast, [double]$share, [int]$n = 2000, [double]$add = 0.0, [scriptblock]$regressed = { $false }) {
  $slowEvery = if ($share -gt 0) { [int][math]::Round(1.0 / $share * 10) } else { 0 }
  $frames = [double[]]::new($n)
  for ($i = 0; $i -lt $n; $i++) {
    $jitter = (($i * 37) % 100) / 100.0
    # Stretches: ten frames in each block of `slowEvery` are slow, so the share is 10 / slowEvery.
    $slow = $slowEvery -gt 0 -and ($i % $slowEvery) -lt 10
    $frames[$i] = if ($slow) { $fast * (2.5 + 0.35 * $jitter) } else { $fast * (1.0 + 0.15 * $jitter) }
    if (& $regressed $i) { $frames[$i] += $add }
  }
  return , $frames
}
function Get-WholeMedian([double[]]$frames) { $s = [double[]]$frames.Clone(); [Array]::Sort($s); $s[[int][math]::Floor($s.Count / 2)] }
$m = Get-CpuModes -Frames (New-Frames 0.5 0.5)
Test-That 'the modes are told apart as numbers: the share, the fast median, the slow median' {
  $m.SlowShare -is [double] -and [math]::Abs($m.SlowShare - 0.5) -lt 0.01 -and $m.FastMedian -ge 0.5 -and $m.FastMedian -le 0.575 -and $m.SlowMedian -ge 1.25
}
Test-That 'a flight with no slow frame has its fast median as its median, and no slow mode' {
  $m0 = Get-CpuModes -Frames (New-Frames 0.5 0.0); $m0.SlowShare -eq 0 -and $m0.FastMedian -eq $m0.WholeMedian -and $null -eq $m0.SlowMedian
}
Test-That 'and a flight with no frame records has none' { $null -eq (Get-CpuModes -Frames @()) }

# The overlook's level: its unlocked cpu_ms median, 0.091 ms, so the limit is 0.091 x 1.25 + 0.1 =
# 0.21375 ms, and a fast mode of 0.095 to 0.109 ms as the overlook flies. A judge that held the
# fast mode's median would pass every one of these regressions (T221's review): each is over on a
# quiet machine because the whole median is judged, the records given or not.
$ov = New-Budget
$ov.runs[0].budgets.locked.cpu_ms.median = 0.091
$ovRun = $ov.runs[0]
$ovLimit = 0.091 * 1.25 + 0.1
function Test-Regression([string]$what, [double[]]$frames) {
  $whole = Get-WholeMedian $frames
  $r = Test-FrameBudgetRun -Budget $ov -Run $ovRun -Summary (New-Summary -cpu $whole) -Frames $frames
  Test-That $what { $whole -gt $ovLimit -and $r.Verdict -eq 'over' -and (Row $r 'cpu_ms' 'median').Over -and (Row $r 'cpu_ms' 'median').Measured -eq $whole }
}
Test-Regression 'a cost of 0.15 ms on every second frame at the overlook''s level is over' `
  (New-Frames 0.095 0.0 -add 0.15 -regressed { param($i) $i % 2 -eq 1 })
Test-Regression 'the same on every third frame, where the machine''s slow share puts the whole median over, is over' `
  (New-Frames 0.095 0.3 -add 0.15 -regressed { param($i) $i % 3 -eq 2 })
$bimodal = [double[]]::new(2000)
for ($i = 0; $i -lt 2000; $i++) { $bimodal[$i] = if (($i % 20) -lt 9) { 0.05 + 0.002 * (($i * 37) % 10) / 10.0 } else { 0.25 + 0.002 * (($i * 37) % 10) / 10.0 } }
Test-Regression 'a flight of 45% frames at 0.05 ms and 55% at 0.25 ms against the overlook''s limit is over' $bimodal
Test-Regression 'a regression on 70% of the frames is over, not inconclusive' `
  (New-Frames 0.095 0.0 -add 0.15 -regressed { param($i) ($i % 10) -lt 7 })

# What the modes are for: said beside a verdict, never moving it. The same summaries with and without
# their records give the same verdict and the same rows, across passes, overs and a busy machine.
$same = $true
$cases = @(
  @{ frames = (New-Frames 0.095 0.0); quiet = $true }
  @{ frames = (New-Frames 0.095 0.53); quiet = $true }
  @{ frames = (New-Frames 0.095 0.2); quiet = $false }
  @{ frames = (New-Frames 0.095 0.0 -add 0.15 -regressed { param($i) $i % 2 -eq 1 }); quiet = $false }
  @{ frames = $bimodal; quiet = $true }
)
foreach ($c in $cases) {
  $s = New-Summary -cpu (Get-WholeMedian $c.frames) -quiet $c.quiet
  $with = Test-FrameBudgetRun -Budget $ov -Run $ovRun -Summary $s -Frames $c.frames
  $without = Test-FrameBudgetRun -Budget $ov -Run $ovRun -Summary $s
  $same = $same -and $with.Verdict -eq $without.Verdict -and $with.Rows.Count -eq $without.Rows.Count -and $null -ne $with.Modes -and $null -eq $without.Modes
  for ($k = 0; $k -lt $with.Rows.Count; $k++) {
    $same = $same -and $with.Rows[$k].Measured -eq $without.Rows[$k].Measured -and $with.Rows[$k].Over -eq $without.Rows[$k].Over
  }
}
Test-That 'the slow share and the modes are said and change no verdict and no row' { $same }
$r = Test-FrameBudgetRun -Budget $ov -Run $ovRun -Summary (New-Summary -cpu (Get-WholeMedian $cases[1].frames)) -Frames $cases[1].frames
$said = (Format-FrameBudgetReport -Results @($r) -Budget $ov) -join "`n"
Test-That 'a slow share over half puts the whole median over: over, and the report says the share' {
  $r.Verdict -eq 'over' -and $said -match 'slow share 5[23]\.\d%' -and $said -match 'whole median 0\.2\d\d ms judged'
}

Write-Host 'the frame thread pinned (T221)'
Test-That 'the rule takes the first logical CPU of the third core, from made-up counts' {
  (Get-FrameCpuChoice -Cores 18 -Logical 36) -eq 4 -and (Get-FrameCpuChoice -Cores 8 -Logical 8) -eq 2 -and
  (Get-FrameCpuChoice -Cores 64 -Logical 128) -eq 4 -and (Get-FrameCpuChoice -Cores 4 -Logical 16) -eq 8
}
Test-That 'and the last core of a smaller machine' { (Get-FrameCpuChoice -Cores 2 -Logical 4) -eq 2 -and (Get-FrameCpuChoice -Cores 1 -Logical 1) -eq 0 }
Test-That '-FrameCpu free is free, a number is that CPU, auto is the rule, and auto where no pin can be made is free' {
  (Resolve-FrameCpu 'free' 18 36) -eq -1 -and (Resolve-FrameCpu '7' 18 36) -eq 7 -and (Resolve-FrameCpu 'auto' 18 36) -eq 4 -and
  (Resolve-FrameCpu 'auto' 18 36 $false) -eq -1
}
Test-That 'and a CPU the machine does not have, or a word, is refused' {
  $a = try { Resolve-FrameCpu '36' 18 36; $false } catch { $true }
  $b = try { Resolve-FrameCpu 'fast' 18 36; $false } catch { $true }
  $a -and $b
}
$calm = New-Summary
$failed = [pscustomobject]@{ Asked = $true; Cpu = 4; Thread = 0; Applied = $false; Held = $false; Looks = 0; Why = 'OpenThread failed (5)' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary $calm -Pin $failed
Test-That 'a pin that could not be applied makes a passing flight inconclusive, and says so' {
  $r.Verdict -eq 'busy' -and $r.MachineState -match 'could not be pinned to CPU 4: OpenThread failed' -and $r.MachineState -match 'inconclusive' -and -not $r.Pin.Applied
}
$lost = [pscustomobject]@{ Asked = $true; Cpu = 4; Thread = 12; Applied = $true; Held = $false; Looks = 9; Why = 'a look found the mask at 0xffff' }
Test-That 'and so does one that did not hold' { (Test-FrameBudgetRun -Budget $b -Run $run -Summary $calm -Pin $lost).Verdict -eq 'busy' }
Test-That 'but an over on a quiet machine stays over, as a free flight is' { (Test-FrameBudgetRun -Budget $b -Run $run -Summary (New-Summary -total 2.26) -Pin $failed).Verdict -eq 'over' }
$held = [pscustomobject]@{ Asked = $true; Cpu = 4; Thread = 12; Applied = $true; Held = $true; Looks = 40; Why = '' }
$r = Test-FrameBudgetRun -Budget $b -Run $run -Summary $calm -Pin $held
Test-That 'a pin that held is judged as any flight, and the report records the CPU and that it held' {
  $r.Verdict -eq 'pass' -and $r.MachineState -match 'pinned to CPU 4, held \(40 looks\)' -and $r.Pin.Cpu -eq 4 -and $r.Pin.Held
}
if ($IsWindows) {
  # The plumbing on a real process with no device: a pwsh that sleeps, its first thread pinned and
  # looked at (the mask found as set), then let go.
  $child = Start-Process pwsh -ArgumentList '-NoProfile', '-Command', 'Start-Sleep -Seconds 4' -PassThru -WindowStyle Hidden
  $live = Set-FrameThreadPin -Process $child -Cpu 0
  Test-FrameThreadPin -Pin $live -Process $child
  Close-FrameThreadPin $live
  $child.WaitForExit()
  Test-That 'a live process''s first thread is pinned, and a look finds the mask it was given' { $live.Applied -and $live.Held -and $live.Looks -eq 1 -and $live.Thread -gt 0 }
}

Write-Host 'a budget file the gate refuses'
$bad = New-Budget
$bad.runs[0].budgets.locked.baseline = [pscustomobject]@{ date = '2026-10-07'; by = 'test' }
Test-That 'a run whose baseline does not say why' { try { Assert-FrameBudget $bad; $false } catch { $_.Exception.Message -match 'why' } }
$bad = New-Budget
$bad.runs = @($bad.runs[0], $bad.runs[0])
Test-That 'two runs with one id' { try { Assert-FrameBudget $bad; $false } catch { $_.Exception.Message -match 'twice' } }
$bad = New-Budget
$bad.runs[0].budgets.locked.gpu_ms.cull = [pscustomobject]@{ median = 0.3 }
Test-That 'a line without its p99' { try { Assert-FrameBudget $bad; $false } catch { $_.Exception.Message -match 'p99' } }
$bad = New-Budget
$bad.runs[0] | Add-Member -NotePropertyName gpu_ms -NotePropertyValue ([pscustomobject]@{ total = [pscustomobject]@{ median = 1; p99 = 2 } })
Test-That 'a budget outside a session state (the v1 shape)' { try { Assert-FrameBudget $bad; $false } catch { $_.Exception.Message -match 'budgets\.<session>' } }
$bad = New-Budget
$bad.runs[0].budgets = [pscustomobject]@{ asleep = (New-Set) }
Test-That 'a session state that is not locked or unlocked' { try { Assert-FrameBudget $bad; $false } catch { $_.Exception.Message -match 'not a session state' } }
$bad = New-Budget
$bad.runs[0].budgets = [pscustomobject]@{}
Test-That 'a run with no budget at all' { try { Assert-FrameBudget $bad; $false } catch { $_.Exception.Message -match 'no budgets' } }
$bad = New-Budget
$bad.format = 'engine.frame-budgets.v1'
Test-That 'the v1 format' { try { Assert-FrameBudget $bad; $false } catch { $_.Exception.Message -match 'v1' } }

Write-Host 'the committed budget file'
$committed = Join-Path $Root 'content/budgets/frame-budgets.json'
Test-That 'reads, and every run names a scene and a path that exist' {
  $plan = Read-FrameBudget -Path $committed
  @($plan.runs).Count -gt 0 -and -not (@($plan.runs) | Where-Object { -not (Test-Path (Join-Path $Root $_.scene)) -or -not (Test-Path (Join-Path $Root $_.path)) })
}
Test-That 'and every baseline in it says which session state it was flown in' {
  $plan = Read-FrameBudget -Path $committed
  -not (@($plan.runs) | ForEach-Object { $run = $_; $run.budgets.PSObject.Properties | Where-Object { $_.Value.baseline.session -ne $_.Name } })
}

Write-Host 'the script over summaries it did not fly'
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("engine-frame-budget-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
try {
  $budgetFile = Join-Path $scratch 'budgets.json'
  (New-Budget) | ConvertTo-Json -Depth 12 | Set-Content $budgetFile
  function Write-Run([string]$dir, $summary) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    @('{"repeat":0,"frame":0}', ($summary | ConvertTo-Json -Depth 12 -Compress)) | Set-Content (Join-Path $dir 'scene-1080p.jsonl')
  }
  $within = Join-Path $scratch 'within'
  Write-Run $within (New-Summary)
  & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $within *> $null
  Test-That 'a run within budget exits 0 and writes its report' { $LASTEXITCODE -eq 0 -and (Test-Path (Join-Path $within 'report.md')) }
  $overDir = Join-Path $scratch 'over'
  Write-Run $overDir (New-Summary -cull 0.9)
  $said = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $overDir 2>&1 | Out-String
  Test-That 'a run over budget exits 1 and names the pass' { $LASTEXITCODE -eq 1 -and $said -match 'over: gpu_ms\.cull median' }
  & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $overDir -Rebaseline *> $null
  Test-That 'a re-baseline without a reason is refused and leaves the file alone' {
    $LASTEXITCODE -ne 0 -and (Read-FrameBudget -Path $budgetFile).runs[0].budgets.locked.gpu_ms.cull.median -eq 0.3
  }
  & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $overDir -Rebaseline -Reason 'the cull does more now' -By 'tester' *> $null
  $now = Read-FrameBudget -Path $budgetFile
  Test-That 'one with a reason writes the measured numbers and the record' {
    $LASTEXITCODE -eq 0 -and $now.runs[0].budgets.locked.gpu_ms.cull.median -eq 0.9 -and $now.runs[0].budgets.locked.baseline.by -eq 'tester' -and $now.runs[0].budgets.locked.baseline.reason -eq 'the cull does more now'
  }

  # The session state through the script (T161): the budget holds the locked set re-baselined just
  # above; an unlocked flight finds no unlocked set, says so and exits 6; a re-baseline from it takes
  # the unlocked set beside the locked one; the same flight then passes, and a locked flight is still
  # held to the locked numbers.
  $unlockedDir = Join-Path $scratch 'unlocked'
  Write-Run $unlockedDir (New-Summary -session 'unlocked' -total 0.8 -totalP99 1.5 -cull 0.04)
  $said = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $unlockedDir 2>&1 | Out-String
  Test-That 'an unlocked flight against a locked budget only exits 6 and says there is no unlocked baseline' {
    $LASTEXITCODE -eq 6 -and $said -match 'nobaseline' -and $said -match 'no unlocked baseline' -and $said -match 'INCONCLUSIVE'
  }
  $changedDir = Join-Path $scratch 'changed'
  Write-Run $changedDir (New-Summary -session 'changed')
  & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $changedDir -Rebaseline -Reason 'mixed' -By 'tester' *> $null
  Test-That 'a re-baseline from a flight whose session changed is refused and writes nothing' {
    $LASTEXITCODE -ne 0 -and $null -eq (Read-FrameBudget -Path $budgetFile).runs[0].budgets.PSObject.Properties['unlocked']
  }
  & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $unlockedDir -Rebaseline -Reason 'the state a player is in' -By 'tester' *> $null
  $now = Read-FrameBudget -Path $budgetFile
  Test-That 'a re-baseline unlocked stores the unlocked set beside the locked one' {
    $LASTEXITCODE -eq 0 -and $now.runs[0].budgets.unlocked.gpu_ms.total.median -eq 0.8 -and $now.runs[0].budgets.unlocked.baseline.session -eq 'unlocked' -and
    $now.runs[0].budgets.locked.gpu_ms.cull.median -eq 0.9
  }
  & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $unlockedDir *> $null
  Test-That 'after which the unlocked flight passes' { $LASTEXITCODE -eq 0 }
  $lockedSlow = Join-Path $scratch 'locked-slow'
  Write-Run $lockedSlow (New-Summary -session 'locked' -cull 1.5)
  $said = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $lockedSlow 2>&1 | Out-String
  Test-That 'and a locked flight is still held to the locked numbers' { $LASTEXITCODE -eq 1 -and $said -match 'over: gpu_ms\.cull median' -and $said -match 'the locked baseline' }

  # The frame thread's modes through the script (T221), with the records beside the summary as
  # engine-view writes them, re-read from where they are kept into a report directory of its own.
  # The budget file's locked cpu_ms median is still the test's 0.5 (limit 0.725).
  function Write-Records([string]$dir, [double[]]$frames) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $lines = for ($i = 0; $i -lt $frames.Count; $i++) { '{"frame":' + $i + ',"cpu":{"passes":0.01},"cpu_ms":' + $frames[$i].ToString('R', [Globalization.CultureInfo]::InvariantCulture) + ',"repeat":0}' }
    @($lines) + @((New-Summary -cpu (Get-WholeMedian $frames)) | ConvertTo-Json -Depth 12 -Compress) | Set-Content (Join-Path $dir 'scene-1080p.jsonl')
  }
  $kept = Join-Path $scratch 'kept-slow'
  Write-Records $kept (New-Frames 0.5 0.53)
  Test-That 'Read-FrameCpu reads every frame record and leaves the summary out' { (Read-FrameCpu -Path (Join-Path $kept 'scene-1080p.jsonl')).Count -eq 2000 }
  $reportDir = Join-Path $scratch 'kept-slow-report'
  $said = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $kept -Out $reportDir 2>&1 | Out-String
  Test-That 'a kept flight whose whole median sits in the slow mode is over by it, says the share, and is reported elsewhere' {
    $LASTEXITCODE -eq 1 -and $said -match 'over: cpu_ms median' -and $said -match 'slow share 5[23]\.\d%' -and
    (Test-Path (Join-Path $reportDir 'report.md')) -and -not (Test-Path (Join-Path $kept 'report.md'))
  }
  Test-That 'and report.json carries the modes as numbers' {
    $j = Get-Content (Join-Path $reportDir 'report.json') -Raw | ConvertFrom-Json
    $mo = @($j)[0].Modes; $mo.SlowShare -is [double] -and $mo.FastMedian -is [double] -and $mo.WholeMedian -gt 0.725
  }
  $calm = Join-Path $scratch 'kept-calm'
  Write-Records $calm (New-Frames 0.5 0.0)
  $said = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $calm -Out (Join-Path $scratch 'kept-calm-report') 2>&1 | Out-String
  Test-That 'one with no slow frame passes and says a share of none' { $LASTEXITCODE -eq 0 -and $said -match 'slow share 0\.0%' }

  # The measurement rule through the scripts (ADR-0064). The budget file is the re-baselined one.
  $recon = Join-Path $scratch 'reconstructed'
  Write-Run $recon (Move-Times (Add-Block (New-Summary -total 0.1 -cull 0.01) -provider 'engine'))
  $said = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $recon 2>&1 | Out-String
  Test-That 'frame-budget.ps1 refuses a reconstructed summary with exit 5 and says why' { $LASTEXITCODE -eq 5 -and $said -match 'refused: reconstructed \(engine' -and $said -match 'REFUSED' }
  & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $recon -Rebaseline -Reason 'it looked fast' -By 'tester' *> $null
  Test-That 'and never re-baselines from it' { $LASTEXITCODE -ne 0 -and (Read-FrameBudget -Path $budgetFile).runs[0].budgets.locked.gpu_ms.cull.median -eq 0.9 }

  # tools/flythrough.ps1 over a stand-in for engine-view that prints one summary line: the matrix
  # puts a native run's numbers in its table and refuses a reconstructed one.
  $stub = Join-Path $scratch 'view-stub.ps1'
  $line = { param($s) $s | Add-Member -NotePropertyName views -NotePropertyValue 'single' -PassThru | ConvertTo-Json -Depth 12 -Compress }
  $nativeLine = & $line (Add-Block (New-Summary))
  $reconLine = & $line (Move-Times (Add-Block (New-Summary) -provider 'engine'))
  @("if (`$env:ENGINE_TEST_STUB_RECONSTRUCTED -eq '1') { @'", $reconLine, "'@ } else { @'", $nativeLine, "'@ }", 'exit 0') | Set-Content $stub
  $flyArgs = @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'flythrough.ps1'), '-Exe', $stub, '-Resolutions', '640x360', '-Occlusion', 'on', '-WaitQuiet', '0')
  $env:ENGINE_TEST_STUB_RECONSTRUCTED = '0'
  $nativeOut = Join-Path $scratch 'fly-native'
  & pwsh @flyArgs -Out $nativeOut *> $null
  Test-That 'flythrough.ps1 tabulates a native run' { $LASTEXITCODE -eq 0 -and (Get-Content (Join-Path $nativeOut 'summary.md') -Raw) -match '\| 2\.000 \|' }
  $env:ENGINE_TEST_STUB_RECONSTRUCTED = '1'
  $reconOut = Join-Path $scratch 'fly-reconstructed'
  & pwsh @flyArgs -Out $reconOut *> $null
  $table = Get-Content (Join-Path $reconOut 'summary.md') -Raw
  Test-That 'flythrough.ps1 refuses a reconstructed run: exit 1, a row that says refused, and no number of it' { $LASTEXITCODE -eq 1 -and $table -match 'refused: reconstructed \(engine' -and $table -notmatch '\| 2\.000 \|' }
  Remove-Item Env:ENGINE_TEST_STUB_RECONSTRUCTED -ErrorAction SilentlyContinue
} finally {
  Remove-Item -Recurse -Force -LiteralPath $scratch -ErrorAction SilentlyContinue
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed" -ForegroundColor Red
  exit 1
}
Write-Host "all $checks checks passed"
exit 0

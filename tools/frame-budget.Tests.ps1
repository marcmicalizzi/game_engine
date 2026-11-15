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
  records who and why and refuses no reason. Then the committed budget file is read, so a malformed
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

function New-Budget {
  [pscustomobject]@{
    format = 'engine.frame-budgets.v1'
    machine = [pscustomobject]@{ device = 'Test GPU 9000'; name = 'the test machine'; preset = 'msvc-release' }
    tolerance = [pscustomobject]@{
      gpu = [pscustomobject]@{ median = [pscustomobject]@{ pct = 10; ms = 0.05 }; p99 = [pscustomobject]@{ pct = 20; ms = 0.1 } }
      cpu = [pscustomobject]@{ median = [pscustomobject]@{ pct = 25; ms = 0.1 }; p99 = [pscustomobject]@{ pct = 50; ms = 0.5 } }
    }
    runs = @([pscustomobject]@{
      id = 'scene-1080p'; scene = 'content/test-scenes/x/scene.json'; path = 'content/test-scenes/x/camera-path.json'
      width = 1920; height = 1080; views = 'single'; repeat = 1; warmup = 30; args = @('--shadows', 'csm')
      gpu_ms = [pscustomobject]@{
        total = [pscustomobject]@{ median = 2.0; p99 = 4.0 }
        cull = [pscustomobject]@{ median = 0.3; p99 = 0.4 }
      }
      cpu_ms = [pscustomobject]@{ median = 0.5; p99 = 1.0 }
      baseline = [pscustomobject]@{ date = '2026-10-07'; by = 'test'; reason = 'the test''s own' }
    })
  }
}
function New-Summary([double]$total = 2.0, [double]$totalP99 = 4.0, [double]$cull = 0.3, [bool]$quiet = $true,
                     [string]$device = 'Test GPU 9000', [double]$cpu = 0.5) {
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
      start = [pscustomobject]@{ cpu_others_pct = 3.0; gpu_util_pct = 2 }
      end = [pscustomobject]@{ cpu_others_pct = 4.0; gpu_util_pct = 5 }
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
Test-That 'the machine state is one line with both ends' { (Format-MachineState (New-Summary)) -match '^quiet \(start 3\.0% others, GPU 2%; end 4\.0% others, GPU 5%\)$' }

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
Test-That 'takes the measured numbers to the microsecond' { $after.gpu_ms.total.median -eq 2.457 -and $after.gpu_ms.total.p99 -eq 4.444 }
Test-That 'takes every pass the run timed and leaves out the ones it did not' { $null -ne $after.gpu_ms.hw -and $null -eq $after.gpu_ms.PSObject.Properties['sw'] }
Test-That 'records who, when, why, the commit, the device and the machine' {
  $after.baseline.by -eq 'me' -and $after.baseline.reason -eq 'the sky got a second pass' -and $after.baseline.commit -eq 'abc1234' -and
  $after.baseline.date -eq '2026-10-08' -and $after.baseline.device -eq 'Test GPU 9000' -and $after.baseline.machine_state -match '^quiet'
}
$b2 = New-Budget
$b2.runs = @($after)
Test-That 'and the result is a budget the gate reads' { try { Assert-FrameBudget $b2; $true } catch { $false } }

Write-Host 'a budget file the gate refuses'
$bad = New-Budget
$bad.runs[0].baseline = [pscustomobject]@{ date = '2026-10-07'; by = 'test' }
Test-That 'a run whose baseline does not say why' { try { Assert-FrameBudget $bad; $false } catch { $_.Exception.Message -match 'why' } }
$bad = New-Budget
$bad.runs = @($bad.runs[0], $bad.runs[0])
Test-That 'two runs with one id' { try { Assert-FrameBudget $bad; $false } catch { $_.Exception.Message -match 'twice' } }
$bad = New-Budget
$bad.runs[0].gpu_ms.cull = [pscustomobject]@{ median = 0.3 }
Test-That 'a line without its p99' { try { Assert-FrameBudget $bad; $false } catch { $_.Exception.Message -match 'p99' } }

Write-Host 'the committed budget file'
$committed = Join-Path $Root 'content/budgets/frame-budgets.json'
Test-That 'reads, and every run names a scene and a path that exist' {
  $plan = Read-FrameBudget -Path $committed
  @($plan.runs).Count -gt 0 -and -not (@($plan.runs) | Where-Object { -not (Test-Path (Join-Path $Root $_.scene)) -or -not (Test-Path (Join-Path $Root $_.path)) })
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
    $LASTEXITCODE -ne 0 -and (Read-FrameBudget -Path $budgetFile).runs[0].gpu_ms.cull.median -eq 0.3
  }
  & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $overDir -Rebaseline -Reason 'the cull does more now' -By 'tester' *> $null
  $now = Read-FrameBudget -Path $budgetFile
  Test-That 'one with a reason writes the measured numbers and the record' {
    $LASTEXITCODE -eq 0 -and $now.runs[0].gpu_ms.cull.median -eq 0.9 -and $now.runs[0].baseline.by -eq 'tester' -and $now.runs[0].baseline.reason -eq 'the cull does more now'
  }

  # The measurement rule through the scripts (ADR-0064). The budget file is the re-baselined one.
  $recon = Join-Path $scratch 'reconstructed'
  Write-Run $recon (Move-Times (Add-Block (New-Summary -total 0.1 -cull 0.01) -provider 'engine'))
  $said = & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $recon 2>&1 | Out-String
  Test-That 'frame-budget.ps1 refuses a reconstructed summary with exit 5 and says why' { $LASTEXITCODE -eq 5 -and $said -match 'refused: reconstructed \(engine' -and $said -match 'REFUSED' }
  & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'frame-budget.ps1') -Budget $budgetFile -FromSummaries $recon -Rebaseline -Reason 'it looked fast' -By 'tester' *> $null
  Test-That 'and never re-baselines from it' { $LASTEXITCODE -ne 0 -and (Read-FrameBudget -Path $budgetFile).runs[0].gpu_ms.cull.median -eq 0.9 }

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

#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/lib/DiskWrites.psm1, the arithmetic of tools/disk-writes.ps1. Runs under CTest as
  `tools.disk_writes`.

.DESCRIPTION
  Canned counter samples, shaped as Get-Counter's sample sets are, so no performance counter is
  read and the cases run on any system: the paths parse with and without a host and with an
  instance holding parentheses, numbered processes add up under their image, a sample's bytes are
  its rate times the seconds since the sample before it, `_total` and the idle process are left
  out, and the report prints megabytes with the named processes (a name not seen as 0.0).

      pwsh tools/disk-writes.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/DiskWrites.psm1') -Force
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

Write-Host 'disk-writes tests'

$p = ConvertFrom-CounterPath '\\BOX\physicaldisk(4 c:)\disk write bytes/sec'
Test-That 'a path with a host parses into object, instance and counter' { $p.Object -eq 'physicaldisk' -and $p.Instance -eq '4 c:' -and $p.Counter -eq 'disk write bytes/sec' }
$p = ConvertFrom-CounterPath '\Process(cl#12)\IO Write Bytes/sec'
Test-That 'a path without a host parses, lower case' { $p.Object -eq 'process' -and $p.Instance -eq 'cl#12' -and $p.Counter -eq 'io write bytes/sec' }
$p = ConvertFrom-CounterPath '\\box\process(setup (x86))\io write bytes/sec'
Test-That 'an instance holding parentheses runs to the last one' { $p.Instance -eq 'setup (x86)' }
$p = ConvertFrom-CounterPath '\\box\memory\committed bytes'
Test-That 'a counter with no instance has an empty one' { $p.Object -eq 'memory' -and $p.Instance -eq '' -and $p.Counter -eq 'committed bytes' }
Test-That 'something that is not a counter path is $null' { $null -eq (ConvertFrom-CounterPath 'not a path') }
Test-That 'numbered processes take their image name' { (Get-ProcessBaseName 'cl#3') -eq 'cl' -and (Get-ProcessBaseName 'nvdisplay.container') -eq 'nvdisplay.container' }

$t0 = [datetime]'2026-11-18T20:00:00'
function New-Sample([string]$path, [double]$rate) { [pscustomobject]@{ Path = $path; CookedValue = $rate } }
$sets = @(
  [pscustomobject]@{ Timestamp = $t0; Samples = @(
      (New-Sample '\\box\physicaldisk(0)\disk write bytes/sec' 1e6),
      (New-Sample '\\box\physicaldisk(4 c:)\disk write bytes/sec' 2e6),
      (New-Sample '\\box\physicaldisk(_total)\disk write bytes/sec' 3e6),
      (New-Sample '\\box\logicaldisk(d:)\disk write bytes/sec' 1e6),
      (New-Sample '\\box\process(cl)\io write bytes/sec' 1e5),
      (New-Sample '\\box\process(cl#1)\io write bytes/sec' 2e5),
      (New-Sample '\\box\process(idle)\io write bytes/sec' 9e9),
      (New-Sample '\\box\process(_total)\io write bytes/sec' 9e9),
      (New-Sample '\\box\process(dwm)\io write bytes/sec' 0)
    ) },
  # 30 s after the first: the second sample stands for 30 s, not the 15 of the interval.
  [pscustomobject]@{ Timestamp = $t0.AddSeconds(30); Samples = @(
      (New-Sample '\\box\physicaldisk(0)\disk write bytes/sec' 1e6),
      (New-Sample '\\box\process(cl#7)\io write bytes/sec' 1e5),
      (New-Sample '\\box\process(link)\io write bytes/sec' 5e5)
    ) }
)
$m = Measure-CounterWrites -Sets $sets -IntervalSeconds 15
Test-That 'the window is the first interval plus the gap to the second sample' { $m.Seconds -eq 45 -and $m.Samples -eq 2 }
Test-That 'a disk adds rate times seconds over the samples' { $m.Disks['0'] -eq 45e6 -and $m.Disks['4 c:'] -eq 30e6 }
Test-That '_total is left out of disks and processes' { -not $m.Disks.Contains('_total') -and -not $m.Processes.Contains('_total') }
Test-That 'a volume is counted apart from the disks' { $m.Volumes['d:'] -eq 15e6 -and $m.Volumes.Count -eq 1 }
Test-That 'numbered compilers add up under one name' { $m.Processes['cl'] -eq (1e5 * 15 + 2e5 * 15 + 1e5 * 30) }
Test-That 'the idle process and a process that wrote nothing are left out' { -not $m.Processes.Contains('idle') -and -not $m.Processes.Contains('dwm') }
Test-That 'processes come largest first' { @($m.Processes.Keys)[0] -eq 'link' -and @($m.Processes.Keys)[1] -eq 'cl' }

$lines = Format-DiskWritesReport -Measure $m -Top 1 -Names @('link.exe', 'MsMpEng') -Title 't'
$text = $lines -join "`n"
Test-That 'the report states the window' { $lines[0] -eq 'disk-writes: 45 s in 2 samples, t' }
Test-That 'the report prints megabytes per disk' { $text -match '(?m)^  0\s+45\.0$' -and $text -match '(?m)^  4 c:\s+30\.0$' }
Test-That '-Top limits the process list' { $text -match '(?m)^  link\s+15\.0$' -and $text -notmatch '(?m)^  cl\s' }
Test-That 'a named process is found without .exe and in any case, and one not seen is 0.0' { $text -match '(?m)named processes[^\n]*\n  link\s+15\.0\n  msmpeng\s+0\.0$' }

$empty = Measure-CounterWrites -Sets @() -IntervalSeconds 15
Test-That 'no samples is an empty window, not an error' { $empty.Seconds -eq 0 -and $empty.Disks.Count -eq 0 -and $empty.Processes.Count -eq 0 }

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed:" -ForegroundColor Red
  foreach ($f in $failures) { Write-Host "  - $f" }
  exit 1
}
Write-Host "disk-writes: $checks checks passed" -ForegroundColor Green
exit 0

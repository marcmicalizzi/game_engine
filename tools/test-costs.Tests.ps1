#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/test-costs.ps1. Runs under CTest as `tools.test_costs`.

.DESCRIPTION
  Writes a CTest log in the shape LastTest.log has — two parts of one parted test, a test the run
  does not ask about, doctest's `--duration=true` lines — and a case-to-file map, runs the tool, and
  reads the table it wrote: the seconds summed per file across the parts, rounded to whole seconds
  and never to 0, the rows ordered longest first, the leading comment and another test's rows kept,
  the named test's old rows replaced, a case no file holds left out and counted. Then the refusals:
  no log, a log without the test, a log without durations (exit 1), and no arguments (exit 2).
  Every case writes into its own directory under the system temp directory, named by a GUID, so
  two copies of this script never share one.
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'
$tool = Join-Path $PSScriptRoot 'test-costs.ps1'
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
$scratch = Join-Path ([IO.Path]::GetTempPath()) "engine-test-costs-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

function Write-Text([string]$path, [string]$text) {
  [IO.File]::WriteAllText($path, ($text -replace "`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
}

function Invoke-Tool([string[]]$toolArgs) {
  $out = (& pwsh -NoProfile -File $tool @toolArgs *>&1 | Out-String)
  return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}

$log = @'
Start testing: Nov 10 01:00 Eastern Standard Time
----------------------------------------------------------
1/3 Testing: renderer.1
1/3 Test: renderer.1
Command: "D:/w/build/msvc-debug/systems/renderer/engine_renderer_tests.exe" "--duration=true" "--source-file=*a_tests.cpp,*c_tests.cpp"
Directory: D:/w/build/msvc-debug/systems/renderer
"renderer.1" start time: Nov 10 01:00 Eastern Standard Time
Output:
----------------------------------------------------------
01:00:01.000 info  renderer [t0] scene loaded ms=12.0
12.400000 s: a: the first case
0.300000 s: a: the second case
0.200000 s: c: the only case
0.100000 s: a case in no file
[doctest] test cases: 4 | 4 passed | 0 failed | 0 skipped
<end of output>
Test time =  13.10 sec
----------------------------------------------------------
Test Passed.
2/3 Testing: flora
2/3 Test: flora
Command: "D:/w/build/msvc-debug/domain/flora/engine_flora_tests.exe" "--duration=true"
Output:
----------------------------------------------------------
99.000000 s: flora: not asked about
<end of output>
Test time =  99.00 sec
----------------------------------------------------------
Test Passed.
3/3 Testing: renderer.2
3/3 Test: renderer.2
Command: "D:/w/build/msvc-debug/systems/renderer/engine_renderer_tests.exe" "--duration=true" "--source-file=*b_tests.cpp"
Output:
----------------------------------------------------------
gpu-lock: waiting for the GPU lock, held by 'someone' (pid 1): a render (until 2026-11-10T01:30:00Z); gives up at 2026-11-10T01:30:00Z
gpu-lock: taken after 600 s
640.000000 s: b: the long one
1.600000 s: b: and another
<end of output>
Test time =  41.70 sec
----------------------------------------------------------
Test Passed.
'@

$map = @'
{ "renderer": { "a: the first case": "a_tests.cpp", "a: the second case": "a_tests.cpp",
                "c: the only case": "c_tests.cpp", "b: the long one": "b_tests.cpp",
                "b: and another": "b_tests.cpp" } }
'@

try {
  Write-Host 'test-costs tests'
  $logPath = Join-Path $scratch 'LastTest.log'
  $mapPath = Join-Path $scratch 'map.json'
  $table = Join-Path $scratch 'test-costs.txt'
  Write-Text $logPath $log
  Write-Text $mapPath $map
  Write-Text $table "# the table's comment`n#   <test> <basename> <whole seconds>`n`ngfx raster_tests.cpp 30`nrenderer old_tests.cpp 7`n"

  $r = Invoke-Tool @('-Log', $logPath, '-Tests', 'renderer', '-CaseFiles', $mapPath, '-Out', $table)
  $rows = @([IO.File]::ReadAllLines($table))
  Test-That 'it writes the table and exits 0' { $r.Code -eq 0 }
  Test-That 'the comment block is kept, first' { $rows[0] -eq "# the table's comment" -and $rows[1] -eq '#   <test> <basename> <whole seconds>' }
  Test-That "another test's rows are kept" { $rows -contains 'gfx raster_tests.cpp 30' }
  Test-That "the named test's old rows are replaced" { -not ($rows -contains 'renderer old_tests.cpp 7') }
  Test-That 'seconds are summed per file across the parts, longest first' {
    $mine = @($rows | Where-Object { $_ -like 'renderer *' })
    ($mine -join '|') -eq 'renderer b_tests.cpp 42|renderer a_tests.cpp 13|renderer c_tests.cpp 1'
  }
  Test-That 'a file that took a fraction of a second still counts, as 1' { $rows -contains 'renderer c_tests.cpp 1' }
  Test-That "a wait for the GPU lock comes off the case it was in (640 s less 600 waited)" { $rows -contains 'renderer b_tests.cpp 42' }
  Test-That 'a test the run did not ask about is not read' { -not ($rows | Where-Object { $_ -like 'flora *' }) }
  Test-That 'a case no file holds is left out and counted' { $r.Out.Contains("'a case in no file' is in no file") -and $r.Out.Contains('1 case(s) left out') }

  $r = Invoke-Tool @('-Log', (Join-Path $scratch 'missing.log'), '-Tests', 'renderer', '-CaseFiles', $mapPath, '-Out', $table)
  Test-That 'no log is refused with 1' { $r.Code -eq 1 -and $r.Out.Contains('no log at') }

  $r = Invoke-Tool @('-Log', $logPath, '-Tests', 'water', '-CaseFiles', $mapPath, '-Out', $table)
  Test-That 'a log without the test is refused with 1' { $r.Code -eq 1 -and $r.Out.Contains('has none of the tests water') }

  $plain = Join-Path $scratch 'plain.log'
  Write-Text $plain (($log -split "`n" | Where-Object { $_ -notmatch ' s: ' }) -join "`n")
  $r = Invoke-Tool @('-Log', $plain, '-Tests', 'renderer', '-CaseFiles', $mapPath, '-Out', $table)
  Test-That 'a log without durations is refused with 1, and says why' { $r.Code -eq 1 -and $r.Out.Contains('--duration=true') }

  $r = Invoke-Tool @()
  Test-That 'no arguments is usage, 2' { $r.Code -eq 2 -and $r.Out.Contains('usage:') }
} finally {
  if ($KeepTemp) { Write-Host "kept $scratch" } else { Remove-Item -Recurse -Force -LiteralPath $scratch -ErrorAction SilentlyContinue }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed:" -ForegroundColor Red
  foreach ($f in $failures) { Write-Host "  - $f" }
  exit 1
}
Write-Host "test-costs: $checks checks passed" -ForegroundColor Green
exit 0

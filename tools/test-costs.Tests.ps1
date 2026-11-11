#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/test-costs.ps1 and the deal it predicts. Runs under CTest as `tools.test_costs`.

.DESCRIPTION
  The table: writes two CTest logs in the shape LastTest.log has — the server's and a desktop
  preset's, two parts of one parted test, a test the run does not ask about, doctest's
  `--duration=true` lines, a printed wait for the GPU lock — and a case-to-file map, runs the tool,
  and reads the table it wrote: the seconds summed per file across the parts, the slowest log's
  seconds a file with that log's label, labels from the logs' names, whole seconds and never 0,
  rows ordered longest first, the leading comment and another test's rows kept, the named test's
  old rows replaced, a case no file holds left out and counted, and the same bytes from a rerun
  and from the logs given in the other order. Then the refusals: no log, a log without the test, a
  log without durations (exit 1), a label count that does not match (2), no arguments (2).

  The logs: tools/lib/CTestLogs.psm1's Save-CTestLog, which tools/dev.ps1's test verb calls after
  every ctest pass: a pass's LastTest.log copied under its time and its pass, nothing for a log the
  pass did not write, a second copy in one second beside the first, the newest -Keep kept, and a
  log of another name left alone.

  The deal: cmake/EngineTestDeal.cmake in script mode (`cmake -P`, -Cmake), over fixture tables:
  the costed files longest first to the least-loaded part, the uncosted ones round the parts by
  name after them, an empty table the name-order deal, a row ending in a comment read, and a
  malformed row refused. And `-Predict` over a hand-written CTestTestfile.cmake: each part's
  seconds as the build deals them and as the table would.

  Every case writes into its own directory under the system temp directory, named by a GUID, so
  two copies of this script never share one.
#>
[CmdletBinding()]
param([switch]$KeepTemp, [string]$Cmake)

$ErrorActionPreference = 'Stop'
$tool = Join-Path $PSScriptRoot 'test-costs.ps1'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
$scratch = Join-Path ([IO.Path]::GetTempPath()) "engine-test-costs-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
if (-not $Cmake) { $found = Get-Command cmake -ErrorAction SilentlyContinue; if ($found) { $Cmake = $found.Source } }

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

$server = @'
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
0.050000 s: c: the on01:00:02.000 info  renderer [t0] another thread's line inside the case's
0.040000 s: a: the 01:00:03.000 info  renderer [t0] a cut two cases begin with
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

# The desktop's run of the same parts: file a slower than on the server, b faster.
$desktop = @'
1/2 Test: renderer.1
Command: "D:/w/build/msvc-debug/systems/renderer/engine_renderer_tests.exe" "--duration=true"
Output:
----------------------------------------------------------
19.600000 s: a: the first case
0.300000 s: a: the second case
0.100000 s: c: the only case
<end of output>
2/2 Test: renderer.2
Command: "D:/w/build/msvc-debug/systems/renderer/engine_renderer_tests.exe" "--duration=true"
Output:
----------------------------------------------------------
10.000000 s: b: the long one
<end of output>
'@

$map = @'
{ "renderer": { "a: the first case": "a_tests.cpp", "a: the second case": "a_tests.cpp",
                "c: the only case": "c_tests.cpp", "b: the long one": "b_tests.cpp",
                "b: and another": "b_tests.cpp" } }
'@

# The deal's fixture: five costed files (two of one cost) and two the table does not name.
$files = @('tiny.cpp', 'mid2.cpp', 'x_new.cpp', 'big.cpp', 'small.cpp', 'a_new.cpp', 'mid.cpp')
$fixtureTable = "# a fixture`nr big.cpp 100`nr mid.cpp 60`nr mid2.cpp 60`nr small.cpp 30  # a row may end in a comment`nr tiny.cpp 10`nother big.cpp 5000`n"

function Invoke-Deal([string]$table, [int]$parts) {
  $probe = Join-Path $scratch "deal-$([guid]::NewGuid().ToString('N')).cmake"
  $script = @(
    "include(`"$(($repo -replace '\\', '/'))/cmake/EngineTestDeal.cmake`")",
    "engine_read_test_costs(`"$(($table -replace '\\', '/'))`")",
    "engine_deal_test_files(r $parts `"$($files -join ';')`" d)",
    "foreach(k RANGE 0 $($parts - 1))",
    '  string(JOIN "," f ${d_${k}})',
    '  message(STATUS "part|${k}|${d_load_${k}}|${f}")',
    'endforeach()'
  ) -join "`n"
  Write-Text $probe $script
  $out = @(& $Cmake -P $probe 2>&1 | ForEach-Object { "$_" })
  return [pscustomobject]@{ Code = $LASTEXITCODE; Parts = @($out | Where-Object { $_ -like '-- part|*' } | ForEach-Object { $_.Substring(3) }); Out = ($out -join "`n") }
}

try {
  Write-Host 'test-costs tests'
  $serverLog = Join-Path $scratch 'LastTest-linux-server.log'
  $desktopDir = Join-Path $scratch 'msvc-debug/Testing/Temporary'
  New-Item -ItemType Directory -Force -Path $desktopDir | Out-Null
  $desktopLog = Join-Path $desktopDir 'LastTest.log'
  $mapPath = Join-Path $scratch 'map.json'
  $table = Join-Path $scratch 'test-costs.txt'
  Write-Text $serverLog $server
  Write-Text $desktopLog $desktop
  Write-Text $mapPath $map
  $seed = "# the table's comment`n#   <test> <basename> <whole seconds>`n`ngfx raster_tests.cpp 30  # linux-server`nrenderer old_tests.cpp 7  # old`n"
  Write-Text $table $seed

  # ---- one log, as before
  $r = Invoke-Tool @('-Log', $serverLog, '-Tests', 'renderer', '-CaseFiles', $mapPath, '-Out', $table)
  $rows = @([IO.File]::ReadAllLines($table))
  Test-That 'it writes the table and exits 0' { $r.Code -eq 0 }
  Test-That 'the comment block is kept, first' { $rows[0] -eq "# the table's comment" -and $rows[1] -eq '#   <test> <basename> <whole seconds>' }
  Test-That "another test's rows are kept, comment and all" { $rows -contains 'gfx raster_tests.cpp 30  # linux-server' }
  Test-That "the named test's old rows are replaced, a trailing comment and all" { -not ($rows | Where-Object { $_ -like 'renderer old_tests.cpp*' }) }
  Test-That "seconds are summed per file across the parts, longest first, each with the log's label (LastTest-<label>.log)" {
    $mine = @($rows | Where-Object { $_ -like 'renderer *' })
    ($mine -join '|') -eq 'renderer b_tests.cpp 42  # linux-server|renderer a_tests.cpp 13  # linux-server|renderer c_tests.cpp 1  # linux-server'
  }
  Test-That 'a file that took a fraction of a second still counts, as 1' { $rows -contains 'renderer c_tests.cpp 1  # linux-server' }
  Test-That 'a wait for the GPU lock comes off the case it was in (640 s less 600 waited)' { $rows -contains 'renderer b_tests.cpp 42  # linux-server' }
  Test-That 'a test the run did not ask about is not read' { -not ($rows | Where-Object { $_ -like 'flora *' }) }
  Test-That 'a case no file holds is left out and counted, and so is a cut name two cases begin with' { $r.Out.Contains("'a case in no file' is in no file") -and $r.Out.Contains("'a: the 01:00:03.000 info") -and $r.Out.Contains('2 case(s) left out') }
  Test-That "a case name cut by another thread's log line is mended when one case begins with the rest" { $r.Out.Contains('1 case name(s) cut by a log line and mended') -and -not $r.Out.Contains("'c: the on01") }

  # ---- two logs: the slowest a file, and whose
  Write-Text $table $seed
  $r = Invoke-Tool @('-Log', "$serverLog,$desktopLog", '-Tests', 'renderer', '-CaseFiles', $mapPath, '-Out', $table)
  $first = [IO.File]::ReadAllBytes($table)
  $rows = @([IO.File]::ReadAllLines($table))
  Test-That 'two logs: each file takes the slowest log, named by its label (a plain LastTest.log by its build directory)' {
    $r.Code -eq 0 -and (@($rows | Where-Object { $_ -like 'renderer *' }) -join '|') -eq 'renderer b_tests.cpp 42  # linux-server|renderer a_tests.cpp 20  # msvc-debug|renderer c_tests.cpp 1  # linux-server'
  }
  $r = Invoke-Tool @('-Log', "$serverLog,$desktopLog", '-Tests', 'renderer', '-CaseFiles', $mapPath, '-Out', $table)
  Test-That 'a rerun on the same logs writes the same bytes' { $r.Code -eq 0 -and [Convert]::ToBase64String([IO.File]::ReadAllBytes($table)) -eq [Convert]::ToBase64String($first) }
  Write-Text $table $seed
  $r = Invoke-Tool @('-Log', "$desktopLog,$serverLog", '-Tests', 'renderer', '-CaseFiles', $mapPath, '-Out', $table)
  Test-That 'the logs in the other order write the same bytes' { $r.Code -eq 0 -and [Convert]::ToBase64String([IO.File]::ReadAllBytes($table)) -eq [Convert]::ToBase64String($first) }
  Write-Text $table $seed
  $r = Invoke-Tool @('-Log', "$serverLog,$desktopLog", '-Label', 'titan-xp,rtx-5090', '-Tests', 'renderer', '-CaseFiles', $mapPath, '-Out', $table)
  Test-That '-Label names the logs instead' { $r.Code -eq 0 -and ([IO.File]::ReadAllLines($table) -contains 'renderer a_tests.cpp 20  # rtx-5090') }

  # ---- refusals
  $r = Invoke-Tool @('-Log', (Join-Path $scratch 'missing.log'), '-Tests', 'renderer', '-CaseFiles', $mapPath, '-Out', $table)
  Test-That 'no log is refused with 1' { $r.Code -eq 1 -and $r.Out.Contains('no log at') }
  $r = Invoke-Tool @('-Log', $serverLog, '-Tests', 'water', '-CaseFiles', $mapPath, '-Out', $table)
  Test-That 'a log without the test is refused with 1' { $r.Code -eq 1 -and $r.Out.Contains('has none of the tests water') }
  $plain = Join-Path $scratch 'plain.log'
  Write-Text $plain (($server -split "`n" | Where-Object { $_ -notmatch ' s: ' }) -join "`n")
  $r = Invoke-Tool @('-Log', $plain, '-Tests', 'renderer', '-CaseFiles', $mapPath, '-Out', $table)
  Test-That 'a log without durations is refused with 1, and says why' { $r.Code -eq 1 -and $r.Out.Contains('--duration=true') }
  $r = Invoke-Tool @('-Log', "$serverLog,$desktopLog", '-Label', 'one', '-Tests', 'renderer', '-CaseFiles', $mapPath, '-Out', $table)
  Test-That 'a label count that does not match the logs is usage, 2' { $r.Code -eq 2 -and $r.Out.Contains('1 label(s) for 2 log(s)') }
  $r = Invoke-Tool @()
  Test-That 'no arguments is usage, 2' { $r.Code -eq 2 -and $r.Out.Contains('usage:') }

  # ---- each ctest pass's log under its own name (tools/lib/CTestLogs.psm1, tools/dev.ps1 test)
  Import-Module (Join-Path $PSScriptRoot 'lib/CTestLogs.psm1') -Force
  $bd = Join-Path $scratch 'logs-build'
  $tmp = Join-Path $bd 'Testing/Temporary'
  New-Item -ItemType Directory -Force -Path $tmp | Out-Null
  $last = Join-Path $tmp 'LastTest.log'
  Test-That 'no LastTest.log: nothing copied' { $null -eq (Save-CTestLog -BuildDir $bd -Pass 'all' -Started (Get-Date)) }
  Write-Text $last 'the first pass'
  (Get-Item -LiteralPath $last).LastWriteTime = (Get-Date).AddMinutes(-10)
  Test-That 'a LastTest.log older than the pass (ctest ran nothing): nothing copied' { $null -eq (Save-CTestLog -BuildDir $bd -Pass 'all' -Started (Get-Date)) }
  $t0 = [datetime]'2026-11-11T02:35:00'
  (Get-Item -LiteralPath $last).LastWriteTime = $t0.AddMinutes(1)
  $a = Save-CTestLog -BuildDir $bd -Pass 'all-invariant-no-driver' -Started $t0
  Write-Text $last 'the second pass'
  (Get-Item -LiteralPath $last).LastWriteTime = $t0.AddMinutes(2)
  $b = Save-CTestLog -BuildDir $bd -Pass 'all-rest' -Started $t0
  Test-That 'each pass is kept beside LastTest.log under its time and its name' {
    (Split-Path -Leaf $a) -eq 'LastTest-20261111-023500-all-invariant-no-driver.log' -and
    (Split-Path -Leaf $b) -eq 'LastTest-20261111-023500-all-rest.log' -and
    [IO.File]::ReadAllText($a) -eq 'the first pass' -and [IO.File]::ReadAllText($b) -eq 'the second pass'
  }
  $c = Save-CTestLog -BuildDir $bd -Pass 'all-rest' -Started $t0
  Test-That 'a second copy in the same second takes -2 and overwrites nothing' { (Split-Path -Leaf $c) -eq 'LastTest-20261111-023500-all-rest-2.log' -and [IO.File]::ReadAllText($b) -eq 'the second pass' }
  $gate = Join-Path $tmp 'LastTest-linux-server.log'
  Write-Text $gate 'a log copied in by hand'
  for ($i = 1; $i -le 5; $i++) {
    (Get-Item -LiteralPath $last).LastWriteTime = $t0.AddHours($i)
    Save-CTestLog -BuildDir $bd -Pass 'filter' -Started $t0.AddHours($i) -Keep 4 | Out-Null
  }
  $kept = @(Get-ChildItem -LiteralPath $tmp -File | Where-Object { $_.Name -match '^LastTest-\d{8}-\d{6}-' } | ForEach-Object Name | Sort-Object)
  Test-That 'only the newest -Keep copies stay, and a log of another name is never touched' {
    ($kept -join ',') -eq 'LastTest-20261111-043500-filter.log,LastTest-20261111-053500-filter.log,LastTest-20261111-063500-filter.log,LastTest-20261111-073500-filter.log' -and
    (Test-Path -LiteralPath $gate) -and (Test-Path -LiteralPath $last)
  }
  (Get-Item -LiteralPath $last).LastWriteTime = $t0.AddHours(9)
  $odd = Save-CTestLog -BuildDir $bd -Pass 'a pass/with:odd chars' -Started $t0.AddHours(9)
  Test-That "a pass's name is made safe for a file name" { (Split-Path -Leaf $odd) -eq 'LastTest-20261111-113500-a-pass-with-odd-chars.log' }

  # ---- the deal, cmake/EngineTestDeal.cmake in script mode
  if (-not $Cmake) {
    Test-That 'a cmake to run the deal with (-Cmake, or cmake on PATH)' { $false }
  } else {
    $fixture = Join-Path $scratch 'fixture-costs.txt'
    Write-Text $fixture $fixtureTable
    $d = Invoke-Deal $fixture 3
    Test-That 'the deal reads a fixture table, a row ending in a comment included' { $d.Code -eq 0 -and $d.Parts.Count -eq 3 }
    Test-That 'costed files longest first, each to the least-loaded part (the lowest of equals); then the rest by name round the parts' {
      ($d.Parts -join ' / ') -eq 'part|0|100|big.cpp,a_new.cpp / part|1|90|mid.cpp,small.cpp,x_new.cpp / part|2|70|mid2.cpp,tiny.cpp'
    }
    Test-That "another test's row costs nothing here" { -not ($d.Parts | Where-Object { $_ -match '\|5\d\d\d\|' }) }
    $empty = Join-Path $scratch 'empty-costs.txt'
    Write-Text $empty "# nothing measured yet`n"
    $d = Invoke-Deal $empty 3
    Test-That 'an empty table is the name-order deal, round the parts' {
      $d.Code -eq 0 -and ($d.Parts -join ' / ') -eq 'part|0|0|a_new.cpp,mid2.cpp,x_new.cpp / part|1|0|big.cpp,small.cpp / part|2|0|mid.cpp,tiny.cpp'
    }
    $d = Invoke-Deal (Join-Path $scratch 'no-such-table.txt') 2
    Test-That 'no table at all is the empty one' { $d.Code -eq 0 -and ($d.Parts -join ' / ') -eq 'part|0|0|a_new.cpp,mid.cpp,small.cpp,x_new.cpp / part|1|0|big.cpp,mid2.cpp,tiny.cpp' }
    $bad = Join-Path $scratch 'bad-costs.txt'
    Write-Text $bad "r big.cpp ten`n"
    $d = Invoke-Deal $bad 3
    # CMake wraps an error's text at word boundaries, where depends on the path's length (the
    # Linux container's /tmp wrapped inside the quote), so the words are compared, not the lines.
    Test-That 'a malformed row stops the configure, quoted' { $d.Code -ne 0 -and ($d.Out -replace '\s+', ' ').Contains("'r big.cpp ten' is not") }

    # ---- -Predict, over a CTestTestfile.cmake as the configure writes one (the deal of the empty
    # table above) and the fixture table
    $build = Join-Path $scratch 'build'
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    $filter = { param([string[]]$names) ($names | ForEach-Object { "*/$_,*\$_" }) -join ',' }
    Write-Text (Join-Path $build 'CTestTestfile.cmake') (@(
        "add_test([=[r.1]=] `"x`" `"--duration=true`" [=[--source-file=$(& $filter @('a_new.cpp', 'mid2.cpp', 'x_new.cpp'))]=])",
        "add_test([=[r.2]=] `"x`" `"--duration=true`" [=[--source-file=$(& $filter @('big.cpp', 'small.cpp'))]=])",
        "add_test([=[r.3]=] `"x`" `"--duration=true`" [=[--source-file=$(& $filter @('mid.cpp', 'tiny.cpp'))]=])",
        "add_test([=[flora]=] `"y`" `"--duration=true`")"
      ) -join "`n")
    $r = Invoke-Tool @('-Predict', '-BuildDir', $build, '-Table', $fixture, '-Cmake', $Cmake)
    $line = (($r.Out -split "`n") | Where-Object { $_ -like 'r: *' } | Select-Object -First 1) -replace '\s+', ' '
    Test-That '-Predict: each part as dealt now and as the table would deal it, with the files the table does not name' {
      $r.Code -eq 0 -and $line.Trim() -eq "r: 7 files, 2 not in the table (0 s); parts' seconds now: 60 130 70 (largest 130); by the table: 100 90 70 (largest 100)"
    }
    Test-That '-Predict reads only parted tests' { -not ($r.Out -match '(?m)^flora') }
    $r = Invoke-Tool @('-Predict', '-BuildDir', (Join-Path $scratch 'nowhere'), '-Table', $fixture, '-Cmake', $Cmake)
    Test-That '-Predict with no build directory is refused with 1' { $r.Code -eq 1 }
  }
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

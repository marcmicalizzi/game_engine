#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Writes cmake/test-costs.txt, the seconds per test source file a parted test is dealt by, and
  predicts the parts a table deals.

.DESCRIPTION
  tools/test-costs.ps1 -Log <LastTest.log>[,<LastTest.log>...] -Tests <name>[,<name>...]
                       [-Label <name>[,<name>...]] [-BuildDir <build/<preset>> | -CaseFiles <map.json>]
                       [-Out <cmake/test-costs.txt>]
  tools/test-costs.ps1 -Predict -BuildDir <build/<preset>> [-Table <file>] [-Tests <name>,...]
                       [-Cmake <cmake>]

  Every module and end-to-end test runs with doctest's `--duration=true` (roadmap T77), so a CTest
  log holds one line of seconds a test case, `0.123456 s: <case name>`, under the test that ran it.
  The first form sums them per source file for the named tests (`renderer` covers renderer.1 to
  renderer.4), in each log on its own, and writes one row a file into -Out:

    <test> <basename> <whole seconds>  # <label>

  the seconds of the log that took longest over that file, and that log's label: the slowest
  machine that ran the file is the binding one (docs/ci/what-to-run.md, "Parts dealt by cost"),
  since a part that fits there fits everywhere. A label is -Label's, or the log's name less
  `LastTest-` and `.log` (`LastTest-linux-server.log` is `linux-server`), or for a plain
  `LastTest.log` the build directory it sits in (`build/msvc-debug/Testing/Temporary/`). The rows
  of the named tests are replaced; every other row and the file's leading comment are kept. The
  rows are sorted by test, then seconds (longest first), then name, by ordinal comparison, and the
  seconds are whole, so the same logs write the same bytes on every machine.

  Which file a case is in comes from the test executable itself, `--list-test-cases --no-skip
  --reporters=xml`, found under -BuildDir by the name on the log's `Command:` line — so a log from
  another machine (the GPU server's, fetched with remote-build.ps1 -Fetch) is read against a local
  build of the same commit. -CaseFiles gives the map instead, as JSON
  `{ "<test>": { "<case name>": "<basename>" } }`, which is how the tool's own test runs with no
  build. A case the map does not place is reported and left out, and a case two runs of one log
  both hold (a part rerun after a skip) counts each time. A wait for the GPU lock the test's own
  process printed (`gpu-lock: taken after N s`) is taken off the case it can have been in (below);
  an end-to-end test's children wait out of sight, so its seconds can carry a wait.

  -Predict reads a configured build's parted tests (its CTestTestfile.cmake files: each
  `<test>.<n>` and the files its `--source-file=` filter names; no build needed) and prints, a test a line, each part's seconds
  as -Table (default cmake/test-costs.txt) costs it, first as the build deals the files now and
  then as the configure would deal them by -Table — the configure's own deal
  (engine_deal_test_files, cmake/EngineTestDeal.cmake) run in script mode, never a copy of it. A
  file the table does not name counts 0, and the line says how many there are. It is a prediction
  from one run's seconds, not a measurement.

  Exit 0 written or predicted, 1 refused, 2 usage.
#>
[CmdletBinding()]
param(
  [string[]]$Log,
  [string[]]$Tests,
  [string[]]$Label,
  [string]$BuildDir,
  [string]$CaseFiles,
  [string]$Out,
  [switch]$Predict,
  [string]$Table,
  [string]$Cmake
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Tests = @($Tests | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Log = @($Log | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$Label = @($Label | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

# Ordinal sort of strings, so an order never depends on the machine's culture.
function Sort-Ordinal([string[]]$items) {
  $list = [System.Collections.Generic.List[string]]::new()
  foreach ($i in $items) { $list.Add($i) }
  $list.Sort([StringComparer]::Ordinal)
  return , $list.ToArray()
}

# ---- -Predict ------------------------------------------------------------------------------------

if ($Predict) {
  if (-not $BuildDir) { Write-Host 'usage: tools/test-costs.ps1 -Predict -BuildDir <build/<preset>> [-Table <file>] [-Tests <name>,...] [-Cmake <cmake>]'; exit 2 }
  if (-not (Test-Path -LiteralPath $BuildDir)) { Write-Host "test-costs: no build directory at $BuildDir"; exit 1 }
  if (-not $Table) { $Table = Join-Path $repo 'cmake/test-costs.txt' }
  if (-not (Test-Path -LiteralPath $Table)) { Write-Host "test-costs: no table at $Table"; exit 1 }
  if (-not $Cmake) {
    $found = Get-Command cmake -ErrorAction SilentlyContinue
    if (-not $found) { Write-Host 'test-costs: no cmake on PATH; give -Cmake'; exit 1 }
    $Cmake = $found.Source
  }

  $costs = @{}
  foreach ($line in [IO.File]::ReadAllLines($Table)) {
    $l = ($line -replace '#.*$', '').Trim()
    if ($l -match '^([A-Za-z0-9_.]+)\s+(\S+)\s+(\d+)$') { $costs["$($Matches[1]) $($Matches[2])"] = [int]$Matches[3] }
  }

  # The configure's CTestTestfile.cmake files, read as text rather than through `ctest
  # --show-only`, which leaves out the command of a test whose executable is not built yet: a
  # prediction is wanted right after a configure.
  $parted = [ordered]@{}
  $testfiles = @(Get-ChildItem -LiteralPath $BuildDir -Recurse -File -Filter 'CTestTestfile.cmake' | Where-Object { $_.FullName -notmatch '[\\/]_deps[\\/]' })
  foreach ($tf in $testfiles) {
    foreach ($line in [IO.File]::ReadLines($tf.FullName)) {
      if ($line -notmatch '^\s*add_test\(\s*\[=\[([A-Za-z0-9_.]+)\.(\d+)\]=\]') { continue }
      $base = $Matches[1]; $n = [int]$Matches[2]
      if ($Tests.Count -gt 0 -and $base -notin $Tests) { continue }
      if ($line -notmatch '--source-file=([^"\]]+)') { continue }
      $files = @($Matches[1] -split ',' | Where-Object { $_.StartsWith('*/') } | ForEach-Object { $_.Substring(2) })
      if (-not $parted.Contains($base)) { $parted[$base] = @{} }
      $parted[$base][$n] = $files
    }
  }
  if ($parted.Count -eq 0) { Write-Host "test-costs: $BuildDir has no parted tests$(if ($Tests.Count) { " named $($Tests -join ', ')" })"; exit 1 }

  $scratch = Join-Path ([IO.Path]::GetTempPath()) "engine-test-costs-predict-$([guid]::NewGuid().ToString('N'))"
  New-Item -ItemType Directory -Force -Path $scratch | Out-Null
  try {
    foreach ($base in (Sort-Ordinal @($parted.Keys))) {
      $parts = $parted[$base]
      $count = ($parts.Keys | Measure-Object -Maximum).Maximum
      $all = Sort-Ordinal @($parts.Values | ForEach-Object { $_ } | Select-Object -Unique)
      $uncosted = @($all | Where-Object { -not $costs.ContainsKey("$base $_") }).Count
      $now = for ($k = 1; $k -le $count; $k++) {
        $s = 0; foreach ($f in @($parts[$k])) { if ($f -and $costs.ContainsKey("$base $f")) { $s += $costs["$base $f"] } }; $s
      }
      $probe = Join-Path $scratch "$base.cmake"
      $tableArg = ($Table -replace '\\', '/')
      $deal = ($repo -replace '\\', '/') + '/cmake/EngineTestDeal.cmake'
      $script = @(
        "include(`"$deal`")",
        "engine_read_test_costs(`"$tableArg`")",
        "engine_deal_test_files($base $count `"$($all -join ';')`" d)",
        "foreach(k RANGE 0 $($count - 1))",
        '  message(STATUS "part|${k}|${d_load_${k}}")',
        'endforeach()'
      ) -join "`n"
      [IO.File]::WriteAllText($probe, $script, (New-Object System.Text.UTF8Encoding $false))
      $lines = @(& $Cmake -P $probe 2>&1 | ForEach-Object { "$_" })
      if ($LASTEXITCODE -ne 0) { Write-Host "test-costs: the deal failed for ${base}:"; $lines | ForEach-Object { Write-Host "  $_" }; exit 1 }
      $by = @($lines | Where-Object { $_ -match '^-- part\|(\d+)\|(\d+)$' } | ForEach-Object { [int]($_ -split '\|')[2] })
      $fmt = { param($v) ($v | ForEach-Object { '{0,6}' -f $_ }) -join ' ' }
      Write-Host ("{0}: {1} files, {2} not in the table (0 s); parts' seconds now: {3} (largest {4}); by the table: {5} (largest {6})" -f `
          $base, $all.Count, $uncosted, (& $fmt $now), (($now | Measure-Object -Maximum).Maximum), (& $fmt $by), (($by | Measure-Object -Maximum).Maximum))
    }
  } finally {
    Remove-Item -Recurse -Force -LiteralPath $scratch -ErrorAction SilentlyContinue
  }
  exit 0
}

# ---- writing the table ---------------------------------------------------------------------------

if ($Log.Count -eq 0 -or $Tests.Count -eq 0) {
  Write-Host 'usage: tools/test-costs.ps1 -Log <LastTest.log>[,...] -Tests <name>[,<name>...] [-Label <name>,...] [-BuildDir <dir> | -CaseFiles <map.json>] [-Out <file>]'
  Write-Host '       tools/test-costs.ps1 -Predict -BuildDir <dir> [-Table <file>] [-Tests <name>,...] [-Cmake <cmake>]'
  exit 2
}
if ($Label.Count -gt 0 -and $Label.Count -ne $Log.Count) { Write-Host "test-costs: $($Label.Count) label(s) for $($Log.Count) log(s)"; exit 2 }
if (-not $Out) { $Out = Join-Path $repo 'cmake/test-costs.txt' }
foreach ($l in $Log) { if (-not (Test-Path -LiteralPath $l)) { Write-Host "test-costs: no log at $l"; exit 1 } }

function Get-LogLabel([string]$path) {
  $name = [IO.Path]::GetFileNameWithoutExtension($path)
  if ($name -match '^LastTest-(.+)$') { return $Matches[1] }
  $dir = Split-Path -Parent (Resolve-Path -LiteralPath $path).Path
  $parts = @(($dir -replace '\\', '/').Split('/') | Where-Object { $_ })
  if ($parts.Count -ge 3 -and $parts[-1] -eq 'Temporary' -and $parts[-2] -eq 'Testing') { return $parts[-3] }
  return $name
}

# One log: per CTest test, the executable it ran and the seconds of each case.
function Read-Log([string]$path) {
  $runs = New-Object System.Collections.Generic.List[object]
  $cur = $null
  foreach ($line in [IO.File]::ReadLines($path)) {
    if ($line -match '^\d+/\d+ Test: (\S+)$') {
      $name = $Matches[1]
      $base = $name -replace '\.\d+$', ''
      $cur = if ($base -in $Tests) { [pscustomobject]@{ Test = $base; Part = $name; Exe = ''; Cases = [ordered]@{}; Waits = (New-Object System.Collections.Generic.List[double]) } } else { $null }
      if ($cur) { $runs.Add($cur) }
      continue
    }
    if (-not $cur) { continue }
    if (-not $cur.Exe -and $line -match '^Command: "([^"]+)"') { $cur.Exe = $Matches[1]; continue }
    if ($line -match 'gpu-lock: taken after (\d+(\.\d+)?) s') { $cur.Waits.Add([double]$Matches[1]); continue }
    if ($line -match '^\s*(\d+\.\d+) s: (.+?)\s*$') {
      $case = $Matches[2]
      $cur.Cases[$case] = [double]$Matches[1] + $(if ($cur.Cases.Contains($case)) { $cur.Cases[$case] } else { 0.0 })
    }
  }
  # A case's duration includes any wait for the GPU lock inside it, which is the machine's and not
  # the case's (2026-11-10: gfx.1's bindless case read 1,726 s, 1,725 of them waiting). The process
  # says how long it waited, but on stderr, which reaches the log ahead of the buffered case lines,
  # so the line order does not say which case waited. Each wait, longest first, comes off the case
  # of the smallest duration that is at least as long as the wait: the only cases it can have been
  # in, and the least time it can misplace. An end-to-end test's children wait out of sight; that
  # stays in.
  foreach ($r in $runs) {
    foreach ($w in ($r.Waits | Sort-Object -Descending)) {
      $best = $null
      foreach ($case in @($r.Cases.Keys)) {
        if ($r.Cases[$case] -ge $w -and ($null -eq $best -or $r.Cases[$case] -lt $r.Cases[$best])) { $best = $case }
      }
      if ($null -ne $best) { $r.Cases[$best] = $r.Cases[$best] - $w }
    }
  }
  return , $runs
}

$logs = for ($i = 0; $i -lt $Log.Count; $i++) {
  [pscustomobject]@{ Path = $Log[$i]; Label = $(if ($Label.Count) { $Label[$i] } else { Get-LogLabel $Log[$i] }); Runs = (Read-Log $Log[$i]) }
}
$allRuns = @($logs | ForEach-Object { $_.Runs } | ForEach-Object { $_ })
if ($allRuns.Count -eq 0) { Write-Host "test-costs: $($Log -join ', ') has none of the tests $($Tests -join ', ')"; exit 1 }
if (@($allRuns | Where-Object { $_.Cases.Count -gt 0 }).Count -eq 0) {
  Write-Host "test-costs: $($Log -join ', ') has the tests but no case durations; was it a run with doctest's --duration=true (every test since T77)?"
  exit 1
}

# Case name to source file, per test.
$map = @{}
if ($CaseFiles) {
  $json = Get-Content -LiteralPath $CaseFiles -Raw | ConvertFrom-Json
  foreach ($t in $json.PSObject.Properties) {
    $m = @{}
    foreach ($c in $t.Value.PSObject.Properties) { $m[$c.Name] = [string]$c.Value }
    $map[$t.Name] = $m
  }
} else {
  if (-not $BuildDir) { Write-Host 'test-costs: give -BuildDir (a build of the same commit) or -CaseFiles'; exit 2 }
  foreach ($t in (Sort-Ordinal @($allRuns | ForEach-Object Test | Select-Object -Unique))) {
    $exeName = [IO.Path]::GetFileNameWithoutExtension((($allRuns | Where-Object Test -eq $t | Select-Object -First 1).Exe -replace '\\', '/').Split('/')[-1])
    $exe = Get-ChildItem -LiteralPath $BuildDir -Recurse -File |
      Where-Object { $_.BaseName -eq $exeName -and ($_.Extension -in @('.exe', '')) -and $_.FullName -notmatch '[\\/]_deps[\\/]' } |
      Select-Object -First 1
    if (-not $exe) { Write-Host "test-costs: no $exeName under $BuildDir"; exit 1 }
    $xml = [xml]((& $exe.FullName --list-test-cases --no-skip --reporters=xml) -join "`n")
    $m = @{}
    foreach ($node in $xml.doctest.TestCase) {
      $file = ($node.filename -replace '\\', '/').Split('/')[-1]
      if ($m.ContainsKey($node.name) -and $m[$node.name] -ne $file) { Write-Host "test-costs: '$($node.name)' is in $($m[$node.name]) and $file; counted against the first" }
      if (-not $m.ContainsKey($node.name)) { $m[$node.name] = $file }
    }
    $map[$t] = $m
  }
}

# Seconds per (test, file) in each log, then the largest over the logs and whose it was.
$best = @{}
$unplaced = 0
$mended = 0
foreach ($lg in $logs) {
  $sums = @{}
  foreach ($r in $lg.Runs) {
    $m = $map[$r.Test]
    foreach ($case in $r.Cases.Keys) {
      $name = $case
      if ($m -and -not $m.ContainsKey($name)) {
        # A log line from another thread can land inside the duration line, cutting the case's
        # name at a `hh:mm:ss.mmm <level>` stamp (gate b58's server log: seven cases). The part
        # before the stamp names the case when exactly one case of the test starts with it.
        $cut = $name -replace '\d{2}:\d{2}:\d{2}\.\d{3}\s+(trace|debug|info|warn|error|fatal)\b.*$', ''
        if ($cut -ne $name -and $cut.Length -gt 0) {
          $hits = @($m.Keys | Where-Object { $_.StartsWith($cut, [StringComparison]::Ordinal) })
          if ($hits.Count -eq 1) { $name = $hits[0]; $script:mended++ }
        }
      }
      if (-not $m -or -not $m.ContainsKey($name)) { $unplaced++; Write-Host "test-costs: $($lg.Label) $($r.Part): '$case' is in no file the map knows; left out"; continue }
      $key = "$($r.Test) $($m[$name])"
      $sums[$key] = $(if ($sums.ContainsKey($key)) { $sums[$key] } else { 0.0 }) + $r.Cases[$case]
    }
  }
  foreach ($key in $sums.Keys) {
    $s = [math]::Max(1, [int][math]::Round($sums[$key]))
    # Ties go to the label that sorts first, so the order of -Log does not change the bytes.
    if (-not $best.ContainsKey($key) -or $s -gt $best[$key].Seconds -or
        ($s -eq $best[$key].Seconds -and [string]::CompareOrdinal($lg.Label, $best[$key].Label) -lt 0)) {
      $best[$key] = [pscustomobject]@{ Seconds = $s; Label = $lg.Label }
    }
  }
}
$runsBy = @($logs | ForEach-Object { $lg = $_; @($lg.Runs | Where-Object { $_.Cases.Count -gt 0 } | Group-Object Test | ForEach-Object { "$($lg.Label) $($_.Name): $($_.Count) run(s)" }) })

# The table: the existing comment block, the other tests' rows, then the named tests' rows by test,
# then cost (longest first), then name. Whole seconds, and never 0 for a file that took any time,
# so a file the table names always counts.
$header = @()
$keep = @()
if (Test-Path -LiteralPath $Out) {
  $inHeader = $true
  foreach ($line in [IO.File]::ReadAllLines($Out)) {
    if ($inHeader -and ($line -match '^\s*#' -or $line -match '^\s*$')) { $header += $line; continue }
    $inHeader = $false
    if ($line -match '^\s*([A-Za-z0-9_.]+)\s+\S+\s+\d+\s*(#.*)?$' -and $Matches[1] -in $Tests) { continue }
    if ($line -match '\S') { $keep += $line }
  }
}
$rows = [System.Collections.Generic.List[object]]::new()
foreach ($key in $best.Keys) {
  $test, $file = $key -split ' ', 2
  $rows.Add([pscustomobject]@{ Test = $test; File = $file; Seconds = $best[$key].Seconds; Label = $best[$key].Label })
}
$rows.Sort([Comparison[object]] {
    param($a, $b)
    $c = [string]::CompareOrdinal($a.Test, $b.Test); if ($c -ne 0) { return $c }
    if ($a.Seconds -ne $b.Seconds) { return $b.Seconds.CompareTo($a.Seconds) }
    return [string]::CompareOrdinal($a.File, $b.File)
  })
$text = @($header) + @($keep) + @($rows | ForEach-Object { "$($_.Test) $($_.File) $($_.Seconds)  # $($_.Label)" })
[IO.File]::WriteAllText($Out, (($text -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding $false))
Write-Host ("test-costs: {0} file(s) for {1} from {2} ({3}); {4} case name(s) cut by a log line and mended; {5} case(s) left out; wrote {6}" -f $rows.Count, ($Tests -join ', '), (($logs | ForEach-Object Label) -join ', '), ($runsBy -join ', '), $mended, $unplaced, $Out)
exit 0

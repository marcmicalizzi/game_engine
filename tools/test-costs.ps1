#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Writes cmake/test-costs.txt, the seconds per test source file a parted test is dealt by.

.DESCRIPTION
  tools/test-costs.ps1 -Log <LastTest.log> -Tests <name>[,<name>...] [-BuildDir <build/<preset>>]
                       [-CaseFiles <map.json>] [-Out <cmake/test-costs.txt>]

  Every module and end-to-end test runs with doctest's `--duration=true` (roadmap T77), so a CTest
  log holds one line of seconds a test case, `0.123456 s: <case name>`, under the test that ran it.
  This sums them per source file for the named tests (`renderer` covers renderer.1 to renderer.4)
  and writes one row a file, `<test> <basename> <whole seconds>`, into -Out: the rows of the named
  tests are replaced, every other row and the file's leading comment are kept.

  Which file a case is in comes from the test executable itself, `--list-test-cases --no-skip
  --reporters=xml`, found under -BuildDir by the name on the log's `Command:` line — so a log from
  another machine (the GPU server's, fetched with remote-build.ps1 -Fetch) is read against a local
  build of the same commit. -CaseFiles gives the map instead, as JSON
  `{ "<test>": { "<case name>": "<basename>" } }`, which is how the tool's own test runs with no
  build. A case the map does not place is reported and left out, and a case two runs of the log
  both hold (a part rerun after a skip) counts each time. A wait for the GPU lock the test's own
  process printed (`gpu-lock: taken after N s`) is taken off the case it can have been in (below);
  an end-to-end test's children wait out of sight, so its seconds can carry a wait.

  The binding numbers are the slowest card's (docs/ci/what-to-run.md, "Parts dealt by cost"), so
  the server's log is the one to read where there is one. Exit 0 written, 1 refused, 2 usage.
#>
[CmdletBinding()]
param(
  [string]$Log,
  [string[]]$Tests,
  [string]$BuildDir,
  [string]$CaseFiles,
  [string]$Out
)

$ErrorActionPreference = 'Stop'
if (-not $Log -or -not $Tests) {
  Write-Host 'usage: tools/test-costs.ps1 -Log <LastTest.log> -Tests <name>[,<name>...] [-BuildDir <dir> | -CaseFiles <map.json>] [-Out <file>]'
  exit 2
}
if (-not $Out) { $Out = Join-Path (Join-Path $PSScriptRoot '..') 'cmake/test-costs.txt' }
if (-not (Test-Path -LiteralPath $Log)) { Write-Host "test-costs: no log at $Log"; exit 1 }
$Tests = @($Tests | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

# The log: per CTest test, the executable it ran and the seconds of each case.
$runs = New-Object System.Collections.Generic.List[object]
$cur = $null
foreach ($line in [IO.File]::ReadLines($Log)) {
  if ($line -match '^\d+/\d+ Test: (\S+)$') {
    $name = $Matches[1]
    $base = $name -replace '\.\d+$', ''
    $cur = if ($base -in $Tests) { [pscustomobject]@{ Test = $base; Part = $name; Exe = ''; Cases = [ordered]@{} } } else { $null }
    if ($cur) { $runs.Add($cur) }
    continue
  }
  if (-not $cur) { continue }
  if (-not $cur.Exe -and $line -match '^Command: "([^"]+)"') { $cur.Exe = $Matches[1]; continue }
  if ($line -match 'gpu-lock: taken after (\d+(\.\d+)?) s') {
    if (-not $cur.PSObject.Properties['Waits']) { $cur | Add-Member -NotePropertyName Waits -NotePropertyValue (New-Object System.Collections.Generic.List[double]) }
    $cur.Waits.Add([double]$Matches[1])
    continue
  }
  if ($line -match '^\s*(\d+\.\d+) s: (.+?)\s*$') {
    $case = $Matches[2]
    $cur.Cases[$case] = [double]$Matches[1] + $(if ($cur.Cases.Contains($case)) { $cur.Cases[$case] } else { 0.0 })
  }
}
# A case's duration includes any wait for the GPU lock inside it, which is the machine's and not the
# case's (2026-11-10: gfx.1's bindless case read 1,726 s, 1,725 of them waiting). The process says
# how long it waited, but on stderr, which reaches the log ahead of the buffered case lines, so the
# line order does not say which case waited. Each wait, longest first, comes off the case of the
# smallest duration that is at least as long as the wait: the only cases it can have been in, and
# the least time it can misplace. An end-to-end test's children wait out of sight; that stays in.
foreach ($r in $runs) {
  if (-not $r.PSObject.Properties['Waits']) { continue }
  foreach ($w in ($r.Waits | Sort-Object -Descending)) {
    $best = $null
    foreach ($case in @($r.Cases.Keys)) {
      if ($r.Cases[$case] -ge $w -and ($null -eq $best -or $r.Cases[$case] -lt $r.Cases[$best])) { $best = $case }
    }
    if ($null -ne $best) { $r.Cases[$best] = $r.Cases[$best] - $w }
  }
}
if ($runs.Count -eq 0) { Write-Host "test-costs: $Log has none of the tests $($Tests -join ', ')"; exit 1 }
$timed = @($runs | Where-Object { $_.Cases.Count -gt 0 })
if ($timed.Count -eq 0) {
  Write-Host "test-costs: $Log has the tests but no case durations; was it a run with doctest's --duration=true (every test since T77)?"
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
  foreach ($t in ($runs | Select-Object -ExpandProperty Test -Unique)) {
    $exeName = [IO.Path]::GetFileNameWithoutExtension((($runs | Where-Object Test -eq $t | Select-Object -First 1).Exe -replace '\\', '/').Split('/')[-1])
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

# Seconds per (test, file).
$sums = @{}
$unplaced = 0
foreach ($r in $timed) {
  $m = $map[$r.Test]
  foreach ($case in $r.Cases.Keys) {
    if (-not $m -or -not $m.ContainsKey($case)) { $unplaced++; Write-Host "test-costs: $($r.Part): '$case' is in no file the map knows; left out"; continue }
    $key = "$($r.Test) $($m[$case])"
    $sums[$key] = $(if ($sums.ContainsKey($key)) { $sums[$key] } else { 0.0 }) + $r.Cases[$case]
  }
}
$parts = @($timed | Group-Object Test | ForEach-Object { "$($_.Name): $($_.Count) run(s)" })

# The table: the existing comment block, the other tests' rows, then the named tests' rows by
# test and then cost, longest first. Whole seconds, and never 0 for a file that took any time, so
# a file the table names always counts.
$header = @()
$keep = @()
if (Test-Path -LiteralPath $Out) {
  $inHeader = $true
  foreach ($line in [IO.File]::ReadAllLines($Out)) {
    if ($inHeader -and ($line -match '^\s*#' -or $line -match '^\s*$')) { $header += $line; continue }
    $inHeader = $false
    if ($line -match '^\s*([A-Za-z0-9_.]+)\s+\S+\s+\d+\s*$' -and $Matches[1] -in $Tests) { continue }
    if ($line -match '\S') { $keep += $line }
  }
}
$rows = $sums.GetEnumerator() | ForEach-Object {
  $test, $file = $_.Key -split ' ', 2
  [pscustomobject]@{ Test = $test; File = $file; Seconds = [math]::Max(1, [int][math]::Round($_.Value)) }
} | Sort-Object Test, @{ Expression = 'Seconds'; Descending = $true }, File
$text = @($header) + @($keep) + @($rows | ForEach-Object { "$($_.Test) $($_.File) $($_.Seconds)" })
[IO.File]::WriteAllText($Out, (($text -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding $false))
Write-Host ("test-costs: {0} file(s) for {1} from {2} ({3}); {4} case(s) left out; wrote {5}" -f @($rows).Count, ($Tests -join ', '), $Log, ($parts -join ', '), $unplaced, $Out)
exit 0

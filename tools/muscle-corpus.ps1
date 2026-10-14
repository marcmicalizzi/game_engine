#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Writes content/muscle/: the muscle line tier's qualification corpus as data (docs/subsystems/muscle.md).

.DESCRIPTION
  tools/muscle-corpus.ps1 -Authoring <path to the authoring track's 06-muscle-activation> [-Out <dir>]

  Reads two files of the authoring track, as data and nothing else (nothing there is run):
    r010-20261013\declarations\d001\CASES.json   the 39 rows' inputs, archived static starts,
                                                 limits, reference throughput and hashes
    r007-20261011\_handoff\corpus.json           the same 39 trajectories with their 289
                                                 held-reference oracle samples each
  and, for the known-limit labels' provenance, r009-20261013\results\s001\summary.json (the
  adopted scheme's per-row numbers).

  Writes, under -Out (default content/muscle beside this script's repository):
    corpus.json          the index: case names in order, the qualified domain's parameter
                         identities, each case's limits and reference throughput, source hashes
    cases/<case>.json    a case's inputs and archived initial state, copied whole, and its 289
                         samples as rows of eleven columns
    expectations.json    each case's class (qualified, known_limit_work, known_limit_force) with
                         r009 S's numbers, and the provenance
  README.md is written by hand and not by this script.

  **Numbers are copied as text, never recomputed**: every number is the source file's own token
  (System.Text.Json's raw text), so a value is the reference's binary64 exactly and the files are
  a function of the sources' bytes. The script also re-derives CASES.json's per-row hashes
  (inputs_sha256, initial_state_sha256, held_samples_sha256) from the copied data under the
  authoring side's canonical form (sorted keys, compact separators, Python's number text) and
  stops if one does not match, so the copy is checked against the hashes the sources carry.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)] [string]$Authoring,
  [string]$Out = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'content/muscle')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$casesPath = Join-Path $Authoring 'r010-20261013/declarations/d001/CASES.json'
$corpusPath = Join-Path $Authoring 'r007-20261011/_handoff/corpus.json'
$summaryPath = Join-Path $Authoring 'r009-20261013/results/s001/summary.json'
$manifestPath = Join-Path $Authoring 'r009-20261013/results/s001/MANIFEST.json'
foreach ($p in @($casesPath, $corpusPath, $summaryPath, $manifestPath)) {
  if (-not (Test-Path -LiteralPath $p)) { throw "not found: $p" }
}

function Get-Sha256([string]$path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-TextSha256([string]$text) {
  $bytes = [Text.Encoding]::UTF8.GetBytes($text)
  $hash = [Security.Cryptography.SHA256]::HashData($bytes)
  return ([BitConverter]::ToString($hash) -replace '-', '').ToLowerInvariant()
}

# The authoring side's digest(): json.dumps(value, sort_keys=True, separators=(",", ":")). Every
# leaf is the source's own token, which json.dumps wrote, so only the objects are re-ordered.
function ConvertTo-Canonical([System.Text.Json.JsonElement]$e) {
  switch ($e.ValueKind) {
    'Object' {
      # Keys are plain identifiers in these files (checked), so a key's JSON is its quoted name.
      $names = [string[]]@($e.EnumerateObject() | ForEach-Object { $_.Name })
      [Array]::Sort($names, [StringComparer]::Ordinal)
      $parts = foreach ($n in $names) {
        if ($n -notmatch '^[A-Za-z0-9_]+$') { throw "unexpected key: $n" }
        '"' + $n + '":' + (ConvertTo-Canonical $e.GetProperty($n))
      }
      return '{' + ($parts -join ',') + '}'
    }
    'Array' {
      $parts = foreach ($x in $e.EnumerateArray()) { ConvertTo-Canonical $x }
      return '[' + ($parts -join ',') + ']'
    }
    default { return $e.GetRawText() }
  }
}

# Pretty JSON with the source's number tokens: two-space indentation, keys in source order.
function ConvertTo-Pretty([System.Text.Json.JsonElement]$e, [string]$indent) {
  $inner = $indent + '  '
  switch ($e.ValueKind) {
    'Object' {
      $parts = foreach ($p in $e.EnumerateObject()) {
        $inner + '"' + $p.Name + '": ' + (ConvertTo-Pretty $p.Value $inner)
      }
      if (-not $parts) { return '{}' }
      return "{`n" + ($parts -join ",`n") + "`n$indent}"
    }
    'Array' {
      $parts = foreach ($x in $e.EnumerateArray()) { $inner + (ConvertTo-Pretty $x $inner) }
      if (-not $parts) { return '[]' }
      return "[`n" + ($parts -join ",`n") + "`n$indent]"
    }
    default { return $e.GetRawText() }
  }
}

function Read-Json([string]$path) {
  $options = [System.Text.Json.JsonDocumentOptions]::new()
  $options.MaxDepth = 256
  return [System.Text.Json.JsonDocument]::Parse([IO.File]::ReadAllText($path), $options)
}

$casesDoc = Read-Json $casesPath
$corpusDoc = Read-Json $corpusPath
$summaryDoc = Read-Json $summaryPath
$sources = [ordered]@{
  'r010-20261013/declarations/d001/CASES.json' = Get-Sha256 $casesPath
  'r007-20261011/_handoff/corpus.json' = Get-Sha256 $corpusPath
  'r009-20261013/results/s001/summary.json' = Get-Sha256 $summaryPath
  'r009-20261013/results/s001/MANIFEST.json' = Get-Sha256 $manifestPath
}
# The summary is the one the S result's manifest lists: the labels' numbers are the reviewed ones.
if (-not ([IO.File]::ReadAllText($manifestPath)).Contains($sources['r009-20261013/results/s001/summary.json'])) {
  throw 'the r009 S result manifest does not list this summary.json'
}

$rows = @($casesDoc.RootElement.GetProperty('rows').EnumerateArray())
$trajectories = @($corpusDoc.RootElement.GetProperty('trajectories').EnumerateArray())
$summaryRows = @($summaryDoc.RootElement.GetProperty('rows').EnumerateArray())
if ($rows.Count -ne 39 -or $trajectories.Count -ne 39 -or $summaryRows.Count -ne 39) {
  throw "expected 39 rows in each source: $($rows.Count), $($trajectories.Count), $($summaryRows.Count)"
}

# The eleven columns of a sample row, and where each comes from in a held-reference sample.
$columns = @('time_s', 'activation', 'fiber_length_m', 'tendon_force_N', 'passive_fiber_energy_J',
  'tendon_energy_J', 'external_work_J', 'active_output_work_J', 'dissipation_J',
  'active_throughput_J', 'external_throughput_J')
$ledgerColumns = @('external_work_J', 'active_output_work_J', 'dissipation_J', 'active_throughput_J',
  'external_throughput_J')

# The labels: D23 (4) and the owner's r009 results review. r009 S's numbers come from its summary.
$knownWork = @('corner_00_step', 'corner_02_step', 'corner_04_step', 'corner_06_step')
$knownForce = @('corner_04_passive', 'corner_06_passive')

New-Item -ItemType Directory -Force -Path (Join-Path $Out 'cases') | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$index = [Collections.Generic.List[string]]::new()
$identities = [Collections.Generic.List[string]]::new()
$expect = [Collections.Generic.List[string]]::new()
$checked = 0

for ($i = 0; $i -lt 39; $i++) {
  $row = $rows[$i]
  $traj = $trajectories[$i]
  $name = $row.GetProperty('case').GetString()
  if ($traj.GetProperty('inputs').GetProperty('case').GetString() -ne $name) { throw "row $i order differs: $name" }
  $summary = $summaryRows[$i]
  if ($summary.GetProperty('case').GetString() -ne $name) { throw "summary row $i order differs: $name" }
  $inputs = $row.GetProperty('inputs')
  $initial = $row.GetProperty('initial_state')
  $samples = $traj.GetProperty('held_reference_samples')

  # The copy against the hashes the sources carry.
  foreach ($pair in @(@('inputs_sha256', $inputs), @('initial_state_sha256', $initial),
      @('held_samples_sha256', $samples))) {
    $expected = $row.GetProperty($pair[0]).GetString()
    $got = Get-TextSha256 (ConvertTo-Canonical $pair[1])
    if ($got -ne $expected) { throw "$name $($pair[0]): the canonical form gives $got, the source says $expected" }
    $checked++
  }
  if ((ConvertTo-Canonical $inputs) -ne (ConvertTo-Canonical $traj.GetProperty('inputs'))) {
    throw "$name`: CASES.json and corpus.json disagree on the inputs"
  }
  $count = $samples.GetArrayLength()
  if ($count -ne $row.GetProperty('held_sample_count').GetInt32()) { throw "$name`: $count samples" }

  $lines = foreach ($s in $samples.EnumerateArray()) {
    $ledger = $s.GetProperty('ledger')
    $values = foreach ($c in $columns) {
      if ($ledgerColumns -contains $c) { $ledger.GetProperty($c).GetRawText() } else { $s.GetProperty($c).GetRawText() }
    }
    '    [' + ($values -join ', ') + ']'
  }
  $caseText = "{`n" +
    "  `"case`": `"$name`",`n" +
    "  `"inputs`": " + (ConvertTo-Pretty $inputs '  ') + ",`n" +
    "  `"initial_state`": " + (ConvertTo-Pretty $initial '  ') + ",`n" +
    "  `"columns`": [" + (($columns | ForEach-Object { "`"$_`"" }) -join ', ') + "],`n" +
    "  `"samples`": [`n" + ($lines -join ",`n") + "`n  ]`n}`n"
  [IO.File]::WriteAllText((Join-Path $Out "cases/$name.json"), $caseText, $utf8)

  $identity = $inputs.GetProperty('parameter_sha256').GetString()
  if (-not $identities.Contains($identity)) { $identities.Add($identity) }
  $limits = $row.GetProperty('limits')
  $index.Add("    {`"case`": `"$name`", `"parameter_sha256`": `"$identity`", " +
    "`"force_N`": $($limits.GetProperty('force_N').GetRawText()), " +
    "`"fiber_m`": $($limits.GetProperty('fiber_m').GetRawText()), " +
    "`"work_J`": $($limits.GetProperty('work_J').GetRawText()), " +
    "`"reference_throughput_J`": $($row.GetProperty('reference_throughput_J').GetRawText()), " +
    "`"held_sample_count`": $count, " +
    "`"held_samples_sha256`": `"$($row.GetProperty('held_samples_sha256').GetString())`"}")

  $metrics = $summary.GetProperty('metrics')
  $class = if ($knownWork -contains $name) { 'known_limit_work' } elseif ($knownForce -contains $name) { 'known_limit_force' } else { 'qualified' }
  $expect.Add("    {`"case`": `"$name`", `"class`": `"$class`", " +
    "`"r009_force_error_N`": $($metrics.GetProperty('tendon_force_N').GetRawText()), " +
    "`"r009_fiber_error_m`": $($metrics.GetProperty('fiber_length_m').GetRawText()), " +
    "`"r009_work_over_limit`": $($metrics.GetProperty('work_over_reference_limit').GetRawText()), " +
    "`"r009_classification`": `"$($summary.GetProperty('classification').GetString())`", " +
    "`"r009_located_crossings`": $($summary.GetProperty('located_crossings').GetRawText())}")
}

$sourceLines = foreach ($k in $sources.Keys) { "    {`"file`": `"$k`", `"sha256`": `"$($sources[$k])`"}" }
$corpusText = "{`n" +
  "  `"format_version`": 1,`n" +
  "  `"description`": `"The muscle line tier's qualification corpus: r010's 39 rows, their archived static starts and the r007 held-reference oracle at t = 0 and every 1/480 s parent endpoint.`",`n" +
  "  `"reference_model_sha256`": `"$($summaryDoc.RootElement.GetProperty('model_sha256').GetString())`",`n" +
  "  `"qualified_domain`": {`n    `"id`": `"synthetic-study-only`",`n    `"version`": 1,`n    `"parameter_sha256`": [`n" +
  (($identities | ForEach-Object { "      `"$_`"" }) -join ",`n") + "`n    ]`n  },`n" +
  "  `"sources`": [`n" + ($sourceLines -join ",`n") + "`n  ],`n" +
  "  `"cases`": [`n" + ($index -join ",`n") + "`n  ]`n}`n"
[IO.File]::WriteAllText((Join-Path $Out 'corpus.json'), $corpusText, $utf8)

$expectText = "{`n" +
  "  `"format_version`": 1,`n" +
  "  `"provenance`": {`n" +
  "    `"labels`": `"D23 (4) and the owner's r009 results review: the four step rows are work-only misses and the two first-sample rows force-only misses at r009 S, carried as known limits until r010's result review changes them`",`n" +
  "    `"r009_s_summary_sha256`": `"$($sources['r009-20261013/results/s001/summary.json'])`",`n" +
  "    `"r009_s_result_manifest_sha256`": `"$($sources['r009-20261013/results/s001/MANIFEST.json'])`",`n" +
  "    `"r009_declaration_sha256`":`"$($summaryDoc.RootElement.GetProperty('declaration_sha256').GetString())`",`n" +
  "    `"r009_execution_manifest_sha256`": `"$($summaryDoc.RootElement.GetProperty('execution_manifest_sha256').GetString())`",`n" +
  "    `"tolerance`": `"a labelled case's failing metric may be at most 1.5 times its r009 number`"`n  },`n" +
  "  `"cases`": [`n" + ($expect -join ",`n") + "`n  ]`n}`n"
[IO.File]::WriteAllText((Join-Path $Out 'expectations.json'), $expectText, $utf8)

$total = (Get-ChildItem -Recurse -File $Out | Measure-Object -Property Length -Sum).Sum
Write-Host "muscle-corpus: 39 cases, $($identities.Count) parameter identities, $checked source hashes reproduced, $([Math]::Round($total / 1MB, 2)) MB under $Out"
foreach ($k in $sources.Keys) { Write-Host "  $($sources[$k])  $k" }

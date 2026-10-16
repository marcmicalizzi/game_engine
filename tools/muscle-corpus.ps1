#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Writes content/muscle/ and systems/muscle/src/qualified_domains.inc: the muscle line tier's
  qualification corpus and its qualified domains, as data (docs/subsystems/muscle.md).

.DESCRIPTION
  tools/muscle-corpus.ps1 -Authoring <path to the authoring track's 06-muscle-activation>
                          [-Sources <sources.json>] [-Out <dir>] [-Table <qualified_domains.inc>]

  Reads the source packets content/muscle/sources.json names, as data and nothing else (nothing in
  the authoring worktree is run). A packet is the files of one qualification, each with its role
  and the SHA-256 it must have, the qualified domain (id and version) its rows belong to, and its
  labels:
    cases     a CASES.json: per row the inputs, the archived static start, the gate limits, the
              reference throughput and the per-row hashes
    samples   the same rows' trajectories with their held-reference oracle samples
    labels    a result's summary.json (per-row numbers the labels and their tolerance rest on) and
              the result manifest that lists it; optionally the declaration and execution manifest
              the summary must name, when a review fixed them
  The synthetic packet (r010's CASES.json, r007's handoff corpus.json, r009 S's and r010's
  summaries) is the first and today the only one. The row count comes from the sources.

  Writes, under -Out (default content/muscle beside this script's repository):
    corpus.json          the index: the packets, the qualified domains with every identity's full
                         nineteen-value parameter set, every case's domain, identity, limits,
                         reference throughput and provenance (packet, file, row), source hashes
    cases/<case>.json    a case's inputs and archived initial state, copied whole, and its samples
                         as rows of eleven columns
    expectations.json    each case's class (qualified, known_limit_work, known_limit_force) with the
                         label results' per-row numbers, and each packet's label provenance
  and -Table (default systems/muscle/src/qualified_domains.inc): the validator's table, every
  domain's identities with their nineteen values as exact hexadecimal floating literals, included
  by systems/muscle/src/validator.cpp and never edited by hand.
  README.md is written by hand and not by this script.

  **Numbers are copied as text, never recomputed**: every number in the JSON is the source file's
  own token (System.Text.Json's raw text), so a value is the reference's binary64 exactly and the
  files are a function of the sources' bytes. The table's literals are that token's binary64 in
  C's hexadecimal form (the token beside it in a comment), which every compiler reads exactly, with
  no decimal rounding to get right; tests/validator_tests.cpp holds the two to each other bit for
  bit. The script stops on a source whose SHA-256 is not the one sources.json names, and re-derives
  each row's hashes (inputs_sha256, initial_state_sha256, held_samples_sha256) and its parameter
  identity (r012's canonical form of the nineteen values) from the copied data under the authoring
  side's canonical form (sorted keys, compact separators, Python's number text), stopping if one
  does not match: the copy is checked against the hashes the sources carry.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)] [string]$Authoring,
  [string]$Sources,
  [string]$Out,
  [string]$Table
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Sources) { $Sources = Join-Path $repo 'content/muscle/sources.json' }
if (-not $Out) { $Out = Join-Path $repo 'content/muscle' }
if (-not $Table) { $Table = Join-Path $repo 'systems/muscle/src/qualified_domains.inc' }

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

# One line of an object of leaves, keys in the order given, the source's tokens.
function ConvertTo-Line([System.Text.Json.JsonElement]$e, [string[]]$names) {
  $parts = foreach ($n in $names) { '"' + $n + '": ' + $e.GetProperty($n).GetRawText() }
  return '{' + ($parts -join ', ') + '}'
}

function Read-Json([string]$path) {
  $options = [System.Text.Json.JsonDocumentOptions]::new()
  $options.MaxDepth = 256
  return [System.Text.Json.JsonDocument]::Parse([IO.File]::ReadAllText($path), $options)
}

# A property that may be absent: the element, or $null.
function Get-Optional([System.Text.Json.JsonElement]$e, [string]$name) {
  foreach ($p in $e.EnumerateObject()) { if ($p.Name -eq $name) { return $p.Value } }
  return $null
}

# A number token's binary64 as C's exact hexadecimal floating literal (0x1.f4p+9 for 1000.0).
# .NET's parse is correctly rounded (IEEE 754-2008 since .NET Core 3.0), so the literal is the
# token's value, and a compiler reads a hexadecimal literal with no rounding at all.
function Format-HexDouble([string]$token) {
  $x = [double]::Parse($token, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture)
  if ([double]::IsNaN($x) -or [double]::IsInfinity($x)) { throw "not a finite number: $token" }
  $bits = [BitConverter]::DoubleToInt64Bits($x)
  $sign = if ($bits -lt 0) { '-' } else { '' }
  $biased = [int](($bits -shr 52) -band 0x7FF)
  $mantissa = $bits -band (([long]1 -shl 52) - 1)
  $digits = $mantissa.ToString('x13').TrimEnd('0')
  $fraction = if ($digits) { '.' + $digits } else { '' }
  if ($biased -eq 0) {
    if ($mantissa -eq 0) { return "${sign}0x0p+0" }
    return "${sign}0x0${fraction}p-1022"
  }
  $exponent = $biased - 1023
  $exponentText = if ($exponent -ge 0) { "+$exponent" } else { "$exponent" }
  return "${sign}0x1${fraction}p$exponentText"
}

# The nineteen values of a parameter set, in the order FiberParameters and SeriesParameters declare
# them (systems/muscle/schemas/muscle.schema), with the authoring names the identity is taken over
# (r012's PARAMETER-FIELDS.json). An identity is the canonical form of exactly these leaves.
$fiberFields = [ordered]@{
  force_scale_N = 'force_scale'
  optimal_fiber_length_m = 'optimal_fiber_length'
  rigid_tendon_length_m = 'rigid_tendon_length'
  vmax_lopt_per_s = 'vmax_lopt_per_s'
  tau_activation_s = 'tau_activation'
  tau_deactivation_s = 'tau_deactivation'
  activation_smoothing = 'activation_smoothing'
  passive_strain_at_unit_force = 'passive_strain_at_unit_force'
  passive_shape = 'passive_shape'
  passive_zero_normalized_length = 'passive_zero_normalized_length'
  min_normalized_length = 'min_normalized_length'
  max_normalized_length = 'max_normalized_length'
  reference_area_m2 = 'reference_area_m2'
  reference_fiber_length_m = 'reference_fiber_length'
}
$seriesFields = [ordered]@{
  tendon_slack_length_m = 'tendon_slack_length'
  tendon_strain_at_unit_force = 'tendon_strain_at_unit_force'
  pennation_at_optimal_rad = 'pennation_at_optimal'
  normalized_damping = 'normalized_damping'
  minimum_cosine = 'minimum_cosine'
}

# r012's rules for the hashed object: exactly the two groups and their fields, every leaf a finite
# number and none of them negative zero.
function Assert-ParameterSet([System.Text.Json.JsonElement]$parameters, [string]$what) {
  $groups = [string[]]@($parameters.EnumerateObject() | ForEach-Object { $_.Name })
  if ((($groups | Sort-Object) -join ',') -ne 'fiber,series') { throw "$what`: parameter groups $($groups -join ', ')" }
  foreach ($pair in @(@('fiber', $fiberFields), @('series', $seriesFields))) {
    $group = $parameters.GetProperty($pair[0])
    $names = [string[]]@($group.EnumerateObject() | ForEach-Object { $_.Name })
    $want = [string[]]@($pair[1].Keys)
    if ((($names | Sort-Object) -join ',') -ne (($want | Sort-Object) -join ',')) {
      throw "$what`: $($pair[0]) fields $($names -join ', ')"
    }
    foreach ($n in $names) {
      $leaf = $group.GetProperty($n)
      if ($leaf.ValueKind -ne 'Number') { throw "$what`: $($pair[0]).$n is not a number" }
      if ($leaf.GetRawText() -match '^-0(\.0*)?([eE].*)?$') { throw "$what`: $($pair[0]).$n is negative zero" }
    }
  }
}

$sourcesDoc = Read-Json $Sources
$sourcesRoot = $sourcesDoc.RootElement
$referenceModel = $sourcesRoot.GetProperty('reference_model_sha256').GetString()

# The eleven columns of a sample row, and where each comes from in a held-reference sample.
$columns = @('time_s', 'activation', 'fiber_length_m', 'tendon_force_N', 'passive_fiber_energy_J',
  'tendon_energy_J', 'external_work_J', 'active_output_work_J', 'dissipation_J',
  'active_throughput_J', 'external_throughput_J')
$ledgerColumns = @('external_work_J', 'active_output_work_J', 'dissipation_J', 'active_throughput_J',
  'external_throughput_J')

New-Item -ItemType Directory -Force -Path (Join-Path $Out 'cases') | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$index = [Collections.Generic.List[string]]::new()
$expect = [Collections.Generic.List[string]]::new()
$provenance = [Collections.Generic.List[string]]::new()
$sourceLines = [Collections.Generic.List[string]]::new()
$packetLines = [Collections.Generic.List[string]]::new()
$caseNames = [Collections.Generic.HashSet[string]]::new()
# Domains in first-appearance order; each holds its identities in first-appearance order.
$domains = [ordered]@{}
$checked = 0

function Resolve-Source([string]$packet, [string]$role, [string]$relative, [string]$sha256) {
  $path = Join-Path $Authoring $relative
  if (-not (Test-Path -LiteralPath $path)) { throw "$packet $role`: not found: $path" }
  $got = Get-Sha256 $path
  if ($got -ne $sha256) { throw "$packet $role $relative`: SHA-256 $got, sources.json names $sha256" }
  $sourceLines.Add("    {`"packet`": `"$packet`", `"role`": `"$role`", `"file`": `"$relative`", `"sha256`": `"$got`"}")
  return $path
}

foreach ($packet in $sourcesRoot.GetProperty('packets').EnumerateArray()) {
  $packetId = $packet.GetProperty('id').GetString()
  $domainId = $packet.GetProperty('domain').GetProperty('id').GetString()
  $domainVersion = $packet.GetProperty('domain').GetProperty('version').GetInt32()
  if ($domainId -notmatch '^[a-z0-9-]+$' -or $domainVersion -lt 1) { throw "$packetId`: domain $domainId version $domainVersion" }

  $casesRel = $null; $casesPath = $null; $samplesRel = $null; $samplesPath = $null
  $results = [Collections.Generic.List[object]]::new()
  foreach ($s in $packet.GetProperty('sources').EnumerateArray()) {
    $role = $s.GetProperty('role').GetString()
    $rel = $s.GetProperty('path').GetString()
    $path = Resolve-Source $packetId $role $rel $s.GetProperty('sha256').GetString()
    switch ($role) {
      'cases' {
        if ($casesPath) { throw "$packetId`: two cases sources" }
        $casesRel = $rel; $casesPath = $path
      }
      'samples' {
        if ($samplesPath) { throw "$packetId`: two samples sources" }
        $samplesRel = $rel; $samplesPath = $path
      }
      'labels' {
        $name = $s.GetProperty('result').GetString()
        if ($name -notmatch '^r[0-9]+$') { throw "$packetId`: a label result's name: $name" }
        $summarySha = $s.GetProperty('sha256').GetString()
        $manifestPath = Resolve-Source $packetId 'labels_manifest' $s.GetProperty('manifest').GetString() $s.GetProperty('manifest_sha256').GetString()
        # The summary is the one its result manifest lists: the labels' numbers are the reviewed ones.
        if (-not ([IO.File]::ReadAllText($manifestPath)).Contains($summarySha)) {
          throw "$packetId`: the $name result manifest does not list this summary.json"
        }
        $doc = Read-Json $path
        $root = $doc.RootElement
        # A review that fixed the declaration and execution manifest: another one is evidence nobody
        # reviewed.
        foreach ($k in @('declaration_sha256', 'execution_manifest_sha256')) {
          $want = Get-Optional $s $k
          if ($null -ne $want -and $root.GetProperty($k).GetString() -ne $want.GetString()) {
            throw "$packetId`: the $name summary names another $k than the review did"
          }
        }
        $model = Get-Optional $root 'model_sha256'
        if ($null -ne $model -and $model.GetString() -ne $referenceModel) {
          throw "$packetId`: the $name summary's model is $($model.GetString()), not the reference $referenceModel"
        }
        $results.Add([pscustomobject]@{
            name = $name; root = $root; rows = @($root.GetProperty('rows').EnumerateArray())
            summary_sha256 = $summarySha; manifest_sha256 = $s.GetProperty('manifest_sha256').GetString()
          })
      }
      default { throw "$packetId`: unknown source role $role" }
    }
  }
  if (-not $casesPath -or -not $samplesPath) { throw "$packetId`: a packet needs a cases and a samples source" }

  $casesDoc = Read-Json $casesPath
  $samplesDoc = Read-Json $samplesPath
  $rows = @($casesDoc.RootElement.GetProperty('rows').EnumerateArray())
  $trajectories = @($samplesDoc.RootElement.GetProperty('trajectories').EnumerateArray())
  if ($rows.Count -eq 0 -or $trajectories.Count -ne $rows.Count) {
    throw "$packetId`: $($rows.Count) case rows and $($trajectories.Count) trajectories"
  }
  foreach ($r in $results) {
    if ($r.rows.Count -ne $rows.Count) { throw "$packetId`: the $($r.name) summary has $($r.rows.Count) rows, not $($rows.Count)" }
  }

  # The labels: a decision's lists of known limits, and which result a labelled case's tolerance is
  # measured from. A packet with no labels has every case qualified.
  $knownWork = @(); $knownForce = @(); $toleranceResult = $null
  $labels = Get-Optional $packet 'labels'
  if ($null -ne $labels) {
    $knownWork = @($labels.GetProperty('known_limit_work').EnumerateArray() | ForEach-Object { $_.GetString() })
    $knownForce = @($labels.GetProperty('known_limit_force').EnumerateArray() | ForEach-Object { $_.GetString() })
    $toleranceResult = $labels.GetProperty('tolerance_result').GetString()
    if (($knownWork.Count + $knownForce.Count) -gt 0 -and -not ($results | Where-Object { $_.name -eq $toleranceResult })) {
      throw "$packetId`: the tolerance result $toleranceResult is not one of the packet's label results"
    }
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add("      `"packet`": `"$packetId`"")
    $lines.Add("      `"decision`": `"$($labels.GetProperty('decision').GetString())`"")
    $lines.Add("      `"labels`": $($labels.GetProperty('description').GetRawText())")
    foreach ($r in $results) {
      $lines.Add("      `"$($r.name)_summary_sha256`": `"$($r.summary_sha256)`"")
      $lines.Add("      `"$($r.name)_result_manifest_sha256`": `"$($r.manifest_sha256)`"")
      $lines.Add("      `"$($r.name)_declaration_sha256`": `"$($r.root.GetProperty('declaration_sha256').GetString())`"")
      $lines.Add("      `"$($r.name)_execution_manifest_sha256`": `"$($r.root.GetProperty('execution_manifest_sha256').GetString())`"")
    }
    $lines.Add("      `"tolerance`": $($labels.GetProperty('tolerance').GetRawText())")
    $provenance.Add("    {`n" + ($lines -join ",`n") + "`n    }")
  }

  $domainKey = "$domainId version $domainVersion"
  if (-not $domains.Contains($domainKey)) {
    $domains[$domainKey] = [pscustomobject]@{ id = $domainId; version = $domainVersion; identities = [ordered]@{} }
  }
  $domain = $domains[$domainKey]
  $labelled = 0

  for ($i = 0; $i -lt $rows.Count; $i++) {
    $row = $rows[$i]
    $traj = $trajectories[$i]
    $name = $row.GetProperty('case').GetString()
    if ($name -notmatch '^[A-Za-z0-9_]+$') { throw "$packetId row $i`: a case name: $name" }
    if (-not $caseNames.Add($name)) { throw "$packetId row $i`: the case $name is already in the corpus" }
    if ($traj.GetProperty('inputs').GetProperty('case').GetString() -ne $name) { throw "$packetId row $i order differs: $name" }
    foreach ($r in $results) {
      if ($r.rows[$i].GetProperty('case').GetString() -ne $name) { throw "$packetId`: $($r.name) summary row $i order differs: $name" }
    }
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
      throw "$name`: the cases and samples sources disagree on the inputs"
    }
    # The parameter identity, under r012's canonical form of the nineteen values.
    $parameters = $inputs.GetProperty('parameters')
    Assert-ParameterSet $parameters $name
    $identity = $inputs.GetProperty('parameter_sha256').GetString()
    $canonical = ConvertTo-Canonical $parameters
    $got = Get-TextSha256 $canonical
    if ($got -ne $identity) { throw "$name parameter_sha256: the canonical form gives $got, the source says $identity" }
    $checked++
    if ($domain.identities.Contains($identity)) {
      if ($domain.identities[$identity].canonical -ne $canonical) { throw "$name`: $identity names two parameter sets" }
    } else {
      $domain.identities[$identity] = [pscustomobject]@{
        canonical = $canonical; parameters = $parameters; packet = $packetId; case = $name; row = $i
      }
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

    $limits = $row.GetProperty('limits')
    $index.Add("    {`"case`": `"$name`", `"domain`": `"$domainId`", `"domain_version`": $domainVersion, " +
      "`"parameter_sha256`": `"$identity`", " +
      "`"force_N`": $($limits.GetProperty('force_N').GetRawText()), " +
      "`"fiber_m`": $($limits.GetProperty('fiber_m').GetRawText()), " +
      "`"work_J`": $($limits.GetProperty('work_J').GetRawText()), " +
      "`"reference_throughput_J`": $($row.GetProperty('reference_throughput_J').GetRawText()), " +
      "`"held_sample_count`": $count, " +
      "`"held_samples_sha256`": `"$($row.GetProperty('held_samples_sha256').GetString())`", " +
      "`"source`": {`"packet`": `"$packetId`", `"cases`": `"$casesRel`", `"row`": $i, `"samples`": `"$samplesRel`", `"trajectory`": $i}}")

    $class = if ($knownWork -contains $name) { 'known_limit_work' } elseif ($knownForce -contains $name) { 'known_limit_force' } else { 'qualified' }
    $fields = [Collections.Generic.List[string]]::new()
    $fields.Add("`"case`": `"$name`"")
    $fields.Add("`"class`": `"$class`"")
    if ($class -ne 'qualified') { $fields.Add("`"tolerance_result`": `"$toleranceResult`""); $labelled++ }
    foreach ($r in $results) {
      $rr = $r.rows[$i]
      $metrics = $rr.GetProperty('metrics')
      $n = $r.name
      $fields.Add("`"${n}_force_error_N`": $($metrics.GetProperty('tendon_force_N').GetRawText())")
      $first = Get-Optional $rr 'first_nominal_force'
      if ($null -ne $first) { $fields.Add("`"${n}_first_sample_force_error_N`": $($first.GetProperty('error_N').GetRawText())") }
      $fields.Add("`"${n}_fiber_error_m`": $($metrics.GetProperty('fiber_length_m').GetRawText())")
      $fields.Add("`"${n}_work_over_limit`": $($metrics.GetProperty('work_over_reference_limit').GetRawText())")
      $fields.Add("`"${n}_classification`": `"$($rr.GetProperty('classification').GetString())`"")
      $fields.Add("`"${n}_located_crossings`": $($rr.GetProperty('located_crossings').GetRawText())")
    }
    $expect.Add('    {' + ($fields -join ', ') + '}')
  }
  if ($labelled -ne ($knownWork.Count + $knownForce.Count)) { throw "$packetId`: a labelled case is not one of the packet's rows" }
  $packetLines.Add("    {`"id`": `"$packetId`", `"domain`": `"$domainId`", `"domain_version`": $domainVersion, `"cases`": $($rows.Count), " +
    "`"description`": $($packet.GetProperty('description').GetRawText())}")
}

# corpus.json: the domains with every identity's parameters, then the sources and the cases.
$fiberNames = [string[]]@($fiberFields.Keys)
$seriesNames = [string[]]@($seriesFields.Keys)
$domainBlocks = foreach ($d in $domains.Values) {
  $entries = foreach ($id in $d.identities.Keys) {
    $q = $d.identities[$id]
    "        {`n" +
    "          `"parameter_sha256`": `"$id`",`n" +
    "          `"source`": {`"packet`": `"$($q.packet)`", `"case`": `"$($q.case)`", `"row`": $($q.row)},`n" +
    "          `"fiber`": " + (ConvertTo-Line $q.parameters.GetProperty('fiber') $fiberNames) + ",`n" +
    "          `"series`": " + (ConvertTo-Line $q.parameters.GetProperty('series') $seriesNames) + "`n" +
    "        }"
  }
  "    {`n      `"id`": `"$($d.id)`",`n      `"version`": $($d.version),`n      `"identities`": [`n" +
  ($entries -join ",`n") + "`n      ]`n    }"
}
$total = $caseNames.Count
$corpusText = "{`n" +
  "  `"format_version`": 2,`n" +
  "  `"description`": `"The muscle line tier's qualification corpus: every source packet's cases with their archived static starts and held-reference oracle samples, the qualified domains their parameter identities form, and where each case came from. Written by tools/muscle-corpus.ps1 from content/muscle/sources.json.`",`n" +
  "  `"reference_model_sha256`": `"$referenceModel`",`n" +
  "  `"packets`": [`n" + ($packetLines -join ",`n") + "`n  ],`n" +
  "  `"qualified_domains`": [`n" + ($domainBlocks -join ",`n") + "`n  ],`n" +
  "  `"sources`": [`n" + ($sourceLines -join ",`n") + "`n  ],`n" +
  "  `"cases`": [`n" + ($index -join ",`n") + "`n  ]`n}`n"
[IO.File]::WriteAllText((Join-Path $Out 'corpus.json'), $corpusText, $utf8)

$expectText = "{`n" +
  "  `"format_version`": 2,`n" +
  "  `"provenance`": [`n" + ($provenance -join ",`n") + "`n  ],`n" +
  "  `"cases`": [`n" + ($expect -join ",`n") + "`n  ]`n}`n"
[IO.File]::WriteAllText((Join-Path $Out 'expectations.json'), $expectText, $utf8)

# The validator's table: every domain's identities, each with its nineteen values as hexadecimal
# literals in declaration order, the source token beside each.
$tableLines = [Collections.Generic.List[string]]::new()
$tableLines.Add('// GENERATED by tools/muscle-corpus.ps1 from content/muscle/sources.json: never edit by hand.')
$tableLines.Add('// Change the sources and run the script; tests/validator_tests.cpp holds this table to')
$tableLines.Add('// content/muscle/corpus.json bit for bit. Included once, by systems/muscle/src/validator.cpp.')
$tableLines.Add('//')
$tableLines.Add('// Each value is its source token''s binary64 as an exact hexadecimal floating literal, which a')
$tableLines.Add('// compiler reads with no rounding; the token itself is the comment beside it.')
$tableLines.Add('')
$domainNumber = 0
$domainRefs = [Collections.Generic.List[string]]::new()
$domainTexts = [Collections.Generic.List[string]]::new()
foreach ($d in $domains.Values) {
  $tableLines.Add("// $($d.id) version $($d.version): $($d.identities.Count) identities.")
  $tableLines.Add("constexpr QualifiedSet k_domain_${domainNumber}_sets[] = {")
  foreach ($id in $d.identities.Keys) {
    $q = $d.identities[$id]
    $tableLines.Add('    {')
    $tableLines.Add("        sha256_literal(`"$id`"),  // first in $($q.case), $($q.packet) row $($q.row)")
    foreach ($group in @(@('fiber', 'FiberParameters', $fiberFields), @('series', 'SeriesParameters', $seriesFields))) {
      $values = $q.parameters.GetProperty($group[0])
      $tableLines.Add("        $($group[1]){")
      foreach ($k in $group[2].Keys) {
        $token = $values.GetProperty($k).GetRawText()
        $tableLines.Add("            .$($group[2][$k]) = $(Format-HexDouble $token),  // $token")
      }
      $tableLines.Add('        },')
    }
    $tableLines.Add('    },')
  }
  $tableLines.Add('};')
  $tableLines.Add('')
  $domainRefs.Add("    {`"$($d.id)`", $($d.version), std::span<const QualifiedSet>(k_domain_${domainNumber}_sets)},")
  $domainTexts.Add("$($d.id) version $($d.version)")
  $domainNumber++
}
$tableLines.Add('constexpr QualifiedDomain k_domains[] = {')
foreach ($l in $domainRefs) { $tableLines.Add($l) }
$tableLines.Add('};')
$tableLines.Add('')
$tableLines.Add('// The domains by name, for a refusal''s rule.')
$tableLines.Add("constexpr const char k_domains_text[] = `"$($domainTexts -join ', ')`";")
[IO.File]::WriteAllText($Table, ($tableLines -join "`n") + "`n", $utf8)

$bytes = (Get-ChildItem -Recurse -File $Out | Measure-Object -Property Length -Sum).Sum
$identityCount = ($domains.Values | ForEach-Object { $_.identities.Count } | Measure-Object -Sum).Sum
Write-Host "muscle-corpus: $total cases from $($packetLines.Count) packet(s), $($domains.Count) domain(s) with $identityCount identities, $checked source hashes reproduced, $([Math]::Round($bytes / 1MB, 2)) MB under $Out"
foreach ($l in $sourceLines) { Write-Host "  $l" }
Write-Host "  table: $Table"

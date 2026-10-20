#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Builds a material pack — the default PBR sets generated meshes fall back to — from a staging
  directory laid out by role.

.DESCRIPTION
  A material pack (docs/subsystems/materials.md, content/materials/README.md) is a zip holding one
  tiled set per role (`palm/bark`, `rock/granite`), a `pack.json` manifest and a `LICENSES.md`.
  `tools/fetch-materials.ps1` unpacks one into the git-ignored `content/materials/`; this script
  makes one. Neither the staging directory nor the zip belongs in the repository: generated images
  live outside it (AGENTS.md, docs/content-generation.md), under D:\workspace\game_engine_local\.

  The staging directory holds one directory per role, `<From>/<species>/<part>/`, each with

    base_color.png (or basecolor.png)  sRGB colour
    normal.png                         tangent-space normals, OpenGL convention (+Y)
    roughness.png, metallic.png        greyscale, linear
    height.png                         greyscale, linear, uncalibrated
    material.json                      the material lab's record: `tile_m` (or `tile_width_m`),
                                       `normal_convention`, and the normal strength
                                       (`normal_scale`, or the lab's `normal_strength_preview`)
    provenance.json                    what made it: `models`, a list of {name, revision}, and
                                       the `seed` and `settings` the run used

  and the pack copies the five maps as `<role>/<map>.png`, writing for each role the maps' SHA-256,
  their resolution, `tile_m`, the normal convention and strength, the provenance as text, and the
  licence of every model that made it. It refuses, before writing anything:

    - a role with a map missing, or a map whose sides are not powers of two;
    - a role whose normal convention is not OpenGL (the renderer reads glTF's, +Y);
    - a role with no provenance, or one naming a model the licence table below does not have —
      a pack ships its images to every game made with the engine, so a model whose terms have not
      been read is not guessed at (docs/content-generation.md, "Material packs").

.EXAMPLE
  tools/pack-materials.ps1 -From D:\workspace\game_engine_local\packages\staging-palm `
    -Out D:\workspace\game_engine_local\packages\materials-palm-2026-10-19.zip -Name palm-defaults
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$From,
  [Parameter(Mandatory = $true)][string]$Out,
  [string]$Name = 'engine-materials',
  [string]$Version = (Get-Date -Format 'yyyy-MM-dd'),
  # A sentence for the pack as a whole, written at the top of LICENSES.md; the per-model lines
  # below are always written whatever this says.
  [string]$Licence = 'Generated material sets. The images are outputs of the models named per role below; each model''s licence line says what it claims over its outputs.'
)

$ErrorActionPreference = 'Stop'

# The models a shipped pack may come from, and what each one's licence says of its outputs. A
# model is added here deliberately, in a commit that says why, after its terms have been read
# (docs/content-generation.md, "Material packs"). Matched against a provenance model's name,
# case-insensitively, as a prefix (so "StableMaterials" matches "gvecchio/StableMaterials" only
# when written so: the name is what the lab records).
$licences = [ordered]@{
  'StableMaterials' = 'StableMaterials: OpenRAIL-M, as the model''s metadata declares. The licence claims no rights over the outputs; its use restrictions bind the use of the model, not the images.'
  'PBRify'          = 'PBRify UpscalerV4: CC0-1.0 (public domain dedication).'
}

$maps = [ordered]@{
  'base_color' = @('base_color.png', 'basecolor.png')
  'normal'     = @('normal.png')
  'roughness'  = @('roughness.png')
  'metallic'   = @('metallic.png')
  'height'     = @('height.png')
}

function Fail([string]$message) {
  [Console]::Error.WriteLine("pack-materials: $message")
  exit 1
}

# A PNG's sides from its IHDR chunk, which the format puts first: bytes 16..23, big-endian.
function Get-PngSize([string]$path) {
  $stream = [IO.File]::OpenRead($path)
  try {
    $head = New-Object byte[] 24
    if ($stream.Read($head, 0, 24) -ne 24) { return $null }
  } finally { $stream.Dispose() }
  $signature = @(0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)
  for ($i = 0; $i -lt 8; $i++) { if ($head[$i] -ne $signature[$i]) { return $null } }
  if ([Text.Encoding]::ASCII.GetString($head, 12, 4) -ne 'IHDR') { return $null }
  $w = ([int]$head[16] -shl 24) -bor ([int]$head[17] -shl 16) -bor ([int]$head[18] -shl 8) -bor [int]$head[19]
  $h = ([int]$head[20] -shl 24) -bor ([int]$head[21] -shl 16) -bor ([int]$head[22] -shl 8) -bor [int]$head[23]
  return [pscustomobject]@{ Width = $w; Height = $h }
}

function Test-PowerOfTwo([int]$n) { return $n -gt 0 -and (($n -band ($n - 1)) -eq 0) }

function Get-LicenceLine([string]$model) {
  foreach ($key in $licences.Keys) {
    if ($model.ToLowerInvariant().StartsWith($key.ToLowerInvariant())) { return $licences[$key] }
  }
  return $null
}

if (-not (Test-Path -LiteralPath $From -PathType Container)) { Fail "no staging directory at $From" }
$from = (Resolve-Path -LiteralPath $From).Path

# Every role: two levels down, <species>/<part>.
$roleDirs = @(Get-ChildItem -LiteralPath $from -Directory | ForEach-Object {
    $species = $_
    Get-ChildItem -LiteralPath $species.FullName -Directory | ForEach-Object {
      [pscustomobject]@{ Role = "$($species.Name)/$($_.Name)"; Dir = $_.FullName }
    }
  } | Sort-Object Role)
if ($roleDirs.Count -eq 0) { Fail "$from holds no <species>/<part> directories" }

$roles = [ordered]@{}
$licenceRows = New-Object System.Collections.Generic.List[string]
foreach ($entry in $roleDirs) {
  $role = $entry.Role
  if ($role -notmatch '^[a-z0-9_]+/[a-z0-9_]+$') { Fail "$role is not a role (<species>/<part>, lower case)" }
  $materialPath = Join-Path $entry.Dir 'material.json'
  $provenancePath = Join-Path $entry.Dir 'provenance.json'
  if (-not (Test-Path -LiteralPath $materialPath)) { Fail "$role has no material.json" }
  if (-not (Test-Path -LiteralPath $provenancePath)) { Fail "$role has no provenance.json: a pack says what made every set" }
  $material = Get-Content -Raw -LiteralPath $materialPath | ConvertFrom-Json
  $provenance = Get-Content -Raw -LiteralPath $provenancePath | ConvertFrom-Json

  $tile = if ($null -ne $material.tile_m) { [double]$material.tile_m } elseif ($null -ne $material.tile_width_m) { [double]$material.tile_width_m } else { $null }
  if ($null -eq $tile -or $tile -lt 0.05 -or $tile -gt 20) { Fail "$role has no tile_m from 0.05 to 20 m in material.json" }
  $strength = if ($null -ne $material.normal_scale) { [double]$material.normal_scale } elseif ($null -ne $material.normal_strength_preview) { [double]$material.normal_strength_preview } else { 1.0 }
  if ($strength -lt 0 -or $strength -gt 4) { Fail "$role's normal strength $strength is not 0 to 4" }
  $convention = "$($material.normal_convention)"
  if ($convention -notmatch 'opengl') { Fail "$role's normal convention is not OpenGL (+Y): '$convention'" }

  $models = @($provenance.models)
  if ($models.Count -eq 0) { Fail "$role's provenance names no model" }
  $roleLicences = New-Object System.Collections.Generic.List[string]
  foreach ($model in $models) {
    $line = Get-LicenceLine "$($model.name)"
    if ($null -eq $line) { Fail "$role was made by '$($model.name)', which has no licence line in this script: read its terms and add it, or leave the set out" }
    if (-not $roleLicences.Contains($line)) { $roleLicences.Add($line) }
  }

  $mapRecords = [ordered]@{}
  foreach ($map in $maps.Keys) {
    $file = $null
    foreach ($candidate in $maps[$map]) {
      $p = Join-Path $entry.Dir $candidate
      if (Test-Path -LiteralPath $p) { $file = $p; break }
    }
    if ($null -eq $file) { Fail "$role has no $map map ($($maps[$map] -join ' or '))" }
    $size = Get-PngSize $file
    if ($null -eq $size) { Fail "$role's $map map is not a PNG" }
    if (-not (Test-PowerOfTwo $size.Width) -or -not (Test-PowerOfTwo $size.Height)) {
      Fail "$role's $map map is $($size.Width)x$($size.Height): a tiled set's sides are powers of two"
    }
    $mapRecords[$map] = [ordered]@{
      file   = "$role/$map.png"
      source = $file
      sha256 = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
      width  = $size.Width
      height = $size.Height
    }
  }

  # The provenance as text: the lab's own fields copied, nothing reinterpreted.
  $record = [ordered]@{
    models   = $models
    seed     = $provenance.seed
    settings = $provenance.settings
  }
  foreach ($field in @('id', 'label', 'seed', 'prompt', 'transforms', 'native_resolution', 'tile_m_status')) {
    if ($null -ne $material.$field -and -not $record.Contains("lab_$field")) { $record["lab_$field"] = $material.$field }
  }
  $roles[$role] = [ordered]@{
    tile_m            = $tile
    normal_scale      = $strength
    normal_convention = 'opengl'
    maps              = $mapRecords
    provenance        = $record
    licence           = ($roleLicences -join ' ')
  }
  $licenceRows.Add("| ``$role`` | $(($models | ForEach-Object { "$($_.name) $($_.revision)".Trim() }) -join ', ') | $($roleLicences -join ' ') |")
}

# Lay the pack out in a scratch directory and zip it.
$work = Join-Path ([IO.Path]::GetTempPath()) "engine-pack-materials-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
  foreach ($role in $roles.Keys) {
    foreach ($map in $roles[$role].maps.Keys) {
      $record = $roles[$role].maps[$map]
      $target = Join-Path $work $record.file
      New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
      Copy-Item -LiteralPath $record.source -Destination $target
      $record.Remove('source')
    }
  }
  $manifest = [ordered]@{
    format  = 'engine.materials.pack/1'
    name    = $Name
    version = $Version
    licence = $Licence
    roles   = $roles
  }
  $manifest | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath (Join-Path $work 'pack.json') -Encoding utf8NoBOM
  $md = @(
    "# $Name ${Version}: licences",
    '',
    $Licence,
    '',
    '| Role | Made by | Licence of the model |',
    '|---|---|---|'
  ) + $licenceRows
  $md -join "`n" | Set-Content -LiteralPath (Join-Path $work 'LICENSES.md') -Encoding utf8NoBOM

  $outFull = [IO.Path]::GetFullPath($Out)
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outFull) | Out-Null
  if (Test-Path -LiteralPath $outFull) { Remove-Item -LiteralPath $outFull -Force }
  Compress-Archive -Path (Join-Path $work '*') -DestinationPath $outFull
  $sha = (Get-FileHash -LiteralPath $outFull -Algorithm SHA256).Hash.ToLowerInvariant()
  [ordered]@{
    pack   = $outFull
    sha256 = $sha
    name   = $Name
    version = $Version
    roles  = @($roles.Keys)
    bytes  = (Get-Item -LiteralPath $outFull).Length
  } | ConvertTo-Json -Compress
} finally {
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Downloads permissively licensed sample assets into content/samples (git-ignored).

.DESCRIPTION
  Fetches a fixed list of models from the Khronos glTF-Sample-Assets repository at a pinned
  commit, so every machine gets the same bytes, and records each model's license in
  content/samples/LICENSES.md from its metadata.json. Only models whose license is in the
  allow list below are fetched; anything else is refused, so a CC-BY-NC model cannot slip
  in by editing the model list without also editing the policy.

  The files land outside version control: binaries do not belong in the repository
  (AGENTS.md), and the script is the reproducible step. engine-view renders one with
  --mesh content/samples/<Name>/<file>.

  The GitHub API allows 60 anonymous requests per hour, which is plenty for one run; set
  GH_TOKEN to raise the limit.

.EXAMPLE
  tools/fetch-samples.ps1                    # every model in the list
  tools/fetch-samples.ps1 -Name Suzanne      # one model
#>
[CmdletBinding()]
param(
  [string[]]$Name = @(),
  [string]$Out = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'content/samples'),
  [switch]$Force
)

$ErrorActionPreference = 'Stop'

$repo = 'KhronosGroup/glTF-Sample-Assets'
$commit = '90d7ede14c7e280af263824604b427a1ca02cb66'   # main on 2026-09-17
$allowedLicenses = @('CC0-1.0', 'CC-BY-4.0')

# Model directory -> variant directory to fetch. glTF-Binary is one file; glTF is a .gltf,
# a .bin, and loose textures. Draco and KTX variants are not readable by domain/assets.
#
# The last two are **skinned and animated**, which is what `engine-view --mesh <file> --animate`
# needs: a skin the importer turns into an anim::Skeleton, a per-vertex geometry::SkinBinding
# stream, and at least one clip. Fox carries three cycles (Survey, Walk, Run) on a 24-joint rig
# and RiggedFigure one on a 19-joint humanoid, which is also the case that exercises
# anim::map_joints' standard-skeleton mapping. Both pass the allow list — Fox is CC0 for the
# model with CC-BY-4.0 on the rigging, the animation and the glTF conversion, RiggedFigure is
# CC-BY-4.0 throughout — and the attribution CC-BY asks for is in content/README.md as well as in
# the generated LICENSES.md, because the generated one is git-ignored with the assets.
#
# CesiumMan is the obvious third and is deliberately **not** here: its metadata carries a second
# legal entry, `LicenseRef-LegalMark-Cesium`, for the logo on its shirt. That is a trademark
# notice rather than a copyright licence, but the allow list below is a policy and not a
# judgement call, so a model whose metadata names a licence this project has not vetted is not
# fetched. Widening the list is a decision to take deliberately, in a commit that says why.
#
# The last four carry **morph targets**, which is what `engine-view --morph` and `--morph-animate`
# and the morph stream of the `.clusters` container need a real file for (docs/subsystems/
# geometry.md, "Morph channels"). Each tests something the procedural fixture cannot:
# AnimatedMorphCube has two targets with POSITION, NORMAL and TANGENT deltas under a node scaled by
# 100 and one clip that animates both weights (CC0); MorphPrimitivesTest has one target across two
# primitives with **no NORMAL deltas** and a non-zero default weight on the mesh (CC-BY-4.0, and its
# second legal entry — the Draco variant's compression, also CC-BY-4.0 — passes the list too);
# MorphStressTest has eight named targets over two primitives and three clips (CC-BY-4.0); and
# SimpleMorph is one triangle with **no normals at all** and two targets (CC0). The candidate
# usually named beside them, AnimatedMorphSphere, is **not in this repository** at the pinned
# commit — it was left behind in the retired glTF-Sample-Models — so it is not here rather than
# refused.
$models = [ordered]@{
  'Suzanne'             = 'glTF'
  'Avocado'             = 'glTF-Binary'
  'BoomBox'             = 'glTF-Binary'
  'Corset'              = 'glTF-Binary'
  'Lantern'             = 'glTF-Binary'
  'SciFiHelmet'         = 'glTF'
  'FlightHelmet'        = 'glTF'
  'Fox'                 = 'glTF-Binary'
  'RiggedFigure'        = 'glTF-Binary'
  'AnimatedMorphCube'   = 'glTF-Binary'
  'MorphPrimitivesTest' = 'glTF-Binary'
  'MorphStressTest'     = 'glTF-Binary'
  'SimpleMorph'         = 'glTF'
}

$headers = @{ 'User-Agent' = 'engine-fetch-samples' }
if ($env:GH_TOKEN) { $headers['Authorization'] = "Bearer $env:GH_TOKEN" }

function Get-Listing([string]$path) {
  $url = "https://api.github.com/repos/$repo/contents/$path`?ref=$commit"
  Invoke-RestMethod -Uri $url -Headers $headers
}

function Get-File([string]$path, [string]$destination, [long]$size) {
  if (-not $Force -and (Test-Path $destination) -and (Get-Item $destination).Length -eq $size) {
    return $false
  }
  $url = "https://raw.githubusercontent.com/$repo/$commit/$path"
  $dir = Split-Path -Parent $destination
  if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
  Invoke-WebRequest -Uri $url -Headers $headers -OutFile $destination
  return $true
}

$selected = if ($Name.Count -gt 0) { $Name } else { @($models.Keys) }
$rows = @()
foreach ($model in $selected) {
  if (-not $models.Contains($model)) { throw "fetch-samples: unknown model '$model'; known: $($models.Keys -join ', ')" }
  $variant = $models[$model]
  Write-Host "== $model ($variant)"

  $metadataPath = Join-Path $Out "$model/metadata.json"
  Get-File "Models/$model/metadata.json" $metadataPath -1 | Out-Null
  $metadata = Get-Content $metadataPath -Raw | ConvertFrom-Json
  $licenses = @($metadata.legal | ForEach-Object { $_.license } | Sort-Object -Unique)
  foreach ($license in $licenses) {
    if ($allowedLicenses -notcontains $license) {
      Remove-Item -Recurse -Force (Join-Path $Out $model)
      throw "fetch-samples: $model is licensed '$license', which is not in the allow list ($($allowedLicenses -join ', ')); not fetched"
    }
  }
  Get-File "Models/$model/LICENSE.md" (Join-Path $Out "$model/LICENSE.md") -1 | Out-Null

  $fetched = 0
  $bytes = 0
  foreach ($entry in (Get-Listing "Models/$model/$variant")) {
    if ($entry.type -ne 'file') { continue }
    $destination = Join-Path $Out "$model/$($entry.name)"
    if (Get-File $entry.path $destination $entry.size) { $fetched++ }
    $bytes += $entry.size
  }
  Write-Host ("   {0} file(s) fetched, {1:N1} MB on disk, license {2}" -f $fetched, ($bytes / 1MB), ($licenses -join ' + '))
  $artists = @($metadata.legal | ForEach-Object { $_.artist } | Where-Object { $_ } | Sort-Object -Unique) -join ', '
  $rows += "| $model | $variant | $($licenses -join ' + ') | $artists | $($metadata.legal[0].owner) |"
}

$licenseFile = Join-Path $Out 'LICENSES.md'
$text = @(
  '# Sample assets',
  '',
  "Fetched by ``tools/fetch-samples.ps1`` from https://github.com/$repo at commit ``$commit``.",
  'These files are not part of the engine and are not committed; each model directory carries',
  'the LICENSE.md and metadata.json the source repository ships.',
  '',
  '| Model | Variant | License | Artist(s) | Owner |',
  '|---|---|---|---|---|'
) + $rows
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force $Out | Out-Null }
Set-Content -Path $licenseFile -Value ($text -join "`n") -NoNewline
Write-Host "wrote $licenseFile"

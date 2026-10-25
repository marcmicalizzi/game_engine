#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Cook the G88 meadow fixture outside the checkout, with a shared density paint and patch family.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Out,
  [string]$Preset = 'msvc-debug',
  [ValidateRange(1,16)][int]$Variants = 8,
  [string]$Recipe = (Join-Path $PSScriptRoot '../content/test-scenes/meadow/dense.flora.json'),
  [ValidateSet('tall_fescue','perennial_ryegrass','timothy','cocksfoot','wood_melick',
               'wavy_hairgrass','bent','red_fescue')][string]$Species
)
$ErrorActionPreference = 'Stop'
$swardRoot = Split-Path $PSScriptRoot
$swardOut = [IO.Path]::GetFullPath($Out)
$swardRepo = [IO.Path]::GetFullPath($swardRoot).TrimEnd('\','/')
if ($swardOut.Equals($swardRepo,[StringComparison]::OrdinalIgnoreCase) -or
    $swardOut.StartsWith($swardRepo + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
  throw 'Generated sward assets must be outside the repository.'
}
$swardExe = Join-Path $swardRoot "build/$Preset/bin/engine-content.exe"
if (-not (Test-Path -LiteralPath $swardExe)) { throw "Build $Preset first." }
New-Item -ItemType Directory -Path $swardOut -Force | Out-Null
$swardFamilies = @()
$swardSource = Get-Content -LiteralPath $Recipe -Raw | ConvertFrom-Json -AsHashtable
if ($swardSource.species -ne 'sward') { throw 'Recipe must name the sward solver.' }
if ($PSBoundParameters.ContainsKey('Species')) { $swardSource.sward.species = $Species }
$swardDensity = if ($swardSource.sward.ContainsKey('density')) { $swardSource.sward.density } else { 1.0 }
foreach ($density in @(0.25,0.5,0.75,1.0)) {
  $near = @(); $cards = @()
  for ($variant = 0; $variant -lt $Variants; ++$variant) {
    $name = 'd{0}-v{1}' -f [int]($density*100),$variant
    $folder = Join-Path $swardOut $name
    New-Item -ItemType Directory -Path $folder -Force | Out-Null
    $swardPatch = $swardSource | ConvertTo-Json -Depth 10 | ConvertFrom-Json -AsHashtable
    $swardPatch.sward.seed = 8801
    $swardPatch.sward.variant = $variant
    $swardPatch.sward.density = $density * $swardDensity
    $swardPatch | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $folder 'patch.flora.json')
    & $swardExe flora (Join-Path $folder 'patch.flora.json') (Join-Path $folder 'patch.clusters') `
      --no-materials --sward-assets $folder > (Join-Path $folder 'build.json')
    if ($LASTEXITCODE -ne 0) { throw "Sward build failed: $name" }
    $near += "$name/patch.clusters"; $cards += "$name/distance.clusters"
  }
  $swardFamilies += @{density=$density; near=$near; cards=$cards}
}
& $swardExe terrain-synth (Join-Path $swardOut 'ground.json') --kind mountain --size 129 `
  --spacing-mm 1000 --origin-mm '-64000,-64000' --seed 0 > (Join-Path $swardOut 'ground-build.json')
if ($LASTEXITCODE -ne 0) { throw 'Meadow heightfield build failed.' }
# The generic mountain fixture scaled to a gentle 3.6 m rise. The samples and their hash stay
# unchanged; vertical_mm_per_unit is the heightfield format's explicit unit conversion.
$header = Get-Content -LiteralPath (Join-Path $swardOut 'ground.json') -Raw | ConvertFrom-Json
$header.vertical_mm_per_unit = 20
$header | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $swardOut 'ground.json')

Add-Type -AssemblyName System.Drawing
$paint = [Drawing.Bitmap]::new(257,257)
try {
  for ($z=0; $z -lt 257; ++$z) {
    for ($x=0; $x -lt 257; ++$x) {
      $wx = -64 + $x*0.5; $wz = -64 + $z*0.5
      # Authored crown LAI proxy, the POC's documented k=0.6 Beer-Lambert approximation.
      $shade = [Math]::Max(0,1-[Math]::Sqrt(($wx+18)*($wx+18)+($wz+12)*($wz+12))/9)
      $light = [Math]::Exp(-0.6*3*$shade)
      $density = $light * [Math]::Clamp((32-[Math]::Abs($wx))/4,0,1) *
        [Math]::Clamp((32-[Math]::Abs($wz))/4,0,1)
      # Trail and pond are also authoritative mask regions, so the runtime sees their overwrite.
      if ([Math]::Abs($wx-0.2*$wz-6) -lt 1) { $density=0 }
      if (($wx+32)*($wx+32)+($wz-28.8)*($wz-28.8) -lt 3.2*3.2) { $density=0 }
      $value=[int][Math]::Round(255*$density)
      $paint.SetPixel($x,$z,[Drawing.Color]::FromArgb(255,$value,$value,$value))
    }
  }
  $paint.Save((Join-Path $swardOut 'density.png'),[Drawing.Imaging.ImageFormat]::Png)
} finally { $paint.Dispose() }
$scene = Get-Content -LiteralPath (Join-Path $swardRoot 'content/test-scenes/meadow/scene.json') -Raw | ConvertFrom-Json -AsHashtable
$scene.placements[0].params.families=$swardFamilies
Copy-Item -LiteralPath (Join-Path $swardRoot 'content/flora/birch-single.flora.json') -Destination (Join-Path $swardOut 'edge-tree.flora.json')
& $swardExe flora (Join-Path $swardOut 'edge-tree.flora.json') (Join-Path $swardOut 'edge-tree.clusters') `
  --no-materials > (Join-Path $swardOut 'edge-tree-build.json')
if ($LASTEXITCODE -ne 0) { throw 'Meadow edge tree build failed.' }
$scene | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $swardOut 'scene.json')
Copy-Item -LiteralPath (Join-Path $swardRoot 'content/test-scenes/meadow/camera-path.json') -Destination (Join-Path $swardOut 'camera-path.json')
& $swardExe terrain-masks (Join-Path $swardOut 'scene.json') --ddc (Join-Path $swardOut 'ddc') > (Join-Path $swardOut 'masks-build.json')
if ($LASTEXITCODE -ne 0) { throw 'Meadow mask cook failed.' }
@{generator='tools/sward-cook.ps1';species=$swardSource.sward.species;recipe=$swardSource;variants=$Variants;seed=8801;
  density='linear R, authoritative cooked palette weight';shade='authored LAI=3, extinction k=0.6; POC proxy, not irradiance';
  source='content/test-scenes/meadow';runtime_paint=$false} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $swardOut 'cook-provenance.json')
Write-Output (Join-Path $swardOut 'scene.json')

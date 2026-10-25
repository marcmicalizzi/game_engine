#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Cook and optionally capture small grass stands for morphology and LOD review.
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)][string]$Out,
  [string]$Preset = 'msvc-debug',
  [ValidateSet('dense','dense-low','mown','grazed','trodden','clumps','tall')]
  [string[]]$Styles = @('dense','dense-low','mown','grazed','trodden','clumps','tall'),
  [switch]$Capture
)
$ErrorActionPreference = 'Stop'
$swardRoot = Split-Path $PSScriptRoot
$swardOut = [IO.Path]::GetFullPath($Out)
# Reject every checkout, including a sibling worktree, rather than just this script's root.
$ancestor = $swardOut
while ($ancestor) {
  if (Test-Path -LiteralPath (Join-Path $ancestor 'AGENTS.md')) {
    throw 'Generated grass showcases must be outside a checkout.'
  }
  $ancestor = Split-Path $ancestor -Parent
}
$content = Join-Path $swardRoot "build/$Preset/bin/engine-content.exe"
$view = Join-Path $swardRoot "build/$Preset/bin/engine-view.exe"
New-Item -ItemType Directory -Path $swardOut -Force | Out-Null
$rows = @()
foreach ($style in $Styles) {
  $folder = Join-Path $swardOut $style
  New-Item -ItemType Directory -Path $folder -Force | Out-Null
  $recipe = Join-Path $swardRoot "content/test-scenes/meadow/$style.flora.json"
  Copy-Item -LiteralPath $recipe -Destination (Join-Path $folder 'patch.flora.json')
  & $content flora "$folder/patch.flora.json" "$folder/patch.clusters" --no-materials `
    --sward-assets $folder *> "$folder/build.json"
  if ($LASTEXITCODE -ne 0) { throw "Grass cook failed: $style" }
  $instances = @()
  for ($z=-4; $z -lt 4; ++$z) {
    for ($x=-4; $x -lt 4; ++$x) {
      $instances += @{mesh=0;translation=@(($x+0.5),0,($z+0.5))}
    }
  }
  # Near geometry and its authored cards are shown separately with identical cameras. This
  # fixture deliberately has no terrain, atmosphere or postprocessing dependency to hide gaps.
  foreach ($form in @('near','cards')) {
    $mesh = if ($form -eq 'near') {'patch.clusters'} else {'distance.clusters'}
    $scene = @{format='engine.scene.v1';name="sward-$style-$form";
      meshes=@(@{path=$mesh});instances=$instances;
      sky=@{latitude_deg=46.5;day_of_year=200;turbidity=2.5}}
    $scene | ConvertTo-Json -Depth 15 | Set-Content "$folder/$form.scene.json"
    $keys=@(
      @{time=0;position=@(0,0.65,3.7);target=@(0,0.2,0)},
      @{time=1;position=@(0,1.7,3.7);target=@(0,0.15,0)},
      @{time=2;position=@(0,3,10);target=@(0,0.15,0)}
    )
    @{format='engine.camera-path.v1';name='grass morphology';fps=30;znear=0.02;keys=$keys;
      markers=@(@{frame=0;name='close'},@{frame=30;name='walk'},@{frame=59;name='overview'})} |
      ConvertTo-Json -Depth 10 | Set-Content "$folder/camera.json"
    if ($Capture) {
      $command="& `"$view`" --scene `"$folder/$form.scene.json`" --camera-path `"$folder/camera.json`" --ddc `"$swardOut/ddc`" --offscreen --width 1600 --height 900 --frames 60 --marker-captures `"$folder/$form-captures`" --shadows off --time-rate 0 --time-of-day 12 --tunable renderer.flora.wind_strength=0"
      & (Join-Path $PSScriptRoot 'gpu-lock.ps1') run -Purpose "G90 $style $form morphology" `
        -Exec $command *> "$folder/$form-capture.log"
      if ($LASTEXITCODE -ne 0) { throw "Grass capture failed: $style $form" }
    }
  }
  $p = Get-Content "$folder/provenance.json" -Raw | ConvertFrom-Json
  $rows += @{style=$style;clusters=$p.full_clusters;form_clusters=$p.form_clusters;
    coverage=$p.projected_coverage_near_coarse_cards}
}
@{generator='tools/sward-showcase.ps1';preset=$Preset;capture=[bool]$Capture;
  wind=0;shadows='off';note='Isolated 8x8 m stands; visual candidates, not frame-budget measurements';
  rows=$rows} | ConvertTo-Json -Depth 10 | Set-Content "$swardOut/showcase.json"
Write-Output $swardOut

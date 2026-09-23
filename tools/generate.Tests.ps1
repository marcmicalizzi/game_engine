#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/generate.ps1. Runs under CTest as `tools.generate`.

.DESCRIPTION
  Everything here is offline: no ComfyUI, no Meshy, no GPU, no credits. The ComfyUI half runs the
  UI-to-API conversion and the per-subject overrides against a synthetic workflow and a synthetic
  /object_info written here (`-ObjectInfo`, `-DryRun`), with one of each thing the converter has
  to get right — a seed followed by its UI-only control value, a widget converted to an input that
  still has a stale placeholder, a Reroute, a bypassed node, a PrimitiveNode, a Note and a muted
  node. The provenance half drives the one backend that needs no service, the Tripo folder: it
  writes a manifest for two images, ingests one GLB, and checks the sidecar, `verify`, the cache,
  and a tampered output — and, when a build tree has generated the JSON Schema for
  engine.content.AssetProvenance, that the sidecar's block validates against it, which is what
  pins the sidecar to the schema the content build will read.

      pwsh tools/generate.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'
$gen = Join-Path $PSScriptRoot 'generate.ps1'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pwshPath = [Environment]::ProcessPath
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

# One child process per call, as a user runs it: stdout is JSON lines, stderr is for people.
function Invoke-Gen {
  $errFile = [IO.Path]::GetTempFileName()
  try {
    $out = & $pwshPath -NoProfile -NonInteractive -File $gen @args 2> $errFile
    $code = $LASTEXITCODE
    $err = Get-Content -Raw -LiteralPath $errFile
  } finally { Remove-Item -LiteralPath $errFile -Force -ErrorAction SilentlyContinue }
  # JSON lines; `convert` prints one indented document instead, which the caller joins itself.
  $json = @($out | Where-Object { $_ -is [string] -and $_.TrimStart().StartsWith('{') } | ForEach-Object { try { $_ | ConvertFrom-Json } catch { } })
  return [pscustomobject]@{ Code = $code; Out = @($out); Json = $json; Err = "$err" }
}

$root = Join-Path ([IO.Path]::GetTempPath()) "engine-generate-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $root | Out-Null
$local = Join-Path $root 'local'

try {
  # ---- fixtures -------------------------------------------------------------------------------
  $objectInfo = [ordered]@{
    CheckpointLoaderSimple = @{ input = @{ required = [ordered]@{ ckpt_name = @(, @('model-a.safetensors'), @{}) } }; input_order = @{ required = @('ckpt_name') } }
    CLIPTextEncode = @{ input = @{ required = [ordered]@{ text = @('STRING', @{ multiline = $true }); clip = @('CLIP') } }; input_order = @{ required = @('text', 'clip') } }
    EmptyLatentImage = @{ input = @{ required = [ordered]@{ width = @('INT', @{}); height = @('INT', @{}); batch_size = @('INT', @{}) } }; input_order = @{ required = @('width', 'height', 'batch_size') } }
    KSampler = @{
      input = @{ required = [ordered]@{
          model = @('MODEL'); seed = @('INT', @{ control_after_generate = $true }); steps = @('INT', @{}); cfg = @('FLOAT', @{})
          sampler_name = @(@('euler', 'dpmpp_2m'), @{}); scheduler = @(@('normal', 'simple'), @{})
          positive = @('CONDITIONING'); negative = @('CONDITIONING'); latent_image = @('LATENT'); denoise = @('FLOAT', @{}) } }
      input_order = @{ required = @('model', 'seed', 'steps', 'cfg', 'sampler_name', 'scheduler', 'positive', 'negative', 'latent_image', 'denoise') }
    }
    LatentUpscale = @{ input = @{ required = [ordered]@{ samples = @('LATENT'); upscale_method = @(@('nearest-exact'), @{}); width = @('INT', @{}); height = @('INT', @{}) } }; input_order = @{ required = @('samples', 'upscale_method', 'width', 'height') } }
    VAEDecode = @{ input = @{ required = [ordered]@{ samples = @('LATENT'); vae = @('VAE') } }; input_order = @{ required = @('samples', 'vae') } }
    SaveImage = @{ input = @{ required = [ordered]@{ images = @('IMAGE'); filename_prefix = @('STRING', @{}) } }; input_order = @{ required = @('images', 'filename_prefix') } }
  }
  $infoFile = Join-Path $root 'object_info.json'
  $objectInfo | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $infoFile -Encoding utf8

  # link = [id, from, from_slot, to, to_slot, type]
  $ui = [ordered]@{
    nodes = @(
      @{ id = 1; type = 'CheckpointLoaderSimple'; mode = 0; widgets_values = @('model-a.safetensors'); inputs = @() }
      @{ id = 2; type = 'CLIPTextEncode'; title = 'Positive'; mode = 0; widgets_values = @('old prompt'); inputs = @(@{ name = 'clip'; type = 'CLIP'; link = 1 }) }
      @{ id = 3; type = 'CLIPTextEncode'; title = 'Negative'; mode = 0; widgets_values = @('old negative'); inputs = @(@{ name = 'clip'; type = 'CLIP'; link = 3 }) }
      @{ id = 4; type = 'EmptyLatentImage'; mode = 0; widgets_values = @(512, 768, 1); inputs = @() }
      @{ id = 5; type = 'KSampler'; mode = 0; widgets_values = @(42, 'randomize', 20, 7.5, 'euler', 'normal', 1.0)
        inputs = @(@{ name = 'model'; type = 'MODEL'; link = 4 }, @{ name = 'positive'; type = 'CONDITIONING'; link = 5 }, @{ name = 'negative'; type = 'CONDITIONING'; link = 6 },
          @{ name = 'latent_image'; type = 'LATENT'; link = 8 }, @{ name = 'steps'; type = 'INT'; widget = @{ name = 'steps' }; link = 9 }) }
      @{ id = 6; type = 'VAEDecode'; mode = 0; inputs = @(@{ name = 'samples'; type = 'LATENT'; link = 10 }, @{ name = 'vae'; type = 'VAE'; link = 11 }) }
      @{ id = 7; type = 'SaveImage'; mode = 0; widgets_values = @('%date:yyyy-MM-dd%/x'); inputs = @(@{ name = 'images'; type = 'IMAGE'; link = 12 }) }
      @{ id = 8; type = 'Note'; mode = 0; widgets_values = @('a note for people') }
      @{ id = 9; type = 'Reroute'; mode = 0; inputs = @(@{ name = ''; type = '*'; link = 2 }) }
      @{ id = 10; type = 'PrimitiveNode'; mode = 0; widgets_values = @(30, 'fixed') }
      @{ id = 11; type = 'LatentUpscale'; mode = 4; widgets_values = @('nearest-exact', 1024, 1024); inputs = @(@{ name = 'samples'; type = 'LATENT'; link = 7 }) }
      @{ id = 12; type = 'ImageInvert'; mode = 2; inputs = @() }
    )
    links = @(
      @(1, 1, 1, 2, 0, 'CLIP'), @(2, 1, 1, 9, 0, 'CLIP'), @(3, 9, 0, 3, 0, 'CLIP'), @(4, 1, 0, 5, 0, 'MODEL'),
      @(5, 2, 0, 5, 1, 'CONDITIONING'), @(6, 3, 0, 5, 2, 'CONDITIONING'), @(7, 4, 0, 11, 0, 'LATENT'), @(8, 11, 0, 5, 3, 'LATENT'),
      @(9, 10, 0, 5, 4, 'INT'), @(10, 5, 0, 6, 0, 'LATENT'), @(11, 1, 2, 6, 1, 'VAE'), @(12, 6, 0, 7, 0, 'IMAGE')
    )
  }
  $wfFile = Join-Path $root 'fixture-workflow.json'
  $ui | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $wfFile -Encoding utf8

  $subjects = [ordered]@{ set = 'test-set'
    triangle_budgets = [ordered]@{ hand = [ordered]@{ triangles = 50000 }; large = [ordered]@{ triangles = 250000 } }
    subjects = @(
      [ordered]@{ name = 'alpha-rock'; category = 'rock'; budget = 'large'; size_m = 1.0; subject = 'a grey rock' }
      [ordered]@{ name = 'beta-cup'; category = 'tool'; budget = 'hand'; size_m = 0.1; subject = 'a tin cup' }) }
  $subjectsFile = Join-Path $root 'subjects.json'
  $subjects | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $subjectsFile -Encoding utf8

  # ---- the prompt template ------------------------------------------------------------------------
  Write-Host 'prompt'
  $r = Invoke-Gen prompt -Subject 'a clay jar'
  $p = $r.Json[0]
  Test-That 'the template starts with the subject' { $p.text.StartsWith('a clay jar. ') }
  Test-That 'and asks for one centred object on a plain light background, no text, no people' {
    $p.text -match 'single object' -and $p.text -match 'centered' -and $p.text -match '85%' -and $p.text -match 'light gray' -and $p.text -match 'no text' -and $p.text -match 'no people' }
  Test-That 'with a negative for the workflows that take one' { $p.negative -match 'multiple objects' -and $p.template -eq 'image-to-3d/1' }
  $r = Invoke-Gen prompt -Subject 'exactly this' -Raw
  Test-That '-Raw passes the subject through untouched' { $r.Json[0].text -eq 'exactly this' -and $null -eq $r.Json[0].template }

  # ---- conversion ---------------------------------------------------------------------------------
  Write-Host 'convert'
  $r = Invoke-Gen convert -Workflow $wfFile -ObjectInfo $infoFile
  $api = ($r.Out -join "`n") | ConvertFrom-Json
  Test-That 'exit 0' { $r.Code -eq 0 }
  Test-That 'the Note, the Reroute, the PrimitiveNode, the bypassed and the muted node are gone' {
    (@($api.PSObject.Properties.Name) -join ',') -eq '1,2,3,4,5,6,7' }
  Test-That 'a seed keeps its value and its control widget is skipped' { $api.'5'.inputs.seed -eq 42 }
  Test-That 'a widget turned input takes the PrimitiveNode''s literal' { $api.'5'.inputs.steps -eq 30 }
  Test-That 'and its stale placeholder does not shift the widgets after it' {
    $api.'5'.inputs.cfg -eq 7.5 -and $api.'5'.inputs.sampler_name -eq 'euler' -and $api.'5'.inputs.scheduler -eq 'normal' -and $api.'5'.inputs.denoise -eq 1 }
  Test-That 'a link through a Reroute reaches its source' { $api.'3'.inputs.clip[0] -eq '1' -and $api.'3'.inputs.clip[1] -eq 1 }
  Test-That 'a link through a bypassed node reaches the input of its type' { $api.'5'.inputs.latent_image[0] -eq '4' -and $api.'5'.inputs.latent_image[1] -eq 0 }
  Test-That 'a lone widget value is a value, not its first character' { $api.'1'.inputs.ckpt_name -eq 'model-a.safetensors' -and $api.'7'.inputs.filename_prefix -eq '%date:yyyy-MM-dd%/x' }
  Test-That 'titles survive as _meta' { $api.'2'._meta.title -eq 'Positive' -and $api.'3'._meta.title -eq 'Negative' }

  # ---- the image stage, planned but not run ---------------------------------------------------------
  Write-Host 'image -DryRun'
  $r = Invoke-Gen image -Workflow $wfFile -ObjectInfo $infoFile -Subjects $subjectsFile -LocalRoot $local -Date 2026-01-02 -DryRun
  Test-That 'one plan line per subject' { $r.Code -eq 0 -and $r.Json.Count -eq 2 -and $r.Json[0].name -eq 'alpha-rock' -and $r.Json[0].status -eq 'dry-run:new' }
  $ov = @($r.Json[0].overrides)
  # (not `$input`: that name is PowerShell's automatic pipeline variable)
  function Get-Ov($node, $inputName) { $ov | Where-Object { $_.node -eq $node -and $_.input -eq $inputName } | Select-Object -First 1 }
  Test-That 'the prompt goes to the node titled Positive, templated' { (Get-Ov '2' 'text').value.StartsWith('a grey rock. A single object') -and (Get-Ov '2' 'text').workflow_value -eq 'old prompt' }
  Test-That 'the negative goes to the node titled Negative' { (Get-Ov '3' 'text').value -match 'multiple objects' }
  Test-That 'the seed is derived from the name and recorded against the workflow''s' { (Get-Ov '5' 'seed').value -eq $r.Json[0].seed -and (Get-Ov '5' 'seed').workflow_value -eq 42 }
  Test-That 'a literal latent size defaults to 1024 square' { (Get-Ov '4' 'width').value -eq 1024 -and (Get-Ov '4' 'height').value -eq 1024 }
  Test-That 'the server prefix is the date and the name, never %date%' { (Get-Ov '7' 'filename_prefix').value -eq 'Agentic/2026-01-02/alpha-rock' }
  Test-That 'the two subjects get different seeds and specs' { $r.Json[0].seed -ne $r.Json[1].seed -and $r.Json[0].spec_hash -ne $r.Json[1].spec_hash }
  $r2 = Invoke-Gen image -Workflow $wfFile -ObjectInfo $infoFile -Subjects $subjectsFile -LocalRoot $local -Date 2026-01-03 -DryRun
  Test-That 'the spec does not depend on the date' { $r2.Json[0].spec_hash -eq $r.Json[0].spec_hash }
  $r3 = Invoke-Gen image -Workflow $wfFile -ObjectInfo $infoFile -Subjects $subjectsFile -LocalRoot $local -Only beta-cup -Seed 7 -Width 768 -Height 512 -Set 'KSampler.steps=12' -DryRun
  $ov = @($r3.Json[0].overrides)
  Test-That '-Only, -Seed, -Width/-Height and -Set reach the graph and are recorded' {
    $r3.Json.Count -eq 1 -and (Get-Ov '5' 'seed').value -eq 7 -and (Get-Ov '4' 'width').value -eq 768 -and (Get-Ov '4' 'height').value -eq 512 -and (Get-Ov '5' 'steps').value -eq 12 -and (Get-Ov '5' 'steps').why -eq '-Set' }
  $r4 = Invoke-Gen image -Workflow $wfFile -ObjectInfo $infoFile -Subjects $subjectsFile -LocalRoot $local -Set 'Nothing.text=x' -DryRun
  Test-That '-Set naming no node is refused' { $r4.Code -ne 0 -and $r4.Err -match 'no node titled or classed' }
  $r5 = Invoke-Gen image -Workflow $wfFile -ObjectInfo $infoFile -Subject 'x' -Name 'Bad Name' -LocalRoot $local -DryRun
  Test-That 'a name that is not a portable file name is refused' { $r5.Code -ne 0 -and $r5.Err -match 'lower-case' }
  $r6 = Invoke-Gen image -Workflow $wfFile -ObjectInfo $infoFile -Subjects $subjectsFile -LocalRoot (Join-Path $repo 'generated-here') -DryRun
  Test-That 'a local root inside the repository is refused' { $r6.Code -ne 0 -and $r6.Err -match 'inside the repository' }

  # ---- comfyui-3d: the TRELLIS.2 workflow's shapes, planned but not run --------------------------------
  #
  # A miniature of the owner's text-to-3D workflow with each thing that broke the first converter or
  # that the backend has to find without ids: a V3 dynamic combo whose option brings its own widgets
  # (RemeshMesh sign_mode), a LOAD_3D viewport widget stored as "" (Save3DAdvanced), a COLOR swatch
  # (ImageCropToMask), a texture size wired from a titled PrimitiveInt into the unwrap and the bake,
  # a model boolean switching between two UNET loaders, and an image sampler beside a
  # reconstruction sampler with a fixed seed of its own.
  Write-Host 'comfyui-3d'
  $bool = { param($d) @('BOOLEAN', @{ default = $d }) }
  $info3d = [ordered]@{}
  foreach ($k in @('CheckpointLoaderSimple', 'CLIPTextEncode', 'EmptyLatentImage', 'KSampler', 'VAEDecode')) { $info3d[$k] = $objectInfo[$k] }
  $info3d.ImageCropToMask = @{ input = @{ required = [ordered]@{ images = @('IMAGE'); masks = @('MASK'); width = @('INT', @{ default = 1024 }); height = @('INT', @{ default = 1024 }); pad_factor = @('FLOAT', @{ default = 1.0 }); grow_mask = @('INT', @{ default = 0 }); background = @('COLOR', @{ default = '#000000'; socketless = $true }) } }
    input_order = @{ required = @('images', 'masks', 'width', 'height', 'pad_factor', 'grow_mask', 'background') } }
  $info3d.PreviewImage = @{ input = @{ required = [ordered]@{ images = @('IMAGE') } }; input_order = @{ required = @('images') } }
  $info3d.Trellis2Conditioning = @{ input = @{ required = [ordered]@{ clip_vision_model = @('CLIP_VISION'); image = @('IMAGE') } }; input_order = @{ required = @('clip_vision_model', 'image') } }
  $info3d.UNETLoader = @{ input = @{ required = [ordered]@{ unet_name = @(, @('trellis_2_int8.safetensors', 'pixal3d_int8.safetensors'), @{}); weight_dtype = @(, @('default'), @{}) } }; input_order = @{ required = @('unet_name', 'weight_dtype') } }
  $info3d.PrimitiveBoolean = @{ input = @{ required = [ordered]@{ value = @('BOOLEAN', @{}) } }; input_order = @{ required = @('value') } }
  $info3d.PrimitiveInt = @{ input = @{ required = [ordered]@{ value = @('INT', @{ control_after_generate = 'fixed' }) } }; input_order = @{ required = @('value') } }
  $info3d.ComfySwitchNode = @{ input = @{ required = [ordered]@{ switch = @('BOOLEAN', @{}) }; optional = [ordered]@{ on_false = @('COMFY_MATCHTYPE_V3', @{ lazy = $true }); on_true = @('COMFY_MATCHTYPE_V3', @{ lazy = $true }) } }
    input_order = @{ required = @('switch'); optional = @('on_false', 'on_true') } }
  $info3d.RemeshMesh = @{ input = @{ required = [ordered]@{ mesh = @('MESH'); resolution = @('INT', @{ default = 512 })
        sign_mode = @('COMFY_DYNAMICCOMBO_V3', @{ options = @(
              @{ key = 'udf'; inputs = @{ required = [ordered]@{ qef = (& $bool $false); drop_inverted_components = (& $bool $false); drop_enclosed_components = (& $bool $false) } } },
              @{ key = 'sdf'; inputs = @{ required = [ordered]@{ qef = (& $bool $true); manifold = (& $bool $false) } } }) })
        band = @('FLOAT', @{ default = 1.0 }); project_back = @('FLOAT', @{ default = 0.0 }); fix_poles = (& $bool $false); smooth_iters = @('INT', @{ default = 0 })
        drop_small_components = @('FLOAT', @{ default = 0.01 }); precluster_max_verts = @('INT', @{ default = 20000000 }) } }
    input_order = @{ required = @('mesh', 'resolution', 'sign_mode', 'band', 'project_back', 'fix_poles', 'smooth_iters', 'drop_small_components', 'precluster_max_verts'); hidden = @('unique_id') } }
  $info3d.DecimateMesh = @{ input = @{ required = [ordered]@{ mesh = @('MESH'); target_face_count = @('INT', @{ default = 200000 })
        placement_mode = @('COMFY_DYNAMICCOMBO_V3', @{ options = @(@{ key = 'midpoint'; inputs = @{ required = [ordered]@{} } }, @{ key = 'qem'; inputs = @{ required = [ordered]@{ line_quadric_weight = @('FLOAT', @{ default = 0.0 }) } } }) }) } }
    input_order = @{ required = @('mesh', 'target_face_count', 'placement_mode') } }
  $info3d.MeshSmoothNormals = @{ input = @{ required = [ordered]@{ mesh = @('MESH'); crease_angle = @('FLOAT', @{ default = 180.0 }) } }; input_order = @{ required = @('mesh', 'crease_angle') } }
  $info3d.UnwrapMesh = @{ input = @{ required = [ordered]@{ mesh = @('MESH'); segmenter = @('COMBO', @{ default = 'pec'; options = @('pec', 'adaptive') }); resolution = @('INT', @{ default = 1024 }); padding = @('INT', @{ default = 1 }); weld_distance = @('FLOAT', @{ default = 0.0 }) } }
    input_order = @{ required = @('mesh', 'segmenter', 'resolution', 'padding', 'weld_distance') } }
  $info3d.BakeTextureFromVoxel = @{ input = @{ required = [ordered]@{ mesh = @('MESH'); voxel_colors = @('VOXEL'); texture_size = @('INT', @{ default = 2048 }) } }; input_order = @{ required = @('mesh', 'voxel_colors', 'texture_size') } }
  $info3d.MeshToFile3D = @{ input = @{ required = [ordered]@{ mesh = @('MESH') } }; input_order = @{ required = @('mesh') } }
  $info3d.Save3DAdvanced = @{ input = @{ required = [ordered]@{ model_3d = @('FILE_3D'); filename_prefix = @('STRING', @{ default = '3d/ComfyUI' }); viewport_state = @('LOAD_3D', @{}); width = @('INT', @{ default = 1024 }); height = @('INT', @{ default = 1024 }) }
      optional = [ordered]@{ model_3d_info = @('LOAD3D_MODEL_INFO'); camera_info = @('LOAD3D_CAMERA') } }
    input_order = @{ required = @('model_3d', 'filename_prefix', 'viewport_state', 'width', 'height'); optional = @('model_3d_info', 'camera_info') } }
  $info3d.RenderUVAtlas = @{ input = @{ required = [ordered]@{ mesh = @('MESH'); resolution = @('INT', @{ default = 1024 }) } }; input_order = @{ required = @('mesh', 'resolution') } }
  $info3d.LoadBackgroundRemovalModel = @{ input = @{ required = [ordered]@{ bg_removal_name = @('COMBO', @{ options = @('birefnet.safetensors') }) } }; input_order = @{ required = @('bg_removal_name') } }
  $info3d.RemoveBackground = @{ input = @{ required = [ordered]@{ bg_removal_model = @('BACKGROUND_REMOVAL'); image = @('IMAGE') } }; input_order = @{ required = @('bg_removal_model', 'image') } }
  $info3dFile = Join-Path $root 'object_info_3d.json'
  $info3d | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $info3dFile -Encoding utf8

  $sock = { param($name, $type, $link) @{ name = $name; type = $type; link = $link } }
  $wsock = { param($name, $type, $link) @{ name = $name; type = $type; link = $link; widget = @{ name = $name } } }
  $ui3d = [ordered]@{
    nodes = @(
      @{ id = 1; type = 'CheckpointLoaderSimple'; mode = 0; widgets_values = @('model-a.safetensors'); inputs = @() }
      @{ id = 20; type = 'CLIPTextEncode'; mode = 0; widgets_values = @('a palm tree'); inputs = @((& $sock 'clip' 'CLIP' 201)) }
      @{ id = 21; type = 'CLIPTextEncode'; mode = 0; widgets_values = @(''); inputs = @((& $sock 'clip' 'CLIP' 202)) }
      @{ id = 22; type = 'EmptyLatentImage'; mode = 0; widgets_values = @(1024, 1024, 1); inputs = @() }
      @{ id = 23; type = 'KSampler'; mode = 0; widgets_values = @(304167832764365, 'randomize', 8, 1, 'euler', 'simple', 1)
        inputs = @((& $sock 'model' 'MODEL' 203), (& $sock 'positive' 'CONDITIONING' 204), (& $sock 'negative' 'CONDITIONING' 205), (& $sock 'latent_image' 'LATENT' 206)) }
      @{ id = 24; type = 'VAEDecode'; mode = 0; inputs = @((& $sock 'samples' 'LATENT' 207), (& $sock 'vae' 'VAE' 208)) }
      @{ id = 25; type = 'ImageCropToMask'; mode = 0; widgets_values = @(1024, 1024, 1.1, 0, '#000000'); inputs = @((& $sock 'images' 'IMAGE' 209), (& $sock 'masks' 'MASK' 233)) }
      @{ id = 26; type = 'RemoveBackground'; mode = 0; inputs = @((& $sock 'bg_removal_model' 'BACKGROUND_REMOVAL' 231), (& $sock 'image' 'IMAGE' 232)) }
      @{ id = 29; type = 'LoadBackgroundRemovalModel'; mode = 0; widgets_values = @('birefnet.safetensors'); inputs = @() }
      @{ id = 27; type = 'PreviewImage'; mode = 0; inputs = @((& $sock 'images' 'IMAGE' 212)) }
      @{ id = 28; type = 'Trellis2Conditioning'; mode = 0; inputs = @((& $sock 'clip_vision_model' 'CLIP_VISION' $null), (& $sock 'image' 'IMAGE' 213)) }
      @{ id = 30; type = 'UNETLoader'; mode = 0; widgets_values = @('trellis_2_int8.safetensors', 'default'); inputs = @() }
      @{ id = 31; type = 'UNETLoader'; mode = 0; widgets_values = @('pixal3d_int8.safetensors', 'default'); inputs = @() }
      @{ id = 32; type = 'PrimitiveBoolean'; title = 'Boolean (Switch to Trellis2)'; mode = 0; widgets_values = @($true); inputs = @() }
      @{ id = 33; type = 'ComfySwitchNode'; mode = 0; widgets_values = @($false); inputs = @((& $sock 'on_false' '*' 214), (& $sock 'on_true' '*' 215), (& $wsock 'switch' 'BOOLEAN' 216)) }
      @{ id = 34; type = 'KSampler'; mode = 0; widgets_values = @(42, 'fixed', 12, 7.5, 'euler', 'normal', 1)
        inputs = @((& $sock 'model' 'MODEL' 217), (& $sock 'positive' 'CONDITIONING' 218), (& $sock 'negative' 'CONDITIONING' 219), (& $sock 'latent_image' 'LATENT' 220)) }
      @{ id = 41; type = 'RemeshMesh'; mode = 0; widgets_values = @(768, 'udf', $false, $false, $false, 1, 0, $false, 20, 0.01, 20000000); inputs = @((& $sock 'mesh' 'MESH' $null)) }
      @{ id = 42; type = 'DecimateMesh'; mode = 0; widgets_values = @(700000, 'midpoint'); inputs = @((& $sock 'mesh' 'MESH' 221)) }
      @{ id = 43; type = 'MeshSmoothNormals'; mode = 0; widgets_values = @(180); inputs = @((& $sock 'mesh' 'MESH' 222)) }
      @{ id = 44; type = 'PrimitiveInt'; title = 'Texture Resolution'; mode = 0; widgets_values = @(4096, 'fixed'); inputs = @() }
      @{ id = 45; type = 'UnwrapMesh'; mode = 0; widgets_values = @('pec', 2048, 1, 0.0002); inputs = @((& $sock 'mesh' 'MESH' 223), (& $wsock 'resolution' 'INT' 224)) }
      @{ id = 46; type = 'BakeTextureFromVoxel'; mode = 0; widgets_values = @(2048); inputs = @((& $sock 'mesh' 'MESH' 225), (& $sock 'voxel_colors' 'VOXEL' $null), (& $wsock 'texture_size' 'INT' 226)) }
      @{ id = 47; type = 'MeshToFile3D'; mode = 0; inputs = @((& $sock 'mesh' 'MESH' 227)) }
      @{ id = 48; type = 'Save3DAdvanced'; mode = 0; widgets_values = @('3d/ComfyUI', '', 1024, 1024)
        inputs = @((& $sock 'model_3d' 'FILE_3D' 228), (& $sock 'model_3d_info' 'LOAD3D_MODEL_INFO' $null), (& $sock 'camera_info' 'LOAD3D_CAMERA' $null)) }
      @{ id = 49; type = 'RenderUVAtlas'; mode = 0; widgets_values = @(1024); inputs = @((& $sock 'mesh' 'MESH' 229)) }
      @{ id = 50; type = 'PreviewImage'; mode = 0; inputs = @((& $sock 'images' 'IMAGE' 230)) }
    )
    links = @(
      @(201, 1, 1, 20, 0, 'CLIP'), @(202, 1, 1, 21, 0, 'CLIP'), @(203, 1, 0, 23, 0, 'MODEL'), @(204, 20, 0, 23, 1, 'CONDITIONING'), @(205, 21, 0, 23, 2, 'CONDITIONING'),
      @(206, 22, 0, 23, 3, 'LATENT'), @(207, 23, 0, 24, 0, 'LATENT'), @(208, 1, 2, 24, 1, 'VAE'), @(209, 24, 0, 25, 0, 'IMAGE'), @(212, 25, 0, 27, 0, 'IMAGE'),
      @(213, 27, 0, 28, 1, 'IMAGE'), @(214, 31, 0, 33, 0, 'MODEL'), @(215, 30, 0, 33, 1, 'MODEL'), @(216, 32, 0, 33, 2, 'BOOLEAN'), @(217, 33, 0, 34, 0, 'MODEL'),
      @(218, 28, 0, 34, 1, 'CONDITIONING'), @(219, 28, 1, 34, 2, 'CONDITIONING'), @(220, 22, 0, 34, 3, 'LATENT'), @(221, 41, 0, 42, 0, 'MESH'), @(222, 42, 0, 43, 0, 'MESH'),
      @(223, 43, 0, 45, 0, 'MESH'), @(224, 44, 0, 45, 2, 'INT'), @(225, 45, 0, 46, 0, 'MESH'), @(226, 44, 0, 46, 2, 'INT'), @(227, 45, 0, 47, 0, 'MESH'),
      @(228, 47, 0, 48, 0, 'FILE_3D'), @(229, 45, 0, 49, 0, 'MESH'), @(230, 49, 0, 50, 0, 'IMAGE'),
      @(231, 29, 0, 26, 0, 'BACKGROUND_REMOVAL'), @(232, 24, 0, 26, 1, 'IMAGE'), @(233, 26, 0, 25, 1, 'MASK')
    )
  }
  $wf3d = Join-Path $root 'text-to-3d.json'
  $ui3d | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $wf3d -Encoding utf8

  $r = Invoke-Gen convert -Workflow $wf3d -ObjectInfo $info3dFile
  $api3 = ($r.Out -join "`n") | ConvertFrom-Json
  Test-That 'a dynamic combo''s option widgets are named <combo>.<input>, and the widgets after them stay aligned' {
    $m = $api3.'41'.inputs
    $m.sign_mode -eq 'udf' -and $m.'sign_mode.qef' -eq $false -and $m.'sign_mode.drop_enclosed_components' -eq $false -and $m.band -eq 1 -and $m.fix_poles -eq $false -and $m.smooth_iters -eq 20 -and $m.precluster_max_verts -eq 20000000 }
  Test-That 'an option with no widgets of its own takes no slot' { $api3.'42'.inputs.target_face_count -eq 700000 -and $api3.'42'.inputs.placement_mode -eq 'midpoint' }
  Test-That 'a LOAD_3D viewport stored as "" is a widget, so width and height after it are read right' {
    $api3.'48'.inputs.filename_prefix -eq '3d/ComfyUI' -and $api3.'48'.inputs.viewport_state -eq '' -and $api3.'48'.inputs.width -eq 1024 -and $api3.'48'.inputs.height -eq 1024 -and -not $api3.'48'.inputs.PSObject.Properties['model_3d_info'] }
  Test-That 'a COLOR swatch is a widget too' { $api3.'25'.inputs.pad_factor -eq 1.1 -and $api3.'25'.inputs.background -eq '#000000' }
  Test-That 'a widget linked from a primitive keeps its link and skips its placeholder' { $api3.'45'.inputs.resolution[0] -eq '44' -and $api3.'45'.inputs.padding -eq 1 -and $api3.'45'.inputs.weld_distance -eq 0.0002 }

  $r = Invoke-Gen 3d -Backend comfyui-3d -Workflow $wf3d -ObjectInfo $info3dFile -Subject 'a weathered wooden crate' -Name crate -LocalRoot $local -Date 2026-01-02 -DryRun
  $p3 = $r.Json[0]
  $ov = @($p3.overrides)
  Test-That 'comfyui-3d plans one run, into generated/trellis' { $r.Code -eq 0 -and $p3.status -eq 'dry-run:new' -and $p3.service -eq 'trellis' }
  Test-That 'every role is found by what it is wired to, not by id' {
    $p3.roles.image_sampler -eq '23' -and $p3.roles.positive -eq '20' -and $p3.roles.negative -eq '21' -and $p3.roles.latent -eq '22' -and $p3.roles.remesh -eq '41' -and $p3.roles.decimate -eq '42' -and
    $p3.roles.unwrap -eq '45' -and $p3.roles.save -eq '48' -and $p3.roles.texture_resolution -eq '44' -and $p3.roles.model_switch -eq '32' -and $p3.roles.input_preview -eq '27' -and $p3.roles.uv_preview -eq '50' -and
    $p3.roles.bake_texture -eq '46' -and $null -eq $p3.roles.bake_normal -and $p3.settings.bake_texture.texture_size -eq '<- node 44' }
  Test-That 'the seed goes to the image sampler only; the reconstruction sampler keeps its own' {
    (Get-Ov '23' 'seed').value -eq $p3.seed -and -not (Get-Ov '34' 'seed') -and $p3.settings.reconstruction_samplers.'34'.seed -eq 42 }
  Test-That 'the prop defaults: 100,000 faces, texture 2048 on the primitive, pec, smoothing 3' {
    (Get-Ov '42' 'target_face_count').value -eq 100000 -and (Get-Ov '44' 'value').value -eq 2048 -and (Get-Ov '45' 'segmenter').value -eq 'pec' -and (Get-Ov '41' 'smooth_iters').value -eq 3 -and
    $p3.settings.unwrap_effective_resolution -eq 2048 -and (Get-Ov '41' 'smooth_iters').workflow_value -eq 20 }
  Test-That 'the prompt, the TRELLIS model and the output prefix, each recorded against the workflow''s value' {
    (Get-Ov '20' 'text').value.StartsWith('a weathered wooden crate. A single object') -and (Get-Ov '20' 'text').workflow_value -eq 'a palm tree' -and
    (Get-Ov '32' 'value').value -eq $true -and $p3.settings.model.file -eq 'trellis_2_int8.safetensors' -and (Get-Ov '48' 'filename_prefix').value -eq '3d/Agentic/2026-01-02/crate' }
  $r = Invoke-Gen 3d -Backend comfyui-3d -Workflow $wf3d -ObjectInfo $info3dFile -Subject 'a tall palm tree' -Name palm -LocalRoot $local -DryRun
  Test-That 'foliage gets pec, and says why' { $r.Json[0].segmenter -eq 'pec' -and $r.Json[0].settings.segmenter.reason -match 'foliage' }
  $r = Invoke-Gen 3d -Backend comfyui-3d -Workflow $wf3d -ObjectInfo $info3dFile -Subject 'a small dead tree with bare leafless branches' -Name deadtree -Segmenter adaptive -LocalRoot $local -DryRun
  Test-That 'adaptive can still be asked for on what is not foliage ("leafless" is not "leaf")' { $r.Code -eq 0 -and $r.Json[0].segmenter -eq 'adaptive' -and -not $r.Json[0].foliage }
  $r = Invoke-Gen 3d -Backend comfyui-3d -Workflow $wf3d -ObjectInfo $info3dFile -Subject 'a tall palm tree' -Name palm -Segmenter adaptive -LocalRoot $local -DryRun
  Test-That 'and adaptive on foliage is refused, not run for hours' { $r.Code -ne 0 -and $r.Err -match 'never run on foliage' }
  $r = Invoke-Gen 3d -Backend comfyui-3d -Workflow $wf3d -ObjectInfo $info3dFile -Subject 'a weathered wooden crate' -Name crate -LocalRoot $local -Date 2026-01-02 -DryRun `
    -Model pixal3d -RemeshSignMode sdf -UnwrapResolution 1024 -FaceCount 700000 -Segmenter pec -RemeshSmoothIters 0 -WeldDistance 0.001 -Set '#34.steps=4'
  $q = $r.Json[0]; $ov = @($q.overrides)
  Test-That '-Model pixal3d flips the switch to the loader that names it' { (Get-Ov '32' 'value').value -eq $false -and $q.settings.model.file -eq 'pixal3d_int8.safetensors' }
  Test-That '-RemeshSignMode sdf swaps the option''s inputs for the new option''s defaults' {
    $m = $q.settings.remesh; $m.sign_mode -eq 'sdf' -and $m.'sign_mode.qef' -eq $true -and $m.'sign_mode.manifold' -eq $false -and -not $m.PSObject.Properties['sign_mode.drop_inverted_components'] }
  Test-That '-UnwrapResolution overrides the wired texture size, and the link is recorded as what it replaced' {
    $q.settings.unwrap.resolution -eq 1024 -and (Get-Ov '45' 'resolution').workflow_value[0] -eq '44' -and $q.settings.texture_resolution -eq 2048 }
  Test-That 'face count, segmenter, smoothing and weld reach the graph' {
    $q.settings.decimate.target_face_count -eq 700000 -and $q.settings.unwrap.segmenter -eq 'pec' -and $q.settings.remesh.smooth_iters -eq 0 -and $q.settings.unwrap.weld_distance -eq 0.001 }
  Test-That '-Set #<id> reaches that node and no other of its class' { $q.settings.reconstruction_samplers.'34'.steps -eq 4 -and $q.settings.image.sampler.steps -eq 8 }
  Test-That 'two different post-processes are two different specs' { $q.spec_hash -ne $p3.spec_hash }
  $r = Invoke-Gen 3d -Backend comfyui-3d -Workflow $wf3d -ObjectInfo $info3dFile -Subject 'a weathered wooden crate' -Name crate -LocalRoot $local -Date 2026-02-03 -DryRun
  Test-That 'and the spec does not depend on the date in the prefix' { $r.Json[0].spec_hash -eq $p3.spec_hash }

  # Image in: the same workflow handed a picture instead of drawing one (E10 fed every service the
  # same images). One image carries a sidecar from the image stage, the other none.
  Write-Host 'comfyui-3d, image in'
  $images3d = Join-Path $root 'images3d'
  New-Item -ItemType Directory -Force -Path $images3d | Out-Null
  $png1 = [Convert]::FromBase64String('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==')
  [IO.File]::WriteAllBytes((Join-Path $images3d 'alpha-rock.png'), $png1)
  [IO.File]::WriteAllBytes((Join-Path $images3d 'beta-cup.png'), $png1 + [byte[]](0))
  $rockSha = (Get-FileHash -Algorithm SHA256 (Join-Path $images3d 'alpha-rock.png')).Hash.ToLowerInvariant()
  [ordered]@{ schema = 'engine.generation.provenance/1'; asset_id = ('ab' * 16); name = 'alpha-rock'; kind = 'image'; seed = 7
    prompt = [ordered]@{ template = 'image-to-3d/1'; subject = 'a grey rock'; text = 'a grey rock. A single object'; negative = $null }
    subject = [ordered]@{ set = 'test-set'; category = 'rock'; size_m = 1.0 } } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $images3d 'alpha-rock.provenance.json') -Encoding utf8
  $r = Invoke-Gen 3d -Backend comfyui-3d -Workflow $wf3d -ObjectInfo $info3dFile -Images $images3d -Subjects $subjectsFile -Budget -LocalRoot $local -Date 2026-01-02 -DryRun
  $rock = $r.Json | Where-Object name -eq 'alpha-rock'
  $cup = $r.Json | Where-Object name -eq 'beta-cup'
  $ov = @($rock.overrides)
  $gr = $rock.graph
  Test-That 'one plan per image, into generated/trellis' { $r.Code -eq 0 -and @($r.Json).Count -eq 2 -and $rock.status -eq 'dry-run:new' -and $rock.service -eq 'trellis' }
  Test-That 'a LoadImage replaces the drawn picture on every input it fed: the background removal and the crop' {
    $gr.'51'.class_type -eq 'LoadImage' -and $gr.'26'.inputs.image[0] -eq '51' -and $gr.'25'.inputs.images[0] -eq '51' -and $gr.'26'.inputs.bg_removal_model[0] -eq '29' }
  Test-That 'the drawing nodes are dropped: sampler, encoders, decode, and the loader that fed only them' {
    -not $gr.PSObject.Properties['23'] -and -not $gr.PSObject.Properties['24'] -and -not $gr.PSObject.Properties['20'] -and -not $gr.PSObject.Properties['21'] -and -not $gr.PSObject.Properties['1'] -and
    @($rock.settings.image.removed_nodes).Count -eq 5 -and $rock.settings.image.replaced_node -eq '24' }
  Test-That 'a node something else still reads stays (the latent the reconstruction sampler shares)' { $gr.PSObject.Properties['22'] -and $gr.'34'.inputs.latent_image[0] -eq '22' }
  Test-That 'no prompt, seed or size is set, and the reconstruction sampler keeps its own seed' {
    -not ($ov | Where-Object { $_.node -in '20', '21', '22', '23' }) -and $gr.'34'.inputs.seed -eq 42 -and $null -eq $rock.roles.image_sampler -and $rock.roles.image_source -eq '51' }
  Test-That 'the image is recorded as supplied, with its hash, its asset id and its sidecar' {
    $rock.settings.image.supplied -eq $true -and $rock.settings.image.sha256 -eq $rockSha -and $rock.settings.image.asset_id -eq ('ab' * 16) -and $rock.settings.image.provenance -match 'alpha-rock\.provenance\.json$' -and
    $null -eq $cup.settings.image.asset_id }
  Test-That 'the prompt is the picture''s, copied from its sidecar and marked so; none without one' {
    $rock.prompt.from_input -eq $true -and $rock.prompt.subject -eq 'a grey rock' -and $rock.prompt.image_seed -eq 7 -and $null -eq $cup.prompt }
  Test-That '-Budget decimates each subject to its class''s budget, and records where the number came from' {
    $rock.settings.decimate.target_face_count -eq 250000 -and $cup.settings.decimate.target_face_count -eq 50000 -and $rock.settings.face_budget.class -eq 'large' -and
    ((@($ov) | Where-Object { $_.node -eq '42' -and $_.input -eq 'target_face_count' }).why -match '^face count: budget:large \(subjects\.json\)$') }
  Test-That 'the spec follows the picture''s bytes' { $rock.spec_hash -ne $cup.spec_hash -and $rock.spec_hash -ne $p3.spec_hash }
  $images3dCopy = Join-Path $root 'images3d-copy'
  New-Item -ItemType Directory -Force -Path $images3dCopy | Out-Null
  Copy-Item -LiteralPath (Join-Path $images3d 'alpha-rock.png') -Destination $images3dCopy
  $r2 = Invoke-Gen 3d -Backend comfyui-3d -Workflow $wf3d -ObjectInfo $info3dFile -Images $images3dCopy -Subjects $subjectsFile -Budget -LocalRoot $local -Date 2026-03-04 -DryRun
  Test-That 'and not the folder it came from, nor the date' { $r2.Code -eq 0 -and $r2.Json[0].spec_hash -eq $rock.spec_hash }
  $r2 = Invoke-Gen 3d -Backend comfyui-3d -Workflow $wf3d -ObjectInfo $info3dFile -Images $images3d -Budget -LocalRoot $local -DryRun
  Test-That '-Budget without a -Subjects list is refused' { $r2.Code -ne 0 -and $r2.Err -match '-Budget takes each subject' }
  $r2 = Invoke-Gen 3d -Backend comfyui-3d -Workflow $wf3d -ObjectInfo $info3dFile -Images $images3d -Subjects $subjectsFile -Budget -FaceCount 70000 -LocalRoot $local -DryRun
  Test-That '-Budget with -FaceCount is refused rather than one silently winning' { $r2.Code -ne 0 -and $r2.Err -match 'both name the face count' }
  $r2 = Invoke-Gen 3d -Backend comfyui-3d -Workflow $wf3d -ObjectInfo $info3dFile -Images $images3d -Only beta-cup -LocalRoot $local -DryRun
  Test-That 'without -Budget, -FaceCount''s default as before' { $r2.Code -eq 0 -and $r2.Json[0].settings.decimate.target_face_count -eq 100000 -and $null -eq $r2.Json[0].settings.face_budget }

  # ---- provenance, through the Tripo folder --------------------------------------------------------
  Write-Host 'manifest / ingest / verify'
  $images = Join-Path $root 'images'
  New-Item -ItemType Directory -Force -Path $images | Out-Null
  $png = [Convert]::FromBase64String('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==')
  [IO.File]::WriteAllBytes((Join-Path $images 'alpha-rock.png'), $png)
  [IO.File]::WriteAllBytes((Join-Path $images 'beta-cup.png'), $png + [byte[]](0))   # a different hash
  $r = Invoke-Gen manifest -Backend tripo-folder -Images $images -LocalRoot $local -Date 2026-01-02
  $tripo = Join-Path (Join-Path $local 'generated') 'tripo'
  $manifestFile = Join-Path $tripo 'manifest-2026-01-02.json'
  Test-That 'the manifest and the README are written' { $r.Code -eq 0 -and (Test-Path $manifestFile) -and (Test-Path (Join-Path $tripo 'README.md')) }
  $m = Get-Content -Raw $manifestFile | ConvertFrom-Json
  Test-That 'one entry per image, with its hash and the file name to save as' {
    @($m.entries).Count -eq 2 -and $m.entries[0].save_as -eq 'alpha-rock.glb' -and $m.entries[0].image_sha256 -eq (Get-FileHash -Algorithm SHA256 (Join-Path $images 'alpha-rock.png')).Hash.ToLowerInvariant() }
  Test-That 'the README names every image and where to drop the GLBs' { $t = Get-Content -Raw (Join-Path $tripo 'README.md'); $t -match 'alpha-rock\.glb' -and $t -match 'beta-cup\.png' -and $t.Contains($m.inbox) }
  $r = Invoke-Gen manifest -Backend tripo-folder -Images $images -LocalRoot $local -Date 2026-01-02
  Test-That 'a manifest is not overwritten without -Force (the owner may have written in it)' { $r.Code -ne 0 -and $r.Err -match 'exists' }

  # ---- the Meshy stage, planned offline (no key, no balance, no credits) ----------------------------
  Write-Host 'meshy -DryRun -Offline'
  $r = Invoke-Gen 3d -Backend meshy -Images $images -LocalRoot $local -Date 2026-01-02 -DryRun -Offline
  $a = $r.Json | Where-Object name -eq 'alpha-rock'
  Test-That 'without -Remesh the request is the first pass''s body, field for field' {
    $r.Code -eq 0 -and $r.Json.Count -eq 2 -and (($a.request | ConvertTo-Json -Compress) -eq '{"ai_model":"latest","should_texture":true,"enable_pbr":true,"topology":"triangle"}') -and $null -eq $a.remesh }
  # The first pass's spec was sha256(image sha256 \n sha256(body JSON)); a batch without -Remesh must
  # still name those outputs, or rerunning it would refuse them as "a different spec".
  $sha = { param($s) ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($s)))).ToLowerInvariant() }
  $alphaSha = (Get-FileHash -Algorithm SHA256 (Join-Path $images 'alpha-rock.png')).Hash.ToLowerInvariant()
  $oldSpec = & $sha ("$alphaSha`n" + (& $sha '{"ai_model":"latest","should_texture":true,"enable_pbr":true,"topology":"triangle"}'))
  Test-That 'and its spec hash is the one the first pass recorded' { $a.spec_hash -eq $oldSpec }
  $r = Invoke-Gen 3d -Backend meshy -Images $images -Subjects $subjectsFile -Remesh -LocalRoot $local -Date 2026-01-02 -DryRun -Offline
  $a = $r.Json | Where-Object name -eq 'alpha-rock'; $b = $r.Json | Where-Object name -eq 'beta-cup'
  Test-That '-Remesh -Subjects takes each image''s polycount from its subject''s budget class' {
    $r.Code -eq 0 -and $a.request.should_remesh -eq $true -and $a.request.target_polycount -eq 250000 -and $b.request.target_polycount -eq 50000 -and $a.request.topology -eq 'triangle' }
  Test-That 'and records where the number came from' { $a.remesh.budget_class -eq 'large' -and $a.remesh.budget_triangles -eq 250000 -and $a.remesh.polycount_from -match '^budget:large' }
  Test-That 'a remeshed request is a different spec from the unremeshed one' { $a.spec_hash -ne $oldSpec }
  $r = Invoke-Gen 3d -Backend meshy -Images $images -Subjects $subjectsFile -Remesh -Topology quad -LocalRoot $local -DryRun -Offline
  Test-That 'a quad remesh asks for half the budget, because its faces are quads' {
    ($r.Json | Where-Object name -eq 'alpha-rock').request.target_polycount -eq 125000 -and ($r.Json | Where-Object name -eq 'alpha-rock').request.topology -eq 'quad' }
  $r = Invoke-Gen 3d -Backend meshy -Images $images -Subjects $subjectsFile -Remesh -TargetPolycount 20000 -LocalRoot $local -DryRun -Offline
  Test-That '-TargetPolycount overrides the budgets for every image' { @($r.Json | Where-Object { $_.request.target_polycount -eq 20000 -and $_.remesh.polycount_from -eq '-TargetPolycount' }).Count -eq 2 }
  $r = Invoke-Gen 3d -Backend meshy -Images $images -TargetPolycount 20000 -LocalRoot $local -DryRun -Offline
  Test-That '-TargetPolycount without -Remesh is refused (the service would ignore it)' { $r.Code -ne 0 -and $r.Err -match 'without -Remesh' }
  $r = Invoke-Gen 3d -Backend meshy -Images $images -Topology quad -LocalRoot $local -DryRun -Offline
  Test-That 'and so is -Topology quad' { $r.Code -ne 0 -and $r.Err -match 'without -Remesh' }
  $r = Invoke-Gen 3d -Backend meshy -Images $images -Remesh -TargetPolycount 50 -LocalRoot $local -DryRun -Offline
  Test-That 'a polycount outside Meshy''s range is refused' { $r.Code -ne 0 -and $r.Err -match 'outside Meshy' }
  $r = Invoke-Gen 3d -Backend meshy -Images $images -Remesh -LocalRoot $local -DryRun -Offline
  Test-That '-Remesh with no polycount and no budget is refused rather than left to the service default' { $r.Code -ne 0 -and $r.Err -match 'needs a polycount' }
  $r = Invoke-Gen 3d -Backend meshy -Images $images -LocalRoot $local -Offline -MaxCredits 0
  Test-That '-Offline is refused outside a dry run' { $r.Code -ne 0 -and $r.Err -match 'DryRun only' }

  $glbBytes = [Text.Encoding]::ASCII.GetBytes('glTF') + [byte[]](2, 0, 0, 0, 20, 0, 0, 0) + [byte[]](0) * 8
  [IO.File]::WriteAllBytes((Join-Path $m.inbox 'alpha-rock.glb'), $glbBytes)
  $r = Invoke-Gen ingest -Backend tripo-folder -Manifest $manifestFile -LocalRoot $local -Date 2026-01-05
  Test-That 'ingest takes the GLB that is there and reports the one that is not' {
    $r.Code -eq 0 -and ($r.Json | Where-Object name -eq 'alpha-rock').status -eq 'ingested' -and ($r.Json | Where-Object name -eq 'beta-cup').status -eq 'missing' }
  $sideFile = Join-Path (Join-Path $tripo '2026-01-05') 'alpha-rock.provenance.json'
  $side = Get-Content -Raw $sideFile | ConvertFrom-Json
  $imgSha = (Get-FileHash -Algorithm SHA256 (Join-Path $images 'alpha-rock.png')).Hash.ToLowerInvariant()
  Test-That 'the sidecar says what, who, from what, and under which licence' {
    $side.schema -eq 'engine.generation.provenance/1' -and $side.service -eq 'tripo' -and $side.kind -eq 'mesh' -and $side.license -eq 'tripo-studio-max' -and
    $side.content_class -eq 'general' -and $side.content_classes.nudity -eq 'none' -and $side.inputs[0].sha256 -eq $imgSha }
  Test-That 'its asset_provenance names the image by content id, as inputs and as derived_from' {
    $side.asset_provenance.inputs[0] -eq $imgSha.Substring(0, 32) -and $side.asset_provenance.derived_from[0] -eq $imgSha.Substring(0, 32) -and
    $side.asset_provenance.license_class -eq 'Proprietary' -and $side.asset_provenance.operator_id -match '^[0-9a-f]{32}$' }
  Test-That 'its asset id is the GLB''s content id' { $side.asset_id -eq ((Get-FileHash -Algorithm SHA256 (Join-Path (Join-Path $tripo '2026-01-05') 'alpha-rock.glb')).Hash.ToLowerInvariant().Substring(0, 32)) }
  Test-That 'asset_provenance has exactly the fields of engine.content.AssetProvenance' {
    (@($side.asset_provenance.PSObject.Properties.Name) -join ',') -eq 'generator,model_id,model_version,prompt_hash,seed,inputs,operator_id,created_at_unix_ms,license,license_class,license_terms_url,commercial_ok,attribution_required,derived_from' }

  # The schema itself, when a configured build tree has generated it: the sidecar's block must be a
  # valid engine.content.AssetProvenance, which is the promise the content build will rely on.
  #
  # Both texts are used exactly as they are on disk, never round-tripped through ConvertFrom-Json and
  # ConvertTo-Json, and a result counts only with no error beside it. PowerShell 7.4 writes the
  # BigInteger that ConvertFrom-Json makes of the schema's u64 bound as an *object*, so a
  # round-tripped schema does not parse — and Test-Json answers $true for a schema it could not
  # parse, on 7.4 and 7.6 alike, with only a non-terminating InvalidJsonSchema error to say so. That
  # made every check here pass vacuously in the Linux container (7.4.12) until the one check that
  # expects a failure noticed; generate.ps1 ("the sidecar against the generated JSON Schema") has
  # the whole story, and `verify` uses the same rule.
  $schemaFile = Get-ChildItem -Path (Join-Path $repo 'build/*/schemas/generated/schemas/json/provenance.schema.json') -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
  if ($schemaFile) {
    $schemaText = [IO.File]::ReadAllText($schemaFile.FullName)
    $at = $schemaText.IndexOf('{')
    $sidecarSchema = $schemaText.Substring(0, $at + 1) + '"type": "object", "required": ["asset_provenance"], "properties": {"asset_provenance": {"$ref": "#/$defs/AssetProvenance"}}, ' + $schemaText.Substring($at + 1)
    function Test-Strict([string]$json) {
      $e = $null
      $ok = Test-Json -Json $json -Schema $sidecarSchema -ErrorAction SilentlyContinue -ErrorVariable e
      $list = @($e | Where-Object { $_ })
      return [pscustomobject]@{ Valid = ([bool]$ok -and $list.Count -eq 0); Ids = @($list | ForEach-Object FullyQualifiedErrorId); Text = (@($list | ForEach-Object { $_.Exception.Message }) -join ' / ') }
    }
    $sideText = [IO.File]::ReadAllText($sideFile)
    Test-That "the generated provenance.schema.json parses under PowerShell $($PSVersionTable.PSVersion) (Test-Json would otherwise pass anything)" {
      @((Test-Strict '{}').Ids | Where-Object { $_ -like 'InvalidJsonSchema,*' }).Count -eq 0 }
    Test-That 'the sidecar as written validates against it' { (Test-Strict $sideText).Valid }
    # Instance Replace with a count: the static overload's fourth argument is RegexOptions, not a count.
    $surprise = ([regex]'"asset_provenance":\s*\{').Replace($sideText, '"asset_provenance": { "surprise": 1,', 1)
    Test-That 'and a block with a field the schema lacks does not, as a validation failure rather than an unread schema' {
      $v = Test-Strict $surprise
      $surprise -ne $sideText -and -not $v.Valid -and ($v.Ids -join ' ') -match 'InvalidJsonAgainstSchema' -and $v.Text -match '/asset_provenance/surprise' }
    $big = ([regex]'"prompt_hash":\s*\d+').Replace($sideText, '"prompt_hash": 18446744073709551615', 1)
    Test-That 'a u64 above 2^63 (half of all prompt hashes) is a number to the validator' { $big -ne $sideText -and (Test-Strict $big).Valid }
    $wrongType = ([regex]'"commercial_ok":\s*(true|false)').Replace($sideText, '"commercial_ok": "no"', 1)
    Test-That 'a field of the wrong type fails too' { $wrongType -ne $sideText -and -not (Test-Strict $wrongType).Valid }

    # `verify` applies the same schema, so a type error its field-name check cannot see still fails.
    $vdir = Join-Path $root 'verify-schema'
    New-Item -ItemType Directory -Force -Path $vdir | Out-Null
    Copy-Item -LiteralPath (Join-Path (Join-Path $tripo '2026-01-05') 'alpha-rock.glb') -Destination $vdir
    [IO.File]::WriteAllText((Join-Path $vdir 'alpha-rock.provenance.json'), $sideText)
    $r = Invoke-Gen verify -Path $vdir -LocalRoot $local -Schema $schemaFile.FullName
    Test-That 'verify -Schema passes the sidecar as written and names the schema it used' { $r.Code -eq 0 -and $r.Json[0].status -eq 'ok' -and $r.Json[0].schema -eq $schemaFile.FullName }
    [IO.File]::WriteAllText((Join-Path $vdir 'alpha-rock.provenance.json'), $wrongType)
    $r = Invoke-Gen verify -Path $vdir -LocalRoot $local -Schema $schemaFile.FullName
    Test-That 'and fails one whose asset_provenance has a field of the wrong type' { $r.Code -ne 0 -and ($r.Json[0].problems -join ' ') -match '/asset_provenance/commercial_ok' }
    $brokenSchema = Join-Path $root 'broken.schema.json'
    [IO.File]::WriteAllText($brokenSchema, '{"$defs": {"AssetProvenance": {"type": 5}}}')
    [IO.File]::WriteAllText((Join-Path $vdir 'alpha-rock.provenance.json'), $sideText)
    $r = Invoke-Gen verify -Path $vdir -LocalRoot $local -Schema $brokenSchema
    Test-That 'and refuses, rather than passes, when the schema does not parse' { $r.Code -ne 0 -and $r.Err -match 'does not parse' }
  } else {
    Write-Host '  skip the JSON Schema check: no build tree has generated provenance.schema.json'
  }

  $r = Invoke-Gen verify -Path $local -LocalRoot $local
  Test-That 'verify re-hashes the outputs and finds nothing wrong' { $r.Code -eq 0 -and $r.Json[0].status -eq 'ok' -and $r.Json[0].sidecars -eq 1 }
  $r = Invoke-Gen ingest -Backend tripo-folder -Manifest $manifestFile -LocalRoot $local -Date 2026-01-05
  Test-That 'a second ingest of the same GLB is a cache hit' { ($r.Json | Where-Object name -eq 'alpha-rock').status -eq 'cached' }
  [IO.File]::WriteAllBytes((Join-Path $m.inbox 'alpha-rock.glb'), $glbBytes + [byte[]](1))
  $r = Invoke-Gen ingest -Backend tripo-folder -Manifest $manifestFile -LocalRoot $local -Date 2026-01-05
  Test-That 'a different GLB under the same name is refused rather than silently replacing the asset' { $r.Code -ne 0 -and $r.Err -match 'different spec' }
  [IO.File]::AppendAllText((Join-Path (Join-Path $tripo '2026-01-05') 'alpha-rock.glb'), 'x')
  $r = Invoke-Gen verify -Path $local -LocalRoot $local
  Test-That 'and verify fails an output that no longer matches its sidecar' { $r.Code -ne 0 -and $r.Json[0].status -eq 'failed' -and ($r.Json[0].problems -join ' ') -match 'no longer matches' }
}
finally {
  if ($KeepTemp) { Write-Host "kept $root" } else { Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed" -ForegroundColor Red
  exit 1
}
Write-Host "all $checks checks passed"
exit 0

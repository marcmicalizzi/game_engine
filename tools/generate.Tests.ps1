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

  $subjects = [ordered]@{ set = 'test-set'; subjects = @(
      [ordered]@{ name = 'alpha-rock'; category = 'rock'; size_m = 1.0; subject = 'a grey rock' }
      [ordered]@{ name = 'beta-cup'; category = 'tool'; size_m = 0.1; subject = 'a tin cup' }) }
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

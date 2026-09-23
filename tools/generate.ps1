#!/usr/bin/env pwsh
<#
.SYNOPSIS
  The generator-service interface (docs/plan/07-content-pipeline.md §7.7) at tool level: text to
  image, image to 3D, and hand-made meshes ingested, every output beside a provenance sidecar.

.DESCRIPTION
  tools/generate.ps1 image    -Workflow <ui-export.json> (-Subjects <list.json> | -Subject <text> -Name <name>)
  tools/generate.ps1 3d       -Backend meshy   -Images <png|dir>...
  tools/generate.ps1 3d       -Backend comfyui -Workflow <image-to-3d ui-export.json> -Images <png|dir>...
  tools/generate.ps1 3d       -Backend comfyui-3d -Workflow <text-to-3d ui-export.json> (-Subjects <list.json> | -Subject <text> -Name <name>)
  tools/generate.ps1 3d       -Backend comfyui-3d -Workflow <text-to-3d ui-export.json> -Images <png|dir>... [-Subjects <list.json>]   # image in
  tools/generate.ps1 pipeline -Pipeline 'text->image->3d' -Workflow <image workflow> -Subjects <list.json>
                              -Backend meshy|tripo-folder|comfyui [-Workflow3d <image-to-3d workflow>]
  tools/generate.ps1 manifest -Backend tripo-folder -Images <png|dir>...     # the list the owner works from by hand
  tools/generate.ps1 ingest   -Backend tripo-folder -Manifest <manifest.json> [-From <dir>]
  tools/generate.ps1 balance                                                 # Meshy credits and the local ledger
  tools/generate.ps1 free                                                    # ComfyUI: unload models, free VRAM
  tools/generate.ps1 prompt   -Subject <text> [-Raw]                         # the prompt the template makes
  tools/generate.ps1 convert  -Workflow <ui-export.json> [-ObjectInfo <file>] # the API form ComfyUI takes
  tools/generate.ps1 verify   [-Path <dir|sidecar>] [-Schema <provenance.schema.json>]  # re-hash, and check against the schema

  Common: [-Date yyyy-MM-dd] [-LocalRoot <dir>] [-Only <name,...>] [-Force] [-DryRun] [-Yes]
  Image:  [-Seed <n>] [-Width <px> -Height <px>] [-Aspect 1:1 -FinalMegapixels 2 -BaseMegapixels <f>]
          [-Negative <text>] [-Raw] [-Set '<title|class|#id>.<input>=<value>'...] [-NoLock] [-NoFree]
  Meshy:  [-AiModel latest] [-CreditsPerTask 30] [-MaxCredits 700] [-MinBalance 165] [-Retries 1]
          [-Concurrency 5] [-Set '<field>=<value>'...] [-Offline (with -DryRun: no balance read)]
          [-Remesh [-TargetPolycount <faces> | -Subjects <list with triangle budgets>] [-Topology triangle|quad]]
  comfyui-3d (the owner's TRELLIS.2 / Pixal3D workflow; the image flags above apply to its image):
          [-Model trellis2|pixal3d] [-FaceCount 100000] [-RemeshResolution <n>] [-RemeshSignMode udf|sdf]
          [-RemeshSmoothIters 3] [-Segmenter pec|adaptive] [-SegmenterTimeoutMinutes 20]
          [-UnwrapResolution <n>] [-UnwrapPadding <n>] [-WeldDistance <f>] [-TextureResolution 2048]
          [-OutputPrefix '3d/Agentic/{date}/{name}'] [-OutputRoot <ComfyUI output dir>] [-Foliage]
          [-TimeoutMinutes 60] [-Budget (with -Subjects: decimate to the subject's triangle budget)]

  IMAGE IN (comfyui-3d with -Images). The workflow draws its own picture with Krea 2; given
  -Images, it is handed one instead, so a mesh starts from the same bytes another service was
  given. The picture the image sampler decodes (found by wiring: the VAEDecode of that sampler's
  latent, which feeds the background removal and the crop) is replaced on every input it fed by a
  LoadImage of the uploaded file, and every node that only fed that decode (the Krea 2 sampler,
  its encoders, latent and loaders) is dropped from the prompt. The sidecar records the image as
  its input and `derived_from` (its asset id), and `parameters.image.supplied` says so.

  REMESH (Meshy). Without -Remesh the request is the one E10's first pass was verified with, and
  Meshy 6/7 do not remesh (`should_remesh` defaults to false for them), so a mesh comes back at
  whatever density the model reconstructed. -Remesh sets `should_remesh`; the polycount is then
  -TargetPolycount (faces, as Meshy counts them, 100-300,000) or, per image, the triangle budget of
  the subject's `budget` class in the -Subjects list (`triangle_budgets`, halved for -Topology quad,
  whose faces are quads). Both are refused without -Remesh, because the service ignores them then.

  WHERE THINGS GO. Every output lands at <LocalRoot>\generated\<service>\<yyyy-MM-dd>\<name>.<ext>
  beside <name>.provenance.json. LocalRoot is $env:ENGINE_LOCAL_ROOT, else
  D:\workspace\game_engine_local — deliberately outside the repository, and the tool refuses a
  root inside it: generated images and meshes, and the owner's workflow files, are never
  committed (docs/content-generation.md). Services: `comfyui` (images), `comfyui-<workflow>`
  (meshes a ComfyUI image-to-3D workflow made), `meshy`, `tripo`.

  THE SIDECAR is `engine.generation.provenance/1`, documented in docs/content-generation.md. Its
  `asset_provenance` block is exactly the schema type engine.content.AssetProvenance
  (schemas/provenance.schema), so the content build can read it without translation; the rest
  is what that record has no field for yet: the prompt, the workflow and every override made to
  it, the output files and their SHA-256, the credits, the timings, the content class.

  A WORKFLOW is any ComfyUI UI export ("Save" in the UI, not "Export (API)", though that form is
  accepted too). It is converted to the API form against the running server's /object_info, and
  the nodes the tool overrides are found by title or by class, never by id: the prompt goes to the
  node titled "Positive" (and "Negative" when one exists), the seed to every literal seed input,
  the size to an Empty*LatentImage or a KreaDualResolutionSelector, the input image to the node
  titled "Input Image" or else the first LoadImage, the server-side file prefix to every
  `filename_prefix`. `-Set` reaches anything else. So a new image-to-3D workflow (TRELLIS,
  Hunyuan3D) needs no code: title its nodes and pass it.

  CREDITS (Meshy). The balance is read before every batch; the count and the estimated cost are
  printed before anything is submitted; a batch that would take the balance below -MinBalance or
  spend more than -MaxCredits is refused before it starts; every task's consumed credits go into
  its sidecar and into <LocalRoot>\generated\meshy\ledger.jsonl. A submitted task's id is written
  to disk before it is polled, so an interrupted run resumes the task instead of paying twice.
  The API key comes from $env:MESHY_API_KEY (a user environment variable) and is never printed,
  logged or written anywhere.

  THE GPU. An image batch (and a ComfyUI image-to-3D batch) holds the machine-wide GPU lock
  (tools/gpu-lock.ps1, D:\workspace\GPU-LOCK.md) for the whole batch, refreshing it as it goes
  rather than taking it per image, and asks ComfyUI to unload its models when the batch ends so
  the next holder gets the memory back.

.EXAMPLE
  tools/generate.ps1 image -Workflow D:\workspace\game_engine_local\workflows\krea2-turbo.json -Subjects content/generation/e10-desert-props.json
  tools/generate.ps1 manifest -Backend tripo-folder -Images D:\workspace\game_engine_local\generated\comfyui\2026-09-22
  tools/generate.ps1 3d -Backend meshy -Images D:\workspace\game_engine_local\generated\comfyui\2026-09-22 -DryRun
#>
[CmdletBinding()]
param(
  [Parameter(Position = 0, Mandatory)]
  [ValidateSet('image', '3d', 'pipeline', 'manifest', 'ingest', 'balance', 'free', 'prompt', 'convert', 'verify')]
  [string]$Command,
  [ValidateSet('comfyui', 'meshy', 'tripo-folder', 'comfyui-3d')] [string]$Backend,
  [string]$Workflow,
  [string]$Workflow3d,
  [string]$Pipeline = 'text->image->3d',
  [string]$Subjects,
  [string]$Subject,
  [string]$Name,
  [string[]]$Only,
  [string[]]$Images,
  [string]$Manifest,
  [string]$From,
  [string]$Path,
  [long]$Seed = -1,
  [int]$Width = 0,
  [int]$Height = 0,
  [string]$Aspect = '',
  [double]$FinalMegapixels = 0,
  [double]$BaseMegapixels = 0,
  [string]$Negative,
  [switch]$Raw,
  [string[]]$Set,
  [string]$Date = (Get-Date -Format 'yyyy-MM-dd'),
  [string]$LocalRoot,
  [string]$Url = $(if ($env:COMFYUI_URL) { $env:COMFYUI_URL } else { 'http://127.0.0.1:8000' }),
  [string]$ObjectInfo,
  [string]$Schema,
  [string]$ServerPrefix = 'Agentic/{date}/{name}',
  [int]$TimeoutSec = 900,
  [string]$AiModel = 'latest',
  [int]$CreditsPerTask = 30,
  [int]$MaxCredits = 700,
  [int]$MinBalance = 165,
  [int]$Retries = 1,
  [int]$Concurrency = 5,
  [int]$TimeoutMinutes = 60,
  [switch]$Remesh,
  [int]$TargetPolycount = 0,
  [ValidateSet('triangle', 'quad')] [string]$Topology = 'triangle',
  [switch]$Offline,
  # comfyui-3d: text -> image -> mesh in one ComfyUI workflow (TRELLIS.2 or Pixal3D)
  [ValidateSet('trellis2', 'pixal3d')] [string]$Model = 'trellis2',
  [int]$FaceCount = 100000,
  [int]$RemeshResolution = 0,
  [ValidateSet('', 'udf', 'sdf')] [string]$RemeshSignMode = '',
  [int]$RemeshSmoothIters = 3,
  [ValidateSet('', 'pec', 'adaptive')] [string]$Segmenter = '',
  [double]$SegmenterTimeoutMinutes = 20,
  [int]$UnwrapResolution = 0,
  [int]$UnwrapPadding = -1,
  [double]$WeldDistance = -1,
  [int]$TextureResolution = 2048,
  [string]$OutputPrefix = '3d/Agentic/{date}/{name}',
  [string]$OutputRoot = $(if ($env:COMFYUI_OUTPUT) { Split-Path -Parent $env:COMFYUI_OUTPUT } else { '' }),
  [switch]$Foliage,
  [switch]$Budget,
  [string]$Operator = $(if ($env:ENGINE_OPERATOR) { $env:ENGINE_OPERATOR } elseif ($env:ENGINE_GPU_LOCK_OWNER) { $env:ENGINE_GPU_LOCK_OWNER } else { 'claude-engine' }),
  [switch]$Yes,
  [switch]$DryRun,
  [switch]$Force,
  [switch]$NoLock,
  [switch]$NoFree
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
# Whether -FaceCount was given, not defaulted: read here because $PSBoundParameters inside a function
# is that function's own (-Budget refuses to guess which of the two was meant).
$script:FaceCountGiven = $PSBoundParameters.ContainsKey('FaceCount')
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
$SidecarSchema = 'engine.generation.provenance/1'
$ManifestSchema = 'engine.generation.tripo-manifest/1'
$LockTool = Join-Path $PSScriptRoot 'gpu-lock.ps1'

# ---- licences -----------------------------------------------------------------------------------
#
# One row per service. `license` is the project-defined id the build's allowlist will name
# (07 §7.5); the two booleans are the schema's conservative defaults (commercial use not assumed,
# attribution assumed) until the owner has reviewed each service's terms, which is recorded as an
# open item in docs/content-generation.md rather than guessed here.
$Licences = @{
  'comfyui' = [ordered]@{ license = 'owner-generated-local'; license_class = 'Proprietary'; license_terms_url = $null; commercial_ok = $false; attribution_required = $true }
  # Local image-to-3D models run in the owner's ComfyUI: the same local row until each model's
  # licence has been reviewed. What TRELLIS.2 and Pixal3D (and the models they load) publish is in
  # docs/content-generation.md, "The local models' licences, as published"; the row is the owner's call.
  'trellis' = [ordered]@{ license = 'owner-generated-local'; license_class = 'Proprietary'; license_terms_url = $null; commercial_ok = $false; attribution_required = $true }
  'pixal3d' = [ordered]@{ license = 'owner-generated-local'; license_class = 'Proprietary'; license_terms_url = $null; commercial_ok = $false; attribution_required = $true }
  'meshy'   = [ordered]@{ license = 'meshy-pro'; license_class = 'Proprietary'; license_terms_url = $null; commercial_ok = $false; attribution_required = $true }
  'tripo'   = [ordered]@{ license = 'tripo-studio-max'; license_class = 'Proprietary'; license_terms_url = $null; commercial_ok = $false; attribution_required = $true }
}

# ---- the prompt template ------------------------------------------------------------------------
#
# What an image-to-3D model wants to be given, learned from the first probes: one object, whole,
# centred and large (a model reconstructs what it can see, and a cropped or tiny object is
# reconstructed as a cropped or blurry one), on a plain light background it can separate cleanly,
# in soft even light (baked-in highlights and cast shadows end up painted into the texture), with
# nothing written on it (text becomes texture garbage) and nobody in it. A three-quarter view from
# slightly above shows three faces, which is what a single-view reconstruction guesses the rest
# from. The negative is used only by workflows that have a node titled "Negative": the turbo
# workflow samples at CFG 1, where a negative prompt has no effect, and it has none.
$Templates = @{
  'image-to-3d/1' = [ordered]@{
    positive = '{subject}. A single object, isolated and centered, the whole object in frame and filling about 85% of a square frame with an even margin on every side, three-quarter view from slightly above, plain uniform light gray studio background, soft even diffuse lighting, no cast shadow, no text, no logo, no people, product photograph, sharp focus.'
    negative = 'text, watermark, logo, signature, people, person, hands, multiple objects, duplicate, cropped, cut off, out of frame, cast shadow, reflection, busy background, scenery, ground plane, blurry, low quality'
  }
}
$TemplateName = 'image-to-3d/1'

# ---- small helpers ------------------------------------------------------------------------------

# stdout carries one JSON line per result and nothing else, so a script or an agent can read it;
# everything a person reads goes to stderr (the engine's own tools draw the same line).
function Write-Log([string]$text) { [Console]::Error.WriteLine($text) }
function Write-Step([string]$text) { Write-Log $text }
function Write-Note([string]$text) { Write-Log $text }
function Write-Warn([string]$text) { Write-Log "warning: $text" }

# Key order is not something a hash may depend on: dictionaries are written with their keys sorted
# ordinally, all the way down, before anything is hashed.
function ConvertTo-Canonical($v) {
  if ($v -is [System.Collections.IDictionary]) {
    $keys = [string[]]@($v.Keys); [Array]::Sort($keys, [StringComparer]::Ordinal)
    $o = [ordered]@{}; foreach ($k in $keys) { $o[$k] = ConvertTo-Canonical $v[$k] }; return $o
  }
  if ($v -is [System.Management.Automation.PSCustomObject]) {
    $keys = [string[]]@($v.PSObject.Properties.Name); [Array]::Sort($keys, [StringComparer]::Ordinal)
    $o = [ordered]@{}; foreach ($k in $keys) { $o[$k] = ConvertTo-Canonical $v.$k }; return $o
  }
  if ($v -is [System.Collections.IList]) { return , @($v | ForEach-Object { ConvertTo-Canonical $_ }) }
  return $v
}
function Get-CanonicalJson($v) { ConvertTo-Canonical $v | ConvertTo-Json -Depth 50 -Compress }

function Get-Utc { [DateTime]::UtcNow }
function Format-Utc([DateTime]$t) { $t.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ') }
function Get-UnixMs([DateTime]$t) { [DateTimeOffset]::new($t.ToUniversalTime()).ToUnixTimeMilliseconds() }

function Get-FileSha256([string]$file) { (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant() }
function Get-TextSha256([string]$text) {
  $bytes = [Text.Encoding]::UTF8.GetBytes($text)
  return ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes))).ToLowerInvariant()
}
# The schema's id128 is 32 hex characters; a content hash becomes an id by taking the first 128
# bits of its SHA-256, which is what "content hashes (as ids)" in AssetProvenance.inputs means.
function ConvertTo-Id128([string]$sha256) { $sha256.Substring(0, 32) }
function ConvertTo-U64([string]$sha256) { [UInt64]::Parse($sha256.Substring(0, 16), [Globalization.NumberStyles]::HexNumber) }

function Read-JsonFile([string]$file) { Get-Content -Raw -LiteralPath $file | ConvertFrom-Json }
function Write-JsonFile([string]$file, $value) {
  $json = $value | ConvertTo-Json -Depth 40
  [IO.File]::WriteAllText($file, $json + "`n", (New-Object Text.UTF8Encoding($false)))
}
function Add-JsonLine([string]$file, $value) {
  $line = ($value | ConvertTo-Json -Depth 20 -Compress) + "`n"
  [IO.File]::AppendAllText($file, $line, (New-Object Text.UTF8Encoding($false)))
}
# ConvertFrom-Json turns ISO timestamps into DateTime; a sidecar read back and written again must
# say the same thing, so every timestamp goes through here on the way out.
function Format-Stamp($value) {
  if ($value -is [DateTime]) { return (Format-Utc $value) }
  return $value
}

function Test-Name([string]$n) {
  if ($n -notmatch '^[a-z0-9][a-z0-9-]*$') { throw "name '$n' must be lower-case letters, digits and hyphens (it is a file name on every machine)" }
}

function Get-LocalRoot {
  $root = if ($LocalRoot) { $LocalRoot } elseif ($env:ENGINE_LOCAL_ROOT) { $env:ENGINE_LOCAL_ROOT } elseif ($IsWin) { 'D:\workspace\game_engine_local' } else { Join-Path $HOME 'game_engine_local' }
  $full = [IO.Path]::GetFullPath($root)
  # Never inside the repository, whichever checkout this script runs from: an output under the
  # tree is one `git add .` away from being committed. The main checkout is the parent of every
  # agent worktree, so its root is checked as well as this one's.
  $roots = @($RepoRoot)
  $marker = [IO.Path]::DirectorySeparatorChar + '.claude' + [IO.Path]::DirectorySeparatorChar + 'worktrees' + [IO.Path]::DirectorySeparatorChar
  $at = $RepoRoot.IndexOf($marker, [StringComparison]::OrdinalIgnoreCase)
  if ($at -gt 0) { $roots += $RepoRoot.Substring(0, $at) }
  foreach ($r in $roots) {
    $prefix = $r.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or $full.TrimEnd('\', '/') -ieq $r.TrimEnd('\', '/')) {
      throw "local root '$full' is inside the repository ($r). Generated outputs are never committed; point -LocalRoot or ENGINE_LOCAL_ROOT outside it."
    }
  }
  return $full
}

function Get-OutputDir([string]$service) {
  $dir = Join-Path (Join-Path (Join-Path (Get-LocalRoot) 'generated') $service) $Date
  if (-not $DryRun) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  return $dir
}

$script:ToolInfo = $null
function Get-ToolInfo {
  if ($script:ToolInfo) { return $script:ToolInfo }
  $commit = ''; $dirty = $false
  try {
    $commit = (& git -C $RepoRoot rev-parse HEAD 2>$null | Out-String).Trim()
    $dirty = -not [string]::IsNullOrWhiteSpace((& git -C $RepoRoot status --porcelain -- tools 2>$null | Out-String))
  } catch { }
  $script:ToolInfo = [ordered]@{ script = 'tools/generate.ps1'; commit = $commit; tools_dirty = $dirty }
  return $script:ToolInfo
}

# PNG width and height from the IHDR chunk, without decoding anything.
function Get-PngSize([string]$file) {
  $fs = [IO.File]::OpenRead($file)
  try {
    $b = New-Object byte[] 24
    if ($fs.Read($b, 0, 24) -lt 24) { return $null }
  } finally { $fs.Dispose() }
  if ($b[0] -ne 0x89 -or $b[1] -ne 0x50 -or $b[2] -ne 0x4E -or $b[3] -ne 0x47) { return $null }
  $w = ([int]$b[16] -shl 24) -bor ([int]$b[17] -shl 16) -bor ([int]$b[18] -shl 8) -bor [int]$b[19]
  $h = ([int]$b[20] -shl 24) -bor ([int]$b[21] -shl 16) -bor ([int]$b[22] -shl 8) -bor [int]$b[23]
  return [ordered]@{ width = $w; height = $h }
}

function Test-GlbMagic([string]$file) {
  $fs = [IO.File]::OpenRead($file)
  try { $b = New-Object byte[] 4; [void]$fs.Read($b, 0, 4) } finally { $fs.Dispose() }
  return ([Text.Encoding]::ASCII.GetString($b) -eq 'glTF')
}

function ConvertFrom-SetValue([string]$text) {
  try { return ($text | ConvertFrom-Json -ErrorAction Stop) } catch { return $text }
}

function Confirm-Action([string]$question) {
  if ($Yes) { return $true }
  try { $answer = Read-Host "$question [y/N]" } catch { throw "$question - not confirmed (non-interactive session; pass -Yes to proceed)" }
  return ($answer -match '^(y|yes)$')
}

# ---- subjects -----------------------------------------------------------------------------------

function Get-SubjectList {
  $list = @()
  if ($Subjects) {
    $doc = Read-JsonFile $Subjects
    $set = if ($doc.set) { [string]$doc.set } else { [IO.Path]::GetFileNameWithoutExtension($Subjects) }
    foreach ($s in @($doc.subjects)) {
      $list += [pscustomobject]@{
        name = [string]$s.name; subject = [string]$s.subject; category = [string]$s.category
        size_m = $s.size_m; seed = $s.seed; set = $set
      }
    }
  } elseif ($Subject) {
    if (-not $Name) { throw '-Subject needs -Name (the file name every stage uses for it)' }
    $list += [pscustomobject]@{ name = $Name; subject = $Subject; category = $null; size_m = $null; seed = $null; set = $null }
  } else {
    throw 'give -Subjects <list.json> or -Subject <text> -Name <name>'
  }
  foreach ($s in $list) { Test-Name $s.name }
  $dupes = $list | Group-Object name | Where-Object Count -gt 1
  if ($dupes) { throw "subject names repeat: $(($dupes | ForEach-Object Name) -join ', ')" }
  if ($Only) {
    $wanted = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $list = @($list | Where-Object { $wanted -contains $_.name })
    $missing = @($wanted | Where-Object { $n = $_; -not ($list | Where-Object name -eq $n) })
    if ($missing) { throw "-Only names no subject called: $($missing -join ', ')" }
  }
  return , @($list)
}

function Get-PromptFor($s) {
  if ($Raw) { return [ordered]@{ template = $null; subject = $s.subject; text = $s.subject; negative = $(if ($Negative) { $Negative } else { '' }) } }
  $t = $Templates[$TemplateName]
  $neg = if ($Negative) { $Negative } else { $t.negative }
  return [ordered]@{ template = $TemplateName; subject = $s.subject; text = $t.positive.Replace('{subject}', $s.subject); negative = $neg }
}

# A seed nobody chose is still a seed that has to be reproducible: derived from the name, so the
# same list regenerates the same images, and small enough (48 bits) for every JSON reader.
function Get-SeedFor($s) {
  if ($Seed -ge 0) { return [long]$Seed }
  if ($null -ne $s.seed) { return [long]$s.seed }
  return [long][UInt64]::Parse((Get-TextSha256 "seed:$($s.name)").Substring(0, 12), [Globalization.NumberStyles]::HexNumber)
}

# ---- ComfyUI: UI export -> API prompt -----------------------------------------------------------
#
# A UI export stores each node's widget values positionally in `widgets_values` and wires nodes
# through a links table (arrays [id, from, from_slot, to, to_slot, type], or objects in newer
# frontends). The API form /prompt takes wants, per node, a map from input name to a literal or to
# [from_node_id, from_slot]. The names of the widget inputs and their order come from the running
# server's /object_info, which is the authority for the node versions actually installed. Lessons
# from the first driver, kept:
#   - a widget input that was converted to a link keeps a stale placeholder in widgets_values; it
#     is skipped (with its control widget) so the later widgets stay aligned;
#   - a seed-like widget with `control_after_generate` is followed by a UI-only value
#     ("randomize", "fixed", ...) that is not an input and must be skipped;
#   - the UI's %date:...% expands only in the browser: through the API a literal % reaches the
#     server's file system, so the date in a file prefix is expanded here;
#   - UI-only nodes (Note, MarkdownNote, Reroute, PrimitiveNode) have no server class: notes are
#     dropped, a Reroute is followed to its source, a PrimitiveNode becomes the literal it holds,
#     and a bypassed node passes its input of the matching type straight through.

$script:ObjectInfoCache = @{}
$script:OfflineInfo = $null
function Get-NodeDef([string]$class) {
  if ($script:ObjectInfoCache.ContainsKey($class)) { return $script:ObjectInfoCache[$class] }
  $def = $null
  if ($ObjectInfo) {
    if (-not $script:OfflineInfo) { $script:OfflineInfo = Read-JsonFile $ObjectInfo }
    if ($script:OfflineInfo.PSObject.Properties[$class]) { $def = $script:OfflineInfo.$class }
  } else {
    $info = Invoke-RestMethod -Uri "$Url/object_info/$([uri]::EscapeDataString($class))" -TimeoutSec 30
    if ($info -and $info.PSObject.Properties[$class]) { $def = $info.$class }
  }
  $script:ObjectInfoCache[$class] = $def
  return $def
}

$UiOnlyNodes = @('Note', 'MarkdownNote', 'Reroute', 'PrimitiveNode')
$ControlWords = @('fixed', 'increment', 'decrement', 'randomize')
# Input kinds the frontend draws as a widget, so each takes a slot in `widgets_values`: the
# primitives, the combo, and the frontend's own widget types that a UI export stores as a value
# (`COLOR` is a swatch; `LOAD_3D` is the 3D viewport's state, saved as "" until the viewport has
# been opened; the V3 dynamic combo is below).
$WidgetKinds = @('INT', 'FLOAT', 'STRING', 'BOOLEAN', 'COMBO', 'COLOR', 'LOAD_3D', 'LOAD_3D_ANIMATION', 'COMFY_DYNAMICCOMBO_V3')

# Whether an input takes a slot in widgets_values. The export itself says so for the frontends this
# was written against (1.52): `node.inputs` lists the *sockets*, connected or not, and a widget that
# was converted to a socket carries a `widget` property there — so an input listed without one is a
# socket and has no widget value, and anything else of a widget kind has one. The kind list is the
# fallback for an input the export does not list at all. (The TRELLIS.2 workflow is what taught
# this: Save3DAdvanced's LOAD_3D viewport state and ImageCropToMask's COLOR are widgets the old
# primitive-only test skipped, and every widget after them shifted by one.)
function Test-WidgetInput($spec, $listed) {
  if ($listed -and -not $listed.PSObject.Properties['widget']) { return $false }
  $kind = $spec[0]
  if ($kind -is [array]) { return $true }
  if ($kind -in $WidgetKinds) { return $true }
  $opts = if ($spec.Count -gt 1) { $spec[1] } else { $null }
  return [bool]($opts -and ($opts.PSObject.Properties['socketless'] -or $opts.PSObject.Properties['default']))
}

# A number the export stored as a string ("1536" for Trellis2UpsampleStage's INT resolution) is
# sent as the number, as the frontend sends it; anything else is passed through as it was saved.
function ConvertTo-WidgetValue($spec, $value) {
  if ($value -is [string]) {
    if ($spec[0] -eq 'INT' -and $value -match '^-?\d+$') { return [long]$value }
    if ($spec[0] -eq 'FLOAT' -and $value -match '^-?\d+(\.\d+)?([eE][-+]?\d+)?$') { return [double]::Parse($value, [Globalization.CultureInfo]::InvariantCulture) }
  }
  return $value
}

# A V3 dynamic combo (`COMFY_DYNAMICCOMBO_V3`: RemeshMesh's sign_mode, DecimateMesh's
# placement_mode) is one widget whose chosen option brings its own widgets with it. The export
# stores the option's values right after the combo's, in the option's order; the API names them
# `<combo>.<input>` — "sign_mode.qef" — which is what the UI itself sent when the owner ran the
# workflow (read back from /history). Returns the option's inputs as [name, spec] pairs.
function Get-DynamicComboInputs($spec, [string]$choice) {
  $opts = if ($spec.Count -gt 1) { $spec[1] } else { $null }
  if (-not $opts -or -not $opts.PSObject.Properties['options']) { return , @() }
  $option = @($opts.options) | Where-Object { [string]$_.key -eq $choice } | Select-Object -First 1
  if (-not $option -or -not $option.inputs) { return , @() }
  $pairs = @()
  foreach ($sec in @('required', 'optional')) {
    if ($option.inputs.PSObject.Properties[$sec]) {
      foreach ($p in $option.inputs.$sec.PSObject.Properties) { $pairs += , @($p.Name, @($p.Value)) }
    }
  }
  return , $pairs
}

function Get-InputSpec($def, [string]$name) {
  if ($def.input.required -and $def.input.required.PSObject.Properties[$name]) { return , @($def.input.required.$name) }
  if ($def.input.optional -and $def.input.optional.PSObject.Properties[$name]) { return , @($def.input.optional.$name) }
  return $null
}

function Resolve-LinkSource($linkId, $nodes, $links, [int]$depth) {
  if ($depth -gt 64) { throw 'a link chain is longer than 64 hops (a cycle through reroutes?)' }
  $l = $links[[string]$linkId]
  if (-not $l) { return $null }
  $src = $nodes[$l.From]
  if (-not $src) { return $null }
  if ($src.type -eq 'Reroute') {
    $in = @($src.inputs)[0]
    if ($null -eq $in -or $null -eq $in.link) { return $null }
    return Resolve-LinkSource $in.link $nodes $links ($depth + 1)
  }
  if ($src.type -eq 'PrimitiveNode') {
    return [pscustomobject]@{ Literal = $true; Value = @($src.widgets_values)[0] }
  }
  if ($src.mode -eq 4) {
    # Bypassed: ComfyUI's frontend routes the first input of the output's type straight through.
    $in = @($src.inputs) | Where-Object { $_.type -eq $l.Type -and $null -ne $_.link } | Select-Object -First 1
    if (-not $in) { return $null }
    return Resolve-LinkSource $in.link $nodes $links ($depth + 1)
  }
  if ($src.mode -eq 2) { throw "an input comes from node $($src.id) ($($src.type)), which is muted; unmute it or remove the link" }
  return [pscustomobject]@{ Literal = $false; Value = @([string]$l.From, [int]$l.FromSlot) }
}

function Convert-Workflow($ui) {
  # Already the API form ("Export (API)"): a map of node id -> {class_type, inputs}.
  if (-not $ui.PSObject.Properties['nodes']) {
    $api = [ordered]@{}; $titles = @{}
    foreach ($p in $ui.PSObject.Properties) {
      if (-not $p.Value.class_type) { throw "not a ComfyUI workflow: '$($p.Name)' has no class_type and there is no 'nodes' array" }
      $inputs = [ordered]@{}
      foreach ($i in $p.Value.inputs.PSObject.Properties) { $inputs[$i.Name] = $i.Value }
      $api[$p.Name] = [ordered]@{ class_type = [string]$p.Value.class_type; inputs = $inputs }
      if ($p.Value._meta -and $p.Value._meta.title) { $titles[$p.Name] = [string]$p.Value._meta.title }
    }
    return [pscustomobject]@{ Api = $api; Titles = $titles }
  }
  if ($ui.PSObject.Properties['definitions'] -and $ui.definitions.subgraphs -and @($ui.definitions.subgraphs).Count -gt 0) {
    throw 'the workflow uses subgraphs, which this converter does not expand; unpack them in the UI and save again'
  }
  $nodes = @{}
  foreach ($n in @($ui.nodes)) { $nodes[[string]$n.id] = $n }
  $links = @{}
  foreach ($l in @($ui.links)) {
    if ($l -is [array]) { $links[[string]$l[0]] = [pscustomobject]@{ From = [string]$l[1]; FromSlot = [int]$l[2]; Type = [string]$l[5] } }
    else { $links[[string]$l.id] = [pscustomobject]@{ From = [string]$l.origin_id; FromSlot = [int]$l.origin_slot; Type = [string]$l.type } }
  }
  $api = [ordered]@{}
  $titles = @{}
  foreach ($n in (@($ui.nodes) | Sort-Object { [long]$_.id })) {
    if ($n.mode -eq 2 -or $n.mode -eq 4) { continue }        # muted / bypassed
    if ($UiOnlyNodes -contains $n.type) { continue }
    $def = Get-NodeDef ([string]$n.type)
    if (-not $def) { throw "node $($n.id) is a '$($n.type)', which the server does not have (a missing custom node, or a UI-only node this converter does not know)" }
    $order = @()
    if ($def.input_order) {
      if ($def.input_order.required) { $order += @($def.input_order.required) }
      if ($def.input_order.optional) { $order += @($def.input_order.optional) }
    } else {
      if ($def.input.required) { $order += @($def.input.required.PSObject.Properties.Name) }
      if ($def.input.optional) { $order += @($def.input.optional.PSObject.Properties.Name) }
    }
    $byName = $n.widgets_values -is [System.Management.Automation.PSCustomObject]
    # @() around the whole `if`: an `if` used as an expression unrolls a one-element array into its
    # element, and a lone widget value would then be indexed as a string, one character at a time.
    $vals = @(if (-not $byName) { $n.widgets_values })
    $w = 0
    $inputs = [ordered]@{}
    foreach ($name in $order) {
      $spec = Get-InputSpec $def $name
      if ($null -eq $spec) { continue }
      $listed = @($n.inputs) | Where-Object { $_ -and $_.name -eq $name } | Select-Object -First 1
      $isWidget = Test-WidgetInput $spec $listed
      $hasControl = $spec.Count -gt 1 -and $spec[1] -and $spec[1].PSObject.Properties['control_after_generate'] -and $spec[1].control_after_generate
      $linked = if ($listed -and $null -ne $listed.link) { $listed } else { $null }
      if ($linked) {
        $src = Resolve-LinkSource $linked.link $nodes $links 0
        if ($src) { $inputs[$name] = $src.Value }
        if ($isWidget -and -not $byName) {
          $w++
          if ($hasControl -and $w -lt $vals.Count -and $vals[$w] -in $ControlWords) { $w++ }
        }
        continue
      }
      if (-not $isWidget) { continue }                       # an unconnected socket (an optional LATENT)
      if ($byName) {
        if ($n.widgets_values.PSObject.Properties[$name]) { $inputs[$name] = $n.widgets_values.$name }
        continue
      }
      if ($w -lt $vals.Count) { $inputs[$name] = ConvertTo-WidgetValue $spec $vals[$w]; $w++ }
      if ($hasControl -and $w -lt $vals.Count -and $vals[$w] -in $ControlWords) { $w++ }
      if ($spec[0] -eq 'COMFY_DYNAMICCOMBO_V3' -and $inputs.Contains($name)) {
        foreach ($pair in (Get-DynamicComboInputs $spec ([string]$inputs[$name]))) {
          $subName = "$name.$($pair[0])"
          $subListed = @($n.inputs) | Where-Object { $_ -and $_.name -eq $subName } | Select-Object -First 1
          if ($subListed -and $null -ne $subListed.link) {
            $src = Resolve-LinkSource $subListed.link $nodes $links 0
            if ($src) { $inputs[$subName] = $src.Value }
            if (Test-WidgetInput $pair[1] $subListed) { $w++ }
          } elseif ((Test-WidgetInput $pair[1] $subListed) -and $w -lt $vals.Count) {
            $inputs[$subName] = ConvertTo-WidgetValue $pair[1] $vals[$w]; $w++
          }
        }
      }
    }
    $id = [string]$n.id
    $api[$id] = [ordered]@{ class_type = [string]$n.type; inputs = $inputs }
    if ($n.title) { $titles[$id] = [string]$n.title }
  }
  return [pscustomobject]@{ Api = $api; Titles = $titles }
}

# A deep copy per job, so one subject's overrides never leak into the next.
function Copy-Graph($graph) {
  $api = $graph.Api | ConvertTo-Json -Depth 50 | ConvertFrom-Json -AsHashtable
  return [pscustomobject]@{ Api = $api; Titles = $graph.Titles }
}

function Test-IsLink($value) {
  return ($value -is [System.Collections.IList]) -and $value.Count -eq 2 -and ($value[0] -is [string])
}

function Find-Nodes($graph, [string]$key) {
  # `#<id>` names one node by id: only for -Set, where a person means a node a class name cannot
  # single out (the TRELLIS.2 workflow has five KSamplers). The tool itself never looks up by id.
  if ($key -match '^#(\d+)$') { if ($graph.Api.Contains($Matches[1])) { return @($Matches[1]) } else { return @() } }
  $byTitle = @($graph.Titles.Keys | Where-Object { $graph.Titles[$_] -eq $key })
  if ($byTitle.Count -gt 0) { return $byTitle }
  return @($graph.Api.Keys | Where-Object { $graph.Api[$_].class_type -eq $key })
}

# Every override is recorded — node, class, title, input, the workflow's value and the new one —
# because "what did the workflow say, and what did the tool change" is the provenance question.
function Set-NodeInput($graph, [string]$id, [string]$inputName, $value, $record, [string]$why) {
  $node = $graph.Api[$id]
  $old = if ($node.inputs.Contains($inputName)) { $node.inputs[$inputName] } else { $null }
  if (Test-IsLink $old) { return $false }                     # driven by another node: leave it
  $node.inputs[$inputName] = $value
  $record.Add([ordered]@{ node = $id; class = $node.class_type; title = $graph.Titles[$id]; input = $inputName; workflow_value = $old; value = $value; why = $why })
  return $true
}

# The checkpoint, UNet, CLIP, VAE and LoRA files the graph names: the model identity a local
# workflow has, recorded by file name (hashing multi-gigabyte files per run is not free, and the
# workflow's own SHA-256 pins which names were asked for).
function Get-ModelFiles($graph) {
  $found = @()
  foreach ($id in $graph.Api.Keys) {
    $node = $graph.Api[$id]
    foreach ($k in @($node.inputs.Keys)) {
      $v = $node.inputs[$k]
      if ($v -is [string] -and ($k -match '(^|_)(ckpt|unet|clip|vae|lora|model|control_net|upscale_model)(_name)?$' -or $v -match '\.(safetensors|ckpt|pt|pth|gguf|bin|onnx)$')) {
        if ($v -match '\.(safetensors|ckpt|pt|pth|gguf|bin|onnx)$') {
          $found += [ordered]@{ node = $id; class = $node.class_type; input = $k; file = $v }
        }
      }
    }
  }
  return , @($found)
}

function Get-OtherParameters($graph) {
  # The literal sampling settings a reader would want without opening the workflow.
  $p = [ordered]@{}
  foreach ($id in $graph.Api.Keys) {
    $node = $graph.Api[$id]
    if ($node.class_type -match 'Sampler|Scheduler|Lora|ResolutionSelector|LatentImage') {
      $lit = [ordered]@{}
      foreach ($k in $node.inputs.Keys) { if (-not (Test-IsLink $node.inputs[$k])) { $lit[$k] = $node.inputs[$k] } }
      $p["$id $($node.class_type)"] = $lit
    }
  }
  return $p
}

# ---- ComfyUI: talking to the server -------------------------------------------------------------

$script:ClientId = [guid]::NewGuid().ToString()

function Get-ComfySystem {
  try {
    $s = Invoke-RestMethod -Uri "$Url/system_stats" -TimeoutSec 15
    return [ordered]@{
      url = $Url; comfyui_version = $s.system.comfyui_version; pytorch_version = $s.system.pytorch_version
      device = (@($s.devices)[0]).name
    }
  } catch { throw "ComfyUI is not answering at $Url ($($_.Exception.Message)). Is it running? (COMFYUI_URL)" }
}

function Invoke-ComfyPrompt($api) {
  $body = @{ prompt = $api; client_id = $script:ClientId } | ConvertTo-Json -Depth 50 -Compress
  try {
    $queued = Invoke-RestMethod -Uri "$Url/prompt" -Method Post -Body ([Text.Encoding]::UTF8.GetBytes($body)) -ContentType 'application/json; charset=utf-8' -TimeoutSec 60
  } catch {
    $detail = if ($_.ErrorDetails -and $_.ErrorDetails.Message) { $_.ErrorDetails.Message } else { $_.Exception.Message }
    throw "ComfyUI refused the prompt: $detail"
  }
  if ($queued.node_errors -and @($queued.node_errors.PSObject.Properties).Count -gt 0) {
    throw ("ComfyUI node errors: " + ($queued.node_errors | ConvertTo-Json -Depth 8 -Compress))
  }
  $promptId = $queued.prompt_id
  $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    $h = Invoke-RestMethod -Uri "$Url/history/$promptId" -TimeoutSec 30
    if ($h.PSObject.Properties[$promptId]) {
      $hist = $h.$promptId
      if ($hist.status.completed -or $hist.status.status_str -eq 'error') {
        if ($hist.status.status_str -eq 'error') {
          $msgs = @($hist.status.messages | Where-Object { $_[0] -eq 'execution_error' } | ForEach-Object { $_[1].exception_message })
          throw "ComfyUI execution failed for prompt $promptId`: $($msgs -join '; ')"
        }
        return [pscustomobject]@{ PromptId = $promptId; History = $hist }
      }
    }
  }
  throw "timed out after $TimeoutSec s waiting for ComfyUI prompt $promptId (the server may be busy with someone else's queue)"
}

# Every file the run produced, whatever the output node calls its list (`images`, `3d`, `gltf`...),
# one object per file into the pipeline; callers collect with @(). (It used to return the list as
# one object, `, @($files)`, and every caller's @() then held a single element — the whole list — so
# a workflow with several output nodes sent all their names to /view as one and got a 400. The
# image workflows have one output node each, which is why it went unseen until TRELLIS.2's twelve.)
function Get-ComfyOutputs($hist) {
  if (-not $hist -or -not $hist.outputs) { return }
  foreach ($out in $hist.outputs.PSObject.Properties) {
    foreach ($key in $out.Value.PSObject.Properties) {
      foreach ($item in @($key.Value)) {
        if ($item -and $item.PSObject -and $item.PSObject.Properties['filename']) {
          [pscustomobject]@{ node = $out.Name; key = $key.Name; filename = [string]$item.filename; subfolder = [string]$item.subfolder; type = $(if ($item.type) { [string]$item.type } else { 'output' }) }
        }
      }
    }
  }
}

function Save-ComfyFile($f, [string]$destination) {
  $q = "filename=$([uri]::EscapeDataString($f.filename))&subfolder=$([uri]::EscapeDataString($f.subfolder))&type=$([uri]::EscapeDataString($f.type))"
  $tmp = "$destination.part"
  Invoke-WebRequest -Uri "$Url/view?$q" -OutFile $tmp -TimeoutSec 300 | Out-Null
  Move-Item -LiteralPath $tmp -Destination $destination -Force
}

function Send-ComfyImage([string]$file) {
  $form = @{ image = Get-Item -LiteralPath $file; overwrite = 'true'; subfolder = 'agentic-inputs' }
  $r = Invoke-RestMethod -Uri "$Url/upload/image" -Method Post -Form $form -TimeoutSec 120
  if ($r.subfolder) { return "$($r.subfolder)/$($r.name)" }
  return [string]$r.name
}

function Invoke-ComfyFree {
  # Unload the models and release the cached memory, so whoever takes the GPU lock next gets
  # the card back rather than the 20 GB this batch left resident.
  $body = '{"unload_models":true,"free_memory":true}'
  try { Invoke-RestMethod -Uri "$Url/free" -Method Post -Body $body -ContentType 'application/json' -TimeoutSec 60 | Out-Null; Write-Note 'comfyui: models unloaded, memory freed' }
  catch { Write-Warn "comfyui: /free failed: $($_.Exception.Message)" }
}

# ---- the GPU lock --------------------------------------------------------------------------------

$script:LockHeld = $false
$script:LockRefreshed = [DateTime]::MinValue
function Enter-GpuLock([string]$purpose) {
  if ($NoLock) { Write-Warn 'running without the GPU lock (-NoLock)'; return }
  # Already held on this run's behalf: `tools/gpu-lock.ps1 run -Exec ...` sets
  # ENGINE_GPU_LOCK_HOLDER to its own pid for what it starts (the bench harness's rule,
  # docs/subsystems/bench.md "Whose lock is mine"). The lock is not re-entrant, so taking it again
  # would wait on ourselves; the wrapper refreshes and releases it. This is how a sweep of several
  # runs holds the GPU once, and keeps ComfyUI's node cache warm between them.
  if ($env:ENGINE_GPU_LOCK_HOLDER) {
    Import-Module (Join-Path $PSScriptRoot 'lib/MachineLock.psm1') -Force
    $held = Read-MachineLock -Path (Get-MachineLockPath -Kind gpu)
    if ($held.Present -and -not $held.Expired -and [string]$held.Pid -eq [string]$env:ENGINE_GPU_LOCK_HOLDER) {
      Write-Note "gpu-lock: held on this run's behalf by pid $($held.Pid) ($($held.Purpose))"
      return
    }
  }
  # Streamed line by line to stderr: while it waits, the lock tool says who holds the GPU and why,
  # which is what a person watching needs to see.
  & $LockTool wait -Purpose $purpose -Minutes 30 -TimeoutMinutes 240 *>&1 | ForEach-Object { Write-Log "  $_" }
  if ($LASTEXITCODE -ne 0) { throw 'could not take the GPU lock (tools/gpu-lock.ps1 status says who holds it)' }
  $script:LockHeld = $true
  $script:LockRefreshed = Get-Utc
}
function Update-GpuLock {
  # One lease for the batch, renewed well before it runs out: a 30-minute lease refreshed every
  # ten minutes survives a slow image and still frees the GPU half an hour after a crash.
  if (-not $script:LockHeld) { return }
  if (((Get-Utc) - $script:LockRefreshed).TotalMinutes -lt 10) { return }
  & $LockTool refresh -Minutes 30 *>&1 | ForEach-Object { Write-Note "  gpu-lock: $_" }
  $script:LockRefreshed = Get-Utc
}
function Exit-GpuLock {
  if (-not $script:LockHeld) { return }
  & $LockTool release *>&1 | ForEach-Object { Write-Note "  gpu-lock: $_" }
  $script:LockHeld = $false
}

# ---- provenance ------------------------------------------------------------------------------------

function New-Sidecar {
  param(
    [string]$Name, [string]$Kind, [string]$Service, [string]$Generator, [string]$ModelId, [string]$ModelVersion,
    $Prompt, $SeedValue, $Parameters, $Backend, $Inputs, $Outputs, $Credits, [DateTime]$Started, [DateTime]$Finished,
    [string]$SpecHash, $DerivedFrom
  )
  $lic = $Licences[$Service.Split('-')[0]]
  # "The prompt or parameter set that drove generation": for an image, the prompt it was given;
  # for a mesh, the whole spec (input image and request), since its prompt was the image's.
  $promptHash = [UInt64]0
  if ($Kind -eq 'image' -and $Prompt -and $Prompt.text) { $promptHash = ConvertTo-U64 (Get-TextSha256 ($Prompt.text + "`0" + [string]$Prompt.negative)) }
  elseif ($SpecHash) { $promptHash = ConvertTo-U64 $SpecHash }
  $primary = @($Outputs)[0]
  $assetProvenance = [ordered]@{
    generator = $Generator
    model_id = $(if ($ModelId) { $ModelId } else { $null })
    model_version = $(if ($ModelVersion) { $ModelVersion } else { $null })
    prompt_hash = $promptHash
    seed = $(if ($null -ne $SeedValue -and [long]$SeedValue -ge 0) { [UInt64]$SeedValue } else { [UInt64]0 })
    inputs = @($Inputs | ForEach-Object { ConvertTo-Id128 $_.sha256 })
    operator_id = ConvertTo-Id128 (Get-TextSha256 "operator:$Operator")
    created_at_unix_ms = Get-UnixMs $Finished
    license = $lic.license
    license_class = $lic.license_class
    license_terms_url = $lic.license_terms_url
    commercial_ok = $lic.commercial_ok
    attribution_required = $lic.attribution_required
    derived_from = @($DerivedFrom)
  }
  return [ordered]@{
    schema = $SidecarSchema
    asset_id = ConvertTo-Id128 $primary.sha256
    name = $Name
    kind = $Kind
    service = $Service
    spec_hash = $SpecHash
    asset_provenance = $assetProvenance
    prompt = $Prompt
    seed = $SeedValue
    parameters = $Parameters
    backend = $Backend
    inputs = @($Inputs)
    outputs = @($Outputs)
    credits = $Credits
    operator = $Operator
    started_utc = Format-Utc $Started
    finished_utc = Format-Utc $Finished
    seconds = [math]::Round(($Finished - $Started).TotalSeconds, 1)
    license = $lic.license
    # Everything this tool produces is general-audience (ADR-0033): no category above `none`. The
    # tool is not a path for classified content, and the subject lists it is fed are reviewed text.
    content_class = 'general'
    content_classes = [ordered]@{ nudity = 'none'; sexual = 'none'; violence = 'none' }
    tool = Get-ToolInfo
  }
}

function New-OutputRecord([string]$role, [string]$file) {
  $rec = [ordered]@{ role = $role; path = [IO.Path]::GetFileName($file); sha256 = Get-FileSha256 $file; bytes = (Get-Item -LiteralPath $file).Length }
  if ($file -match '\.png$') { $size = Get-PngSize $file; if ($size) { $rec.width = $size.width; $rec.height = $size.height } }
  return $rec
}

function Get-SidecarPath([string]$dir, [string]$n) { Join-Path $dir "$n.provenance.json" }

# An existing output is a cache hit when its sidecar recorded the same spec, and a conflict when
# it recorded a different one: the tool never silently replaces an asset made from another spec.
function Test-Existing([string]$sidecar, [string]$specHash, [string]$what) {
  if (-not (Test-Path -LiteralPath $sidecar)) { return 'new' }
  if ($Force) { return 'new' }
  $old = Read-JsonFile $sidecar
  if ($old.spec_hash -eq $specHash) {
    foreach ($o in @($old.outputs)) {
      $f = Join-Path (Split-Path -Parent $sidecar) $o.path
      if (-not (Test-Path -LiteralPath $f)) { return 'new' }
    }
    return 'cached'
  }
  throw "$what already exists from a different spec ($sidecar). Use another -Date or name, or -Force to replace it."
}

# ---- stage: image (ComfyUI) --------------------------------------------------------------------

function Get-WorkflowGraph([string]$file) {
  if (-not $file) { throw 'this stage needs -Workflow <ComfyUI UI export>' }
  if (-not (Test-Path -LiteralPath $file)) { throw "no workflow at $file" }
  $graph = Convert-Workflow (Read-JsonFile $file)
  return [pscustomobject]@{ Graph = $graph; File = (Resolve-Path -LiteralPath $file).Path; Sha256 = Get-FileSha256 $file; Name = [IO.Path]::GetFileName($file) }
}

function Set-JobOverrides($g, [string]$n, $promptRec, [long]$seedValue, [string]$inputImage) {
  $ov = New-Object System.Collections.Generic.List[object]
  if ($promptRec) {
    $pos = @(Find-Nodes $g 'Positive')
    if ($pos.Count -eq 0) { throw 'the workflow has no node titled "Positive"; title the prompt node in the UI and save again' }
    foreach ($id in $pos) {
      $field = if ($g.Api[$id].inputs.Contains('text')) { 'text' } elseif ($g.Api[$id].inputs.Contains('prompt')) { 'prompt' } else { 'text' }
      [void](Set-NodeInput $g $id $field $promptRec.text $ov 'prompt')
    }
    $negNodes = @(Find-Nodes $g 'Negative')
    if ($negNodes.Count -eq 0) { $promptRec.negative = $null }  # the workflow has no negative: record none
    foreach ($id in $negNodes) {
      $field = if ($g.Api[$id].inputs.Contains('text')) { 'text' } else { 'prompt' }
      [void](Set-NodeInput $g $id $field ([string]$promptRec.negative) $ov 'negative prompt')
    }
  }
  $sizeSet = $false
  foreach ($id in @($g.Api.Keys)) {
    $node = $g.Api[$id]
    foreach ($seedInput in @('seed', 'noise_seed', 'random_seed')) {
      if ($node.inputs.Contains($seedInput)) { [void](Set-NodeInput $g $id $seedInput $seedValue $ov 'seed') }
    }
    if ($node.class_type -eq 'KreaDualResolutionSelector') {
      # Two-stage default: square at 2 MP (1456x1456), which image-to-3D models take whole.
      [void](Set-NodeInput $g $id 'aspect_ratio' $(if ($Aspect) { $Aspect } else { '1:1' }) $ov 'size')
      [void](Set-NodeInput $g $id 'final_megapixels' $(if ($FinalMegapixels -gt 0) { $FinalMegapixels } else { 2.0 }) $ov 'size')
      if ($BaseMegapixels -gt 0) { [void](Set-NodeInput $g $id 'base_megapixels' $BaseMegapixels $ov 'size') }
      $sizeSet = $true
    }
  }
  if (-not $sizeSet) {
    foreach ($id in @($g.Api.Keys)) {
      $node = $g.Api[$id]
      if ($node.class_type -match '^Empty.*LatentImage$' -and -not (Test-IsLink $node.inputs['width'])) {
        # Turbo default: 1024x1024, square because an image-to-3D model is given a square.
        [void](Set-NodeInput $g $id 'width' $(if ($Width -gt 0) { $Width } else { 1024 }) $ov 'size')
        [void](Set-NodeInput $g $id 'height' $(if ($Height -gt 0) { $Height } else { 1024 }) $ov 'size')
      }
    }
  }
  if ($inputImage) {
    $targets = @(Find-Nodes $g 'Input Image')
    if ($targets.Count -eq 0) { $targets = @(Find-Nodes $g 'LoadImage') | Select-Object -First 1 }
    if (@($targets).Count -eq 0) { throw 'the workflow has no node titled "Input Image" and no LoadImage node to take the input image' }
    foreach ($id in @($targets)) { [void](Set-NodeInput $g $id 'image' $inputImage $ov 'input image') }
  }
  $prefix = $ServerPrefix.Replace('{date}', $Date).Replace('{name}', $n)
  foreach ($id in @($g.Api.Keys)) {
    if ($g.Api[$id].inputs.Contains('filename_prefix')) { [void](Set-NodeInput $g $id 'filename_prefix' $prefix $ov 'server file prefix') }
  }
  foreach ($s in @($Set)) {
    if (-not $s) { continue }
    if ($s -notmatch '^(?<key>[^=]+)\.(?<input>[^.=]+)=(?<value>.*)$') { throw "-Set '$s' is not <title|class>.<input>=<value>" }
    $ids = @(Find-Nodes $g $Matches.key)
    if ($ids.Count -eq 0) { throw "-Set '$s': no node titled or classed '$($Matches.key)'" }
    $value = ConvertFrom-SetValue $Matches.value
    foreach ($id in $ids) {
      if (-not (Set-NodeInput $g $id $Matches.input $value $ov '-Set')) { throw "-Set '$s': that input of node $id is driven by a link" }
    }
  }
  return , $ov
}

function Get-SpecHash($wf, $g, [string]$extra) {
  # What makes two runs the same run: the workflow's bytes, and the whole graph as sent minus the
  # file prefix (which carries the date, and the date is not part of what was asked for).
  $copy = Copy-Graph $g
  foreach ($id in @($copy.Api.Keys)) { if ($copy.Api[$id].inputs.Contains('filename_prefix')) { $copy.Api[$id].inputs['filename_prefix'] = '' } }
  return Get-TextSha256 "$($wf.Sha256)`n$(Get-CanonicalJson $copy.Api)`n$extra"
}

function Invoke-ImageStage {
  $subjectList = Get-SubjectList
  $wf = Get-WorkflowGraph $Workflow
  $dir = Get-OutputDir 'comfyui'
  $plan = @()
  foreach ($s in $subjectList) {
    $g = Copy-Graph $wf.Graph
    $p = Get-PromptFor $s
    $sd = Get-SeedFor $s
    $ov = Set-JobOverrides $g $s.name $p $sd $null
    $spec = Get-SpecHash $wf $g ''
    $state = Test-Existing (Get-SidecarPath $dir $s.name) $spec "image '$($s.name)'"
    $plan += [pscustomobject]@{ Subject = $s; Graph = $g; Prompt = $p; Seed = $sd; Overrides = $ov; Spec = $spec; State = $state }
  }
  $todo = @($plan | Where-Object State -eq 'new')
  Write-Step "comfyui: $($plan.Count) images with $($wf.Name): $($todo.Count) to generate, $($plan.Count - $todo.Count) already made from the same spec"
  Write-Note "  into $dir"
  if ($DryRun) {
    return , @($plan | ForEach-Object { [pscustomobject][ordered]@{ name = $_.Subject.name; status = "dry-run:$($_.State)"; seed = $_.Seed; prompt = $_.Prompt; overrides = $_.Overrides; spec_hash = $_.Spec } })
  }
  $results = @()
  foreach ($j in ($plan | Where-Object State -eq 'cached')) {
    $side = Get-SidecarPath $dir $j.Subject.name
    $results += [pscustomobject]@{ name = $j.Subject.name; status = 'cached'; path = (Join-Path $dir "$($j.Subject.name).png"); sidecar = $side }
  }
  if ($todo.Count -gt 0) {
    $system = Get-ComfySystem
    $models = Get-ModelFiles $wf.Graph
    Enter-GpuLock "comfyui: $($todo.Count) images, $($wf.Name)"
    try {
      $i = 0
      foreach ($j in $todo) {
        $i++
        Update-GpuLock
        $n = $j.Subject.name
        $t0 = Get-Utc
        [Console]::Error.Write(("  [{0}/{1}] {2} (seed {3}) ..." -f $i, $todo.Count, $n, $j.Seed))
        try {
          $run = Invoke-ComfyPrompt $j.Graph.Api
          $files = @(Get-ComfyOutputs $run.History | Where-Object { $_.filename -match '\.(png|jpg|jpeg|webp)$' -and $_.type -eq 'output' })
          if ($files.Count -eq 0) { throw 'the workflow produced no output image (does it end in a SaveImage node?)' }
          $target = Join-Path $dir "$n.png"
          Save-ComfyFile $files[0] $target
          $t1 = Get-Utc
          $size = Get-PngSize $target
          $params = [ordered]@{ width = $size.width; height = $size.height; sampling = Get-OtherParameters $j.Graph }
          $backend = [ordered]@{
            comfyui = $system
            workflow = $wf.Name; workflow_sha256 = $wf.Sha256; workflow_path = $wf.File
            models = $models
            overrides = $j.Overrides
            prompt_id = $run.PromptId
            server_files = @($files | ForEach-Object { ("$($_.subfolder)/$($_.filename)").TrimStart('/') })
            api_prompt = $j.Graph.Api
          }
          $unet = @($models | Where-Object { $_.class -match 'UNETLoader|CheckpointLoader' } | Select-Object -First 1)
          $side = New-Sidecar -Name $n -Kind 'image' -Service 'comfyui' -Generator "comfyui/$($wf.Name)" `
            -ModelId $(if ($unet) { $unet[0].file } else { $null }) -ModelVersion $null `
            -Prompt $j.Prompt -SeedValue $j.Seed -Parameters $params -Backend $backend -Inputs @() `
            -Outputs @(New-OutputRecord 'image' $target) -Credits 0 -Started $t0 -Finished $t1 -SpecHash $j.Spec -DerivedFrom @()
          $side['subject'] = [ordered]@{ set = $j.Subject.set; category = $j.Subject.category; size_m = $j.Subject.size_m }
          Write-JsonFile (Get-SidecarPath $dir $n) $side
          Write-Log (" {0:N1} s" -f ($t1 - $t0).TotalSeconds)
          $results += [pscustomobject]@{ name = $n; status = 'generated'; path = $target; sidecar = (Get-SidecarPath $dir $n); seconds = [math]::Round(($t1 - $t0).TotalSeconds, 1) }
        } catch {
          Write-Log ' failed'
          Write-Warn "$n`: $($_.Exception.Message)"
          $results += [pscustomobject]@{ name = $n; status = 'failed'; error = $_.Exception.Message }
        }
      }
    } finally {
      if (-not $NoFree) { Invoke-ComfyFree }
      Exit-GpuLock
    }
  }
  return , @($results)
}

# ---- inputs for the 3D stages -----------------------------------------------------------------------

function Get-ImageInputs {
  if (-not $Images) { throw 'this stage needs -Images <png|dir>...' }
  $list = @()
  foreach ($item in $Images) {
    foreach ($part in ($item -split ',')) {
      $p = $part.Trim(); if (-not $p) { continue }
      if (Test-Path -LiteralPath $p -PathType Container) { $list += @(Get-ChildItem -LiteralPath $p -File | Where-Object { $_.Extension -in '.png', '.jpg', '.jpeg' } | Sort-Object Name | ForEach-Object FullName) }
      elseif (Test-Path -LiteralPath $p) { $list += (Resolve-Path -LiteralPath $p).Path }
      else { throw "no image at $p" }
    }
  }
  $inputs = @()
  foreach ($f in $list) {
    $stem = [IO.Path]::GetFileNameWithoutExtension($f)
    $sideFile = Join-Path (Split-Path -Parent $f) "$stem.provenance.json"
    $side = if (Test-Path -LiteralPath $sideFile) { Read-JsonFile $sideFile } else { $null }
    $inputs += [pscustomobject]@{
      name = $stem; path = $f; sha256 = Get-FileSha256 $f
      sidecar = $(if ($side) { $sideFile } else { $null }); sidecar_data = $side
      asset_id = $(if ($side) { [string]$side.asset_id } else { $null })
    }
  }
  foreach ($i in $inputs) { Test-Name $i.name }
  if ($Only) {
    $wanted = @($Only | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    $inputs = @($inputs | Where-Object { $wanted -contains $_.name })
  }
  if ($inputs.Count -eq 0) { throw 'no input images' }
  return , @($inputs)
}

function New-InputRecord($in) {
  $rec = [ordered]@{ role = 'image'; path = $in.path; sha256 = $in.sha256 }
  if ($in.sidecar) { $rec.provenance = $in.sidecar; $rec.asset_id = $in.asset_id }
  return $rec
}

# The prompt that made the image is the prompt that made the mesh: copied, so one sidecar tells
# the whole chain without the reader chasing files.
function Get-UpstreamPrompt($in) {
  if (-not $in.sidecar_data -or -not $in.sidecar_data.prompt) { return $null }
  $p = $in.sidecar_data.prompt
  return [ordered]@{ from_input = $true; template = $p.template; subject = $p.subject; text = $p.text; negative = $p.negative; image_seed = $in.sidecar_data.seed }
}

# ---- stage: 3D through Meshy ------------------------------------------------------------------------

function Get-MeshyKey {
  $k = $env:MESHY_API_KEY
  if (-not $k -and $IsWin) { $k = [Environment]::GetEnvironmentVariable('MESHY_API_KEY', 'User') }
  if (-not $k) { throw 'MESHY_API_KEY is not set. It is a user environment variable; this tool never takes the key on the command line.' }
  return $k
}

$MeshyApi = 'https://api.meshy.ai/openapi/v1'
function Invoke-Meshy([string]$method, [string]$path, $body) {
  $headers = @{ Authorization = "Bearer $(Get-MeshyKey)" }
  for ($attempt = 1; ; $attempt++) {
    try {
      if ($body) {
        $json = $body | ConvertTo-Json -Depth 10 -Compress
        return Invoke-RestMethod -Uri "$MeshyApi$path" -Method $method -Headers $headers -Body ([Text.Encoding]::UTF8.GetBytes($json)) -ContentType 'application/json' -TimeoutSec 120
      }
      return Invoke-RestMethod -Uri "$MeshyApi$path" -Method $method -Headers $headers -TimeoutSec 60
    } catch {
      $status = 0
      if ($_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
      # 429 is Meshy's rate limit and queue limit; 5xx is the service having a bad minute. Both
      # are worth waiting out a few times; anything else is an answer.
      if (($status -eq 429 -or $status -ge 500) -and $attempt -lt 6) { Start-Sleep -Seconds (15 * $attempt); continue }
      $detail = if ($_.ErrorDetails -and $_.ErrorDetails.Message) { $_.ErrorDetails.Message } else { $_.Exception.Message }
      throw "meshy $method $path failed (HTTP $status): $detail"
    }
  }
}

function Get-MeshyBalance { [int](Invoke-Meshy 'GET' '/balance' $null).balance }

function Get-MeshyBody([int]$polycount) {
  # The body the owner's first task was verified with (30 credits): the newest model, textured,
  # with PBR maps, as triangles. Without -Remesh it is exactly that body, field for field, so the
  # first pass's spec hashes still name its outputs and a rerun of it is a cache hit. With -Remesh
  # the two fields the remesh phase reads follow it (topology is only read when remeshing, which is
  # why the old body's `topology` is not a remesh). Anything else goes through -Set and is recorded.
  $body = [ordered]@{ ai_model = $AiModel; should_texture = $true; enable_pbr = $true; topology = $Topology }
  if ($Remesh) {
    $body.should_remesh = $true
    $body.target_polycount = $polycount
  }
  foreach ($s in @($Set)) {
    if (-not $s) { continue }
    if ($s -notmatch '^(?<key>[a-z_]+)=(?<value>.*)$') { throw "-Set '$s' is not <field>=<value> for the Meshy request" }
    $body[$Matches.key] = ConvertFrom-SetValue $Matches.value
  }
  return $body
}

# Meshy's remesh range for `target_polycount` (openapi v1 image-to-3d, read 2026-09-23). The
# service says the count it returns "may deviate from the target depending on the geometry".
$MeshyPolycountMin = 100
$MeshyPolycountMax = 300000

# The triangle budget per subject, from the -Subjects list: `triangle_budgets.<class>.triangles`
# for the subject's `budget` class. Keyed by subject name, which is the image's file stem.
function Get-SubjectBudgets {
  $map = @{}
  if (-not $Subjects) { return $map }
  $doc = Read-JsonFile $Subjects
  $budgets = $doc.triangle_budgets
  foreach ($s in @($doc.subjects)) {
    if (-not $s.budget) { continue }
    $class = [string]$s.budget
    if (-not $budgets -or -not $budgets.PSObject.Properties[$class]) { throw "subject '$($s.name)' names budget class '$class', which $Subjects does not define under triangle_budgets" }
    $tri = [int]$budgets.$class.triangles
    if ($tri -le 0) { throw "budget class '$class' in $Subjects has no positive 'triangles'" }
    $map[[string]$s.name] = [pscustomobject]@{ class = $class; triangles = $tri }
  }
  return $map
}

# The polycount one image is remeshed to, and where the number came from (recorded beside it).
function Get-MeshyPolycount($in, $budgets) {
  if (-not $Remesh) { return [pscustomobject]@{ faces = 0; from = $null; budget_class = $null; budget_triangles = $null } }
  if ($TargetPolycount -gt 0) { return [pscustomobject]@{ faces = $TargetPolycount; from = '-TargetPolycount'; budget_class = $null; budget_triangles = $null } }
  $b = $budgets[$in.name]
  if (-not $b) { throw "-Remesh needs a polycount for '$($in.name)': pass -TargetPolycount <faces>, or -Subjects <list> in which that subject names a 'budget' class" }
  # A budget is in triangles; a quad-dominant remesh counts quads, each two triangles.
  $faces = if ($Topology -eq 'quad') { [int][math]::Floor($b.triangles / 2) } else { $b.triangles }
  return [pscustomobject]@{ faces = $faces; from = "budget:$($b.class) ($([IO.Path]::GetFileName($Subjects)))"; budget_class = $b.class; budget_triangles = $b.triangles }
}

function Test-MeshyArguments {
  if (-not $Remesh) {
    if ($TargetPolycount -gt 0) { throw '-TargetPolycount without -Remesh: Meshy reads target_polycount only in its remesh phase, which is off unless -Remesh is given' }
    if ($Topology -ne 'triangle') { throw "-Topology $Topology without -Remesh: Meshy reads topology only in its remesh phase, which is off unless -Remesh is given" }
  }
  if ($TargetPolycount -ne 0 -and ($TargetPolycount -lt $MeshyPolycountMin -or $TargetPolycount -gt $MeshyPolycountMax)) {
    throw "-TargetPolycount $TargetPolycount is outside Meshy's remesh range of $MeshyPolycountMin to $MeshyPolycountMax faces"
  }
  if ($Offline -and -not $DryRun) { throw '-Offline is for -DryRun only: a real batch reads the balance before it spends anything' }
}

# The request without the image: what the sidecar, the task file and the ledger record.
function Get-RequestRecord($body) { $r = [ordered]@{}; foreach ($k in $body.Keys) { $r[$k] = $body[$k] }; return $r }

function Invoke-MeshyStage($inputs) {
  Test-MeshyArguments
  $dir = Get-OutputDir 'meshy'
  $ledger = Join-Path (Split-Path -Parent $dir) 'ledger.jsonl'
  $budgets = Get-SubjectBudgets
  $jobs = @()
  foreach ($in in $inputs) {
    # One body per image: with -Remesh from a subject list, each subject carries its own polycount.
    # The spec hashes that body the way the first pass hashed its one shared body, so a batch
    # without -Remesh names exactly the outputs it named before.
    $poly = Get-MeshyPolycount $in $budgets
    $body = Get-MeshyBody $poly.faces
    $bodyHash = Get-TextSha256 ($body | ConvertTo-Json -Compress)
    $spec = Get-TextSha256 "$($in.sha256)`n$bodyHash"
    $side = Get-SidecarPath $dir $in.name
    $taskFile = Join-Path $dir "$($in.name).meshy-task.json"
    $state = Test-Existing $side $spec "mesh '$($in.name)'"
    $taskId = $null; $attempt = 1
    if ($state -eq 'new' -and (Test-Path -LiteralPath $taskFile) -and -not $Force) {
      $t = Read-JsonFile $taskFile
      if ($t.spec_hash -eq $spec -and $t.task_id) { $state = 'resume'; $taskId = [string]$t.task_id; $attempt = [int]$t.attempt }
    }
    # (Not `$remesh`: PowerShell's names ignore case, and a local of that name would shadow the
    # -Remesh parameter for every function this one calls, through dynamic scope.)
    $remeshRecord = $null
    if ($Remesh) { $remeshRecord = [ordered]@{ should_remesh = $true; topology = $Topology; target_polycount = $poly.faces; polycount_from = $poly.from; budget_class = $poly.budget_class; budget_triangles = $poly.budget_triangles } }
    $jobs += [pscustomobject]@{ Input = $in; Body = $body; Remesh = $remeshRecord; Spec = $spec; State = $state; TaskId = $taskId; Attempt = $attempt; Submitted = $null; Progress = -1; Sidecar = $side; TaskFile = $taskFile }
  }
  $new = @($jobs | Where-Object State -eq 'new')
  $resume = @($jobs | Where-Object State -eq 'resume')
  $estimate = $new.Count * $CreditsPerTask
  $balance = if ($Offline) { $null } else { Get-MeshyBalance }
  Write-Step "meshy: $($jobs.Count) images: $($new.Count) to submit at ~$CreditsPerTask credits = ~$estimate credits; $($resume.Count) already submitted (resumed, no new spend); $($jobs.Count - $new.Count - $resume.Count) already made"
  if ($Offline) { Write-Step "meshy: balance not read (-Offline); floor $MinBalance; this run's cap $MaxCredits" }
  else { Write-Step "meshy: balance $balance -> ~$($balance - $estimate) after; floor $MinBalance; this run's cap $MaxCredits; up to $Retries retry per failed task within the cap" }
  $distinct = @($jobs | ForEach-Object { $_.Body | ConvertTo-Json -Compress } | Sort-Object -Unique)
  foreach ($d in $distinct) { Write-Note "  request: $d + the image as a data URI ($(@($jobs | Where-Object { ($_.Body | ConvertTo-Json -Compress) -eq $d }).Count) images)" }
  Write-Note "  into $dir"
  if ($estimate -gt $MaxCredits) { throw "refused: ~$estimate credits is over this run's cap of $MaxCredits (-MaxCredits)" }
  if ($null -ne $balance -and $balance - $estimate -lt $MinBalance) { throw "refused: ~$estimate credits would take the balance from $balance below the floor of $MinBalance (-MinBalance)" }
  if ($DryRun) {
    return , @($jobs | ForEach-Object {
        [pscustomobject][ordered]@{ name = $_.Input.name; status = "dry-run:$($_.State)"; image = $_.Input.path; task_id = $_.TaskId; request = (Get-RequestRecord $_.Body); remesh = $_.Remesh; spec_hash = $_.Spec } })
  }
  if ($new.Count -gt 0 -and -not (Confirm-Action "Spend ~$estimate Meshy credits on $($new.Count) tasks?")) { throw 'not confirmed' }

  $results = @()
  foreach ($j in ($jobs | Where-Object State -eq 'cached')) { $results += [pscustomobject]@{ name = $j.Input.name; status = 'cached'; sidecar = $j.Sidecar } }
  $queue = New-Object System.Collections.Generic.Queue[object]
  foreach ($j in $new) { $queue.Enqueue($j) }
  $inflight = New-Object System.Collections.Generic.List[object]
  foreach ($j in $resume) { $j.Submitted = Get-Utc; $inflight.Add($j) }
  $committed = 0          # credits this run has committed to (estimated at submission)
  $consumed = 0           # credits the finished tasks report
  while ($queue.Count -gt 0 -or $inflight.Count -gt 0) {
    while ($inflight.Count -lt $Concurrency -and $queue.Count -gt 0) {
      $j = $queue.Dequeue()
      if ($committed + $CreditsPerTask -gt $MaxCredits -or $balance - $committed - $CreditsPerTask -lt $MinBalance) {
        Write-Warn "$($j.Input.name): not submitted, the cap or the floor would be crossed"
        $results += [pscustomobject]@{ name = $j.Input.name; status = 'not-submitted'; reason = 'credit cap or floor' }
        continue
      }
      $request = Get-RequestRecord $j.Body
      $ext = [IO.Path]::GetExtension($j.Input.path).TrimStart('.').ToLowerInvariant(); if ($ext -eq 'jpg') { $ext = 'jpeg' }
      $request.image_url = "data:image/$ext;base64," + [Convert]::ToBase64String([IO.File]::ReadAllBytes($j.Input.path))
      $r = Invoke-Meshy 'POST' '/image-to-3d' $request
      $j.TaskId = [string]$r.result
      $j.Submitted = Get-Utc
      $j.Progress = -1
      $committed += $CreditsPerTask
      # On disk before the first poll: an interrupted run resumes this task rather than paying again.
      Write-JsonFile $j.TaskFile ([ordered]@{ name = $j.Input.name; task_id = $j.TaskId; attempt = $j.Attempt; spec_hash = $j.Spec; image = $j.Input.path; image_sha256 = $j.Input.sha256; request = (Get-RequestRecord $j.Body); remesh = $j.Remesh; submitted_utc = Format-Utc $j.Submitted })
      # The request goes into the ledger too (without the image), so the ledger alone says what
      # every credit bought: a remeshed task and an unremeshed one cost the same and are not the
      # same asset.
      Add-JsonLine $ledger ([ordered]@{ utc = Format-Utc $j.Submitted; event = 'submitted'; name = $j.Input.name; task_id = $j.TaskId; attempt = $j.Attempt; estimate = $CreditsPerTask; request = (Get-RequestRecord $j.Body); remesh = $j.Remesh })
      Write-Log "  submitted $($j.Input.name) -> $($j.TaskId) (attempt $($j.Attempt))"
      $inflight.Add($j)
    }
    if ($inflight.Count -eq 0) { break }
    Start-Sleep -Seconds 10
    # A snapshot, because the loop removes finished tasks. `.ToArray()` and not `@($inflight)`:
    # array-wrapping a generic List throws "Argument types do not match" in PowerShell 7.6, which
    # is how the first E10 batch stopped after its fifth submission (it resumed without paying).
    foreach ($j in $inflight.ToArray()) {
      try { $t = Invoke-Meshy 'GET' "/image-to-3d/$($j.TaskId)" $null } catch { Write-Warn "$($j.Input.name): poll failed: $($_.Exception.Message)"; continue }
      if ([int]$t.progress -ne $j.Progress -and $t.status -eq 'IN_PROGRESS') { $j.Progress = [int]$t.progress; Write-Note "  $($j.Input.name): $($t.status) $($t.progress)%" }
      if ($t.status -eq 'SUCCEEDED') {
        $inflight.Remove($j) | Out-Null
        try {
          $results += Save-MeshyResult $j $t $dir $ledger
          $consumed += [int]$t.consumed_credits
        } catch {
          Write-Warn "$($j.Input.name): the task succeeded but saving it failed: $($_.Exception.Message) (the task file is kept; a rerun resumes it)"
          $results += [pscustomobject]@{ name = $j.Input.name; status = 'failed'; error = $_.Exception.Message; task_id = $j.TaskId }
        }
      } elseif ($t.status -in @('FAILED', 'CANCELED', 'EXPIRED')) {
        $inflight.Remove($j) | Out-Null
        $consumed += [int]$t.consumed_credits
        $msg = if ($t.task_error -and $t.task_error.message) { [string]$t.task_error.message } else { [string]$t.status }
        Add-JsonLine $ledger ([ordered]@{ utc = Format-Utc (Get-Utc); event = 'failed'; name = $j.Input.name; task_id = $j.TaskId; attempt = $j.Attempt; consumed_credits = $t.consumed_credits; error = $msg })
        $failFile = Join-Path $dir "$($j.Input.name).meshy-failed.json"
        $history = @(); if (Test-Path -LiteralPath $failFile) { $history = @((Read-JsonFile $failFile).attempts) }
        $history += [ordered]@{ task_id = $j.TaskId; attempt = $j.Attempt; status = $t.status; error = $msg; consumed_credits = $t.consumed_credits; utc = Format-Utc (Get-Utc) }
        Write-JsonFile $failFile ([ordered]@{ name = $j.Input.name; image = $j.Input.path; image_sha256 = $j.Input.sha256; attempts = $history })
        Remove-Item -LiteralPath $j.TaskFile -Force -ErrorAction SilentlyContinue
        Write-Warn "$($j.Input.name): $($t.status): $msg"
        if ($j.Attempt -le $Retries) {
          $j.Attempt++; $j.TaskId = $null; $queue.Enqueue($j)
          Write-Note "  $($j.Input.name): retrying (attempt $($j.Attempt))"
        } else {
          $results += [pscustomobject]@{ name = $j.Input.name; status = 'failed'; error = $msg; task_id = $j.TaskId }
        }
      } elseif (((Get-Utc) - $j.Submitted).TotalMinutes -gt $TimeoutMinutes) {
        $inflight.Remove($j) | Out-Null
        Write-Warn "$($j.Input.name): still $($t.status) after $TimeoutMinutes minutes; left running (a rerun resumes task $($j.TaskId))"
        $results += [pscustomobject]@{ name = $j.Input.name; status = 'timed-out'; task_id = $j.TaskId }
      }
    }
  }
  $after = Get-MeshyBalance
  Write-Step "meshy: balance $balance -> $after ($($balance - $after) spent; the finished tasks report $consumed)"
  Add-JsonLine $ledger ([ordered]@{ utc = Format-Utc (Get-Utc); event = 'batch'; balance_before = $balance; balance_after = $after; tasks_consumed = $consumed; remesh = [bool]$Remesh; topology = $Topology; target_polycount = $(if ($Remesh) { if ($TargetPolycount -gt 0) { $TargetPolycount } else { 'per subject budget' } } else { $null }) })
  return , @($results)
}

function Save-MeshyResult($j, $t, [string]$dir, [string]$ledger) {
  $body = Get-RequestRecord $j.Body
  $n = $j.Input.name
  $glb = Join-Path $dir "$n.glb"
  # The URL is a signed link whose signature carries `~` and `&`: it is taken from the parsed JSON
  # object and handed over untouched, never cut out of text (a regex over the response corrupted
  # it twice on first contact).
  $url = [string]$t.model_urls.glb
  if (-not $url) { throw 'the task reports no GLB' }
  Invoke-WebRequest -Uri $url -OutFile "$glb.part" -TimeoutSec 600 -MaximumRetryCount 3 -RetryIntervalSec 10 | Out-Null
  if (-not (Test-GlbMagic "$glb.part")) { Remove-Item -LiteralPath "$glb.part" -Force; throw 'the download is not a GLB' }
  Move-Item -LiteralPath "$glb.part" -Destination $glb -Force
  $outputs = @(New-OutputRecord 'mesh' $glb)
  if ($t.thumbnail_url) {
    $thumb = Join-Path $dir "$n.thumb.png"
    try { Invoke-WebRequest -Uri ([string]$t.thumbnail_url) -OutFile $thumb -TimeoutSec 120 | Out-Null; $outputs += New-OutputRecord 'thumbnail' $thumb } catch { Write-Warn "$n`: thumbnail download failed" }
  }
  $started = [DateTimeOffset]::FromUnixTimeMilliseconds([long]$t.created_at).UtcDateTime
  $finished = if ($t.finished_at) { [DateTimeOffset]::FromUnixTimeMilliseconds([long]$t.finished_at).UtcDateTime } else { Get-Utc }
  $reported = if ($t.PSObject.Properties['ai_model'] -and $t.ai_model) { [string]$t.ai_model } else { $null }
  $backend = [ordered]@{
    meshy = [ordered]@{
      task_id = $j.TaskId; attempt = $j.Attempt; status = $t.status
      ai_model_requested = $AiModel; ai_model_reported = $reported
      created_at_ms = $t.created_at; started_at_ms = $t.started_at; finished_at_ms = $t.finished_at
      consumed_credits = $t.consumed_credits
      request = $body
      # null for a task that was not remeshed; otherwise what was asked for and where the number
      # came from (a subject's budget class, or -TargetPolycount). The service calls the target
      # approximate, so what came back is the GLB's to say, not this record's.
      remesh = $j.Remesh
    }
  }
  $inRec = New-InputRecord $j.Input
  $derived = @(); if ($j.Input.asset_id) { $derived += $j.Input.asset_id } else { $derived += ConvertTo-Id128 $j.Input.sha256 }
  $side = New-Sidecar -Name $n -Kind 'mesh' -Service 'meshy' -Generator 'meshy/image-to-3d' -ModelId 'meshy-image-to-3d' `
    -ModelVersion $(if ($reported) { $reported } else { "$AiModel (alias, resolved by the service on $(Format-Utc $started))" }) `
    -Prompt (Get-UpstreamPrompt $j.Input) -SeedValue $null -Parameters $body -Backend $backend -Inputs @($inRec) `
    -Outputs $outputs -Credits $t.consumed_credits -Started $started -Finished $finished -SpecHash $j.Spec -DerivedFrom $derived
  Write-JsonFile $j.Sidecar $side
  Remove-Item -LiteralPath $j.TaskFile -Force -ErrorAction SilentlyContinue
  Add-JsonLine $ledger ([ordered]@{ utc = Format-Utc (Get-Utc); event = 'succeeded'; name = $n; task_id = $j.TaskId; attempt = $j.Attempt; consumed_credits = $t.consumed_credits })
  Write-Log ("  done {0}: {1:N1} MB, {2} credits, {3:N0} s on the service" -f $n, ((Get-Item -LiteralPath $glb).Length / 1MB), $t.consumed_credits, ($finished - $started).TotalSeconds)
  return [pscustomobject]@{ name = $n; status = 'generated'; path = $glb; sidecar = $j.Sidecar; credits = $t.consumed_credits; task_id = $j.TaskId }
}

# ---- stage: 3D through a ComfyUI workflow ---------------------------------------------------------

function Invoke-Comfy3dStage($inputs, [string]$workflowFile) {
  $wf = Get-WorkflowGraph $workflowFile
  $service = 'comfyui-' + ([IO.Path]::GetFileNameWithoutExtension($wf.Name).ToLowerInvariant() -replace '[^a-z0-9]+', '-').Trim('-')
  $dir = Get-OutputDir $service
  $plan = @()
  foreach ($in in $inputs) {
    $g = Copy-Graph $wf.Graph
    $sd = if ($Seed -ge 0) { $Seed } else { [long][UInt64]::Parse((Get-TextSha256 "seed:$($in.name)").Substring(0, 12), [Globalization.NumberStyles]::HexNumber) }
    $ov = Set-JobOverrides $g $in.name $null $sd 'pending-upload'
    $spec = Get-SpecHash $wf $g $in.sha256
    $state = Test-Existing (Get-SidecarPath $dir $in.name) $spec "mesh '$($in.name)'"
    $plan += [pscustomobject]@{ Input = $in; Seed = $sd; Spec = $spec; State = $state }
  }
  $todo = @($plan | Where-Object State -eq 'new')
  Write-Step "$service`: $($plan.Count) images through $($wf.Name): $($todo.Count) to run, $($plan.Count - $todo.Count) already made"
  if ($DryRun) { return , @($plan | ForEach-Object { [pscustomobject]@{ name = $_.Input.name; status = "dry-run:$($_.State)"; spec_hash = $_.Spec } }) }
  $results = @($plan | Where-Object State -eq 'cached' | ForEach-Object { [pscustomobject]@{ name = $_.Input.name; status = 'cached'; sidecar = (Get-SidecarPath $dir $_.Input.name) } })
  if ($todo.Count -eq 0) { return , @($results) }
  $system = Get-ComfySystem
  $models = Get-ModelFiles $wf.Graph
  Enter-GpuLock "comfyui image-to-3D: $($todo.Count) meshes, $($wf.Name)"
  try {
    foreach ($j in $todo) {
      Update-GpuLock
      $n = $j.Input.name
      $t0 = Get-Utc
      try {
        $uploaded = Send-ComfyImage $j.Input.path
        $g = Copy-Graph $wf.Graph
        $ov = Set-JobOverrides $g $n $null $j.Seed $uploaded
        $run = Invoke-ComfyPrompt $g.Api
        $files = @(Get-ComfyOutputs $run.History | Where-Object { $_.type -eq 'output' })
        $meshes = @($files | Where-Object { $_.filename -match '\.(glb|gltf|obj|ply|fbx|stl)$' })
        $outputs = @()
        $k = 0
        foreach ($f in $meshes) {
          $ext = [IO.Path]::GetExtension($f.filename).ToLowerInvariant()
          $target = if ($k -eq 0) { Join-Path $dir "$n$ext" } else { Join-Path $dir "$n.$k$ext" }
          Save-ComfyFile $f $target; $outputs += New-OutputRecord 'mesh' $target; $k++
        }
        $k = 0
        foreach ($f in ($files | Where-Object { $_.filename -match '\.(png|jpg|jpeg|webp)$' })) {
          $target = Join-Path $dir ("$n.preview$(if ($k) { ".$k" })" + [IO.Path]::GetExtension($f.filename))
          Save-ComfyFile $f $target; $outputs += New-OutputRecord 'preview' $target; $k++
        }
        if ($outputs.Count -eq 0) { throw 'the workflow produced no output file' }
        if ($meshes.Count -eq 0) { Write-Warn "$n`: no mesh among the outputs; kept what there was" }
        $t1 = Get-Utc
        $backend = [ordered]@{
          comfyui = $system; workflow = $wf.Name; workflow_sha256 = $wf.Sha256; workflow_path = $wf.File
          models = $models; overrides = $ov; prompt_id = $run.PromptId; uploaded_as = $uploaded
          server_files = @($files | ForEach-Object { ("$($_.subfolder)/$($_.filename)").TrimStart('/') }); api_prompt = $g.Api
        }
        $derived = @(); if ($j.Input.asset_id) { $derived += $j.Input.asset_id } else { $derived += ConvertTo-Id128 $j.Input.sha256 }
        $side = New-Sidecar -Name $n -Kind 'mesh' -Service $service -Generator "comfyui/$($wf.Name)" -ModelId $(if ($models.Count) { $models[0].file } else { $null }) `
          -ModelVersion $null -Prompt (Get-UpstreamPrompt $j.Input) -SeedValue $j.Seed -Parameters (Get-OtherParameters $g) -Backend $backend `
          -Inputs @(New-InputRecord $j.Input) -Outputs $outputs -Credits 0 -Started $t0 -Finished $t1 -SpecHash $j.Spec -DerivedFrom $derived
        Write-JsonFile (Get-SidecarPath $dir $n) $side
        Write-Log ("  done {0}: {1:N1} s" -f $n, ($t1 - $t0).TotalSeconds)
        $results += [pscustomobject]@{ name = $n; status = 'generated'; path = (Join-Path $dir $outputs[0].path); sidecar = (Get-SidecarPath $dir $n) }
      } catch {
        Write-Warn "$n`: $($_.Exception.Message)"
        $results += [pscustomobject]@{ name = $n; status = 'failed'; error = $_.Exception.Message }
      }
    }
  } finally {
    if (-not $NoFree) { Invoke-ComfyFree }
    Exit-GpuLock
  }
  return , @($results)
}

# ---- stage: text to mesh in one ComfyUI workflow (comfyui-3d: TRELLIS.2 or Pixal3D) -----------------
#
# The owner's workflow (krea2-turbo-to-trellis2-or-pixal3d.json) is the whole chain in one graph:
# Krea 2 turbo draws the image from the prompt, the background is removed and the object cropped,
# TRELLIS.2 — or Pixal3D, by one boolean — reconstructs it, and a post-process chain remeshes
# (RemeshMesh), decimates (DecimateMesh), smooths normals, unwraps (UnwrapMesh), bakes the colour,
# normal and occlusion maps and saves a GLB (Save3DAdvanced). The parameters that decide whether
# the engine can use the result are the post-process ones, so those are this backend's flags, and
# everything the workflow also carries is recorded beside them.
#
# **Every node is found by what it is and what it is wired to, never by id**, so a workflow saved
# again with new ids still works: the image sampler is the KSampler whose positive comes from a
# CLIPTextEncode (the four TRELLIS samplers take theirs from the reconstruction stages, keep their
# fixed seeds, and are recorded), the prompt and negative are that sampler's two encoders, the
# size is its latent, the post-process nodes are the one node of each class, the texture size is
# the primitive wired into the unwrap and the bake, and the model is the boolean that drives the
# switches between the two UNET loaders. Each role's id is recorded in the sidecar.
#
# **The segmenter.** UnwrapMesh's `pec` charts on the GPU in seconds; `adaptive` runs on the CPU and
# is superlinear in the number of separate surface pieces: on a palm crown it ran for hours at a
# few percent even at 50,000 faces. It was meant to be the default for props, on the owner's two
# samples (a bare tree with 1,818 islands under `adaptive`, a palm with 27,429 under `pec`), but
# those were two different meshes. On the same geometry — E10's sweep, a crate and a boulder with
# the image and the reconstruction held identical — `adaptive` made more and smaller islands than
# `pec` (crate 1,620 islands of median 26 texels against 762 of 541; boulder 354 of 312 against 237
# of 10,618) and took 30-56x as long; its one advantage was fewer seam vertices. So `pec` is the
# default, `adaptive` is there to ask for, it is never run on foliage, and the unwrap node is
# watched whichever runs: past -SegmenterTimeoutMinutes it is interrupted and the run recorded as
# aborted. The unwrap's wall time is recorded for every run.

$FoliagePattern = '\b(palms?|fronds?|leaf|leaves|leafy|foliage|bush(es)?|shrubs?|grass(es)?|ferns?|needles|pines?|conifers?|firs?|spruces?|vines?|hedges?|flowers?|blossoms?|petals?|reeds?|bamboo|moss|ivy|hay|straw)\b'

function Test-FoliageSubject($s) {
  if ($Foliage) { return 'the -Foliage switch' }
  if ($s.category -and [string]$s.category -match '^(foliage|tree|bush|shrub|grass|flower)$') { return "category '$($s.category)'" }
  $m = [regex]::Match([string]$s.subject, $FoliagePattern, 'IgnoreCase')
  if ($m.Success) { return "the subject says '$($m.Value)'" }
  return $null
}

function Get-LinkFrom($g, [string]$id, [string]$inputName) {
  if (-not $g.Api.Contains($id)) { return $null }
  $v = $g.Api[$id].inputs[$inputName]
  if (Test-IsLink $v) { return [string]$v[0] }
  return $null
}
function Find-ByClass($g, [string]$pattern) { return , @($g.Api.Keys | Where-Object { $g.Api[$_].class_type -match $pattern } | Sort-Object { [long]$_ }) }
# The first of some node ids as a plain string, or $null. Not `| Select-Object -First 1`: that hands
# back a PSObject-wrapped string, and OrderedDictionary.Contains compares string.Equals(PSObject),
# which is false — so the first sweep's sidecars found no bake node in their timing table and
# recorded bake_seconds as null, though every bake node's own time is in `timings.nodes`.
function Get-FirstId($ids) { $a = @($ids | Where-Object { $_ }); if ($a.Count -eq 0) { return $null }; return [string]$a[0].ToString() }

function Get-Comfy3dRoles($g) {
  $r = [ordered]@{}
  $samplers = Find-ByClass $g '^KSampler(Advanced)?$'
  $image = @($samplers | Where-Object { $src = Get-LinkFrom $g $_ 'positive'; $src -and $g.Api[$src].class_type -eq 'CLIPTextEncode' })
  if ($image.Count -ne 1) { throw "comfyui-3d: the workflow needs exactly one sampler whose positive comes from a CLIPTextEncode (the image sampler); it has $($image.Count)" }
  $r.image_sampler = $image[0]
  $r.positive = Get-LinkFrom $g $image[0] 'positive'
  $neg = Get-LinkFrom $g $image[0] 'negative'
  $r.negative = if ($neg -and $g.Api[$neg].class_type -eq 'CLIPTextEncode') { $neg } else { $null }
  $lat = Get-LinkFrom $g $image[0] 'latent_image'
  $r.latent = if ($lat -and $g.Api[$lat].class_type -match '^Empty.*LatentImage$') { $lat } else { $null }
  $r.reconstruction_samplers = @($samplers | Where-Object { $_ -ne $image[0] })
  foreach ($role in @(@('remesh', 'RemeshMesh'), @('decimate', 'DecimateMesh'), @('unwrap', 'UnwrapMesh'))) {
    $ids = Find-ByClass $g "^$($role[1])$"
    if ($ids.Count -ne 1) { throw "comfyui-3d: the workflow needs exactly one $($role[1]) node; it has $($ids.Count)" }
    $r[$role[0]] = $ids[0]
  }
  $saves = Find-ByClass $g '^(Save3DAdvanced|SaveGLB)$'
  if ($saves.Count -ne 1) { throw "comfyui-3d: the workflow needs exactly one Save3DAdvanced (or SaveGLB) node; it has $($saves.Count)" }
  $r.save = $saves[0]
  $r.smooth_normals = Find-ByClass $g '^MeshSmoothNormals$'
  $r.bake_texture = Get-FirstId (Find-ByClass $g '^BakeTextureFromVoxel$')
  $r.bake_normal = Get-FirstId (Find-ByClass $g '^BakeNormalMapFromMesh$')
  $r.bake_occlusion = Get-FirstId (Find-ByClass $g '^BakeAmbientOcclusion$')
  # The texture size: the one primitive wired into the unwrap's atlas resolution and the bake's
  # texture size (the workflow drives both from "Texture Resolution"), else none and both are set.
  $tex = Get-LinkFrom $g $r.unwrap 'resolution'
  if (-not $tex -and $r.bake_texture) { $tex = Get-LinkFrom $g $r.bake_texture 'texture_size' }
  $r.texture_resolution = if ($tex -and $g.Api[$tex].class_type -match '^Primitive(Int|Integer)$') { $tex } else { $null }
  # The model: a PrimitiveBoolean that drives switches, one of which chooses between two UNET loaders.
  $r.model_switch = $null; $r.model_loaders = $null
  $switches = Find-ByClass $g '^ComfySwitchNode$'
  $bools = @($switches | ForEach-Object { Get-LinkFrom $g $_ 'switch' } | Where-Object { $_ -and $g.Api[$_].class_type -eq 'PrimitiveBoolean' } | Sort-Object -Unique)
  if ($bools.Count -gt 1) { throw "comfyui-3d: more than one boolean drives the model switches ($($bools -join ', ')); cannot tell which picks the model" }
  if ($bools.Count -eq 1) {
    $r.model_switch = $bools[0]
    foreach ($s in $switches) {
      if ((Get-LinkFrom $g $s 'switch') -ne $bools[0]) { continue }
      $t = Get-LinkFrom $g $s 'on_true'; $f = Get-LinkFrom $g $s 'on_false'
      if ($t -and $f -and $g.Api[$t].class_type -eq 'UNETLoader' -and $g.Api[$f].class_type -eq 'UNETLoader') { $r.model_loaders = [ordered]@{ on_true = $t; on_false = $f } }
    }
  }
  # What to keep besides the mesh: the image the reconstruction saw, and the atlas render.
  $cond = Find-ByClass $g '^(Trellis2Conditioning|Pixal3DConditioning)$'
  $r.input_preview = Get-FirstId @($cond | ForEach-Object { Get-LinkFrom $g $_ 'image' } | Where-Object { $_ -and $g.Api[$_].class_type -eq 'PreviewImage' } | Sort-Object -Unique)
  $atlas = Find-ByClass $g '^RenderUVAtlas$'
  $r.uv_preview = Get-FirstId @($g.Api.Keys | Where-Object { $g.Api[$_].class_type -eq 'PreviewImage' -and (Get-LinkFrom $g $_ 'images') -in $atlas })
  return $r
}

# Which loader is which model, by its file name: the boolean is set so the requested one is chosen.
function Get-ModelBranch($g, $roles, [string]$wanted) {
  $want = if ($wanted -eq 'pixal3d') { 'pixal' } else { 'trellis' }
  if (-not $roles.model_loaders) { return $null }
  foreach ($branch in @('on_true', 'on_false')) {
    $id = $roles.model_loaders[$branch]
    if ([string]$g.Api[$id].inputs['unet_name'] -match $want) { return [pscustomobject]@{ Value = ($branch -eq 'on_true'); Loader = $id; File = [string]$g.Api[$id].inputs['unet_name'] } }
  }
  throw "comfyui-3d: neither UNET loader the model switch chooses between names a $want model"
}

# Image in (-Images): the picture is given rather than drawn, so a mesh starts from the same bytes
# another service was given (E10 fed Meshy and Tripo the ComfyUI images; TRELLIS.2 has to start
# from those too to be compared like for like). The picture the workflow draws is the VAEDecode of
# the image sampler's latent — found by that wiring, like every other role — and it feeds the
# background removal and the crop. A LoadImage takes its place on every input it fed, and then
# every node that only fed the decode is dropped from the prompt, from the decode backwards, each
# one only once nothing left reads it: the Krea 2 sampler, its encoders and latent, its UNet, CLIP
# and VAE loaders. A node something else still reads (a latent a reconstruction sampler shares) stays.
# Nothing is dropped by class or by id, so a workflow saved again, or one that draws its picture
# another way, either works or is refused with a sentence.
function Set-ImageSource($g, [string]$imageSampler) {
  $decodes = @($g.Api.Keys | Where-Object { $g.Api[$_].class_type -eq 'VAEDecode' -and (Get-LinkFrom $g $_ 'samples') -eq $imageSampler })
  if ($decodes.Count -ne 1) { throw "comfyui-3d -Images: the workflow needs exactly one VAEDecode of the image sampler's latent (the picture it draws, which the given image replaces); it has $($decodes.Count)" }
  $decode = [string]$decodes[0]
  $newId = [string](([long[]]@($g.Api.Keys | ForEach-Object { [long]$_ }) | Measure-Object -Maximum).Maximum + 1)
  $rewired = New-Object System.Collections.Generic.List[object]
  foreach ($id in @($g.Api.Keys)) {
    $node = $g.Api[$id]
    foreach ($k in @($node.inputs.Keys)) {
      $v = $node.inputs[$k]
      if ((Test-IsLink $v) -and [string]$v[0] -eq $decode -and [long]$v[1] -eq 0) {
        $node.inputs[$k] = @($newId, 0)
        $rewired.Add([ordered]@{ node = $id; class = $node.class_type; input = $k })
      }
    }
  }
  if ($rewired.Count -eq 0) { throw "comfyui-3d -Images: nothing reads the image decode (node $decode), so there is nothing for the given image to feed" }
  $g.Api[$newId] = [ordered]@{ class_type = 'LoadImage'; inputs = [ordered]@{ image = 'pending-upload' } }
  $removed = New-Object System.Collections.Generic.List[object]
  $queue = [System.Collections.Generic.Queue[string]]::new()
  $queue.Enqueue($decode)
  while ($queue.Count -gt 0) {
    $id = $queue.Dequeue()
    if (-not $g.Api.Contains($id)) { continue }
    $read = $false
    foreach ($other in @($g.Api.Keys)) {
      foreach ($v in @($g.Api[$other].inputs.Values)) { if ((Test-IsLink $v) -and [string]$v[0] -eq $id) { $read = $true; break } }
      if ($read) { break }
    }
    if ($read) { continue }
    $sources = @($g.Api[$id].inputs.Values | Where-Object { Test-IsLink $_ } | ForEach-Object { [string]$_[0] } | Sort-Object -Unique)
    $removed.Add([ordered]@{ node = $id; class = $g.Api[$id].class_type })
    $g.Api.Remove($id)
    foreach ($s in $sources) { $queue.Enqueue($s) }
  }
  # .ToArray(), never @($list): array-wrapping a generic List throws "Argument types do not match" on
  # PowerShell 7.6 (the Meshy stage met it first; see docs/content-generation.md, the credit rules).
  return [pscustomobject]@{ Node = $newId; Replaced = $decode; Rewired = $rewired.ToArray(); Removed = $removed.ToArray() }
}

# The subject an image stands for, when the picture is given: the -Subjects list's entry of the same
# name (its category and budget), else the image's own sidecar (the subject it was drawn from), else
# the name alone. The foliage rule and the budget read it, as they read a subject in text mode.
function Get-ImageSubjects($inputs) {
  $byName = @{}
  $set = $null
  if ($Subjects) {
    $doc = Read-JsonFile $Subjects
    $set = if ($doc.set) { [string]$doc.set } else { [IO.Path]::GetFileNameWithoutExtension($Subjects) }
    foreach ($s in @($doc.subjects)) { $byName[[string]$s.name] = $s }
  }
  $list = @()
  foreach ($in in $inputs) {
    $l = $byName[$in.name]
    $sd = $in.sidecar_data
    $list += [pscustomobject]@{
      name = $in.name
      subject = $(if ($l) { [string]$l.subject } elseif ($sd -and $sd.prompt) { [string]$sd.prompt.subject } else { $null })
      category = $(if ($l) { [string]$l.category } elseif ($sd -and $sd.subject) { [string]$sd.subject.category } else { $null })
      size_m = $(if ($l) { $l.size_m } elseif ($sd -and $sd.subject) { $sd.subject.size_m } else { $null })
      seed = $null
      set = $(if ($l) { $set } elseif ($sd -and $sd.subject) { [string]$sd.subject.set } else { $null })
      input = $in
    }
  }
  return , @($list)
}

# Replace a link with a literal (a flag that overrides what the workflow wires, such as an unwrap
# resolution apart from the texture's); recorded like any override, with the link as the old value.
function Set-NodeInputOverLink($graph, [string]$id, [string]$inputName, $value, $record, [string]$why) {
  $node = $graph.Api[$id]
  $old = if ($node.inputs.Contains($inputName)) { $node.inputs[$inputName] } else { $null }
  $node.inputs[$inputName] = $value
  $record.Add([ordered]@{ node = $id; class = $node.class_type; title = $graph.Titles[$id]; input = $inputName; workflow_value = $old; value = $value; why = $why })
}

# A V3 dynamic combo changed to another option: the old option's inputs go, the new one's arrive at
# their defaults (ComfyUI refuses a prompt that carries the wrong option's inputs).
function Set-DynamicCombo($graph, [string]$id, [string]$inputName, [string]$value, $record, [string]$why) {
  $node = $graph.Api[$id]
  $old = $node.inputs[$inputName]
  if ([string]$old -eq $value) { $record.Add([ordered]@{ node = $id; class = $node.class_type; title = $graph.Titles[$id]; input = $inputName; workflow_value = $old; value = $value; why = $why }); return }
  $def = Get-NodeDef $node.class_type
  $spec = Get-InputSpec $def $inputName
  $pairs = Get-DynamicComboInputs $spec $value
  if ($spec[1].options -and -not (@($spec[1].options) | Where-Object { [string]$_.key -eq $value })) { throw "$($node.class_type).$inputName has no option '$value'" }
  foreach ($k in @($node.inputs.Keys | Where-Object { $_ -like "$inputName.*" })) { $node.inputs.Remove($k) }
  $node.inputs[$inputName] = $value
  foreach ($pair in $pairs) {
    $d = if ($pair[1].Count -gt 1 -and $pair[1][1].PSObject.Properties['default']) { $pair[1][1].default } else { $null }
    $node.inputs["$inputName.$($pair[0])"] = $d
  }
  $record.Add([ordered]@{ node = $id; class = $node.class_type; title = $graph.Titles[$id]; input = $inputName; workflow_value = $old; value = $value; why = "$why (its options' inputs at their defaults)" })
}

function Get-NodeLiterals($g, [string]$id) {
  $o = [ordered]@{}
  if (-not $id -or -not $g.Api.Contains($id)) { return $null }
  foreach ($k in $g.Api[$id].inputs.Keys) {
    if ($k -eq 'viewport_state') { continue }
    $v = $g.Api[$id].inputs[$k]
    $o[$k] = if (Test-IsLink $v) { "<- node $($v[0])" } else { $v }
  }
  return $o
}

# Apply this run's parameters to one copy of the graph. Returns the overrides (each with the
# workflow's value beside the new one), the effective settings read back from the graph, and the
# segmenter decision with its reason.
function Set-Comfy3dOverrides($g, $roles, $s, $promptRec, [long]$seedValue, $imageIn) {
  $ov = New-Object System.Collections.Generic.List[object]
  $foliageWhy = Test-FoliageSubject $s
  $seg = $Segmenter
  $reason = '-Segmenter'
  if ($seg -eq 'adaptive' -and $foliageWhy) {
    throw "comfyui-3d: '$($s.name)' is foliage ($foliageWhy), and the adaptive segmenter is never run on foliage: it is superlinear in separate surface pieces and ran for hours on a palm crown at 50,000 faces. Use -Segmenter pec."
  }
  if (-not $seg) {
    $seg = 'pec'
    $reason = if ($foliageWhy) { "the default, and foliage ($foliageWhy): adaptive is superlinear in separate surface pieces" }
    else { 'the default: on the same geometry pec made fewer and larger islands than adaptive, 30-56x faster (E10, TRELLIS.2 sweep)' }
  }
  $branch = $null
  if ($roles.model_switch) {
    $branch = Get-ModelBranch $g $roles $Model
    if ($branch) { [void](Set-NodeInput $g $roles.model_switch 'value' $branch.Value $ov "model: $Model") }
  } elseif ($Model -ne 'trellis2') { throw 'comfyui-3d: this workflow has no model switch, so -Model pixal3d cannot be honoured' }

  # Text in, the picture is drawn here: prompt, seed and size go to the image sampler. Image in, those
  # nodes are gone (Set-ImageSource) and the picture's own sidecar says how it was drawn.
  if (-not $imageIn) {
    $posField = if ($g.Api[$roles.positive].inputs.Contains('text')) { 'text' } else { 'prompt' }
    [void](Set-NodeInput $g $roles.positive $posField $promptRec.text $ov 'prompt')
    if ($roles.negative) { [void](Set-NodeInput $g $roles.negative 'text' ([string]$promptRec.negative) $ov 'negative prompt') }
    else { $promptRec.negative = $null }
    [void](Set-NodeInput $g $roles.image_sampler 'seed' $seedValue $ov 'image seed')
    if ($roles.latent) {
      [void](Set-NodeInput $g $roles.latent 'width' $(if ($Width -gt 0) { $Width } else { 1024 }) $ov 'image size')
      [void](Set-NodeInput $g $roles.latent 'height' $(if ($Height -gt 0) { $Height } else { 1024 }) $ov 'image size')
    }
  }

  if ($RemeshResolution -gt 0) { [void](Set-NodeInput $g $roles.remesh 'resolution' $RemeshResolution $ov 'remesh resolution') }
  if ($RemeshSignMode) { Set-DynamicCombo $g $roles.remesh 'sign_mode' $RemeshSignMode $ov 'remesh sign mode' }
  if ($RemeshSmoothIters -ge 0) { [void](Set-NodeInput $g $roles.remesh 'smooth_iters' $RemeshSmoothIters $ov 'remesh smoothing iterations') }
  # -Budget: the subject's triangle budget from the -Subjects list (the classes Meshy's remesh and the
  # owner's Tripo run were given), so three generators meet at one budget; else -FaceCount.
  $faces = $FaceCount; $facesWhy = 'face count'; $budgetRec = $null
  if ($Budget) {
    $b = $script:SubjectBudgets[$s.name]
    if (-not $b) { throw "-Budget needs a triangle budget for '$($s.name)': a -Subjects list in which that subject names a 'budget' class under triangle_budgets" }
    $faces = $b.triangles
    $facesWhy = "face count: budget:$($b.class) ($([IO.Path]::GetFileName($Subjects)))"
    $budgetRec = [ordered]@{ class = $b.class; triangles = $b.triangles; from = [IO.Path]::GetFileName($Subjects) }
  }
  [void](Set-NodeInput $g $roles.decimate 'target_face_count' $faces $ov $facesWhy)
  [void](Set-NodeInput $g $roles.unwrap 'segmenter' $seg $ov "segmenter: $reason")
  if ($UnwrapPadding -ge 0) { [void](Set-NodeInput $g $roles.unwrap 'padding' $UnwrapPadding $ov 'unwrap padding') }
  if ($WeldDistance -ge 0) { [void](Set-NodeInput $g $roles.unwrap 'weld_distance' $WeldDistance $ov 'unwrap weld distance') }
  if ($roles.texture_resolution) { [void](Set-NodeInput $g $roles.texture_resolution 'value' $TextureResolution $ov 'texture resolution') }
  else {
    [void](Set-NodeInput $g $roles.unwrap 'resolution' $TextureResolution $ov 'texture resolution')
    if ($roles.bake_texture) { [void](Set-NodeInput $g $roles.bake_texture 'texture_size' $TextureResolution $ov 'texture resolution') }
  }
  if ($UnwrapResolution -gt 0) { Set-NodeInputOverLink $g $roles.unwrap 'resolution' $UnwrapResolution $ov 'unwrap resolution, apart from the texture''s' }
  $prefix = $OutputPrefix.Replace('{date}', $Date).Replace('{name}', $s.name)
  [void](Set-NodeInput $g $roles.save 'filename_prefix' $prefix $ov 'server file prefix')
  foreach ($x in @($Set)) {
    if (-not $x) { continue }
    if ($x -notmatch '^(?<key>[^=]+)\.(?<input>[^.=]+)=(?<value>.*)$') { throw "-Set '$x' is not <title|class|#id>.<input>=<value>" }
    $ids = @(Find-Nodes $g $Matches.key)
    if ($ids.Count -eq 0) { throw "-Set '$x': no node titled or classed '$($Matches.key)'" }
    $value = ConvertFrom-SetValue $Matches.value
    foreach ($id in $ids) { if (-not (Set-NodeInput $g $id $Matches.input $value $ov '-Set')) { throw "-Set '$x': that input of node $id is driven by a link" } }
  }

  # The effective settings, read back from the graph: what the flags set and what the workflow kept.
  $texValue = if ($roles.texture_resolution) { $g.Api[$roles.texture_resolution].inputs['value'] } else { $TextureResolution }
  $unwrapRes = $g.Api[$roles.unwrap].inputs['resolution']; if (Test-IsLink $unwrapRes) { $unwrapRes = $texValue }
  $imageRec = if ($imageIn) {
    $in = $s.input
    [ordered]@{
      supplied = $true; path = $in.path; sha256 = $in.sha256; asset_id = $in.asset_id; provenance = $in.sidecar
      uploaded_as = $null   # set when it is uploaded, just before the run
      load_node = $imageIn.Node; replaced_node = $imageIn.Replaced; rewired = $imageIn.Rewired; removed_nodes = $imageIn.Removed
    }
  } else { [ordered]@{ supplied = $false; sampler = Get-NodeLiterals $g $roles.image_sampler; latent = Get-NodeLiterals $g $roles.latent } }
  $settings = [ordered]@{
    model = $(if ($branch) { [ordered]@{ name = $Model; file = $branch.File; switch_value = $branch.Value } } else { [ordered]@{ name = $Model; file = $null } })
    image = $imageRec
    face_budget = $budgetRec
    reconstruction_samplers = [ordered]@{}
    remesh = Get-NodeLiterals $g $roles.remesh
    decimate = Get-NodeLiterals $g $roles.decimate
    smooth_normals = @($roles.smooth_normals | ForEach-Object { [ordered]@{ node = $_; crease_angle = $g.Api[$_].inputs['crease_angle'] } })
    unwrap = Get-NodeLiterals $g $roles.unwrap
    unwrap_effective_resolution = $unwrapRes
    segmenter = [ordered]@{ value = $seg; reason = $reason; foliage = $foliageWhy; timeout_minutes = $SegmenterTimeoutMinutes }
    texture_resolution = $texValue
    bake_texture = Get-NodeLiterals $g $roles.bake_texture
    bake_normal = Get-NodeLiterals $g $roles.bake_normal
    bake_occlusion = Get-NodeLiterals $g $roles.bake_occlusion
    output_prefix = $prefix
  }
  foreach ($id in $roles.reconstruction_samplers) { $settings.reconstruction_samplers[$id] = Get-NodeLiterals $g $id }
  return [pscustomobject]@{ Overrides = $ov; Settings = $settings; Segmenter = $seg; Foliage = $foliageWhy; Model = $branch }
}

# ---- running one prompt and watching it -----------------------------------------------------------
#
# /history says when a prompt finished and what it wrote, but not which node ran when, and the
# unwrap's wall time and the segmenter timeout both need exactly that. ComfyUI's websocket says it:
# `executing` names each node as it starts (so a node's time is the gap to the next), `progress`
# carries a node's own progress, `execution_cached` lists the nodes it reused. The socket is read
# with one receive pending at a time and polled with a short wait, because cancelling a receive
# aborts a .NET ClientWebSocket. Without a socket the run still works, from /history alone, with
# no per-node times and only the overall limit.

function Get-MachineSample {
  $s = [ordered]@{ utc = Format-Utc (Get-Utc); cpu_pct = $null; gpu_util_pct = $null; gpu_memory_used_mib = $null; gpu_memory_total_mib = $null }
  if ($IsWin) { try { $s.cpu_pct = [int](@(Get-CimInstance -ClassName Win32_Processor -ErrorAction Stop) | Measure-Object -Property LoadPercentage -Average).Average } catch { } }
  try {
    $line = @(& nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total --format=csv,noheader,nounits 2>$null)[0]
    if ($line) { $p = $line -split ',\s*'; $s.gpu_util_pct = [int]$p[0]; $s.gpu_memory_used_mib = [int]$p[1]; $s.gpu_memory_total_mib = [int]$p[2] }
  } catch { }
  return $s
}

function Get-ComfyProcess {
  if (-not $IsWin) { return $null }
  try {
    $port = ([uri]$Url).Port
    $conn = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction Stop | Select-Object -First 1
    return Get-Process -Id $conn.OwningProcess -ErrorAction Stop
  } catch { return $null }
}

function Get-LockLine { try { return ((& $LockTool status *>&1 | Out-String).Trim()) } catch { return 'unknown' } }

function Invoke-ComfyWatched($api, [string]$watchNode, [double]$watchMinutes, [double]$overallMinutes, $classOf) {
  $t = [ordered]@{
    source = 'websocket'; prompt_id = $null; status = $null; error = $null
    queued_utc = $null; started_utc = $null; finished_utc = $null
    nodes = [ordered]@{}; order = New-Object System.Collections.Generic.List[string]; cached = @()
    watch = [ordered]@{ node = $watchNode; seconds = $null; progress = $null; limit_minutes = $watchMinutes; aborted = $false }
    samples = New-Object System.Collections.Generic.List[object]
    comfyui_cpu_seconds = $null; gpu_lock_at_start = Get-LockLine; gpu_lock_at_end = $null
  }
  $proc = Get-ComfyProcess
  $cpu0 = if ($proc) { $proc.TotalProcessorTime.TotalSeconds } else { $null }
  $t.samples.Add((Get-MachineSample))
  $ws = $null
  try {
    $ws = [System.Net.WebSockets.ClientWebSocket]::new()
    $wsUri = [uri](($Url.TrimEnd('/') -replace '^http', 'ws') + "/ws?clientId=$($script:ClientId)")
    if (-not $ws.ConnectAsync($wsUri, [Threading.CancellationToken]::None).Wait(10000)) { throw 'no answer in 10 s' }
  } catch {
    Write-Warn "comfyui: no websocket ($($_.Exception.Message)); no per-node times, and only the overall limit applies"
    if ($ws) { $ws.Dispose() }; $ws = $null; $t.source = 'history'
  }
  $body = @{ prompt = $api; client_id = $script:ClientId } | ConvertTo-Json -Depth 50 -Compress
  try {
    $queued = Invoke-RestMethod -Uri "$Url/prompt" -Method Post -Body ([Text.Encoding]::UTF8.GetBytes($body)) -ContentType 'application/json; charset=utf-8' -TimeoutSec 60
  } catch {
    if ($ws) { $ws.Dispose() }
    $detail = if ($_.ErrorDetails -and $_.ErrorDetails.Message) { $_.ErrorDetails.Message } else { $_.Exception.Message }
    throw "ComfyUI refused the prompt: $detail"
  }
  if ($queued.node_errors -and @($queued.node_errors.PSObject.Properties).Count -gt 0) {
    if ($ws) { $ws.Dispose() }
    throw ("ComfyUI node errors: " + ($queued.node_errors | ConvertTo-Json -Depth 8 -Compress))
  }
  $promptId = [string]$queued.prompt_id
  $t.prompt_id = $promptId
  $t.queued_utc = Get-Utc
  $deadline = $t.queued_utc.AddMinutes($overallMinutes)
  $buffer = [byte[]]::new(65536)
  $segment = [ArraySegment[byte]]::new($buffer)
  $acc = [IO.MemoryStream]::new()
  $pending = $null
  $current = $null; $currentStart = $null
  $lastCheck = [DateTime]::MinValue; $lastSample = Get-Utc
  $done = $null
  try {
    while (-not $done) {
      $text = $null
      if ($ws -and $ws.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
        try {
          if (-not $pending) { $pending = $ws.ReceiveAsync($segment, [Threading.CancellationToken]::None) }
          if ($pending.Wait(1000)) {
            $res = $pending.Result; $pending = $null
            if ($res.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { $ws.Dispose(); $ws = $null; $t.source = 'websocket, then history' }
            else {
              $acc.Write($buffer, 0, $res.Count)
              if ($res.EndOfMessage) {
                if ($res.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Text) { $text = [Text.Encoding]::UTF8.GetString($acc.ToArray()) }
                $acc.SetLength(0)   # binary frames are live previews: dropped
              }
            }
          }
        } catch { Write-Warn "comfyui: the websocket failed ($($_.Exception.Message)); continuing from /history"; $pending = $null; try { $ws.Dispose() } catch { }; $ws = $null; $t.source = 'websocket, then history' }
      } else { Start-Sleep -Seconds 2 }
      $now = Get-Utc
      if ($text) {
        $m = $null; try { $m = $text | ConvertFrom-Json } catch { }
        $d = if ($m) { $m.data } else { $null }
        if ($d -and $d.PSObject.Properties['prompt_id'] -and [string]$d.prompt_id -eq $promptId) {
          switch ([string]$m.type) {
            'execution_start' { $t.started_utc = $now }
            'execution_cached' { $t.cached = @($d.nodes | ForEach-Object { [string]$_ }) }
            'executing' {
              if ($current) { $t.nodes[$current] = [math]::Round(($now - $currentStart).TotalSeconds, 2) }
              $current = if ($null -ne $d.node) { [string]$d.node } else { $null }
              $currentStart = $now
              if ($current) { $t.order.Add($current) } else { if (-not $done) { $done = 'success' } }
            }
            'progress' { if ([string]$d.node -eq $watchNode) { $t.watch.progress = "$($d.value)/$($d.max)" } }
            'execution_success' { if ($current) { $t.nodes[$current] = [math]::Round(($now - $currentStart).TotalSeconds, 2); $current = $null }; $done = 'success' }
            'execution_interrupted' { $done = 'interrupted' }
            'execution_error' { $t.error = "node $($d.node_id) ($($d.node_type)): $($d.exception_message)"; $done = 'error' }
          }
        }
      }
      # The watched node (the unwrap): interrupted past its limit, and the run recorded as aborted.
      if (-not $done -and $watchNode -and $current -eq $watchNode -and -not $t.watch.aborted -and ($now - $currentStart).TotalMinutes -gt $watchMinutes) {
        Write-Warn ("comfyui: {0} ({1}) has run {2:N1} min, over the {3} min limit (progress {4}); interrupting" -f $watchNode, $classOf[$watchNode], ($now - $currentStart).TotalMinutes, $watchMinutes, $t.watch.progress)
        $t.watch.aborted = $true
        $t.nodes[$current] = [math]::Round(($now - $currentStart).TotalSeconds, 2)
        try { Invoke-RestMethod -Uri "$Url/interrupt" -Method Post -Body (@{ prompt_id = $promptId } | ConvertTo-Json -Compress) -ContentType 'application/json' -TimeoutSec 30 | Out-Null } catch { Write-Warn "comfyui: /interrupt failed: $($_.Exception.Message)" }
      }
      if (-not $done -and $now -gt $deadline -and -not $t.watch.aborted) {
        Write-Warn "comfyui: prompt $promptId still running after $overallMinutes min; interrupting"
        $t.error = "over the overall limit of $overallMinutes minutes (-TimeoutMinutes)"
        try { Invoke-RestMethod -Uri "$Url/interrupt" -Method Post -Body (@{ prompt_id = $promptId } | ConvertTo-Json -Compress) -ContentType 'application/json' -TimeoutSec 30 | Out-Null } catch { }
        $deadline = $now.AddMinutes(5)   # give the interrupt time to land before giving up on it
        $t.watch.aborted = $true
      }
      if (($now - $lastSample).TotalSeconds -ge 15) { $t.samples.Add((Get-MachineSample)); $lastSample = $now; Update-GpuLock }
      # /history: the fallback without a socket, and the authority on completion with one.
      if (-not $done -and ($now - $lastCheck).TotalSeconds -ge $(if ($ws) { 15 } else { 2 })) {
        $lastCheck = $now
        try {
          $h = Invoke-RestMethod -Uri "$Url/history/$promptId" -TimeoutSec 30
          if ($h.PSObject.Properties[$promptId]) {
            $st = $h.$promptId.status
            if ($st.completed) { $done = 'success' }
            elseif ($st.status_str -eq 'error') {
              $interrupted = @($st.messages | Where-Object { $_[0] -eq 'execution_interrupted' }).Count -gt 0
              $done = if ($interrupted) { 'interrupted' } else { 'error' }
              if (-not $interrupted -and -not $t.error) { $t.error = (@($st.messages | Where-Object { $_[0] -eq 'execution_error' } | ForEach-Object { "node $($_[1].node_id) ($($_[1].node_type)): $($_[1].exception_message)" }) -join '; ') }
            }
          }
        } catch { }
        if (-not $done -and $now -gt $deadline.AddMinutes(10)) { $done = 'error'; $t.error = 'the prompt never finished, even after an interrupt' }
      }
    }
  } finally {
    if ($ws) { try { $ws.Abort() } catch { }; $ws.Dispose() }
    $acc.Dispose()
  }
  $t.finished_utc = Get-Utc
  $t.status = if ($t.watch.aborted) { 'aborted' } else { $done }
  if ($t.watch.node -and $t.nodes.Contains($t.watch.node)) { $t.watch.seconds = $t.nodes[$t.watch.node] }
  elseif ($t.watch.node -and $t.cached -contains $t.watch.node) { $t.watch.seconds = 0 }
  $t.samples.Add((Get-MachineSample))
  if ($proc) { try { $proc.Refresh(); $t.comfyui_cpu_seconds = [math]::Round($proc.TotalProcessorTime.TotalSeconds - $cpu0, 1) } catch { } }
  $t.gpu_lock_at_end = Get-LockLine
  $hist = $null
  try { $h = Invoke-RestMethod -Uri "$Url/history/$promptId" -TimeoutSec 60; if ($h.PSObject.Properties[$promptId]) { $hist = $h.$promptId } } catch { }
  return [pscustomobject]@{ Track = $t; History = $hist }
}

# The machine while it ran, in the shape the harness and the bench quote (docs/experiments/README.md).
function Get-MachineSummary($t) {
  function Span($field) {
    $v = @($t.samples | ForEach-Object { $_[$field] } | Where-Object { $null -ne $_ } | ForEach-Object { [double]$_ })
    if ($v.Count -eq 0) { return $null }
    return [ordered]@{ min = ($v | Measure-Object -Minimum).Minimum; max = ($v | Measure-Object -Maximum).Maximum; mean = [math]::Round(($v | Measure-Object -Average).Average, 1) }
  }
  $wall = if ($t.started_utc -and $t.finished_utc) { ($t.finished_utc - $t.started_utc).TotalSeconds } else { $null }
  return [ordered]@{
    samples = $t.samples.Count
    cpu_pct = Span 'cpu_pct'
    gpu_util_pct = Span 'gpu_util_pct'
    gpu_memory_used_mib = Span 'gpu_memory_used_mib'
    gpu_memory_total_mib = $(if ($t.samples.Count) { $t.samples[0]['gpu_memory_total_mib'] } else { $null })
    comfyui_cpu_seconds = $t.comfyui_cpu_seconds
    comfyui_cores_busy = $(if ($t.comfyui_cpu_seconds -and $wall) { [math]::Round($t.comfyui_cpu_seconds / $wall, 2) } else { $null })
    gpu_lock_at_start = $t.gpu_lock_at_start
    gpu_lock_at_end = $t.gpu_lock_at_end
  }
}

function Get-TimingSummary($t, $roles, $classOf) {
  $sec = { param($id) if ($id -and $t.nodes.Contains($id)) { $t.nodes[$id] } elseif ($id -and $t.cached -contains $id) { 0 } else { $null } }
  $execution = if ($t.started_utc -and $t.finished_utc) { [math]::Round(($t.finished_utc - $t.started_utc).TotalSeconds, 1) } else { $null }
  $bake = @(@($roles.bake_texture, $roles.bake_normal, $roles.bake_occlusion) | Where-Object { $_ } | ForEach-Object { & $sec $_ } | Where-Object { $null -ne $_ })
  return [ordered]@{
    source = $t.source
    queued_utc = $(if ($t.queued_utc) { Format-Utc $t.queued_utc } else { $null })
    started_utc = $(if ($t.started_utc) { Format-Utc $t.started_utc } else { $null })
    finished_utc = Format-Utc $t.finished_utc
    queue_wait_seconds = $(if ($t.started_utc) { [math]::Round(($t.started_utc - $t.queued_utc).TotalSeconds, 1) } else { $null })
    execution_seconds = $execution
    wall_seconds = [math]::Round(($t.finished_utc - $t.queued_utc).TotalSeconds, 1)
    image_sampler_seconds = & $sec $roles.image_sampler
    reconstruction_seconds = $(if ($roles.reconstruction_samplers) { $v = @($roles.reconstruction_samplers | ForEach-Object { & $sec $_ } | Where-Object { $null -ne $_ }); if ($v.Count) { [math]::Round(($v | Measure-Object -Sum).Sum, 1) } else { $null } } else { $null })
    remesh_seconds = & $sec $roles.remesh
    decimate_seconds = & $sec $roles.decimate
    unwrap_seconds = & $sec $roles.unwrap
    unwrap_progress = $t.watch.progress
    bake_seconds = $(if ($bake.Count) { [math]::Round(($bake | Measure-Object -Sum).Sum, 1) } else { $null })
    cached_nodes = @($t.cached)
    nodes = @($t.order | Select-Object -Unique | ForEach-Object { [ordered]@{ node = $_; class = $classOf[$_]; seconds = $t.nodes[$_] } })
  }
}

# The GLB the save node wrote: from /history, fetched through /view like every other output; else,
# when a node reports its file somewhere Get-ComfyOutputs does not look, from the output directory —
# but only under this run's own prefix, never the workflow's shared `3d/ComfyUI` folder, where the
# owner's own files live.
function Find-SavedMesh($hist, $roles, [string]$prefix, [DateTime]$since) {
  $files = @(Get-ComfyOutputs $hist)
  $mesh = @($files | Where-Object { $_.node -eq $roles.save -and $_.filename -match '\.(glb|gltf)$' }) | Select-Object -First 1
  if (-not $mesh) { $mesh = @($files | Where-Object { $_.filename -match '\.glb$' -and $_.type -eq 'output' }) | Select-Object -First 1 }
  if ($mesh) { return [pscustomobject]@{ Remote = $mesh; Local = $null } }
  if (-not $OutputRoot -or $prefix -notmatch '/' -or $prefix -match '^3d/ComfyUI$') { return $null }
  $folder = Join-Path $OutputRoot (Split-Path -Parent $prefix)
  $stem = Split-Path -Leaf $prefix
  if (-not (Test-Path -LiteralPath $folder)) { return $null }
  $local = @(Get-ChildItem -LiteralPath $folder -File -Filter "$stem*.glb" | Where-Object { $_.LastWriteTimeUtc -ge $since.AddSeconds(-5) } | Sort-Object LastWriteTimeUtc -Descending) | Select-Object -First 1
  if ($local) { return [pscustomobject]@{ Remote = $null; Local = $local.FullName } }
  return $null
}

function Invoke-ComfyText3dStage {
  $imageMode = [bool]$Images
  if ($Budget -and -not $Subjects) { throw '-Budget takes each subject''s triangle budget from a -Subjects list; give one' }
  if ($Budget -and $script:FaceCountGiven) { throw '-Budget and -FaceCount both name the face count; give one' }
  $script:SubjectBudgets = if ($Budget) { Get-SubjectBudgets } else { @{} }
  # Text in, the subjects come from -Subjects or -Subject; image in, from the images (-Images, -Only),
  # each matched to its -Subjects entry by name for the category, the budget and the foliage rule.
  # Not `$subjects`: a local of that name is -Subjects to every function this one calls (PowerShell
  # names ignore case and resolve through dynamic scope), and the budget's record reads -Subjects.
  $subjectList = if ($imageMode) { Get-ImageSubjects (Get-ImageInputs) } else { Get-SubjectList }
  $wf = Get-WorkflowGraph $Workflow
  $roles = Get-Comfy3dRoles $wf.Graph
  $classOf = @{}; foreach ($id in $wf.Graph.Api.Keys) { $classOf[$id] = $wf.Graph.Api[$id].class_type }
  if ($imageMode) {
    # The drawing nodes are dropped from every copy of the graph; the roles that named them name
    # nothing now, and the image node is new (the same id in every copy: one past the largest).
    $imageSampler = $roles.image_sampler
    $probe = Set-ImageSource (Copy-Graph $wf.Graph) $imageSampler
    $classOf[$probe.Node] = 'LoadImage'
    foreach ($k in @('image_sampler', 'positive', 'negative', 'latent')) { $roles[$k] = $null }
    $roles.image_source = $probe.Node
  }
  $service = if ($Model -eq 'pixal3d') { 'pixal3d' } else { 'trellis' }
  $dir = Get-OutputDir $service
  $plan = @()
  foreach ($s in $subjectList) {
    $g = Copy-Graph $wf.Graph
    if ($imageMode) {
      $imageIn = Set-ImageSource $g $imageSampler
      $p = Get-UpstreamPrompt $s.input
      $sd = if ($s.input.sidecar_data -and $null -ne $s.input.sidecar_data.seed) { [long]$s.input.sidecar_data.seed } else { $null }
      $applied = Set-Comfy3dOverrides $g $roles $s $p 0 $imageIn
      # What makes two runs the same run: the graph as sent, and the picture's bytes (the image
      # node carries a placeholder until the upload names the file on the server).
      $spec = Get-SpecHash $wf $g "image:$($s.input.sha256)"
    } else {
      $imageIn = $null
      $p = Get-PromptFor $s
      $sd = Get-SeedFor $s
      $applied = Set-Comfy3dOverrides $g $roles $s $p $sd $null
      $spec = Get-SpecHash $wf $g ''
    }
    $state = Test-Existing (Get-SidecarPath $dir $s.name) $spec "mesh '$($s.name)'"
    $plan += [pscustomobject]@{ Subject = $s; Graph = $g; Prompt = $p; Seed = $sd; Applied = $applied; Spec = $spec; State = $state; ImageIn = $imageIn }
  }
  $todo = @($plan | Where-Object State -eq 'new')
  Write-Step "comfyui-3d ($service$(if ($imageMode) { ', images in' })): $($plan.Count) meshes through $($wf.Name): $($todo.Count) to run, $($plan.Count - $todo.Count) already made from the same spec"
  Write-Note "  roles: $((@($roles.Keys | Where-Object { $roles[$_] -and $roles[$_] -isnot [System.Collections.IDictionary] -and $roles[$_] -isnot [array] } | ForEach-Object { "$_=$($roles[$_])" })) -join ' ')"
  if ($imageMode) { Write-Note "  image in: node $($probe.Replaced) ($($classOf[$probe.Replaced])) replaced by LoadImage $($probe.Node) on $(@($probe.Rewired | ForEach-Object { "$($_.class).$($_.input)" }) -join ', '); dropped $(@($probe.Removed | ForEach-Object { "$($_.node) $($_.class)" }) -join ', ')" }
  Write-Note "  into $dir"
  if ($DryRun) {
    return , @($plan | ForEach-Object {
        [pscustomobject][ordered]@{ name = $_.Subject.name; status = "dry-run:$($_.State)"; service = $service; seed = $_.Seed; segmenter = $_.Applied.Segmenter; foliage = $_.Applied.Foliage
          prompt = $_.Prompt; settings = $_.Applied.Settings; overrides = $_.Applied.Overrides; roles = $roles; spec_hash = $_.Spec
          graph = $(if ($imageMode) { $_.Graph.Api } else { $null }) }
      })
  }
  $results = @($plan | Where-Object State -eq 'cached' | ForEach-Object { [pscustomobject]@{ name = $_.Subject.name; status = 'cached'; path = (Join-Path $dir "$($_.Subject.name).glb"); sidecar = (Get-SidecarPath $dir $_.Subject.name) } })
  if ($todo.Count -eq 0) { return , @($results) }
  $system = Get-ComfySystem
  $models = Get-ModelFiles $wf.Graph
  $runs = Join-Path $dir 'runs.jsonl'
  Enter-GpuLock "comfyui-3d: $($todo.Count) meshes$(if ($imageMode) { ' from given images' }), $($wf.Name)"
  try {
    $i = 0
    foreach ($j in $todo) {
      $i++
      Update-GpuLock
      $n = $j.Subject.name
      Write-Log ("  [{0}/{1}] {2}: {3}, {4} faces, segmenter {5}, smooth {6}, weld {7}, texture {8}" -f $i, $todo.Count, $n, $(if ($imageMode) { "image $([IO.Path]::GetFileName($j.Subject.input.path))" } else { "seed $($j.Seed)" }),
          $j.Applied.Settings.decimate.target_face_count, $j.Applied.Segmenter, $j.Applied.Settings.remesh.smooth_iters, $j.Applied.Settings.unwrap.weld_distance, $j.Applied.Settings.texture_resolution)
      $t0 = Get-Utc
      try {
        if ($j.ImageIn) {
          # Uploaded now, inside the lock, as the image stage's inputs are; the name the server
          # gives it is recorded, and the spec was already fixed by the picture's bytes.
          $uploaded = Send-ComfyImage $j.Subject.input.path
          $j.Graph.Api[$j.ImageIn.Node].inputs['image'] = $uploaded
          $j.Applied.Overrides.Add([ordered]@{ node = $j.ImageIn.Node; class = 'LoadImage'; title = $null; input = 'image'; workflow_value = $null; value = $uploaded
              why = "input image, supplied (-Images): replaces node $($j.ImageIn.Replaced), the picture the workflow draws" })
          $j.Applied.Settings.image.uploaded_as = $uploaded
        }
        $run = Invoke-ComfyWatched $j.Graph.Api $roles.unwrap $SegmenterTimeoutMinutes $TimeoutMinutes $classOf
        $tr = $run.Track
        $timings = Get-TimingSummary $tr $roles $classOf
        $machine = Get-MachineSummary $tr
        $t1 = Get-Utc
        $modelFile = if ($j.Applied.Model) { $j.Applied.Model.File } else { @($models | Where-Object { $_.class -eq 'UNETLoader' } | Select-Object -First 1).file }
        $backend = [ordered]@{
          comfyui = $system; workflow = $wf.Name; workflow_sha256 = $wf.Sha256; workflow_path = $wf.File
          models = $models; model_used = $modelFile; roles = $roles; overrides = $j.Applied.Overrides; prompt_id = $tr.prompt_id
          server_files = @(); api_prompt = $j.Graph.Api
        }
        $line = [ordered]@{ utc = Format-Utc $t1; name = $n; status = $tr.status; spec_hash = $j.Spec; seed = $j.Seed; model = $Model
          face_count = $j.Applied.Settings.decimate.target_face_count; segmenter = $j.Applied.Segmenter; smooth_iters = $j.Applied.Settings.remesh.smooth_iters
          weld_distance = $j.Applied.Settings.unwrap.weld_distance; texture = $j.Applied.Settings.texture_resolution
          wall_seconds = $timings.wall_seconds; unwrap_seconds = $timings.unwrap_seconds; unwrap_progress = $timings.unwrap_progress; cached_nodes = @($tr.cached).Count
          cpu_pct = $machine.cpu_pct; gpu_util_pct = $machine.gpu_util_pct; comfyui_cores_busy = $machine.comfyui_cores_busy }
        if ($j.ImageIn) { $line.image_sha256 = $j.Subject.input.sha256 }
        if ($tr.status -ne 'success') {
          # No mesh, so no provenance sidecar: the attempt is recorded beside where it would have
          # been, with everything a sidecar would carry, so an abort is a result and not a gap.
          $record = [ordered]@{
            schema = 'engine.generation.attempt/1'; name = $n; kind = 'mesh'; service = $service; status = $tr.status; spec_hash = $j.Spec
            reason = $(if ($tr.watch.aborted -and -not $tr.error) { "the unwrap ($($j.Applied.Segmenter) segmenter) ran past -SegmenterTimeoutMinutes $SegmenterTimeoutMinutes and was interrupted at progress $($tr.watch.progress)" } else { $tr.error })
            prompt = $j.Prompt; seed = $j.Seed; parameters = $j.Applied.Settings; backend = $backend; timings = $timings; machine_state = $machine
            operator = $Operator; started_utc = Format-Utc $t0; finished_utc = Format-Utc $t1; tool = Get-ToolInfo
          }
          $recordFile = Join-Path $dir "$n.$($tr.status).json"
          Write-JsonFile $recordFile $record
          $line.reason = $record.reason
          Add-JsonLine $runs $line
          Write-Warn "$n`: $($tr.status): $($record.reason) (recorded in $recordFile)"
          $results += [pscustomobject]@{ name = $n; status = $(if ($tr.status -eq 'aborted') { 'aborted' } else { 'failed' }); error = $record.reason; record = $recordFile; unwrap_seconds = $timings.unwrap_seconds; wall_seconds = $timings.wall_seconds }
          continue
        }
        $found = Find-SavedMesh $run.History $roles $j.Applied.Settings.output_prefix $t0
        if (-not $found) { throw 'the run succeeded but no GLB was found, in /history or under the output prefix' }
        $glb = Join-Path $dir "$n.glb"
        if ($found.Remote) { Save-ComfyFile $found.Remote $glb; $backend.server_files += ("$($found.Remote.subfolder)/$($found.Remote.filename)").TrimStart('/') }
        else { Copy-Item -LiteralPath $found.Local -Destination $glb -Force; $backend.server_files += $found.Local }
        if (-not (Test-GlbMagic $glb)) { throw "$glb is not a GLB" }
        $outputs = @(New-OutputRecord 'mesh' $glb)
        $files = @(Get-ComfyOutputs $run.History)
        foreach ($keep in @(@('image', $roles.input_preview, 'input-image'), @('uv-atlas', $roles.uv_preview, 'uv-atlas'))) {
          if (-not $keep[1]) { continue }
          $f = @($files | Where-Object { $_.node -eq $keep[1] -and $_.filename -match '\.(png|jpg|jpeg|webp)$' }) | Select-Object -First 1
          if (-not $f) { continue }
          $target = Join-Path $dir ("$n.$($keep[0])" + [IO.Path]::GetExtension($f.filename))
          try { Save-ComfyFile $f $target; $outputs += New-OutputRecord $keep[2] $target } catch { Write-Warn "$n`: could not fetch the $($keep[0]) preview: $($_.Exception.Message)" }
        }
        # Image in, the mesh is derived from the picture it was given: that picture is its input, and its
        # asset id (from its own sidecar, else the first 128 bits of its hash) is `derived_from`, as the
        # Meshy and ComfyUI image-to-3D stages record it. The seed is the picture's, not the mesh's.
        $inRecs = @(); $derived = @()
        if ($j.ImageIn) {
          $inRecs = @(New-InputRecord $j.Subject.input)
          $derived = @($(if ($j.Subject.input.asset_id) { $j.Subject.input.asset_id } else { ConvertTo-Id128 $j.Subject.input.sha256 }))
        }
        $side = New-Sidecar -Name $n -Kind 'mesh' -Service $service -Generator "comfyui/$($wf.Name)" -ModelId $modelFile -ModelVersion $null `
          -Prompt $j.Prompt -SeedValue $(if ($j.ImageIn) { $null } else { $j.Seed }) -Parameters $j.Applied.Settings -Backend $backend -Inputs $inRecs `
          -Outputs $outputs -Credits 0 -Started $t0 -Finished $t1 -SpecHash $j.Spec -DerivedFrom $derived
        $side['timings'] = $timings
        $side['machine_state'] = $machine
        $side['subject'] = [ordered]@{ set = $j.Subject.set; category = $j.Subject.category; size_m = $j.Subject.size_m }
        Write-JsonFile (Get-SidecarPath $dir $n) $side
        $line.glb_bytes = (Get-Item -LiteralPath $glb).Length
        Add-JsonLine $runs $line
        Write-Log ("  done {0}: {1:N1} MB in {2:N0} s (unwrap {3} s, {4} cached nodes)" -f $n, ((Get-Item -LiteralPath $glb).Length / 1MB), $timings.wall_seconds, $timings.unwrap_seconds, @($tr.cached).Count)
        $results += [pscustomobject]@{ name = $n; status = 'generated'; path = $glb; sidecar = (Get-SidecarPath $dir $n); wall_seconds = $timings.wall_seconds; unwrap_seconds = $timings.unwrap_seconds }
      } catch {
        # Where, too: a bare "400 (Bad Request)" from one of a dozen HTTP calls is not a diagnosis.
        Write-Warn "$n`: $($_.Exception.Message) (tools/generate.ps1:$($_.InvocationInfo.ScriptLineNumber): $("$($_.InvocationInfo.Line)".Trim()))"
        $results += [pscustomobject]@{ name = $n; status = 'failed'; error = $_.Exception.Message }
      }
    }
  } finally {
    if (-not $NoFree) { Invoke-ComfyFree }
    Exit-GpuLock
  }
  return , @($results)
}

# ---- stage: Tripo, by hand -------------------------------------------------------------------------
#
# Tripo's Studio credits cannot be spent through its API, so the owner generates by hand. The tool
# writes what to do (a manifest and a README), and ingests what comes back against the manifest,
# so the comparison with the other services uses the same input images byte for byte.

function Get-TripoRoot { Join-Path (Join-Path (Get-LocalRoot) 'generated') 'tripo' }

function Invoke-TripoManifest($inputs) {
  $root = Get-TripoRoot
  $inbox = Join-Path $root 'inbox'
  $file = Join-Path $root "manifest-$Date.json"
  if ((Test-Path -LiteralPath $file) -and -not $Force) { throw "$file exists; -Force to rewrite it (anything the owner recorded in it would be lost)" }
  $entries = @()
  foreach ($in in $inputs) {
    $entries += [ordered]@{
      name = $in.name
      image = $in.path
      image_sha256 = $in.sha256
      image_asset_id = $(if ($in.asset_id) { $in.asset_id } else { ConvertTo-Id128 $in.sha256 })
      subject = $(if ($in.sidecar_data -and $in.sidecar_data.prompt) { [string]$in.sidecar_data.prompt.subject } else { $null })
      save_as = "$($in.name).glb"
      recorded = [ordered]@{ model_version = $null; settings = $null; credits = $null; generated_at = $null; notes = $null }
    }
  }
  $manifest = [ordered]@{
    schema = $ManifestSchema
    created_utc = Format-Utc (Get-Utc)
    date = $Date
    service = 'tripo'
    license = $Licences['tripo'].license
    inbox = $inbox
    settings = [ordered]@{
      mode = 'Image to 3D, one image'
      model_version = 'the newest version Tripo Studio offers; write its name into recorded.model_version'
      texture = 'on (HD texture if offered)'
      pbr = 'on'
      topology = 'triangles; no quad remesh; default face limit'
      export = 'GLB'
    }
    entries = $entries
  }
  if ($DryRun) { return , @([pscustomobject]@{ status = 'dry-run'; manifest = $file; entries = $entries.Count; settings = $manifest.settings }) }
  New-Item -ItemType Directory -Force -Path $inbox | Out-Null
  Write-JsonFile $file $manifest
  $readme = @(
    "# Tripo by hand — manifest-$Date"
    ''
    "Generated by ``tools/generate.ps1 manifest`` on $(Format-Utc (Get-Utc)). Tripo Studio's credits cannot be"
    'spent through its API, so these are made by hand, from exactly the images the other services got.'
    ''
    '1. In Tripo Studio, choose **Image to 3D** with the newest model, texture and PBR on, no quad'
    '   remesh, default face limit.'
    '2. For each row, upload the image and download the result as **GLB** into'
    "   ``$inbox``, named exactly as the last column says."
    "3. If you can, fill in ``recorded`` for each entry in ``manifest-$Date.json`` (model version, anything"
    '   you changed from the defaults, credits spent, notes). Leave it empty rather than guess.'
    '4. Tell the agent; it runs:'
    ''
    "       pwsh tools/generate.ps1 ingest -Backend tripo-folder -Manifest `"$file`""
    ''
    '   which copies each GLB beside a provenance sidecar and leaves the rest alone.'
    ''
    '| # | name | upload this image | save the GLB as |'
    '|---|---|---|---|'
  )
  $i = 0
  foreach ($e in $entries) { $i++; $readme += "| $i | $($e.name) | ``$($e.image)`` | ``$($e.save_as)`` |" }
  [IO.File]::WriteAllText((Join-Path $root 'README.md'), ($readme -join "`n") + "`n", (New-Object Text.UTF8Encoding($false)))
  Write-Step "tripo: manifest of $($entries.Count) entries at $file, instructions in $(Join-Path $root 'README.md'), inbox $inbox"
  return , @([pscustomobject]@{ status = 'written'; manifest = $file; readme = (Join-Path $root 'README.md'); inbox = $inbox; entries = $entries.Count })
}

function Invoke-TripoIngest {
  if (-not $Manifest) { throw 'ingest needs -Manifest <manifest.json>' }
  $m = Read-JsonFile $Manifest
  if ($m.schema -ne $ManifestSchema) { throw "$Manifest is not a $ManifestSchema file" }
  $inbox = if ($From) { $From } else { [string]$m.inbox }
  $dir = Get-OutputDir 'tripo'
  $results = @()
  foreach ($e in @($m.entries)) {
    $src = Join-Path $inbox ([string]$e.save_as)
    if (-not (Test-Path -LiteralPath $src)) { $results += [pscustomobject]@{ name = $e.name; status = 'missing'; expected = $src }; continue }
    if (-not (Test-GlbMagic $src)) { $results += [pscustomobject]@{ name = $e.name; status = 'failed'; error = "$src is not a GLB" }; continue }
    $imageNow = if (Test-Path -LiteralPath $e.image) { Get-FileSha256 $e.image } else { $null }
    if ($imageNow -ne $e.image_sha256) { Write-Warn "$($e.name): the input image changed or moved since the manifest was written; the sidecar records the manifest's hash" }
    $srcSha = Get-FileSha256 $src
    $spec = Get-TextSha256 "$($e.image_sha256)`n$srcSha"
    $side = Get-SidecarPath $dir $e.name
    $state = Test-Existing $side $spec "mesh '$($e.name)'"
    if ($state -eq 'cached') { $results += [pscustomobject]@{ name = $e.name; status = 'cached'; sidecar = $side }; continue }
    if ($DryRun) { $results += [pscustomobject]@{ name = $e.name; status = 'would-ingest'; from = $src }; continue }
    $target = Join-Path $dir "$($e.name).glb"
    Copy-Item -LiteralPath $src -Destination $target -Force
    $made = (Get-Item -LiteralPath $src).LastWriteTimeUtc
    $rec = $e.recorded
    $credits = if ($rec -and $null -ne $rec.credits) { $rec.credits } else { $null }
    $backend = [ordered]@{
      tripo = [ordered]@{ manifest = (Resolve-Path -LiteralPath $Manifest).Path; manifest_settings = $m.settings; recorded = $rec; source_file = $src; source_modified_utc = Format-Utc $made }
    }
    $inRec = [ordered]@{ role = 'image'; path = [string]$e.image; sha256 = [string]$e.image_sha256; asset_id = [string]$e.image_asset_id }
    $upstream = $null
    $imgSide = Join-Path (Split-Path -Parent ([string]$e.image)) ("$([IO.Path]::GetFileNameWithoutExtension([string]$e.image)).provenance.json")
    if (Test-Path -LiteralPath $imgSide) {
      $inRec.provenance = $imgSide
      $upstream = Get-UpstreamPrompt ([pscustomobject]@{ sidecar_data = (Read-JsonFile $imgSide) })
    }
    $sideData = New-Sidecar -Name $e.name -Kind 'mesh' -Service 'tripo' -Generator 'tripo-studio/image-to-3d' -ModelId 'tripo-studio' `
      -ModelVersion $(if ($rec -and $rec.model_version) { [string]$rec.model_version } else { $null }) -Prompt $upstream -SeedValue $null `
      -Parameters $(if ($rec -and $rec.settings) { $rec.settings } else { $m.settings }) -Backend $backend -Inputs @($inRec) `
      -Outputs @(New-OutputRecord 'mesh' $target) -Credits $credits -Started $made -Finished $made -SpecHash $spec -DerivedFrom @([string]$e.image_asset_id)
    Write-JsonFile $side $sideData
    $results += [pscustomobject]@{ name = $e.name; status = 'ingested'; path = $target; sidecar = $side }
  }
  $missing = @($results | Where-Object status -eq 'missing').Count
  Write-Step "tripo: $(@($results | Where-Object status -eq 'ingested').Count) ingested, $(@($results | Where-Object status -eq 'cached').Count) already in, $missing not in the inbox yet"
  return , @($results)
}

# ---- verify ----------------------------------------------------------------------------------------

$AssetProvenanceFields = @('generator', 'model_id', 'model_version', 'prompt_hash', 'seed', 'inputs', 'operator_id', 'created_at_unix_ms',
  'license', 'license_class', 'license_terms_url', 'commercial_ok', 'attribution_required', 'derived_from')

# ---- the sidecar against the generated JSON Schema, failing closed ------------------------------------
#
# Two PowerShell traps sit between a sidecar and the JSON Schema the build generates from
# schemas/provenance.schema, and both fail *open*, so each is handled here rather than trusted:
#
#   - Test-Json answers $true when it cannot parse the *schema* at all (7.4.12 and 7.6.6 alike). The
#     only sign is a non-terminating `InvalidJsonSchema` error, which `-ErrorAction SilentlyContinue`
#     hides. So a result counts as valid only with no error recorded beside it, and a schema that
#     does not parse is thrown — never read as a pass.
#   - PowerShell 7.4's ConvertTo-Json writes a System.Numerics.BigInteger — which ConvertFrom-Json
#     makes of any integer above Int64.MaxValue: the schema's own u64 bound 18446744073709551615, or a
#     sidecar's prompt_hash above 2^63 — as an *object of its properties* ({"IsPowerOfTwo": ...})
#     instead of a number; 7.6 writes the number. A schema round-tripped through the two cmdlets on
#     7.4 stops parsing, and by the first trap every document then "validates". That is how
#     `tools.generate` passed on Windows (7.6.6) and failed in the Linux container (7.4.12, the
#     Dockerfile's pin): the one check that expects a *failure* was the only one that noticed.
#     So neither text is ever round-tripped: the schema is wrapped by splicing text, and a sidecar
#     is validated as the bytes on disk.
function Get-ProvenanceSchemaFile {
  if ($Schema) {
    if (-not (Test-Path -LiteralPath $Schema)) { throw "no JSON Schema at $Schema" }
    return (Resolve-Path -LiteralPath $Schema).Path
  }
  # This checkout's build trees only (never another checkout's, whose schema may be older or newer).
  $found = @(Get-ChildItem -Path (Join-Path $RepoRoot 'build/*/schemas/generated/schemas/json/provenance.schema.json') -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending)
  if ($found.Count -eq 0) { return $null }
  return $found[0].FullName
}

function Get-SidecarSchemaText([string]$schemaFile) {
  $text = [IO.File]::ReadAllText($schemaFile)
  $at = $text.IndexOf('{')
  if ($at -lt 0) { throw "$schemaFile is not a JSON Schema" }
  # The whole sidecar: an object whose `asset_provenance` is exactly engine.content.AssetProvenance.
  # The rest of the sidecar is this tool's own record and is checked by name in Invoke-Verify.
  $wrap = '"type": "object", "required": ["asset_provenance"], "properties": {"asset_provenance": {"$ref": "#/$defs/AssetProvenance"}}, '
  return $text.Substring(0, $at + 1) + $wrap + $text.Substring($at + 1)
}

function Test-JsonStrict([string]$json, [string]$schemaText) {
  $errs = $null
  $ok = Test-Json -Json $json -Schema $schemaText -ErrorAction SilentlyContinue -ErrorVariable errs
  $list = @($errs | Where-Object { $_ })
  $unparsed = @($list | Where-Object { $_.FullyQualifiedErrorId -like 'InvalidJsonSchema,*' })
  if ($unparsed.Count -gt 0) {
    throw "the JSON Schema does not parse under PowerShell $($PSVersionTable.PSVersion) ($($unparsed[0].Exception.Message) $($unparsed[0].Exception.InnerException.Message)); nothing was validated"
  }
  return [pscustomobject]@{ Valid = ([bool]$ok -and $list.Count -eq 0); Errors = @($list | ForEach-Object { $_.Exception.Message }) }
}

function Invoke-Verify {
  $target = if ($Path) { $Path } else { Join-Path (Get-LocalRoot) 'generated' }
  $files = @(if (Test-Path -LiteralPath $target -PathType Container) { Get-ChildItem -LiteralPath $target -Recurse -File -Filter '*.provenance.json' | Sort-Object FullName | ForEach-Object FullName } else { $target })
  $problems = New-Object System.Collections.Generic.List[string]
  $schemaFile = Get-ProvenanceSchemaFile
  $schemaText = if ($schemaFile) { Get-SidecarSchemaText $schemaFile } else { $null }
  if ($schemaFile) { Write-Note "verify: asset_provenance against $schemaFile" }
  else { Write-Warn 'verify: no build tree has generated provenance.schema.json (pass -Schema); checking field names only' }
  foreach ($f in $files) {
    $before = $problems.Count
    if ($schemaText) {
      $v = Test-JsonStrict ([IO.File]::ReadAllText($f)) $schemaText
      foreach ($e in $v.Errors) { $problems.Add("$f`: $e") }
    }
    $s = Read-JsonFile $f
    if ($s.schema -ne $SidecarSchema) { $problems.Add("$f`: schema is '$($s.schema)', not $SidecarSchema"); continue }
    foreach ($k in @('asset_id', 'name', 'kind', 'service', 'asset_provenance', 'outputs', 'license', 'content_class')) {
      if (-not $s.PSObject.Properties[$k]) { $problems.Add("$f`: no '$k'") }
    }
    $ap = $s.asset_provenance
    if ($ap) {
      foreach ($p in $ap.PSObject.Properties.Name) { if ($AssetProvenanceFields -notcontains $p) { $problems.Add("$f`: asset_provenance has '$p', which engine.content.AssetProvenance does not") } }
      foreach ($k in @('generator', 'operator_id', 'license')) { if (-not $ap.$k) { $problems.Add("$f`: asset_provenance.$k is missing") } }
      foreach ($id in @($ap.inputs) + @($ap.derived_from) + @($ap.operator_id)) { if ($id -and $id -notmatch '^[0-9a-f]{32}$') { $problems.Add("$f`: '$id' is not an id128") } }
      if ($ap.license -ne $s.license) { $problems.Add("$f`: the two licence fields disagree") }
    }
    if ($s.content_class -ne 'general') { $problems.Add("$f`: content_class is '$($s.content_class)'; this tool only makes general-audience content") }
    $dir = Split-Path -Parent $f
    foreach ($o in @($s.outputs)) {
      $of = Join-Path $dir $o.path
      if (-not (Test-Path -LiteralPath $of)) { $problems.Add("$f`: output $($o.path) is missing"); continue }
      if ((Get-FileSha256 $of) -ne $o.sha256) { $problems.Add("$f`: output $($o.path) no longer matches its SHA-256") }
    }
    if (@($s.outputs).Count -gt 0 -and $s.asset_id -ne (ConvertTo-Id128 @($s.outputs)[0].sha256)) { $problems.Add("$f`: asset_id is not the first output's content id") }
    $state = if ($problems.Count -eq $before) { 'ok' } else { 'FAIL' }
    Write-Log "  $state  $f"
  }
  Write-Step "verify: $($files.Count) sidecars, $($problems.Count) problems"
  foreach ($p in $problems) { Write-Log "  $p" }
  return , @([pscustomobject]@{ status = $(if ($problems.Count -eq 0) { 'ok' } else { 'failed' }); sidecars = $files.Count; schema = $schemaFile; problems = @($problems) })
}

# ---- dispatch --------------------------------------------------------------------------------------

# Each command yields result objects; they are printed here, one JSON line each, and the exit code
# says whether any of them failed: 0 all well, 1 something failed (the lines say what), and a
# thrown error (bad arguments, a refused spend, a server that is not there) exits 1 with the
# message on stderr before anything is spent.
function Write-Results($results) {
  $failed = $false
  foreach ($r in @($results)) {
    if ($null -eq $r) { continue }
    [Console]::Out.WriteLine(($r | ConvertTo-Json -Depth 30 -Compress))
    if ($r.status -in @('failed', 'timed-out', 'not-submitted', 'aborted')) { $failed = $true }
  }
  if ($failed) { exit 1 }
}

switch ($Command) {
  'prompt' {
    if (-not $Subject) { throw 'prompt needs -Subject <text>' }
    Write-Results @([pscustomobject](Get-PromptFor ([pscustomobject]@{ name = 'x'; subject = $Subject })))
  }
  'convert' {
    $wf = Get-WorkflowGraph $Workflow
    $out = [ordered]@{}
    foreach ($id in $wf.Graph.Api.Keys) {
      $node = $wf.Graph.Api[$id]
      $entry = [ordered]@{ class_type = $node.class_type; inputs = $node.inputs }
      if ($wf.Graph.Titles.ContainsKey($id)) { $entry._meta = [ordered]@{ title = $wf.Graph.Titles[$id] } }
      $out[$id] = $entry
    }
    $out | ConvertTo-Json -Depth 50
  }
  'free' { Invoke-ComfyFree }
  'balance' {
    $b = Get-MeshyBalance
    $ledger = Join-Path (Join-Path (Join-Path (Get-LocalRoot) 'generated') 'meshy') 'ledger.jsonl'
    $spent = 0
    if (Test-Path -LiteralPath $ledger) {
      foreach ($line in (Get-Content -LiteralPath $ledger)) { $e = $line | ConvertFrom-Json; if ($e.event -in 'succeeded', 'failed' -and $e.consumed_credits) { $spent += [int]$e.consumed_credits } }
    }
    Write-Results @([pscustomobject]@{ meshy_balance = $b; ledger = $ledger; ledger_consumed_credits = $spent })
  }
  'image' { Write-Results (Invoke-ImageStage) }
  '3d' {
    if (-not $Backend) { throw '3d needs -Backend meshy|comfyui|comfyui-3d (tripo-folder is `manifest` and `ingest`)' }
    switch ($Backend) {
      'meshy' { Write-Results (Invoke-MeshyStage (Get-ImageInputs)) }
      'comfyui' { Write-Results (Invoke-Comfy3dStage (Get-ImageInputs) $Workflow) }
      'comfyui-3d' { Write-Results (Invoke-ComfyText3dStage) }
      'tripo-folder' { throw 'tripo-folder has no API stage: `manifest` writes the list, `ingest` reads the GLBs back' }
    }
  }
  'manifest' {
    if ($Backend -and $Backend -ne 'tripo-folder') { throw 'manifest is the tripo-folder backend''s' }
    Write-Results (Invoke-TripoManifest (Get-ImageInputs))
  }
  'ingest' {
    if ($Backend -and $Backend -ne 'tripo-folder') { throw 'ingest is the tripo-folder backend''s' }
    Write-Results (Invoke-TripoIngest)
  }
  'verify' { Write-Results (Invoke-Verify) }
  'pipeline' {
    if ($Pipeline -notin @('text->image->3d', 'text->image')) { throw "-Pipeline '$Pipeline': the stages are 'text->image' and 'text->image->3d'" }
    if ($Pipeline -eq 'text->image->3d' -and -not $Backend) { throw 'text->image->3d needs -Backend meshy|comfyui|tripo-folder for the 3D stage' }
    if ($Backend -eq 'comfyui' -and -not $Workflow3d) { throw 'the comfyui 3D stage needs -Workflow3d <image-to-3D ui-export.json>' }
    $imageResults = Invoke-ImageStage
    $made = @($imageResults | Where-Object { $_.status -in 'generated', 'cached' })
    if ($Pipeline -eq 'text->image' -or $DryRun) { Write-Results $imageResults; return }
    foreach ($r in $imageResults) { [Console]::Out.WriteLine(($r | ConvertTo-Json -Depth 30 -Compress)) }
    if ($made.Count -eq 0) { throw 'no images to take to 3D' }
    $Images = @($made | ForEach-Object path)
    $inputs = Get-ImageInputs
    switch ($Backend) {
      'meshy' { Write-Results (Invoke-MeshyStage $inputs) }
      'comfyui' { Write-Results (Invoke-Comfy3dStage $inputs $Workflow3d) }
      'tripo-folder' { Write-Results (Invoke-TripoManifest $inputs) }
    }
  }
}

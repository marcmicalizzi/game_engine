#!/usr/bin/env pwsh
<#
.SYNOPSIS
  The generator-service interface (docs/plan/07-content-pipeline.md §7.7) at tool level: text to
  image, image to 3D, and hand-made meshes ingested, every output beside a provenance sidecar.

.DESCRIPTION
  tools/generate.ps1 image    -Workflow <ui-export.json> (-Subjects <list.json> | -Subject <text> -Name <name>)
  tools/generate.ps1 3d       -Backend meshy   -Images <png|dir>...
  tools/generate.ps1 3d       -Backend comfyui -Workflow <image-to-3d ui-export.json> -Images <png|dir>...
  tools/generate.ps1 pipeline -Pipeline 'text->image->3d' -Workflow <image workflow> -Subjects <list.json>
                              -Backend meshy|tripo-folder|comfyui [-Workflow3d <image-to-3d workflow>]
  tools/generate.ps1 manifest -Backend tripo-folder -Images <png|dir>...     # the list the owner works from by hand
  tools/generate.ps1 ingest   -Backend tripo-folder -Manifest <manifest.json> [-From <dir>]
  tools/generate.ps1 balance                                                 # Meshy credits and the local ledger
  tools/generate.ps1 free                                                    # ComfyUI: unload models, free VRAM
  tools/generate.ps1 prompt   -Subject <text> [-Raw]                         # the prompt the template makes
  tools/generate.ps1 convert  -Workflow <ui-export.json> [-ObjectInfo <file>] # the API form ComfyUI takes
  tools/generate.ps1 verify   [-Path <dir|sidecar>]                           # re-hash outputs against sidecars

  Common: [-Date yyyy-MM-dd] [-LocalRoot <dir>] [-Only <name,...>] [-Force] [-DryRun] [-Yes]
  Image:  [-Seed <n>] [-Width <px> -Height <px>] [-Aspect 1:1 -FinalMegapixels 2 -BaseMegapixels <f>]
          [-Negative <text>] [-Raw] [-Set '<title|class>.<input>=<value>'...] [-NoLock] [-NoFree]
  Meshy:  [-AiModel latest] [-CreditsPerTask 30] [-MaxCredits 700] [-MinBalance 165] [-Retries 1]
          [-Concurrency 5] [-Set '<field>=<value>'...]

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
  [ValidateSet('comfyui', 'meshy', 'tripo-folder')] [string]$Backend,
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
  [string]$ServerPrefix = 'Agentic/{date}/{name}',
  [int]$TimeoutSec = 900,
  [string]$AiModel = 'latest',
  [int]$CreditsPerTask = 30,
  [int]$MaxCredits = 700,
  [int]$MinBalance = 165,
  [int]$Retries = 1,
  [int]$Concurrency = 5,
  [int]$TimeoutMinutes = 60,
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
      $kind = $spec[0]
      $isWidget = ($kind -is [array]) -or ($kind -in @('INT', 'FLOAT', 'STRING', 'BOOLEAN', 'COMBO'))
      $hasControl = $spec.Count -gt 1 -and $spec[1] -and $spec[1].PSObject.Properties['control_after_generate'] -and $spec[1].control_after_generate
      $linked = @($n.inputs) | Where-Object { $_ -and $_.name -eq $name -and $null -ne $_.link } | Select-Object -First 1
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
      if ($w -lt $vals.Count) { $inputs[$name] = $vals[$w]; $w++ }
      if ($hasControl -and $w -lt $vals.Count -and $vals[$w] -in $ControlWords) { $w++ }
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

# Every file the run produced, whatever the output node calls its list (`images`, `3d`, `gltf`...).
function Get-ComfyOutputs($hist) {
  $files = @()
  foreach ($out in $hist.outputs.PSObject.Properties) {
    foreach ($key in $out.Value.PSObject.Properties) {
      foreach ($item in @($key.Value)) {
        if ($item -and $item.PSObject -and $item.PSObject.Properties['filename']) {
          $files += [pscustomobject]@{ node = $out.Name; key = $key.Name; filename = [string]$item.filename; subfolder = [string]$item.subfolder; type = $(if ($item.type) { [string]$item.type } else { 'output' }) }
        }
      }
    }
  }
  return , @($files)
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
  $subjects = Get-SubjectList
  $wf = Get-WorkflowGraph $Workflow
  $dir = Get-OutputDir 'comfyui'
  $plan = @()
  foreach ($s in $subjects) {
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

function Get-MeshyBody {
  # The body the owner's first task was verified with (30 credits): the newest model, textured,
  # with PBR maps, as triangles. Anything else goes through -Set <field>=<value> and is recorded.
  $body = [ordered]@{ ai_model = $AiModel; should_texture = $true; enable_pbr = $true; topology = 'triangle' }
  foreach ($s in @($Set)) {
    if (-not $s) { continue }
    if ($s -notmatch '^(?<key>[a-z_]+)=(?<value>.*)$') { throw "-Set '$s' is not <field>=<value> for the Meshy request" }
    $body[$Matches.key] = ConvertFrom-SetValue $Matches.value
  }
  return $body
}

function Invoke-MeshyStage($inputs) {
  $dir = Get-OutputDir 'meshy'
  $ledger = Join-Path (Split-Path -Parent $dir) 'ledger.jsonl'
  $body = Get-MeshyBody
  $bodyHash = Get-TextSha256 ($body | ConvertTo-Json -Compress)
  $jobs = @()
  foreach ($in in $inputs) {
    $spec = Get-TextSha256 "$($in.sha256)`n$bodyHash"
    $side = Get-SidecarPath $dir $in.name
    $taskFile = Join-Path $dir "$($in.name).meshy-task.json"
    $state = Test-Existing $side $spec "mesh '$($in.name)'"
    $taskId = $null; $attempt = 1
    if ($state -eq 'new' -and (Test-Path -LiteralPath $taskFile) -and -not $Force) {
      $t = Read-JsonFile $taskFile
      if ($t.spec_hash -eq $spec -and $t.task_id) { $state = 'resume'; $taskId = [string]$t.task_id; $attempt = [int]$t.attempt }
    }
    $jobs += [pscustomobject]@{ Input = $in; Spec = $spec; State = $state; TaskId = $taskId; Attempt = $attempt; Submitted = $null; Progress = -1; Sidecar = $side; TaskFile = $taskFile }
  }
  $new = @($jobs | Where-Object State -eq 'new')
  $resume = @($jobs | Where-Object State -eq 'resume')
  $estimate = $new.Count * $CreditsPerTask
  $balance = Get-MeshyBalance
  Write-Step "meshy: $($jobs.Count) images: $($new.Count) to submit at ~$CreditsPerTask credits = ~$estimate credits; $($resume.Count) already submitted (resumed, no new spend); $($jobs.Count - $new.Count - $resume.Count) already made"
  Write-Step "meshy: balance $balance -> ~$($balance - $estimate) after; floor $MinBalance; this run's cap $MaxCredits; up to $Retries retry per failed task within the cap"
  Write-Note "  request: $(($body | ConvertTo-Json -Compress)) + the image as a data URI"
  Write-Note "  into $dir"
  if ($estimate -gt $MaxCredits) { throw "refused: ~$estimate credits is over this run's cap of $MaxCredits (-MaxCredits)" }
  if ($balance - $estimate -lt $MinBalance) { throw "refused: ~$estimate credits would take the balance from $balance below the floor of $MinBalance (-MinBalance)" }
  if ($DryRun) { return , @($jobs | ForEach-Object { [pscustomobject]@{ name = $_.Input.name; status = "dry-run:$($_.State)"; image = $_.Input.path; task_id = $_.TaskId } }) }
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
      $request = [ordered]@{}; foreach ($k in $body.Keys) { $request[$k] = $body[$k] }
      $ext = [IO.Path]::GetExtension($j.Input.path).TrimStart('.').ToLowerInvariant(); if ($ext -eq 'jpg') { $ext = 'jpeg' }
      $request.image_url = "data:image/$ext;base64," + [Convert]::ToBase64String([IO.File]::ReadAllBytes($j.Input.path))
      $r = Invoke-Meshy 'POST' '/image-to-3d' $request
      $j.TaskId = [string]$r.result
      $j.Submitted = Get-Utc
      $j.Progress = -1
      $committed += $CreditsPerTask
      # On disk before the first poll: an interrupted run resumes this task rather than paying again.
      Write-JsonFile $j.TaskFile ([ordered]@{ name = $j.Input.name; task_id = $j.TaskId; attempt = $j.Attempt; spec_hash = $j.Spec; image = $j.Input.path; image_sha256 = $j.Input.sha256; request = $body; submitted_utc = Format-Utc $j.Submitted })
      Add-JsonLine $ledger ([ordered]@{ utc = Format-Utc $j.Submitted; event = 'submitted'; name = $j.Input.name; task_id = $j.TaskId; attempt = $j.Attempt; estimate = $CreditsPerTask })
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
          $results += Save-MeshyResult $j $t $dir $body $ledger
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
  Add-JsonLine $ledger ([ordered]@{ utc = Format-Utc (Get-Utc); event = 'batch'; balance_before = $balance; balance_after = $after; tasks_consumed = $consumed })
  return , @($results)
}

function Save-MeshyResult($j, $t, [string]$dir, $body, [string]$ledger) {
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

function Invoke-Verify {
  $target = if ($Path) { $Path } else { Join-Path (Get-LocalRoot) 'generated' }
  $files = @(if (Test-Path -LiteralPath $target -PathType Container) { Get-ChildItem -LiteralPath $target -Recurse -File -Filter '*.provenance.json' | Sort-Object FullName | ForEach-Object FullName } else { $target })
  $problems = New-Object System.Collections.Generic.List[string]
  foreach ($f in $files) {
    $before = $problems.Count
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
  return , @([pscustomobject]@{ status = $(if ($problems.Count -eq 0) { 'ok' } else { 'failed' }); sidecars = $files.Count; problems = @($problems) })
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
    if ($r.status -in @('failed', 'timed-out', 'not-submitted')) { $failed = $true }
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
    if (-not $Backend) { throw '3d needs -Backend meshy|comfyui (tripo-folder is `manifest` and `ingest`)' }
    switch ($Backend) {
      'meshy' { Write-Results (Invoke-MeshyStage (Get-ImageInputs)) }
      'comfyui' { Write-Results (Invoke-Comfy3dStage (Get-ImageInputs) $Workflow) }
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

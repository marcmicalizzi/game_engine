#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Downloads a small pinned set of CC0 foliage (trees, shrubs, ground plants, a leaf atlas) into
  content/samples/foliage (git-ignored), and makes each plant's alpha-tested material whole.

.DESCRIPTION
  The static-asset world (content/test-scenes/static-world, docs/experiments/static-asset-world.md)
  is the engine's proof that a consumer with bought assets and no solver is served as well as the
  forest. Its first foliage was the engine's own kit, because the Khronos set fetch-samples.ps1
  pins has nothing alpha-tested. This script fetches real bought foliage for it (roadmap T139).

  **What is fetched, and why only that.** The owner permitted CC0 foliage and plant assets and
  nothing else (2026-11-14). Every file is listed in tools/fetch-foliage.json with its URL, its
  SHA-256 and its size, so every machine gets the same bytes; a file whose hash differs is deleted
  and the run fails. Before an item's first file, its own page is read and must state CC0:
  Poly Haven's item page carries the schema.org licence URL of CC0 1.0, ambientCG's states that
  every asset is released under CC0. An item whose page does not say so is not fetched. Widening
  the licence list is a decision to take deliberately, in a commit that says why.

  **What is made from it.** Poly Haven ships a plant's glTF with a JPEG base colour (no alpha
  channel) and its opacity as a separate `*_alpha` map that the glTF does not reference, under a
  material that already says MASK or BLEND (or, for the conifer twigs, says nothing). Drawn as
  shipped, every leaf card is a solid quad, because glTF's only mask is the base colour's alpha.
  So for each alpha map the script writes an RGBA PNG with `engine-content alpha-merge` (the
  engine's own decoder and PNG writer: the same bytes everywhere) and a `<id>.masked.gltf` beside
  the shipped one whose leaf materials sample it with `alphaMode` MASK at the glTF default cutoff
  0.5. A BLEND leaf material becomes MASK: the engine draws into a visibility buffer and has no
  sorted transparency, so it draws BLEND as opaque (docs/experiments/static-asset-world.md,
  "Bought foliage"). The shipped `<id>.gltf` is kept as it came, because drawing it as shipped is
  part of the finding. The leaf atlas's Color and Opacity are merged the same way.

  The files land outside version control (AGENTS.md: no binary assets); this script and its
  manifest are the reproducible step. A checkout without them configures, builds and passes:
  nothing in a test reads content/samples/foliage.

.EXAMPLE
  tools/fetch-foliage.ps1                                   # every item, then the masked variants
  tools/fetch-foliage.ps1 -Name fir_sapling,fern_02         # some items
  tools/fetch-foliage.ps1 -EngineContent build/msvc-release/bin/engine-content.exe
#>
[CmdletBinding()]
param(
  [string[]]$Name = @(),
  [string]$Out = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'content/samples/foliage'),
  [string]$EngineContent = '',
  [switch]$NoPrepare
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$manifest = Get-Content (Join-Path $PSScriptRoot 'fetch-foliage.json') -Raw | ConvertFrom-Json
$headers = @{ 'User-Agent' = 'engine-fetch-foliage' }
$cc0 = 'https://creativecommons.org/publicdomain/zero/1.0/'

function Assert-Cc0($item) {
  $html = (Invoke-WebRequest -Uri $item.page -Headers $headers).Content
  $ok = switch ($item.source) {
    'polyhaven' { $html.Contains("`"license`":`"$cc0`"") }
    'ambientcg' { $html -match 'released under the Creative Commons CC0' }
    default { $false }
  }
  if (-not $ok) { throw "fetch-foliage: $($item.id)'s page $($item.page) does not state CC0; not fetched" }
}

function Get-PinnedFile($item, $file) {
  $dest = Join-Path $Out (Join-Path $item.id $file.path)
  if ((Test-Path $dest) -and (Get-Item $dest).Length -eq $file.size -and
      (Get-FileHash -Algorithm SHA256 $dest).Hash.ToLowerInvariant() -eq $file.sha256) { return 0 }
  $dir = Split-Path -Parent $dest
  if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
  $part = "$dest.part"
  Invoke-WebRequest -Uri $file.url -Headers $headers -OutFile $part
  $sha = (Get-FileHash -Algorithm SHA256 $part).Hash.ToLowerInvariant()
  if ($sha -ne $file.sha256) {
    Remove-Item -LiteralPath $part
    throw "fetch-foliage: $($file.url) has SHA-256 $sha, pinned $($file.sha256); removed"
  }
  Move-Item -Force -LiteralPath $part -Destination $dest
  return 1
}

# Extracts the images of a zip into its own empty directory: only .png and .jpg entries with no
# directory part are written, so an entry naming `..` or an absolute path is never followed.
function Expand-Images([string]$zip, [string]$into) {
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  if (-not (Test-Path $into)) { New-Item -ItemType Directory -Force $into | Out-Null }
  $archive = [IO.Compression.ZipFile]::OpenRead($zip)
  try {
    foreach ($entry in $archive.Entries) {
      if ($entry.FullName -ne $entry.Name) { continue }
      if ($entry.Name -notmatch '\.(png|jpg)$') { continue }
      $target = Join-Path $into $entry.Name
      if ((Test-Path $target) -and (Get-Item $target).Length -eq $entry.Length) { continue }
      [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
    }
  } finally { $archive.Dispose() }
}

function Find-EngineContent {
  if ($EngineContent) { return (Resolve-Path $EngineContent).Path }
  foreach ($preset in 'msvc-release', 'msvc-debug') {
    $candidate = Join-Path $repoRoot "build/$preset/bin/engine-content.exe"
    if (Test-Path $candidate) { return $candidate }
  }
  return $null
}

function Invoke-AlphaMerge([string]$tool, [string]$color, [string]$alpha, [string]$out) {
  if ((Test-Path $out) -and (Get-Item $out).LastWriteTimeUtc -ge (Get-Item $color).LastWriteTimeUtc -and
      (Get-Item $out).LastWriteTimeUtc -ge (Get-Item $alpha).LastWriteTimeUtc) { return $null }
  $line = & $tool alpha-merge $color $alpha $out
  if ($LASTEXITCODE -ne 0) { throw "fetch-foliage: engine-content alpha-merge failed on $color" }
  return ($line | ConvertFrom-Json)
}

# <id>.masked.gltf: every material that samples the alpha map's base colour, and either already
# asks for a mask (MASK or BLEND) or is the alpha map's own named part (`twigs`, `leaves`), samples
# the merged RGBA image with alphaMode MASK. Everything else is the shipped file's.
function Write-MaskedGltf($item, [string]$tool) {
  $dir = Join-Path $Out $item.id
  $gltfPath = Join-Path $dir "$($item.id).gltf"
  $gltf = Get-Content $gltfPath -Raw | ConvertFrom-Json -Depth 64
  $changed = @()
  foreach ($file in $item.files) {
    if ($file.path -notmatch '^textures/(.+)_alpha_(\w+)\.jpg$') { continue }
    $stem = $Matches[1]; $res = $Matches[2]
    $part = $stem.Substring([Math]::Min($stem.Length, $item.id.Length)).TrimStart('_')
    $diffUri = "textures/$($stem)_diff_$res.jpg"
    $rgbaUri = "textures/$($stem)_rgba_$res.png"
    $merged = Invoke-AlphaMerge $tool (Join-Path $dir $diffUri) (Join-Path $dir $file.path) (Join-Path $dir $rgbaUri)
    if ($merged) { Write-Host ("   {0}: coverage {1:P1} at the cut" -f $rgbaUri, $merged.coverage) }
    $imageIndex = $gltf.images.Count
    $gltf.images += [pscustomobject]@{ mimeType = 'image/png'; name = "$($stem)_rgba"; uri = $rgbaUri }
    $textureIndex = $gltf.textures.Count
    $gltf.textures += [pscustomobject]@{ source = $imageIndex }
    foreach ($material in $gltf.materials) {
      $base = $material.pbrMetallicRoughness.baseColorTexture
      if ($null -eq $base) { continue }
      if ($gltf.images[$gltf.textures[$base.index].source].uri -ne $diffUri) { continue }
      $mode = $material.alphaMode
      if (-not (($mode -in @('MASK', 'BLEND')) -or $part)) { continue }
      $base.index = $textureIndex
      $material | Add-Member -Force -NotePropertyName alphaMode -NotePropertyValue 'MASK'
      if ($null -eq $material.alphaCutoff) { $material | Add-Member -Force -NotePropertyName alphaCutoff -NotePropertyValue 0.5 }
      $changed += "$($material.name) ($(if ($mode) { $mode } else { 'OPAQUE' }) -> MASK)"
    }
  }
  if ($changed.Count -eq 0) { return '' }
  $gltf | ConvertTo-Json -Depth 64 -Compress | Set-Content -Encoding utf8NoBOM (Join-Path $dir "$($item.id).masked.gltf")
  return ($changed -join ', ')
}

# <id>_cards.gltf: the leaf atlas on three crossed, two-sided cards of 1.2 m (the way a bought
# card bush uses one), so the atlas is drawn as foliage and not as a flat swatch. The buffer is a
# data URI: 12 corners (position, normal, UV) and 18 indices.
function Write-AtlasCards($item, [string]$imageName) {
  $pos = [Collections.Generic.List[single]]::new(); $nrm = [Collections.Generic.List[single]]::new()
  $uv = [Collections.Generic.List[single]]::new(); $idx = [Collections.Generic.List[uint16]]::new()
  $axes = @(@(1.0, 0.0), @(0.5, 0.8660254), @(-0.5, 0.8660254))   # 0, 60 and 120 degrees
  $k = 0
  foreach ($a in $axes) {
    $dx = 0.6 * $a[0]; $dz = 0.6 * $a[1]
    foreach ($c in @(@(-1, 0, 0, 1), @(1, 0, 1, 1), @(1, 1.2, 1, 0), @(-1, 1.2, 0, 0))) {
      $pos.AddRange([single[]]@(($c[0] * $dx), $c[1], ($c[0] * $dz)))
      $nrm.AddRange([single[]]@((-$a[1]), 0, $a[0]))
      $uv.AddRange([single[]]@($c[2], $c[3]))
    }
    $idx.AddRange([uint16[]]@($k, ($k + 1), ($k + 2), $k, ($k + 2), ($k + 3))); $k += 4
  }
  $bytes = [Collections.Generic.List[byte]]::new()
  foreach ($list in $pos, $nrm, $uv) { foreach ($f in $list) { $bytes.AddRange([BitConverter]::GetBytes([single]$f)) } }
  foreach ($i in $idx) { $bytes.AddRange([BitConverter]::GetBytes([uint16]$i)) }
  $n = 12
  $gltf = [ordered]@{
    asset = @{ version = '2.0'; generator = 'tools/fetch-foliage.ps1' }
    scene = 0; scenes = @(@{ nodes = @(0) }); nodes = @(@{ mesh = 0; name = "$($item.id)_cards" })
    meshes = @(@{ primitives = @(@{ attributes = @{ POSITION = 0; NORMAL = 1; TEXCOORD_0 = 2 }; indices = 3; material = 0 }) })
    materials = @(@{ name = "$($item.id)_leaves"; alphaMode = 'MASK'; alphaCutoff = 0.5; doubleSided = $true
                     pbrMetallicRoughness = @{ baseColorTexture = @{ index = 0 }; metallicFactor = 0; roughnessFactor = 0.8 } })
    textures = @(@{ source = 0 }); images = @(@{ uri = $imageName })
    buffers = @(@{ byteLength = $bytes.Count; uri = 'data:application/octet-stream;base64,' + [Convert]::ToBase64String($bytes.ToArray()) })
    bufferViews = @(
      @{ buffer = 0; byteOffset = 0; byteLength = $n * 12 }, @{ buffer = 0; byteOffset = $n * 12; byteLength = $n * 12 },
      @{ buffer = 0; byteOffset = $n * 24; byteLength = $n * 8 }, @{ buffer = 0; byteOffset = $n * 32; byteLength = 36 })
    accessors = @(
      @{ bufferView = 0; componentType = 5126; count = $n; type = 'VEC3'; min = @(-0.6, 0, -0.52); max = @(0.6, 1.2, 0.52) },
      @{ bufferView = 1; componentType = 5126; count = $n; type = 'VEC3' },
      @{ bufferView = 2; componentType = 5126; count = $n; type = 'VEC2' },
      @{ bufferView = 3; componentType = 5123; count = 18; type = 'SCALAR' })
  }
  $gltf | ConvertTo-Json -Depth 16 -Compress | Set-Content -Encoding utf8NoBOM (Join-Path $Out "$($item.id)/$($item.id)_cards.gltf")
}

$Name = @($Name | ForEach-Object { $_ -split ',' } | Where-Object { $_ })   # `pwsh -File` passes a list as one string
$selected = if ($Name.Count -gt 0) { @($manifest.items | Where-Object { $Name -contains $_.id }) } else { @($manifest.items) }
if ($Name.Count -gt 0 -and $selected.Count -ne $Name.Count) {
  throw "fetch-foliage: unknown item in '$($Name -join ', ')'; known: $(($manifest.items | ForEach-Object id) -join ', ')"
}
$tool = if ($NoPrepare) { $null } else { Find-EngineContent }
$madeById = @{}
foreach ($item in $selected) {
  Write-Host "== $($item.id) ($($item.kind), $($item.source))"
  Assert-Cc0 $item
  $fetched = 0
  $bytes = 0
  foreach ($file in $item.files) { $fetched += Get-PinnedFile $item $file; $bytes += $file.size }
  Write-Host ("   {0} file(s) fetched, {1:N1} MB pinned, licence {2}" -f $fetched, ($bytes / 1MB), $item.licence)
  $made = ''
  if ($tool) {
    if ($item.source -eq 'polyhaven') {
      $made = Write-MaskedGltf $item $tool
    } elseif ($item.kind -eq 'leaf atlas') {
      $zip = Join-Path $Out (Join-Path $item.id $item.files[0].path)
      $into = Join-Path $Out (Join-Path $item.id 'extracted')
      Expand-Images $zip $into
      $color = Get-ChildItem $into -Filter '*_Color.png' | Select-Object -First 1
      $opacity = Get-ChildItem $into -Filter '*_Opacity.png' | Select-Object -First 1
      if ($color -and $opacity) {
        Invoke-AlphaMerge $tool $color.FullName $opacity.FullName (Join-Path $Out "$($item.id)/$($item.id)_rgba.png") | Out-Null
        Write-AtlasCards $item "$($item.id)_rgba.png"
        $made = "$($item.id)_rgba.png, $($item.id)_cards.gltf"
      }
    }
    if ($made) { Write-Host "   made: $made" }
  } elseif (-not $NoPrepare) {
    Write-Warning "fetch-foliage: no engine-content found (build it, or pass -EngineContent); the masked variants were not written"
  }
  $madeById[$item.id] = $made
}

# LICENSES.md names every item that is on disk, not only those this run was asked for: a run with
# -Name rewrites the file, so it lists what an earlier run fetched too (an item counts as on disk
# when every one of its pinned files is there at its pinned size; its hashes were checked when it
# was fetched). What was made from an item fetched earlier is read from the files beside it.
function Get-MadeOnDisk($item) {
  $dir = Join-Path $Out $item.id
  $made = @()
  foreach ($f in "$($item.id).masked.gltf", "$($item.id)_rgba.png", "$($item.id)_cards.gltf") {
    if (Test-Path (Join-Path $dir $f)) { $made += $f }
  }
  return ($made -join ', ')
}
$rows = @()
foreach ($item in $manifest.items) {
  $present = $true
  foreach ($file in $item.files) {
    $path = Join-Path $Out (Join-Path $item.id $file.path)
    if (-not (Test-Path $path) -or (Get-Item $path).Length -ne $file.size) { $present = $false; break }
  }
  if (-not $present) { continue }
  $made = if ($madeById.ContainsKey($item.id)) { $madeById[$item.id] } else { Get-MadeOnDisk $item }
  # A licence stated site-wide rather than on the item's page is quoted, with the page that states
  # it and the day it was read (ambientCG: its item pages say CC0 only through that sentence).
  $licence = if ($item.licence_quote) {
    "$($item.licence): `"$($item.licence_quote)`" ($($item.licence_page), read $($item.licence_read))"
  } else { $item.licence }
  $rows += "| $($item.id) | $($item.kind) | $licence | $($item.authors) | $($item.page) | $(if ($made) { $made } else { '-' }) |"
}

$licenseFile = Join-Path $Out 'LICENSES.md'
$text = @(
  '# Foliage samples',
  '',
  'Fetched by `tools/fetch-foliage.ps1` from the URLs pinned in `tools/fetch-foliage.json`, each',
  "item's licence read from its own page before it was fetched. Every item is CC0 1.0 (public",
  'domain dedication, no attribution required); the authors are named as their pages credit them.',
  'These files are not part of the engine and are not committed.',
  '',
  '| Item | Kind | Licence | Authors | Page | Made here |',
  '|---|---|---|---|---|---|'
) + $rows
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force $Out | Out-Null }
Set-Content -Path $licenseFile -Value ($text -join "`n") -NoNewline
Write-Host "wrote $licenseFile"

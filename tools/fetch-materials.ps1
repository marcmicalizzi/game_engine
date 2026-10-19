#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Unpacks a material pack into content/materials (git-ignored), every file checked against its
  manifest and the pack against a pinned hash.

.DESCRIPTION
  A material pack (docs/subsystems/materials.md, content/materials/README.md) is the default PBR
  sets the tree and rock solvers fall back to by role (`palm/bark`, `rock/granite`) when a recipe
  names none. It travels as a zip made by tools/pack-materials.ps1, the way tools/fetch-samples.ps1
  brings the Khronos models into content/samples: the images never enter the repository, the
  script is the reproducible step, and the licence file lands beside them.

  The zip's SHA-256 must be the expected one: `-Expect`, or the pinned hash below when the source is
  the pinned default. A pack whose hash is not the one asked for is refused before anything is
  unpacked, and so is one whose `pack.json` is not a pack, names a file outside itself, or lists a
  map whose bytes do not hash to what it says. On success the directory's previous contents are
  replaced (its README.md stays), and the summary is one JSON line.

  **No default is hosted yet.** Until the owner hosts one, `$pinnedSource` is empty: pass
  `-Source <zip path or URL> -Expect <sha256>`, for instance the pack tools/pack-materials.ps1 wrote
  under D:\workspace\game_engine_local\packages\.

.EXAMPLE
  tools/fetch-materials.ps1 -Source D:\workspace\game_engine_local\packages\materials-palm-2026-10-19.zip -Expect <sha256>
#>
[CmdletBinding()]
param(
  [string]$Source = '',
  [string]$Expect = '',
  [string]$Out = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..')).Path 'content/materials')
)

$ErrorActionPreference = 'Stop'

# The pinned default pack: where it is hosted and the SHA-256 of its zip. Empty until the owner
# hosts one; a change to either is a commit that says which pack and why.
$pinnedSource = ''
$pinnedSha256 = ''

function Fail([string]$message, [int]$code = 1) {
  [Console]::Error.WriteLine("fetch-materials: $message")
  exit $code
}

if ($Source -eq '') {
  if ($pinnedSource -eq '') {
    Fail ("no default material pack is hosted yet: pass -Source <zip path or URL> -Expect <sha256>, " +
      "for instance a pack tools/pack-materials.ps1 wrote under D:\workspace\game_engine_local\packages\") 2
  }
  $Source = $pinnedSource
}
if ($Expect -eq '') {
  if ($Source -eq $pinnedSource -and $pinnedSha256 -ne '') {
    $Expect = $pinnedSha256
  } else {
    Fail "a pack from $Source needs -Expect <sha256>: every pack this script unpacks is checked against a hash" 2
  }
}
$Expect = $Expect.ToLowerInvariant()

$work = Join-Path ([IO.Path]::GetTempPath()) "engine-fetch-materials-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $work | Out-Null
try {
  $zip = Join-Path $work 'pack.zip'
  if ($Source -match '^https?://') {
    Invoke-WebRequest -Uri $Source -OutFile $zip -UseBasicParsing
  } else {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) { Fail "no pack at $Source" }
    Copy-Item -LiteralPath $Source -Destination $zip
  }
  $sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($sha -ne $Expect) { Fail "the pack from $Source hashes to $sha, and $Expect was expected: not unpacked" }

  $unpacked = Join-Path $work 'pack'
  Expand-Archive -LiteralPath $zip -DestinationPath $unpacked
  $manifestPath = Join-Path $unpacked 'pack.json'
  if (-not (Test-Path -LiteralPath $manifestPath)) { Fail 'the pack has no pack.json' }
  $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
  if ($manifest.format -ne 'engine.materials.pack/1') { Fail "pack.json is not an engine.materials.pack/1 ('$($manifest.format)')" }
  $root = [IO.Path]::GetFullPath($unpacked)
  $listed = New-Object System.Collections.Generic.HashSet[string]
  $roleNames = @($manifest.roles.PSObject.Properties.Name)
  if ($roleNames.Count -eq 0) { Fail 'pack.json lists no roles' }
  foreach ($role in $roleNames) {
    $entry = $manifest.roles.$role
    foreach ($map in @('base_color', 'normal', 'roughness', 'metallic', 'height')) {
      $record = $entry.maps.$map
      if ($null -eq $record -or "$($record.file)" -eq '') { Fail "$role lists no $map map" }
      $full = [IO.Path]::GetFullPath((Join-Path $unpacked $record.file))
      if (-not $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar)) { Fail "$role's $map map names a file outside the pack: $($record.file)" }
      if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { Fail "$role's $map map $($record.file) is not in the pack" }
      $got = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant()
      if ($got -ne "$($record.sha256)".ToLowerInvariant()) { Fail "$($record.file) hashes to $got, and pack.json says $($record.sha256)" }
      [void]$listed.Add($full)
    }
  }

  # The licence file: the pack's own, or one written from the manifest when it carries none.
  $licencePath = Join-Path $unpacked 'LICENSES.md'
  if (-not (Test-Path -LiteralPath $licencePath)) {
    $rows = foreach ($role in $roleNames) { "| ``$role`` | $($manifest.roles.$role.licence) |" }
    (@("# $($manifest.name) $($manifest.version): licences", '', "$($manifest.licence)", '', '| Role | Licence |', '|---|---|') + $rows) -join "`n" |
      Set-Content -LiteralPath $licencePath -Encoding utf8NoBOM
  }

  # Replace what was there, keeping the directory's README.md (the one committed file in it).
  New-Item -ItemType Directory -Force -Path $Out | Out-Null
  Get-ChildItem -LiteralPath $Out -Force | Where-Object { $_.Name -ne 'README.md' } |
    Remove-Item -Recurse -Force
  Get-ChildItem -LiteralPath $unpacked -Force | Where-Object { $_.Name -ne 'README.md' } |
    Copy-Item -Destination $Out -Recurse -Force
  [ordered]@{
    out     = (Resolve-Path -LiteralPath $Out).Path
    source  = $Source
    sha256  = $sha
    name    = $manifest.name
    version = $manifest.version
    roles   = $roleNames
    files   = $listed.Count
  } | ConvertTo-Json -Compress
} finally {
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

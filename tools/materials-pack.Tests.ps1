#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/pack-materials.ps1 and tools/fetch-materials.ps1. Runs under CTest as
  `tools.materials_pack`.

.DESCRIPTION
  Offline, in a scratch directory, over stand-in sets whose maps are PNG headers of the test's own
  (the scripts read a PNG's size from its IHDR chunk and hash its bytes; nothing decodes them). It
  checks that

  - a staging directory of two roles packs into a zip whose pack.json lists both roles with every
    map's SHA-256, resolution, `tile_m`, normal convention and strength, the provenance and the
    licence line, and whose LICENSES.md names each role;
  - a role with a map missing, a map whose sides are not powers of two, a DirectX normal map, or a
    provenance naming a model with no licence line is refused, and writes no zip;
  - fetch-materials refuses a pack with no -Expect (exit 2), a pack whose hash is not the expected
    one, and a pack whose map bytes do not match its manifest, leaving the destination untouched;
  - a good pack unpacks into the destination with its licence file, replacing what was there but
    keeping the destination's README.md;
  - with no source and nothing pinned, fetch-materials says where to point -Source (exit 2).

      pwsh tools/materials-pack.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'
$pack = Join-Path $PSScriptRoot 'pack-materials.ps1'
$fetch = Join-Path $PSScriptRoot 'fetch-materials.ps1'
$pwshPath = [Environment]::ProcessPath
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

function Invoke-Script([string]$script) {
  $errFile = [IO.Path]::GetTempFileName()
  try {
    $out = & $pwshPath -NoProfile -NonInteractive -File $script @args 2> $errFile
    $code = $LASTEXITCODE
    $err = Get-Content -Raw -LiteralPath $errFile
  } finally { Remove-Item -LiteralPath $errFile -Force -ErrorAction SilentlyContinue }
  $json = @($out | Where-Object { $_ -is [string] -and $_.TrimStart().StartsWith('{') } | ForEach-Object { try { $_ | ConvertFrom-Json } catch { } })
  return [pscustomobject]@{ Code = $code; Json = $json; Err = "$err" }
}

# A PNG header of the given size, and a tail that makes each file's bytes its own.
function Write-Png([string]$path, [int]$w, [int]$h, [string]$tail) {
  $bytes = New-Object System.Collections.Generic.List[byte]
  $bytes.AddRange([byte[]](0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13))
  $bytes.AddRange([Text.Encoding]::ASCII.GetBytes('IHDR'))
  foreach ($v in @($w, $h)) {
    foreach ($shift in @(24, 16, 8, 0)) { $bytes.Add([byte](($v -shr $shift) -band 255)) }
  }
  $bytes.AddRange([byte[]](8, 6, 0, 0, 0))
  $bytes.AddRange([Text.Encoding]::ASCII.GetBytes($tail))
  [IO.File]::WriteAllBytes($path, $bytes.ToArray())
}

function New-Role([string]$staging, [string]$role, [int]$size = 64, [string]$model = 'StableMaterials', [string]$convention = 'OpenGL +Y') {
  $dir = Join-Path $staging $role
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  foreach ($map in @('basecolor', 'normal', 'roughness', 'metallic', 'height')) { Write-Png (Join-Path $dir "$map.png") $size $size "$role/$map" }
  @{ id = $role; tile_m = 0.35; normal_convention = $convention; normal_strength_preview = 0.4; seed = 7; prompt = 'stand-in' } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $dir 'material.json')
  @{ models = @(@{ name = $model; revision = 'abc123' }, @{ name = 'PBRify UpscalerV4'; revision = 'v4' }); seed = 7; settings = @{ steps = 50 } } |
    ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $dir 'provenance.json')
  return $dir
}

$root = Join-Path ([IO.Path]::GetTempPath()) "engine-materials-pack-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $root | Out-Null
try {
  Write-Host 'pack-materials'
  $staging = Join-Path $root 'staging'
  New-Role $staging 'palm/bark' | Out-Null
  New-Role $staging 'rock/sandstone' 128 | Out-Null
  $zip = Join-Path $root 'out/pack.zip'
  $r = Invoke-Script $pack -From $staging -Out $zip -Name 'stand-in' -Version '1'
  Test-That 'a staging directory of two roles packs' { $r.Code -eq 0 -and (Test-Path -LiteralPath $zip) }
  $sha = if ($r.Json.Count -gt 0) { $r.Json[0].sha256 } else { '' }
  Test-That 'the summary names the zip''s SHA-256' { $sha -eq (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() }
  $peek = Join-Path $root 'peek'
  Expand-Archive -LiteralPath $zip -DestinationPath $peek
  $manifest = Get-Content -Raw -LiteralPath (Join-Path $peek 'pack.json') | ConvertFrom-Json
  Test-That 'pack.json is a pack with both roles' { $manifest.format -eq 'engine.materials.pack/1' -and @($manifest.roles.PSObject.Properties.Name).Count -eq 2 }
  $bark = $manifest.roles.'palm/bark'
  Test-That 'a role carries tile_m, the convention and the lab''s normal strength' { $bark.tile_m -eq 0.35 -and $bark.normal_convention -eq 'opengl' -and $bark.normal_scale -eq 0.4 }
  Test-That 'every map has its file, hash and resolution' {
    $m = $manifest.roles.'rock/sandstone'.maps.base_color
    $m.file -eq 'rock/sandstone/base_color.png' -and $m.width -eq 128 -and $m.sha256 -eq (Get-FileHash -LiteralPath (Join-Path $peek $m.file) -Algorithm SHA256).Hash.ToLowerInvariant()
  }
  Test-That 'the provenance is copied as text and the licences named' { $bark.provenance.models[0].revision -eq 'abc123' -and $bark.licence -match 'OpenRAIL' -and $bark.licence -match 'CC0' }
  Test-That 'LICENSES.md names each role' { (Get-Content -Raw -LiteralPath (Join-Path $peek 'LICENSES.md')) -match 'rock/sandstone' }

  foreach ($case in @(
      @{ What = 'a missing map'; Make = { param($s) $d = New-Role $s 'palm/frond'; Remove-Item -LiteralPath (Join-Path $d 'height.png') }; Says = 'height' },
      @{ What = 'a non-power-of-two map'; Make = { param($s) New-Role $s 'palm/frond' 96 | Out-Null }; Says = 'powers of two' },
      @{ What = 'a DirectX normal map'; Make = { param($s) New-Role $s 'palm/frond' 64 'StableMaterials' 'DirectX -Y' | Out-Null }; Says = 'OpenGL' },
      @{ What = 'a model with no licence line'; Make = { param($s) New-Role $s 'palm/frond' 64 'Krea' | Out-Null }; Says = 'Krea' })) {
    $s = Join-Path $root "bad-$([guid]::NewGuid().ToString('N'))"
    & $case.Make $s
    $badZip = Join-Path $root 'bad.zip'
    $b = Invoke-Script $pack -From $s -Out $badZip
    Test-That "$($case.What) is refused with a sentence" { $b.Code -eq 1 -and $b.Err -match [regex]::Escape($case.Says) -and -not (Test-Path -LiteralPath $badZip) }
  }

  Write-Host 'fetch-materials'
  $dest = Join-Path $root 'content/materials'
  New-Item -ItemType Directory -Force -Path $dest | Out-Null
  Set-Content -LiteralPath (Join-Path $dest 'README.md') -Value 'kept'
  Set-Content -LiteralPath (Join-Path $dest 'stale.txt') -Value 'old'
  $f = Invoke-Script $fetch -Source $zip -Out $dest
  Test-That 'a pack with no -Expect is refused (exit 2)' { $f.Code -eq 2 -and $f.Err -match 'Expect' }
  $f = Invoke-Script $fetch -Source $zip -Expect ('0' * 64) -Out $dest
  Test-That 'a pack whose hash is not the expected one is refused' { $f.Code -eq 1 -and $f.Err -match 'hashes to' -and (Test-Path -LiteralPath (Join-Path $dest 'stale.txt')) }
  $f = Invoke-Script $fetch -Out $dest
  Test-That 'no source and nothing pinned says where to point -Source (exit 2)' { $f.Code -eq 2 -and $f.Err -match '-Source' }

  # A pack whose bytes are not its manifest's: one map rewritten after packing.
  $tampered = Join-Path $root 'tampered'
  Copy-Item -LiteralPath $peek -Destination $tampered -Recurse
  Write-Png (Join-Path $tampered 'palm/bark/normal.png') 64 64 'not the packed bytes'
  $tamperedZip = Join-Path $root 'tampered.zip'
  Compress-Archive -Path (Join-Path $tampered '*') -DestinationPath $tamperedZip
  $tsha = (Get-FileHash -LiteralPath $tamperedZip -Algorithm SHA256).Hash
  $f = Invoke-Script $fetch -Source $tamperedZip -Expect $tsha -Out $dest
  Test-That 'a map whose bytes are not the manifest''s is refused' { $f.Code -eq 1 -and $f.Err -match 'normal.png' -and (Test-Path -LiteralPath (Join-Path $dest 'stale.txt')) }

  $f = Invoke-Script $fetch -Source $zip -Expect $sha -Out $dest
  Test-That 'a good pack unpacks' { $f.Code -eq 0 -and $f.Json.Count -eq 1 -and $f.Json[0].files -eq 10 }
  Test-That 'the destination holds the pack and its licence file' { (Test-Path -LiteralPath (Join-Path $dest 'pack.json')) -and (Test-Path -LiteralPath (Join-Path $dest 'LICENSES.md')) -and (Test-Path -LiteralPath (Join-Path $dest 'rock/sandstone/height.png')) }
  Test-That 'what was there is replaced and README.md kept' { -not (Test-Path -LiteralPath (Join-Path $dest 'stale.txt')) -and (Get-Content -Raw -LiteralPath (Join-Path $dest 'README.md')).Trim() -eq 'kept' }
} finally {
  if ($KeepTemp) { Write-Host "kept $root" } else { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host "$checks checks, $($failures.Count) failed"
if ($failures.Count -gt 0) { exit 1 }
exit 0

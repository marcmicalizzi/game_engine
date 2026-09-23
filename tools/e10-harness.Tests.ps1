#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/e10-harness.ps1. Runs under CTest as `tools.e10_harness`.

.DESCRIPTION
  Offline: no GPU, no captures, no generated assets. The judging and the report are driven through
  `-FromReport` over a synthetic report written here — a stale binary must put the STALE BINARIES
  caveat at the top of report.md and into report.json, a thin asset's object-only FLIP must carry
  its low-coverage mark, and re-judging under another -LowCoverage must move it. The staleness check
  itself runs through `-CheckBinaries` against this checkout's own build tree when one exists: a
  binary stamped with HEAD must never be called stale for changes "between HEAD and HEAD", which is
  the one way the check has already been wrong.

      pwsh tools/e10-harness.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'
$harness = Join-Path $PSScriptRoot 'e10-harness.ps1'
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

function Invoke-Harness {
  $errFile = [IO.Path]::GetTempFileName()
  try {
    $out = & $pwshPath -NoProfile -NonInteractive -File $harness @args 2> $errFile
    $code = $LASTEXITCODE
    $err = Get-Content -Raw -LiteralPath $errFile
  } finally { Remove-Item -LiteralPath $errFile -Force -ErrorAction SilentlyContinue }
  $json = @($out | Where-Object { $_ -is [string] -and $_.TrimStart().StartsWith('{') } | ForEach-Object { try { $_ | ConvertFrom-Json } catch { } })
  return [pscustomobject]@{ Code = $code; Out = @($out); Json = $json; Err = "$err" }
}

$root = Join-Path ([IO.Path]::GetTempPath()) "engine-e10-harness-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Force -Path $root | Out-Null

function New-Report([string]$dir, [bool]$stale) {
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $binary = { param($name) [ordered]@{ name = $name; sha256 = ('0' * 64); built = '2026-09-22T10:00:00.0000000Z'
      stamp = [ordered]@{ commit = ('a' * 40); dirty = $false }; judged_by = 'build stamp'; stale = $stale
      stale_reasons = @(if ($stale) { 'built from aaaaaaaaaaaa, and 3 file(s) it is made of changed between that and HEAD bbbbbbbbbbbb (first: domain/geometry/src/x.cpp)' }) } }
  $row = { param($name, $coverage, $flip, $smallest)
    [ordered]@{ name = $name; source = "$name.glb"; glb_bytes = 1048576; glb_sha256 = ('1' * 64); service = 'trellis'
      triangles = 100000; clusters = 900; lod_levels = 10; materials = 1; images = 3; warnings = 0; warning_rules = [ordered]@{}
      container_bytes = 2097152; build_ms = 500; islands = 400; seam_fraction = 0.2; smallest_island_texels_4096 = $smallest
      smallest_island_triangles = 1; median_island_texels_4096 = 5000; uv_area = 0.5
      coarse_visible_pairs = 300; finest_visible_pairs = 900; psnr = 38.0; ssim = 0.99; flip_mean = $flip; flip_p95 = 0.05; flip_max = 0.5
      coverage = $coverage; flip_object_mean = [math]::Round($flip / $coverage, 5) } }
  $state = [ordered]@{ min = 1; max = 2 }
  $report = [ordered]@{
    schema = 'engine.e10.report/1'; title = 'synthetic'; started_utc = '2026-09-23T00:00:00Z'; finished_utc = '2026-09-23T00:01:00Z'; seconds = 60
    folders = @('x'); settings = [ordered]@{ width = 640; height = 640; orbit = 20.8; framing = 'fit: the whole bounding sphere in frame with a 5% margin (orbit 20.8)'; frames = 4; coarse_lod_px = 1; finest_lod_px = 0.05; shadows = 'off'; raster = 'hw (default)' }
    tools = [ordered]@{ source = 'build/msvc-release/bin'; commit = ('b' * 40); built_from = @(('a' * 40)); preset = 'msvc-release'
      binaries = @((& $binary 'engine-content.exe'), (& $binary 'engine-view.exe'), (& $binary 'engine-image.exe'))
      freshness = [ordered]@{ head = ('b' * 40); stale = $stale; allow_stale = $stale; staleness_paths = @('domain/geometry') }; harness = 'tools/e10-harness.ps1' }
    machine_state = [ordered]@{ samples = 4; cpu_others_pct = $state; gpu_util_pct = $state; gpu_memory_used_mib = $state; gpu_memory_total_mib = $state; captures_warned = 0; gpu_lock_at_start = 'free'; gpu_lock_at_end = 'free' }
    assets = @((& $row 'solid-crate' 0.45 0.009 5.0), (& $row 'thin-tree' 0.06 0.0072 5.0))
  }
  $file = Join-Path $dir 'report.json'
  [IO.File]::WriteAllText($file, ($report | ConvertTo-Json -Depth 20))
  return $file
}

try {
  Write-Host '-FromReport, stale binaries'
  $file = New-Report (Join-Path $root 'stale') $true
  $r = Invoke-Harness -FromReport $file
  $md = Get-Content -Raw (Join-Path $root 'stale/report.md')
  $json = Get-Content -Raw $file | ConvertFrom-Json
  Test-That 'exit 0 (judging is not refusing: the binaries were already allowed)' { $r.Code -eq 0 }
  Test-That 'report.md opens with the STALE BINARIES caveat, before any number' {
    $lines = $md -split "`n"; $lines[2] -match '^> \*\*STALE BINARIES\.\*\*' -and $md.IndexOf('STALE BINARIES') -lt $md.IndexOf('Pass rate') }
  Test-That 'and names each stale binary with its reason' { $md -match 'engine-view\.exe`: built from aaaaaaaaaaaa, and 3 file' }
  Test-That 'report.json carries the caveat as its second key' { (@($json.PSObject.Properties.Name)[2] -eq 'caveat') -and $json.caveat -match '^STALE BINARIES: engine-content\.exe, engine-view\.exe, engine-image\.exe' }
  Test-That 'the framing is stated with the orbit' { $md -match '--orbit 20\.8' -and $md -match 'framing fit' }

  Write-Host '-FromReport, low coverage'
  $solid = $json.assets | Where-Object name -eq 'solid-crate'
  $thin = $json.assets | Where-Object name -eq 'thin-tree'
  Test-That 'a thin asset''s object FLIP is marked unreliable, a solid one''s is not' { $thin.object_flip_reliable -eq $false -and $solid.object_flip_reliable -eq $true }
  Test-That 'report.md marks it and says why' { $md -match '\| 0\.120 †' -and $md -match 'Object FLIP unreliable below 15% coverage\*\* \(thin-tree\)' }
  Test-That 'the verdicts are unchanged by it: both pass on the whole-frame FLIP' { $thin.status -eq 'pass' -and $solid.status -eq 'pass' }
  $r = Invoke-Harness -FromReport $file -LowCoverage 0.05
  $json = Get-Content -Raw $file | ConvertFrom-Json
  Test-That 're-judged with -LowCoverage 0.05, the mark goes' { (($json.assets | Where-Object name -eq 'thin-tree').object_flip_reliable -eq $true) -and -not ((Get-Content -Raw (Join-Path $root 'stale/report.md')) -match '†') }

  Write-Host '-FromReport, fresh binaries'
  $file = New-Report (Join-Path $root 'fresh') $false
  $r = Invoke-Harness -FromReport $file
  $md = Get-Content -Raw (Join-Path $root 'fresh/report.md')
  $json = Get-Content -Raw $file | ConvertFrom-Json
  Test-That 'no caveat when nothing is stale, and the report says so' { $md -cnotmatch 'STALE' -and $null -eq $json.caveat -and $md -match 'none stale' }
  Test-That 'times read back from a report stay ISO 8601, not the local culture''s format' { $md -match 'from 2026-09-23T00:00:00' }

  Write-Host '-CheckBinaries'
  $exe = if ($IsWindows -or $env:OS -eq 'Windows_NT') { '.exe' } else { '' }
  $bin = Get-ChildItem -Path (Join-Path $repo "build/*/bin/engine-image$exe") -ErrorAction SilentlyContinue |
    Where-Object { (Test-Path (Join-Path $_.DirectoryName "engine-content$exe")) -and (Test-Path (Join-Path $_.DirectoryName "engine-view$exe")) } |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
  if ($bin) {
    $r = Invoke-Harness -CheckBinaries -Bin $bin.DirectoryName -Out (Join-Path $root 'check')
    $c = $r.Json | Select-Object -Last 1
    Test-That '-CheckBinaries answers with one JSON line and exit 0 or 3' { $c -and $r.Code -in 0, 3 -and @($c.binaries).Count -eq 3 }
    Test-That 'every binary says how it was judged' { @($c.binaries | Where-Object { -not $_.judged_by }).Count -eq 0 }
    Test-That 'a binary stamped with HEAD is never stale for changes between HEAD and HEAD' {
      @($c.binaries | Where-Object { $_.stamp -and $_.stamp.commit -eq $c.freshness.head -and (@($_.stale_reasons) -join ' ') -match 'changed between' }).Count -eq 0 }
  } else {
    Write-Host '  skip -CheckBinaries: no build tree has engine-content, engine-view and engine-image'
  }
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

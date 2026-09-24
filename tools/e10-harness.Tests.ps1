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

function New-Report([string]$dir, [bool]$stale, [bool]$collapse = $false, [bool]$atlas = $false) {
  New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $binary = { param($name) [ordered]@{ name = $name; sha256 = ('0' * 64); built = '2026-09-22T10:00:00.0000000Z'
      stamp = [ordered]@{ commit = ('a' * 40); dirty = $false }; judged_by = 'build stamp'; stale = $stale
      stale_reasons = @(if ($stale) { 'built from aaaaaaaaaaaa, and 3 file(s) it is made of changed between that and HEAD bbbbbbbbbbbb (first: domain/geometry/src/x.cpp)' }) } }
  $row = { param($name, $coverage, $flip, $smallest, $coarse = 300, $finest = 900, $seam = 0.2, $median = 5000)
    [ordered]@{ name = $name; source = "$name.glb"; glb_bytes = 1048576; glb_sha256 = ('1' * 64); service = 'trellis'
      triangles = 100000; clusters = 900; lod_levels = 10; materials = 1; images = 3; warnings = 0; warning_rules = [ordered]@{}
      container_bytes = 2097152; build_ms = 500; islands = 400; seam_fraction = $seam; smallest_island_texels_4096 = $smallest
      smallest_island_triangles = 1; median_island_texels_4096 = $median; uv_area = 0.5
      coarse_visible_pairs = $coarse; finest_visible_pairs = $finest; psnr = 38.0; ssim = 0.99; flip_mean = $flip; flip_p95 = 0.05; flip_max = 0.5
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
  if ($collapse) {
    # The three cases the collapse check tells apart, each measured once in E10: a mesh whose LOD
    # coarsens, one that does not (Meshy's triangle remesh: a fragmented atlas, the coarse cut is the
    # finest), and one that does not need to (a low-poly game asset, already sparse on screen).
    $report.assets = @(
      (& $row 'sound-crate' 0.45 0.009 5.0 300 900),                      # share 0.33, 4.9 pairs per 1,000 object px
      (& $row 'stuck-cactus' 0.46 0.0 1.0 2567 2567 0.96 26),             # share 1.00, 13.6 per kpx, fragmented atlas
      (& $row 'stuck-sound-atlas' 0.45 0.004 5.0 880 900),                # share 0.98, 4.9 per kpx, atlas sound
      (& $row 'sparse-helmet' 0.452 0.0019 36.0 223 233))                 # share 0.96, but 1.26 per kpx
  }
  if ($atlas) {
    # A repacked build (`-Atlas repack -CompareKept`): the atlas step's block and the one-camera
    # comparison with the kept build, on the first asset only.
    $report.settings.atlas = 'repack'
    $report.settings.compare_kept = $true
    $report.assets[0].atlas = [ordered]@{ mode = 'repack'; charts = 61; materials = 1; resolution = 2048; utilization = 0.71; pack_attempts = 3
      unatlased_triangles = 0; proxy_charts = 58; folded_triangles = 233; image_bytes = 12582912; ms = [ordered]@{ total = 9000 }
      colour_error_mean = 1.17; colour_error_p99 = 13.0; normal_error_mean_deg = 1.04; normal_error_p99_deg = 21.7; notes = @() }
    $report.assets[0].reference = [ordered]@{ against = 'kept'; psnr = 38.79; ssim = 0.99583; flip_mean = 0.01407; flip_p95 = 0.052; flip_object_mean = 0.031 }
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

  Write-Host '-FromReport, LOD collapse'
  $file = New-Report (Join-Path $root 'collapse') $false $true
  $r = Invoke-Harness -FromReport $file
  $md = Get-Content -Raw (Join-Path $root 'collapse/report.md')
  $json = Get-Content -Raw $file | ConvertFrom-Json
  $a = @{}; foreach ($x in $json.assets) { $a[$x.name] = $x }
  Test-That 'the share is computed from the two counts a report already carries' { $a['sound-crate'].collapse_share -eq 0.3333 -and $a['stuck-cactus'].collapse_share -eq 1 -and $a['sparse-helmet'].finest_pairs_per_kpx -eq 1.259 }
  Test-That 'a mesh whose LOD coarsens passes the check' { $a['sound-crate'].checks.collapse -eq 'pass' -and $a['sound-crate'].status -eq 'pass' }
  Test-That 'a mesh whose coarse cut is its finest fails it, though its FLIP is 0' { $a['stuck-cactus'].checks.collapse -eq 'fail' -and $a['stuck-cactus'].checks.flip -eq 'pass' -and $a['stuck-cactus'].status -eq 'fail' }
  Test-That 'the diagnosis says why: the atlas when it is fragmented, the geometry when it is not' {
    $a['stuck-cactus'].diagnosis -match 'does not collapse.*100% of the finest cut''s pairs \(2,567 of 2,567\).*the atlas is why' -and
    $a['stuck-sound-atlas'].diagnosis -match 'does not collapse.*the atlas is not fragmented, so the geometry is why' }
  Test-That 'a mesh already sparse on screen is not judged: it has nothing to shed at this view' { $a['sparse-helmet'].checks.collapse -eq 'n/a' -and $a['sparse-helmet'].status -eq 'pass' }
  Test-That 'report.md counts the check, shows the share and the density, and states the line' {
    $md -match 'LOD collapse 2 \(an asset' -and $md -match '2,567 / 2,567 \(100%; 13\.6\)' -and $md -match '223 / 233 \(96%; 1\.3, too sparse to judge\)' -and $md -match 'the coarse cut draws <= 75% of the finest cut''s pairs' }
  Test-That 'report.json carries the thresholds and why' { $json.thresholds.max_collapse_share -eq 0.75 -and $json.thresholds.min_collapse_density_pairs_per_kpx -eq 3 -and $json.thresholds.why.collapse -match 'Khronos' }
  $r = Invoke-Harness -FromReport $file -MaxCollapseShare 1.0 -MinCollapseDensity 1.0
  $json = Get-Content -Raw $file | ConvertFrom-Json
  $a = @{}; foreach ($x in $json.assets) { $a[$x.name] = $x }
  Test-That 're-judged under other lines, the verdicts move with them' { $a['stuck-cactus'].checks.collapse -eq 'pass' -and $a['sparse-helmet'].checks.collapse -eq 'pass' -and $json.totals.failed_by_check.collapse -eq 0 }

  Write-Host '-FromReport, a repacked build'
  $file = New-Report (Join-Path $root 'atlas') $false $false $true
  $r = Invoke-Harness -FromReport $file
  $md = Get-Content -Raw (Join-Path $root 'atlas/report.md')
  Test-That 'the captures line names the atlas step and the one-camera comparison' { $md -match 'Containers built with `--atlas repack`' -and $md -match 'compared with the kept build''s from one camera, orbiting lights off' }
  Test-That 'the atlas table carries the charts, the proxy charts, the folds and the kept comparison' {
    $md -match '\| solid-crate \| 61 \(58\) \| 233 \| 2048 \| 71\.0 \| 1\.17 / 13\.0 \| 1\.04 / 21\.7 \|' -and $md -match '\| 0\.0141 \(0\.031\) \| 38\.8 \|' }
  Test-That 'an asset with neither block is not in that table' { -not ($md -match '\| thin-tree \| — \(') }

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

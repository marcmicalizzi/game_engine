#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Render the scene corpus through the real-time path and through the reference path tracer,
  compare them, and fail when a scene's FLIP exceeds the threshold stored with it.

.DESCRIPTION
  Plan 04 §4.8's gate. Every file in `content/test-scenes/` says what to load, what to draw it
  with, where the camera is, how many samples the reference spends, and how far the two pictures
  may differ; this script runs each one through `render.evaluate` on one `engine-host --stdio`
  process, writes a JSON report with FLIP mean and p95, PSNR, SSIM, the render times and the
  machine's state, and exits 1 when a scene is over its threshold.

  **What the gate is for.** The real-time path has no global illumination, so the two pictures
  cannot meet and a threshold set where they would meet would only mean "this never passes". The
  thresholds are today's numbers with margin, and what they catch is a *regression*. So that a
  large threshold can be read rather than guessed at, every scene is also compared against the
  reference at **one bounce**, and the report carries both: the gap between them is the indirect
  light the real-time path does not have.

  **What it needs.** A device with cluster acceleration structures and ray queries — the
  reference traces the structures the frame builds, and there are none without them. On any other
  machine every scene comes back "unavailable", the report says so, and the script exits 0,
  because a machine that cannot run the gate has not failed it. A scene whose sample assets are
  missing (`tools/fetch-samples.ps1` fetches them and git ignores them) is skipped the same way.

  **Measuring on this box.** The report carries `render.evaluate`'s machine-state span for every
  scene, so a run taken beside other work is readable as the upper bound it is
  (docs/subsystems/bench.md). `-RequireQuiet` refuses to start when the machine is busy.

      pwsh tools/ci/reference-compare.ps1 [-Preset msvc-release] [-OutDir <dir>]
                                          [-Scene <name>]... [-Spp <n>] [-UpdateThresholds]
                                          [-Margin 1.5] [-RequireQuiet]

  `-UpdateThresholds` writes the measured numbers back into the scene files with `-Margin` times
  the headroom, which is how the first set was produced and how a deliberate change to the
  real-time path is accepted. It is never run in CI.
#>
[CmdletBinding()]
param(
  [string]$Preset = 'msvc-release',
  [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path,
  [string]$OutDir = '',
  [string[]]$Scene = @(),
  [int]$Spp = 0,
  [switch]$UpdateThresholds,
  [double]$Margin = 1.5,
  [switch]$RequireQuiet
)

$ErrorActionPreference = 'Stop'

function Resolve-Host {
  param([string]$root, [string]$preset)
  $exe = if ($IsWindows) { 'engine-host.exe' } else { 'engine-host' }
  $path = Join-Path $root "build/$preset/bin/$exe"
  if (-not (Test-Path $path)) {
    throw "no engine-host at $path; build it first: tools/dev.ps1 build -Preset $preset"
  }
  return (Resolve-Path $path).Path
}

# One engine-host process fed one request per line. A scene id lives in the host, so the whole
# corpus goes through one process: loading a mesh is the expensive part and the host keeps it.
class RpcHost {
  [System.Diagnostics.Process]$Process
  [int]$NextId = 1

  RpcHost([string]$exe) {
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $exe
    $info.Arguments = '--stdio'
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $false
    $info.UseShellExecute = $false
    $this.Process = [System.Diagnostics.Process]::Start($info)
  }

  [pscustomobject] Call([string]$method, [hashtable]$params) {
    $request = @{
      jsonrpc = '2.0'
      id      = $this.NextId++
      method  = $method
      params  = $params
    } | ConvertTo-Json -Depth 20 -Compress
    $this.Process.StandardInput.WriteLine($request)
    $this.Process.StandardInput.Flush()
    $line = $this.Process.StandardOutput.ReadLine()
    if ($null -eq $line) { throw "engine-host closed the connection during $method" }
    return ($line | ConvertFrom-Json)
  }

  [void] Close() {
    $this.Process.StandardInput.Close()
    $this.Process.WaitForExit(10000) | Out-Null
  }
}

# A PSCustomObject (what ConvertFrom-Json gives) into the hashtable ConvertTo-Json round-trips.
function ConvertTo-Hashtable {
  param($value)
  if ($null -eq $value) { return $null }
  if ($value -is [System.Management.Automation.PSCustomObject]) {
    $out = @{}
    foreach ($property in $value.PSObject.Properties) {
      $out[$property.Name] = ConvertTo-Hashtable $property.Value
    }
    return $out
  }
  if ($value -is [System.Collections.IEnumerable] -and $value -isnot [string]) {
    return @($value | ForEach-Object { ConvertTo-Hashtable $_ })
  }
  return $value
}

$hostExe = Resolve-Host $Root $Preset
if ([string]::IsNullOrEmpty($OutDir)) { $OutDir = Join-Path $Root "build/$Preset/reference" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$reportPath = Join-Path $OutDir 'reference-compare.json'

$sceneDir = Join-Path $Root 'content/test-scenes'
$files = Get-ChildItem -Path $sceneDir -Filter '*.json' | Sort-Object Name
if ($Scene.Count -gt 0) {
  $files = $files | Where-Object { $Scene -contains $_.BaseName }
  if ($files.Count -eq 0) { throw "no scene in $sceneDir matches $($Scene -join ', ')" }
}

# The corpus is rendered from one process, and the working directory is the repository root so a
# scene's relative mesh paths mean what they say.
Push-Location $Root
$results = @()
$failures = @()
$rpc = $null
try {
  $rpc = [RpcHost]::new($hostExe)
  foreach ($file in $files) {
    $definition = Get-Content -Raw -Path $file.FullName | ConvertFrom-Json
    $name = if ($definition.name) { $definition.name } else { $file.BaseName }
    Write-Host "reference-compare: $name" -NoNewline

    $missing = @()
    foreach ($required in @($definition.requires)) {
      if ($required -and -not (Test-Path (Join-Path $Root $required))) { $missing += $required }
    }
    if ($missing.Count -gt 0) {
      Write-Host "  skipped (missing $($missing -join ', '))"
      $results += [ordered]@{ scene = $name; status = 'skipped'
                              reason = "missing content: $($missing -join ', ')" }
      continue
    }

    $loadParams = ConvertTo-Hashtable $definition.load
    if ($null -eq $loadParams) { $loadParams = @{} }
    $loadParams['settings'] = ConvertTo-Hashtable $definition.settings
    $loaded = $rpc.Call('render.load', $loadParams)
    if ($loaded.error) {
      # 1007 is "this machine cannot render that", which is not a failed gate.
      $status = if ($loaded.error.code -eq 1007) { 'unavailable' } else { 'error' }
      Write-Host "  $status ($($loaded.error.message))"
      $results += [ordered]@{ scene = $name; status = $status; reason = $loaded.error.message }
      if ($status -eq 'error') { $failures += "$name`: $($loaded.error.message)" }
      continue
    }

    $reference = ConvertTo-Hashtable $definition.reference
    if ($null -eq $reference) { $reference = @{} }
    if ($Spp -gt 0) { $reference['spp'] = $Spp }
    $evaluate = @{
      scene       = $loaded.result.scene
      width       = $definition.width
      height      = $definition.height
      frame       = $definition.frame
      reference   = $reference
      direct_only = $true
      out_dir     = $OutDir
      name        = $name
    }
    if ($definition.orbit) { $evaluate['orbit'] = ConvertTo-Hashtable $definition.orbit }
    if ($definition.camera) { $evaluate['camera'] = ConvertTo-Hashtable $definition.camera }
    $evaluated = $rpc.Call('render.evaluate', $evaluate)
    if ($evaluated.error) {
      $status = if ($evaluated.error.code -eq 1007) { 'unavailable' } else { 'error' }
      Write-Host "  $status ($($evaluated.error.message))"
      $results += [ordered]@{ scene = $name; status = $status; reason = $evaluated.error.message }
      if ($status -eq 'error') { $failures += "$name`: $($evaluated.error.message)" }
      continue
    }

    $r = $evaluated.result
    $meanLimit = [double]$definition.thresholds.flip_mean
    $p95Limit = [double]$definition.thresholds.flip_p95
    $over = @()
    if ($meanLimit -gt 0 -and $r.full.flip_mean -gt $meanLimit) {
      $over += ("flip_mean {0:N4} > {1:N4}" -f $r.full.flip_mean, $meanLimit)
    }
    if ($p95Limit -gt 0 -and $r.full.flip_p95 -gt $p95Limit) {
      $over += ("flip_p95 {0:N4} > {1:N4}" -f $r.full.flip_p95, $p95Limit)
    }
    $status = if ($over.Count -gt 0) { 'failed' } else { 'ok' }
    if ($meanLimit -le 0 -and $p95Limit -le 0) { $status = 'unset' }
    Write-Host ("  {0}  FLIP {1:N4} (p95 {2:N4}), direct-only {3:N4}, PSNR {4:N1} dB, SSIM {5:N4}, {6:N1} s" -f `
        $status, $r.full.flip_mean, $r.full.flip_p95, $r.direct.flip_mean, $r.full.psnr, `
        $r.full.ssim, ($r.realtime_seconds + $r.reference_seconds))
    if ($over.Count -gt 0) { $failures += "$name`: $($over -join ', ')" }

    $results += [ordered]@{
      scene              = $name
      status             = $status
      width              = $r.width
      height             = $r.height
      spp                = $r.samples
      bounces            = $reference['bounces']
      visible_pairs      = $r.stats.visible_pairs
      # Where the geometry came from — "miss" built from the glTF, "hit" read from the derived-data
      # cache, "file" a container named outright, "none" the heightfield. A number whose provenance
      # is not recorded is a number nobody can reproduce, and this field has already earned its
      # place once: it is what identified a `thin-geometry` result that alternated between 0.0116
      # and 0.0051 as a *container* defect rather than a rendering one — a `.clusters` file did not
      # carry the images its GLB embedded, so the Lantern drew untextured from the cache and
      # textured from the source (docs/subsystems/geometry.md, "Embedded images"). Fixed on
      # 2026-09-18; the corpus now gives the same numbers cold and warm, and the field stays,
      # because the next divergence will not announce itself either.
      mesh_cache         = $loaded.result.mesh_cache
      full               = [ordered]@{ flip_mean = $r.full.flip_mean; flip_p95 = $r.full.flip_p95
                                       flip_max = $r.full.flip_max; psnr = $r.full.psnr
                                       ssim = $r.full.ssim }
      # The same real-time picture against the reference at one bounce. `full` minus this is the
      # indirect light the real-time path does not have, which is what the threshold is mostly
      # made of on a scene with interiors.
      direct_only        = [ordered]@{ flip_mean = $r.direct.flip_mean; flip_p95 = $r.direct.flip_p95
                                       flip_max = $r.direct.flip_max; psnr = $r.direct.psnr
                                       ssim = $r.direct.ssim }
      indirect_share     = if ($r.full.flip_mean -gt 0) {
                             [math]::Round(1.0 - ($r.direct.flip_mean / $r.full.flip_mean), 4)
                           } else { 0 }
      thresholds         = [ordered]@{ flip_mean = $meanLimit; flip_p95 = $p95Limit }
      realtime_seconds   = $r.realtime_seconds
      reference_seconds  = $r.reference_seconds
      reference_trace_ms = $r.reference_trace_ms
      metrics_ms         = $r.metrics_ms
      realtime_gpu_ms    = $r.stats.gpu_ms
      gpu_memory         = $r.stats.gpu_memory
      machine_state      = $r.machine_state
      files              = $r.files
    }

    if ($UpdateThresholds) {
      $definition.thresholds.flip_mean = [math]::Round($r.full.flip_mean * $Margin, 4)
      $definition.thresholds.flip_p95 = [math]::Round($r.full.flip_p95 * $Margin, 4)
      $definition | ConvertTo-Json -Depth 20 | Set-Content -Path $file.FullName -Encoding utf8
      Write-Host ("  thresholds updated to {0:N4} / {1:N4}" -f $definition.thresholds.flip_mean,
                                                              $definition.thresholds.flip_p95)
    }
  }
} finally {
  if ($null -ne $rpc) { $rpc.Close() }
  Pop-Location
}

$report = [ordered]@{
  format  = 'engine.reference-compare.v1'
  preset  = $Preset
  taken   = (Get-Date).ToUniversalTime().ToString('o')
  commit  = (& git -C $Root rev-parse --short HEAD 2>$null)
  scenes  = $results
  failed  = $failures
}
$report | ConvertTo-Json -Depth 20 | Set-Content -Path $reportPath -Encoding utf8
Write-Host "reference-compare: report at $reportPath"

if ($RequireQuiet) {
  foreach ($entry in $results) {
    if ($entry.machine_state -and $entry.machine_state.start.cpu_others_pct -gt 25) {
      Write-Warning "the machine was busy while $($entry.scene) ran; the times are upper bounds"
    }
  }
}
if ($failures.Count -gt 0) {
  Write-Host ''
  Write-Host 'reference-compare: FAILED' -ForegroundColor Red
  foreach ($failure in $failures) { Write-Host "  $failure" }
  exit 1
}
exit 0

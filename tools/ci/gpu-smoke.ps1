#!/usr/bin/env pwsh
<#
.SYNOPSIS
  GPU smoke run for a self-hosted Windows runner: build, report the machine's
  Vulkan adapters, run the tests, render the extreme resolutions.

.DESCRIPTION
  tools/ci/gpu-smoke.ps1 [-Preset <name>] [-Filter <regex>] [-Clean]
                         [-RequireAdapters] [-SkipCaptures]
                         [-CaptureTimeout <seconds>] [-CaptureFrames <n>]

  Four steps, in order:

    1. tools/dev.ps1 build -Preset <preset>  - configures first if needed, and on
       Windows imports the Visual Studio developer environment into this process,
       so cmake, ninja, and ctest resolve for the rest of the script.
    2. build/<preset>/bin/engine-cli gpu.adapters, printed in full. This is the
       machine's capability report and it is what explains the tests that skip:
       a GPU without mesh shaders skips the mesh-shader, software-raster,
       occlusion, and shading tests and still runs the vertex path, the cull and
       LOD tests, the resolve reflection, and everything on the CPU.
    3. ctest --preset <preset>. The log it leaves in
       build/<preset>/Testing/Temporary/LastTest.log is what CI keeps.
    4. engine-view at 3840x2160 and at the four resolutions plan 04 section 4.6
       asks CI to render nightly - 11520x2160 surround, 7680x4320, 1080x3840
       portrait, 5120x1440 ultrawide - each with --frames <n> --capture. The
       JSON summary of each run, the adapter report, and the ctest status are
       written to build/<preset>/gpu-smoke.json, and the pictures to
       build/<preset>/captures/. Every resolution is attempted whatever the ones
       before it did: each has its own time budget and its own result, so one
       that crashes or hangs cannot hide the rest.

  On a machine whose GPU other agents share, the suite and the captures wait for
  the card rather than run beside a render, and the log says whom they waited
  for: the tests take the machine-wide GPU lock themselves, per GPU device
  (docs/subsystems/gpu_lock.md), and step 4 runs under the lock this script takes
  (tools/lib/MachineLock.psm1, docs/subsystems/bench.md "The GPU lock"). A machine
  without the lock's directory (D:\workspace, or wherever ENGINE_GPU_LOCK points)
  runs without it.

  Default preset: msvc-release on Windows, linux-clang-debug elsewhere (the
  Linux runner uses tools/ci/gpu-smoke.sh, which does the same four steps).

  Adapter enumeration that finds no device is a warning, not a failure, so a
  driver problem does not look like a test failure; -RequireAdapters turns it
  into one, which is what a machine that is supposed to have a GPU wants. A
  resolution where engine-view exits 3 (no display, no Vulkan device, no mesh
  shaders, no presentation) is a skip for the same reason; any other nonzero
  exit, or a run that outlives its budget, fails the script after all five have
  been tried.

  The script's exit status is ctest's, or 1 when ctest passed and a capture
  failed.

.EXAMPLE
  tools/ci/gpu-smoke.ps1
  tools/ci/gpu-smoke.ps1 -Preset msvc-debug -Filter gfx -RequireAdapters
  tools/ci/gpu-smoke.ps1 -SkipCaptures
#>
[CmdletBinding()]
param(
  [string]$Preset,
  [string]$Filter,
  [switch]$Clean,
  [switch]$RequireAdapters,
  [switch]$SkipCaptures,
  [int]$CaptureTimeout = 300,
  [int]$CaptureFrames = 60
)

$ErrorActionPreference = 'Stop'
# PowerShell 7.4 and later turn a nonzero native exit code into a terminating
# error when ErrorActionPreference is Stop. Every step here checks its own exit
# code, and the ctest status is the script's result, so keep the old behaviour.
$PSNativeCommandUseErrorActionPreference = $false

$Root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
Import-Module (Join-Path $Root 'tools/lib/MachineLock.psm1') -Force
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
if (-not $Preset) { $Preset = if ($IsWin) { 'msvc-release' } else { 'linux-clang-debug' } }

$Dev = Join-Path $Root 'tools/dev.ps1'
$BuildDir = Join-Path $Root "build/$Preset"

function Write-Section([string]$Text) {
  Write-Host ''
  Write-Host "== $Text" -ForegroundColor Cyan
}

Write-Section "gpu smoke on $([Environment]::MachineName): preset $Preset"
Write-Host "os:        $([Environment]::OSVersion.VersionString)"
Write-Host "cpus:      $([Environment]::ProcessorCount)"
Write-Host "root:      $Root"

if ($Clean) {
  Write-Section 'clean'
  & $Dev clean -Preset $Preset
}

Write-Section 'build'
& $Dev build -Preset $Preset

# ---- the GPU lock ----------------------------------------------------------
#
# On a machine that may share its card with other agents (docs/subsystems/bench.md,
# "The GPU lock") the heavy GPU work runs under the machine-wide lock. The adapter
# report opens no device and needs none. The suite takes it itself: every test
# process holds it for as long as it has a GPU device open and no longer
# (docs/subsystems/gpu_lock.md, ADR-0050), so wrapping ctest would only hold the
# card through the suite's CPU-only hour. The captures are this script's own
# engine-view runs, which take nothing by themselves, so the script takes the
# lock around them — just before the first, released before the report is
# written, whatever happened in between. A runner that does not use the lock (no
# D:\workspace, or ENGINE_GPU_LOCK naming a directory that does not exist) warns
# and runs without it. engine-view inherits ENGINE_GPU_LOCK_HOLDER, so its
# machine-state samples know the lock is held on its behalf.
$gpuLock = $null
try {
  Write-Section 'gpu.adapters'
  $cliName = if ($IsWin) { 'engine-cli.exe' } else { 'engine-cli' }
  $Cli = Join-Path $BuildDir "bin/$cliName"
  if (-not (Test-Path $Cli)) { throw "engine-cli not found at $Cli; did the build produce it?" }

  $adapters = & $Cli gpu.adapters 2>&1
  $adaptersStatus = $LASTEXITCODE
  $adaptersText = ($adapters | Out-String)
  Write-Host $adaptersText.TrimEnd()

  # gpu.adapters answers the driver question in its result, not in its exit code:
  # it returns available=false with an error when the loader or the ICD behind it
  # is missing, which is what a hosted runner looks like, and available=true with
  # an empty list when an ICD is installed but no device is reachable.
  $available = $false
  $adapterCount = 0
  $adapterError = ''
  if ($adaptersStatus -eq 0) {
    try {
      $parsed = $adaptersText | ConvertFrom-Json
      $available = [bool]$parsed.available
      $adapterCount = @($parsed.adapters).Count
      $adapterError = "$($parsed.error)"
    }
    catch { $adapterError = "gpu.adapters printed something that is not JSON: $($_.Exception.Message)" }
  }
  else { $adapterError = "engine-cli exited $adaptersStatus" }

  if (-not $available -or $adapterCount -eq 0) {
    $reason = if ($adapterError) { $adapterError } else { 'the loader reported no physical device' }
    $message = "no usable Vulkan device on this machine ($reason), so every GPU test will skip. " +
               'Check that the display driver is installed and that its ICD is registered ' +
               '(see docs/ci/self-hosted-runners.md).'
    if ($RequireAdapters) { throw $message }
    Write-Warning $message
  }
  else { Write-Host "$adapterCount adapter(s) reported" }

  Write-Section 'ctest'
  if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) {
    throw 'ctest not found on PATH. On Windows tools/dev.ps1 supplies it from the Visual Studio installation; install the C++ workload.'
  }
  $ctestArgs = @('--preset', $Preset)
  if ($Filter) { $ctestArgs += @('-R', $Filter) }
  Push-Location $Root
  try {
    & ctest @ctestArgs
    $status = $LASTEXITCODE
  }
  finally { Pop-Location }

  $log = Join-Path $BuildDir 'Testing/Temporary/LastTest.log'

  # ---- extreme-resolution captures (plan 04 section 4.6) ---------------------
  #
  # The four resolutions the plan names, plus plain 4K as the number the others
  # are read against. They are here because "no 16-bit screen coordinates
  # anywhere, all screen-space structures sized from the render config" is a
  # claim that only a run at 48:9 and at 8K can check: an overflow in a tile
  # count, a Hi-Z level count, or a workgroup dispatch shows up as a wrong
  # picture or a device loss, and nothing smaller finds it.
  $resolutions = @(
    [pscustomobject]@{ Width = 3840;  Height = 2160; Why = '4K, the baseline the others are read against' },
    [pscustomobject]@{ Width = 11520; Height = 2160; Why = 'triple 4K surround, 48:9' },
    [pscustomobject]@{ Width = 7680;  Height = 4320; Why = '8K' },
    [pscustomobject]@{ Width = 1080;  Height = 3840; Why = 'portrait' },
    [pscustomobject]@{ Width = 5120;  Height = 1440; Why = 'ultrawide' }
  )

  $capturesDir = Join-Path $BuildDir 'captures'
  $captureResults = @()
  $captureFailures = 0
  if ($SkipCaptures) {
    Write-Section 'extreme-resolution captures (skipped by -SkipCaptures)'
  }
  else {
    Write-Section 'extreme-resolution captures'
    $viewName = if ($IsWin) { 'engine-view.exe' } else { 'engine-view' }
    $View = Join-Path $BuildDir "bin/$viewName"
    if (-not (Test-Path $View)) {
      Write-Warning "engine-view not found at $View; skipping the captures"
    }
    else {
      New-Item -ItemType Directory -Force -Path $capturesDir | Out-Null
      Write-Section 'gpu lock'
      $gpuLock = Open-MachineLockSession -Kind gpu -Purpose "gpu-smoke $Preset captures on $([Environment]::MachineName)"
      foreach ($r in $resolutions) {
        $name = "$($r.Width)x$($r.Height)"
        $capture = Join-Path $capturesDir "$name.png"
        $stdoutPath = Join-Path $capturesDir "$name.stdout.txt"
        $stderrPath = Join-Path $capturesDir "$name.stderr.txt"
        Remove-Item -Force -ErrorAction SilentlyContinue $capture, $stdoutPath, $stderrPath
        $arguments = @('--width', "$($r.Width)", '--height', "$($r.Height)",
                       '--frames', "$CaptureFrames", '--capture', $capture)
        Write-Host "-- $name ($($r.Why))"
        $started = Get-Date
        # Its own time budget, its own result: one resolution that hangs or dies
        # must not stop the others from being tried.
        $proc = Start-Process -FilePath $View -ArgumentList $arguments -NoNewWindow -PassThru `
                              -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        $finished = $proc.WaitForExit($CaptureTimeout * 1000)
        if (-not $finished) {
          try { $proc.Kill($true) } catch { }
          try { $proc.WaitForExit(10000) | Out-Null } catch { }
        }
        $seconds = [math]::Round((Get-Date).Subtract($started).TotalSeconds, 2)
        $exitCode = if ($finished) { $proc.ExitCode } else { $null }

        $summary = $null
        if (Test-Path $stdoutPath) {
          foreach ($line in (Get-Content -LiteralPath $stdoutPath)) {
            $trimmed = $line.Trim()
            if ($trimmed.StartsWith('{')) {
              try { $summary = $trimmed | ConvertFrom-Json } catch { }
            }
          }
        }

        # engine-view exits 3 when it cannot draw at all (no display, no Vulkan
        # device, no mesh shaders, no presentation). That is a skip on a headless
        # server or a runner that is a service, not a failure.
        $state = if (-not $finished) { 'timeout' }
                 elseif ($exitCode -eq 0) { 'ok' }
                 elseif ($exitCode -eq 3) { 'skipped' }
                 else { 'failed' }
        if ($state -eq 'ok' -or $state -eq 'skipped') { } else { $captureFailures++ }

        $result = [ordered]@{
          name = $name
          why = $r.Why
          requested_width = $r.Width
          requested_height = $r.Height
          status = $state
          exit_code = $exitCode
          seconds = $seconds
          timeout_seconds = $CaptureTimeout
          frames = $CaptureFrames
          capture = if ($state -eq 'ok' -and (Test-Path $capture)) { "captures/$name.png" } else { $null }
          summary = $summary
        }
        # engine-view is a windowed app: a window manager may hand back a smaller
        # surface than 11520x2160 was asked for, and the summary's width and
        # height are what was really rendered. Say so rather than let the file
        # claim a resolution that never reached a rasterizer.
        if ($summary -ne $null -and $summary.width -ne $null) {
          $result.rendered_width = [int]$summary.width
          $result.rendered_height = [int]$summary.height
          $result.clamped = ([int]$summary.width -ne $r.Width -or [int]$summary.height -ne $r.Height)
        }
        $captureResults += [pscustomobject]$result

        switch ($state) {
          'ok' {
            $rendered = if ($result.rendered_width) { "$($result.rendered_width)x$($result.rendered_height)" } else { '?' }
            $note = if ($result.clamped) { " (the window manager gave $rendered)" } else { '' }
            Write-Host "   ok in $seconds s$note"
          }
          'skipped' { Write-Host "   skipped (engine-view exited 3: nothing to draw with)" }
          'timeout' { Write-Warning "   no result within $CaptureTimeout s; the process was killed" }
          default   { Write-Warning "   engine-view exited $exitCode after $seconds s; see $stderrPath" }
        }
      }
    }
  }
}
finally {
  Close-MachineLockSession -Session $gpuLock
}

# One machine-readable file for the whole run, beside the ctest log: what the
# GPU is, what the tests did, and what came out of each resolution.
$adapterReport = $null
if ($null -ne $parsed) { $adapterReport = $parsed }
$report = [ordered]@{
  machine = [Environment]::MachineName
  os = [Environment]::OSVersion.VersionString
  preset = $Preset
  generated_utc = (Get-Date).ToUniversalTime().ToString('o')
  ctest_status = $status
  adapters = $adapterReport
  adapter_error = $adapterError
  captures = $captureResults
}
$reportPath = Join-Path $BuildDir 'gpu-smoke.json'
($report | ConvertTo-Json -Depth 12) | Set-Content -LiteralPath $reportPath -Encoding utf8

Write-Section 'result'
if (Test-Path $log) { Write-Host "ctest log: $log" } else { Write-Warning "no ctest log at $log" }
Write-Host "report:   $reportPath"
foreach ($c in $captureResults) { Write-Host ("captures: {0,-11} {1}" -f $c.name, $c.status) }
Write-Host "ctest exit status: $status"
if ($status -eq 0 -and $captureFailures -gt 0) {
  Write-Warning "$captureFailures resolution(s) did not render; failing the run"
  $status = 1
}
exit $status

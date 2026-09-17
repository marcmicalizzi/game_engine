#!/usr/bin/env pwsh
<#
.SYNOPSIS
  GPU smoke run for a self-hosted Windows runner: build, report the machine's
  Vulkan adapters, run the tests.

.DESCRIPTION
  tools/ci/gpu-smoke.ps1 [-Preset <name>] [-Filter <regex>] [-Clean] [-RequireAdapters]

  Three steps, in order:

    1. tools/dev.ps1 build -Preset <preset>  - configures first if needed, and on
       Windows imports the Visual Studio developer environment into this process,
       so cmake, ninja, and ctest resolve for the rest of the script.
    2. build/<preset>/bin/engine-cli gpu.adapters, printed in full. This is the
       machine's capability report and it is what explains the tests that skip:
       a GPU without mesh shaders skips the mesh-shader, software-raster,
       occlusion, and shading tests and still runs the vertex path, the cull and
       LOD tests, the resolve reflection, and everything on the CPU.
    3. ctest --preset <preset>. The script exits with ctest's status; the log it
       leaves in build/<preset>/Testing/Temporary/LastTest.log is what CI keeps.

  Default preset: msvc-release on Windows, linux-clang-debug elsewhere (the
  Linux runner uses tools/ci/gpu-smoke.sh, which calls the same steps).

  Adapter enumeration that finds no device is a warning, not a failure, so a
  driver problem does not look like a test failure; -RequireAdapters turns it
  into one, which is what a machine that is supposed to have a GPU wants.

.EXAMPLE
  tools/ci/gpu-smoke.ps1
  tools/ci/gpu-smoke.ps1 -Preset msvc-debug -Filter gfx -RequireAdapters
#>
[CmdletBinding()]
param(
  [string]$Preset,
  [string]$Filter,
  [switch]$Clean,
  [switch]$RequireAdapters
)

$ErrorActionPreference = 'Stop'
# PowerShell 7.4 and later turn a nonzero native exit code into a terminating
# error when ErrorActionPreference is Stop. Every step here checks its own exit
# code, and the ctest status is the script's result, so keep the old behaviour.
$PSNativeCommandUseErrorActionPreference = $false

$Root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
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
Write-Section 'result'
if (Test-Path $log) { Write-Host "ctest log: $log" } else { Write-Warning "no ctest log at $log" }
Write-Host "ctest exit status: $status"
exit $status

#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Pack a preset's tests, benches and apps into a zip that runs on a machine with
  no toolchain, no checkout and no PowerShell 7.

.DESCRIPTION
  tools/package-tests.ps1 [-Preset <name>] [-Out <zip>] [-WithSamples]
                          [-WithSymbols] [-Build] [-NoZip]

  The owner has machines this engine has never run on and little patience for
  setting them up: a Maxwell desktop with 8 GB of free disk, a Surface Pro, a
  2014 laptop. Installing Visual Studio, CMake, Git and PowerShell 7 on each of
  them to find out whether a shader compiles is not the way to learn it. So:
  one zip, one double-click, one file to send back.

  What goes in:

    bin/                every test, bench and app executable the preset built,
                        flattened into one directory (no PDBs unless
                        -WithSymbols). Flattened because bundle.json names each
                        test by executable and the layout under build/ is a
                        CMake detail nobody on the far end should have to know.
    content/input-logs  the device-log corpus foundation/input replays. Bundled
                        rather than skipped: it is 176 KB and it is the only
                        description of those three devices the project has.
                        Its sessions/ are engine-view's synthetic session and
                        the trajectory it has to fly.
    content/input-maps  engine-view's default bindings, which its tests hold
                        against the compiled-in map (4 KB).
    content/roles       the five named role configurations, which the protocol
                        test reads to check the shipped file loads (3 KB).
    content/migration-corpus
                        one save game per version of the save format, which
                        engine-cli's corpus test loads and holds to its
                        state hashes (about 110 KB a save).
    content/test-scenes the reference-scene definitions (17 KB), for a later
                        job that wants them.
    content/samples     the Khronos glTF models, with -WithSamples only. They
                        are hundreds of megabytes and nothing in the suite
                        needs them.
    bundle.json         commit, preset, CPU baseline, and the test list with
                        each test's arguments, labels, resource lock and
                        timeout, so the runner does not have to guess.
    run-tests.ps1       the runner. Windows PowerShell 5.1 only: no pwsh 7, no
                        CMake, no ctest, no Visual Studio on the target.
    run-tests.cmd       so it can be double-clicked.
    README.txt          four lines for the person at the other end.

  What stays out, and why:

    shaders/manifest.json  it names absolute paths into the build tree and the
                        pinned Slang compiler under _deps, which is a toolchain
                        and not a fixture. The two shader-library cases that
                        need it say so and stop; the reflection of every shipped
                        shader runs from the embedded SPIR-V and needs no file.
    the CTest tests that are really scripts (lint, docs-check, the tools tests,
                        the docs gate) - they run PowerShell 7 or bash over the
                        source tree, and a bundle has neither. bundle.json lists
                        them under `excluded_tests` with the reason, so the far
                        end can see that they were left out on purpose.

  The bundle is self-locating: run-tests.ps1 sets ENGINE_BUNDLE_ROOT, and
  tests/support/test_paths.h makes that **take over** from the paths CMake baked
  in rather than merely be a fallback. That is what lets the bundle be verified
  on the machine that built it: a fallback would quietly find the build tree,
  pass, and prove nothing.

.EXAMPLE
  tools/package-tests.ps1 -Preset msvc-release
  tools/package-tests.ps1 -Preset msvc-release -Out D:\out\engine-tests.zip -WithSamples
#>
[CmdletBinding()]
param(
  [string]$Preset,
  [string]$Out,
  [switch]$WithSamples,
  [switch]$WithSymbols,
  [switch]$Build,
  [switch]$NoZip
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
if (-not $Preset) { $Preset = if ($IsWin) { 'msvc-release' } else { 'linux-clang-debug' } }
$BuildDir = Join-Path $Root "build/$Preset"
$ExeSuffix = if ($IsWin) { '.exe' } else { '' }

function Write-Section([string]$Text) {
  Write-Host ''
  Write-Host "== $Text" -ForegroundColor Cyan
}

# ctest is not on PATH outside a developer prompt; tools/dev.ps1 imports the whole VS environment
# for the build, and this script needs one program out of it.
function Find-CTest {
  $onPath = Get-Command ctest -ErrorAction SilentlyContinue
  if ($onPath) { return $onPath.Source }
  if (-not $IsWin) { throw 'ctest not found on PATH.' }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsPath) {
      $candidate = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
      if (Test-Path $candidate) { return $candidate }
    }
  }
  throw 'ctest not found on PATH or in the Visual Studio installation; run this from a developer prompt.'
}

if ($Build) {
  Write-Section "build $Preset"
  & (Join-Path $Root 'tools/dev.ps1') build -Preset $Preset
  if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}

if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
  throw "no build at $BuildDir. Run tools/dev.ps1 build -Preset $Preset (or pass -Build)."
}

Write-Section "packaging $Preset"

# ---- what the build says about itself ------------------------------------------------------

$commit = ''
$branch = ''
$dirty = $false
try {
  Push-Location $Root
  $commit = (& git rev-parse HEAD 2>$null | Out-String).Trim()
  $branch = (& git rev-parse --abbrev-ref HEAD 2>$null | Out-String).Trim()
  $status = (& git status --porcelain 2>$null | Out-String).Trim()
  $dirty = -not [string]::IsNullOrEmpty($status)
}
catch { }
finally { Pop-Location }
$commitShort = if ($commit.Length -ge 7) { $commit.Substring(0, 7) } else { 'nocommit' }

# The CPU baseline is another agent's switch and may not exist yet: read it out of the cache if
# it is there and say "unknown" if it is not, rather than inventing a value or failing.
$cpuBaseline = 'unknown'
$cachePath = Join-Path $BuildDir 'CMakeCache.txt'
foreach ($line in (Get-Content -LiteralPath $cachePath)) {
  if ($line -match '^ENGINE_CPU_BASELINE(?::[A-Z]+)?=(.*)$') {
    $value = $Matches[1].Trim()
    if ($value) { $cpuBaseline = $value }
    break
  }
}

# ---- the test list, from ctest itself ------------------------------------------------------
#
# Not from a glob over the build directory: ctest is the one place that knows a test's arguments,
# its labels, its RESOURCE_LOCK and its timeout, and those are exactly what the runner needs in
# order to serialize the GPU end-to-end tests the way a ctest run does.

$ctest = Find-CTest
Push-Location $BuildDir
try {
  $listing = & $ctest --show-only=json-v1 2>&1 | Out-String
  if ($LASTEXITCODE -ne 0) { throw "ctest --show-only failed ($LASTEXITCODE): $listing" }
}
finally { Pop-Location }
$catalogue = $listing | ConvertFrom-Json

function Get-Property($test, [string]$name) {
  if (-not $test.properties) { return $null }
  foreach ($p in $test.properties) {
    if ($p.name -eq $name) { return $p.value }
  }
  return $null
}

$buildFull = (Resolve-Path $BuildDir).Path
$tests = @()
$excluded = @()
$wanted = @{}   # full path -> $true, the executables the tests actually need

foreach ($t in $catalogue.tests) {
  $command = @($t.command)
  if ($command.Count -eq 0) {
    $excluded += [ordered]@{ name = $t.name; reason = 'no command' }
    continue
  }
  $exe = $command[0]
  $isOurs = $false
  if (Test-Path -LiteralPath $exe -PathType Leaf) {
    $full = (Resolve-Path -LiteralPath $exe).Path
    $isOurs = $full.StartsWith($buildFull, [System.StringComparison]::OrdinalIgnoreCase) -and
              ($full -notmatch '[\\/]_deps[\\/]')
  }
  if (-not $isOurs) {
    $excluded += [ordered]@{
      name = $t.name
      reason = "runs '$([System.IO.Path]::GetFileName($exe))' over the source tree; a bundle has no checkout, no PowerShell 7 and no bash"
    }
    continue
  }
  $wanted[$full] = $true
  $lock = Get-Property $t 'RESOURCE_LOCK'
  $labels = Get-Property $t 'LABELS'
  $timeout = Get-Property $t 'TIMEOUT'
  $tests += [ordered]@{
    name = $t.name
    exe = 'bin/' + [System.IO.Path]::GetFileName($full)
    args = @($command | Select-Object -Skip 1)
    labels = @($labels)
    # A list in CTest; one lock is all this tree uses, and the runner treats it as a name.
    resource_lock = if ($lock) { @($lock)[0] } else { '' }
    timeout_seconds = if ($timeout) { [int]$timeout } else { 0 }
  }
}

Write-Host "tests:    $($tests.Count) from ctest, $($excluded.Count) excluded"

# ---- the executables -------------------------------------------------------------------------
#
# Every test's own executable, plus every app: `engine-cli` is what the runner asks for the
# adapter report, `engine-host` is what `engine-cli` and `engine-mcp` spawn, `engine-content` is
# what the engine-view tests drive, and `schemac` is a test's child process. Taking all of them is
# both simpler and more honest than trying to work out which app some test spawns: `engine-mcp`
# arrived after this was written and needed no line here.

$allExes = Get-ChildItem -Path $BuildDir -Recurse -File |
  Where-Object { $_.FullName -notmatch '[\\/]_deps[\\/]' } |
  Where-Object { if ($IsWin) { $_.Extension -eq '.exe' } else { $_.Extension -eq '' -and $_.Name -match '^(engine[_-]|schemac)' } }
foreach ($e in $allExes) { $wanted[$e.FullName] = $true }

$exeFiles = $wanted.Keys | Sort-Object
Write-Host "binaries: $($exeFiles.Count)"

# ---- lay the bundle out ----------------------------------------------------------------------

$stageName = "engine-tests-$Preset-$commitShort"
$stageRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("engine-bundle-" + [System.Guid]::NewGuid().ToString('N'))
$stage = Join-Path $stageRoot $stageName
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'bin') | Out-Null

$executables = @()
foreach ($path in $exeFiles) {
  $name = [System.IO.Path]::GetFileName($path)
  Copy-Item -LiteralPath $path -Destination (Join-Path $stage "bin/$name") -Force
  $executables += [ordered]@{ path = "bin/$name"; bytes = (Get-Item -LiteralPath $path).Length }
  if ($WithSymbols) {
    $pdb = [System.IO.Path]::ChangeExtension($path, '.pdb')
    if (Test-Path -LiteralPath $pdb) {
      Copy-Item -LiteralPath $pdb -Destination (Join-Path $stage 'bin') -Force
    }
  }
}

# Data the tests open. `content/input-logs`, `content/input-maps`, `content/roles` and
# `content/migration-corpus` are the ones the suite needs; the rest is small and useful to have on
# the far end.
$dataDirs = @('content/input-logs', 'content/input-maps', 'content/roles', 'content/test-scenes',
              'content/migration-corpus')
if ($WithSamples) { $dataDirs += 'content/samples' }
$data = @()
foreach ($rel in $dataDirs) {
  $source = Join-Path $Root $rel
  if (-not (Test-Path $source)) {
    Write-Warning "no $rel in this checkout; the bundle will not carry it"
    continue
  }
  $target = Join-Path $stage $rel
  New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
  Copy-Item -LiteralPath $source -Destination $target -Recurse -Force
  $bytes = (Get-ChildItem -Path $target -Recurse -File | Measure-Object Length -Sum).Sum
  $data += [ordered]@{ path = $rel; bytes = [int64]$bytes }
  Write-Host ("data:     {0} ({1:N1} MB)" -f $rel, ($bytes / 1MB))
}

# ---- bundle.json ------------------------------------------------------------------------------

$bundle = [ordered]@{
  schema = 1
  generated_utc = (Get-Date).ToUniversalTime().ToString('o')
  name = $stageName
  commit = $commit
  commit_short = $commitShort
  branch = $branch
  dirty = $dirty
  preset = $Preset
  # From the build's cache when the switch exists; "unknown" otherwise, which is the honest
  # answer on a tree that has not got it yet.
  cpu_baseline = $cpuBaseline
  built_on = [ordered]@{
    machine = [Environment]::MachineName
    os = [Environment]::OSVersion.VersionString
  }
  with_samples = [bool]$WithSamples
  with_symbols = [bool]$WithSymbols
  # What the far end needs installed. MSVC binaries need the 2015-2022 x64 redistributable and
  # nothing else: the Vulkan loader comes with the display driver.
  runtime = [ordered]@{
    dlls = @('VCRUNTIME140.dll', 'VCRUNTIME140_1.dll', 'MSVCP140.dll')
    installer = 'Microsoft Visual C++ 2015-2022 Redistributable (x64)'
    url = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'
  }
  executables = $executables
  data = $data
  tests = $tests
  excluded_tests = $excluded
}
$bundleJson = $bundle | ConvertTo-Json -Depth 12
[System.IO.File]::WriteAllText((Join-Path $stage 'bundle.json'), $bundleJson,
                               (New-Object System.Text.UTF8Encoding($false)))

Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bundle/run-tests.ps1') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bundle/run-tests.cmd') -Destination $stage -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'bundle/README.txt') -Destination $stage -Force

# ---- zip ---------------------------------------------------------------------------------------

$stageBytes = (Get-ChildItem -Path $stage -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("staged:   {0:N1} MB in {1}" -f ($stageBytes / 1MB), $stage)

# A relative -Out has no parent to create; make it absolute first so both branches below can
# just ask for the parent directory.
function Resolve-Out([string]$path) {
  if ([System.IO.Path]::IsPathRooted($path)) { return $path }
  return (Join-Path (Get-Location).Path $path)
}

if ($NoZip) {
  if (-not $Out) { $Out = Join-Path $BuildDir $stageName }
  $Out = Resolve-Out $Out
  if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
  New-Item -ItemType Directory -Force -Path (Split-Path $Out -Parent) | Out-Null
  Move-Item -LiteralPath $stage -Destination $Out
  Remove-Item -Recurse -Force $stageRoot -ErrorAction SilentlyContinue
  Write-Section 'result'
  Write-Host "bundle:   $Out"
  Write-Host ("size:     {0:N1} MB unpacked" -f ($stageBytes / 1MB))
  exit 0
}

if (-not $Out) { $Out = Join-Path $BuildDir "$stageName.zip" }
$Out = Resolve-Out $Out
New-Item -ItemType Directory -Force -Path (Split-Path $Out -Parent) | Out-Null
if (Test-Path $Out) { Remove-Item -Force $Out }
# Optimal rather than Fastest: this file is copied to a machine with 8 GB of free disk, over a
# USB stick or a home network, and the minute of CPU here is the cheapest part of the journey.
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
  $stageRoot, $Out, [System.IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item -Recurse -Force $stageRoot -ErrorAction SilentlyContinue

$zipBytes = (Get-Item -LiteralPath $Out).Length
Write-Section 'result'
Write-Host "bundle:   $Out"
Write-Host ("size:     {0:N1} MB zipped, {1:N1} MB unpacked" -f ($zipBytes / 1MB), ($stageBytes / 1MB))
Write-Host "contents: $($executables.Count) executables, $($tests.Count) tests, $($excluded.Count) excluded"
Write-Host ''
Write-Host 'On the far end: unzip, double-click run-tests.cmd, send back results.json,'
Write-Host 'results.txt and adapters.json. See docs/ci/self-hosted-runners.md.'

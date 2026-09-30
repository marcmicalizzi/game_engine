#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Single entry point for building, testing, and linting the engine.

.DESCRIPTION
  tools/dev.ps1 <command> [-Preset <name>] [-Filter <regex>] [-Verbose]

  Commands:
    configure   Run CMake with the preset (default: msvc-debug on Windows, linux-clang-debug on Linux).
    build       Build the preset (configures first if needed).
    test        Run CTest for the preset. -Filter is a regex on test names. Tests take the
                machine-wide GPU lock themselves, per GPU device, so do not wrap this in
                tools/gpu-lock.ps1 run: that still works, and holds the GPU for the whole
                run again (docs/subsystems/gpu_lock.md). Exits 75 when a test gave up
                waiting for the lock and so did not run; 1 when one failed.
    bench       Build, then run every engine_*_bench executable. -Filter is a glob on
                benchmark names; JSON lines land in build/<preset>/bench/<module>.jsonl.
                -GpuLock passes --gpu-lock to each: every executable waits for the
                machine-wide GPU lock, holds it while it measures, and releases it
                (docs/subsystems/bench.md, "The GPU lock").
    lint        Run the banned-pattern lint over the tree.
    docs        Check that the documentation moved with the code (tools/docs-check.ps1).
    format      Run clang-format in place over engine sources.
    modules     Print build/<preset>/modules.json.
    clean       Remove build/<preset>.

  On Windows the script locates Visual Studio through vswhere, imports the x64
  developer environment, and prefers the CMake and Ninja bundled with it, so
  nothing beyond Visual Studio and PowerShell 7 needs to be on PATH.
#>
[CmdletBinding()]
param(
  [Parameter(Position = 0)]
  [ValidateSet('configure', 'build', 'test', 'bench', 'lint', 'docs', 'format', 'modules', 'clean')]
  [string]$Command = 'build',
  [string]$Preset,
  [string]$Filter,
  [switch]$Fresh,
  [switch]$GpuLock
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')

if (-not $Preset) { $Preset = if ($IsWin) { 'msvc-debug' } else { 'linux-clang-debug' } }
$BuildDir = Join-Path $Root "build/$Preset"

function Import-VsDevEnvironment {
  if ($env:VSCMD_VER) { return }   # already inside a developer prompt
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found; install Visual Studio with the C++ workload.' }
  $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $vsPath) { throw 'No Visual Studio installation with the C++ toolset was found.' }
  $devCmd = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
  Write-Verbose "Importing developer environment from $devCmd"
  # VsDevCmd.bat expects vswhere.exe on PATH for some of its probes.
  $env:PATH = "$(Split-Path $vswhere);$env:PATH"
  $envDump = & cmd.exe /d /c "`"$devCmd`" -arch=x64 -host_arch=x64 -no_logo && set"
  foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
  }
  # Prefer the bundled CMake and Ninja.
  $cmakeDir = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
  $ninjaDir = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
  $llvmDir  = Join-Path $vsPath 'VC\Tools\Llvm\x64\bin'
  foreach ($d in @($cmakeDir, $ninjaDir, $llvmDir)) { if (Test-Path $d) { $env:PATH = "$d;$env:PATH" } }
}

function Ensure-Tool([string]$name) {
  if (-not (Get-Command $name -ErrorAction SilentlyContinue)) { throw "$name not found on PATH." }
}

function Invoke-Configure {
  Ensure-Tool cmake
  $args = @('--preset', $Preset)
  if ($Fresh) { $args += '--fresh' }
  Push-Location $Root
  try { & cmake @args; if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" } }
  finally { Pop-Location }
}

function Invoke-Build {
  # A failed configure can leave CMakeCache.txt without build.ninja; require both.
  $configured = (Test-Path (Join-Path $BuildDir 'CMakeCache.txt')) -and (Test-Path (Join-Path $BuildDir 'build.ninja'))
  if ($Fresh -or -not $configured) { Invoke-Configure }
  Ensure-Tool cmake
  Push-Location $Root
  try { & cmake --build --preset $Preset; if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" } }
  finally { Pop-Location }
}

function Invoke-Test {
  Invoke-Build
  Ensure-Tool ctest
  $args = @('--preset', $Preset)
  if ($Filter) { $args += @('-R', $Filter) }
  Push-Location $Root
  # Streamed and kept: CTest lists the tests that did not run at the end, and a test whose
  # processes gave up waiting for the GPU lock (exit 75, SKIP_RETURN_CODE) is one of them. It
  # did not fail — it did not run — and a run that skipped one must not read as a pass either,
  # so it ends with the same 75 and says which (docs/subsystems/gpu_lock.md, "Giving up").
  $lines = @()
  try {
    & ctest @args | Tee-Object -Variable lines | Out-Host
    $code = $LASTEXITCODE
  }
  finally { Pop-Location }
  $skipped = @($lines | ForEach-Object { if ("$_" -match '^\s*\d+ - (\S+) \(Skipped\)') { $Matches[1] } })
  if ($code -ne 0) { throw "tests failed ($code)" }
  if ($skipped.Count -gt 0) {
    Write-Host ("{0} test(s) did not run: their processes gave up waiting for the machine-wide GPU lock ({1}). " -f $skipped.Count, ($skipped -join ', ')) -ForegroundColor Yellow
    Write-Host 'Nothing failed. Run them again when the lock is free: tools/gpu-lock.ps1 status says who has it.' -ForegroundColor Yellow
    exit 75
  }
}

function Invoke-Bench {
  Invoke-Build
  $exes = Get-ChildItem -Path $BuildDir -Recurse -File |
    Where-Object { $_.Name -match '^engine_.+_bench(\.exe)?$' -and $_.FullName -notmatch '[\\/]_deps[\\/]' }
  if (-not $exes) { throw 'no bench executables found; is ENGINE_BUILD_BENCH on?' }
  $outDir = Join-Path $BuildDir 'bench'
  New-Item -ItemType Directory -Force -Path $outDir | Out-Null
  foreach ($exe in $exes) {
    $module = $exe.BaseName -replace '^engine_', '' -replace '_bench$', ''
    $benchArgs = @("--json=$(Join-Path $outDir "$module.jsonl")")
    if ($Filter) { $benchArgs += "--filter=$Filter" }
    # Per executable rather than once around the loop: each holds the lock for exactly the run it
    # measures, and another agent waiting for the GPU gets it between two modules rather than after
    # all of them. What each run measured under is in its own JSON header either way.
    if ($GpuLock) { $benchArgs += '--gpu-lock' }
    Write-Host "== $module" -ForegroundColor Cyan
    & $exe.FullName @benchArgs
    if ($LASTEXITCODE -ne 0) { throw "bench $module failed ($LASTEXITCODE)" }
  }
  Write-Host "results: $outDir"
}

function Invoke-Lint {
  & (Join-Path $PSScriptRoot 'lint.ps1') -Root $Root
  if ($LASTEXITCODE -ne 0) { throw "lint failed ($LASTEXITCODE)" }
}

function Invoke-Docs {
  & (Join-Path $PSScriptRoot 'docs-check.ps1') -Root $Root
  if ($LASTEXITCODE -ne 0) { throw "docs-check failed ($LASTEXITCODE)" }
}

function Invoke-Format {
  Ensure-Tool clang-format
  $files = Get-ChildItem -Path $Root -Recurse -Include *.h, *.hpp, *.cpp, *.inl -File |
    Where-Object { $_.FullName -notmatch '[\\/](build|third_party|_deps)[\\/]' }
  foreach ($f in $files) { & clang-format -i --style=file $f.FullName }
  Write-Host "formatted $($files.Count) files"
}

function Show-Modules {
  $json = Join-Path $BuildDir 'modules.json'
  if (-not (Test-Path $json)) { Invoke-Configure }
  Get-Content $json
}

if ($IsWin) { Import-VsDevEnvironment }

switch ($Command) {
  'configure' { Invoke-Configure }
  'build'     { Invoke-Build }
  'test'      { Invoke-Test }
  'bench'     { Invoke-Bench }
  'lint'      { Invoke-Lint }
  'docs'      { Invoke-Docs }
  'format'    { Invoke-Format }
  'modules'   { Show-Modules }
  'clean'     { if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir; Write-Host "removed $BuildDir" } }
}

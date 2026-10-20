#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Single entry point for building, testing, and linting the engine.

.DESCRIPTION
  tools/dev.ps1 <command> [-Preset <name>] [-Filter <regex>] [-Affected [-Base <ref>]] [-Verbose]

  Commands:
    configure   Run CMake with the preset (default: msvc-debug on Windows, linux-clang-debug on Linux).
                On Windows, in an agent's worktree (under .claude\worktrees\) or the merge gate's
                (branch `gate`), with ccache 4.13 or later on PATH, every compile goes through
                ccache into one cache for the machine (<main checkout>_local\ccache, or
                $env:ENGINE_CCACHE_DIR), so an unchanged file another checkout compiled is a hit;
                debug information then goes in the object (/Z7), since ccache cannot cache /Zi.
                Off in the primary checkout unless ENGINE_CCACHE=1, because a cached object's
                debug information names the checkout that compiled it first; ENGINE_CCACHE=0 is
                off everywhere (tools/lib/CompilerCache.psm1, docs/ci/local-linux.md "The
                compiler cache"). A tree whose launcher no longer matches is configured again.
    build       Build the preset (configures first if needed). -Affected builds only what
                `test -Affected` would run: the affected modules' libraries, tests and benches,
                and the apps that link them (tools/lib/Affected.psm1, Get-AffectedTargets).
    test        Run CTest for the preset. -Filter is a regex on test names. Tests take the
                machine-wide GPU lock themselves, per GPU device, so do not wrap this in
                tools/gpu-lock.ps1 run: that still works, and holds the GPU for the whole
                run again (docs/subsystems/gpu_lock.md). Exits 75 when a test gave up
                waiting for the lock and so did not run; 1 when one failed.
                -Affected runs the tests the change can reach and no others: the modules
                holding a file that differs from -Base (default main; committed, staged,
                unstaged and untracked all count), every module that depends on one, and
                for a changed capability every module above its layer; a change to the
                build system, the test support, vendored code or committed content runs
                everything and says why (tools/lib/Affected.psm1). It is what a change is
                tested with before it is handed over; the whole suites run at the merge.
                It builds what those tests need and nothing else, as `build -Affected` does.
    affected    Print what `test -Affected` would run, without building or running it.
    bench       Build, then run every engine_*_bench executable. -Filter is a glob on
                benchmark names; JSON lines land in build/<preset>/bench/<module>.jsonl.
                -GpuLock passes --gpu-lock to each: every executable waits for the
                machine-wide GPU lock, holds it while it measures, and releases it
                (docs/subsystems/bench.md, "The GPU lock").
    lint        Run the banned-pattern lint over the tree, and the licence check
                (tools/license-check.ps1: third_party/LICENSES.md against ADR-0014's list,
                every FetchContent declaration, and the preset's fetched licence files).
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
  [ValidateSet('configure', 'build', 'test', 'affected', 'bench', 'lint', 'docs', 'format', 'modules', 'clean')]
  [string]$Command = 'build',
  [string]$Preset,
  [string]$Filter,
  [switch]$Affected,
  [string]$Base = 'main',
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

# Whether this checkout compiles through the machine's compiler cache (tools/lib/CompilerCache.psm1),
# decided once per invocation from the checkout's kind, ENGINE_CCACHE and the ccache on PATH.
function Get-CcacheSetup {
  if ($script:CcachePlan) { return $script:CcachePlan }
  Import-Module (Join-Path $PSScriptRoot 'lib/CompilerCache.psm1') -Force
  $gitDir = & git -C $Root rev-parse --path-format=absolute --git-dir 2>$null
  $commonDir = & git -C $Root rev-parse --path-format=absolute --git-common-dir 2>$null
  if (-not $gitDir -or -not $commonDir) {
    # Not a git checkout (an exported SDK, say): nothing to share a cache with, and nothing to decide by.
    $script:CcachePlan = [pscustomobject]@{ Enabled = $false; Reason = 'not a git checkout' }
    return $script:CcachePlan
  }
  $branch = & git -C $Root rev-parse --abbrev-ref HEAD 2>$null
  $kind = Get-CheckoutKind -Root $Root -GitDir $gitDir -CommonDir $commonDir -Branch "$branch"
  $mainRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $commonDir))
  $exe = ''
  $version = $null
  $cmd = Get-Command ccache -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  # Asked only where the answer matters, so a primary checkout without ENGINE_CCACHE=1 never runs it.
  if ($cmd -and $env:ENGINE_CCACHE -ne '0' -and ($kind -in @('agent', 'gate') -or $env:ENGINE_CCACHE -eq '1')) {
    $exe = $cmd.Source
    $version = ConvertFrom-CcacheVersion "$(& $exe --version 2>$null | Select-Object -First 1)"
  }
  $script:CcachePlan = Get-CcachePlan -Kind $kind -Setting "$env:ENGINE_CCACHE" -Exe $exe -Version $version `
                                      -MainRoot $mainRoot -CacheDir "$env:ENGINE_CCACHE_DIR" -IsWindows $IsWin
  return $script:CcachePlan
}

function Invoke-Configure {
  Ensure-Tool cmake
  $args = @('--preset', $Preset)
  if ($Fresh) { $args += '--fresh' }
  $plan = Get-CcacheSetup
  $cacheArgs = @(Get-CcacheConfigureArgs -Plan $plan -CachedLauncher (Read-CachedLauncher $BuildDir))
  $args += $cacheArgs
  if ($plan.Enabled) {
    Write-Host "ccache: on ($($plan.Reason)); cache $($plan.CacheDir), base_dir $($plan.BaseDir), ccache $($plan.Version)" -ForegroundColor Cyan
  } elseif ($cacheArgs.Count -gt 0 -or $env:ENGINE_CCACHE) {
    Write-Host "ccache: off ($($plan.Reason))" -ForegroundColor Cyan
  } else {
    Write-Verbose "ccache: off ($($plan.Reason))"
  }
  Push-Location $Root
  try { & cmake @args; if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" } }
  finally { Pop-Location }
}

function Invoke-Build {
  # A failed configure can leave CMakeCache.txt without build.ninja; require both. A tree whose
  # compiler launcher is not the one this checkout now wants (ccache installed since, ENGINE_CCACHE
  # changed) is configured again; that changes every compile command, so its next build is whole.
  $configured = (Test-Path (Join-Path $BuildDir 'CMakeCache.txt')) -and (Test-Path (Join-Path $BuildDir 'build.ninja'))
  $plan = Get-CcacheSetup   # first: it imports the module the next line calls
  $relaunch = $configured -and (Test-CcacheReconfigure -Plan $plan -CachedLauncher (Read-CachedLauncher $BuildDir))
  if ($Fresh -or -not $configured -or $relaunch) { Invoke-Configure }
  Ensure-Tool cmake
  $buildArgs = @('--build', '--preset', $Preset)
  if ($Affected) {
    $result = Get-Affected
    if (-not $result.All) {
      $targets = @(Get-AffectedBuildTargets $result)
      if ($targets.Count -eq 0) {
        Write-Host '  build: nothing; no affected module has a target in this preset' -ForegroundColor Cyan
        return
      }
      Write-Host ("  build: {0} target(s): {1}" -f $targets.Count, ($targets -join ' ')) -ForegroundColor Cyan
      $buildArgs += @('--target') + $targets
    }
  }
  Push-Location $Root
  try { & cmake @buildArgs; if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" } }
  finally { Pop-Location }
}

# The targets of an affected set that this build tree has (tools/lib/Affected.psm1). The tree's own
# ninja lists them: CMake writes a phony alias for every target.
function Get-AffectedBuildTargets($result) {
  $ninja = Select-String -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Pattern '^CMAKE_MAKE_PROGRAM(:[A-Z]+)?=(.*)$' |
    Select-Object -First 1 | ForEach-Object { $_.Matches[0].Groups[2].Value }
  if (-not $ninja) { $ninja = 'ninja' }
  $lines = & $ninja -C $BuildDir -t targets all 2>$null
  if ($LASTEXITCODE -ne 0) { throw "ninja -t targets failed in $BuildDir ($LASTEXITCODE)" }
  return Get-AffectedTargets -Modules $result.Modules -Available (ConvertFrom-NinjaTargets -Lines @($lines))
}

# What the change against -Base can reach, from the preset's module graph (tools/lib/Affected.psm1).
# Once per invocation: `test -Affected` asks before its build and again for its tests.
function Get-Affected {
  if ($script:AffectedResult) { return $script:AffectedResult }
  $script:AffectedResult = Get-AffectedOnce
  return $script:AffectedResult
}

function Get-AffectedOnce {
  $graph = Join-Path $BuildDir 'modules.json'
  if (-not (Test-Path -LiteralPath $graph)) { throw "no $graph; run: tools/dev.ps1 configure -Preset $Preset" }
  Import-Module (Join-Path $PSScriptRoot 'lib/Affected.psm1') -Force
  $json = Get-Content -LiteralPath $graph -Raw | ConvertFrom-Json
  $changed = @(Get-ChangedPaths -Root $Root -Base $Base)
  $result = Get-AffectedTests -Modules $json.modules -Layers $json.layers -Changed $changed
  Write-Host ("affected: {0} path(s) differ from {1}" -f $changed.Count, $Base) -ForegroundColor Cyan
  if ($result.All) {
    Write-Host "  everything: $($result.Reason)" -ForegroundColor Cyan
  } else {
    $touched = if ($result.Changed.Count -gt 0) { $result.Changed -join ', ' } else { 'no module' }
    Write-Host ("  changed: {0}" -f $touched) -ForegroundColor Cyan
    Write-Host ("  tests of {0} of {1} modules{2}: {3}" -f $result.Modules.Count, $json.modules.Count,
                $(if ($result.Tools) { ", and the tools' own" } else { '' }),
                $(if ($result.Modules.Count -gt 0) { $result.Modules -join ' ' } else { 'only the lint and the documentation check' })) -ForegroundColor Cyan
  }
  foreach ($note in $result.Notes) { Write-Host "  $note" -ForegroundColor Cyan }
  return $result
}

function Show-Affected {
  $result = Get-Affected
  if (-not $result.All) {
    Write-Host "  ctest -R '$($result.Filter)'"
    if (Test-Path -LiteralPath (Join-Path $BuildDir 'build.ninja')) {
      $targets = @(Get-AffectedBuildTargets $result)
      Write-Host ("  build -Affected: {0} target(s){1}" -f $targets.Count, $(if ($targets.Count) { ": $($targets -join ' ')" } else { '' }))
    }
  }
}

function Invoke-Test {
  if ($Affected -and $Filter) { throw '-Affected chooses the tests; it cannot be combined with -Filter.' }
  Invoke-Build
  Ensure-Tool ctest
  $args = @('--preset', $Preset)
  if ($Affected) {
    # After the build, so the module graph is the one this tree configures.
    $result = Get-Affected
    if (-not $result.All) { $args += @('-R', $result.Filter) }
  }
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
    # CTest keeps a skipped test's output out of the console; the line that says how long it
    # waited and for whom is in the log, and it is the line a reader needs.
    $log = Join-Path $BuildDir 'Testing/Temporary/LastTest.log'
    if (Test-Path -LiteralPath $log) {
      Select-String -LiteralPath $log -Pattern 'gpu-lock: gave up waiting' | ForEach-Object { Write-Host "  $($_.Line)" -ForegroundColor Yellow }
    }
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
  # And the licence check (ADR-0014), with the preset's fetched sources when it has been configured.
  $licenseArgs = @{ Root = $Root }
  if (Test-Path -LiteralPath (Join-Path $BuildDir '_deps')) { $licenseArgs.BuildDir = $BuildDir }
  & (Join-Path $PSScriptRoot 'license-check.ps1') @licenseArgs
  if ($LASTEXITCODE -ne 0) { throw "license check failed ($LASTEXITCODE)" }
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
  'affected'  { Show-Affected }
  'bench'     { Invoke-Bench }
  'lint'      { Invoke-Lint }
  'docs'      { Invoke-Docs }
  'format'    { Invoke-Format }
  'modules'   { Show-Modules }
  'clean'     { if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir; Write-Host "removed $BuildDir" } }
}

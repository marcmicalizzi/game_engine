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
                The shaders have a cache of their own by the same rules (ENGINE_SHADER_CACHE=1
                or 0, the store <main checkout>_local\shader-cache or $env:ENGINE_SHADER_CACHE_DIR;
                docs/ci/what-to-run.md, "The shader cache"), on any system, ccache or none.
    build       Build the preset (configures first if needed). -Affected builds only what
                `test -Affected` would run: the affected modules' libraries, tests and benches,
                the apps that link them, and every app when the tools' own tests run, since
                they start the apps (tools/lib/Affected.psm1, Get-AffectedTargets). A link that
                fails with LNK1236 or LNK1107 (a corrupt object or library) is repaired once: the
                files are moved to build/<preset>/lnk1236/, that target is rebuilt with
                CCACHE_RECACHE=1 at a lower parallel level, and the build runs again (docs/ci/local-linux.md, "A corrupt object at the link").
    test       Run CTest for the preset. -Filter is a regex on test names. Tests take the
                machine-wide GPU lock themselves, per GPU device, so do not wrap this in
                tools/gpu-lock.ps1 run: that still works, and holds the GPU for the whole
                run again (docs/subsystems/gpu_lock.md). Exits 75 when a test gave up
                waiting for the lock and so did not run; 1 when one failed.
                -Affected runs the tests the change can reach and no others: the modules
                holding a file that differs from -Base (default main; committed, staged,
                unstaged and untracked all count), and the tests whose targets link or start
                one (modules.json's test edges, roadmap T31); a committed content directory,
                a support header, a tools script or a CMake helper reaches the modules whose
                files name it, include it or call it; what the rule cannot place (the build
                system's core, vendored code, the schema compiler) runs everything and says
                why (tools/lib/Affected.psm1). -AffectedRule modules is the older rule (the
                module graph, and everything above a changed capability's layer), -AffectedRule
                all selects everything; $env:ENGINE_AFFECTED_RULE says the same. It is what a
                change is tested with before it is handed over; the whole suites run at the merge.
                It builds what those tests need and nothing else, as `build -Affected` does.
                -Jobs <n> runs <n> tests at once (`ctest -j`); without it a Windows preset runs
                one at a time, as it always has, and a Linux preset as its test preset says.
                The tests that can open a GPU device run as two chains (the resource locks
                `gpu_device.1` and `.2`, dealt by cost over the tests labelled `gpu`; one chain
                where the machine-wide GPU lock is not in use or a test's work allowance does not
                fit twice in its lock wait, as on the linux-server presets; ENGINE_TEST_GPU_CHAINS), so two of
                them run at once and the machine-wide lock orders their devices; the tests that
                can open a window run one at a time (`e2e_apps`); the wall-clock tests run alone
                (cmake/EngineTesting.cmake, docs/ci/what-to-run.md "Jobs", "Two GPU chains").
                -SkipInvariantGpu, in an off-configuration (modules.json says minimal or names a
                disabled capability), runs the tests labelled `config_invariant` (renderer, gfx:
                libraries that are the same code in every configuration) with the Vulkan
                drivers hidden, so their CPU cases run and their GPU cases skip as on a machine
                with no GPU, and then everything else as usual. In an all-on configuration it
                changes nothing. It is the merge gate's (docs/ci/what-to-run.md, "The
                off-configurations' GPU cases").
                -NoDriver runs the whole test run, in any configuration, with no Vulkan driver to
                find and the GPU lock's switch at 0 (tools/lib/NoDriver.psm1: four variables,
                put back when the run ends), so every GPU case skips and nothing asks for the
                machine-wide lock. It is the no-driver form of a change that needs no GPU
                (docs/ci/what-to-run.md, "The no-driver form").
                Every ctest pass's LastTest.log is also kept beside it as
                LastTest-<yyyyMMdd-HHmmss>-<pass>.log (all, affected or filter, and with
                -SkipInvariantGpu's two passes -invariant-no-driver and -rest), the newest 16,
                so a later pass or a rerun does not overwrite the seconds tools/test-costs.ps1
                reads (tools/lib/CTestLogs.psm1).
    affected   Print what `test -Affected` would run, without building or running it.
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
  [switch]$GpuLock,
  [ValidateRange(0, 64)][int]$Jobs = 0,
  [switch]$SkipInvariantGpu,
  [switch]$NoDriver,
  [ValidateSet('', 'edges', 'modules', 'all')][string]$AffectedRule = ''
)

$ErrorActionPreference = 'Stop'
# Only a test run has GPU cases to skip; a bench's GPU work is what -GpuLock is for.
if ($NoDriver -and $Command -ne 'test') { throw "-NoDriver is a test run's switch (tools/dev.ps1 test -NoDriver), not $Command's." }
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
  # The shader compile cache decides by the same facts (Get-ShaderCacheSetup).
  $script:CheckoutKind = $kind
  $script:MainRoot = $mainRoot
  $exe = ''
  $version = $null
  $cmd = Get-Command ccache -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  # On PATH, or where winget put it for a shell started before the install (Find-Ccache).
  $found = Find-Ccache -OnPath "$(if ($cmd) { $cmd.Source })" -LocalAppData "$env:LOCALAPPDATA"
  # Asked only where the answer matters, so a primary checkout without ENGINE_CCACHE=1 never runs it.
  if ($found -and $env:ENGINE_CCACHE -ne '0' -and ($kind -in @('agent', 'gate') -or $env:ENGINE_CCACHE -eq '1')) {
    $exe = $found.Path
    $script:CcacheWhere = $found.Where
    $version = ConvertFrom-CcacheVersion "$(& $exe --version 2>$null | Select-Object -First 1)"
  }
  $script:CcachePlan = Get-CcachePlan -Kind $kind -Setting "$env:ENGINE_CCACHE" -Exe $exe -Version $version `
                                      -MainRoot $mainRoot -CacheDir "$env:ENGINE_CCACHE_DIR" -IsWindows $IsWin
  return $script:CcachePlan
}

# Whether this checkout's shaders compile through the machine's shader compile cache
# (tools/lib/CompilerCache.psm1, Get-ShaderCachePlan): the same kinds of checkout as ccache, decided by
# ENGINE_SHADER_CACHE, on every system, with or without a ccache.
function Get-ShaderCacheSetup {
  if ($script:ShaderCachePlan) { return $script:ShaderCachePlan }
  $null = Get-CcacheSetup   # imports the module and finds the checkout's kind
  if (-not $script:CheckoutKind) {
    $script:ShaderCachePlan = [pscustomobject]@{ Enabled = $false; Reason = 'not a git checkout' }
    return $script:ShaderCachePlan
  }
  $script:ShaderCachePlan = Get-ShaderCachePlan -Kind $script:CheckoutKind -Setting "$env:ENGINE_SHADER_CACHE" `
                                                -MainRoot $script:MainRoot -CacheDir "$env:ENGINE_SHADER_CACHE_DIR"
  return $script:ShaderCachePlan
}

function Invoke-Configure {
  Ensure-Tool cmake
  $args = @('--preset', $Preset)
  if ($Fresh) { $args += '--fresh' }
  $plan = Get-CcacheSetup
  $cacheArgs = @(Get-CcacheConfigureArgs -Plan $plan -CachedLauncher (Read-CachedLauncher $BuildDir))
  $args += $cacheArgs
  $shaderPlan = Get-ShaderCacheSetup
  $args += @(Get-ShaderCacheConfigureArgs -Plan $shaderPlan -CachedDir (Read-CachedShaderCacheDir $BuildDir))
  if ($shaderPlan.Enabled) {
    Write-Host "shader cache: on ($($shaderPlan.Reason)); store $($shaderPlan.Dir)" -ForegroundColor Cyan
  } else {
    Write-Verbose "shader cache: off ($($shaderPlan.Reason))"
  }
  if ($plan.Enabled) {
    Write-Host "ccache: on ($($plan.Reason)); cache $($plan.CacheDir), base_dir $($plan.BaseDir), ccache $($plan.Version) at $($plan.Exe) (from $script:CcacheWhere)" -ForegroundColor Cyan
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
  $relaunch = $relaunch -or ($configured -and (Test-ShaderCacheReconfigure -Plan (Get-ShaderCacheSetup) -CachedDir (Read-CachedShaderCacheDir $BuildDir)))
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
  $built = Invoke-CMakeBuild $buildArgs
  if ($built.Code -eq 0) { return }
  # A corrupt object at the link (LNK1236; docs/ci/local-linux.md, "A corrupt object at the link"):
  # the object and its library are removed, that target is rebuilt once with the cache told to
  # compile and store again, and the build runs once more. Anything else fails as it always did.
  $ninja = Get-NinjaProgram
  $query = { param($target) & $ninja -C $BuildDir -t query $target 2>$null }
  $repairs = @(Get-CorruptObjectRepair -Lines $built.Lines -Query $query)
  if ($repairs.Count -eq 0) { throw "build failed ($($built.Code))" }
  foreach ($r in $repairs) {
    if ($r.Remove.Count -eq 0) {
      Write-Host "build: $($r.Code) in $($r.Named), which the build graph does not show; not retried" -ForegroundColor Yellow
      throw "build failed ($($built.Code))"
    }
  }
  # The bad files are moved aside, not deleted: the cause is not known, and they are the evidence.
  # The lead is a short write under pressure (2026-11-14: a library LNK1107 twice while free commit
  # fell from 53 to 29 GB, then linked at parallel level 4 with 38 GB free), so the line says the
  # free commit and the rebuild runs at a quarter of the cores, at most 4.
  $kept = Join-Path $BuildDir ("lnk1236/{0:yyyyMMdd-HHmmss}" -f (Get-Date))
  $free = ''
  if ($IsWin) {
    try { $free = ", {0:N1} GB of commit free" -f ((Get-CimInstance Win32_OperatingSystem).FreeVirtualMemory / 1MB) } catch { }
  }
  $jobs = [Math]::Max(1, [Math]::Min(4, [int][Math]::Floor([Environment]::ProcessorCount / 4)))
  foreach ($r in $repairs) {
    foreach ($f in $r.Remove) {
      $from = Join-Path $BuildDir $f
      if (-not (Test-Path -LiteralPath $from)) { continue }
      $to = Join-Path $kept $f
      New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
      Move-Item -LiteralPath $from -Destination $to -Force
    }
    $sizes = ($r.Remove | ForEach-Object { $i = Get-Item -LiteralPath (Join-Path $kept $_) -ErrorAction Ignore; if ($i) { "$($i.Length) B" } }) -join ', '
    Write-Host ("build: {0} (a corrupt object or library) in {1}: moved {2} ({3}) to {4}, rebuilding {5} once with CCACHE_RECACHE=1 at -j {6}{7}" -f
                $r.Code, $r.Named, ($r.Remove -join ', '), $sizes, $kept, $r.Target, $jobs, $free) -ForegroundColor Yellow
  }
  $recache = $env:CCACHE_RECACHE
  $env:CCACHE_RECACHE = '1'
  try { $rebuilt = Invoke-CMakeBuild (@('--build', '--preset', $Preset, '--parallel', "$jobs", '--target') + @($repairs | ForEach-Object { $_.Target })) }
  finally { $env:CCACHE_RECACHE = $recache }
  if ($rebuilt.Code -ne 0) { throw "build failed ($($rebuilt.Code))" }
  $built = Invoke-CMakeBuild $buildArgs
  if ($built.Code -ne 0) { throw "build failed ($($built.Code))" }
}

# One `cmake --build`, its output shown as it comes and kept for the LNK1236 check above.
function Invoke-CMakeBuild([string[]]$arguments) {
  $lines = @()
  Push-Location $Root
  try {
    & cmake @arguments | Tee-Object -Variable lines | Out-Host
    $code = $LASTEXITCODE
  }
  finally { Pop-Location }
  return [pscustomobject]@{ Code = $code; Lines = @($lines) }
}

function Get-NinjaProgram {
  $ninja = Select-String -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Pattern '^CMAKE_MAKE_PROGRAM(:[A-Z]+)?=(.*)$' |
    Select-Object -First 1 | ForEach-Object { $_.Matches[0].Groups[2].Value }
  if (-not $ninja) { $ninja = 'ninja' }
  return $ninja
}

# The targets of an affected set that this build tree has (tools/lib/Affected.psm1). The tree's own
# ninja lists them: CMake writes a phony alias for every target.
function Get-AffectedBuildTargets($result) {
  $ninja = Select-String -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Pattern '^CMAKE_MAKE_PROGRAM(:[A-Z]+)?=(.*)$' |
    Select-Object -First 1 | ForEach-Object { $_.Matches[0].Groups[2].Value }
  if (-not $ninja) { $ninja = 'ninja' }
  $lines = & $ninja -C $BuildDir -t targets all 2>$null
  if ($LASTEXITCODE -ne 0) { throw "ninja -t targets failed in $BuildDir ($LASTEXITCODE)" }
  return Get-AffectedTargets -Modules $result.Modules -Apps $result.Apps -Available (ConvertFrom-NinjaTargets -Lines @($lines))
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
  # The rule (roadmap T31): the test edges by default; `modules` the older module-graph rule,
  # `all` everything. -AffectedRule wins over $env:ENGINE_AFFECTED_RULE.
  $rule = if ($AffectedRule) { $AffectedRule } elseif ($env:ENGINE_AFFECTED_RULE) { $env:ENGINE_AFFECTED_RULE } else { 'edges' }
  if ($rule -notin @('edges', 'modules', 'all')) { throw "ENGINE_AFFECTED_RULE '$rule' is none of edges, modules, all" }
  $mentions = if ($rule -eq 'edges') { @(Get-AffectedMentions -Root $Root -Changed $changed) } else { @() }
  $result = Get-AffectedTests -Modules $json.modules -Layers $json.layers -Changed $changed -Mentions $mentions -Rule $rule -Disabled @($json.disabled_capabilities)
  Write-Host ("affected: {0} path(s) differ from {1} (rule: {2})" -f $changed.Count, $Base, $rule) -ForegroundColor Cyan
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

# Whether this preset's tree leaves a capability out (modules.json: `minimal`, or a disabled
# capability), which is when -SkipInvariantGpu has anything to skip.
function Test-OffConfiguration {
  $graph = Join-Path $BuildDir 'modules.json'
  if (-not (Test-Path -LiteralPath $graph)) { return $false }
  $json = Get-Content -LiteralPath $graph -Raw | ConvertFrom-Json
  return [bool]$json.minimal -or @($json.disabled_capabilities).Count -gt 0
}

# One `ctest` over the preset, streamed and kept: CTest lists the tests that did not run at the
# end, and a test whose processes gave up waiting for the GPU lock (exit 75, SKIP_RETURN_CODE) is
# one of them. Its LastTest.log is copied to a name of its own, `LastTest-<time>-<pass>.log` beside
# it (tools/lib/CTestLogs.psm1), because the next pass or the next invocation writes the same file.
Import-Module (Join-Path $PSScriptRoot 'lib/CTestLogs.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'lib/NoDriver.psm1') -Force
function Invoke-CTest([string[]]$ctestArgs, [string]$pass) {
  Push-Location $Root
  $lines = @()
  $started = Get-Date
  try {
    & ctest @ctestArgs | Tee-Object -Variable lines | Out-Host
    $code = $LASTEXITCODE
  }
  finally { Pop-Location }
  $kept = Save-CTestLog -BuildDir $BuildDir -Pass $pass -Started $started
  if ($kept) { Write-Host "ctest log ($pass): $kept" -ForegroundColor Cyan }
  return [pscustomobject]@{ Code = $code; Lines = @($lines); Log = $kept }
}

function Invoke-Test {
  if ($Affected -and $Filter) { throw '-Affected chooses the tests; it cannot be combined with -Filter.' }
  Invoke-Build
  Ensure-Tool ctest
  $args = @('--preset', $Preset)
  # What the run selects, in its logs' names: every test, the affected ones, or a filter's.
  $kind = 'all'
  if ($Affected) {
    # After the build, so the module graph is the one this tree configures.
    $result = Get-Affected
    if (-not $result.All) { $args += @('-R', $result.Filter); $kind = 'affected' }
  }
  if ($Filter) { $args += @('-R', $Filter); $kind = 'filter' }
  # On the command line it wins over a test preset's `execution.jobs`; absent, the preset decides
  # (the Windows presets name none, so an author's run is serial as before).
  if ($Jobs -gt 0) { $args += @('-j', "$Jobs") }
  # A run that skipped a test must not read as a pass, so it ends with 75 and says which
  # (docs/subsystems/gpu_lock.md, "A wait is bounded"); a failure in either run below fails the whole.
  $runs = @()
  # The author's no-driver form: the four for the whole run, both passes included, and put back
  # whatever happens (the variables are this pwsh process's, so the caller's session gets them back).
  if ($NoDriver) { Write-Host "-NoDriver: no Vulkan driver to find and the GPU lock's switch at 0 for the whole run; every GPU case skips and nothing waits for the lock" -ForegroundColor Cyan }
  $noDriverSaved = Enter-NoDriver -BuildDir $BuildDir -When ([bool]$NoDriver)
  try {
  if ($SkipInvariantGpu -and (Test-OffConfiguration)) {
    # The libraries labelled `config_invariant` are the same code in every configuration (the
    # configure-time check in engine_module_tests and the lint's config-invariant-conditional
    # rule hold them to it), so their GPU cases ran in the all-on suite and need not run here.
    # Their CPU cases still do: with no Vulkan driver to find, every GPU case skips the way it
    # does on a hosted runner with no GPU, and no device means no GPU lock. First, so that the
    # main run's LastTest.log is the one the build tree keeps.
    Write-Host "-SkipInvariantGpu: $Preset leaves capabilities out; the config_invariant tests run with no Vulkan driver, then the rest" -ForegroundColor Cyan
    $saved = Enter-NoDriver -BuildDir $BuildDir
    try { $runs += Invoke-CTest ($args + @('-L', '^config_invariant$')) "$kind-invariant-no-driver" }
    finally { Exit-NoDriver $saved }
    $runs += Invoke-CTest ($args + @('-LE', '^config_invariant$')) "$kind-rest"
  } else {
    if ($SkipInvariantGpu) { Write-Host "-SkipInvariantGpu: $Preset is an all-on configuration; nothing skipped" -ForegroundColor Cyan }
    $runs += Invoke-CTest $args $kind
  }
  }
  finally { Exit-NoDriver $noDriverSaved }
  $lines = @($runs | ForEach-Object { $_.Lines })
  $code = @($runs | Where-Object { $_.Code -ne 0 } | Select-Object -First 1 | ForEach-Object { $_.Code })
  $code = if ($code.Count -gt 0) { $code[0] } else { 0 }
  $skipped = @($lines | ForEach-Object { if ("$_" -match '^\s*\d+ - (\S+) \(Skipped\)') { $Matches[1] } })
  if ($code -ne 0) { throw "tests failed ($code)" }
  if ($skipped.Count -gt 0) {
    Write-Host ("{0} test(s) did not run: their processes gave up waiting for the machine-wide GPU lock ({1}). " -f $skipped.Count, ($skipped -join ', ')) -ForegroundColor Yellow
    # CTest keeps a skipped test's output out of the console; the line that says how long it
    # waited and for whom is in the log, and it is the line a reader needs: every pass's, each
    # kept under its own name.
    foreach ($log in @($runs | ForEach-Object { $_.Log } | Where-Object { $_ })) {
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

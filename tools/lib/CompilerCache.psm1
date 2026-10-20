# The Windows half of the shared compiler cache (roadmap T67): whether `tools/dev.ps1 configure` puts
# ccache in front of MSVC for this checkout, and what it then gives CMake. The Linux half lives in the
# containers (tools/lib/LinuxVolumes.psm1, Get-PresetScript). docs/ci/local-linux.md, "The compiler
# cache", is the long form, with the measurements and the caveat below.
#
# The decision is a pure function of facts the script gathers — what kind of checkout this is, what
# ENGINE_CCACHE says, which ccache is on PATH — so tools/linux-build.Tests.ps1 drives every rule
# without a compiler, a ccache or a build tree.
#
# **The caveat that decides where it is on.** ccache serves an object compiled in another checkout
# when the compile is the same, and on Windows that needs two settings that change what the object
# says about where it came from: base_dir, so the per-worktree absolute paths are rewritten relative
# to the build directory before they are hashed and before the compiler sees them; and
# hash_dir=false, so the build directory's own absolute path (which /Z7 debug information records)
# is not hashed either. An object served from the cache therefore carries, in its debug information,
# the build directory of the checkout that compiled it first, and its __FILE__ strings are relative
# ("..\..\core\base\src\assert.cpp") rather than absolute. A debugger opened on such a build can show
# another worktree's paths. So the launcher is **on by default only where nobody debugs**: in an
# agent's worktree (a root under .claude\worktrees\) and in the merge gate's (branch `gate`). In the
# owner's primary checkout, and in any other worktree, it is off unless ENGINE_CCACHE=1, so the
# owner's own debugging never meets an agent's paths. ENGINE_CCACHE=0 turns it off everywhere.

Set-StrictMode -Version Latest

# **4.13**, the first release that calls caching cl.exe supported rather than experimental. What the
# tree's MSVC compiles need, and the release each arrived in (ccache's release notes): caching MSVC at
# all (4.6), per-invocation settings on the command line (`ccache key=value cl ...`, 4.8: the launcher
# carries every setting, so a build run from an IDE or straight through ninja caches exactly as
# dev.ps1's does), /Z7 taken as overriding an earlier /Z option (4.8), /Fd left out of the hash (4.9;
# CMake's Ninja generator passes a compile PDB name even with /Z7), the absolute paths in MSVC's
# diagnostics rewritten relative under base_dir like the rest (4.11), the INCLUDE and EXTERNAL_INCLUDE variables in the hash
# (4.12: a different Visual Studio or SDK is a miss, not a wrong hit), and cl.exe supported (4.13).
# /showIncludes, which Ninja's `deps = msvc` reads for header dependencies, is stored with each result
# with its paths under base_dir made relative, so a hit replays the include list of *this* checkout's
# headers and Ninja's dependency log stays this checkout's. /Zc:preprocessor, /utf-8, /permissive-
# and the rest of cmake/EngineWarnings.cmake are ordinary options to it. The engine's own code uses no
# precompiled headers, but SDL3 and Jolt do (/Yc, /Yu), so a configure with the cache on switches
# them off (Get-CcacheConfigureArgs); and nothing scans for C++ modules (CMAKE_CXX_SCAN_FOR_MODULES is
# off), so there is no /sourceDependencies either.
$script:MinimumVersion = [version]'4.13'

# The same rule as the containers' 30 GB (docs/ci/local-linux.md, "The compiler cache", for the
# measured object sizes it comes from).
$script:MaxSize = '30G'

function Get-CheckoutKind {
  <#
  .SYNOPSIS
    `primary` for the repository's main checkout (its git directory is the common one), `agent` for a
    worktree under .claude\worktrees\, `gate` for the merge gate's worktree (branch `gate`), and
    `worktree` for any other linked worktree.
  #>
  param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$GitDir,
    [Parameter(Mandatory)][string]$CommonDir,
    [string]$Branch
  )
  $key = { param($p) ($p -replace '\\', '/').TrimEnd('/').ToLowerInvariant() }
  if ((& $key $GitDir) -eq (& $key $CommonDir)) { return 'primary' }
  if ($Root -match '[\\/]\.claude[\\/]worktrees[\\/]') { return 'agent' }
  if ($Branch -eq 'gate') { return 'gate' }
  return 'worktree'
}

function ConvertFrom-CcacheVersion([string]$Text) {
  # `ccache --version`'s first line: "ccache version 4.13.1". $null for anything else.
  if ("$Text" -match 'ccache version (\d+\.\d+(?:\.\d+)?)') { return [version]$Matches[1] }
  return $null
}

function Get-CcachePlan {
  <#
  .SYNOPSIS
    Whether this configure puts ccache in front of the compiler, why, and with what. Enabled, Reason
    (a phrase for the one line dev.ps1 prints), and when enabled: Exe, Version, BaseDir, CacheDir,
    MaxSize, Launcher (the CMake list for CMAKE_<LANG>_COMPILER_LAUNCHER).
  .PARAMETER MainRoot
    The repository's main checkout. base_dir is its parent, which holds it, its .claude\worktrees\
    and the gate's worktree beside it (D:\workspace, for D:\workspace\game_engine and
    D:\workspace\ge-gate); the cache is <MainRoot>_local\ccache beside it, where the owner's other
    machine-local files already live, unless CacheDir names another.
  #>
  param(
    [Parameter(Mandatory)][ValidateSet('primary', 'agent', 'gate', 'worktree')][string]$Kind,
    [AllowEmptyString()][string]$Setting,
    [AllowEmptyString()][string]$Exe,
    [AllowNull()][version]$Version,
    [Parameter(Mandatory)][string]$MainRoot,
    [AllowEmptyString()][string]$CacheDir,
    [bool]$IsWindows = $true
  )
  $off = { param($why) [pscustomobject]@{ Enabled = $false; Reason = $why } }
  if (-not $IsWindows) { return & $off 'Windows only: the Linux containers carry their own (tools/linux-build.ps1)' }
  if ($Setting -eq '0') { return & $off 'ENGINE_CCACHE=0' }
  # On by default where nobody debugs; elsewhere only when asked for.
  $wanted = ($Kind -in @('agent', 'gate')) -or ($Setting -eq '1')
  if (-not $wanted) {
    $what = if ($Kind -eq 'primary') { 'the primary checkout' } else { 'a worktree that is neither an agent''s nor the gate''s' }
    return & $off "$what (ENGINE_CCACHE=1 turns it on; a cached object's debug information names the checkout that compiled it first)"
  }
  if (-not $Exe) { return & $off 'ccache is not on PATH (winget install ccache)' }
  if (-not $Version -or $Version -lt $script:MinimumVersion) {
    return & $off "ccache $(if ($Version) { $Version } else { 'of an unknown version' }) is older than $script:MinimumVersion, the first that supports cl.exe"
  }
  # By string rather than Split-Path and Join-Path, so the rule reads the same on a machine whose
  # separator is the other one (tools.linux_build runs this module's tests in the containers too).
  $main = $MainRoot.TrimEnd('\', '/')
  $sep = if ($main.Contains('\')) { '\' } else { '/' }
  $base = $main -replace '[\\/][^\\/]+$', ''
  $leaf = $main -replace '^.*[\\/]', ''
  if (-not $CacheDir) { $CacheDir = "$base$sep${leaf}_local${sep}ccache" }
  $fwd = { param($p) $p -replace '\\', '/' }
  $launcher = @(
    (& $fwd $Exe),
    "base_dir=$(& $fwd $base)",
    "cache_dir=$(& $fwd $CacheDir)",
    'hash_dir=false',
    "max_size=$script:MaxSize"
  ) -join ';'
  return [pscustomobject]@{
    Enabled = $true; Reason = "$Kind checkout"; Exe = $Exe; Version = $Version; BaseDir = $base
    CacheDir = $CacheDir; MaxSize = $script:MaxSize; Launcher = $launcher
  }
}

function Get-CcacheConfigureArgs {
  <#
  .SYNOPSIS
    The cache variables a configure passes for a plan. Enabled: the launcher for C and C++; the
    debug information in the object (/Z7, CMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded) for the
    configurations that carry it, because ccache cannot cache /Zi — every /Zi compile writes into one
    shared PDB through mspdbsrv, which is a side effect no cache can replay; and no precompiled
    headers (CMAKE_DISABLE_PRECOMPILE_HEADERS), which the engine never uses but SDL3 and Jolt do,
    so that their sources cache like the rest (the containers found 749 compiles a run that could
    not, docs/ci/local-linux.md "The compiler cache"). Disabled: nothing at all,
    unless the build tree still carries a launcher an earlier configure of this script set
    (-CachedLauncher, read from CMakeCache.txt), which it then removes with the format, so a tree
    switched off is the tree it was before it was switched on.
  #>
  param([Parameter(Mandatory)][object]$Plan, [AllowEmptyString()][string]$CachedLauncher)
  if ($Plan.Enabled) {
    return @(
      "-DCMAKE_C_COMPILER_LAUNCHER=$($Plan.Launcher)",
      "-DCMAKE_CXX_COMPILER_LAUNCHER=$($Plan.Launcher)",
      '-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>',
      '-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON'
    )
  }
  if (Test-OwnLauncher $CachedLauncher) {
    return @('-UCMAKE_C_COMPILER_LAUNCHER', '-UCMAKE_CXX_COMPILER_LAUNCHER', '-UCMAKE_MSVC_DEBUG_INFORMATION_FORMAT',
             '-UCMAKE_DISABLE_PRECOMPILE_HEADERS')
  }
  return @()
}

function Test-OwnLauncher([string]$Launcher) {
  # A launcher this module wrote: a ccache executable followed by its settings. Anything else — none,
  # or a launcher somebody set by hand (sccache, a ccache without settings) — is not this module's
  # to remove.
  return "$Launcher" -match '(^|[\\/])ccache(\.exe)?;(.*;)?cache_dir='
}

function Test-CcacheReconfigure {
  # Whether a configured tree has to be configured again before it builds: its launcher is not the
  # one the plan wants (turned on, turned off, or a different cache or ccache).
  param([Parameter(Mandatory)][object]$Plan, [AllowEmptyString()][string]$CachedLauncher)
  if ($Plan.Enabled) { return $CachedLauncher -ne $Plan.Launcher }
  return Test-OwnLauncher $CachedLauncher
}

function Read-CachedLauncher([string]$BuildDir) {
  # CMAKE_CXX_COMPILER_LAUNCHER as the build tree's CMakeCache.txt has it, or ''.
  $cache = Join-Path $BuildDir 'CMakeCache.txt'
  if (-not (Test-Path -LiteralPath $cache)) { return '' }
  $line = Select-String -LiteralPath $cache -Pattern '^CMAKE_CXX_COMPILER_LAUNCHER(:[A-Z]+)?=(.*)$' | Select-Object -First 1
  if ($line) { return $line.Matches[0].Groups[2].Value }
  return ''
}

Export-ModuleMember -Function Get-CheckoutKind, ConvertFrom-CcacheVersion, Get-CcachePlan, Get-CcacheConfigureArgs,
  Test-CcacheReconfigure, Read-CachedLauncher

#requires -Version 7
<#
.SYNOPSIS
  Writes the game author's SDK: a self-contained folder a game repository builds, runs and packages
  a game module against, without a checkout of the engine (docs/game-authoring.md; roadmap P42).

.DESCRIPTION
  tools/export-game-sdk.ps1 -Preset msvc-release -Out <dir> [-Zip <file>] [-Scenes <paths...>]
                            [-BuildDir <dir>] [-Force]

  A game the project ships lives in its own repository and pins an engine build (ADR-0047,
  roadmap P18). Until now the engine side of that repository was a checkout, a configured and
  built preset, and an exported build command whose build/<preset>/game_modules/build_config.cmake
  holds absolute paths into that build tree. This script makes it a folder the author copies:

    include/      the public headers a module compiles against: exactly the directories the
                  build's exported command lists (core/*/include and foundation/hotload/include),
                  their contents merged, nothing from any src/
    schemas/      the schema root schemac resolves `.schema` imports against
    bin/          schemac.exe (the host tool), engine-view.exe (the preset's; shaders embedded)
                  and game/walk_poc.dll, the worked example a new author runs first
    cmake/        EngineGameModuleBuild.cmake and a relocatable build_config.cmake: every path
                  into the tree rewritten relative to the config's own directory, so the folder
                  can be moved or unzipped anywhere; the compiler and MSVC environment are the
                  exporting machine's, and tools/build-module.ps1 re-derives them through vswhere
                  on a machine without them
    content/      the test scenes a game can start from (desert-endless and ruins-walk by default,
                  -Scenes for others) with every file each names, and a ruins kit generated where
                  a scene names one the tree does not commit; content/sessions/walk-synthetic.jsonl,
                  a two-second recorded walk for offscreen runs
    tools/        build-module.ps1, run-game.ps1 and package-game.ps1 (the tree's own script,
                  which runs from this layout) with tools/lib/
    sdk.json      the engine commit and whether it was dirty, the preset, the CPU baseline, the
                  hotload ABI version, the export date, the scenes carried
    README.md     what the folder is, the three commands, where the contract is documented
    LICENSE       the engine's Apache-2.0, which the headers and binaries here are under

  It refuses an output inside the repository, and a build without
  build/<preset>/game_modules/build_config.cmake (one configured without the hotload capability,
  such as msvc-minimal: it builds no module and exports no command). Linux is not covered yet: the
  suffix and flags are the preset's, and a Linux SDK would be a Linux preset's export.

.EXAMPLE
  tools/export-game-sdk.ps1 -Preset msvc-release -Out D:\sdk\engine-sdk -Zip D:\sdk\engine-sdk.zip
#>
[CmdletBinding()]
param(
  [string]$Preset = 'msvc-release',
  [string]$BuildDir,
  [Parameter(Mandatory = $true)][string]$Out,
  [string]$Zip,
  [string[]]$Scenes = @('content/test-scenes/desert-endless/scene.json', 'content/test-scenes/ruins-walk/scene.json'),
  [switch]$Force
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

Import-Module (Join-Path $PSScriptRoot 'lib/GameContent.psm1') -Force

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
$ExeSuffix = if ($IsWin) { '.exe' } else { '' }
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build/$Preset" }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$Bin = Join-Path $BuildDir 'bin'

function Refuse([string]$why) {
  Write-Error "export-game-sdk: $why" -ErrorAction Continue
  exit 2
}
function Full([string]$p) {
  if ([System.IO.Path]::IsPathRooted($p)) { return [System.IO.Path]::GetFullPath($p) }
  return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $p))
}
function Write-Utf8([string]$path, [string]$text) {
  [System.IO.File]::WriteAllText($path, $text.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding($false)))
}

# ---- refusals ----------------------------------------------------------------------------------

$OutDir = Full $Out
$repoPrefix = $Root.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if ($OutDir.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or $OutDir -eq $Root.TrimEnd('\', '/')) {
  Refuse "the output $OutDir is inside the repository; an SDK is a copy the author takes elsewhere, never a directory of the tree"
}
# The owner's local workspace (D:\workspace\game_engine_local) is where generated outputs live, so an SDK
# may be exported there; what is refused is content *from* it inside a package or an SDK, which the
# scene resolver and package-game.ps1 enforce on their inputs.
$config = Join-Path $BuildDir 'game_modules/build_config.cmake'
if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
  Refuse "no ${config}: the build at $BuildDir was configured without the hotload capability (ENGINE_WITH_HOTLOAD=OFF, or a minimal preset), so it builds no game module and exports no build command; export from a preset that has it, such as msvc-release"
}
$engineView = Join-Path $Bin "engine-view$ExeSuffix"
if (-not (Test-Path -LiteralPath $engineView)) { Refuse "no $engineView; build the preset first (tools/dev.ps1 build -Preset $Preset)" }
$walkPoc = Join-Path $Bin "game/walk_poc.dll"
if (-not $IsWin) { $walkPoc = Join-Path $Bin 'game/walk_poc.so' }
if (-not (Test-Path -LiteralPath $walkPoc)) { Refuse "no $walkPoc; it is built with engine-view when the hotload capability is on" }
if (Test-Path -LiteralPath $OutDir) {
  if ((Get-ChildItem -LiteralPath $OutDir -Force | Measure-Object).Count -gt 0) {
    if (-not $Force) { Refuse "$OutDir exists and is not empty; -Force replaces it" }
    Remove-Item -LiteralPath $OutDir -Recurse -Force
  }
}

# ---- the exported command's configuration -------------------------------------------------------

$configText = Get-Content -LiteralPath $config -Raw
$gm = @{}
foreach ($m in [regex]::Matches($configText, '(?m)^set\((GM_\w+) \[==\[(.*?)\]==\]\)\s*$')) {
  $gm[$m.Groups[1].Value] = $m.Groups[2].Value
}
$gmMsvc = $configText -match '(?m)^set\(GM_MSVC TRUE\)'
foreach ($k in @('GM_CXX', 'GM_FLAGS', 'GM_DEFINES', 'GM_INCLUDES', 'GM_SCHEMAC', 'GM_SCHEMA_ROOT', 'GM_SUFFIX')) {
  if (-not $gm.ContainsKey($k)) { Refuse "$config has no $k; it was written by another cmake/EngineGameModule.cmake than this tree's" }
}
$includeDirs = @($gm['GM_INCLUDES'] -split ';' | Where-Object { $_ })
$schemac = $gm['GM_SCHEMAC']
if (-not (Test-Path -LiteralPath $schemac -PathType Leaf)) { Refuse "no $schemac (the build's schemac); build the preset first" }
$schemaRoot = $gm['GM_SCHEMA_ROOT']

Write-Host "export-game-sdk: $Preset from $BuildDir -> $OutDir"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
foreach ($d in @('include', 'schemas', 'bin/game', 'cmake', 'content/sessions', 'tools/lib')) {
  New-Item -ItemType Directory -Force -Path (Join-Path $OutDir $d) | Out-Null
}

# include/: the contents of each listed directory, merged. Every one of them is a module's public
# include/ (core/<module>/include holds core/<module>/..., foundation/hotload/include holds
# foundation/hotload/...), so the merge is a union with nothing colliding; a module compiles
# against the same headers the tree's own modules do, and nothing from any src/ is here. A
# directory under the build tree would be generated code (schema headers); none is listed today,
# and one that appears is copied the same way and named in sdk.json's `includes` as generated.
$includes = @()
$buildPrefix = $BuildDir.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
foreach ($dir in $includeDirs) {
  $full = [System.IO.Path]::GetFullPath($dir)
  if (-not (Test-Path -LiteralPath $full -PathType Container)) { Refuse "the exported command lists $dir, which does not exist" }
  $generated = $full.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)
  $rel = if ($generated) { 'build/' + [System.IO.Path]::GetRelativePath($BuildDir, $full).Replace('\', '/') }
         else { [System.IO.Path]::GetRelativePath($Root, $full).Replace('\', '/') }
  Copy-Item -Path (Join-Path $full '*') -Destination (Join-Path $OutDir 'include') -Recurse -Force
  $includes += [ordered]@{ source = $rel; generated = [bool]$generated }
}

# schemas/: the root schemac resolves imports against; the build file stays out.
Get-ChildItem -LiteralPath $schemaRoot -Recurse -File | Where-Object { $_.Name -ne 'CMakeLists.txt' } | ForEach-Object {
  $rel = [System.IO.Path]::GetRelativePath($schemaRoot, $_.FullName)
  $target = Join-Path $OutDir "schemas/$rel"
  New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
  Copy-Item -LiteralPath $_.FullName -Destination $target -Force
}

# bin/
Copy-Item -LiteralPath $schemac -Destination (Join-Path $OutDir "bin/schemac$ExeSuffix") -Force
Copy-Item -LiteralPath $engineView -Destination (Join-Path $OutDir "bin/engine-view$ExeSuffix") -Force
Copy-Item -LiteralPath $walkPoc -Destination (Join-Path $OutDir 'bin/game') -Force

# cmake/: the command, and its configuration made relocatable. GM_INCLUDES becomes the one merged
# include/, schemac and the schema root are named from the config's own directory, and the
# compiler, flags, definitions and the MSVC environment are kept as recorded: they are what the
# engine was built with, and a module has to be compiled with the same ones (the CPU baseline,
# floating-point contraction off, the runtime library). On a machine without the recorded
# compiler, tools/build-module.ps1 derives one through vswhere (cmake/EngineGameModuleBuild.cmake).
Copy-Item -LiteralPath (Join-Path $Root 'cmake/EngineGameModuleBuild.cmake') -Destination (Join-Path $OutDir 'cmake') -Force
$relocatable = @"
# Generated by tools/export-game-sdk.ps1 from $Preset's build_config.cmake (cmake/EngineGameModule.cmake).
# Read by EngineGameModuleBuild.cmake beside it. Every path into the SDK is relative to this file,
# so the folder can be moved; the compiler and the MSVC environment are the exporting machine's,
# and tools/build-module.ps1 substitutes this machine's when they are not here.
get_filename_component(GM_SDK_ROOT "`${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(GM_CXX [==[$($gm['GM_CXX'])]==])
set(GM_MSVC $(if ($gmMsvc) { 'TRUE' } else { 'FALSE' }))
set(GM_FLAGS [==[$($gm['GM_FLAGS'])]==])
set(GM_DEFINES [==[$($gm['GM_DEFINES'])]==])
set(GM_INCLUDES "`${GM_SDK_ROOT}/include")
set(GM_LINK_FLAGS [==[$($gm['GM_LINK_FLAGS'])]==])
set(GM_SCHEMAC "`${GM_SDK_ROOT}/bin/schemac$ExeSuffix")
set(GM_SCHEMA_ROOT "`${GM_SDK_ROOT}/schemas")
set(GM_SUFFIX [==[$($gm['GM_SUFFIX'])]==])
set(GM_ENV_INCLUDE [==[$($gm['GM_ENV_INCLUDE'])]==])
set(GM_ENV_LIB [==[$($gm['GM_ENV_LIB'])]==])
set(GM_ENV_PATH [==[$($gm['GM_ENV_PATH'])]==])
"@
Write-Utf8 (Join-Path $OutDir 'cmake/build_config.cmake') $relocatable

# content/: each scene and every file it names, at its path under content/, so the scene's own
# relative paths resolve. A ruins kit the tree does not commit (content/test-scenes/ruins-walk's
# kit/ is generated, never committed) is generated into the SDK's copy by the build's own
# engine-content, so the scene runs from the SDK as it does from a checkout.
$ContentRoot = Join-Path $Root 'content'
$engineContent = Join-Path $Bin "engine-content$ExeSuffix"
$carried = @()
foreach ($scene in $Scenes) {
  $scenePath = Full (Join-Path $Root $scene)
  if ([System.IO.Path]::IsPathRooted($scene)) { $scenePath = Full $scene }
  if (-not (Test-Path -LiteralPath $scenePath -PathType Leaf)) { Refuse "no scene $scene" }
  try { $files = @(Resolve-SceneContent -Scene $scenePath -Roots @($ContentRoot)) }
  catch { Refuse $_.Exception.Message }
  foreach ($f in $files) {
    $rel = [System.IO.Path]::GetRelativePath($ContentRoot, $f.Path)
    $target = Join-Path $OutDir "content/$rel"
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Copy-Item -LiteralPath $f.Path -Destination $target -Force
  }
  $sceneRel = [System.IO.Path]::GetRelativePath($ContentRoot, $scenePath).Replace('\', '/')
  $copied = Join-Path $OutDir "content/$sceneRel"
  $traits = Get-SceneTraits -Scene $copied
  $kits = @()
  if ($null -ne $traits.Json.PSObject.Properties['ruins'] -and $null -ne $traits.Json.ruins) {
    foreach ($entry in @($traits.Json.ruins)) {
      if ($null -eq $entry.PSObject.Properties['kit'] -or -not $entry.kit) { continue }
      $kitIndex = Join-Path (Split-Path $copied -Parent) $entry.kit
      if (Test-Path -LiteralPath $kitIndex -PathType Leaf) { $kits += [string]$entry.kit; continue }
      if (-not (Test-Path -LiteralPath $engineContent)) { Refuse "$scene names the kit $($entry.kit), which is not in the tree, and there is no $engineContent to generate it" }
      $kitDir = Split-Path $kitIndex -Parent
      New-Item -ItemType Directory -Force -Path $kitDir | Out-Null
      & $engineContent ruins-kit $kitDir | Out-Null
      if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $kitIndex -PathType Leaf)) { Refuse "engine-content ruins-kit $kitDir failed ($LASTEXITCODE)" }
      $kits += [string]$entry.kit
    }
  }
  $carried += [ordered]@{ scene = "content/$sceneRel"; world = $traits.Streams; dunes = $traits.Dunes; kits_generated = $kits }
  Write-Host ("scene:    content/{0} ({1} files{2})" -f $sceneRel, $files.Count, $(if ($kits.Count) { ", kit generated" } else { '' }))
}

# content/input-map.json: the action map this build's sessions fly with, written by the build's own
# engine-view (`--dump-action-map`, roadmap P47) with its hash. A game pins the map it ships with by
# this file, not by a number in a test (docs/game-authoring.md, "The action map"): a game's
# recordings name this hash, and tools/run-game.ps1 passes the file as `--input-map`.
$mapFile = Join-Path $OutDir 'content/input-map.json'
New-Item -ItemType Directory -Force -Path (Split-Path $mapFile -Parent) | Out-Null
& $engineView --dump-action-map $mapFile | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $mapFile)) { Refuse "engine-view --dump-action-map failed ($LASTEXITCODE)" }
# The hash as the text engine-view wrote: a u64 past 2^63 does not survive PowerShell's JSON
# numbers, so it is carried as digits from here on.
$mapText = Get-Content -LiteralPath $mapFile -Raw
if ($mapText -notmatch '"hash":\s*(\d+)') { Refuse "$mapFile carries no hash" }
$mapHash = $Matches[1]

# content/sessions/walk-synthetic.jsonl: a recorded walk for offscreen runs. engine-view's --game
# runs offscreen only as a replay, and a recording's header has to carry the action map's hash and
# the walker's numbers as this build has them. The camera and the columns come from the committed
# synthetic fly session (content/input-logs/sessions/fly-synthetic.jsonl, regenerated from code and
# held byte for byte by apps/engine_view/tests/fly_tests.cpp), the **map is the exported one above**
# — the fly session's is the first revision, which has no `interact`, so a replay of it never
# pressed E for a game (before 2026-10-18 it did not) — and the walk block is the walker's
# defaults (apps/engine_view/walk.h, WalkParams). Two seconds at 240 Hz: W held from tick 10, E
# (interact) pressed at tick 40, inside the twenty frames (80 ticks) the SDK's test runs. The start
# is high over the ground so it is above any scene's terrain; the walker drops.
$flySession = Join-Path $Root 'content/input-logs/sessions/fly-synthetic.jsonl'
if (-not (Test-Path -LiteralPath $flySession)) { Refuse "no $flySession to take a session header from" }
$flyHeader = (Get-Content -LiteralPath $flySession -TotalCount 1) | ConvertFrom-Json -Depth 16 -AsHashtable
$session = [ordered]@{}
foreach ($k in $flyHeader['session'].Keys) { $session[$k] = $flyHeader['session'][$k] }
$session['recorded_by'] = 'synthetic: tools/export-game-sdk.ps1 (a walk for offscreen runs of a game module)'
$session['scene'] = [ordered]@{ scene = ''; mesh = ''; procedural = ''; grid = 0; grid_instances = 0 }
$session['start'] = [ordered]@{ position = @(0.0, 300.0, 0.0); yaw = 0.0; pitch = -0.2 }
$session['ticks'] = 480
$session['walk'] = [ordered]@{
  version = 2; height = 1.8; radius = 0.3; eye_height = 1.65; speed = 1.5; sprint = 5.0
  max_slope_deg = 40.0; step_height = 0.35; gravity = 9.81; jump_speed = 4.0
  collision = [ordered]@{ spacing_m = 1.0; radius_tiles = 1.5; max_tiles = 2; mesh_error_m = 0.05; ground_error_m = 0.05; max_refreshes = 2 }
  start_walking = $true
}
$header = [ordered]@{ columns = $flyHeader['columns']; map = 0; session = $session; type = $flyHeader['type']; version = $flyHeader['version'] }
$headerLine = ($header | ConvertTo-Json -Depth 16 -Compress)
if ($headerLine -notmatch '"map":0,') { Refuse 'the synthetic session header has no map field to fill' }
$headerLine = $headerLine.Replace('"map":0,', "`"map`":$mapHash,")
$events = @('[10,"key",26,1.0,0]', '[40,"key",8,1.0,0]', '[41,"key",8,0.0,0]')
Write-Utf8 (Join-Path $OutDir 'content/sessions/walk-synthetic.jsonl') ((@($headerLine) + $events) -join "`n")

# tools/
Copy-Item -LiteralPath (Join-Path $Root 'tools/sdk/build-module.ps1') -Destination (Join-Path $OutDir 'tools') -Force
Copy-Item -LiteralPath (Join-Path $Root 'tools/sdk/run-game.ps1') -Destination (Join-Path $OutDir 'tools') -Force
Copy-Item -LiteralPath (Join-Path $Root 'tools/package-game.ps1') -Destination (Join-Path $OutDir 'tools') -Force
Copy-Item -LiteralPath (Join-Path $Root 'tools/lib/GameContent.psm1') -Destination (Join-Path $OutDir 'tools/lib') -Force

# ---- sdk.json: what this SDK is --------------------------------------------------------------

$versionLine = (& $engineView --version 2>$null | Out-String).Trim()
$stamp = $null
try { $stamp = $versionLine | ConvertFrom-Json } catch { }
if ($null -eq $stamp -or -not $stamp.commit) { Refuse "engine-view --version printed no build stamp: $versionLine" }
$cpuBaseline = 'unknown'
foreach ($line in (Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) {
  if ($line -match '^ENGINE_CPU_BASELINE(?::[A-Z]+)?=(.*)$') { $cpuBaseline = $Matches[1].Trim(); break }
}
$abi = 0
$abiHeader = Join-Path $Root 'foundation/hotload/include/foundation/hotload/module_abi.h'
if ((Get-Content -LiteralPath $abiHeader -Raw) -match 'k_abi_version\s*=\s*(\d+)') { $abi = [int]$Matches[1] }
if ($abi -eq 0) { Refuse "no k_abi_version in $abiHeader" }

$sdkJson = [ordered]@{
  format = 'engine.game.sdk'
  version = 1
  commit = [string]$stamp.commit
  dirty = [bool]$stamp.dirty
  preset = $Preset
  cpu_baseline = $cpuBaseline
  abi_version = $abi
  exported_utc = (Get-Date).ToUniversalTime().ToString('o')
  exported_on = [ordered]@{ machine = [Environment]::MachineName; os = [Environment]::OSVersion.VersionString }
  compiler = $gm['GM_CXX']
  module_suffix = $gm['GM_SUFFIX']
  includes = $includes
  scenes = $carried
  session = 'content/sessions/walk-synthetic.jsonl'
  action_map = 'content/input-map.json'
  example_module = "bin/game/walk_poc$($gm['GM_SUFFIX'])"
  contract = 'docs/subsystems/apps.md#the-game-contract'
  authoring_guide = 'docs/game-authoring.md'
}
Write-Utf8 (Join-Path $OutDir 'sdk.json') ($sdkJson | ConvertTo-Json -Depth 8)

# ---- README.md and the licence ----------------------------------------------------------------

Copy-Item -LiteralPath (Join-Path $Root 'LICENSE') -Destination (Join-Path $OutDir 'LICENSE') -Force
if (Test-Path -LiteralPath (Join-Path $Root 'NOTICE')) { Copy-Item -LiteralPath (Join-Path $Root 'NOTICE') -Destination (Join-Path $OutDir 'NOTICE') -Force }
$commitShort = ([string]$stamp.commit).Substring(0, [Math]::Min(12, ([string]$stamp.commit).Length))
$sceneList = ($carried | ForEach-Object { "- ``$($_.scene)``$(if ($_.world) { ' (streams a world)' })$(if ($_.kits_generated.Count) { ' - its ruins kit generated at export' })" }) -join "`n"
$readme = @"
# The engine's game SDK

Everything a game module needs from the engine, in one folder: the headers a module compiles
against, the schema compiler, the build command, engine-view to run it in, two scenes to start
from, and the tool that packs a build for a tester. It was exported from engine commit
``$commitShort``$(if ($stamp.dirty) { ' (with local changes)' }), preset ``$Preset``, CPU baseline ``$cpuBaseline``,
hotload ABI $abi (``sdk.json`` has every field). A game repository pins this folder and that commit;
``tools/new-game-repo.ps1`` in the engine's tree makes one from it.

**What a game module is**, and the contract it is written against — one struct of state, ``on_load``,
``game_tick``, the ``GameTick`` view, the services — is documented in the engine's tree, not here,
because the services change with the engine and a copy would go stale:
``docs/game-authoring.md`` (the author's guide) and ``docs/subsystems/apps.md``, "The game contract",
at the commit above. The headers in ``include/foundation/hotload/`` are the contract's source.

## The three commands

`````powershell
tools/build-module.ps1 -Source <dir of .cpp and .schema> -Out <file.dll>
tools/run-game.ps1     -Module <file.dll> -Scene content/test-scenes/desert-endless/scene.json [-Report report.json] [-Offscreen [-Frames n]]
tools/package-game.ps1 -ModuleDll <file.dll> -Scene <scene.json> [-Out <zip>]
`````

Run the worked example first: ``tools/run-game.ps1 -Module bin/game/walk_poc.dll -Scene content/test-scenes/desert-endless/scene.json``
(W A S D walks, the mouse looks, E interacts, Escape twice ends). Build a module while
``run-game.ps1`` has it open and the session swaps the new build in with its state carried.
``-Offscreen`` replays ``content/sessions/walk-synthetic.jsonl`` with no window — the same ticks
every time, so a module's state after it is a function of the module alone. ``package-game.ps1``
makes the zip a tester double-clicks and sends ``report.json`` back from; a scene of the game's own
is packaged with its directory's content, a scene from ``content/`` with the SDK's.

## What is here

| Path | What |
|---|---|
| ``include/`` | the public headers of the engine's core modules and ``foundation/hotload`` (``module_abi.h``, ``module_state.h``): what a module may include; nothing from any ``src/`` |
| ``schemas/`` | the schema root; a module's ``.schema`` may import from it |
| ``bin/`` | ``schemac`` (the schema compiler), ``engine-view`` (the preset's build, shaders embedded), ``game/walk_poc.dll`` (the example) |
| ``cmake/`` | ``EngineGameModuleBuild.cmake`` and ``build_config.cmake``: the engine's exact compiler flags, with every path into this folder relative to the config, so the folder can be moved |
| ``content/`` | the scenes:
$sceneList
| | ``sessions/walk-synthetic.jsonl``: two seconds of walking with E pressed once, for ``-Offscreen``, recorded against ``input-map.json`` |
| | ``input-map.json``: the action map the SDK's engine-view flies with and its ``hash``; pin it by this file (``tools/run-game.ps1`` passes it as ``--input-map``) |
| ``tools/`` | the three commands and ``lib/GameContent.psm1`` |
| ``sdk.json`` | the commit, preset, baseline, ABI, date and scenes |

## Requirements

Windows x64 with PowerShell 7 (``winget install Microsoft.PowerShell``), CMake 3.28 or later (on
PATH, or Visual Studio's), and Visual Studio 2022 or later with "Desktop development with C++".
``build_config.cmake`` records the compiler and environment of the machine that exported this SDK;
on another machine ``build-module.ps1`` finds Visual Studio through ``vswhere`` and uses its
compiler with the same flags. The GPU needs a Vulkan driver. **Linux is not covered by this
export**: the module suffix and the flags are ``$Preset``'s; a Linux SDK is a Linux preset's export,
which nothing writes yet.

## Licence

The headers, binaries and content in this folder are the engine's, under the Apache License 2.0
(``LICENSE``, ``NOTICE``): a game built against them owes attribution and nothing else. The game's
own code, assets and licence are the author's — the engine's own reference game chose GPL-3.0 code
with non-commercial assets (its ADR-0047), and nothing here chooses for anyone else.
"@
Write-Utf8 (Join-Path $OutDir 'README.md') $readme

# ---- the result ---------------------------------------------------------------------------------

$bytes = (Get-ChildItem -LiteralPath $OutDir -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("sdk:      {0} ({1:N1} MB, {2} files)" -f $OutDir, ($bytes / 1MB), (Get-ChildItem -LiteralPath $OutDir -Recurse -File).Count)
if ($Zip) {
  $zipPath = Full $Zip
  if ($zipPath.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) { Refuse "the zip $zipPath is inside the repository" }
  New-Item -ItemType Directory -Force -Path (Split-Path $zipPath -Parent) | Out-Null
  if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  [System.IO.Compression.ZipFile]::CreateFromDirectory($OutDir, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)
  Write-Host ("zip:      {0} ({1:N1} MB)" -f $zipPath, ((Get-Item -LiteralPath $zipPath).Length / 1MB))
}
exit 0

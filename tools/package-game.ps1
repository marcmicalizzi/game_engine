<#
.SYNOPSIS
  Packs engine-view, a game module and a scene into a zip a tester unzips, double-clicks and sends
  one report.json back from (docs/subsystems/apps.md, "package-game: a zip somebody else
  double-clicks"; roadmap P41, P42).

.DESCRIPTION
  tools/package-tests.ps1's pattern for a game rather than a test suite: everything a machine with
  no toolchain, no checkout and no PowerShell 7 needs to play a proof of concept and say how it
  went.

    engine-view.exe     the preset's own build: statically linked, its shaders embedded (the
                        build's shaders/manifest.json names absolute paths into the build tree and
                        a compiler, so it stays out, as package-tests leaves it out)
    game/<Module>.dll   the game module (engine_game_module, or a module built from a game
                        repository with the SDK's build-module.ps1), loaded with --game
    content/...         the scene and every file it names, resolved, at paths that keep the scene's
                        own relative paths resolving
    run.cmd             double-click: starts the walking session with the game and a report,
                        and says where report.json is
    README.txt          what it is, how to run it, the keys, what to send back, and that it writes
                        nothing outside its folder
    package.json        the commit, the preset, the module, the scene, the files, and the exact
                        arguments run.cmd passes (what the package test runs)

  **Two layouts, one script.** Run from the engine tree (tools/package-game.ps1) it packs
  build/<Preset>/bin/engine-view and build/<Preset>/bin/game/<Module>.dll, and the scene must be
  under the repository's content/. Run from an exported game SDK (<sdk>/tools/package-game.ps1,
  tools/export-game-sdk.ps1; the marker is <sdk>/sdk.json) it packs the SDK's bin/engine-view, the
  module from -ModuleDll (a game repository's build) or bin/game/<Module>.dll, and the scene may be
  the SDK's (under <sdk>/content) or the game repository's own (anywhere else), whose content is
  resolved against the scene's own directory and carried under content/<its directory's name>/.
  In both layouts it refuses anything under the owner's local workspace
  (D:\workspace\game_engine_local: generated assets and workflow files are never shipped, AGENTS.md).
  The resolver is tools/lib/GameContent.psm1, shared with the exporter.

  The far end needs the Microsoft Visual C++ 2015-2022 Redistributable (x64), which almost every
  Windows machine has, and a Vulkan driver, which the display driver installs.

.EXAMPLE
  tools/package-game.ps1 -Preset msvc-release -Module walk_poc -Scene content/test-scenes/desert-endless/scene.json

  run.cmd starts the dunes at -TimeRate game seconds a world second (1: real time; passed only
  where the scene's terrain has dunes) and the sun's day at -SunRate (60: a game day in 24
  minutes), or at -DayLength real seconds a day when that is given (3600: a sixty-minute day;
  it wins over -SunRate). [ ] step the day's length a rung and , . the world's rate, which the
  water, the smoke, the sway and the dunes run at (docs/subsystems/apps.md, "The two rates on
  four keys").

.EXAMPLE
  <sdk>/tools/package-game.ps1 -ModuleDll build/my_game.dll -Scene scenes/start/scene.json -Out my_game.zip

  From an SDK, for a game repository's own module and scene.
#>
[CmdletBinding()]
param(
  [string]$Preset,
  [string]$Module,
  [string]$ModuleDll,
  [Parameter(Mandatory = $true)][string]$Scene,
  [string]$Out,
  [int]$Width = 1600,
  [int]$Height = 900,
  [double]$TimeRate = 1,
  [double]$SunRate = 60,
  [double]$DayLength = 0
)
# A day length is the sun rate said the other way round (86,400 / -DayLength); 0 leaves -SunRate.
if ($DayLength -lt 0) { throw "-DayLength is real seconds a day, 0 or more" }
if ($DayLength -gt 0) { $SunRate = 86400.0 / $DayLength }

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

Import-Module (Join-Path $PSScriptRoot 'lib/GameContent.psm1') -Force

$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
$ExeSuffix = if ($IsWin) { '.exe' } else { '' }
$ModuleSuffix = if ($IsWin) { '.dll' } else { '.so' }

function Refuse([string]$why) {
  Write-Error "package-game: $why" -ErrorAction Continue
  exit 2
}

# ---- which layout this script is running from ------------------------------------------------

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$sdkMarker = Join-Path $Root 'sdk.json'
$FromSdk = Test-Path -LiteralPath $sdkMarker -PathType Leaf
$sdk = $null
if ($FromSdk) {
  $sdk = Get-Content -LiteralPath $sdkMarker -Raw | ConvertFrom-Json -Depth 16
  if (-not $Preset) { $Preset = [string]$sdk.preset }
  $Bin = Join-Path $Root 'bin'
  $BuildDir = $Root
}
else {
  if (-not $Preset) { $Preset = 'msvc-release' }
  $BuildDir = Join-Path $Root "build/$Preset"
  $Bin = Join-Path $BuildDir 'bin'
}
$ContentRoot = [System.IO.Path]::GetFullPath((Join-Path $Root 'content'))

# ---- what is shipped -------------------------------------------------------------------------

$exe = Join-Path $Bin "engine-view$ExeSuffix"
if (-not (Test-Path -LiteralPath $exe)) {
  if ($FromSdk) { Refuse "no $exe; the SDK is incomplete (tools/export-game-sdk.ps1 writes it)" }
  Refuse "no $exe; build it first (tools/dev.ps1 build -Preset $Preset)"
}
if ($ModuleDll) {
  $moduleFile = if ([System.IO.Path]::IsPathRooted($ModuleDll)) { $ModuleDll } else { Join-Path (Get-Location).Path $ModuleDll }
  $moduleFile = [System.IO.Path]::GetFullPath($moduleFile)
  if (-not (Test-Path -LiteralPath $moduleFile -PathType Leaf)) { Refuse "no game module $ModuleDll" }
  if (Test-UnderLocalWorkspace $moduleFile) { Refuse "$ModuleDll is under game_engine_local, which is never shipped" }
  $fromFile = [System.IO.Path]::GetFileNameWithoutExtension($moduleFile)
  if ($Module -and $Module -ne $fromFile) { Refuse "-Module $Module does not name -ModuleDll $ModuleDll; give one or the other" }
  $Module = $fromFile
  $ModuleSuffix = [System.IO.Path]::GetExtension($moduleFile)
}
elseif ($Module) {
  $moduleFile = Join-Path $Bin "game/$Module$ModuleSuffix"
  if (-not (Test-Path -LiteralPath $moduleFile)) {
    if ($FromSdk) { Refuse "no game module $moduleFile in the SDK; a module built from a game repository is given as -ModuleDll <file>" }
    Refuse "no game module $moduleFile; it is built by engine_game_module(NAME $Module) with the hotload capability on"
  }
}
else {
  Refuse 'name the module: -Module <name> (built by this tree or carried by the SDK) or -ModuleDll <file> (built elsewhere)'
}
if ($Module -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') { Refuse "the module's name '$Module' is not an identifier" }

$scenePath = if ([System.IO.Path]::IsPathRooted($Scene)) { $Scene } else { Join-Path (Get-Location).Path $Scene }
if (-not (Test-Path -LiteralPath $scenePath -PathType Leaf)) { Refuse "no scene file $Scene" }
$scenePath = [System.IO.Path]::GetFullPath($scenePath)
if (Test-UnderLocalWorkspace $scenePath) { Refuse "$Scene is under game_engine_local, which is never shipped" }

# Where the scene's content may come from, and the path each root's files keep in the package.
#   tree layout:  the repository's content/, kept at content/<path under it>
#   SDK layout:   the SDK's content/ the same way; a scene anywhere else is a game repository's,
#                 its directory is its root, kept at content/<the directory's name>/<path under it>
$roots = @()
$keep = @{}
$sceneUnderContent = $null -ne (Get-ContentRoot -Path $scenePath -Roots @($ContentRoot))
if ($sceneUnderContent) {
  $roots += $ContentRoot
  $keep[$ContentRoot] = 'content'
}
elseif ($FromSdk) {
  $sceneDir = [System.IO.Path]::GetFullPath((Split-Path $scenePath -Parent))
  $roots += $sceneDir
  $keep[$sceneDir] = 'content/' + (Split-Path $sceneDir -Leaf)
}
else {
  Refuse "$Scene is not under the repository's content/ directory: from the engine tree a package ships the repository's fixtures and nothing else (a game repository's scene is packaged from its SDK, docs/game-authoring.md)"
}

try {
  $resolved = @(Resolve-SceneContent -Scene $scenePath -Roots $roots)
}
catch {
  Refuse $_.Exception.Message
}
$traits = Get-SceneTraits -Scene $scenePath
$streams = $traits.Streams
$dunes = $traits.Dunes

# ---- the build's identity --------------------------------------------------------------------

$commit = ''
$dirty = $false
if ($FromSdk) {
  $commit = [string]$sdk.commit
  $dirty = [bool]$sdk.dirty
}
else {
  try {
    Push-Location $Root
    $commit = (& git rev-parse HEAD 2>$null | Out-String).Trim()
    $dirty = -not [string]::IsNullOrEmpty((& git status --porcelain 2>$null | Out-String).Trim())
  }
  catch { }
  finally { Pop-Location }
}
$commitShort = if ($commit.Length -ge 7) { $commit.Substring(0, 7) } else { 'nocommit' }

# ---- lay it out ------------------------------------------------------------------------------

$stageName = "$Module-$Preset-$commitShort"
$stageRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("engine-game-" + [System.Guid]::NewGuid().ToString('N'))
$stage = Join-Path $stageRoot $stageName
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'game') | Out-Null
Copy-Item -LiteralPath $exe -Destination $stage -Force
Copy-Item -LiteralPath $moduleFile -Destination (Join-Path $stage "game/$Module$ModuleSuffix") -Force
$shipped = @()
$sceneRel = $null
foreach ($f in $resolved) {
  $rel = $keep[$f.Root] + '/' + [System.IO.Path]::GetRelativePath($f.Root, $f.Path).Replace('\', '/')
  $target = Join-Path $stage $rel
  New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
  Copy-Item -LiteralPath $f.Path -Destination $target -Force
  $shipped += [ordered]@{ path = $rel; bytes = (Get-Item -LiteralPath $f.Path).Length }
  if ($f.Path -eq $scenePath) { $sceneRel = $rel.Replace('/', '\') }
}
# The material pack (docs/subsystems/materials.md), when this tree or SDK has one fetched into
# content/materials: the default sets a generated mesh's parts fall back to, with its LICENSES.md,
# so a tester's palms draw what the author's do. engine-view is told where it is (--materials),
# since the package's cache root is named and the repository's lookup does not run.
$packDir = Join-Path $ContentRoot 'materials'
$withPack = Test-Path -LiteralPath (Join-Path $packDir 'pack.json')
if ($withPack) {
  New-Item -ItemType Directory -Force -Path (Join-Path $stage 'content') | Out-Null
  Copy-Item -LiteralPath $packDir -Destination (Join-Path $stage 'content') -Recurse -Force
  foreach ($p in Get-ChildItem -LiteralPath $packDir -Recurse -File) {
    $shipped += [ordered]@{ path = 'content/materials/' + [System.IO.Path]::GetRelativePath($packDir, $p.FullName).Replace('\', '/'); bytes = $p.Length }
  }
}

# The arguments run.cmd passes, every path inside the folder: `{dir}` is the folder.
$runArgs = @('--scene', "{dir}\$sceneRel")
if ($streams) { $runArgs += '--world' }
if ($dunes -and $TimeRate -gt 0) { $runArgs += @('--time-rate', ([string]$TimeRate)) }
if ($SunRate -gt 0) { $runArgs += @('--sun-rate', ([string]$SunRate)) }
$runArgs += @('--walk', '--game', "{dir}\game\$Module$ModuleSuffix", '--report', '{dir}\report.json',
              '--ddc', '{dir}\ddc', '--width', "$Width", '--height', "$Height")
if ($withPack) { $runArgs += @('--materials', '{dir}\content\materials') }
$quoted = ($runArgs | ForEach-Object { if ($_ -match '\{dir\}') { '"' + $_.Replace('{dir}\', '%~dp0') + '"' } else { $_ } }) -join ' '

$run = @"
@echo off
rem $Module, a proof of concept on the engine (engine-view --game). Double-click to play.
rem Everything it writes stays in this folder: report.json, ddc\ (a cache), game\*.hotload\.
setlocal
cd /d "%~dp0"
echo Starting $Module. Walk with W A S D and the mouse; Escape gives the mouse back, Escape again ends.
"%~dp0engine-view$ExeSuffix" $quoted %*
set CODE=%ERRORLEVEL%
echo.
echo The report is %~dp0report.json - please send that one file back.
if "%~1"=="" pause
exit /b %CODE%
"@
[System.IO.File]::WriteAllText((Join-Path $stage 'run.cmd'), $run.Replace("`r`n", "`n").Replace("`n", "`r`n"),
                               (New-Object System.Text.ASCIIEncoding))

$readme = @"
$Module - a playable proof of concept on the engine
=====================================================

What it is: the engine's viewer (engine-view) running a small game module, $Module, in
the walking session over the scene $sceneRel. It is an early test build, made to find out how
the engine runs on other machines and to collect bug reports - not a finished game.

How to run it: unzip the folder anywhere and double-click run.cmd. A window opens on the
desert; you are on foot. Close the window, or press Escape twice, to end the run.

Keys (engine-view's action map, revision 5):
  W A S D        walk            Shift           sprint
  mouse          look            Space           jump
  E              interact        Q               use
  1 2 3 4        the game's own  F               fly / walk
  Escape         give the mouse back; again to end
  [ ]            the day longer / shorter (starts at a game day in $([math]::Round(86400 / [math]::Max($SunRate, 1) / 60)) minutes; a rung is 24 h, 4 h, 1 h, 24 min, 4 min, 1 min, 10 s or still)
  , .            the world slower / faster: the water, smoke, trees and dunes (1 is real time, 0 paused; the sky keeps its pace)
  - = 0          exposure darker / brighter / hold
  A gamepad: left stick walks, right stick looks, South jumps, West interacts.

What to send back: report.json, which appears in this folder when the run ends (whether it
went well or not). It holds the machine (graphics adapter, driver, Windows version, CPU),
how smooth the frames were, how far you walked, and how the game ended. Nothing else is
collected and nothing is sent anywhere by the program itself.

What it writes: only inside this folder - report.json, ddc\ (a cache of built geometry; delete
it freely) and game\$Module$ModuleSuffix.hotload\ (copies the engine loads the game from).

It needs a graphics card with a Vulkan driver (any recent NVIDIA, AMD or Intel driver) and the
Microsoft Visual C++ 2015-2022 Redistributable (x64), which almost every Windows machine has:
https://aka.ms/vs/17/release/vc_redist.x64.exe

Built from engine commit $commitShort$(if ($dirty) { ' (with local changes)' } else { '' }), preset $Preset.
"@
[System.IO.File]::WriteAllText((Join-Path $stage 'README.txt'), $readme.Replace("`r`n", "`n").Replace("`n", "`r`n"),
                               (New-Object System.Text.UTF8Encoding($false)))

$manifest = [ordered]@{
  format = 'engine.game.package'
  version = 1
  generated_utc = (Get-Date).ToUniversalTime().ToString('o')
  name = $stageName
  commit = $commit
  dirty = $dirty
  preset = $Preset
  layout = $(if ($FromSdk) { 'sdk' } else { 'tree' })
  module = $Module
  module_source = $(if ($ModuleDll) { $moduleFile.Replace('\', '/') } else { "bin/game/$Module$ModuleSuffix" })
  scene = $sceneRel.Replace('\', '/')
  world = [bool]$streams
  dunes = [bool]$dunes
  time_rate = $TimeRate
  sun_rate = $SunRate
  args = $runArgs
  materials = [bool]$withPack
  files = $shipped
  runtime = [ordered]@{
    dlls = @('VCRUNTIME140.dll', 'VCRUNTIME140_1.dll', 'MSVCP140.dll')
    installer = 'Microsoft Visual C++ 2015-2022 Redistributable (x64)'
    url = 'https://aka.ms/vs/17/release/vc_redist.x64.exe'
  }
}
[System.IO.File]::WriteAllText((Join-Path $stage 'package.json'), ($manifest | ConvertTo-Json -Depth 8),
                               (New-Object System.Text.UTF8Encoding($false)))

# ---- zip -------------------------------------------------------------------------------------

$stageBytes = (Get-ChildItem -Path $stage -Recurse -File | Measure-Object Length -Sum).Sum
if (-not $Out) { $Out = Join-Path $(if ($FromSdk) { (Get-Location).Path } else { $BuildDir }) "$stageName.zip" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
New-Item -ItemType Directory -Force -Path (Split-Path $Out -Parent) | Out-Null
if (Test-Path -LiteralPath $Out) { Remove-Item -Force -LiteralPath $Out }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
  $stageRoot, $Out, [System.IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item -Recurse -Force $stageRoot -ErrorAction SilentlyContinue

$zipBytes = (Get-Item -LiteralPath $Out).Length
Write-Host "package:  $Out"
Write-Host ("size:     {0:N1} MB zipped, {1:N1} MB unpacked; {2} content files" -f ($zipBytes / 1MB), ($stageBytes / 1MB), $resolved.Count)
Write-Host "folder:   $stageName (unzip, double-click run.cmd, send back report.json)"

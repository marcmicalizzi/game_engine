<#
.SYNOPSIS
  Packs engine-view, a game module and a scene into a zip a tester unzips, double-clicks and sends
  one report.json back from (docs/subsystems/apps.md, "package-game: a zip somebody else
  double-clicks"; roadmap P41).

.DESCRIPTION
  tools/package-tests.ps1's pattern for a game rather than a test suite: everything a machine with
  no toolchain, no checkout and no PowerShell 7 needs to play a proof of concept and say how it
  went.

    engine-view.exe     the preset's own build: statically linked, its shaders embedded (the
                        build's shaders/manifest.json names absolute paths into the build tree and
                        a compiler, so it stays out, as package-tests leaves it out)
    game/<Module>.dll   the game module (engine_game_module), loaded with --game
    content/...         the scene and every file it names, resolved, at their paths under the
                        repository, so the scene's relative paths still resolve
    run.cmd             double-click: starts the walking session with the game and a report,
                        and says where report.json is
    README.txt          what it is, how to run it, the keys, what to send back, and that it writes
                        nothing outside its folder
    package.json        the commit, the preset, the module, the scene, the files, and the exact
                        arguments run.cmd passes (what the package test runs)

  It refuses anything under the owner's local workspace (D:\workspace\game_engine_local: generated
  assets and workflow files are never shipped, AGENTS.md) and anything outside the repository's
  content/ directory: a package carries the repository's fixtures, nothing else.

  The far end needs the Microsoft Visual C++ 2015-2022 Redistributable (x64), which almost every
  Windows machine has, and a Vulkan driver, which the display driver installs.

.EXAMPLE
  tools/package-game.ps1 -Preset msvc-release -Module walk_poc -Scene content/test-scenes/desert-endless/scene.json
#>
[CmdletBinding()]
param(
  [string]$Preset = 'msvc-release',
  [Parameter(Mandatory = $true)][string]$Module,
  [Parameter(Mandatory = $true)][string]$Scene,
  [string]$Out,
  [int]$Width = 1600,
  [int]$Height = 900
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDir = Join-Path $Root "build/$Preset"
$Bin = Join-Path $BuildDir 'bin'
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
$ExeSuffix = if ($IsWin) { '.exe' } else { '' }
$ModuleSuffix = if ($IsWin) { '.dll' } else { '.so' }
$ContentRoot = [System.IO.Path]::GetFullPath((Join-Path $Root 'content'))
$Local = 'game_engine_local'

function Refuse([string]$why) {
  Write-Error "package-game: $why" -ErrorAction Continue
  exit 2
}

# A path is shippable when it is under the repository's content/ and nowhere near the owner's
# local workspace, whatever the spelling (a relative climb, a drive letter's case, a junction's
# name).
function Test-Shippable([string]$path) {
  $full = [System.IO.Path]::GetFullPath($path)
  if ($full -match [regex]::Escape($Local)) { return $false }
  $prefix = $ContentRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
  return $full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
}

# ---- what is shipped -------------------------------------------------------------------------

$exe = Join-Path $Bin "engine-view$ExeSuffix"
if (-not (Test-Path -LiteralPath $exe)) {
  Refuse "no $exe; build it first (tools/dev.ps1 build -Preset $Preset)"
}
$moduleFile = Join-Path $Bin "game/$Module$ModuleSuffix"
if (-not (Test-Path -LiteralPath $moduleFile)) {
  Refuse "no game module $moduleFile; it is built by engine_game_module(NAME $Module) with the hotload capability on"
}
$scenePath = if ([System.IO.Path]::IsPathRooted($Scene)) { $Scene } else { Join-Path (Get-Location).Path $Scene }
if (-not (Test-Path -LiteralPath $scenePath -PathType Leaf)) { Refuse "no scene file $Scene" }
$scenePath = [System.IO.Path]::GetFullPath($scenePath)
if (-not (Test-Shippable $scenePath)) {
  Refuse "$Scene is not under the repository's content/ directory (or is under $Local): a package ships the repository's fixtures and nothing else"
}

# The scene and every file it names, resolved against the file naming it: each string value of the
# JSON that is a relative path to an existing file or directory beside it. A JSON file it names
# (a camera path, a placements file) is read for its own names in turn.
$files = [System.Collections.Generic.List[string]]::new()
$seen = @{}
function Add-Named([string]$path) {
  $full = [System.IO.Path]::GetFullPath($path)
  if ($seen.ContainsKey($full)) { return }
  $seen[$full] = $true
  if (-not (Test-Shippable $full)) {
    Refuse "the scene names $full, which is outside the repository's content/ directory (or under $Local)"
  }
  if (Test-Path -LiteralPath $full -PathType Container) {
    foreach ($f in Get-ChildItem -LiteralPath $full -Recurse -File) { Add-Named $f.FullName }
    return
  }
  $files.Add($full)
  if ($full.EndsWith('.json', [System.StringComparison]::OrdinalIgnoreCase)) {
    $dir = Split-Path $full -Parent
    $json = Get-Content -LiteralPath $full -Raw | ConvertFrom-Json -Depth 64
    foreach ($s in (Get-Strings $json)) {
      if ($s.Length -eq 0 -or $s.Length -gt 400 -or $s -match '[\*\?"<>|]' -or $s.Contains("`n")) { continue }
      if ([System.IO.Path]::IsPathRooted($s)) { continue }
      $candidate = Join-Path $dir $s
      if (Test-Path -LiteralPath $candidate) { Add-Named $candidate }
    }
  }
}
function Get-Strings($value) {
  if ($null -eq $value) { return }
  if ($value -is [string]) { $value; return }
  if ($value -is [System.Collections.IEnumerable]) {
    foreach ($v in $value) { Get-Strings $v }
    return
  }
  if ($value -is [psobject]) {
    foreach ($p in $value.PSObject.Properties) { Get-Strings $p.Value }
  }
}
Add-Named $scenePath

# Whether the scene streams a world (a `world` block or `ruins`): `--world` is passed only then,
# since engine-view refuses it for a scene with nothing to stream.
$sceneJson = Get-Content -LiteralPath $scenePath -Raw | ConvertFrom-Json -Depth 64
$streams = ($null -ne $sceneJson.PSObject.Properties['world']) -or ($null -ne $sceneJson.PSObject.Properties['ruins'])

# ---- the build's identity --------------------------------------------------------------------

$commit = ''
$dirty = $false
try {
  Push-Location $Root
  $commit = (& git rev-parse HEAD 2>$null | Out-String).Trim()
  $dirty = -not [string]::IsNullOrEmpty((& git status --porcelain 2>$null | Out-String).Trim())
}
catch { }
finally { Pop-Location }
$commitShort = if ($commit.Length -ge 7) { $commit.Substring(0, 7) } else { 'nocommit' }

# ---- lay it out ------------------------------------------------------------------------------

$stageName = "$Module-$Preset-$commitShort"
$stageRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("engine-game-" + [System.Guid]::NewGuid().ToString('N'))
$stage = Join-Path $stageRoot $stageName
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'game') | Out-Null
Copy-Item -LiteralPath $exe -Destination $stage -Force
Copy-Item -LiteralPath $moduleFile -Destination (Join-Path $stage 'game') -Force
$shipped = @()
foreach ($f in $files) {
  $rel = [System.IO.Path]::GetRelativePath($Root, $f)
  $target = Join-Path $stage $rel
  New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
  Copy-Item -LiteralPath $f -Destination $target -Force
  $shipped += [ordered]@{ path = $rel.Replace('\', '/'); bytes = (Get-Item -LiteralPath $f).Length }
}
$sceneRel = [System.IO.Path]::GetRelativePath($Root, $scenePath).Replace('/', '\')

# The arguments run.cmd passes, every path inside the folder: `{dir}` is the folder.
$runArgs = @('--scene', "{dir}\$sceneRel")
if ($streams) { $runArgs += '--world' }
$runArgs += @('--walk', '--game', "{dir}\game\$Module$ModuleSuffix", '--report', '{dir}\report.json',
              '--ddc', '{dir}\ddc', '--width', "$Width", '--height', "$Height")
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
  [ ]            the sun's day slower / faster
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

Built from commit $commitShort$(if ($dirty) { ' (with local changes)' } else { '' }), preset $Preset.
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
  module = $Module
  scene = $sceneRel.Replace('\', '/')
  world = [bool]$streams
  args = $runArgs
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
if (-not $Out) { $Out = Join-Path $BuildDir "$stageName.zip" }
if (-not [System.IO.Path]::IsPathRooted($Out)) { $Out = Join-Path (Get-Location).Path $Out }
New-Item -ItemType Directory -Force -Path (Split-Path $Out -Parent) | Out-Null
if (Test-Path -LiteralPath $Out) { Remove-Item -Force -LiteralPath $Out }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
  $stageRoot, $Out, [System.IO.Compression.CompressionLevel]::Optimal, $false)
Remove-Item -Recurse -Force $stageRoot -ErrorAction SilentlyContinue

$zipBytes = (Get-Item -LiteralPath $Out).Length
Write-Host "package:  $Out"
Write-Host ("size:     {0:N1} MB zipped, {1:N1} MB unpacked; {2} content files" -f ($zipBytes / 1MB), ($stageBytes / 1MB), $files.Count)
Write-Host "folder:   $stageName (unzip, double-click run.cmd, send back report.json)"

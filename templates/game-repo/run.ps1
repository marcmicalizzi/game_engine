#requires -Version 7
<#
.SYNOPSIS
  Runs @@NAME@@ in the pinned SDK's engine-view, in the walking session over a scene.

.DESCRIPTION
  ./run.ps1 [-Scene <scene.json>] [-Report <file>] [-Offscreen [-Frames <n>]] [-EngineArgs <args>]

  The scene is the SDK's desert by default (content/test-scenes/desert-endless/scene.json in the
  SDK); -Scene names another, the SDK's or this repository's own. -Report writes engine-view's
  report at exit (the file a tester sends back). -Offscreen replays the SDK's synthetic walk with
  no window, for a test. -EngineArgs passes more to engine-view (`-EngineArgs "--start 10,30,20 0,0"`).
  The derived-data cache goes to ddc/ here, which .gitignore keeps out.
#>
[CmdletBinding()]
param(
  [string]$Scene,
  [string]$Report,
  [switch]$Offscreen,
  [int]$Frames = 0,
  [string]$InputMap,  # default: the SDK's content/input-map.json, the map this game pins
  [string[]]$EngineArgs = @()
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$here = $PSScriptRoot
$pin = Get-Content -LiteralPath (Join-Path $here 'engine-sdk.json') -Raw | ConvertFrom-Json
$sdk = if ([System.IO.Path]::IsPathRooted($pin.sdk)) { $pin.sdk } else { Join-Path $here $pin.sdk }
if (-not (Test-Path -LiteralPath (Join-Path $sdk 'sdk.json'))) { throw "run: no engine SDK at $sdk (engine-sdk.json names it)" }
$module = Join-Path $here 'build/@@NAME@@.dll'
if (-not (Test-Path -LiteralPath $module)) { throw "run: no $module; ./build.ps1 first" }
if (-not $Scene) { $Scene = Join-Path $sdk 'content/test-scenes/desert-endless/scene.json' }
$forward = @{ Module = $module; Scene = $Scene; Ddc = (Join-Path $here 'ddc') }
if ($Report) { $forward.Report = $Report }
if ($Offscreen) { $forward.Offscreen = $true }
if ($Frames -gt 0) { $forward.Frames = $Frames }
if ($InputMap) { $forward.InputMap = $InputMap }
if ($EngineArgs.Count -gt 0) { $forward.EngineArgs = $EngineArgs }
& (Join-Path $sdk 'tools/run-game.ps1') @forward
exit $LASTEXITCODE

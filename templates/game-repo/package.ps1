#requires -Version 7
<#
.SYNOPSIS
  Packs @@NAME@@ with the pinned SDK's engine-view and a scene into the zip a tester double-clicks.

.DESCRIPTION
  ./package.ps1 [-Scene <scene.json>] [-Out <zip>]

  The SDK's tools/package-game.ps1 over build/@@NAME@@.dll: engine-view, the module, the scene and
  every file it names, run.cmd, README.txt and package.json. The scene is the SDK's desert by
  default; a scene of this repository's own is carried with its directory's content. The tester
  unzips, double-clicks run.cmd and sends report.json back.
#>
[CmdletBinding()]
param(
  [string]$Scene,
  [string]$Out
)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$here = $PSScriptRoot
$pin = Get-Content -LiteralPath (Join-Path $here 'engine-sdk.json') -Raw | ConvertFrom-Json
$sdk = if ([System.IO.Path]::IsPathRooted($pin.sdk)) { $pin.sdk } else { Join-Path $here $pin.sdk }
if (-not (Test-Path -LiteralPath (Join-Path $sdk 'sdk.json'))) { throw "package: no engine SDK at $sdk (engine-sdk.json names it)" }
$module = Join-Path $here 'build/@@NAME@@.dll'
if (-not (Test-Path -LiteralPath $module)) { throw "package: no $module; ./build.ps1 first" }
if (-not $Scene) { $Scene = Join-Path $sdk 'content/test-scenes/desert-endless/scene.json' }
if (-not $Out) { $Out = Join-Path $here 'build/@@NAME@@.zip' }
& (Join-Path $sdk 'tools/package-game.ps1') -ModuleDll $module -Scene $Scene -Out $Out
exit $LASTEXITCODE

#requires -Version 7
<#
.SYNOPSIS
  Builds @@NAME@@ into build/@@NAME@@.dll with the pinned engine SDK's compiler, flags and schemac.

.DESCRIPTION
  ./build.ps1

  Every .cpp in src/ is one translation unit and every .schema in src/ is compiled to the state
  tables, by the SDK's tools/build-module.ps1 (engine-sdk.json says where the SDK is). Run it while
  ./run.ps1 has the game open and the session swaps the new build in on the next frame, state
  carried.
#>
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$here = $PSScriptRoot
$pin = Get-Content -LiteralPath (Join-Path $here 'engine-sdk.json') -Raw | ConvertFrom-Json
$sdk = if ([System.IO.Path]::IsPathRooted($pin.sdk)) { $pin.sdk } else { Join-Path $here $pin.sdk }
if (-not (Test-Path -LiteralPath (Join-Path $sdk 'sdk.json'))) { throw "build: no engine SDK at $sdk (engine-sdk.json names it)" }
& (Join-Path $sdk 'tools/build-module.ps1') -Source (Join-Path $here 'src') -Out (Join-Path $here 'build/@@NAME@@.dll')
exit $LASTEXITCODE

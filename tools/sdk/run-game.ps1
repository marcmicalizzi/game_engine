#requires -Version 7
<#
.SYNOPSIS
  Runs a game module in this SDK's engine-view, in the walking session over a scene
  (docs/game-authoring.md in the engine's tree; the SDK's README.md).

.DESCRIPTION
  tools/run-game.ps1 -Module <file.dll> -Scene <scene.json> [-Report <file.json>]
                     [-Offscreen [-Replay <session.jsonl>] [-Frames <n>]]
                     [-Ddc <dir>] [-TimeRate <r>] [-SunRate <r>] [-EngineArgs <args...>]

  A window opens on the scene with the module loaded (`--walk --game`); `--world` is passed where
  the scene streams one (a `world` block or `ruins`) and `--time-rate` where its terrain has dunes,
  the same rule tools/package-game.ps1 ships with. Rebuild the module while it runs
  (tools/build-module.ps1 over the same output) and the session swaps the new build in on the next
  frame with the state carried. -Report writes engine-view's report at exit, the file a tester
  sends back.

  -Offscreen runs with no window: a recorded session is replayed (`--replay-input --offscreen`),
  by default the SDK's content/sessions/walk-synthetic.jsonl, two seconds of walking with E pressed
  once, and -Frames stops it early. A `--game` session is a function of its ticks, so the same
  recording replays to the same module state every time: it is how a module is tested without a
  desktop, and what the SDK's own test does.

  -EngineArgs hands engine-view more arguments as written, because PowerShell reads a bare
  `--width` as a parameter of this script: an array from a PowerShell prompt
  (`-EngineArgs '--start','10,30,20','0,0'`, `-EngineArgs '--game-state','save.json'`) or one
  space-separated string from any shell (`-EngineArgs "--width 320 --height 200"`);
  `bin/engine-view --help` lists them.

.EXAMPLE
  tools/run-game.ps1 -Module ..\my_game\build\my_game.dll -Scene content\test-scenes\desert-endless\scene.json -Report report.json

.EXAMPLE
  tools/run-game.ps1 -Module ..\my_game\build\my_game.dll -Scene content\test-scenes\desert-endless\scene.json -Offscreen -Frames 20 -Report report.json -EngineArgs "--width 320 --height 200"
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Module,
  [Parameter(Mandatory = $true)][string]$Scene,
  [string]$Report,
  [switch]$Offscreen,
  [string]$Replay,
  [int]$Frames = 0,
  [string]$Ddc,
  [double]$TimeRate = 1,
  [double]$SunRate = 60,
  [string[]]$EngineArgs = @()
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

Import-Module (Join-Path $PSScriptRoot 'lib/GameContent.psm1') -Force

$Sdk = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$IsWin = $IsWindows -or ($env:OS -eq 'Windows_NT')
$exe = Join-Path $Sdk ('bin/engine-view' + $(if ($IsWin) { '.exe' } else { '' }))
if (-not (Test-Path -LiteralPath $exe)) { throw "run-game: no $exe; this is not a complete SDK" }

function Full([string]$p) {
  if ([System.IO.Path]::IsPathRooted($p)) { return [System.IO.Path]::GetFullPath($p) }
  return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $p))
}
$modulePath = Full $Module
if (-not (Test-Path -LiteralPath $modulePath -PathType Leaf)) { throw "run-game: no module $Module (tools/build-module.ps1 builds one)" }
$scenePath = Full $Scene
if (-not (Test-Path -LiteralPath $scenePath -PathType Leaf)) { throw "run-game: no scene $Scene" }
if (-not $Ddc) { $Ddc = Join-Path $Sdk 'ddc' }
$ddcPath = Full $Ddc

$traits = Get-SceneTraits -Scene $scenePath
$cmd = @('--scene', $scenePath)
if ($traits.Streams) { $cmd += '--world' }
if ($traits.Dunes -and $TimeRate -gt 0) { $cmd += @('--time-rate', ([string]$TimeRate)) }
if ($SunRate -gt 0) { $cmd += @('--sun-rate', ([string]$SunRate)) }
if ($Offscreen -or $Replay) {
  if (-not $Replay) { $Replay = Join-Path $Sdk 'content/sessions/walk-synthetic.jsonl' }
  $replayPath = Full $Replay
  if (-not (Test-Path -LiteralPath $replayPath -PathType Leaf)) { throw "run-game: no session recording $Replay" }
  $cmd += @('--replay-input', $replayPath, '--offscreen')
}
else {
  $cmd += '--walk'
}
$cmd += @('--game', $modulePath, '--ddc', $ddcPath)
if ($Report) { $cmd += @('--report', (Full $Report)) }
if ($Frames -gt 0) { $cmd += @('--frames', "$Frames") }
# -EngineArgs as an array from a PowerShell prompt, or as one space-separated string from another
# shell (`pwsh -File run-game.ps1 ... -EngineArgs "--width 160 --height 96"`), where an array
# cannot be spelt; a value with a comma of its own (`--start 10,30,20 0,0`) survives either way.
foreach ($a in $EngineArgs) {
  if ($a -match '\s') { $cmd += @($a -split '\s+' | Where-Object { $_ } | ForEach-Object { $_.Trim("'", '"') }) }
  else { $cmd += $a }
}

Write-Host "run-game: engine-view $($cmd -join ' ')"
& $exe @cmd
exit $LASTEXITCODE

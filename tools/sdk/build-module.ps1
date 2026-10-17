#requires -Version 7
<#
.SYNOPSIS
  Builds a game module from a directory of sources with this SDK's compiler, flags, headers and
  schemac (docs/game-authoring.md in the engine's tree; the SDK's README.md).

.DESCRIPTION
  tools/build-module.ps1 -Source <dir> -Out <file.dll>

  Every *.cpp in -Source is one unity translation unit and every *.schema is run through schemac,
  its generated state tables compiled in, exactly as the engine builds its own game modules: the
  command is the engine's cmake/EngineGameModuleBuild.cmake, read from this SDK's cmake/ with the
  relocatable cmake/build_config.cmake beside it (the headers, schemac and the schema root named
  relative to that file, so the SDK can live anywhere). Intermediate files go to
  <Out without extension>.build/ beside the output.

  The configuration records the compiler and the MSVC environment of the machine that exported
  the SDK. When that compiler is not on this machine, this script finds Visual Studio through
  vswhere, imports its x64 developer environment (VsDevCmd.bat) and hands the command that
  machine's cl.exe instead; the engine's flags are the same. It needs CMake (on PATH, or the one
  Visual Studio installs) and a Visual Studio with the C++ x64 tools. Linux is not covered by an
  exported SDK yet (the README says so).

.EXAMPLE
  tools/build-module.ps1 -Source ..\my_game\src -Out ..\my_game\build\my_game.dll
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Source,
  [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$Sdk = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$config = Join-Path $Sdk 'cmake/build_config.cmake'
$script = Join-Path $Sdk 'cmake/EngineGameModuleBuild.cmake'
foreach ($f in @($config, $script)) {
  if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { throw "build-module: no $f; this is not a complete SDK" }
}
$sourceDir = (Resolve-Path -LiteralPath $Source).Path
if (-not (Test-Path -LiteralPath $sourceDir -PathType Container)) { throw "build-module: -Source $Source is not a directory" }
$outPath = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Get-Location).Path $Out }
$outPath = [System.IO.Path]::GetFullPath($outPath)
New-Item -ItemType Directory -Force -Path (Split-Path $outPath -Parent) | Out-Null

# ---- the compiler the configuration records, or this machine's ------------------------------

$text = Get-Content -LiteralPath $config -Raw
$recordedCxx = if ($text -match '(?m)^set\(GM_CXX \[==\[(.*?)\]==\]\)') { $Matches[1] } else { '' }
$msvc = $text -match '(?m)^set\(GM_MSVC TRUE\)'
$extra = @()

function Find-VsWhere {
  $candidates = @()
  if (${env:ProgramFiles(x86)}) { $candidates += Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe' }
  if ($env:ProgramFiles) { $candidates += Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe' }
  foreach ($c in $candidates) { if (Test-Path -LiteralPath $c) { return $c } }
  return $null
}

function Find-VisualStudio {
  $vswhere = Find-VsWhere
  if (-not $vswhere) { return $null }
  $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
  if ($path) { return ([string]$path).Trim() }
  return $null
}

if ($msvc -and -not (Test-Path -LiteralPath $recordedCxx -PathType Leaf)) {
  $vs = Find-VisualStudio
  if (-not $vs) {
    throw "build-module: the SDK was exported against $recordedCxx, which is not on this machine, and vswhere finds no Visual Studio with the C++ x64 tools to use instead. Install Visual Studio 2022 or later with 'Desktop development with C++'."
  }
  $devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
  if (-not (Test-Path -LiteralPath $devcmd)) { throw "build-module: no $devcmd" }
  # The developer environment, imported into this process: INCLUDE, LIB and PATH are what cl.exe
  # needs; the whole set is taken so a tool it runs finds what it expects too.
  $lines = & cmd.exe /s /c "`"$devcmd`" -arch=amd64 -host_arch=amd64 -no_logo && set" 2>&1
  foreach ($line in $lines) {
    if ("$line" -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
  }
  $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
  if (-not $cl) { throw "build-module: $devcmd ran but cl.exe is not on its PATH; is the C++ x64 toolset installed?" }
  $extra += "-DCOMPILER=$($cl.Source.Replace('\', '/'))"
  Write-Host "build-module: the recorded compiler is not here; using $($cl.Source) from $vs"
}

# ---- cmake ----------------------------------------------------------------------------------

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmake) { $cmake = $cmake.Source }
else {
  $vs = Find-VisualStudio
  if ($vs) {
    $candidate = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $candidate) { $cmake = $candidate }
  }
}
if (-not $cmake) { throw 'build-module: cmake is not on PATH and Visual Studio has none; install CMake 3.28 or later (winget install Kitware.CMake)' }

# ---- build ----------------------------------------------------------------------------------

$cmakeArgs = @("-DCONFIG=$($config.Replace('\', '/'))", "-DSOURCE_DIR=$($sourceDir.Replace('\', '/'))",
               "-DOUTPUT=$($outPath.Replace('\', '/'))") + $extra + @('-P', $script.Replace('\', '/'))
& $cmake @cmakeArgs
$code = $LASTEXITCODE
if ($code -ne 0) {
  Write-Error "build-module: the build failed ($code)" -ErrorAction Continue
  exit $code
}
Write-Host "built:    $outPath"
exit 0

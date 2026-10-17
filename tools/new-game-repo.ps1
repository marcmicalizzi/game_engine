#requires -Version 7
<#
.SYNOPSIS
  Makes a new game repository from templates/game-repo/, pinning an exported engine SDK
  (docs/game-authoring.md; roadmap P42).

.DESCRIPTION
  tools/new-game-repo.ps1 -Name <name> -Out <dir> -Sdk <sdk dir> [-RelativeSdk]

  Instantiates the template into -Out, a new directory outside the engine repository: README.md,
  src/<name>.cpp and src/<name>.schema (a minimal, readable module: a day counter, one HUD line, one
  meter, a run that ends after three game days), build.ps1, run.ps1, package.ps1, .gitignore,
  LICENSE.md (a placeholder that says the licence is the author's), and engine-sdk.json, which holds
  the SDK's path and the fields of its sdk.json the game pins (commit, dirty, preset, CPU baseline,
  ABI version). The template is text with @@NAME@@ placeholders; -Name must be a C identifier,
  since it names the module, its schema namespace and its DLL.

  The SDK's path is written as given — absolute, or with -RelativeSdk relative to the new
  repository — so a repository that keeps its SDK beside it stays portable. `git init` is not run;
  the README says the first commit is the author's.

.EXAMPLE
  tools/new-game-repo.ps1 -Name dunes -Out D:\games\dunes -Sdk D:\games\engine-sdk -RelativeSdk
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$Name,
  [Parameter(Mandatory = $true)][string]$Out,
  [Parameter(Mandatory = $true)][string]$Sdk,
  [switch]$RelativeSdk
)

$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Template = Join-Path $Root 'templates/game-repo'

function Refuse([string]$why) {
  Write-Error "new-game-repo: $why" -ErrorAction Continue
  exit 2
}
function Full([string]$p) {
  if ([System.IO.Path]::IsPathRooted($p)) { return [System.IO.Path]::GetFullPath($p) }
  return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $p))
}

if ($Name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') {
  Refuse "the name '$Name' is not a C identifier (letters, digits and underscores, not starting with a digit): it names the module, its schema namespace and its DLL"
}
$OutDir = Full $Out
$repoPrefix = $Root.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
if ($OutDir.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase) -or $OutDir -eq $Root.TrimEnd('\', '/')) {
  Refuse "the output $OutDir is inside the engine repository; a game lives in a repository of its own (ADR-0047)"
}
if ((Test-Path -LiteralPath $OutDir) -and (Get-ChildItem -LiteralPath $OutDir -Force | Measure-Object).Count -gt 0) {
  Refuse "$OutDir exists and is not empty"
}
$SdkDir = Full $Sdk
$sdkJsonPath = Join-Path $SdkDir 'sdk.json'
if (-not (Test-Path -LiteralPath $sdkJsonPath -PathType Leaf)) {
  Refuse "$SdkDir is not an exported engine SDK (no sdk.json); tools/export-game-sdk.ps1 writes one"
}
$sdkJson = Get-Content -LiteralPath $sdkJsonPath -Raw | ConvertFrom-Json -Depth 16
if ($sdkJson.format -ne 'engine.game.sdk') { Refuse "$sdkJsonPath is not an engine.game.sdk file" }

# ---- the template, substituted ------------------------------------------------------------------

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$written = @()
foreach ($file in Get-ChildItem -LiteralPath $Template -Recurse -File -Force) {
  $rel = [System.IO.Path]::GetRelativePath($Template, $file.FullName).Replace('@@NAME@@', $Name)
  $target = Join-Path $OutDir $rel
  New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
  $text = (Get-Content -LiteralPath $file.FullName -Raw).Replace('@@NAME@@', $Name)
  [System.IO.File]::WriteAllText($target, $text, (New-Object System.Text.UTF8Encoding($false)))
  $written += $rel.Replace('\', '/')
}

# ---- engine-sdk.json: the pin ----------------------------------------------------------------------

$sdkPath = if ($RelativeSdk) { [System.IO.Path]::GetRelativePath($OutDir, $SdkDir).Replace('\', '/') } else { $SdkDir.Replace('\', '/') }
$pin = [ordered]@{
  format = 'engine.game.sdk-pin'
  version = 1
  sdk = $sdkPath
  commit = [string]$sdkJson.commit
  dirty = [bool]$sdkJson.dirty
  preset = [string]$sdkJson.preset
  cpu_baseline = [string]$sdkJson.cpu_baseline
  abi_version = [int]$sdkJson.abi_version
  # ConvertFrom-Json reads an ISO date as a DateTime; written back as the ISO string it was.
  exported_utc = $(if ($sdkJson.exported_utc -is [datetime]) { $sdkJson.exported_utc.ToUniversalTime().ToString('o') } else { [string]$sdkJson.exported_utc })
}
[System.IO.File]::WriteAllText((Join-Path $OutDir 'engine-sdk.json'), ($pin | ConvertTo-Json -Depth 4),
                               (New-Object System.Text.UTF8Encoding($false)))
$written += 'engine-sdk.json'

Write-Host "new-game-repo: $Name at $OutDir, pinning engine $(([string]$sdkJson.commit).Substring(0, 12)) ($($sdkJson.preset)) at $sdkPath"
foreach ($w in ($written | Sort-Object)) { Write-Host "  $w" }
Write-Host 'Next: ./build.ps1, then ./run.ps1. git init is yours.'
exit 0

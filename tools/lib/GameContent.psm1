#requires -Version 7
<#
.SYNOPSIS
  What a scene is made of, for the tools that ship one: tools/package-game.ps1 (a zip a tester
  double-clicks) and tools/export-game-sdk.ps1 (the game author's SDK; docs/game-authoring.md).

.DESCRIPTION
  A scene file (engine.scene.v1) names its content by relative paths: a camera path, a placements
  file, a ruins kit, a mesh. Shipping the scene means shipping every file it names, resolved against
  the file naming it, recursively through the JSON files it names in turn — and refusing anything
  that lives where a package may not reach: the owner's local workspace
  (D:\workspace\game_engine_local holds generated assets and workflow files that are never shipped,
  AGENTS.md) and anything outside the roots the caller allows (the repository's content/, an SDK's
  content/, a game repository's own scene directory).

  Two scripts need exactly that rule, so it lives here once rather than as two copies that would
  drift (the brief of roadmap P42).

    Resolve-SceneContent -Scene <scene.json> -Roots <dir>[,<dir>]
        the scene and every file it names, each with the root it was found under
    Get-SceneTraits -Scene <scene.json>
        whether the scene streams a world (--world) and whether its terrain has dunes (--time-rate)
    Test-UnderLocalWorkspace <path>
        the one refusal that holds everywhere: a path under game_engine_local, whatever the spelling
#>

Set-StrictMode -Version Latest

$script:LocalWorkspace = 'game_engine_local'

function Test-UnderLocalWorkspace {
  <#
  .SYNOPSIS
    Whether a path is under the owner's local workspace, by name, whatever the spelling (a relative
    climb, a drive letter's case, a junction's name).
  #>
  param([Parameter(Mandatory = $true)][string]$Path)
  $full = [System.IO.Path]::GetFullPath($Path)
  return $full -match [regex]::Escape($script:LocalWorkspace)
}

function Get-ContentRoot {
  <#
  .SYNOPSIS
    The first of $Roots a full path is under, or $null.
  #>
  param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string[]]$Roots)
  $full = [System.IO.Path]::GetFullPath($Path)
  foreach ($root in $Roots) {
    $prefix = [System.IO.Path]::GetFullPath($root).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if ($full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) { return $prefix.TrimEnd('\', '/') }
  }
  return $null
}

function Get-JsonStrings($value) {
  if ($null -eq $value) { return }
  if ($value -is [string]) { $value; return }
  if ($value -is [System.Collections.IEnumerable]) {
    foreach ($v in $value) { Get-JsonStrings $v }
    return
  }
  if ($value -is [psobject]) {
    foreach ($p in $value.PSObject.Properties) { Get-JsonStrings $p.Value }
  }
}

function Resolve-SceneContent {
  <#
  .SYNOPSIS
    The scene and every file it names, resolved against the file naming it.
  .DESCRIPTION
    Each string value of the scene's JSON that is a relative path to an existing file or directory
    beside it is content; a directory is every file under it; a JSON file it names (a camera path,
    a placements file, a kit's index) is read for its own names in turn. Every file must be under
    one of $Roots and never under the local workspace: the first one that is not stops the walk
    with a terminating error naming it, so a package is whole or nothing.
  .OUTPUTS
    One object per file, in the order found, with `Path` (the full path) and `Root` (the root it
    was found under, for the relative path a package or an SDK keeps it at).
  #>
  param(
    [Parameter(Mandatory = $true)][string]$Scene,
    [Parameter(Mandatory = $true)][string[]]$Roots
  )
  $files = [System.Collections.Generic.List[object]]::new()
  $seen = @{}
  function Add-Named([string]$path) {
    $full = [System.IO.Path]::GetFullPath($path)
    if ($seen.ContainsKey($full)) { return }
    $seen[$full] = $true
    if (Test-UnderLocalWorkspace $full) {
      throw "the scene names $full, which is under $($script:LocalWorkspace): the owner's generated assets and workflow files are never shipped"
    }
    $root = Get-ContentRoot -Path $full -Roots $Roots
    if ($null -eq $root) {
      throw "the scene names $full, which is outside the content a package may carry ($($Roots -join ', '))"
    }
    if (Test-Path -LiteralPath $full -PathType Container) {
      foreach ($f in Get-ChildItem -LiteralPath $full -Recurse -File) { Add-Named $f.FullName }
      return
    }
    $files.Add([pscustomobject]@{ Path = $full; Root = $root })
    if ($full.EndsWith('.json', [System.StringComparison]::OrdinalIgnoreCase)) {
      $dir = Split-Path $full -Parent
      $json = Get-Content -LiteralPath $full -Raw | ConvertFrom-Json -Depth 64
      foreach ($s in (Get-JsonStrings $json)) {
        if ($s.Length -eq 0 -or $s.Length -gt 400 -or $s -match '[\*\?"<>|]' -or $s.Contains("`n")) { continue }
        if ([System.IO.Path]::IsPathRooted($s)) { continue }
        $candidate = Join-Path $dir $s
        if (Test-Path -LiteralPath $candidate) { Add-Named $candidate }
      }
    }
  }
  $scenePath = [System.IO.Path]::GetFullPath($Scene)
  if (-not (Test-Path -LiteralPath $scenePath -PathType Leaf)) { throw "no scene file $Scene" }
  Add-Named $scenePath
  return $files.ToArray()
}

function Get-SceneTraits {
  <#
  .SYNOPSIS
    What the run script has to know about a scene: whether it streams a world (a `world` block or
    `ruins`: `--world` is passed only then, since engine-view refuses it for a scene with nothing to
    stream) and whether its terrain has dunes (a `terrain` block with `bands`: `--time-rate` is
    passed only then, since the dunes' time-lapse needs the dune generator).
  #>
  param([Parameter(Mandatory = $true)][string]$Scene)
  $json = Get-Content -LiteralPath $Scene -Raw | ConvertFrom-Json -Depth 64
  $streams = ($null -ne $json.PSObject.Properties['world']) -or ($null -ne $json.PSObject.Properties['ruins'])
  $dunes = ($null -ne $json.PSObject.Properties['terrain']) -and ($null -ne $json.terrain) -and
           ($null -ne $json.terrain.PSObject.Properties['bands'])
  return [pscustomobject]@{ Streams = [bool]$streams; Dunes = [bool]$dunes; Json = $json }
}

Export-ModuleMember -Function Resolve-SceneContent, Get-SceneTraits, Test-UnderLocalWorkspace, Get-ContentRoot

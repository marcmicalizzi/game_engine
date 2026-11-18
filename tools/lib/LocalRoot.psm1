#requires -Version 7
<#
.SYNOPSIS
  Where the local folder is: the one place a tool finds it (docs/ci/machine-paths.md, roadmap T175).

.DESCRIPTION
  The local folder holds what is never committed — generated images and meshes, the owner's
  workflow files, captures, derived-data roots, scratch (AGENTS.md; docs/content-generation.md). It
  is `$env:ENGINE_LOCAL_ROOT` when that is set, and otherwise the folder beside the primary
  checkout named after it with `_local` added: `game_engine_local` beside `game_engine`. The
  primary checkout is found from git's common directory, so every worktree of it — an agent's under
  `.claude\worktrees\`, the gate's beside it — resolves to the same folder as the primary does. The
  compiler and shader caches use the same rule (`<main checkout>_local`, tools/lib/CompilerCache.psm1),
  which is why the two agree.

  Before T175 three tools carried this machine's own path as their default and a fourth its
  overlay's; a folder named in the repository has to be right on every machine, and a path of one
  machine is right on none of the others. On this machine the derived folder is the one they named.

    Resolve-EngineLocalRoot -Explicit <dir> -Environment <dir> -CommonDir <dir>   # pure
    Get-EngineLocalRoot [-Explicit <dir>] [-From <dir>]                           # asks git
#>

Set-StrictMode -Version Latest

# The decision alone, from the three facts it rests on, so a test can give it any of them:
# a directory the caller named, the environment's ENGINE_LOCAL_ROOT, and git's common directory for
# the checkout the tool runs from. $null when none of them answers.
function Resolve-EngineLocalRoot {
  [CmdletBinding()]
  param([string]$Explicit = '', [string]$Environment = '', [string]$CommonDir = '')
  if ($Explicit) { return [IO.Path]::GetFullPath($Explicit) }
  if ($Environment) { return [IO.Path]::GetFullPath($Environment) }
  if (-not $CommonDir) { return $null }
  $common = [IO.Path]::GetFullPath($CommonDir).TrimEnd('\', '/')
  # A non-bare repository's common directory is `<primary checkout>/.git`; anything else (a bare
  # repository, a separate git directory) has no checkout beside which the folder could be.
  if ((Split-Path -Leaf $common) -ne '.git') { return $null }
  $primary = Split-Path -Parent $common
  $parent = Split-Path -Parent $primary
  if (-not $parent) { return $null }
  return (Join-Path $parent ((Split-Path -Leaf $primary) + '_local'))
}

# The local folder for a tool run from $From (by default this module's own checkout), or a refusal
# that says how to name it when neither the caller, the environment nor git can.
function Get-EngineLocalRoot {
  [CmdletBinding()]
  param([string]$Explicit = '', [string]$From = (Join-Path $PSScriptRoot '..' '..'))
  $common = ''
  if (-not $Explicit -and -not $env:ENGINE_LOCAL_ROOT) {
    $out = & git -C $From rev-parse --path-format=absolute --git-common-dir 2>$null
    if ($LASTEXITCODE -eq 0 -and $out) { $common = "$out".Trim() }
  }
  $root = Resolve-EngineLocalRoot -Explicit $Explicit -Environment $env:ENGINE_LOCAL_ROOT -CommonDir $common
  if (-not $root) {
    throw "no local folder: $From is not inside a git checkout; set ENGINE_LOCAL_ROOT (or pass -LocalRoot) to the folder that holds generated outputs, outside the repository"
  }
  return $root
}

Export-ModuleMember -Function Resolve-EngineLocalRoot, Get-EngineLocalRoot

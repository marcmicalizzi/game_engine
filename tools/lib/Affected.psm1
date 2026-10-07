# Which tests a change can reach (tools/dev.ps1 test -Affected). Pure functions over a module list
# and a list of changed paths, so the rule is tested as a rule (tools/affected.Tests.ps1).
#
# The rule, and why it is shaped this way (AGENTS.md, "What to run, and when"):
#   - A changed file belongs to the module whose directory holds it (build/<preset>/modules.json).
#   - A module's tests can be reached by a change in anything it depends on, so the affected set is
#     the changed modules and every module that depends on one, transitively.
#   - A capability (an optional module) is linked by hosts and by other modules' *test* targets
#     without being anyone's dependency, which the module graph does not record; so a changed
#     capability also selects every module above the layer its directory is in. The directory's
#     layer, not the module's: a capability's schema types are a module of their own at the bottom
#     of the graph that lives in the capability's directory (world_schemas in systems/world), and
#     a change there is a change to the capability.
#   - Two modules may share a directory for that reason; a file in it belongs to both.
#   - What no module owns and everything is built from — the build system, the test support, the
#     vendored code, the schema compiler, the committed content — selects everything, and says so.
#   - Documentation selects nothing but the checks that always run.
# The lint and the documentation check always run: they are seconds, and they are the two checks a
# change of any kind can fail.

Set-StrictMode -Version Latest

$script:AlwaysTests = @('lint.banned_patterns', 'lint.licenses', 'docs_check')
$script:LayerDirs = @('core', 'foundation', 'domain', 'systems', 'apps', 'game')

function Get-AffectedTests {
  <#
  .SYNOPSIS
    The tests a set of changed paths can reach.
  .OUTPUTS
    A hashtable: All (bool), Reason (why everything, or $null), Modules (affected module names,
    sorted), Changed (module names holding a changed file), Tools (bool: the tools' own tests),
    Filter (a CTest -R regex, or $null when All), Notes (strings worth printing).
  #>
  param(
    [Parameter(Mandatory)] [object[]]$Modules,   # modules.json's "modules": name, layer, path, optional, deps
    [Parameter(Mandatory)] [string[]]$Layers,    # modules.json's "layers", lowest first
    [AllowEmptyCollection()] [string[]]$Changed = @()
  )
  $notes = New-Object System.Collections.Generic.List[string]
  $changedModules = New-Object System.Collections.Generic.HashSet[string]
  $tools = $false
  $all = $null

  # Longest path first, so core/schema's file is not claimed by a module at core/.
  $byPath = $Modules | Sort-Object { $_.path.Length } -Descending
  foreach ($raw in $Changed) {
    $path = ($raw -replace '\\', '/').Trim()
    if (-not $path) { continue }
    $owners = @()
    foreach ($m in $byPath) {
      if ($path -eq $m.path -or $path.StartsWith($m.path + '/')) {
        if ($owners.Count -gt 0 -and $m.path.Length -lt $owners[0].path.Length) { break }
        $owners += $m
      }
    }
    if ($owners.Count -gt 0) {
      # A module's own page moves with it and tests nothing.
      if ($path -like '*.md') { continue }
      foreach ($m in $owners) { [void]$changedModules.Add($m.name) }
      continue
    }
    if ($path -like 'docs/*' -or $path -like '*.md' -or $path -like '.github/*' -or
        $path -in @('.clang-format', '.gitignore', '.gitattributes', 'LICENSE', 'NOTICE')) { continue }
    if ($path -like 'tools/schemac/*') { if (-not $all) { $all = "the schema compiler changed ($path)" }; continue }
    if ($path -like 'tools/*') { $tools = $true; continue }
    $top = ($path -split '/')[0]
    if ($top -in $script:LayerDirs -and ($path -split '/').Count -gt 2) {
      # A directory under a layer that this preset has no module for: a capability it switched off.
      $notes.Add("not in this preset, so not tested by it: $path")
      continue
    }
    if (-not $all) { $all = "$path is built into or read by everything" }
  }

  if ($all) {
    return @{ All = $true; Reason = $all; Modules = @($Modules | ForEach-Object { $_.name } | Sort-Object)
              Changed = @($changedModules | Sort-Object); Tools = $true; Filter = $null; Notes = $notes.ToArray() }
  }

  # Everything that depends on a changed module, transitively.
  $affected = New-Object System.Collections.Generic.HashSet[string]
  foreach ($name in $changedModules) { [void]$affected.Add($name) }
  $grew = $true
  while ($grew) {
    $grew = $false
    foreach ($m in $Modules) {
      if ($affected.Contains($m.name)) { continue }
      foreach ($d in $m.deps) {
        if ($affected.Contains($d)) { [void]$affected.Add($m.name); $grew = $true; break }
      }
    }
  }
  # A changed capability: every module of a higher layer may link it into a host or a test.
  foreach ($name in @($changedModules)) {
    $m = $Modules | Where-Object { $_.name -eq $name } | Select-Object -First 1
    if (-not $m.optional) { continue }
    $directory = ($m.path -split '/')[0]
    $rank = [array]::IndexOf($Layers, $(if ($directory -in $Layers) { $directory } else { $m.layer }))
    $added = 0
    foreach ($other in $Modules) {
      if ([array]::IndexOf($Layers, $other.layer) -gt $rank -and $affected.Add($other.name)) { $added++ }
    }
    if ($added -gt 0) { $notes.Add("$name is a capability: $added more modules above its layer may link it into a host or a test") }
  }

  $names = @($affected | Sort-Object)
  $parts = New-Object System.Collections.Generic.List[string]
  if ($names.Count -gt 0) {
    # A module's tests, its bench's smoke run, and a part of its tests registered as a test of its
    # own, `<module>.<part>` (engine_view.frame_loop, whose flights would double engine_view's run).
    $parts.Add('^(bench\.)?(' + (($names | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')(\.[A-Za-z0-9_]+)?$')
  }
  foreach ($t in $script:AlwaysTests) { $parts.Add('^' + [regex]::Escape($t) + '$') }
  if ($tools) { $parts.Add('^tools\.') }
  return @{ All = $false; Reason = $null; Modules = $names; Changed = @($changedModules | Sort-Object)
            Tools = $tools; Filter = ($parts -join '|'); Notes = $notes.ToArray() }
}

function Get-ChangedPaths {
  <#
  .SYNOPSIS
    The paths that differ from a base: committed since the merge base, staged or not, and untracked.
  #>
  param([Parameter(Mandatory)] [string]$Root, [string]$Base = 'main')
  $paths = New-Object System.Collections.Generic.HashSet[string]
  Push-Location $Root
  try {
    $mergeBase = (& git merge-base $Base HEAD 2>$null)
    if ($LASTEXITCODE -ne 0 -or -not $mergeBase) { throw "no merge base between '$Base' and HEAD; name another with -Base" }
    foreach ($line in (& git diff --name-only $mergeBase HEAD)) { if ($line) { [void]$paths.Add($line) } }
    foreach ($line in (& git diff --name-only HEAD)) { if ($line) { [void]$paths.Add($line) } }
    foreach ($line in (& git ls-files --others --exclude-standard)) { if ($line) { [void]$paths.Add($line) } }
  }
  finally { Pop-Location }
  return @($paths | Sort-Object)
}

Export-ModuleMember -Function Get-AffectedTests, Get-ChangedPaths

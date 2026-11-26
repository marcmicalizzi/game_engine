# Which tests a change can reach (tools/dev.ps1 test -Affected). Get-AffectedTests is a pure
# function over the module graph, the changed paths and the text references Get-AffectedMentions
# found, so the rule is tested as a rule (tools/affected.Tests.ps1); docs/ci/what-to-run.md,
# "What affected means", is the page.
#
# The rule, and why it is shaped this way (AGENTS.md, "What to run, and when"):
#   - A changed file belongs to the module whose directory holds it (build/<preset>/modules.json).
#     Two modules may share a directory (a capability and its schema types); a file in it belongs
#     to both.
#   - **The test edges** (roadmap T31; the `edges` rule, the default): a module's tests run when a
#     changed module is one their targets link or start. modules.json's `test_reach` holds that
#     closure, walked at configure time over the test executable, the bench and (for an app) the
#     app itself: what they link, what they build first (add_dependencies: the executables a test
#     starts), and what they are compiled with the path of ($<TARGET_FILE:...>)
#     (cmake/EngineTestReach.cmake). It holds every module the module depends on, and the
#     capabilities a test links that its module does not, which is what the module graph alone
#     could not say and why the older rule selected everything above a changed capability's layer.
#   - What a test reads at run time and no link records is found from the text, when the rule runs
#     (Get-AffectedMentions), so an edit that adds a reference counts without a configure:
#       content/<dir>/...   the modules whose files name `content/<dir>` (or build a content path
#                           out of pieces, which is every directory), through every content
#                           directory whose files refer to that one (a scene's `../../flora/...`);
#       tools/...           a script (or a file one reads): the tests whose command names it or a
#                           file of tools/ that names it, transitively (`runs`; the commands from
#                           Get-ToolTests); anything else there (a C++ tool), every tools test; and
#                           the modules whose files run a script from the tree (`"tools/...`,
#                           `${CMAKE_SOURCE_DIR}/tools/`);
#       tests/support/*.h   the modules whose files include it, or a support header that does;
#       a shader            also every module whose shaders include it, through relative paths;
#       cmake/<file>.cmake  the modules whose CMakeLists call what it defines, when all it holds is
#                           function and macro definitions and every caller is a module's
#                           CMakeLists; anything else, everything.
#     A reference in a module's tests/ or bench/ reaches that module's tests; one anywhere else in
#     a module (its sources, its CMakeLists) is a change to the module, so it reaches everything
#     that links it.
#   - **A changed file in a module's tests/ or bench/** is compiled into that module's test and
#     bench executables, which nothing links: it selects that module's tests alone (2026-11-18:
#     three test sources of the renderer had selected the fifteen modules whose tests link it). A
#     header there selects too the tests whose files include it, through other headers (water's
#     tests include gfx's water_reference.h); a shader there is a shader (above).
#   - A source file a test target compiles from outside every module (tests/support/test_main.cpp,
#     apps/tests/...) and the directory of a target that is no module (a game module) are in
#     `test_reach` too, so they reach the tests that build them.
#   - **It never selects too little**: what it cannot place — the build system, the vendored code,
#     the schema compiler, a content file at the top of content/, a support source no target lists,
#     a reference from a file no module holds — selects everything, and says so.
#   - `modules` (-Rule) is the older rule, the module graph and the capability's layer, used too
#     when modules.json has no test edges (a tree configured before them); `all` selects
#     everything.
#   - Documentation selects nothing but the checks that always run.
#   - The tools' own tests (`tools.*`) start the apps they test (tools.game_sdk exports and runs
#     engine-view; the e10 harness reads this tree's binaries), and CTest records no target a
#     test needs, so when any of them runs every app the graph declares is built with them
#     (`Apps`): the app set comes from engine_app() through modules.json, not from a list written
#     here.
# The lint, the documentation check and the structure check (the hubs' ceilings, T81) always run:
# they are seconds, and they are the checks a change of any kind can fail — a page that grows is
# documentation and selects no module, and it can still pass its ceiling.

Set-StrictMode -Version Latest

$script:AlwaysTests = @('lint.banned_patterns', 'lint.licenses', 'docs_check', 'structure_check')
$script:LayerDirs = @('core', 'foundation', 'domain', 'systems', 'apps', 'game')

function Get-AffectedTests {
  <#
  .SYNOPSIS
    The tests a set of changed paths can reach.
  .OUTPUTS
    A hashtable: All (bool), Reason (why everything, or $null), Modules (affected module names,
    sorted), Changed (module names holding a changed file), Tools (bool: the tools' own tests),
    Filter (a CTest -R regex, or $null when All), Apps (the apps to build because the tools'
    tests run: every module of the apps layer when Tools, else none), Notes (strings worth
    printing).
  #>
  param(
    [Parameter(Mandatory)] [object[]]$Modules,   # modules.json's "modules": name, layer, path, optional, deps[, test_reach]
    [Parameter(Mandatory)] [string[]]$Layers,    # modules.json's "layers", lowest first
    [AllowEmptyCollection()] [string[]]$Changed = @(),
    [AllowEmptyCollection()] [object[]]$Mentions = @(),   # Get-AffectedMentions' records
    [ValidateSet('edges', 'modules', 'all')] [string]$Rule = 'edges',
    [AllowEmptyCollection()] [string[]]$Disabled = @(),   # modules.json's "disabled_capabilities"
    # Each test whose command names a file under tools/, with those files (Get-ToolTests, from
    # `ctest --show-only=json-v1`). Absent, a change under tools/ runs every tools.* test.
    [AllowNull()] [hashtable]$ToolTests = $null
  )
  if ($Rule -eq 'all') {
    $apps = @($Modules | Where-Object { $_.layer -eq 'apps' } | ForEach-Object { $_.name } | Sort-Object)
    return @{ All = $true; Reason = 'the rule `all` was asked for'; Modules = @($Modules | ForEach-Object { $_.name } | Sort-Object)
              Changed = @(); Tools = $true; ToolTests = @(); Filter = $null; Apps = $apps; Notes = @() }
  }
  $withEdges = @($Modules | Where-Object { $_.PSObject.Properties.Name -contains 'test_reach' -and $null -ne $_.test_reach })
  if ($Rule -eq 'edges' -and $Modules.Count -gt 0 -and $withEdges.Count -eq $Modules.Count) {
    return Get-AffectedByEdges -Modules $Modules -Changed $Changed -Mentions $Mentions -Disabled $Disabled -ToolTests $ToolTests
  }
  $notes = New-Object System.Collections.Generic.List[string]
  if ($Rule -eq 'edges' -and $Modules.Count -gt 0) {
    $notes.Add('modules.json has no test edges (configured before roadmap T31): the module graph''s rule')
  }
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

  $apps = @($Modules | Where-Object { $_.layer -eq 'apps' } | ForEach-Object { $_.name } | Sort-Object)
  if ($all) {
    return @{ All = $true; Reason = $all; Modules = @($Modules | ForEach-Object { $_.name } | Sort-Object)
              Changed = @($changedModules | Sort-Object); Tools = $true; ToolTests = @(); Filter = $null; Apps = $apps
              Notes = $notes.ToArray() }
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
  if (-not $tools) { $apps = @() }
  return @{ All = $false; Reason = $null; Modules = $names; Changed = @($changedModules | Sort-Object)
            Tools = $tools; ToolTests = @(); Filter = ($parts -join '|'); Apps = $apps; Notes = $notes.ToArray() }
}

# The modules whose directory holds a path: the deepest, or every module that shares it.
function Get-PathOwners([object[]]$ByPath, [string]$Path) {
  $owners = @()
  foreach ($m in $ByPath) {
    if ($Path -eq $m.path -or $Path.StartsWith($m.path + '/')) {
      if ($owners.Count -gt 0 -and $m.path.Length -lt $owners[0].path.Length) { break }
      $owners += $m
    }
  }
  return , $owners
}

function Get-AffectedByEdges {
  # The `edges` rule (the header above). Same result shape as Get-AffectedTests.
  param([object[]]$Modules, [string[]]$Changed, [object[]]$Mentions, [string[]]$Disabled, [hashtable]$ToolTests)
  $notes = New-Object System.Collections.Generic.List[string]
  $disabledNames = New-Object System.Collections.Generic.HashSet[string]
  foreach ($d in @($Disabled)) { if ($d) { [void]$disabledNames.Add($d.ToLowerInvariant()) } }
  $changedModules = New-Object System.Collections.Generic.HashSet[string]   # reach every test linking them
  $direct = New-Object System.Collections.Generic.HashSet[string]           # these modules' tests, no further
  $script:edgeAll = $null
  function Set-All([string]$why) { if (-not $script:edgeAll) { $script:edgeAll = $why } }
  $tools = $false
  $byPath = @($Modules | Sort-Object { $_.path.Length } -Descending)

  # A source file outside every module that a test target compiles, and a non-module target's
  # directory, from each module's test_reach.
  $fileMap = @{}
  $dirMap = @{}
  foreach ($m in $Modules) {
    foreach ($f in @($m.test_reach.files)) { if ($f) { if (-not $fileMap.ContainsKey($f)) { $fileMap[$f] = @() }; $fileMap[$f] += $m.name } }
    foreach ($d in @($m.test_reach.dirs)) { if ($d) { if (-not $dirMap.ContainsKey($d)) { $dirMap[$d] = @() }; $dirMap[$d] += $m.name } }
  }
  $dirsByLength = @($dirMap.Keys | Sort-Object { $_.Length } -Descending)
  function Find-Reached([string]$path) {
    if ($fileMap.ContainsKey($path)) { return , @($fileMap[$path]) }
    foreach ($d in $dirsByLength) { if ($path.StartsWith($d + '/')) { return , @($dirMap[$d]) } }
    return , @()
  }

  # A file that names something a test reads: whose tests that reaches.
  function Add-Reader([string]$file, [string]$what) {
    $file = $file -replace '\\', '/'
    if ($file -like '*.md' -or $file -like 'docs/*' -or $file -like 'content/*') { return }
    $owners = Get-PathOwners $byPath $file
    if ($owners.Count -gt 0) {
      # A test or bench source of the module reaches its tests; a header there may be included by
      # another module's tests too (water's include gfx's water_reference.h), so it counts as a
      # change to the module, as anything else of the module does.
      $testSource = $file -notmatch '\.(h|hpp|inl)$'
      foreach ($o in $owners) {
        if ($testSource -and ($file.StartsWith($o.path + '/tests/') -or $file.StartsWith($o.path + '/bench/'))) { [void]$direct.Add($o.name) }
        else { [void]$changedModules.Add($o.name) }
      }
      return
    }
    $reached = Find-Reached $file
    if ($reached.Count -gt 0) { foreach ($n in $reached) { [void]$direct.Add($n) }; return }
    if ($file -like 'tools/*' -or $file -like 'templates/*') { $script:edgeTools = $true; return }
    Set-All "$file names $what, and no module holds it"
  }
  $script:edgeTools = $false

  $slang = New-Object System.Collections.Generic.List[string]
  $contentDirs = New-Object System.Collections.Generic.HashSet[string]
  $headers = New-Object System.Collections.Generic.List[string]
  $cmake = New-Object System.Collections.Generic.List[string]
  $testHeaders = New-Object System.Collections.Generic.List[string]
  $toolScripts = New-Object System.Collections.Generic.List[string]
  $toolsChanged = $false
  foreach ($raw in $Changed) {
    $path = ($raw -replace '\\', '/').Trim()
    if (-not $path) { continue }
    $owners = Get-PathOwners $byPath $path
    if ($owners.Count -gt 0) {
      if ($path -like '*.md') { continue }
      # A file of a module's tests/ or bench/ is compiled into (or read by) that module's test and
      # bench executables, and nothing links them: their tests alone. A header there may be
      # included by another module's tests (water's include gfx's water_reference.h), so its
      # includers' tests too; a shader there keeps the shader rule below.
      $inTests = @($owners | Where-Object { $path.StartsWith($_.path + '/tests/') -or $path.StartsWith($_.path + '/bench/') })
      if ($inTests.Count -eq $owners.Count -and $path -notlike '*.slang') {
        foreach ($m in $owners) { [void]$direct.Add($m.name) }
        if ($path -match '\.(h|hpp|inl)$') { $testHeaders.Add($path) }
        continue
      }
      foreach ($m in $owners) { [void]$changedModules.Add($m.name) }
      if ($path -like '*.slang') { $slang.Add($path) }
      continue
    }
    if ($path -like 'docs/*' -or $path -like '*.md' -or $path -like '.github/*' -or
        $path -in @('.clang-format', '.gitignore', '.gitattributes', 'LICENSE', 'NOTICE')) { continue }
    if ($path -like 'tools/schemac/*') { Set-All "the schema compiler changed ($path)"; continue }
    if ($path -like 'third_party/*') { Set-All "$path is vendored code"; continue }
    $reached = Find-Reached $path
    if ($reached.Count -gt 0) { foreach ($n in $reached) { [void]$direct.Add($n) }; continue }
    $parts = $path -split '/'
    if ($path -like 'tools/*') {
      # A script, or a file a script reads, reaches the tests whose command names it or a file of
      # tools/ that names it (Get-AffectedMentions' `runs`); a C++ tool (tools/shader_meter) or
      # anything else there, every tools test, as before the map existed.
      $toolsChanged = $true
      $leaf = [IO.Path]::GetFileName($path)
      if ($null -ne $ToolTests -and $ToolTests.Count -gt 0 -and $leaf -ne 'CMakeLists.txt' -and
          $path -match '\.(ps1|psm1|sh|cmd|json|txt)$') { $toolScripts.Add($path) }
      else { $tools = $true }
      continue
    }
    # The game author's templates are read by the tools that export and start a game repository
    # (tools/new-game-repo.ps1, tools.game_sdk), and by nothing a module builds.
    if ($path -like 'templates/*') { $tools = $true; continue }
    if ($path -like 'tests/support/*') {
      if ($path -match '\.(h|hpp|inl)$') { $headers.Add($path) } else { Set-All "$path is built into every test" }
      continue
    }
    if ($path -like 'content/*') {
      if ($parts.Count -ge 3) { [void]$contentDirs.Add($parts[1]) } else { Set-All "$path is in no content directory" }
      continue
    }
    if ($path -match '^cmake/[^/]+\.cmake$') { $cmake.Add($path); continue }
    # A directory under a layer that no module holds and no test target reaches: a capability this
    # preset switched off when its name says so, and otherwise something built or read in a way
    # the edges do not see (apps/engine_shell, which engine-host copies by a glob), so everything.
    if ($parts[0] -in $script:LayerDirs -and $parts.Count -gt 2) {
      if ($disabledNames.Contains($parts[1].ToLowerInvariant())) { $notes.Add("not in this preset, so not tested by it: $path") }
      else { Set-All "$path is in no module and no test target reaches it" }
      continue
    }
    Set-All "$path is built into or read by everything"
  }

  $mentions = @($Mentions | Where-Object { $null -ne $_ })
  foreach ($c in $cmake) {
    $callers = @($mentions | Where-Object { $_.kind -eq 'cmake' -and $_.target -eq $c })
    foreach ($rec in $callers) {
      if ($rec.file -eq '*') { Set-All "$c $($rec.why)"; continue }
      $owners = Get-PathOwners $byPath $rec.file
      if ($owners.Count -eq 0) { Set-All "$($rec.file) calls what $c defines, and no module holds it"; continue }
      foreach ($o in $owners) { [void]$changedModules.Add($o.name) }
    }
    if ($callers.Count -eq 0) { $notes.Add("$c defines what no CMakeLists calls") }
    else { $notes.Add("$c is called from $(@($callers | ForEach-Object { $_.file } | Sort-Object -Unique) -join ', ')") }
  }
  foreach ($rec in @($mentions | Where-Object { $_.kind -eq 'slang' -and $slang.Contains($_.target) })) {
    $owners = Get-PathOwners $byPath ($rec.file -replace '\\', '/')
    if ($owners.Count -eq 0) { Set-All "$($rec.file) includes $($rec.target), and no module holds it"; continue }
    foreach ($o in $owners) { [void]$changedModules.Add($o.name) }
  }
  if ($contentDirs.Count -gt 0) {
    # Every content directory whose files refer to a changed one, transitively.
    $grew = $true
    while ($grew) {
      $grew = $false
      foreach ($rec in @($mentions | Where-Object { $_.kind -eq 'refers' -and $contentDirs.Contains(($_.target -split '/')[1]) })) {
        $from = ($rec.file -replace '\\', '/') -split '/'
        if ($from.Count -ge 3 -and $contentDirs.Add($from[1])) { $grew = $true }
      }
    }
    foreach ($rec in @($mentions | Where-Object { $_.kind -eq 'reads' -and ($_.target -eq 'content/*' -or
                                                  ($_.target -like 'content/*' -and $contentDirs.Contains(($_.target -split '/')[1]))) })) {
      Add-Reader $rec.file $rec.target
    }
    $notes.Add("content: $(@($contentDirs | Sort-Object) -join ', ') and the modules whose files name them")
  }
  if ($toolsChanged) {
    foreach ($rec in @($mentions | Where-Object { $_.kind -eq 'reads' -and $_.target -eq 'tools' })) { Add-Reader $rec.file 'a script in tools/' }
  }
  foreach ($h in $headers) {
    $includers = @($mentions | Where-Object { $_.kind -eq 'includes' -and $_.target -eq $h })
    foreach ($rec in $includers) { if ($rec.file -notlike 'tests/support/*') { Add-Reader $rec.file $h } }
    $notes.Add("$h is included by $($includers.Count) file(s)")
  }
  # A test header's includers (transitively through headers, from the mentions): a test or bench
  # file of a module reaches that module's tests; a module's own source that includes one is a
  # change to the module; a test target's shared source reaches the tests built from it.
  foreach ($h in $testHeaders) {
    $includers = @($mentions | Where-Object { $_.kind -eq 'includes' -and $_.target -eq $h })
    $others = New-Object System.Collections.Generic.SortedSet[string] ([StringComparer]::Ordinal)
    foreach ($rec in $includers) {
      $file = $rec.file -replace '\\', '/'
      $owners = Get-PathOwners $byPath $file
      if ($owners.Count -gt 0) {
        foreach ($o in $owners) {
          [void]$others.Add($o.name)
          if ($file.StartsWith($o.path + '/tests/') -or $file.StartsWith($o.path + '/bench/')) { [void]$direct.Add($o.name) }
          else { [void]$changedModules.Add($o.name) }
        }
        continue
      }
      $reached = Find-Reached $file
      if ($reached.Count -gt 0) { foreach ($n in $reached) { [void]$direct.Add($n) }; continue }
      Set-All "$file includes $h, and no module holds it"
    }
    $notes.Add("$h is a test header; included by $($includers.Count) file(s)$(if ($others.Count) { " of $(@($others) -join ', ')" })")
  }
  # A script under tools/: the tests whose command names it, or a file of tools/ that names it.
  $toolSelected = New-Object System.Collections.Generic.SortedSet[string] ([StringComparer]::Ordinal)
  foreach ($tp in $toolScripts) {
    $reach = New-Object System.Collections.Generic.HashSet[string] ([StringComparer]::OrdinalIgnoreCase)
    [void]$reach.Add($tp)
    foreach ($rec in @($mentions | Where-Object { $_.kind -eq 'runs' -and $_.target -eq $tp })) { [void]$reach.Add(($rec.file -replace '\\', '/')) }
    $hit = @(foreach ($name in $ToolTests.Keys) { if (@($ToolTests[$name] | Where-Object { $reach.Contains($_) }).Count -gt 0) { $name } })
    foreach ($n in $hit) { [void]$toolSelected.Add($n) }
    if ($hit.Count -eq 0) { $notes.Add("no test runs $tp, nor a script that names it") }
  }
  if ($script:edgeTools) { $tools = $true }

  $apps = @($Modules | Where-Object { $_.layer -eq 'apps' } | ForEach-Object { $_.name } | Sort-Object)
  if ($script:edgeAll) {
    return @{ All = $true; Reason = $script:edgeAll; Modules = @($Modules | ForEach-Object { $_.name } | Sort-Object)
              Changed = @($changedModules | Sort-Object); Tools = $true; ToolTests = @(); Filter = $null; Apps = $apps
              Notes = $notes.ToArray() }
  }

  # A module's tests are selected when what they link or start holds a changed module. The reach
  # is test_reach's modules, the module itself, and its dependencies through the graph (which the
  # link closure holds already; kept, so a reach can only be wider than the graph's).
  $byName = @{}
  foreach ($m in $Modules) { $byName[$m.name] = $m }
  $closure = @{}
  function Get-Reach([string]$name) {
    if ($closure.ContainsKey($name)) { return $closure[$name] }
    $set = New-Object System.Collections.Generic.HashSet[string]
    $queue = New-Object System.Collections.Generic.Queue[string]
    $queue.Enqueue($name)
    foreach ($r in @($byName[$name].test_reach.modules)) { if ($r) { $queue.Enqueue($r) } }
    while ($queue.Count -gt 0) {
      $n = $queue.Dequeue()
      if (-not $set.Add($n)) { continue }
      if ($byName.ContainsKey($n)) { foreach ($d in @($byName[$n].deps)) { if ($d) { $queue.Enqueue($d) } } }
    }
    $closure[$name] = $set
    return $set
  }
  $affected = New-Object System.Collections.Generic.HashSet[string]
  foreach ($n in $direct) { [void]$affected.Add($n) }
  if ($changedModules.Count -gt 0) {
    foreach ($m in $Modules) {
      $reach = Get-Reach $m.name
      foreach ($c in $changedModules) { if ($reach.Contains($c)) { [void]$affected.Add($m.name); break } }
    }
  }

  $names = @($affected | Sort-Object)
  $parts = New-Object System.Collections.Generic.List[string]
  if ($names.Count -gt 0) {
    $parts.Add('^(bench\.)?(' + (($names | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')(\.[A-Za-z0-9_]+)?$')
  }
  foreach ($t in $script:AlwaysTests) { $parts.Add('^' + [regex]::Escape($t) + '$') }
  if ($tools) { $parts.Add('^tools\.') }
  if ($toolSelected.Count -gt 0) {
    $parts.Add('^(' + ((@($toolSelected) | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')$')
  }
  $anyTools = $tools -or $toolSelected.Count -gt 0
  if (-not $anyTools) { $apps = @() }
  return @{ All = $false; Reason = $null; Modules = $names; Changed = @($changedModules | Sort-Object)
            Tools = $anyTools; ToolTests = @($toolSelected); Filter = ($parts -join '|'); Apps = $apps; Notes = $notes.ToArray() }
}

function Get-ToolTests {
  <#
  .SYNOPSIS
    Each test whose command names a file under tools/, with those files relative to the checkout:
    the map Get-AffectedTests' -ToolTests takes, read from `ctest --show-only=json-v1`'s text.
  #>
  param(
    [Parameter(Mandatory)] [AllowEmptyString()] [string]$Json,
    [Parameter(Mandatory)] [string]$Root
  )
  $map = @{}
  if (-not $Json.Trim()) { return $map }
  $doc = $Json | ConvertFrom-Json
  $prefix = (($Root -replace '\\', '/').TrimEnd('/')) + '/tools/'
  $pattern = [regex]::Escape($prefix) + '[^;"''\s]+'
  foreach ($t in @($doc.tests)) {
    if (-not $t -or -not ($t.PSObject.Properties.Name -contains 'command')) { continue }
    $named = New-Object System.Collections.Generic.SortedSet[string] ([StringComparer]::Ordinal)
    foreach ($arg in @($t.command)) {
      foreach ($m in [regex]::Matches(("$arg" -replace '\\', '/'), $pattern, 'IgnoreCase')) {
        [void]$named.Add('tools/' + $m.Value.Substring($prefix.Length))
      }
    }
    if ($named.Count -gt 0) { $map[[string]$t.name] = @($named) }
  }
  return $map
}

function Get-AffectedMentions {
  <#
  .SYNOPSIS
    The text references the `edges` rule needs for a set of changed paths, read from the working
    tree with git grep (tracked and untracked, not ignored): records of kind, target and file.
  .DESCRIPTION
    reads    target content/<dir> (or content/* for a path built from pieces) or tools: a file that
             names it;
    refers   target content/<dir>: a content file in another directory that refers to it;
    includes target tests/support/<h>: a file that includes it, or a support header that does;
    slang    target a changed shader: a shader that includes it, through any number of others;
    cmake    target cmake/<f>.cmake: a CMakeLists or .cmake that calls what it defines, or file '*'
             with `why` when it holds more than definitions (then the rule selects everything).
  #>
  param(
    [Parameter(Mandatory)] [string]$Root,
    [AllowEmptyCollection()] [string[]]$Changed = @(),
    [string]$Rev = ''   # a commit to read instead of the working tree (to replay a branch's choice)
  )
  $out = New-Object System.Collections.Generic.List[object]
  function Add-Record([string]$kind, [string]$target, [string]$file, [string]$why = '') {
    $out.Add([pscustomobject]@{ kind = $kind; target = $target; file = ($file -replace '\\', '/'); why = $why })
  }
  $source = if ($Rev) { @($Rev) } else { @('--untracked') }
  # git is started through ProcessStartInfo.ArgumentList rather than `& git`: on Linux and macOS
  # PowerShell expands a wildcard in a native command's argument against the current directory,
  # a variable's value included, so `*CMakeLists.txt` reached git as `CMakeLists.txt` from the
  # root of a checkout and the grep read the root CMakeLists alone (2026-11-18, the three Linux
  # containers). An argument list is passed as it is, on every platform.
  function Invoke-Grep([string[]]$options, [string[]]$pathspec) {
    $gitArgs = @('-C', $Root, 'grep') + $options + $source + @('--') + $pathspec
    $info = [System.Diagnostics.ProcessStartInfo]::new('git')
    foreach ($a in $gitArgs) { $info.ArgumentList.Add($a) }
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $process = [System.Diagnostics.Process]::Start($info)
    $errors = $process.StandardError.ReadToEndAsync()
    $stdout = $process.StandardOutput.ReadToEnd()
    $process.WaitForExit()
    [void]$errors.Result
    $process.Dispose()
    $lines = @($stdout -split "`r?`n" | Where-Object { $_ })
    if ($Rev) { $lines = @($lines | ForEach-Object { $_.Substring($Rev.Length + 1) }) }
    return $lines
  }
  function Find-Files([string]$pattern, [string[]]$pathspec) { return Invoke-Grep @('-l', '-I', '-E', '-e', $pattern) $pathspec }
  # The lines that match, without their comment (`//` in C++, Slang and the schemas, `#` in
  # PowerShell and CMake), so a comment that names a path reads nothing: File and Text each.
  function Find-Lines([string[]]$patterns, [string[]]$pathspec) {
    $options = @('-n', '-I', '-E')
    foreach ($p in $patterns) { $options += @('-e', $p) }
    foreach ($line in (Invoke-Grep $options $pathspec)) {
      if ($line -notmatch '^([^:]+):\d+:(.*)$') { continue }
      $file = $Matches[1]; $text = $Matches[2]
      if ($file -match '\.(ps1|psm1|cmake)$' -or $file -like '*CMakeLists.txt') { $text = $text -replace '#.*$', '' }
      elseif ($file -match '\.(c|cc|cpp|h|hpp|inl|slang|schema)$') { $text = $text -replace '//.*$', '' }
      [pscustomobject]@{ File = $file; Text = $text }
    }
  }
  $code = @('.', ':!content', ':!docs', ':!*.md', ':!third_party')
  $paths = @($Changed | ForEach-Object { ($_ -replace '\\', '/').Trim() } | Where-Object { $_ })

  # content/<dir>: the directories that refer to it, then what names any of them.
  $dirs = New-Object System.Collections.Generic.HashSet[string]
  foreach ($p in $paths) { $s = $p -split '/'; if ($s[0] -eq 'content' -and $s.Count -ge 3) { [void]$dirs.Add($s[1]) } }
  if ($dirs.Count -gt 0) {
    $queue = New-Object System.Collections.Generic.Queue[string]
    foreach ($d in $dirs) { $queue.Enqueue($d) }
    while ($queue.Count -gt 0) {
      $d = $queue.Dequeue()
      $e = [regex]::Escape($d)
      foreach ($f in (Find-Files "\.\./$e/|content/$e/" @('content', ':!*.md'))) {
        $s = $f -split '/'
        if ($s.Count -lt 3 -or $s[1] -eq $d) { continue }
        Add-Record 'refers' "content/$d" $f
        if ($dirs.Add($s[1])) { $queue.Enqueue($s[1]) }
      }
    }
    # One read of the tree for every directory, and for a content path built out of pieces
    # ("content" joined to a name), which names no directory and so reads them all. A log
    # category called "content" builds no path.
    $alt = (@($dirs) | ForEach-Object { [regex]::Escape($_) }) -join '|'
    $named = "content/($alt)([^A-Za-z0-9_.-]|$)"
    $bare = '"content"|["''/]content/?["'']'
    foreach ($hit in (Find-Lines @($named, $bare) $code)) {
      foreach ($d in $dirs) {
        if ($hit.Text -match "content/$([regex]::Escape($d))([^A-Za-z0-9_.-]|$)") { Add-Record 'reads' "content/$d" $hit.File }
      }
      if ($hit.Text -match $bare -and $hit.Text -notmatch 'LOG_CATEGORY') { Add-Record 'reads' 'content/*' $hit.File }
    }
  }

  # tools/: what runs a script from the tree.
  $toolPaths = @($paths | Where-Object { $_ -like 'tools/*' -and $_ -notlike 'tools/schemac/*' })
  if ($toolPaths.Count -gt 0) {
    $script = '"/?tools/|[}]/tools/'
    foreach ($hit in (Find-Lines @($script) @('core', 'foundation', 'domain', 'systems', 'apps', 'game', 'tests'))) {
      if ($hit.Text -match $script) { Add-Record 'reads' 'tools' $hit.File }
    }
  }

  # A file under tools/: the files of tools/ that name it, through any number of others (a test
  # script names the script it runs, which imports a module by its file name), so the rule can
  # select the tests whose command names any of them. By name, comments included: a name in a
  # comment selects a test too many, never too few.
  foreach ($tp in $toolPaths) {
    $names = New-Object System.Collections.Generic.HashSet[string]
    [void]$names.Add([IO.Path]::GetFileName($tp))
    # A file of a directory under tools/ may be read by the directory, by no name of its own
    # (structure-check.ps1 reads every tools/structure-raises/*.json): the directory's name too.
    # Not tools/lib/, whose modules are imported by name and whose name every script holds.
    $dirParts = $tp -split '/'
    if ($dirParts.Count -gt 2 -and $dirParts[1] -ne 'lib') { [void]$names.Add($dirParts[1]) }
    $seen = New-Object System.Collections.Generic.HashSet[string]
    [void]$seen.Add($tp)
    $grew = $true
    while ($grew) {
      $grew = $false
      $alt = (@($names) | ForEach-Object { [regex]::Escape($_) }) -join '|'
      foreach ($f in (Find-Files "(^|[^A-Za-z0-9_.-])($alt)([^A-Za-z0-9_.-]|$)" @('tools', ':!tools/schemac', ':!*.md'))) {
        if (-not $seen.Add($f)) { continue }
        Add-Record 'runs' $tp $f
        if ($names.Add([IO.Path]::GetFileName($f))) { $grew = $true }
      }
    }
  }

  # tests/support headers and the headers in a module's tests/ or bench/: their includers, through
  # the headers that include them.
  foreach ($h in @($paths | Where-Object { $_ -match '\.(h|hpp|inl)$' -and ($_ -like 'tests/support/*' -or
                                           $_ -match '^(core|foundation|domain|systems|apps|game)/.+/(tests|bench)/') })) {
    $names = New-Object System.Collections.Generic.HashSet[string]
    [void]$names.Add([IO.Path]::GetFileName($h))
    $seen = New-Object System.Collections.Generic.HashSet[string]
    $grew = $true
    while ($grew) {
      $grew = $false
      $alt = (@($names) | ForEach-Object { [regex]::Escape($_) }) -join '|'
      foreach ($f in (Find-Files "#[[:space:]]*include[[:space:]]*[<`"]([^`">]*/)?($alt)[`">]" @('.', ':!third_party'))) {
        if (-not $seen.Add($f)) { continue }
        Add-Record 'includes' $h $f
        $relay = $f -like 'tests/support/*' -or ($f -match '\.(h|hpp|inl)$' -and $f -match '/(tests|bench)/')
        if ($relay -and $names.Add([IO.Path]::GetFileName($f))) { $grew = $true }
      }
    }
  }

  # A shader: the shaders that include it, transitively, by name.
  foreach ($sh in @($paths | Where-Object { $_ -like '*.slang' })) {
    $names = New-Object System.Collections.Generic.HashSet[string]
    [void]$names.Add([IO.Path]::GetFileName($sh))
    $seen = New-Object System.Collections.Generic.HashSet[string]
    $grew = $true
    while ($grew) {
      $grew = $false
      $alt = (@($names) | ForEach-Object { [regex]::Escape($_) }) -join '|'
      foreach ($f in (Find-Files "#[[:space:]]*include[[:space:]]*`"([^`"]*/)?($alt)`"" @('*.slang'))) {
        if (-not $seen.Add($f)) { continue }
        Add-Record 'slang' $sh $f
        if ($names.Add([IO.Path]::GetFileName($f))) { $grew = $true }
      }
    }
  }

  # cmake/<f>.cmake: narrow only when it holds nothing but definitions.
  foreach ($c in @($paths | Where-Object { $_ -match '^cmake/[^/]+\.cmake$' })) {
    if ($Rev) {
      $text = @(& git -C $Root show "${Rev}:$c" 2>$null)
      $exists = $LASTEXITCODE -eq 0
    } else {
      $full = Join-Path $Root $c
      $exists = Test-Path -LiteralPath $full
      $text = if ($exists) { @(Get-Content -LiteralPath $full) } else { @() }
    }
    if (-not $exists) { Add-Record 'cmake' $c '*' 'was removed'; continue }
    $scan = Get-CMakeDefinitions $text
    if ($scan.Other) { Add-Record 'cmake' $c '*' "does more than define functions ($($scan.Other))"; continue }
    $stem = [IO.Path]::GetFileNameWithoutExtension($c)
    $words = @($scan.Names) + @($stem)
    foreach ($w in $words) {
      $lines = Invoke-Grep @('-n', '-I', '-w', '-F', '-e', $w) @('*.cmake', '*CMakeLists.txt')
      foreach ($line in $lines) {
        if ($line -notmatch '^([^:]+):\d+:(.*)$') { continue }
        $file = $Matches[1]; $text = $Matches[2]
        if ($file -eq $c) { continue }
        if ($text -match '^\s*#') { continue }
        if ($w -eq $stem -and $text -match '^\s*include\s*\(') { continue }
        if ($text -match "^\s*(function|macro)\s*\(\s*$([regex]::Escape($w))\b") { Add-Record 'cmake' $c '*' "has a function another file defines too ($w in $file)"; continue }
        Add-Record 'cmake' $c $file
      }
    }
  }
  return $out.ToArray()
}

function Get-CMakeDefinitions {
  <#
  .SYNOPSIS
    The functions and macros a CMake file defines, and the first command it runs outside one
    (Other; $null when it runs none, so including it only defines them).
  #>
  param([AllowEmptyCollection()] [AllowEmptyString()] [string[]]$Lines = @())
  $names = New-Object System.Collections.Generic.List[string]
  $other = $null
  $block = 0      # function/macro nesting
  $depth = 0      # open parentheses of the command being read
  $quoted = $false
  foreach ($line in $Lines) {
    # The line without its comment, and its parentheses counted, outside quoted arguments.
    $sb = New-Object System.Text.StringBuilder
    $open = 0; $close = 0
    for ($i = 0; $i -lt $line.Length; $i++) {
      $ch = $line[$i]
      if ($quoted) {
        if ($ch -eq '\') { $i++; continue }
        if ($ch -eq '"') { $quoted = $false }
        continue
      }
      if ($ch -eq '"') { $quoted = $true; continue }
      if ($ch -eq '#') { break }
      if ($ch -eq '(') { $open++ } elseif ($ch -eq ')') { $close++ }
      [void]$sb.Append($ch)
    }
    $text = $sb.ToString()
    if ($depth -eq 0 -and $text -match '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(') {
      $cmd = $Matches[1].ToLowerInvariant()
      if ($cmd -in @('function', 'macro')) {
        if ($block -eq 0 -and $text -match '^\s*\w+\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)') { $names.Add($Matches[1]) }
        $block++
      } elseif ($cmd -in @('endfunction', 'endmacro')) {
        $block--
      } elseif ($block -eq 0 -and -not $other -and $cmd -ne 'include_guard') {
        $other = $cmd
      }
    } elseif ($depth -eq 0 -and $block -eq 0 -and $text.Trim() -and -not $other) {
      $other = 'text outside a command'
    }
    $depth += $open - $close
    if ($depth -lt 0 -and -not $other) { $other = 'parentheses that do not balance' }
  }
  # A file this reading cannot follow to its end is not narrowed.
  if (-not $other -and ($block -ne 0 -or $depth -ne 0 -or $quoted)) { $other = 'a block or an argument left open' }
  return @{ Names = $names.ToArray(); Other = $other }
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

function Get-AffectedTargets {
  <#
  .SYNOPSIS
    The build targets `tools/dev.ps1 build -Affected` builds for an affected set: each module's
    library (and its _impl, the archive behind a whole-archive module's interface target), its test
    executable and its bench, of those the build tree has. An app is a module in modules.json, so an
    app that links a changed module is in the set already, and its target is engine_<app>.
  .DESCRIPTION
    A test's other executables come with it: a test that runs another binary names it with
    add_dependencies (engine_cli's tests on engine-host, engine-view and engine-content, hotload's
    on its game modules), so building the test target builds them. What the selected tests do not
    need is not built: a change in domain/flora compiles what can reach flora's tests and not the
    city, the muscle system or the editor. Sorted, so the same change asks for the same build.

    -Apps (Get-AffectedTests' Apps) adds each named app's binary, engine_<app>, and not its tests:
    the tools' tests start the apps but run none of their tests. An app's own add_dependencies
    bring what it starts (engine-view its game modules, engine-mcp engine-host).

    -Helpers (Get-TestHelperTargets) adds the executables a selected test's command runs that no
    module target brings (T225: `platform.error_mode`'s probe), each that the tree has.
  #>
  param(
    [AllowEmptyCollection()] [string[]]$Modules = @(),
    [AllowEmptyCollection()] [string[]]$Available = @(),
    [AllowEmptyCollection()] [string[]]$Apps = @(),
    [AllowEmptyCollection()] [string[]]$Helpers = @()
  )
  $have = New-Object System.Collections.Generic.HashSet[string]
  foreach ($t in $Available) { if ($t) { [void]$have.Add($t) } }
  $out = New-Object System.Collections.Generic.SortedSet[string] ([StringComparer]::Ordinal)
  foreach ($m in $Modules) {
    foreach ($t in @("engine_$m", "engine_${m}_impl", "engine_${m}_tests", "engine_${m}_bench")) {
      if ($have.Contains($t)) { [void]$out.Add($t) }
    }
  }
  foreach ($a in $Apps) { if ($have.Contains("engine_$a")) { [void]$out.Add("engine_$a") } }
  foreach ($h in $Helpers) { if ($h -and $have.Contains($h)) { [void]$out.Add($h) } }
  return @($out)
}

function Get-TestHelperTargets {
  <#
  .SYNOPSIS
    The build targets a selected test's command runs that are not a module's: every argument of
    the command, in `ctest --show-only=json-v1`'s text, whose file name (without `.exe`) is a target
    of the tree, for each test whose name the affected set's filter matches (roadmap T225).
  .DESCRIPTION
    `platform.error_mode` is `add_test(NAME platform.error_mode COMMAND
    engine_platform_error_mode_probe)`: its command is an executable of its own that no module's
    library, test executable or bench links or depends on, so the module's targets did not build it
    and CTest reported the selected test Not Run. CTest writes a target's file into the command
    whether or not it is built, so the command is where every such helper is named, however the
    build declares it: the command itself, or a `$<TARGET_FILE:...>` argument to a script.
    -Filter $null (everything selected) takes every test's.
  #>
  param(
    [Parameter(Mandatory)] [AllowEmptyString()] [string]$Json,
    [AllowNull()] [AllowEmptyString()] [string]$Filter,
    [AllowEmptyCollection()] [string[]]$Available = @()
  )
  $have = New-Object System.Collections.Generic.HashSet[string]
  foreach ($t in $Available) { if ($t) { [void]$have.Add($t) } }
  $out = New-Object System.Collections.Generic.SortedSet[string] ([StringComparer]::Ordinal)
  if (-not $Json.Trim()) { return @($out) }
  $doc = $Json | ConvertFrom-Json
  foreach ($t in @($doc.tests)) {
    if (-not $t -or -not ($t.PSObject.Properties.Name -contains 'command')) { continue }
    if ($Filter -and -not ([string]$t.name -match $Filter)) { continue }
    foreach ($arg in @($t.command)) {
      $leaf = ("$arg" -replace '\\', '/').TrimEnd('/')
      $leaf = $leaf.Substring($leaf.LastIndexOf('/') + 1)
      if ($leaf -match '(?i)^(.+)\.exe$') { $leaf = $Matches[1] }
      if ($leaf -and $have.Contains($leaf)) { [void]$out.Add($leaf) }
    }
  }
  return @($out)
}

function ConvertFrom-NinjaTargets {
  # `ninja -t targets all` as target names: the phony aliases CMake writes for its targets
  # ("engine_flora: phony"), not the files.
  param([AllowEmptyCollection()] [AllowEmptyString()] [string[]]$Lines = @())
  $names = foreach ($line in $Lines) { if ("$line" -match '^([A-Za-z0-9_.+-]+): phony\s*$') { $Matches[1] } }
  return @($names)
}

Export-ModuleMember -Function Get-AffectedTests, Get-ToolTests, Get-AffectedMentions,Get-CMakeDefinitions, Get-ChangedPaths, Get-AffectedTargets, Get-TestHelperTargets, ConvertFrom-NinjaTargets

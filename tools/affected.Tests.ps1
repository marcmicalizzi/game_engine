#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/lib/Affected.psm1, the rule behind `tools/dev.ps1 test -Affected`. Runs under
  CTest as `tools.affected`.

.DESCRIPTION
  The rule decides which tests an agent runs before handing a branch back, so a hole in it is a
  defect that reaches the merge gate unseen. It is tested as a rule over a small module graph
  written here — nothing reads the tree or git.

      pwsh tools/affected.Tests.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'lib/Affected.psm1') -Force

$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { Write-Host "       threw: $($_.Exception.Message)"; $ok = $false }
  if ($ok) { Write-Host "  ok   $what" } else { Write-Host "  FAIL $what" -ForegroundColor Red; $failures.Add($what) }
}

$layers = @('core', 'foundation', 'domain', 'systems', 'apps', 'game')
function M([string]$name, [string]$layer, [string]$path, [string[]]$deps = @(), [switch]$Optional) {
  [pscustomobject]@{ name = $name; layer = $layer; path = $path; optional = [bool]$Optional; deps = $deps }
}
# A graph with the shapes the real one has: a chain, a leaf, a module nested under another's
# directory, a capability nobody depends on, and two hosts.
$modules = @(
  (M 'base' 'core' 'core/base'),
  (M 'schema' 'core' 'core/schema' @('base')),
  (M 'schema_types' 'core' 'core/schema/types' @('schema')),
  (M 'math' 'core' 'core/math' @('base')),
  (M 'io' 'foundation' 'foundation/io' @('base')),
  (M 'gfx' 'domain' 'domain/gfx' @('base', 'math', 'io')),
  (M 'physics' 'domain' 'domain/physics' @('base', 'math')),
  (M 'sky' 'domain' 'domain/sky' @('base', 'math') -Optional),
  (M 'renderer' 'systems' 'systems/renderer' @('base', 'gfx')),
  (M 'cloth_schemas' 'core' 'systems/cloth' @('base', 'schema') -Optional),
  (M 'cloth' 'systems' 'systems/cloth' @('base', 'physics', 'cloth_schemas') -Optional),
  (M 'engine_view' 'apps' 'apps/engine_view' @('base', 'renderer')),
  (M 'engine_cli' 'apps' 'apps/engine_cli' @('base', 'io'))
)
function Affected([string[]]$changed) { Get-AffectedTests -Modules $modules -Layers $layers -Changed $changed }
function Runs($r, [string]$test) { $r.All -or ($test -match $r.Filter) }

Write-Host 'a leaf'
$r = Affected @('domain/physics/src/solver.cpp')
Test-That 'a changed module and what depends on it, and nothing above it when it is no capability' { ($r.Modules -join ',') -eq 'cloth,physics' }
Test-That 'its tests and its bench run' { (Runs $r 'physics') -and (Runs $r 'bench.physics') -and (Runs $r 'cloth') }
Test-That 'and a part of its tests registered on its own as <module>.<part>' { (Runs $r 'physics.long_flight') -and -not (Runs $r 'gfx.long_flight') }
Test-That 'an unrelated module does not' { -not (Runs $r 'gfx') -and -not (Runs $r 'renderer') -and -not (Runs $r 'engine_view') }
Test-That 'a name that only contains the module name does not match' { -not (Runs $r 'physics_extra') -and -not (Runs $r 'my.physics') }
Test-That 'the lint, the licence check and the documentation check always run' { (Runs $r 'lint.banned_patterns') -and (Runs $r 'lint.licenses') -and (Runs $r 'docs_check') }
Test-That 'and the structure check, but not its own tests' { (Runs $r 'structure_check') -and -not (Runs $r 'tools.structure_check') }
Test-That 'the tools own tests do not' { -not (Runs $r 'tools.lint') -and -not $r.Tools }

Write-Host 'a module in the middle'
$r = Affected @('domain/gfx/shaders/scene.slang')
Test-That 'reaches its dependents transitively' { ($r.Modules -join ',') -eq 'engine_view,gfx,renderer' }
Test-That 'and not a host that does not link it' { -not (Runs $r 'engine_cli') }

Write-Host 'the bottom'
$r = Affected @('core/base/include/core/base/types.h')
Test-That 'reaches every module' { $r.Modules.Count -eq $modules.Count -and -not $r.All }

Write-Host 'nested directories'
$r = Affected @('core/schema/types/a.schema')
Test-That 'a file belongs to the deepest module that holds it' { ($r.Changed -join ',') -eq 'schema_types' }
$r = Affected @('core/schema/src/reader.cpp')
Test-That 'and the outer module keeps its own' { ($r.Changed -join ',') -eq 'schema' -and ($r.Modules -contains 'schema_types') }

Write-Host 'a capability'
$r = Affected @('domain/sky/src/earth.cpp')
Test-That 'selects itself' { $r.Modules -contains 'sky' }
Test-That 'and every module above its layer, which may link it into a host or a test' {
  ($r.Modules -contains 'renderer') -and ($r.Modules -contains 'cloth') -and ($r.Modules -contains 'engine_view') -and ($r.Modules -contains 'engine_cli')
}
Test-That 'but nothing at or below its layer that does not depend on it' { -not ($r.Modules -contains 'gfx') -and -not ($r.Modules -contains 'physics') -and -not ($r.Modules -contains 'base') }
Test-That 'and says why' { @($r.Notes | Where-Object { $_ -match 'sky is a capability' }).Count -eq 1 }
$r = Affected @('systems/cloth/src/cloth.cpp')
Test-That 'a capability in a higher layer reaches only what is above it' { ($r.Modules -join ',') -eq 'cloth,cloth_schemas,engine_cli,engine_view' }
Test-That 'a file in a directory two modules share belongs to both' { ($r.Changed -join ',') -eq 'cloth,cloth_schemas' }
Test-That 'and its schema types, a module at the bottom, do not drag in everything above the bottom' {
  -not ($r.Modules -contains 'gfx') -and -not ($r.Modules -contains 'physics') -and -not ($r.Modules -contains 'renderer')
}

Write-Host 'what everything is built from'
foreach ($path in 'cmake/EngineModule.cmake', 'CMakeLists.txt', 'CMakePresets.json', 'core/CMakeLists.txt',
                  'tests/support/test_main.cpp', 'third_party/doctest/doctest.h', 'tools/schemac/main.cpp',
                  'content/test-scenes/desert-erg/scene.json') {
  $r = Affected @($path)
  Test-That "$path selects everything, and says so" { $r.All -and $r.Reason -and $null -eq $r.Filter }
}

Write-Host 'what tests nothing'
foreach ($path in 'docs/plan/04-renderer.md', 'AGENTS.md', 'domain/gfx/README.md', 'content/test-scenes/desert-erg/README.md',
                  '.github/workflows/ci.yml', '.clang-format') {
  $r = Affected @($path)
  Test-That "$path selects only the checks that always run" { -not $r.All -and $r.Modules.Count -eq 0 -and (Runs $r 'docs_check') -and (Runs $r 'structure_check') -and -not (Runs $r 'gfx') }
}
$r = Affected @()
Test-That 'no change at all is the same' { -not $r.All -and $r.Modules.Count -eq 0 -and (Runs $r 'lint.banned_patterns') }

Write-Host 'the tools'
$r = Affected @('tools/lint.ps1')
Test-That 'a script selects the tools own tests and no module' { $r.Tools -and (Runs $r 'tools.lint') -and (Runs $r 'tools.affected') -and $r.Modules.Count -eq 0 }

Write-Host 'a capability this preset switched off'
$r = Affected @('domain/ruins/src/blocks.cpp')
Test-That 'is not tested by this preset, and the run says so instead of testing everything' {
  -not $r.All -and $r.Modules.Count -eq 0 -and @($r.Notes | Where-Object { $_ -match 'not in this preset' }).Count -eq 1
}

Write-Host 'several at once'
$r = Affected @('domain\physics\src\solver.cpp', 'tools/lint.ps1', 'docs/adr/README.md')
Test-That 'backslashes are paths too, and the sets add up' { ($r.Modules -join ',') -eq 'cloth,physics' -and $r.Tools }
$r = Affected @('domain/physics/src/solver.cpp', 'cmake/EngineModule.cmake')
Test-That 'one path that selects everything is enough' { $r.All }

Write-Host 'build -Affected: the targets'
$ninja = ConvertFrom-NinjaTargets -Lines @(
  'engine_physics: phony', 'engine_physics_tests: phony', 'engine_physics_bench: phony', 'engine_cloth_impl: phony',
  'engine_cloth_tests: phony', 'engine_gfx: phony', 'engine_gfx_tests: phony', 'engine_engine_view: phony',
  'engine_engine_view_tests: phony', 'domain/physics/libengine_physics.a: CXX_STATIC_LIBRARY_LINKER__engine_physics_Debug',
  'all: phony', '')
Test-That 'ninja''s phony aliases are the target names, and its files are not' {
  ($ninja -contains 'engine_physics') -and ($ninja -contains 'all') -and -not ($ninja -match '/') -and $ninja.Count -eq 10
}
$r = Affected @('domain/physics/src/solver.cpp')
$t = Get-AffectedTargets -Modules $r.Modules -Available $ninja
Test-That 'each affected module''s library, archive, tests and bench that the tree has, sorted' {
  ($t -join ',') -eq 'engine_cloth_impl,engine_cloth_tests,engine_physics,engine_physics_bench,engine_physics_tests'
}
Test-That 'and nothing the change cannot reach' { -not ($t -contains 'engine_gfx') -and -not ($t -contains 'engine_engine_view') -and -not ($t -contains 'all') }
$r = Affected @('domain/gfx/src/device.cpp')
$t = Get-AffectedTargets -Modules $r.Modules -Available $ninja
Test-That 'an app that links the changed module is a module of the set, so its binary and its end-to-end tests build' {
  ($t -contains 'engine_engine_view') -and ($t -contains 'engine_engine_view_tests') -and -not ($t -contains 'engine_physics')
}
Test-That 'a change no module holds builds nothing' { (Get-AffectedTargets -Modules @() -Available $ninja).Count -eq 0 }
$ninjaApps = $ninja + @('engine_engine_cli', 'engine_engine_cli_tests')
$r = Affected @('tools/export-game-sdk.ps1')
$t = Get-AffectedTargets -Modules $r.Modules -Apps $r.Apps -Available $ninjaApps
Test-That 'a tools/-only change names every app of the graph, because the tools tests start them' { ($r.Apps -join ',') -eq 'engine_cli,engine_view' }
Test-That 'and builds their binaries, not their tests (tools.game_sdk runs engine-view)' {
  ($t -join ',') -eq 'engine_engine_cli,engine_engine_view'
}
$r = Affected @('domain/physics/src/solver.cpp', 'docs/adr/README.md')
Test-That 'a change that does not reach the tools tests names no app' { $r.Apps.Count -eq 0 -and -not $r.Tools }
$r = Affected @('domain/physics/src/solver.cpp', 'tools/lint.ps1')
$t = Get-AffectedTargets -Modules $r.Modules -Apps $r.Apps -Available $ninjaApps
Test-That 'a module and a script: the module''s targets and the apps together' {
  ($t -contains 'engine_physics_tests') -and ($t -contains 'engine_engine_view') -and ($t -contains 'engine_engine_cli') -and -not ($t -contains 'engine_engine_view_tests')
}
$r = Affected @('CMakeLists.txt')
Test-That 'when everything is selected the apps are named too' { $r.All -and ($r.Apps -join ',') -eq 'engine_cli,engine_view' }

Write-Host 'the test edges (roadmap T31): the same shapes, with what each module''s tests link and start'
# test_reach as cmake/EngineTestReach.cmake writes it. renderer's tests link sky, which renderer
# does not depend on; engine_cli's tests start engine-view (add_dependencies), so they reach all
# engine_view links; engine_view's start a game module (game/walk) and compile a shared test file
# from apps/tests; every test links the test main. gfx's row leaves out a dependency on purpose:
# the graph's dependencies count whatever the closure says.
function E([string]$name, [string]$layer, [string]$path, [string[]]$deps, [string[]]$reach, [string[]]$files = @(), [string[]]$dirs = @(), [switch]$Optional) {
  [pscustomobject]@{ name = $name; layer = $layer; path = $path; optional = [bool]$Optional; deps = $deps
                     test_reach = [pscustomobject]@{ modules = $reach; files = $files; dirs = $dirs } }
}
$main = @('tests/support/test_main.cpp')
$edgeModules = @(
  (E 'base' 'core' 'core/base' @() @('base') $main),
  (E 'schema' 'core' 'core/schema' @('base') @('base', 'schema') $main),
  (E 'schema_types' 'core' 'core/schema/types' @('schema') @('base', 'schema', 'schema_types') $main),
  (E 'math' 'core' 'core/math' @('base') @('base', 'math') $main),
  (E 'io' 'foundation' 'foundation/io' @('base') @('base', 'io') $main),
  (E 'gfx' 'domain' 'domain/gfx' @('base', 'math', 'io') @('base', 'gfx') $main),
  (E 'physics' 'domain' 'domain/physics' @('base', 'math') @('base', 'math', 'physics') $main),
  (E 'sky' 'domain' 'domain/sky' @('base', 'math') @('base', 'math', 'sky') $main -Optional),
  (E 'renderer' 'systems' 'systems/renderer' @('base', 'gfx') @('base', 'gfx', 'io', 'math', 'renderer', 'sky') $main),
  (E 'cloth_schemas' 'core' 'systems/cloth' @('base', 'schema') @('base', 'cloth_schemas', 'schema') $main -Optional),
  (E 'cloth' 'systems' 'systems/cloth' @('base', 'physics', 'cloth_schemas') @('base', 'cloth', 'cloth_schemas', 'math', 'physics', 'schema') $main -Optional),
  (E 'engine_view' 'apps' 'apps/engine_view' @('base', 'renderer') @('base', 'engine_view', 'gfx', 'io', 'math', 'renderer', 'sky') ($main + 'apps/tests/host_tests.cpp') @('game/walk')),
  (E 'engine_cli' 'apps' 'apps/engine_cli' @('base', 'io') @('base', 'engine_cli', 'engine_view', 'gfx', 'io', 'math', 'renderer', 'sky') $main @('game/walk'))
)
function Edges([string[]]$changed, [object[]]$mentions = @()) {
  Get-AffectedTests -Modules $edgeModules -Layers $layers -Changed $changed -Mentions $mentions
}
# (Not `R`: that is PowerShell's alias of Invoke-History, and an alias wins over a function.)
function Ref([string]$kind, [string]$target, [string]$file, [string]$why = '') { [pscustomobject]@{ kind = $kind; target = $target; file = $file; why = $why } }

$r = Edges @('domain/sky/src/earth.cpp')
Test-That 'a capability reaches the tests that link it, and not everything above its layer' { ($r.Modules -join ',') -eq 'engine_cli,engine_view,renderer,sky' }
Test-That 'renderer''s tests link it without renderer depending on it' { Runs $r 'renderer' }
Test-That 'cloth, above its layer and linking none of it, is not selected now' { -not (Runs $r 'cloth') -and -not $r.All }
$r = Edges @('systems/renderer/src/frame.cpp')
Test-That 'a test that starts an app reaches what the app links (engine_cli starts engine-view)' { ($r.Modules -join ',') -eq 'engine_cli,engine_view,renderer' }
$r = Edges @('core/math/src/vec.cpp')
Test-That 'a dependency the closure left out still counts through the graph (gfx on math)' { $r.Modules -contains 'gfx' -and $r.Modules -contains 'physics' -and -not ($r.Modules -contains 'io') }
$r = Edges @('domain/physics/src/solver.cpp')
Test-That 'a leaf reaches what links it' { ($r.Modules -join ',') -eq 'cloth,physics' }

# Never too little: whatever a module's tests reach, a change there selects them.
foreach ($m in $edgeModules) {
  $r = Edges @("$($m.path)/src/x.cpp")
  $missing = @($edgeModules | Where-Object { ($_.test_reach.modules -contains $m.name) -and -not ($r.Modules -contains $_.name) })
  Test-That "a change in $($m.name) selects every module whose tests reach it" { $missing.Count -eq 0 }
}

$r = Edges @('tests/support/test_main.cpp')
Test-That 'a source the test targets compile from outside every module reaches the tests that compile it' { -not $r.All -and $r.Modules.Count -eq $edgeModules.Count }
$r = Edges @('apps/tests/host_tests.cpp')
Test-That 'a shared test file reaches only the tests built from it' { ($r.Modules -join ',') -eq 'engine_view' }
$r = Edges @('game/walk/src/level.cpp')
Test-That 'a game module''s directory reaches the tests that start it, though no module holds it' { ($r.Modules -join ',') -eq 'engine_cli,engine_view' -and @($r.Notes).Count -eq 0 }
$r = Edges @('tests/support/test_temp_dir.cpp')
Test-That 'a support source no test target lists still selects everything' { $r.All }
$r = Edges @('apps/engine_shell/index.html')
Test-That 'a directory under a layer that no module holds and no target reaches selects everything (a glob''s copy)' { $r.All }
$r = Get-AffectedTests -Modules $edgeModules -Layers $layers -Changed @('domain/ruins/src/blocks.cpp') -Disabled @('RUINS')
Test-That 'unless the preset switched off a capability of that name, which it says' {
  -not $r.All -and $r.Modules.Count -eq 0 -and @($r.Notes | Where-Object { $_ -match 'not in this preset' }).Count -eq 1
}

$r = Edges @('tests/support/tile_fixture.h') @((Ref 'includes' 'tests/support/tile_fixture.h' 'systems/renderer/tests/tile_tests.cpp'),
                                               (Ref 'includes' 'tests/support/tile_fixture.h' 'tests/support/other_fixture.h'))
Test-That 'a support header reaches the tests that include it, and only them' { ($r.Modules -join ',') -eq 'renderer' -and -not $r.All }
$r = Edges @('tests/support/tile_fixture.h') @((Ref 'includes' 'tests/support/tile_fixture.h' 'domain/gfx/include/domain/gfx/x.h'))
Test-That 'one a module''s own header includes is a change to the module' { ($r.Modules -join ',') -eq 'engine_cli,engine_view,gfx,renderer' }
$r = Edges @('tests/support/tile_fixture.h') @((Ref 'includes' 'tests/support/tile_fixture.h' 'shared/odd.cpp'))
Test-That 'one a file no module holds includes selects everything' { $r.All }
$r = Edges @('tests/support/tile_fixture.h')
Test-That 'one nothing includes selects no module' { -not $r.All -and $r.Modules.Count -eq 0 }

$scenes = @((Ref 'reads' 'content/test-scenes' 'systems/renderer/tests/scene_tests.cpp'),
            (Ref 'reads' 'content/flora' 'domain/physics/tests/tree_tests.cpp'),
            (Ref 'refers' 'content/flora' 'content/test-scenes/forest/scene.json'),
            (Ref 'reads' 'content/test-scenes' 'tools/frame-budget.ps1'))
$r = Edges @('content/test-scenes/forest/scene.json') $scenes
Test-That 'a scene reaches the tests whose files name its directory, and the tools that do' { ($r.Modules -join ',') -eq 'renderer' -and $r.Tools -and -not $r.All }
$r = Edges @('content/flora/oak.flora.json') $scenes
Test-That 'a file a scene refers to reaches the scene''s readers too' { ($r.Modules -join ',') -eq 'physics,renderer' }
$r = Edges @('content/flora/oak.flora.json') ($scenes + (Ref 'reads' 'content/*' 'domain/gfx/src/materials.cpp'))
Test-That 'a path built from pieces reads every directory, and its module is changed' { ($r.Modules -join ',') -eq 'engine_cli,engine_view,gfx,physics,renderer' }
$r = Edges @('content/flora/oak.flora.json') ($scenes + (Ref 'reads' 'content/flora' 'domain/gfx/tests/flora_reference.h'))
Test-That 'a test header that names it is a change to its module (another module''s tests may include it)' { $r.Modules -contains 'engine_view' -and $r.Modules -contains 'gfx' }
$r = Edges @('content/flora/oak.flora.json') ($scenes + (Ref 'reads' 'content/flora' 'scripts/loose.ps1'))
Test-That 'a reader no module holds selects everything' { $r.All }
$r = Edges @('content/README.txt')
Test-That 'a file at the top of content/ selects everything' { $r.All }
$r = Edges @('content/wetland/fen.json') $scenes
Test-That 'a content directory nothing names selects no module' { -not $r.All -and $r.Modules.Count -eq 0 }

$r = Edges @('tools/gpu-lock.ps1') @((Ref 'reads' 'tools' 'foundation/io/tests/lock_tests.cpp'))
Test-That 'a script reaches the tools tests and the modules whose tests run a script' { $r.Tools -and ($r.Modules -join ',') -eq 'io' }
$r = Edges @('templates/game-repo/CMakeLists.txt')
Test-That 'the game templates reach the tools tests and no module' { $r.Tools -and $r.Modules.Count -eq 0 -and -not $r.All }

$r = Edges @('domain/gfx/shaders/brdf.slang') @((Ref 'slang' 'domain/gfx/shaders/brdf.slang' 'systems/cloth/shaders/cloth.slang'))
Test-That 'a shader another module''s shader includes is a change to that module too' { $r.Modules -contains 'cloth' -and $r.Modules -contains 'renderer' }

$r = Edges @('cmake/EngineCloth.cmake') @((Ref 'cmake' 'cmake/EngineCloth.cmake' 'systems/cloth/CMakeLists.txt'))
Test-That 'a helper of definitions alone reaches the modules whose CMakeLists call it' { ($r.Modules -join ',') -eq 'cloth,cloth_schemas' -and -not $r.All }
$r = Edges @('cmake/EngineCloth.cmake') @((Ref 'cmake' 'cmake/EngineCloth.cmake' 'cmake/EngineModule.cmake'))
Test-That 'one another helper calls selects everything' { $r.All }
$r = Edges @('cmake/EngineCloth.cmake') @((Ref 'cmake' 'cmake/EngineCloth.cmake' '*' 'does more than define functions (set)'))
Test-That 'one that does more than define selects everything, and says why' { $r.All -and $r.Reason -match 'does more than define' }
$r = Edges @('cmake/EngineCloth.cmake')
Test-That 'one nothing calls selects no module, and says so' { -not $r.All -and $r.Modules.Count -eq 0 -and @($r.Notes | Where-Object { $_ -match 'no CMakeLists calls' }).Count -eq 1 }
# An empty mention list may arrive as $null (dev.ps1's `@(Get-AffectedMentions ...)` of nothing
# through a parameter); it must not become one null record whose `.kind` fails.
$nullMentions = $null
try {
  $r = Get-AffectedTests -Modules $edgeModules -Layers $layers -Changed @('cmake/EngineCloth.cmake') -Mentions $nullMentions
  $nullError = $null
} catch { $nullError = $_ }
Test-That 'a cmake/ helper with -Mentions $null selects without an error, as with none' {
  $null -eq $nullError -and -not $r.All -and $r.Modules.Count -eq 0
}
foreach ($path in 'CMakeLists.txt', 'CMakePresets.json', 'cmake/test-costs.txt', 'third_party/doctest/doctest.h', 'tools/schemac/main.cpp') {
  $r = Edges @($path)
  Test-That "$path still selects everything" { $r.All -and $r.Reason }
}

$r = Get-AffectedTests -Modules $edgeModules -Layers $layers -Changed @('domain/sky/src/earth.cpp') -Rule modules
Test-That 'the rule `modules` is the module graph''s, edges or not' { $r.Modules -contains 'cloth' -and @($r.Notes | Where-Object { $_ -match 'sky is a capability' }).Count -eq 1 }
$r = Get-AffectedTests -Modules $edgeModules -Layers $layers -Changed @('docs/x.md') -Rule all
Test-That 'the rule `all` selects everything' { $r.All -and $r.Reason -match 'all' }
$r = Affected @('domain/sky/src/earth.cpp')
Test-That 'a graph with no test edges falls back to the module graph''s rule, and says so' {
  $r.Modules -contains 'cloth' -and @($r.Notes | Where-Object { $_ -match 'no test edges' }).Count -eq 1
}

Write-Host 'reading a CMake helper'
$d = Get-CMakeDefinitions @('# a helper', 'include_guard(GLOBAL)', 'function(engine_cloth name)', '  string(REGEX MATCH "#[^)]*(" x "${name}")',
                            '  if(x)', '    message(STATUS "(")', '  endif()', 'endfunction()', 'macro(_engine_cloth_inner)', 'endmacro()')
Test-That 'definitions alone: their names, and nothing else run' { ($d.Names -join ',') -eq 'engine_cloth,_engine_cloth_inner' -and $null -eq $d.Other }
$d = Get-CMakeDefinitions @('set(ENGINE_CLOTH ON)', 'function(engine_cloth)', 'endfunction()')
Test-That 'a command at the top is named' { $d.Other -eq 'set' }
$d = Get-CMakeDefinitions @('function(engine_cloth)', '  if(x', 'endfunction()')
Test-That 'a block left open is not narrowed' { $d.Other }
$d = Get-CMakeDefinitions @('function(engine_cloth)', 'endfunction()', 'engine_cloth(', '  a)')
Test-That 'a call at the top over two lines is a command' { $d.Other -eq 'engine_cloth' }

Write-Host 'finding the references: Get-AffectedMentions over a small repository'
# The one part that reads a tree: git grep over a repository written here, a unique directory under
# the system temp directory, removed at the end.
$repo = Join-Path ([IO.Path]::GetTempPath()) "engine-affected-$([guid]::NewGuid().ToString('N'))"
function Set-RepoFile([string]$rel, [string]$text) {
  $full = Join-Path $repo $rel
  New-Item -ItemType Directory -Force -Path (Split-Path $full) | Out-Null
  [IO.File]::WriteAllText($full, $text)
}
try {
  New-Item -ItemType Directory -Path $repo | Out-Null
  Set-RepoFile 'content/test-scenes/forest/scene.json' '{"tree": "../../flora/oak.flora.json"}'
  Set-RepoFile 'content/flora/oak.flora.json' '{}'
  Set-RepoFile 'content/flora/README.md' 'see content/test-scenes/forest'
  Set-RepoFile 'content/budgets/budgets.json' '{"scene": "content/test-scenes/forest/scene.json"}'
  Set-RepoFile 'systems/renderer/tests/scene_tests.cpp' "auto p = ENGINE_SOURCE_DIR `"/content/test-scenes/forest`";`n"
  Set-RepoFile 'domain/gfx/src/comment.cpp' "// reads content/flora one day`nint x;`n"
  Set-RepoFile 'domain/gfx/src/pieces.cpp' "auto p = join(root, `"content`");`nENGINE_LOG_CATEGORY_DEFINE(log_c, `"content`");`n"
  Set-RepoFile 'domain/gfx/src/log.cpp' "ENGINE_LOG_CATEGORY_DEFINE(log_c, `"content`");`n"
  Set-RepoFile 'foundation/io/tests/lock_tests.cpp' "auto s = ENGINE_SOURCE_DIR `"/tools/gpu-lock.ps1`";`n"
  Set-RepoFile 'tools/gpu-lock.ps1' "# tools/gpu-lock.ps1`n"
  Set-RepoFile 'tests/support/tile_fixture.h' "#pragma once`n"
  Set-RepoFile 'tests/support/wrapper.h' "#include <tile_fixture.h>`n"
  Set-RepoFile 'systems/renderer/tests/tile_tests.cpp' "#include <wrapper.h>`n"
  Set-RepoFile 'domain/gfx/shaders/brdf.slang' "float f();`n"
  Set-RepoFile 'domain/gfx/shaders/material.slang' "#include `"brdf.slang`"`n"
  Set-RepoFile 'systems/cloth/shaders/cloth.slang' "#include `"../../../domain/gfx/shaders/material.slang`"`n"
  Set-RepoFile 'cmake/EngineCloth.cmake' "# a helper`nfunction(engine_cloth_setup name)`n  message(STATUS `"#(`")`nendfunction()`n"
  Set-RepoFile 'cmake/EngineGlobal.cmake' "set(ENGINE_GLOBAL ON)`nfunction(engine_global)`nendfunction()`n"
  Set-RepoFile 'CMakeLists.txt' "include(EngineCloth)`ninclude(EngineGlobal)`n"
  Set-RepoFile 'systems/cloth/CMakeLists.txt' "engine_cloth_setup(cloth) # EngineCloth`n"
  & git -C $repo init -q 2>$null
  & git -C $repo add -A 2>$null

  $changed = @('content/flora/oak.flora.json', 'tools/gpu-lock.ps1', 'tests/support/tile_fixture.h',
               'domain/gfx/shaders/brdf.slang', 'cmake/EngineCloth.cmake', 'cmake/EngineGlobal.cmake')
  $found = @(Get-AffectedMentions -Root $repo -Changed $changed)
  function Has([string]$kind, [string]$target, [string]$file) { @($found | Where-Object { $_.kind -eq $kind -and $_.target -eq $target -and $_.file -eq $file }).Count -gt 0 }
  Test-That 'a scene that refers to a changed file by ../.. is found, and its directory with it' { Has 'refers' 'content/flora' 'content/test-scenes/forest/scene.json' }
  Test-That 'and the directories that refer to that one, but not a page that names it' {
    (Has 'refers' 'content/test-scenes' 'content/budgets/budgets.json') -and -not (Has 'refers' 'content/test-scenes' 'content/flora/README.md')
  }
  Test-That 'a test that names the scene''s directory reads it' { Has 'reads' 'content/test-scenes' 'systems/renderer/tests/scene_tests.cpp' }
  Test-That 'a comment that names a directory reads nothing' { -not (Has 'reads' 'content/flora' 'domain/gfx/src/comment.cpp') }
  Test-That 'a path built from "content" reads every directory, a log category called so does not' {
    (Has 'reads' 'content/*' 'domain/gfx/src/pieces.cpp') -and -not (Has 'reads' 'content/*' 'domain/gfx/src/log.cpp')
  }
  Test-That 'a test that runs a script from the tree reads the tools' { Has 'reads' 'tools' 'foundation/io/tests/lock_tests.cpp' }
  Test-That 'a support header is found through the support header that includes it' {
    (Has 'includes' 'tests/support/tile_fixture.h' 'tests/support/wrapper.h') -and (Has 'includes' 'tests/support/tile_fixture.h' 'systems/renderer/tests/tile_tests.cpp')
  }
  Test-That 'a shader is found through the shaders that include it, across directories' {
    (Has 'slang' 'domain/gfx/shaders/brdf.slang' 'domain/gfx/shaders/material.slang') -and (Has 'slang' 'domain/gfx/shaders/brdf.slang' 'systems/cloth/shaders/cloth.slang')
  }
  Test-That 'a helper of definitions is called from a module''s CMakeLists, and its include() is no call' {
    (Has 'cmake' 'cmake/EngineCloth.cmake' 'systems/cloth/CMakeLists.txt') -and -not (Has 'cmake' 'cmake/EngineCloth.cmake' 'CMakeLists.txt') -and
    -not (Has 'cmake' 'cmake/EngineCloth.cmake' '*')
  }
  Test-That 'a helper that sets a variable at its top is everything''s' { Has 'cmake' 'cmake/EngineGlobal.cmake' '*' }
  & git -C $repo -c user.email=t@example.invalid -c user.name=t commit -q -m fixture 2>$null
  $rev = (& git -C $repo rev-parse HEAD).Trim()
  Set-RepoFile 'systems/renderer/tests/scene_tests.cpp' "int nothing;`n"
  $atRev = @(Get-AffectedMentions -Root $repo -Changed @('content/flora/oak.flora.json') -Rev $rev)
  Test-That 'with -Rev it reads the commit, not the working tree' {
    @($atRev | Where-Object { $_.kind -eq 'reads' -and $_.file -eq 'systems/renderer/tests/scene_tests.cpp' }).Count -eq 1
  }
}
finally {
  if (Test-Path -LiteralPath $repo) { Remove-Item -LiteralPath $repo -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed" -ForegroundColor Red
  exit 1
}
Write-Host "all $checks checks passed"
exit 0

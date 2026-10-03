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
Test-That 'an unrelated module does not' { -not (Runs $r 'gfx') -and -not (Runs $r 'renderer') -and -not (Runs $r 'engine_view') }
Test-That 'a name that only contains the module name does not match' { -not (Runs $r 'physics_extra') -and -not (Runs $r 'my.physics') }
Test-That 'the lint and the documentation check always run' { (Runs $r 'lint.banned_patterns') -and (Runs $r 'docs_check') }
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
  Test-That "$path selects only the checks that always run" { -not $r.All -and $r.Modules.Count -eq 0 -and (Runs $r 'docs_check') -and -not (Runs $r 'gfx') }
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

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed" -ForegroundColor Red
  exit 1
}
Write-Host "all $checks checks passed"
exit 0

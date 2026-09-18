#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Tests for tools/lint.ps1. Runs under CTest as `tools.lint`.

.DESCRIPTION
  Builds a small source tree in a temporary directory, checks that it passes, and then breaks it
  one way at a time: a banned container, an exception, the rule ADR-0028 seam 5 added — an
  `#include <flecs.h>` outside `domain/ecs`, `systems/` and `game/` — and the test-hygiene rule,
  a `temp_directory_path()` in a tests/ or bench/ directory. Each case asserts the message *and*
  that the run failed, because a check that reports a problem and exits 0 is worse than no check
  at all. The scoped rules get the positive cases too: the flecs include is fine inside its
  allowed roots, including in their tests and benches, where the older rules deliberately do not
  apply; and the temp-path rule is the mirror image, applying only inside tests/ and bench/ and
  never to `tests/support`, which is where `engine::test::TempDir` computes the one root.

  Every case gets its own copy of the fixture, so a case cannot pass because of what another one
  left behind, and every fixture root is a fresh GUID under the system temp directory, so two
  copies of this script — another worktree, a second agent, CI — never share one.

      pwsh tools/lint.Tests.ps1 [-KeepTemp]
#>
[CmdletBinding()]
param([switch]$KeepTemp)

$ErrorActionPreference = 'Stop'

$lint = Join-Path $PSScriptRoot 'lint.ps1'
$failures = New-Object System.Collections.Generic.List[string]
$checks = 0
$roots = New-Object System.Collections.Generic.List[string]

function Invoke-Lint([string]$root) {
  # The report is written with Write-Host, which is the information stream, not output: *>&1
  # brings every stream back so the tests can read what a contributor would see.
  $out = (& $lint -Root $root *>&1 | Out-String)
  return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}

function Set-FixtureFile([string]$root, [string]$rel, [string]$text) {
  $path = Join-Path $root $rel
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
  $text = $text -replace "`r`n", "`n"
  if (-not $text.EndsWith("`n")) { $text += "`n" }
  [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false))
}

# A tree that passes: a core module, an ECS module that may include flecs, a system that bridges
# to it, a physics module that must not, the shared tests/support directory where
# `temp_directory_path` is the one legal spelling, and a tool whose own tests are held to the
# test-hygiene rule although tools/ is exempt from everything else.
function New-Fixture {
  $root = Join-Path ([IO.Path]::GetTempPath()) "engine-lint-$([guid]::NewGuid().ToString('N'))"
  $roots.Add($root)

  Set-FixtureFile $root 'core/base/include/core/base/types.h' "#pragma once`nnamespace engine { using u32 = unsigned; }`n"
  Set-FixtureFile $root 'domain/ecs/include/domain/ecs/sim_world.h' "#pragma once`n#include <flecs.h>`nnamespace engine::ecs { struct SimWorld {}; }`n"
  Set-FixtureFile $root 'domain/ecs/tests/ecs_tests.cpp' "#include <flecs.h>`nint main() { return 0; }`n"
  Set-FixtureFile $root 'domain/ecs/bench/ecs_bench.cpp' "#include <flecs.h>`nint main() { return 0; }`n"
  Set-FixtureFile $root 'domain/physics/src/solver.cpp' "#include <core/base/types.h>`nnamespace engine::physics { void step() {} }`n"
  Set-FixtureFile $root 'domain/physics/tests/physics_tests.cpp' "#include <map>`nint main() { return 0; }`n"
  Set-FixtureFile $root 'systems/simulation/src/bridge.cpp' "#include <flecs.h>`nnamespace engine::simulation { void tick() {} }`n"
  Set-FixtureFile $root 'game/desert/src/main.cpp' "#include `"flecs.h`"`nint main() { return 0; }`n"
  # The one file allowed to name the system temp directory, and a module whose *source* names it
  # legitimately: the rule is about tests, not about engine code that manages temporary files.
  Set-FixtureFile $root 'tests/support/test_temp_dir.h' "#pragma once`n#include <filesystem>`nnamespace engine::test { inline auto root() { return std::filesystem::temp_directory_path(); } }`n"
  Set-FixtureFile $root 'foundation/io/src/vfs.cpp' "#include <filesystem>`nnamespace engine::io { auto scratch() { return std::filesystem::temp_directory_path(); } }`n"
  Set-FixtureFile $root 'tools/schemac/tests/schemac_tests.cpp' "#include <test_temp_dir.h>`nint main() { return 0; }`n"
  return $root
}

function Test-That([string]$what, [scriptblock]$condition) {
  $script:checks++
  $ok = $false
  try { $ok = [bool](& $condition) } catch { $ok = $false }
  if ($ok) {
    Write-Host "  ok   $what"
  } else {
    Write-Host "  FAIL $what" -ForegroundColor Red
    $failures.Add($what)
  }
}

# The lint failed, and it said this about it. The expected text is a literal, because the message
# is the product: whoever reads it has to know which file to open and which rule they hit.
function Test-Reports([string]$what, [string]$root, [string]$expected) {
  $script:checks++
  $result = Invoke-Lint $root
  if ($result.Code -eq 1 -and $result.Out.Contains($expected)) {
    Write-Host "  ok   $what"
  } else {
    Write-Host "  FAIL $what (exit $($result.Code), expected to see: $expected)" -ForegroundColor Red
    Write-Host ($result.Out -split "`n" | ForEach-Object { "       $_" }) -Separator "`n"
    $failures.Add($what)
  }
}

try {
  Write-Host 'lint tests'

  $clean = New-Fixture
  $result = Invoke-Lint $clean
  Test-That 'a clean tree passes' { $result.Code -eq 0 }
  Test-That 'flecs is allowed in domain/ecs, its tests and its bench' {
    ($result.Code -eq 0) -and -not $result.Out.Contains('domain/ecs')
  }
  Test-That 'flecs is allowed in systems/ and game/' {
    ($result.Code -eq 0) -and -not $result.Out.Contains('systems/simulation')
  }
  Test-That 'a banned container in a tests/ directory is still allowed' {
    ($result.Code -eq 0) -and -not $result.Out.Contains('physics_tests.cpp')
  }
  Test-That 'tests/support may name the system temp directory' {
    ($result.Code -eq 0) -and -not $result.Out.Contains('test_temp_dir.h')
  }
  Test-That 'engine source outside tests/ may name it too' {
    ($result.Code -eq 0) -and -not $result.Out.Contains('foundation/io/src/vfs.cpp')
  }

  # AGENTS.md "Test hygiene": a fixed path under the system temp directory is a shared mutable
  # global with the machine for a scope, and two copies of one test binary delete each other's
  # fixtures. This is the rule that stops it coming back, in every module and in tools/.
  $root = New-Fixture
  Set-FixtureFile $root 'domain/physics/tests/physics_tests.cpp' "#include <filesystem>`nauto p = std::filesystem::temp_directory_path() / `"engine_physics_tests`";`n"
  Test-Reports 'a fixed temp path in a test is reported' $root 'domain/physics/tests/physics_tests.cpp:2: [test-temp-path]'
  Test-Reports 'and the message names TempDir' $root 'engine::test::TempDir'

  $root = New-Fixture
  Set-FixtureFile $root 'foundation/store/bench/store_bench.cpp' "#include <filesystem>`nauto p = std::filesystem::temp_directory_path();`n"
  Test-Reports 'a bench directory is held to it as well' $root 'foundation/store/bench/store_bench.cpp:2: [test-temp-path]'

  # tools/ is exempt from the container and exception rules, and is *not* exempt from this one:
  # a test under tools/ runs on the same machine as every other test.
  $root = New-Fixture
  Set-FixtureFile $root 'tools/schemac/tests/schemac_tests.cpp' "#include <cstdlib>`nconst char* t = getenv(`"TEMP`");`n"
  Test-Reports "a tool's own test is held to it too" $root 'tools/schemac/tests/schemac_tests.cpp:2: [test-temp-path]'

  $root = New-Fixture
  Set-FixtureFile $root 'domain/nav/tests/nav_tests.cpp' "#include <cstdio>`nchar* p = tmpnam(nullptr);  // engine-lint: allow-temp-path deliberate`n"
  Test-That 'a line-level marker opts out of the temp-path rule' { (Invoke-Lint $root).Code -eq 0 }

  # A `Pending` path is a deadline, not an exemption: the violation is printed on every run and
  # the run still passes, so a conversion in flight does not turn the gate red and cannot be
  # forgotten either. Both halves are asserted, because silence would be the failure here.
  $root = New-Fixture
  Set-FixtureFile $root 'apps/engine_view/tests/mesh_view_tests.cpp' "#include <filesystem>`nauto t = std::filesystem::temp_directory_path();`n"
  $pendingResult = Invoke-Lint $root
  Test-That 'a Pending path does not fail the run' { $pendingResult.Code -eq 0 }
  Test-That 'but is reported on every run' {
    $pendingResult.Out.Contains('apps/engine_view/tests/mesh_view_tests.cpp:2: [test-temp-path]') -and
    $pendingResult.Out.Contains('Pending list')
  }

  $root = New-Fixture
  Set-FixtureFile $root 'domain/physics/src/solver.cpp' "#include <map>`nnamespace engine::physics { std::map<int, int> m; }`n"
  Test-Reports 'a banned container is reported with its file and line' $root 'domain/physics/src/solver.cpp:2: [std-container]'

  $root = New-Fixture
  Set-FixtureFile $root 'domain/physics/src/solver.cpp' "namespace engine::physics {`nvoid f() {`n  throw 1;`n}`n}`n"
  Test-Reports 'an exception is reported' $root 'domain/physics/src/solver.cpp:3: [exceptions]'

  # ADR-0028 seam 5, which is the rule this file was added for.
  $root = New-Fixture
  Set-FixtureFile $root 'domain/physics/src/solver.cpp' "#include <flecs.h>`nnamespace engine::physics { void step() {} }`n"
  Test-Reports 'flecs in domain/physics is reported' $root 'domain/physics/src/solver.cpp:1: [flecs-include]'
  Test-Reports 'and the message says where flecs belongs' $root 'flecs.h belongs to domain/ecs, systems/ and game/'

  $root = New-Fixture
  Set-FixtureFile $root 'foundation/store/src/database.cpp' "#include `"flecs.h`"`nnamespace engine::store {}`n"
  Test-Reports 'a quoted include is caught too' $root 'foundation/store/src/database.cpp:1: [flecs-include]'

  # The confinement rule applies to tests and benches as well, unlike the container and exception
  # rules: an ECS-free module whose *test* reaches flecs has an ECS dependency in its build.
  $root = New-Fixture
  Set-FixtureFile $root 'domain/nav/tests/nav_tests.cpp' "#include <flecs.h>`nint main() { return 0; }`n"
  Test-Reports "a module's test may not reach flecs either" $root 'domain/nav/tests/nav_tests.cpp:1: [flecs-include]'

  $root = New-Fixture
  Set-FixtureFile $root 'core/schema/src/type_info.cpp' "// engine-lint: allow-flecs deliberate, see ADR-0028`n#include <flecs.h>`n"
  Test-That 'a file-level marker opts out' { (Invoke-Lint $root).Code -eq 0 }

  $root = New-Fixture
  Set-FixtureFile $root 'core/schema/src/type_info.cpp' "#include <flecs.h>  // engine-lint: allow-flecs deliberate`n"
  Test-That 'a line-level marker opts out' { (Invoke-Lint $root).Code -eq 0 }
} finally {
  foreach ($r in $roots) {
    if ($KeepTemp) {
      Write-Host "kept $r"
    } else {
      Remove-Item -Recurse -Force -LiteralPath $r -ErrorAction SilentlyContinue
    }
  }
}

Write-Host ''
if ($failures.Count -gt 0) {
  Write-Host "$($failures.Count) of $checks checks failed:" -ForegroundColor Red
  foreach ($f in $failures) { Write-Host "  - $f" }
  exit 1
}
Write-Host "lint: $checks checks passed" -ForegroundColor Green
exit 0

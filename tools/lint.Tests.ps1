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
  never to `tests/support`, which is where `engine::test::TempDir` computes the one root. The two
  Vulkan rules (docs/subsystems/gfx.md, "The RHI surface and the backend surface") get both kinds:
  a `Vk` type, a `VK_` constant and a Vulkan include in a public header are each reported, a
  comment naming an extension is not, the two backend directories may say what they like, the
  `allow-vulkan` marker opts a line or a file out, and the backend set included from the renderer,
  its tests or engine-host is reported while gfx, the window system and engine-view may include it.
  `std::function` in engine code is reported, in a comment or a test it is not, and the marker opts
  a line out. The size-table rule is held over the fixture's modules: a module without a table or an
  exemption, a table with no entry, a table nothing compiles, and an exemption list line with no
  reason, naming no module, or naming a module that has a table now are each reported.

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
$script:TrackFixtures = $false

function Invoke-Lint([string]$root, [switch]$Untracked) {
  # The machine-path rule reads the files git tracks, so its cases ($script:TrackFixtures) make a
  # fixture a repository with every file in its index first (re-added on each call, since cases add
  # files between calls); -Untracked leaves it a plain directory. The other rules' cases skip the
  # step, which costs a git process or two a case.
  if ($script:TrackFixtures -and -not $Untracked) {
    if (-not (Test-Path -LiteralPath (Join-Path $root '.git'))) { & git -C $root init -q 2>&1 | Out-Null }
    & git -C $root add -A 2>&1 | Out-Null
  }
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
  # The Vulkan backend set and who may see it (docs/subsystems/gfx.md, "The RHI surface and the
  # backend surface"): the two named backend directories say Vulkan freely, gfx's sources include
  # them, engine-view includes them because it presents, and a public header that only *mentions*
  # an extension in a comment is not a leak.
  Set-FixtureFile $root 'domain/gfx/include/domain/gfx/backend/vulkan/vulkan.h' "#pragma once`n#include <volk.h>`nnamespace engine::gfx::vk { inline VkBuffer native(); }`n"
  Set-FixtureFile $root 'foundation/window/include/foundation/window/backend/vulkan/surface.h' "#pragma once`ntypedef struct VkInstance_T* VkInstance;`n"
  Set-FixtureFile $root 'domain/gfx/src/device.cpp' "#include <domain/gfx/backend/vulkan/vulkan.h>`n#include <vk_mem_alloc.h>`nnamespace engine::gfx { VkDevice d; }`n"
  Set-FixtureFile $root 'domain/gfx/tests/device_tests.cpp' "#include <domain/gfx/backend/vulkan/vulkan.h>`nint main() { return VK_SUCCESS; }`n"
  Set-FixtureFile $root 'domain/gfx/include/domain/gfx/budget.h' "#pragma once`n// Reports VK_EXT_memory_budget when the device has it; VkPhysicalDevice is behind Handles.`nnamespace engine::gfx { struct MemoryBudget { bool valid = false; }; }  // VK_EXT_memory_budget`n"
  Set-FixtureFile $root 'apps/engine_view/main.cpp' "#include <domain/gfx/backend/vulkan/swapchain.h>`n#include <foundation/window/backend/vulkan/surface.h>`nint main() { VkSurfaceKHR s = VK_NULL_HANDLE; return 0; }`n"
  # std::function (AGENTS.md "Rules that are checked"): a header that says in a comment why it takes
  # a function pointer instead is not a use, and a test may use one.
  Set-FixtureFile $root 'core/jobs/include/core/jobs/job.h' "#pragma once`n// A function pointer and a context: no std::function, no allocation.`nnamespace engine::jobs { struct Job { void (*fn)(void*); void* ctx; }; }`n"
  Set-FixtureFile $root 'domain/physics/tests/callback_tests.cpp' "#include <functional>`nstd::function<bool()> condition;`n"
  # The size-table rule: a module with a compiled table, a module on the exemption list, and two
  # CMakeLists.txt that declare no module (an app, and one whose engine_module is a comment).
  Set-FixtureFile $root 'domain/physics/CMakeLists.txt' "engine_module(NAME physics LAYER domain DEPS base`n  SOURCES src/solver.cpp)`nengine_module_tests(NAME physics SOURCES tests/physics_tests.cpp tests/size_table.cpp)`n"
  Set-FixtureFile $root 'domain/physics/tests/size_table.cpp' "#include <core/base/size_table.h>`nENGINE_EXPECT_SIZE(4, 4, int);`n"
  Set-FixtureFile $root 'domain/nav/CMakeLists.txt' "engine_module(NAME nav LAYER domain DEPS base)`nengine_module_tests(NAME nav SOURCES tests/nav_tests.cpp)`n"
  Set-FixtureFile $root 'tools/size-table-exemptions.txt' "# a comment`n`ndomain/nav: nothing per element, mirrored or on disk; closed by its first such type`n"
  Set-FixtureFile $root 'apps/engine_view/CMakeLists.txt' "# Not a module: an app, which this comment's engine_module(...) does not change.`nengine_app(NAME engine_view OUTPUT engine-view SOURCES main.cpp)`n"
  # The shell (shell-html-sink): a page whose one script is a file, a comment that names a handler,
  # and a module that builds nodes and sets text, imports a literal and passes a timer a function.
  Set-FixtureFile $root 'apps/engine_shell/index.html' "<!doctype html>`n<!--`n  Never <img onerror=x> or style=`"`" here.`n-->`n<link rel=`"stylesheet`" href=`"/shell/shell.css`">`n<script type=`"module`" src=`"/shell/main.js`"></script>`n<div class=`"app`" id=`"app`"></div>`n"
  Set-FixtureFile $root 'apps/engine_shell/main.js' "// Text, never innerHTML or eval(s).`n/* insertAdjacentHTML is not used:`n   document.write(s) neither. */`nexport function show(el, s) { el.textContent = s; el.append(document.createTextNode(s)); }`nsetTimeout(() => show(document.body, `"x`"), 10);`nimport(`"./testhook.js`").then((m) => m.install());`n"
  # The editor's person flag (person-flag): a script, a batch file, a workflow and a CMake file that
  # start engine-editor with no seat, each naming the flag only in a comment of its own kind, a
  # --person given to another program, and engine-content's own --persons beside the editor.
  Set-FixtureFile $root 'tools/run-editor.ps1' @'
<#
  Never --person here: a seat is a person's.
#>
$editor = Join-Path $bin 'engine-editor.exe'
& $editor --doc $doc --roles roles.json  # not --person
& engine-content dwelling $out --persons 4
'@
  Set-FixtureFile $root 'tools/bundle/run-editor.cmd' "@echo off`nrem engine-editor --person ada is a person's to type`n:: --person-role reviewer neither`nengine-editor.exe --doc world`n"
  Set-FixtureFile $root '.github/workflows/editor.yml' "jobs:`n  editor:`n    steps:`n      # never with --person`n      - run: build/msvc-debug/bin/engine-editor --doc world --no-shell`n"
  Set-FixtureFile $root 'apps/engine_editor/CMakeLists.txt' "#[[ A seat (--person) is a person's.`n]]`nadd_test(NAME editor.doc COMMAND `$<TARGET_FILE:engine_engine_editor> --doc world)  # no --person`n"
  Set-FixtureFile $root 'tools/greet.ps1' "& greeter --person ada`n"
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

  # The `Pending` list is empty (2026-09-18): the one path that was on it, engine-view's tests, is
  # held to the rule like every other test directory. The mechanism stays in lint.ps1 for the next
  # conversion in flight; with nothing listed there is nothing of it to assert here.
  $root = New-Fixture
  Set-FixtureFile $root 'apps/engine_view/tests/mesh_view_tests.cpp' "#include <filesystem>`nauto t = std::filesystem::temp_directory_path();`n"
  Test-Reports 'a path that used to be pending is held to the rule again' $root 'apps/engine_view/tests/mesh_view_tests.cpp:2: [test-temp-path]'

  $root = New-Fixture
  Set-FixtureFile $root 'domain/physics/src/solver.cpp' "#include <map>`nnamespace engine::physics { std::map<int, int> m; }`n"
  Test-Reports 'a banned container is reported with its file and line' $root 'domain/physics/src/solver.cpp:2: [std-container]'

  # std::function, which AGENTS.md lists among the banned types and nothing checked until T80.
  Test-That 'a comment naming std::function is not a use, and a test may use one' {
    ($result.Code -eq 0) -and -not $result.Out.Contains('std-function')
  }

  $root = New-Fixture
  Set-FixtureFile $root 'domain/physics/src/solver.cpp' "#include <functional>`nnamespace engine::physics { void step(std::function<void(int)> f) { f(1); } }`n"
  Test-Reports 'std::function in engine code is reported with its file and line' $root 'domain/physics/src/solver.cpp:2: [std-function]'
  Test-Reports 'and the message names the marker' $root 'engine-lint: allow-std-function <reason>'

  $root = New-Fixture
  Set-FixtureFile $root 'domain/physics/src/solver.cpp' "#include <functional>`nnamespace engine::physics { std::move_only_function<void()> pending; }`n"
  Test-Reports 'std::move_only_function is reported too' $root 'domain/physics/src/solver.cpp:2: [std-function]'

  $root = New-Fixture
  Set-FixtureFile $root 'domain/physics/src/solver.cpp' "#include <functional>`nstd::function<void()> g_on_load;  // engine-lint: allow-std-function cold initialisation, once at start-up`n"
  Test-That 'a line-level allow-std-function marker opts out' { (Invoke-Lint $root).Code -eq 0 }

  # The size-table rule (AGENTS.md "Size table"), over the fixture's two modules and its list.
  Test-That 'a module with a compiled table and a module on the exemption list pass, and an app is not a module' {
    ($result.Code -eq 0) -and -not $result.Out.Contains('size-table')
  }

  $root = New-Fixture
  Remove-Item -LiteralPath (Join-Path $root 'domain/physics/tests/size_table.cpp')
  Test-Reports 'a module with no table and no exemption is reported' $root 'domain/physics/CMakeLists.txt:1: [size-table] a module has tests/size_table.cpp'
  Test-Reports 'and the message names the exemption list' $root 'tools/size-table-exemptions.txt'

  $root = New-Fixture
  Set-FixtureFile $root 'domain/physics/tests/size_table.cpp' "#include <core/base/size_table.h>`n// ENGINE_EXPECT_SIZE(4, 4, int);`n"
  Test-Reports 'a table with no entry is reported' $root 'domain/physics/tests/size_table.cpp:1: [size-table] a size table holds at least one'

  $root = New-Fixture
  Set-FixtureFile $root 'domain/physics/CMakeLists.txt' "engine_module(NAME physics LAYER domain DEPS base)`nengine_module_tests(NAME physics SOURCES tests/physics_tests.cpp)`n"
  Test-Reports 'a table the tests do not compile is reported' $root 'domain/physics/CMakeLists.txt:1: [size-table] tests/size_table.cpp is compiled'

  $root = New-Fixture
  Set-FixtureFile $root 'tools/size-table-exemptions.txt' "# a comment`n`ndomain/nav:`n"
  Test-Reports 'an exemption with no reason is reported' $root 'tools/size-table-exemptions.txt:3: [size-table] an exemption is'

  $root = New-Fixture
  Set-FixtureFile $root 'tools/size-table-exemptions.txt' "domain/nav: a reason`ndomain/gone: it was removed`n"
  Test-Reports 'an exemption naming no module is reported' $root 'tools/size-table-exemptions.txt:2: [size-table] domain/gone declares no module'

  $root = New-Fixture
  Set-FixtureFile $root 'domain/nav/CMakeLists.txt' "engine_module(NAME nav LAYER domain DEPS base)`nengine_module_tests(NAME nav SOURCES tests/nav_tests.cpp tests/size_table.cpp)`n"
  Set-FixtureFile $root 'domain/nav/tests/size_table.cpp' "#include <core/base/size_table.h>`nENGINE_EXPECT_SIZE(8, 8, double);`n"
  Test-Reports 'an exemption for a module that has a table now is reported' $root 'tools/size-table-exemptions.txt:3: [size-table] domain/nav has a size table now'

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

  # The Vulkan leak (plan 10 §10.6, 2026-09-25). The clean fixture already holds the backend set
  # in both named directories, gfx's sources and tests including it, engine-view including it, and
  # a public header whose comments name an extension; each check below breaks one of those rules.
  Test-That 'the backend directories may say Vulkan, and a comment naming an extension is fine' {
    ($result.Code -eq 0) -and -not $result.Out.Contains('vulkan')
  }

  $root = New-Fixture
  Set-FixtureFile $root 'domain/gfx/include/domain/gfx/timeline.h' "#pragma once`nnamespace engine::gfx {`nstruct FrameContext { VkCommandBuffer begin_frame(); };`n}`n"
  Test-Reports 'a Vulkan type in a public header is reported' $root 'domain/gfx/include/domain/gfx/timeline.h:3: [vulkan-in-public-header]'
  Test-Reports 'and the message says where it belongs' $root 'domain/gfx/rhi.h'

  $root = New-Fixture
  Set-FixtureFile $root 'systems/renderer/include/systems/renderer/frame_target.h' "#pragma once`nnamespace engine::renderer { inline constexpr int k_format = VK_FORMAT_R8G8B8A8_UNORM; }`n"
  Test-Reports 'a Vulkan constant in a public header is reported' $root 'systems/renderer/include/systems/renderer/frame_target.h:2: [vulkan-in-public-header]'

  $root = New-Fixture
  Set-FixtureFile $root 'foundation/window/include/foundation/window/display.h' "#pragma once`n#include <vulkan/vulkan_core.h>`n"
  Test-Reports 'a Vulkan include in a public header is reported' $root 'foundation/window/include/foundation/window/display.h:2: [vulkan-in-public-header]'

  $root = New-Fixture
  Set-FixtureFile $root 'domain/gfx/include/domain/gfx/timeline.h' "#pragma once`nstruct Legacy { VkSemaphore s; };  // engine-lint: allow-vulkan the reason, stated`n"
  Test-That 'a line-level allow-vulkan marker opts a public header line out' { (Invoke-Lint $root).Code -eq 0 }

  $root = New-Fixture
  Set-FixtureFile $root 'domain/gfx/include/domain/gfx/timeline.h' "#pragma once`n// engine-lint: allow-vulkan the reason, stated`nstruct Legacy { VkSemaphore s; };`n"
  Test-That 'a file-level allow-vulkan marker opts a public header out' { (Invoke-Lint $root).Code -eq 0 }

  # The confinement half: the renderer records through the engine's vocabulary and must not reach
  # the backend, in its sources or in its tests.
  $root = New-Fixture
  Set-FixtureFile $root 'systems/renderer/src/scene_renderer.cpp' "#include <domain/gfx/backend/vulkan/vulkan.h>`nnamespace engine::renderer {}`n"
  Test-Reports 'the renderer including the backend set is reported' $root 'systems/renderer/src/scene_renderer.cpp:1: [vulkan-backend-include]'
  Test-Reports 'and the message names who may' $root 'domain/gfx, foundation/window and the two apps that present, apps/engine_view and apps/engine_editor'

  $root = New-Fixture
  Set-FixtureFile $root 'systems/renderer/tests/renderer_tests.cpp' "#include <volk.h>`nint main() { return 0; }`n"
  Test-Reports "the renderer's tests may not reach volk either" $root 'systems/renderer/tests/renderer_tests.cpp:1: [vulkan-backend-include]'

  $root = New-Fixture
  Set-FixtureFile $root 'apps/engine_host/render_methods.cpp' "#include `"foundation/window/backend/vulkan/surface.h`"`n"
  Test-Reports 'a quoted include of the window backend outside a presenting app is reported' $root 'apps/engine_host/render_methods.cpp:1: [vulkan-backend-include]'

  # config-invariant-conditional (docs/ci/what-to-run.md, "The off-configurations' GPU cases"): a
  # module registered CONFIG_INVARIANT has its GPU cases run in the all-on suite only, so a
  # preprocessor conditional on a capability switch in any of its files is reported; the same
  # conditional in a module that is not, a mention in a string, and a CONFIG_INVARIANT that is only
  # a comment are not. Each fixture module carries a compiled size table: the size-table rule holds
  # every module of a fixture, and a "passes" case here is about this rule alone (the two rules met
  # for the first time on batch 58's stack, where three of these cases failed on the other one).
  $invariantCmake = "engine_module(NAME renderer LAYER systems)`nengine_module_tests(NAME renderer SOURCES tests/r.cpp tests/size_table.cpp PARTS 4 CONFIG_INVARIANT)`n"
  $invariantTable = "#include <core/base/size_table.h>`nENGINE_EXPECT_SIZE(4, 4, int);`n"
  $root = New-Fixture
  Set-FixtureFile $root 'systems/renderer/CMakeLists.txt' $invariantCmake
  Set-FixtureFile $root 'systems/renderer/tests/size_table.cpp' $invariantTable
  Set-FixtureFile $root 'systems/renderer/src/scene_water.cpp' "namespace engine::renderer {`nconst char* why() { return `"(ENGINE_WITH_WATER is off)`"; }`n}`n"
  Test-That 'a CONFIG_INVARIANT module that names a switch only in a string passes' { (Invoke-Lint $root).Code -eq 0 }

  $root = New-Fixture
  Set-FixtureFile $root 'systems/renderer/CMakeLists.txt' $invariantCmake
  Set-FixtureFile $root 'systems/renderer/tests/size_table.cpp' $invariantTable
  Set-FixtureFile $root 'systems/renderer/src/scene_sky.cpp' "namespace engine::renderer {`n#if ENGINE_WITH_SKY`nvoid sky() {}`n#endif`n}`n"
  Test-Reports 'a capability conditional in a CONFIG_INVARIANT module is reported' $root 'systems/renderer/src/scene_sky.cpp:2: [config-invariant-conditional]'
  Test-Reports 'and the message names the way out' $root 'Drop CONFIG_INVARIANT from its engine_module_tests()'

  $root = New-Fixture
  Set-FixtureFile $root 'systems/renderer/CMakeLists.txt' $invariantCmake
  Set-FixtureFile $root 'systems/renderer/tests/size_table.cpp' $invariantTable
  Set-FixtureFile $root 'systems/renderer/tests/minimal_tests.cpp' "#ifdef ENGINE_MINIMAL`nint minimal_only = 1;`n#endif`n"
  Test-Reports "its tests are held to it too" $root 'systems/renderer/tests/minimal_tests.cpp:1: [config-invariant-conditional]'

  $root = New-Fixture
  Set-FixtureFile $root 'systems/renderer/CMakeLists.txt' $invariantCmake
  Set-FixtureFile $root 'systems/renderer/tests/size_table.cpp' $invariantTable
  Set-FixtureFile $root 'systems/renderer/src/scene_sky.cpp' "#if defined(ENGINE_WITH_SKY)  // engine-lint: allow-config-conditional the reason, stated`nvoid sky() {}`n#endif`n"
  Test-That 'a line-level allow-config-conditional marker opts a line out' { (Invoke-Lint $root).Code -eq 0 }

  $root = New-Fixture
  Set-FixtureFile $root 'systems/water/CMakeLists.txt' "engine_module(NAME water LAYER systems OPTIONAL)`nengine_module_tests(NAME water SOURCES tests/w.cpp tests/size_table.cpp)  # not CONFIG_INVARIANT`n"
  Set-FixtureFile $root 'systems/water/tests/size_table.cpp' $invariantTable
  Set-FixtureFile $root 'systems/water/src/water.cpp' "#if ENGINE_WITH_TERRAIN`nvoid shore() {}`n#endif`n"
  Test-That 'the same conditional in a module that is not CONFIG_INVARIANT passes, a commented mention included' { (Invoke-Lint $root).Code -eq 0 }

  # shell-html-sink (docs/subsystems/engine_shell.md, "What another client's content cannot do"):
  # every file of apps/engine_shell/ puts text on the page as text. Each sink is reported in a
  # module of its own, on its own line; the fixture's page and module, whose comments name sinks and
  # whose code uses textContent, a text node, a literal import() and a timer given a function, pass.
  Test-That 'the shell''s text, its literal import and its function timer pass' {
    ($result.Code -eq 0) -and -not $result.Out.Contains('apps/engine_shell')
  }
  $sinks = [ordered]@{
    'innerHTML'                = 'el.innerHTML = s;'
    'outerHTML'                = 'el.outerHTML = s;'
    'insertAdjacentHTML'       = 'el.insertAdjacentHTML("beforeend", s);'
    'document.write'           = 'document.write(s);'
    'eval'                     = 'const v = eval(s);'
    'Function'                 = 'const f = new Function(s);'
    'a timer given a string'   = 'setTimeout("show()", 10);'
    'import() of a name'       = 'import(name).then((m) => m.run());'
    'createContextualFragment' = 'el.append(range.createContextualFragment(s));'
    'parseFromString'          = 'const d = new DOMParser().parseFromString(s, "text/html");'
    'srcdoc'                   = 'frame.srcdoc = s;'
  }
  foreach ($name in $sinks.Keys) {
    $root = New-Fixture
    Set-FixtureFile $root 'apps/engine_shell/queue.js' "// A module of the shell.`nexport function draw(el, s, name, range, frame) {`n  $($sinks[$name])`n}`n"
    Test-Reports "the shell's $name is reported" $root 'apps/engine_shell/queue.js:3: [shell-html-sink]'
  }
  $pageSinks = [ordered]@{
    'an inline script'    = '<script>start();</script>'
    'an inline handler'   = '<button onclick="start()">Start</button>'
    'a style attribute'   = '<div style="color: red"></div>'
    'a style element'     = '<style>body { color: red; }</style>'
    'a javascript: URL'   = '<a href="javascript:start()">Start</a>'
  }
  foreach ($name in $pageSinks.Keys) {
    $root = New-Fixture
    Set-FixtureFile $root 'apps/engine_shell/queue.html' "<!doctype html>`n<!-- a page of the shell -->`n$($pageSinks[$name])`n"
    Test-Reports "the shell page's $name is reported" $root 'apps/engine_shell/queue.html:3: [shell-html-sink]'
  }
  $root = New-Fixture
  Set-FixtureFile $root 'apps/engine_shell/queue.js' "export function draw(el, s) {`n  el.innerHTML = s;`n}`n"
  Test-Reports 'and the message names the way out' $root 'build nodes with dom.js''s h() and set text'
  $root = New-Fixture
  Set-FixtureFile $root 'apps/engine_shell/queue.js' "export function draw(el, s) {`n  el.innerHTML = s;  // engine-lint: allow-html-sink the reason, stated`n}`n"
  Test-That 'a line-level allow-html-sink marker opts a line out' { (Invoke-Lint $root).Code -eq 0 }
  $root = New-Fixture
  Set-FixtureFile $root 'tools/report/page.js' "document.body.innerHTML = report;`n"
  Test-That 'a script outside apps/engine_shell/ is not the shell''s' { (Invoke-Lint $root).Code -eq 0 }

  # person-flag (AGENTS.md, the agents' rule beside the view-policy one; engine_shell.md, "Who the
  # person at the editor is"): a committed script or launch configuration that starts engine-editor
  # passes --person on no line. The clean fixture holds one of each kind with the flag only in
  # comments, a --person given to another program, and engine-content's --persons.
  Test-That 'engine-editor started with no seat, the flag in a comment of each kind, another program''s --person and engine-content''s --persons pass' {
    ($result.Code -eq 0) -and -not $result.Out.Contains('person-flag')
  }
  $root = New-Fixture
  Set-FixtureFile $root 'tools/review.ps1' @'
$editor = Join-Path $bin 'engine-editor.exe'
& $editor --doc $doc --roles roles.json --person ada
'@
  Test-Reports 'a scratch script starting engine-editor with --person is refused' $root 'tools/review.ps1:2: [person-flag]'
  Test-Reports 'and the message says whose a review item for a person is' $root 'a review item for a person is closed by a person'
  $starts = [ordered]@{
    'a batch file, with --person-role'  = @('tools/bundle/review.cmd', "@echo off`nengine-editor.exe --doc world --person-role reviewer`n", 2)
    'a CI workflow'                     = @('.github/workflows/review.yml', "jobs:`n  review:`n    steps:`n      - run: build/msvc-debug/bin/engine-editor --doc world --person ada`n", 4)
    'a CMake test definition'           = @('apps/engine_editor/CMakeLists.txt', "add_test(NAME editor.seat COMMAND `$<TARGET_FILE:engine_engine_editor>`n  --doc world --person ada)`n", 2)
    'a launch configuration'            = @('.claude/launch.json', "{`n  `"configurations`": [{`n    `"runtimeExecutable`": `"build/msvc-debug/bin/engine-editor.exe`",`n    `"runtimeArgs`": [`"--doc`", `"world`", `"--person`", `"ada`"]`n  }]`n}`n", 4)
    'a game repository''s template'     = @('templates/game-repo/run.ps1', "& (Join-Path `$sdk 'bin/engine-editor.exe') --doc `$doc --person `$env:USERNAME`n", 1)
  }
  foreach ($name in $starts.Keys) {
    $root = New-Fixture
    $case = $starts[$name]
    Set-FixtureFile $root $case[0] $case[1]
    Test-Reports "engine-editor started with a seat from $name is refused" $root "$($case[0]):$($case[2]): [person-flag]"
  }
  $root = New-Fixture
  Set-FixtureFile $root 'tools/review.ps1' @'
$editor = Join-Path $bin 'engine-editor.exe'
& $editor --doc $doc --person ada  # engine-lint: allow-person-flag the reason, stated
'@
  Test-That 'a line-level allow-person-flag marker with its reason opts a line out' { (Invoke-Lint $root).Code -eq 0 }
  $root = New-Fixture
  Set-FixtureFile $root 'tools/review.ps1' @'
$editor = Join-Path $bin 'engine-editor.exe'
& $editor --doc $doc --person ada  # engine-lint: allow-person-flag
'@
  Test-Reports 'the marker without a reason is refused' $root 'tools/review.ps1:2: [person-flag] an allow-person-flag marker says why'
  # The tree itself: nothing committed starts engine-editor with a seat, and nothing uses the marker
  # (the lint's own two files name both to define and test the rule).
  $repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
  $tree = Invoke-Lint $repo
  Test-That 'the rule finds nothing in the tree today' {
    -not [string]::IsNullOrEmpty($tree.Out) -and -not $tree.Out.Contains('[person-flag]')
  }
  Test-That 'and no script, CMake file or launch configuration in the tree uses its marker' {
    if (Test-Path -LiteralPath (Join-Path $repo '.git')) {
      $uses = @(git -C $repo grep -l -e 'engine-lint: allow-person-flag' -- '*.ps1' '*.psm1' '*.cmd' '*.bat' '*.sh' '*.yml' '*.yaml' '*.json' '*CMakeLists.txt' '*.cmake' ':!tools/lint.ps1' ':!tools/lint.Tests.ps1')
      ($LASTEXITCODE -eq 1) -and $uses.Count -eq 0
    } else {
      # A tree that is no checkout (the Linux container's synced copy has no .git): the same kinds
      # of file, found by name outside the build and derived-data trees, the lint's own two files
      # left out.
      $own = @('tools/lint.ps1', 'tools/lint.Tests.ps1')
      $tops = @(Get-ChildItem -LiteralPath $repo -Force | Where-Object { @('build', 'ddc', '.claude') -notcontains $_.Name })
      $uses = @($tops | Get-ChildItem -Recurse -File -Include '*.ps1', '*.psm1', '*.cmd', '*.bat', '*.sh', '*.yml', '*.yaml', '*.json', 'CMakeLists.txt', '*.cmake' |
          Where-Object { $own -notcontains (([IO.Path]::GetRelativePath($repo, $_.FullName)) -replace '\\', '/') } |
          Select-String -SimpleMatch 'engine-lint: allow-person-flag' -List)
      $uses.Count -eq 0
    }
  }

  # machine-path (docs/ci/machine-paths.md, roadmap T175): every tracked text file, whatever its
  # kind, names no path of the machine it was written on. The samples are put together from pieces
  # so that this file, which the rule reads too, names none itself.
  $script:TrackFixtures = $true
  $drive = 'D' + ':'
  $cdrive = 'C' + ':'
  $ws = $drive + '\workspace'
  $caught = [ordered]@{
    'a Windows profile in a page'                    = @('docs/notes.md', "The captures are in ``$cdrive\Users\marc\AppData\Local\Temp\x``.")
    'a profile with forward slashes in a script'     = @('tools/a.ps1', "`$p = '$cdrive/Users/marc/.codex/worktrees/x'")
    'a POSIX home in a shell script'                 = @('tools/b.sh', ("cd /ho" + "me/marc/src"))
    'a macOS home in a README'                       = @('content/x/README.md', ("see /Us" + "ers/marc/Desktop"))
    'a Git Bash spelling of a profile'               = @('docs/c.md', ("``/c/Us" + "ers/marc/x``"))
    'a WSL spelling of a profile'                    = @('docs/d.md', ("``/mnt/c/Us" + "ers/marc/x``"))
    'the workspace folder in a script default'       = @('tools/e.ps1', "`$root = '$ws\game_engine_local'")
    'the workspace folder alone, forward slashes'    = @('cmake/F.cmake', "set(_dir `"$drive/workspace`")")
    'the workspace folder in a C++ test string'      = @('apps/x/tests/x_tests.cpp', "const char* p = `"$drive/workspace/game_engine_assets/x`";")
    'a Git Bash spelling of the workspace folder'    = @('tools/g.sh', ("cd /d/work" + "space/game_engine"))
    'a WSL spelling of the workspace folder'         = @('tools/h.sh', ("ls /mnt/d/work" + "space"))
    'an absolute path into an agent worktree'        = @('docs/i.json', ("{`"root`": `"E:\\src\\engine\\.claude\\worktrees\\agent-a" + "0123456789abcdef`"}"))
    'an absolute path into a slot worktree'          = @('docs/j.md', ("``/srv/engine/.claude/worktrees/slot-" + "2/build``"))
  }
  foreach ($case in $caught.GetEnumerator()) {
    $root = New-Fixture
    Set-FixtureFile $root $case.Value[0] $case.Value[1]
    Test-Reports "$($case.Key) is reported" $root "$($case.Value[0]):1: [machine-path]"
  }

  $root = New-Fixture
  $passes = @(
    "$cdrive\Program Files\Microsoft Visual Studio\18\Professional\VC\Tools\Llvm\x64\bin\clang-format.exe",
    "$cdrive\Windows\System32\wsl.exe and $cdrive\PROGRA~1\x",
    "$cdrive\Users\someone\AppData\Local\Microsoft\WinGet\Links\ccache.exe, $cdrive/Users/runner/work",
    "$cdrive\Users\<user>\.ssh, $cdrive\Users\...\x, $cdrive\Users\%USERNAME%\x, $cdrive\Users\`$env:USERNAME",
    "%USERPROFILE%\.ssh and `$env:LOCALAPPDATA\x and ~/.cache and `$HOME/game_engine_local",
    "/ho" + "me/runner/work and /ho" + "me/<user>/x and file:///ho" + "me/x and https://example.com/home/marc",
    "$drive\games\engine-sdk, E:\cc, $drive\work\engine\.claude\worktrees\agent-a1, $drive\...\x, <workspace>\gpu.lock",
    "game_engine_local\captures\x and .claude/worktrees/agent-a0123456789abcdef (relative) and $drive\workspaces-old is not it"
  )
  Set-FixtureFile $root 'docs/neutral.md' ($passes -join "`n")
  # A binary file is not read, whatever bytes it holds.
  $binary = [Text.Encoding]::UTF8.GetBytes("x$ws\y") + [byte[]](0, 1, 2)
  New-Item -ItemType Directory -Force -Path (Join-Path $root 'content') | Out-Null
  [IO.File]::WriteAllBytes((Join-Path $root 'content/blob.bin'), [byte[]](@(0) + $binary))
  $result = Invoke-Lint $root
  Test-That 'what is the same on every machine passes: toolchains, placeholders, neutral names, sample drives, a binary' { $result.Code -eq 0 }
  if ($result.Code -ne 0) { Write-Host $result.Out }

  $root = New-Fixture
  Set-FixtureFile $root 'tools/k.ps1' "`$lock = '$ws\gpu.lock'   # engine-lint: allow-machine-path the protocol's fixed path, shared with tools outside the repository"
  Set-FixtureFile $root 'core/l.cpp' "  return `"$ws\\gpu.lock`";  // engine-lint: allow-machine-path the protocol's location"
  Test-That 'a line marked with its reason passes, in any comment syntax' { (Invoke-Lint $root).Code -eq 0 }

  $root = New-Fixture
  Set-FixtureFile $root 'tools/k.ps1' "`$lock = '$ws\gpu.lock'   # engine-lint: allow-machine-path"
  Test-Reports 'a marker with no reason does not let a line through' $root 'tools/k.ps1:1: [machine-path]'

  $root = New-Fixture
  Set-FixtureFile $root 'docs/adr/0001-x.md' "The protocol is ``$ws\GPU-LOCK.md``."
  Set-FixtureFile $root 'tools/machine-path-exemptions.txt' "# a comment`n`ndocs/adr/0001-x.md: an accepted ADR is never edited`n"
  Test-That 'a file on the exemption list with its reason passes' { (Invoke-Lint $root).Code -eq 0 }

  $root = New-Fixture
  Set-FixtureFile $root 'docs/adr/0001-x.md' "The protocol is ``$ws\GPU-LOCK.md``."
  Set-FixtureFile $root 'tools/machine-path-exemptions.txt' "docs/adr/0001-x.md`n"
  Test-Reports 'an exemption with no reason is refused' $root 'tools/machine-path-exemptions.txt:1: [machine-path] an exemption is "<file>: <reason>"'

  $root = New-Fixture
  Set-FixtureFile $root 'tools/machine-path-exemptions.txt' "docs/adr/0009-gone.md: it was accepted`n"
  Test-Reports 'an exemption naming no file of the tree is refused' $root 'docs/adr/0009-gone.md is not a file of the tree; remove the line'

  $root = New-Fixture
  Set-FixtureFile $root 'docs/adr/0001-x.md' "The protocol is GPU-LOCK.md's."
  Set-FixtureFile $root 'tools/machine-path-exemptions.txt' "docs/adr/0001-x.md: an accepted ADR is never edited`n"
  Test-Reports 'an exemption for a file that no longer names a path is refused' $root 'docs/adr/0001-x.md names no path of this machine now; remove its exemption in the same change'

  # Only what git tracks is read: an ignored file never reaches the repository. A tree that is no
  # checkout at all (the Linux container's synced copy has no .git) skips the rule and says so.
  $root = New-Fixture
  Set-FixtureFile $root '.gitignore' "scratch/`n"
  Set-FixtureFile $root 'scratch/notes.md' "under ``$ws\game_engine_local``"
  Test-That 'an ignored file is not read' { (Invoke-Lint $root).Code -eq 0 }

  $root = New-Fixture
  Set-FixtureFile $root 'docs/notes.md' "under ``$ws\game_engine_local``"
  $result = Invoke-Lint $root -Untracked
  Test-That 'a tree that is no checkout skips the rule, and says so' { $result.Code -eq 0 -and $result.Out.Contains('machine-path skipped') }

  # human-flag (docs/subsystems/protocol.md, "The task ledger and review items"): a role's `human`
  # flag is read in domain/protocol/src/policy.cpp alone, and every other check that a caller is a
  # person reads Caller::human_via, so a check copied from the working role's flag is reported.
  $root = New-Fixture
  Set-FixtureFile $root 'domain/protocol/src/policy.cpp' "namespace engine::protocol {`nbool person(const Role* role) { return role->human; }`n}`n"
  Set-FixtureFile $root 'domain/protocol/src/ledger.cpp' "namespace engine::protocol {`n// Not role->human: a person is known by caller.human_via alone.`nbool may_close(const Caller& caller) { return !caller.human_via.empty(); }`n}`n"
  Test-That 'the flag read in policy.cpp, human_via elsewhere and a comment naming the flag pass' { (Invoke-Lint $root).Code -eq 0 }

  $root = New-Fixture
  Set-FixtureFile $root 'domain/protocol/src/ledger.cpp' "namespace engine::protocol {`nbool may_close(const Caller& caller) { return caller.role != nullptr && caller.role->human; }`n}`n"
  Test-Reports 'a read of the flag outside policy.cpp is reported' $root 'domain/protocol/src/ledger.cpp:2: [human-flag]'
  Test-Reports 'and the message names the field to read instead' $root 'reads Caller::human_via'

  $root = New-Fixture
  Set-FixtureFile $root 'apps/engine_editor/review.cpp' "bool person(const Role& r) { return r.human; }`n"
  Test-Reports 'a read through a reference, in an app, is reported' $root 'apps/engine_editor/review.cpp:1: [human-flag]'

  $root = New-Fixture
  Set-FixtureFile $root 'domain/protocol/tests/policy_tests.cpp' "bool shipped(const Role& r) { return r.human; }  // engine-lint: allow-human-flag the shipped file's one person`n"
  Test-That 'a line-level allow-human-flag marker opts a line out' { (Invoke-Lint $root).Code -eq 0 }
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

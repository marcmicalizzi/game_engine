#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Scaffold a new engine capability: a module that adds itself and changes nothing else (ADR-0027).

.DESCRIPTION
  tools/new-capability.ps1 -Name <snake_case> [-Layer systems] [-Deps "base containers math jobs"]
                           [-WithSchema] [-WithBench] [-WithProtocol] [-Root <dir>]

  Generates, under <layer>/<name>/:
    CMakeLists.txt                       engine_module(... OPTIONAL), tests, optional bench
    include/<layer>/<name>/<name>.h      system skeleton, LOD policy, determinism, the checklist
    src/<name>.cpp
    tests/<name>_tests.cpp               one passing test per generated entry point
    tests/size_table.cpp                 ADR-0019
    schemas/<name>.schema                with -WithSchema
    bench/<name>_bench.cpp               with -WithBench
  A dependency that is itself an optional capability (domain/ecs, foundation/store, ...) also
  generates an `engine_capability_requires()` line, so that a configure with that capability
  switched off leaves this one out rather than failing (docs/plan/08-toolchain.md §8.5).

  and, outside it, only the lines that announce the module exists:
    <layer>/CMakeLists.txt               add_subdirectory(<name>), appended
    CMakeLists.txt                       add_subdirectory(<layer>), if the layer is new
    docs/subsystems/<name>.md            the page, with the ADR-0027 checklist embedded
    docs/subsystems/README.md            its row

  The capability is optional by construction: ENGINE_WITH_<UPPER_NAME> (ON by default) removes
  the module, its schema library, its tests, and its bench from the build, and ENGINE_MINIMAL=ON
  (the *-minimal presets, and CI) removes every capability at once. That build is the proof the
  rest of the tree does not depend on it.

  The script refuses to overwrite: it writes only files that do not exist, and it stops before
  writing the first one if the module directory or the docs page is already there.

.EXAMPLE
  tools/new-capability.ps1 -Name cloth -Layer systems -Deps "base containers math jobs" -WithSchema -WithBench
  tools/dev.ps1 test -Filter cloth
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true, Position = 0)]
  [string]$Name,

  [ValidateSet('core', 'foundation', 'domain', 'systems')]
  [string]$Layer = 'systems',

  # Engine modules this capability depends on, by name. Whitespace- or comma-separated, so
  # -Deps "base containers math" and -Deps base,containers,math both work. core/base is added
  # when it is missing, because the generated sources include <core/base/types.h>.
  [string[]]$Deps = @(),

  [switch]$WithSchema,
  [switch]$WithBench,
  [switch]$WithProtocol,

  # Repository root. Only the tests override it, to scaffold into a temporary tree.
  [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'

$LayerOrder = @{ core = 0; foundation = 1; domain = 2; systems = 3; apps = 4; game = 5 }

function ConvertTo-PascalCase([string]$snake) {
  ($snake -split '_' | ForEach-Object { $_.Substring(0, 1).ToUpperInvariant() + $_.Substring(1) }) -join ''
}

# Every module the tree already declares, with its layer, read from the CMake manifests rather
# than from modules.json so that the script works before the first configure. `$script:Capabilities`
# is filled at the same time: module name -> the capability switch it belongs to, for the modules
# declared OPTIONAL. A new capability that depends on one of those has to declare the edge
# (engine_capability_requires), or a configure with that capability off fails outright instead of
# leaving this one out — which is what `domain/ecs` did to the first capability that ticked.
function Get-DeclaredModules([string]$root) {
  $modules = @{}
  $script:Capabilities = @{}
  # Matched against the path relative to the root, because the root itself may sit under a
  # directory the pattern would otherwise exclude (a git worktree under .claude/, for one).
  $manifests = Get-ChildItem -Path $root -Recurse -Filter 'CMakeLists.txt' -File |
    Where-Object {
      [IO.Path]::GetRelativePath($root, $_.FullName) -notmatch '(^|[\\/])(build|third_party|_deps|\.git|\.claude)([\\/]|$)'
    }
  foreach ($manifest in $manifests) {
    $text = Get-Content -LiteralPath $manifest.FullName -Raw
    foreach ($m in [regex]::Matches($text, 'engine_module\(\s*NAME\s+(\w+)\s+LAYER\s+(\w+)([^)]*)\)')) {
      $modules[$m.Groups[1].Value] = $m.Groups[2].Value
      $rest = $m.Groups[3].Value
      if ($rest -match 'CAPABILITY\s+(\w+)') {
        $script:Capabilities[$m.Groups[1].Value] = $Matches[1]
      } elseif ($rest -match '(^|\s)OPTIONAL(\s|$)') {
        $script:Capabilities[$m.Groups[1].Value] = $m.Groups[1].Value
      }
    }
    foreach ($m in [regex]::Matches($text, 'engine_schema_library\(\s*NAME\s+(\w+)')) {
      $modules[$m.Groups[1].Value] = 'core'
    }
    foreach ($m in [regex]::Matches($text, 'engine_app\(\s*NAME\s+(\w+)')) {
      $modules[$m.Groups[1].Value] = 'apps'
    }
  }
  return $modules
}

$created = New-Object System.Collections.Generic.List[string]
$touched = New-Object System.Collections.Generic.List[string]

# Writes one file, LF-terminated and UTF-8 without a BOM (.editorconfig, .gitattributes).
function New-GeneratedFile([string]$path, [string]$text) {
  if (Test-Path -LiteralPath $path) { throw "refusing to overwrite $path" }
  $dir = Split-Path -Parent $path
  if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
  $text = $text -replace "`r`n", "`n"
  if (-not $text.EndsWith("`n")) { $text += "`n" }
  [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false))
  $created.Add([IO.Path]::GetRelativePath($Root, $path))
}

function Add-CMakeLine([string]$path, [string]$line) {
  $text = (Get-Content -LiteralPath $path -Raw) -replace "`r`n", "`n"
  if (($text -split "`n") -contains $line) { return $false }
  if (-not $text.EndsWith("`n")) { $text += "`n" }
  $text += "$line`n"
  [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false))
  return $true
}

# ---- validate --------------------------------------------------------------------------------

if ($Name -cnotmatch '^[a-z][a-z0-9]*(_[a-z0-9]+)*$') {
  throw "invalid capability name '$Name': snake_case, lower case, starting with a letter (AGENTS.md conventions)."
}

$Deps = @($Deps | ForEach-Object { $_ -split '[,\s]+' } | Where-Object { $_ })
$declared = Get-DeclaredModules $Root

if ($declared.ContainsKey($Name)) {
  throw "module '$Name' already exists in $($declared[$Name])/; pick another name."
}

$moduleDir = Join-Path $Root "$Layer/$Name"
if (Test-Path -LiteralPath $moduleDir) {
  throw "$moduleDir already exists; refusing to overwrite."
}

if ($WithProtocol -and $LayerOrder[$Layer] -lt $LayerOrder['domain']) {
  throw "-WithProtocol needs domain/protocol, which is above the $Layer layer; put the capability in domain or systems."
}

if ($Deps -notcontains 'base') { $Deps = @('base') + $Deps }
if ($WithProtocol -and $Deps -notcontains 'protocol') { $Deps += 'protocol' }
if ($WithSchema -and $Deps -notcontains "${Name}_schemas") { $Deps += "${Name}_schemas" }

foreach ($dep in $Deps) {
  if ($dep -eq "${Name}_schemas") { continue }   # declared first, in the file generated below
  if (-not $declared.ContainsKey($dep)) {
    throw "unknown dependency '$dep'. Declared modules: $(($declared.Keys | Sort-Object) -join ', ')"
  }
  if ($LayerOrder[$declared[$dep]] -gt $LayerOrder[$Layer]) {
    throw "dependency '$dep' is in the $($declared[$dep]) layer, above $Layer; a module depends only downward (ADR-0021)."
  }
}
$Deps = @($Deps | Select-Object -Unique)

# Capabilities this one cannot be built without, deduced from the dependencies the caller named.
# The generated schema library shares this capability's own switch, so it is not an edge.
$Requires = @()
foreach ($dep in $Deps) {
  if ($dep -eq "${Name}_schemas") { continue }
  if ($script:Capabilities.ContainsKey($dep)) { $Requires += $script:Capabilities[$dep] }
}
$Requires = @($Requires | Select-Object -Unique | Sort-Object)

$Pascal = ConvertTo-PascalCase $Name
$Upper = $Name.ToUpperInvariant()
$System = "$($Pascal)System"
$HeaderPath = "$Layer/$Name/$Name.h"

# ---- <layer>/<name>/CMakeLists.txt ------------------------------------------------------------

$schemaLine = ''
if ($WithSchema) {
  $schemaLine = @"


# The capability's own component and event types (ADR-0007). They live here, not in the shared
# schemas/ directory, so that adding them touches no file this capability does not own;
# CAPABILITY ties them to the same switch, so they leave the build with it.
engine_schema_library(NAME ${Name}_schemas SCHEMAS schemas/$Name.schema CAPABILITY $Name)
"@
}

$benchLine = ''
if ($WithBench) {
  $benchLine = @"


# TODO($Name): benchmark the hot path, not the skeleton (plan 11 §11.8).
engine_module_bench(NAME $Name
  SOURCES bench/$($Name)_bench.cpp)
"@
}

$requiresLine = ''
if ($Requires.Count -gt 0) {
  $requiresNames = $Requires -join ' '
  if ($Requires.Count -eq 1) {
    $requiresPhrase = "``$requiresNames`` is itself an optional capability and this one links it"
    $requiresOff = 'that capability'
  } else {
    $requiresPhrase = "``$requiresNames`` are themselves optional capabilities and this one links them"
    $requiresOff = 'any of them'
  }
  $requiresLine = @"


# The capability graph (docs/plan/08-toolchain.md section 8.5). $requiresPhrase,
# so the edge is declared: with $requiresOff switched off, this capability is switched off too —
# with a line on the configure's status output and a row in modules.json's
# disabled_capabilities — instead of failing a configure that is perfectly legitimate.
engine_capability_requires($Name $requiresNames)
"@
}

$cmakeText = @"
# $Name capability (ADR-0027). TODO($Name): one line saying what it is.
#
# OPTIONAL creates ENGINE_WITH_$Upper (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — the scheduler entry, tunables,
# schema types — from being dropped by the linker because nothing references them.$requiresLine$schemaLine

engine_module(NAME $Name LAYER $Layer OPTIONAL WHOLE_ARCHIVE
  DEPS $($Deps -join ' ')
  SOURCES src/$Name.cpp)

engine_module_tests(NAME $Name
  SOURCES tests/$($Name)_tests.cpp tests/size_table.cpp)$benchLine
"@

# ---- include/<layer>/<name>/<name>.h -----------------------------------------------------------

if ($WithSchema) {
  $checklistSchema = "[x] schema types      schemas/$Name.schema, engine_schema_library"
} else {
  $checklistSchema = "[ ] schema types      add schemas/$Name.schema, or say why there is no visible state"
}
if ($WithProtocol) {
  $checklistProtocol = "[x] protocol methods  register_methods() below, called by the app that owns the dispatcher"
} else {
  $checklistProtocol = "[ ] protocol methods  only if agents or the editor drive it; rerun with -WithProtocol"
}
if ($WithBench) {
  $checklistBench = "[x] bench             bench/$($Name)_bench.cpp"
} else {
  $checklistBench = "[ ] bench             required once there is a hot path; rerun with -WithBench"
}

$schemaInclude = ''
if ($WithSchema) { $schemaInclude = "`n#include <schemas/$Name.h>" }

$protocolForward = ''
if ($WithProtocol) {
  $protocolForward = @"

namespace engine::protocol {
class Dispatcher;
}
"@
}

$protocolDecl = ''
if ($WithProtocol) {
  $protocolDecl = @"

/// Adds this capability's protocol methods to a dispatcher (docs/plan/06-agent-tooling.md §6.2).
/// The app that owns the dispatcher calls it; the dispatcher never learns about capabilities on
/// its own, and a capability that is not linked adds nothing to the method table.
void register_methods(protocol::Dispatcher& dispatcher);
"@
}

$headerText = @"
#pragma once

// $Name capability (ADR-0027; docs/plan/02-architecture.md §2.8,
// docs/plan/05-simulation.md §5.15, docs/plan/11-performance-principles.md §11.10).
//
// TODO($Name): one paragraph — what this capability is, what it owns, what it does not do.
//
// Scaffolded by tools/new-capability.ps1. The module is additive by construction: nothing under
// core/ or foundation/, the render graph, the scheduler, or another capability is edited to add
// it, and ENGINE_WITH_$Upper=OFF (or ENGINE_MINIMAL=ON) removes it from the build
// entirely. Each registration point below is either wired up or deliberately not needed, and
// docs/subsystems/$Name.md says which. The list is ADR-0027 decision 2.
//
//   $checklistSchema
//   [ ] scheduler entry   the SystemDesc fields below, registered from src/$Name.cpp
//   [ ] render passes     gfx::RenderGraph::add_pass() from this system, never a graph edit
//   [ ] derived data      a content-build step keyed by the asset property block it consumes
//   $checklistProtocol
//   [ ] tunables          tunables::Int/Float/Bool/Enum at namespace scope, read outside hot loops
//   [x] LOD policy        ${System}::lod_tier() (the observer function, plan 05 §5.4)
//   [x] determinism       k_determinism below
//   [ ] zero cost unused  name the mechanism (plan 11 §11.10) on the docs page
//   [x] docs, tests, size table
//   $checklistBench
//
// The hot-path rules are unchanged (plan 11 §11.4): the scheduler reaches this system through a
// constant-initialized table of function pointers, never a virtual interface, and no loop in the
// engine asks at run time whether this capability is present — the linker already answered.

#include <core/base/types.h>$schemaInclude
$protocolForward
namespace engine::$Name {

// ---- the scheduler's registration table entry -------------------------------------------------
//
// These are SystemDesc's fields (ADR-0027). The tick scheduler does not exist yet — Phase 1 is
// the renderer core — so they are documented constants here, and the commit that lands the
// scheduler turns them into table fields without changing their meaning.

/// Tick phase this system runs in (plan 05 §5.2): "input", "events_in", "lod", "systems",
/// "physics", "post_physics", "events_out", "persist".
inline constexpr const char* k_phase = "systems";

/// Determinism stance (ADR-0010). "hashed": the state lives in the fixed-step tick, enters the
/// sim hash, and replays exactly. "derived": output of the sim that is never read back into
/// gameplay (GPU work, visuals). TODO($Name): decide, and say why on the docs page.
inline constexpr const char* k_determinism = "hashed";

/// LOD tiers this system runs at, one bit per tier, bit 0 is LOD0 (ADR-0010). A system that runs
/// at every tier is usually a system that has not thought about it.
inline constexpr u32 k_lod_tiers = 0b0011u;

/// The capability's system. begin_tick() runs once before the parallel systems phase, tick()
/// inside it, end_tick() once after. They are plain member functions reached through the
/// registration table, so a tick costs no virtual dispatch, and a tick with no instances of this
/// capability does not call them at all: the scheduler drops a system whose instance set is
/// empty (plan 11 §11.10), which is what makes an unused capability free.
struct $System {
  /// Once per tick, before the parallel phase: take the tick index, size this tick's pools.
  void begin_tick(u64 tick) noexcept;

  /// The per-tick work, over this capability's own instances only.
  /// TODO($Name): this is the capability.
  void tick(f32 dt) noexcept;

  /// Once per tick, after the parallel phase: publish results, emit events.
  void end_tick() noexcept;

  /// The LOD tier for one instance given its observer score: the minimum over observers of
  /// f(distance, importance, observer weight) (plan 05 §5.4, ADR-0010). Lower is nearer. The
  /// hysteresis band belongs here, in the capability, not in the caller.
  /// TODO($Name): the capability's real tiers and their bands.
  static u32 lod_tier(f32 observer_score) noexcept;

  u64 tick_index() const noexcept { return tick_; }
  f32 elapsed() const noexcept { return elapsed_; }

 private:
  u64 tick_ = 0;
  f32 elapsed_ = 0.0f;
};
$protocolDecl
}  // namespace engine::$Name
"@

# ---- src/<name>.cpp ----------------------------------------------------------------------------

$protocolSrcInclude = ''
if ($WithProtocol) { $protocolSrcInclude = "`n#include <domain/protocol/rpc.h>`n" }

$protocolDef = ''
if ($WithProtocol) {
  $protocolDef = @"

void register_methods(protocol::Dispatcher&) {
  // TODO($Name): dispatcher.add(protocol::method<Params, Result, &handler>("$Name.thing", "doc"));
  // Params and Result are schema types, so the catalogue engine.methods returns, the JSON Schema
  // schemac emits, and this handler agree by construction (ADR-0007).
}
"@
}

$sourceText = @"
#include <$HeaderPath>
$protocolSrcInclude
namespace engine::$Name {

void ${System}::begin_tick(u64 tick) noexcept {
  tick_ = tick;
  elapsed_ = 0.0f;
}

void ${System}::tick(f32 dt) noexcept {
  // TODO($Name): the capability's per-tick work. Iterate this capability's own storage in memory
  // order; no data-dependent branch inside the inner loop (plan 11 §11.3, §11.4).
  elapsed_ += dt;
}

void ${System}::end_tick() noexcept {
  // TODO($Name): publish results and emit events.
}

u32 ${System}::lod_tier(f32 observer_score) noexcept {
  // TODO($Name): the real tier boundaries and their hysteresis (plan 05 §5.4). Placeholder:
  // simulated near, dropped far, so the policy exists and is tested from the first commit.
  return observer_score < 1.0f ? 0u : 1u;
}
$protocolDef
}  // namespace engine::$Name
"@

# ---- tests ------------------------------------------------------------------------------------

$testsText = @"
// Tests for the $Name capability (ADR-0027). TODO($Name): the invariants listed on the docs page
// belong here, one test each, before the capability ships.
#include <$HeaderPath>

#include <doctest/doctest.h>

#include <string_view>

using namespace engine;

TEST_CASE("$Name system runs a tick") {
  ${Name}::$System system;
  system.begin_tick(7);
  system.tick(1.0f / 60.0f);
  system.end_tick();
  CHECK(system.tick_index() == 7u);
  CHECK(system.elapsed() == doctest::Approx(1.0f / 60.0f));
}

TEST_CASE("$Name LOD policy never coarsens as the observer gets nearer") {
  // The tier is monotonic in the observer score (plan 05 §5.4); hysteresis, once it exists,
  // widens the boundaries but must not break this.
  CHECK(${Name}::${System}::lod_tier(0.0f) <= ${Name}::${System}::lod_tier(10.0f));
  CHECK(${Name}::${System}::lod_tier(10.0f) <= ${Name}::${System}::lod_tier(1000.0f));
}

TEST_CASE("$Name declares a determinism stance") {
  const std::string_view determinism{${Name}::k_determinism};
  CHECK((determinism == "hashed" || determinism == "derived"));
}
"@

$sizeTableText = @"
// Size table for $Layer/$Name (ADR-0019). TODO($Name): every hot type this capability adds —
// components, per-instance solver state, GPU-mirrored structs — gets an entry, so that footprint
// regressions fail the build rather than the frame rate.
#include <core/base/size_table.h>

#include <$HeaderPath>

using namespace engine;

ENGINE_EXPECT_SIZE(16, 8, ${Name}::$System);
"@

$benchText = @"
// Micro-benchmarks for the $Name capability (docs/plan/11-performance-principles.md §11.8).
// TODO($Name): measure the capability's hot path. The before-and-after numbers in a change
// description come from here, and a capability with a hot path and no bench is not finished.
#include <foundation/bench/bench.h>

#include <$HeaderPath>

using namespace engine;

ENGINE_BENCH_ARGS($($Name)_tick, "$Name.tick", 64, 1024, 16384) {
  const u32 instances = static_cast<u32>(state.arg());
  ${Name}::$System system;
  system.begin_tick(0);
  while (state.keep_running()) {
    for (u32 i = 0; i < instances; ++i)
      system.tick(1.0f / 60.0f);
    bench::keep(system.elapsed());
  }
  state.set_items(instances);
}
"@

$schemaText = @"
// $Name components and events (ADR-0007, ADR-0027). The capability's own types live with the
// capability, so adding them touches no shared file, and ENGINE_WITH_$Upper=OFF takes them out of
// the build with everything else it owns.
namespace engine.$Name

/// TODO($Name): the component that makes an entity participate in this capability. An entity
/// without it costs nothing: no component, no instance, no work (plan 11 §11.10).
struct $Pascal @version(1) @kind(component) {
  /// TODO($Name): replace with the capability's real fields.
  enabled: bool = true
}

/// TODO($Name): what this capability tells the rest of the world about. Gameplay-relevant events
/// are hashed and replay (ADR-0010).
struct $($Pascal)Event @version(1) @kind(event) {
  entity: id128
  tick: u64 = 0
}
"@

# ---- docs/subsystems/<name>.md -----------------------------------------------------------------

if ($WithSchema) {
  $docsSchemaRow = "| Component and event types | ``$Layer/$Name/schemas/$Name.schema`` | scaffolded, TODO |"
} else {
  $docsSchemaRow = "| Component and event types | none | TODO: add one, or say why this has no visible state |"
}
if ($WithProtocol) {
  $docsProtocolRow = "| Protocol methods | ``$($Name)::register_methods()`` | scaffolded, TODO |"
} else {
  $docsProtocolRow = "| Protocol methods | none | not needed |"
}
if ($WithBench) {
  $docsBenchRow = "| Bench | ``$Layer/$Name/bench/$($Name)_bench.cpp`` | scaffolded, TODO |"
} else {
  $docsBenchRow = "| Bench | none | TODO once there is a hot path |"
}
if ($Requires.Count -gt 0) {
  $docsRequiresCall = 'engine_capability_requires(' + $Name + ' ' + ($Requires -join ' ') + ')'
  $docsRequiresRow = '| Capabilities it requires | `' + $docsRequiresCall + '` | declared; off when ' +
                     ($Requires -join ', ') + ' is off |'
} else {
  $docsRequiresRow = '| Capabilities it requires | none | this capability stands alone |'
}
$docsBenchCommand = ''
if ($WithBench) { $docsBenchCommand = " Benchmarks: ``tools/dev.ps1 bench -Filter '$Name.*'``." }

$docsText = @"
# $Name ($Layer)

**Purpose.** TODO($Name): one paragraph. What this capability is, which use cases it serves, and
what it deliberately does not do.

**Why this shape.** TODO($Name): why it is built this way and not another way — the alternatives
that were on the table, the measurement or constraint that chose the data layout, the solver, and
the LOD policy, and what would have to change for a different answer to be right. A page that says
only what the code does leaves the next reader to rediscover the reasoning at the price the first
one paid (AGENTS.md, "Write the why, not only the what"). If a decision here is one a future
contributor could be surprised by, it is an ADR, and this paragraph links it.

**Owned data.** TODO($Name): what this module is the source of truth for. Nothing else may hold or
mutate it.

**Invariants.** TODO($Name): bullet list, each one checked by a test.

**Public API.** ``include/$Layer/$Name/$Name.h``.

**Depends on.** $(($Deps | ForEach-Object { "``$_``" }) -join ', ').

**Testing.** ``tools/dev.ps1 test -Filter $Name``.$docsBenchCommand

**Performance notes.** TODO($Name): hot paths, layout decisions, tunables, and the budget this
capability is held to (docs/plan/11-performance-principles.md §11.1).

## Capability contract (ADR-0027)

This is a capability: it was added without editing ``core/``, ``foundation/``, the render graph,
the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
$docsRequiresRow
$docsSchemaRow
| Tick scheduler entry | ``$($Name)::$System``, phase ``$($Name)::k_phase`` | TODO: register when the scheduler lands |
| Render-graph passes | none | TODO: state whether this capability draws |
| Content-build derived step | none | TODO: state whether anything is precomputed from content |
$docsProtocolRow
| Tunables | none | TODO: every run-time parameter, never a constant |
| LOD policy | ``$($System)::lod_tier()`` | scaffolded, TODO: real tiers and hysteresis |
| Determinism | ``$($Name)::k_determinism`` = ``hashed`` | TODO: confirm, and say why |
| Zero cost when unused | TODO: which mechanism of plan 11 §11.10 applies | TODO |
| Tests and size table | ``tests/$($Name)_tests.cpp``, ``tests/size_table.cpp`` | scaffolded |
$docsBenchRow
| Removal proof | ``ENGINE_WITH_$Upper``, off in the minimal build | works |

**Removing it.** ``cmake --preset msvc-minimal`` (or ``-DENGINE_WITH_$Upper=OFF``) drops the
module, its tests, and its bench; the module disappears from ``build/<preset>/modules.json`` and
is listed there under ``disabled_capabilities``. Everything else must still build and pass.
"@

# ---- write -------------------------------------------------------------------------------------

$docsDir = Join-Path $Root 'docs/subsystems'
$docsPage = Join-Path $docsDir "$Name.md"
$docsReadme = Join-Path $docsDir 'README.md'
$layerCMake = Join-Path $Root "$Layer/CMakeLists.txt"
$rootCMake = Join-Path $Root 'CMakeLists.txt'

if (Test-Path -LiteralPath $docsPage) { throw "refusing to overwrite $docsPage" }
if (-not (Test-Path -LiteralPath $rootCMake)) { throw "$rootCMake not found; is -Root a repository root?" }

New-GeneratedFile (Join-Path $moduleDir 'CMakeLists.txt') $cmakeText
New-GeneratedFile (Join-Path $moduleDir "include/$Layer/$Name/$Name.h") $headerText
New-GeneratedFile (Join-Path $moduleDir "src/$Name.cpp") $sourceText
New-GeneratedFile (Join-Path $moduleDir "tests/$($Name)_tests.cpp") $testsText
New-GeneratedFile (Join-Path $moduleDir 'tests/size_table.cpp') $sizeTableText
if ($WithSchema) { New-GeneratedFile (Join-Path $moduleDir "schemas/$Name.schema") $schemaText }
if ($WithBench) { New-GeneratedFile (Join-Path $moduleDir "bench/$($Name)_bench.cpp") $benchText }
New-GeneratedFile $docsPage $docsText

# The layer's CMakeLists: the root adds lower layers first, and within a layer a module is
# declared after its dependencies, so appending is the right place for a capability — everything
# it depends on is already above it.
if (-not (Test-Path -LiteralPath $layerCMake)) {
  New-GeneratedFile $layerCMake @"
# Layer L$($LayerOrder[$Layer]): $Layer. Declaration order matters (dependencies first).
"@
}
if (Add-CMakeLine $layerCMake "add_subdirectory($Name)") {
  $touched.Add("$Layer/CMakeLists.txt: add_subdirectory($Name)")
}

# The root CMakeLists: hook the layer up if this is the layer's first module.
$rootText = (Get-Content -LiteralPath $rootCMake -Raw) -replace "`r`n", "`n"
if ($rootText -notmatch "(?m)^\s*add_subdirectory\($Layer\)\s*$") {
  if ($rootText -match "(?m)^\s*#\s*add_subdirectory\($Layer\)\s*$") {
    $rootText = $rootText -replace "(?m)^\s*#\s*add_subdirectory\($Layer\)\s*$", "add_subdirectory($Layer)"
  } elseif ($rootText -match "(?m)^\s*add_subdirectory\(apps\)\s*$") {
    $rootText = $rootText -replace "(?m)^(\s*add_subdirectory\(apps\)\s*)$", "add_subdirectory($Layer)`n`$1"
  } else {
    throw "cannot find where to put add_subdirectory($Layer) in $rootCMake; add it by hand."
  }
  [System.IO.File]::WriteAllText($rootCMake, $rootText, (New-Object System.Text.UTF8Encoding $false))
  $touched.Add("CMakeLists.txt: add_subdirectory($Layer)")
}

# The docs/subsystems README row, after the last module of this layer or of a lower one.
if (Test-Path -LiteralPath $docsReadme) {
  $lines = @((Get-Content -LiteralPath $docsReadme -Raw) -replace "`r`n", "`n" -split "`n")
  $row = "| $Name | $Layer | [$Name.md]($Name.md) |"
  if ($lines -notcontains $row) {
    $insertAt = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
      if ($lines[$i] -match '^\|\s*[\w ,+/()]+\s*\|\s*(\w+)\s*\|\s*\[') {
        $rowLayer = $Matches[1]
        if ($LayerOrder.ContainsKey($rowLayer) -and $LayerOrder[$rowLayer] -le $LayerOrder[$Layer]) {
          $insertAt = $i + 1
        }
      }
    }
    if ($insertAt -lt 0) { throw "cannot find the module table in $docsReadme; add the row by hand." }
    $lines = @($lines[0..($insertAt - 1)]) + @($row) + @($lines[$insertAt..($lines.Count - 1)])
    [System.IO.File]::WriteAllText($docsReadme, ($lines -join "`n"), (New-Object System.Text.UTF8Encoding $false))
    $touched.Add("docs/subsystems/README.md: the $Name row")
  }
}

# ---- report ------------------------------------------------------------------------------------

Write-Host ''
Write-Host "created the $Name capability ($Layer layer, switch ENGINE_WITH_$Upper):" -ForegroundColor Cyan
foreach ($f in $created) { Write-Host "  + $f" }
foreach ($t in $touched) { Write-Host "  ~ $t" }
Write-Host ''
Write-Host 'next:'
Write-Host "  tools/dev.ps1 test -Filter $Name                                      # it builds and passes as generated"
Write-Host '  cmake --preset msvc-minimal; cmake --build --preset msvc-minimal      # the removal proof'
Write-Host "  then work through the checklist in $Layer/$Name/include/$HeaderPath and docs/subsystems/$Name.md"
Write-Host ''
Write-Host 'The contract is ADR-0027 and docs/plan/02-architecture.md §2.8: nothing outside'
Write-Host "$Layer/$Name/ and those two documentation files should change to finish this capability."

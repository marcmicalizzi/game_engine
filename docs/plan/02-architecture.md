# 02 — Recommended High-Level Architecture

## 2.1 Shape of the system

The engine is a **headless simulation-and-rendering server** with a **text-based, versioned authoring document** as its source of truth, driven by clients over a **schema-generated protocol**. Humans use a GUI editor client; agents use an MCP bridge client; CI uses a CLI client. All three issue the same commands through the same transaction system.

```
┌─────────────────────────────────────────────────────────────────────┐
│  Clients                                                            │
│   GUI editor   │  MCP bridge (agents)  │  CLI / CI runner  │ tests  │
└───────┬────────┴───────────┬───────────┴─────────┬─────────┴────┬───┘
        │      Engine Protocol (JSON-RPC over WS/stdio + bulk channel) │
┌───────▼─────────────────────▼─────────────────────▼──────────────▼───┐
│  engine-host process                                                 │
│  ┌──────────────┐ ┌──────────────┐ ┌───────────────┐ ┌────────────┐  │
│  │ Transaction  │ │ Query/       │ │ Capture/      │ │ Validation │  │
│  │ service      │ │ introspection│ │ profiling svc │ │ service    │  │
│  └──────┬───────┘ └──────┬───────┘ └───────┬───────┘ └─────┬──────┘  │
│  ┌──────▼───────────────▼────────────────▼───────────────▼──────┐   │
│  │ World Document (authoring model, in-memory, layered)          │   │
│  └──────┬───────────────────────────────────────────────────────┬┘   │
│         │ compile (derived-data graph)                          │    │
│  ┌──────▼──────────────┐   ┌────────────────────────────────────▼─┐  │
│  │ Runtime World       │   │ Persistent World State (event log +  │  │
│  │ (ECS, hot state)    │◄──┤ snapshots, SQLite-backed)            │  │
│  └──────┬──────────────┘   └──────────────────────────────────────┘  │
│  ┌──────▼──────────────────────────────────────────────────────────┐ │
│  │ Systems: sim scheduler · physics · nav · anim · audio · destr.  │ │
│  │          streaming · renderer (cluster geo + RT) · UI · input   │ │
│  └─────────────────────────────────────────────────────────────────┘ │
│  Foundation: jobs · memory · schema/reflection · serialization ·     │
│              I/O · logging/telemetry · tunables · profiling          │
└──────────────────────────────────────────────────────────────────────┘
        │                                    │
┌───────▼──────────┐               ┌─────────▼──────────────────────┐
│ Derived Data     │               │ Content Build (out-of-process)  │
│ Cache (CAS)      │◄──────────────┤ mesh→clusters→AS, textures,     │
└──────────────────┘               │ navmesh, structural graphs, …   │
                                   └────────────────────────────────┘
```

Three data domains, deliberately distinct because their access patterns are distinct:

| Domain | Lives in | Access pattern | Owner |
|---|---|---|---|
| **Authoring document** | Text files in git, loaded to memory | Edited transactionally by humans/agents; diffed; merged | Transaction service |
| **Runtime world** | ECS archetype storage + per-system SoA buffers + GPU buffers | Per-frame, cache-friendly, transient; rebuilt from the other two on load | Systems |
| **Persistent world state** | Event log + periodic snapshots (SQLite) | Append-heavy writes, ad-hoc queries, survives across sessions and saves | World state service |

The runtime world is a *materialization*: authoring document (what the designer built) + persistent state (what has happened since) → runtime entities at the current sim LOD. This separation is what makes sim LOD, save games, and agent introspection tractable. Details in [03-data-model](03-data-model.md).

## 2.2 Process model

- **`engine-host`**: the single runtime binary. Runs in one of three modes: `sim` (no GPU, no window; playtesting, world simulation, tests), `offscreen` (GPU, no window; captures, image tests, benchmarks), `windowed` (normal play, or editor viewport). All modes speak the protocol.
- **`content-build`**: out-of-process, parallel, cache-driven derived-data compiler. Also callable in-process for editor hot-reload of a single asset.
- **`editor`**: the full human-facing GUI client: viewport with gizmos, outliner and property panels, asset browser, material, terrain, animation, quest and dialogue tools, profiler, review queue. It is a complete development environment for teams that never use an agent and the place humans adjust what agents produced ([06 §6.13](06-agent-tooling.md#613-human-developer-tooling)). It connects to a local `engine-host` and has no privileged access: every gesture compiles to the same commands agents send. Dear ImGui initially.
- **`mcp-bridge`**: adapts the Engine Protocol to MCP tools with curated, well-documented surfaces. Stateless; can be restarted without disturbing the engine.
- **`engine-cli`**: thin scriptable client for CI and shell use.
- **Generator services** (mesh/texture/speech generators): separate processes behind a uniform "generator" interface; may be remote.

One `engine-host` may host multiple *sessions* (documents/worlds) for agents working in parallel, but a single session has a single authoritative document.

## 2.3 Subsystem boundaries and layering

Layering is enforced by the build: a module may only depend on modules in lower layers or the same layer where explicitly allowed. Agents (and humans) can then reason locally about any module.

```
L5  game/                  gameplay systems, content, quests   (per-game)
L4  apps/                  engine-host, editor, mcp-bridge, engine-cli, content-build
L3  systems/               renderer, streaming, simulation, destruction, deformation, ui, animation,
                           audio-system, nav-system, physics-system, world-state, worldgen
L2  domain/                gfx (RHI+render graph), physics (Jolt wrapper), nav (Recast wrapper),
                           audio (device), ecs (world runtime), doc (authoring document),
                           protocol (RPC), ddc (derived data cache), geometry (cluster builder)
L1  foundation/            io/vfs, asset-db, tunables, profiling, telemetry, calibration,
                           scripting-host
L0  core/                  platform, memory, containers, math, jobs, log, schema (codegen runtime),
                           serialization, hash, time, ids
```

**Ownership rules**

- Every `L2+` module owns exactly one kind of data and exposes it through a schema-declared interface. No module reaches into another's storage.
- Hot systems (renderer, physics, animation) own their own SoA/GPU representations. The ECS holds *identity, relationships, and cold-to-warm component data*; it is the index, not the hot loop.
- Cross-system communication is via (a) the sim event bus for gameplay-meaningful events, (b) explicit per-frame data handoffs declared in the frame graph, never via ad-hoc calls into another system mid-update.
- Anything that can be a **pure function of content** (mesh → clusters, mesh → structural graph, tile → navmesh) is a content-build step, not a runtime step, even if it also has a fast in-process path for editing.

## 2.4 Frame and time model

```
wall clock ──► frame pacing ──► render frame N (variable dt, non-deterministic)
                                     ▲ interpolates
sim clock  ──► fixed step (e.g. 60 Hz) ──► sim tick k (deterministic)
                                     ▲ drives
game clock ──► scaled sim time (e.g. 1 game-minute per sim-second) ──► scheduler events
```

- **Sim tick** is fixed-step, deterministic, seeded, and replayable from an input log. It runs the LOD0/LOD1 systems. Tick counters and game time are 64-bit integers; floating point appears only in per-frame deltas, so precision does not degrade over long sessions.
- **Game time** is sim time × scale; it drives the event scheduler for LOD2/LOD3 and world systems. It can advance in large jumps (sleep, travel, headless fast-forward).
- **Render frame** is decoupled, may run faster or slower than sim, interpolates between sim states, and has no effect on sim.
- **Observers** (players, cameras, playtest bots) are first-class; LOD assignment is a function over the set of observers.

## 2.5 Repository organization for agent comprehension

```
/
  AGENTS.md                 how to work in this repo (build, test, conventions, layering rules)
  docs/
    plan/                   this plan
    adr/                    architecture decision records, numbered, immutable once accepted
    subsystems/             one page per module: purpose, owned data, invariants, public API, tests
  schemas/                  IDL source of truth: document schema, protocol, events, components
  core/ foundation/ domain/ systems/ apps/ game/   (layers above)
    <module>/
      CMakeLists.txt        engine_module(NAME LAYER DEPS ...) is the module manifest;
                            CMake enforces layering at configure time and emits build/modules.json
      README.md             purpose, invariants, how to test
      include/<module>/     public headers only
      src/
      tests/                unit + property tests
      bench/                micro-benchmarks registered with the tunables system
  tools/                    PowerShell 7 scripts and small C++ tools (no Python, ADR-0021), codegen, CI scripts
  content/
    test-scenes/            deterministic reference scenes (versioned)
    golden/                 golden images and metrics, by scene × resolution × GPU class
  third_party/              vendored or vcpkg manifests, with LICENSES.md
```

Conventions that specifically help LLM-generated code:

- **One schema, everything generated.** Components, events, protocol messages, document types are declared once in `schemas/`; C++ types, JSON (de)serializers, JSON Schema, protocol docs, MCP tool definitions, and migration stubs are generated. Agents never hand-write serialization.
- **Machine-readable module manifests.** Each module declares its name, layer, and dependencies once, in its `CMakeLists.txt` through `engine_module()`. CMake refuses a dependency on a higher layer at configure time and writes `build/modules.json` so agents can read the module graph without parsing CMake.
- **Invariants as code.** Every module exposes `validate()`; debug builds run it at system boundaries; the validation service exposes it to agents.
- **No implicit global state.** Systems receive their dependencies explicitly; there is exactly one service locator and it is only used at the app layer.
- **Small public surfaces.** Public headers are the contract; internal headers are not visible to other modules.
- **One container set.** Engine code uses `core/containers` (flat maps and sets, open-addressing hash maps, small and fixed vectors, slot maps, intrusive lists, bitsets). Node-based standard containers and `std::shared_ptr` are banned outside tools, tests, and cold initialization, enforced by clang-tidy ([11 §11.2](11-performance-principles.md#112-data-layout-and-footprint-first)).
- **A size table for hot types**, checked by `static_assert`, so footprint regressions fail the build rather than the frame rate.
- **Tests and benchmarks are colocated** with the module and discoverable by a uniform command.
- **ADRs** record every decision in this plan that gets confirmed or reversed, so agents can find *why* something is the way it is without reading git history.
- **Documentation moves with the code.** Every module has a `docs/subsystems/` page, every page is indexed, every link resolves, and a change to an interface lands with the page that describes it — the invariant is stated once, in [AGENTS.md](../../AGENTS.md), and checked by `tools/docs-check.ps1` (CTest: `docs_check`) and by CI's documentation gate (`tools/docs-gate.sh`). The documents are the context an agent starts from, so a stale one costs every future session.

## 2.6 Explicit scope for the first engine version

In scope: Windows first, Linux second, with feature parity as the goal and documented gaps where a vendor extension is missing on one platform; Vulkan behind a thin API-neutral RHI (rationale in [08-toolchain §8.3](08-toolchain.md#83-graphics-api)); desktop GPUs with hardware RT (NVIDIA first, AMD second, Intel third); single-player for the first game, with the multiplayer-readiness rules in [05 §5.12](05-simulation.md#512-multiplayer-readiness) enforced from the start; keyboard/mouse/gamepad; both agent-driven and fully manual development tooling; a 1–3 hour game.

Out of scope for the first engine version, recorded as decisions (see [10-roadmap-risks](10-roadmap-risks.md#106-decisions-deliberately-deferred)): consoles; mobile (not precluded, see §2.7); shipping multiplayer (kept open, not built); VR; D3D12 and Metal backends (deferred); non-RT GPUs as a quality target (they run the baseline tier at reduced quality).

## 2.7 Generality: what the engine must not preclude

The first game is small and single-player, but the engine should not bake in limits a later game would hit. The following are cheap to get right now and expensive to retrofit.

| Later need | What a limit would look like | Architectural hook now |
|---|---|---|
| Very large or space-scale worlds (flight, open worlds beyond ~10 km) | float32 world positions jitter far from the origin | Positions are (tile index, float local offset); rendering uses camera-relative transforms; no absolute float32 world coordinate exists anywhere |
| Long sessions | float seconds accumulate error after hours | 64-bit tick counters and integer game time; float only in per-frame deltas |
| Many animated units (RTS, crowds, 10^4 and up) | CPU skinning, one draw per entity | GPU skinning, cluster instancing, animation LOD; entity count is a budget, not a constant |
| Multiplayer | wall-clock reads in gameplay, a "the player" singleton, render-coupled logic | Deterministic fixed-step sim, observer set, schema replication annotations, headless `sim` mode as a dedicated server ([05 §5.12](05-simulation.md#512-multiplayer-readiness)) |
| Split-screen, VR, surround, portrait | single-view assumptions | `ViewSet` is first-class ([04 §4.6](04-renderer.md#46-extreme-displays)) |
| Modding | binary levels, hard-coded content paths | Document layers *are* mods: a mod is a layer plus a content pack; scripting is sandboxed |
| Custom rendering (stylized looks, extra passes) | a closed render pipeline | The render graph accepts game-registered passes; the material graph is extensible; Slang modules |
| Extreme resolutions and FOV | packed screen coordinates, fixed atlas sizes | Checked compact types with fallback; every size from the render config ([11 §11.6](11-performance-principles.md#116-no-hidden-limits)) |
| Localization and accessibility | ASCII assumptions, hard-coded text, fixed input | UTF-8 everywhere, string IDs, RTL-capable text layout, remappable input, UI scale |
| HDR, high refresh, VRR | SDR-only pipeline, fixed frame pacing | Scene-referred lighting, per-display output transform, VRR-aware frame pacing |
| Mobile (a later consideration, not a first-class target) | an RT-and-mesh-shader-only renderer | RHI capability tiers; the non-RT fallback path is kept working as the **baseline tier**; the cluster DAG can emit traditional LOD meshes at build time; SDL3 covers Android/iOS windowing and touch. A real port would still need a dedicated bandwidth-conscious renderer tier and is not revisited before Phase 7 is complete |
| Arbitrary limits | max lights, bones, entities, texture size as constants | Every limit is a configurable budget with a validator, never a compile-time constant |
| Capabilities a given game does not use (fluids, fire, vehicles, deformable volumes, scent) | Every capability the engine *could* have costs a pass, a tick entry, or a reserved pool in every game | Every capability is roughed in as an abstraction, an asset hook, an LOD policy, and a determinism stance ([05 §5.15](05-simulation.md#515-capability-inventory)), and an unused one costs nothing: systems register only when linked, a system with no instances is not scheduled, and a pass whose inputs do not exist is not in the graph ([11 §11.10](11-performance-principles.md#1110-absent-capabilities-are-free)) |

## 2.8 Adding a capability

[§2.7](#27-generality-what-the-engine-must-not-preclude) says an unused capability must cost nothing; this section says how a *used* one gets added. The engine is open source and aims at AAA quality, so the studios that adopt it will need capabilities it does not have, and the question that decides whether the engine is usable is what happens on that day. The answer is fixed by [ADR-0027](../adr/0027-additive-capabilities.md): a capability is one module (or a small group) in the layer it belongs to, it attaches through the registration points below and through nothing else, and "we need capability X" decomposes into "implement `XSystem`, `XComponent`, a backend, an LOD policy, tests, a bench, and a docs page" rather than "fork the engine and modify six foundational systems".

Two rules keep it honest. The extension surface is conventions plus a small number of constant-initialized registration tables, never virtual interfaces or generic dispatch in a hot loop ([11 §11.4](11-performance-principles.md#114-branch-free-hot-paths-and-constexpr-dispatch)). And the proof of additivity is a build, not a runtime layer: every capability carries an `ENGINE_WITH_<NAME>` option, `ENGINE_MINIMAL=ON` turns all of them off, and CI builds that configuration — a capability that cannot be removed from the build was never additive. Binary plugin ABIs are out of scope; additivity is source-level and resolved by the linker, which is what keeps the cost at zero.

The checklist. `tools/new-capability.ps1` ([08 §8.5](08-toolchain.md#85-build-system-and-ci)) generates every row of it, and the same list is repeated as TODO markers in the generated header, so the contract travels with the code.

| Registration point | File or call | Required? |
|---|---|---|
| The module itself | `<layer>/<name>/CMakeLists.txt`: `engine_module(NAME <name> LAYER <layer> OPTIONAL DEPS ...)`, depending only downward, plus the `add_subdirectory` line in `<layer>/CMakeLists.txt` | Always |
| A capability it cannot be built without | `engine_capability_requires(<name> <other>)` at the top of the same file. The capability is then switched off with what it requires instead of failing the configure, and `modules.json` lists it under `disabled_capabilities` ([08 §8.5](08-toolchain.md#85-build-system-and-ci)) | If it links another capability — anything that ticks links `domain/ecs` |
| Component and event types | `<layer>/<name>/schemas/<name>.schema`, compiled by `engine_schema_library(... CAPABILITY <name>)`; the types register themselves ([ADR-0007](../adr/0007-schema-code-generation.md)) | If the capability has state a document, a save, or the protocol can see |
| A system in the tick scheduler | A constant-initialized `SystemDesc` in the static registration table, placed by tick phase ([§5.2](05-simulation.md#52-sim-scheduler)); `begin_tick`/`tick`/`end_tick` are plain functions, not virtuals | If the capability ticks |
| Data the capability owns outside the ECS | A name registered with `sim::resource_id`, listed in `SystemDesc::reads_resources`/`writes_resources`. A resource conflict is a wave boundary exactly like a component conflict, which is how two systems that share a pool declare their ordering — [§3.4](03-data-model.md#34-the-runtime-world) tells a hot system to own its data, and a component mask cannot name it ([sim](../subsystems/sim.md)) | If two of its systems are ordered by a pool, a buffer or any storage that is not a component |
| Render-graph passes | `gfx::RenderGraph::add_pass()` from the system's frame setup; the graph is never edited to know a pass exists ([§4.2](04-renderer.md#42-frame-architecture)) | If the capability draws or dispatches |
| Derived data | A `derived` step registered with the content build, keyed by the asset property block it consumes ([§7.3](07-content-pipeline.md#73-content-build-the-derived-data-graph)) | If anything must be precomputed from content |
| Protocol methods | `Dispatcher::add(method<P, R, Fn>(name, doc))` from the capability's `register_methods()` ([§6.2](06-agent-tooling.md#62-engine-protocol)) | If agents or the editor drive it |
| Tunables | `tunables::Int/Float/Bool/Enum` at namespace scope, read once outside hot loops ([ADR-0011](../adr/0011-tunables-before-calibration.md)) | For every run-time parameter |
| LOD policy | A tier function of the observer score with hysteresis, owned by the capability ([§5.4](05-simulation.md#54-lod-tier-assignment), [ADR-0010](../adr/0010-deterministic-sim-and-lod-contract.md)) | Always |
| Determinism declaration | `hashed` (in the fixed-step tick and the sim hash) or `derived` (never read back), in the header and on the docs page | Always |
| Zero cost when unused | Which mechanism of [11 §11.10](11-performance-principles.md#1110-absent-capabilities-are-free) applies — no instances, no graph inputs, no asset property block, no linked code — named on the docs page | Always |
| Tests, size table, bench | `tests/<name>_tests.cpp`, `tests/size_table.cpp` ([ADR-0019](../adr/0019-efficiency-first-class.md)), `bench/<name>_bench.cpp` | Tests and size table always; bench when there is a hot path |
| Docs page | `docs/subsystems/<name>.md` plus its row in that directory's README; the subsystem page and the plan status note land in the same change as the code ([AGENTS.md](../../AGENTS.md)) | Always |
| Removal proof | `ENGINE_WITH_<UPPER_NAME>`, created by `OPTIONAL`; the module disappears from `modules.json` in the minimal build and everything else still builds and passes | Always |
| Scene generators | A `GroundProviderDesc` or `PlacementGeneratorDesc` added to `scene_gen::GeneratorRegistry` by a static registrar in the capability's own source; the scene reader and the world ring look it up by the name the scene gives ([ADR-0046](../adr/0046-scene-generators-register-themselves.md), proposed 2026-09-26, not yet built — until it is, a generator that expands inside the scene reader links as [ADR-0037](../adr/0037-scene-reader-links-ruins-where-configured.md) describes) | If the capability generates ground or placements a scene names |

What a capability must never need: edits to `core/`, `foundation/`, the render graph, the scheduler, the content build's driver, or another capability. A capability that genuinely needs one because the registration point does not exist yet extends this table first, as a plan change and an ADR, and lands that before the capability does.

### 2.8.1 The first capability built through the contract

**Status, 2026-09-18.** [`systems/animation`](../subsystems/animation.md) is the first capability built end to end through the table above, and the point of building it was as much to find out what the contract is like from the inside as to get an animation system. What follows is that report, because a contract nobody has used is a proposal.

**What held.** The scaffold (`tools/new-capability.ps1`) produced a module that built and passed as generated, and the shape it generated — one module, `OPTIONAL`, a schema file, a system skeleton, an LOD policy, a determinism stance, a size table, a docs page with a place for the *why* — is the shape the finished capability has. The registration points carried everything they promised to carry:

- **Components from the schema IDL** ([ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 1) cost one `.schema` file and one generated call. The protocol reached them with no per-component code: `WorldCommands::set_json(id, "engine.animation.AnimationPlayer", …)` works because the type system is the schema's, which is the seam's whole argument and it is correct.
- **Systems through `ecs::register_system` with a `sim::SystemDesc`** (seam 2) cost one call each and caught a real mistake during development: the debug check asserted on a write the descriptor did not declare, which is exactly the failure that is invisible at run time.
- **The LOD policy** attached to `sim::TierAssignment` and `sim::MaterializationHooks` without either of them changing, and the capability's own policy is checkable against the engine's reference implementation because `tier_of` is public — which is what [§5.4](05-simulation.md#54-lod-tier-assignment) intended and it works.
- **The removal proof** is real: `ENGINE_MINIMAL=ON` drops the module, its schema library, its tests and its bench, and the rest of the tree builds and passes.

**What needed a seam fixed, and why neither was the capability's fault.** Both landed as their own commits, before the capability, as decision 3 requires.

1. **A capability could not depend on another capability.** [ADR-0027](../adr/0027-additive-capabilities.md) decision 1 says a capability is never a dependency of a module that is not part of the same capability, and that rule was written when no capability ticked. Anything that ticks links `domain/ecs`, which is itself optional — so with `ENGINE_WITH_ECS=OFF` and `ENGINE_WITH_ANIMATION=ON`, `engine_module()` refused and the configure died. That is the case the ADR's own "revisit when" anticipated: *the grouping rule in decision 1 needs to become a declared capability graph*. It is now one — `engine_capability_requires(animation ecs)`, a row in the table above — and a capability with an unsatisfied requirement is switched off with a reason instead of failing a legitimate build.

2. **`domain/anim`'s sampler could not write into a pool.** The module promised that "every function writes into storage the caller already owns", which was true while a caller owned one pose and stops being true the moment a caller owns thousands. The fix is `anim::PoseView`/`ConstPoseView`, three spans over the same three channels, with `Pose` converting implicitly so no call site changed. This is worth naming as a *category*: a domain module written before anything ticked it will tend to own its outputs, and the first capability built on it is where that shows up. It is not a violation of decision 3 — `domain/anim` is neither foundational nor another capability — but it is a diff outside the capability's directory, and the right answer was to land it separately with its own reasoning rather than to copy poses in and out of a scratch buffer forever.

**What the contract still cannot express**, recorded so the next capability does not have to rediscover it:

- **A system cannot declare that it writes data outside the ECS.** `sim::ComponentMask` addresses components, and "the pose pool" is not one — which is awkward precisely because [§3.4](03-data-model.md#34-the-runtime-world) *tells* hot systems to own their own data. Two systems of one capability that share such a structure have no way to declare the dependency between them. `systems/animation` resolved it with a phase boundary, which is honest and happens to be right on its own merits; a capability whose two pool-ordered systems belong in the same phase has only over-declaring on a proxy component, which costs parallelism the schedule cannot explain. The clean fix is a named resource in the descriptor — an opaque id a capability registers and then lists in `reads`/`writes` beside its components — and it belongs to the scheduler's design ([ADR-0028](../adr/0028-ecs-and-persistent-store.md) decision 7) rather than to this table.
- **The materialization hooks and the entity store do not share an identity vocabulary.** `sim::MaterializationHooks` and `sim::EntityRecord` speak `u64`, while [ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 3 says persistent identity is `Id128` and a flecs entity id must not outlive a tick. A capability bridging the two has to choose which the `u64` is, and nothing lets it say which it chose; two capabilities choosing differently would not interoperate. Deciding it is the change that joins `domain/sim`'s hooks table to an ECS world, which [ecs](../subsystems/ecs.md) already lists as not yet done.
- **The scaffold's output is not `clang-format` clean**, because its comment lines are templated on the capability's name and the long ones overflow for a name of the wrong length. Harmless, and worth one line here so the next person does not think they broke something: run `tools/dev.ps1 format` immediately after scaffolding.

**The number that matters for the claim.** Everything outside `systems/animation/` in the capability's own commits is two documentation files and the two seam fixes above, each with its own commit and its own argument. That is what ADR-0027 said a capability should cost, and it is what this one cost.

**Follow-up, 2026-09-18: the three gaps above are closed, and so are two the report did not name.** Each landed as its own commit, and the point of doing them together is that they are all the same kind of thing — a contract that was fine until somebody used it.

- **A system can now declare data outside the ECS.** `sim::SystemDesc` carries `reads_resources`/`writes_resources` over a process-wide registry of names (`sim::resource_id`, `sim::resource_mask`), and the scheduler's wave layering treats a resource conflict exactly like a component conflict — writer against every other toucher, two readers free — with both masks in `schedule_hash()`. The registry is the process's and not a world's because a resource belongs to a *capability* and [sim](../subsystems/sim.md) has no world to ask; 64 of them, one word, stated and counted on overflow. `ecs::register_system`'s debug check learns that resources are **not** query terms — a query has no term for a pose pool, which is the point — and adds the one thing it can check, that every declared id is one the registry handed out. `systems/animation`'s `build_skinning_matrices` is back in `Systems`, ordered after `sample_poses` by `animation.pose_pool`. The report's own worry was the right one: the phase boundary was defensible, but it moved a system for a reason that had nothing to do with which phase it belongs in.
- **The materialization hooks say whose id they carry.** `EntityRecord::entity` is the `Id128` the record came off the store with, `materialize` **returns** the runtime handle the record now has, and `promote`/`demote`/`dematerialize` take a `sim::EntityHandle` — a distinct type, valid only inside the call, which only `domain/ecs` may look inside (`ecs::handle_of`, `ecs::entity_of`, `ecs::handle_for`). [ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 3's rule stops being a convention reviews enforce and becomes awkward to write.
- **The scaffold is `clang-format` clean for a name of any length**, because the wrapping and the include grouping are computed rather than written out, and its test suite now runs clang-format over the output of four scaffolds and asserts it changes nothing.
- **`Vector::resize` grows geometrically** ([containers](../subsystems/containers.md)). The pool the first gap is about was also O(n²) to fill, and `systems/animation` worked around that too — by reserving a doubled capacity by hand. That workaround is gone. The measurement is 2.03 s against 975 µs for 16,384 runs, with the renderer's and geometry's big arrays holding the same bytes as before, and it is worth naming as the same *category* as `domain/anim`'s owned outputs above: a core container written before anything grew an arena through it will tend to take the caller's number literally.
- **The capability graph is exercised by a build.** `msvc-no-ecs` and `linux-clang-no-ecs` are everything on except `ENGINE_WITH_ECS`, so `animation` has to follow `ecs` off through `engine_capability_requires`; CI runs the Linux one. All-on never consults the graph and the minimal build switches everything off for a different reason, so this is the only configuration that would catch a capability that links another and forgot to declare it.

**What the contract still cannot express**, after all that:

- **A relationship has no bit in `sim::ComponentMask`.** `ecs::query_access` counts pair terms and says nothing about them; deciding what a relationship's bit means is the scheduler's design and is the one gap [ADR-0028](../adr/0028-ecs-and-persistent-store.md) seam 2 knowingly leaves.
- **Nothing drives the hooks table from an ECS world yet.** The identity vocabulary is settled and bridged, so what is missing is a hook that *creates* the entity a record names — the persistence layer's, and it arrives with it.
- **`SystemDesc::tiers` and `Determinism` are still recorded and not enforced**, and the executor inside the phases is still flecs' pipeline. Worth knowing for anyone reading a declaration as a guarantee: flecs inserts no sync point for an in-place write to a `$this` term, so what orders two of a capability's systems *today* is that flecs hands each worker the same slice of the same table in consecutive systems. The declaration is what [ADR-0028](../adr/0028-ecs-and-persistent-store.md) decision 7's scheduler will read, and writing it is what turns that from a property of flecs' partitioning into a stated one.

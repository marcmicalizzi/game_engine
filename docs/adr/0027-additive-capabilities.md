# ADR-0027: Capabilities are additive modules: registration points, not forks; the proof is a build, not a runtime layer

- **Status:** Accepted
- **Date:** 2026-09-17
- **Plan references:** docs/plan/02-architecture.md §2.3, §2.5, §2.7 and §2.8, docs/plan/05-simulation.md §5.1, §5.2, §5.4 and §5.15, docs/plan/11-performance-principles.md §11.4 and §11.10, docs/plan/08-toolchain.md §8.5, docs/plan/07-content-pipeline.md §7.3, AGENTS.md. Builds on ADR-0010 (LOD is a system property; the observer function), ADR-0007 (schema code generation), ADR-0019 (size table), ADR-0021 (`engine_module()` manifests), ADR-0014 (Apache-2.0). Supersedes nothing.

## Context

The engine is open source and aims at AAA quality. Every studio that picks it up will need a capability it does not have — a fluid, a vehicle, a wound model, a stylized pass, a scent field — and [05 §5.15](../plan/05-simulation.md#515-capability-inventory) is a list of hooks, not of implementations. What happens on the day a studio needs one decides whether the engine is usable. There are two possible answers and only one of them is a project.

The bad answer is the industry norm: fork the engine, modify six foundational systems to make room for the new one, and carry the diff forward against every upstream release. It punishes the contributor twice — once to build the thing, once a quarter forever — and it guarantees the work never comes back upstream, because a change spread across six foundational files is not a reviewable pull request.

The good answer is that "we need capability X" decomposes into "implement `XSystem`, `XComponent`, a backend, an LOD policy, tests, a bench, and a docs page", all of it new files in one directory, and nothing else in the tree moves. That is only true if the attachment surface is decided in advance and small enough to enumerate. This ADR enumerates it.

Two constraints shape the answer. First, the extension surface is **conventions plus a small number of constant-initialized registration tables**, never virtual interfaces or generic dispatch in a hot loop: [11 §11.4](../plan/11-performance-principles.md#114-branch-free-hot-paths-and-constexpr-dispatch) and [§11.10](../plan/11-performance-principles.md#1110-absent-capabilities-are-free) stand unchanged, and an extension mechanism that costs an indirect call per entity per tick is not an extension mechanism the engine can have. Second, **the proof of additivity is a build, not a runtime layer**: if a capability can be removed from the build and everything else still compiles and passes its tests, it was additive; if it cannot, no amount of interface hygiene makes it so. Binary plugin ABIs are explicitly out of scope — additivity here is source-level, resolved by the linker, which is what keeps the cost at zero.

## Decision

1. **A capability is a module, or a small group of modules, in the layer it belongs to.** It is declared once with `engine_module(NAME <name> LAYER <layer> ...)` ([ADR-0021](0021-build-system-and-tooling.md)), lives in `<layer>/<name>/` with the standard layout, and depends only downward. Most capabilities are `systems/` ([02 §2.3](../plan/02-architecture.md#23-subsystem-boundaries-and-layering)); a capability that is a backend for an existing domain module (a solver, a codec, a rasterization path) belongs in that module's layer. A capability never becomes a dependency of a module that is not itself part of the same capability, because that would make the lower module unbuildable without it.

2. **It attaches through the registration points below and through nothing else.** Each point is a table an existing system reads, a file an existing tool compiles, or an API call the capability makes into a lower layer. None of them is edited by hand to add a capability.

   | Registration point | Mechanism | Required? |
   |---|---|---|
   | Component and event types | `<layer>/<name>/schemas/<name>.schema` compiled by `engine_schema_library(NAME <name>_schemas SCHEMAS schemas/<name>.schema CAPABILITY <name>)`; types register themselves with `schema::Registry::global()` ([ADR-0007](0007-schema-code-generation.md)) | If the capability has state a document, a save, or the protocol can see |
   | A system in the tick scheduler | One constant-initialized `SystemDesc` per system, in a static registration table read by the scheduler at startup, placed by tick phase ([05 §5.2](../plan/05-simulation.md#52-sim-scheduler)); shape in "The registration table" below | If the capability ticks |
   | Render-graph passes | The system calls `gfx::RenderGraph::add_pass()` during its frame setup. The graph is never edited to know about a pass, and a pass whose inputs no producer wrote is not compiled into the frame ([04 §4.2](../plan/04-renderer.md#42-frame-architecture)) | If the capability draws or dispatches |
   | Derived data | A `derived` step registered with the content build's step table, keyed by the asset property block it consumes ([07 §7.3](../plan/07-content-pipeline.md#73-content-build-the-derived-data-graph)) | If the capability needs anything precomputed from content |
   | Protocol methods | `protocol::Dispatcher::add(protocol::method<P, R, Fn>(name, doc))` with schema types for `P` and `R`, from the capability's `register_methods()`, called by whichever app builds the dispatcher ([06 §6.2](../plan/06-agent-tooling.md#62-engine-protocol)) | If agents or the editor drive it |
   | Tunables | `tunables::Int/Float/Bool/Enum` objects at namespace scope, which register in their constructors ([ADR-0011](0011-tunables-before-calibration.md)) | For every run-time parameter; never a hard-coded constant |
   | LOD policy | A tier function of the observer score — the minimum over observers of f(distance, importance, weight) — with hysteresis, owned by the capability ([05 §5.4](../plan/05-simulation.md#54-lod-tier-assignment), [ADR-0010](0010-deterministic-sim-and-lod-contract.md)) | Always. A capability with no LOD policy is not accepted |
   | Determinism declaration | `hashed` (state lives in the fixed-step tick and enters the sim hash, and replays) or `derived` (output that is never read back into gameplay), stated in the header and on the docs page ([ADR-0010](0010-deterministic-sim-and-lod-contract.md)) | Always |
   | Zero-cost-when-unused mechanism | Which of [11 §11.10](../plan/11-performance-principles.md#1110-absent-capabilities-are-free)'s mechanisms makes the capability free when nothing uses it — no instances, no graph inputs, no asset property block, no linked code — named on the docs page | Always |
   | Docs page | `docs/subsystems/<name>.md` and its row in `docs/subsystems/README.md` | Always |
   | Tests, size table, bench | `tests/<name>_tests.cpp`, `tests/size_table.cpp` ([ADR-0019](0019-efficiency-first-class.md)), `bench/<name>_bench.cpp` for anything with a hot path | Tests and size table always; bench when there is a hot path |

   **The registration table.** Systems reach the scheduler as data, not as a base class. One constant-initialized descriptor per system, one static registration object per module, and the linker decides whether it exists:

   ```cpp
   struct SystemDesc {
     const char*   name;          // unique, snake_case, the module name for a single-system capability
     TickPhase     phase;         // Input, EventsIn, Lod, Systems, Physics, PostPhysics, EventsOut, Persist
     ComponentMask reads;         // component types read, and written: the schedule within a
     ComponentMask writes;        // phase is computed from these, in parallel, deterministically
     LodMask       tiers;         // tiers the system runs at (ADR-0010)
     Determinism   determinism;   // Hashed or Derived
     void (*begin_tick)(SystemContext&);   // once per tick, before the parallel phase
     void (*tick)(SystemContext&, Batch);  // per batch, parallel
     void (*end_tick)(SystemContext&);     // once per tick, after
     LodTier (*lod_policy)(const ObserverSet&, EntityHandle);
   };
   ```

   The scheduler does not exist yet — Phase 1 is the renderer core — so this shape is the contract it will be built to consume, and `tools/new-capability.ps1` emits a system skeleton whose members match it field for field. Until then a capability's descriptor fields are documented constants in its header, and turning them into the real table is a mechanical change made once, in the commit that lands the scheduler, not a redesign of every capability. A module whose registration object must survive static linking is declared `WHOLE_ARCHIVE`, the same mechanism the generated schema types already use.

3. **What a capability must never need.** Edits to `core/`, `foundation/`, the render graph, the tick scheduler, the content build's driver, or another capability. Adding a member to a foundational struct, a `case` to a foundational switch, an `#include` of a capability from a lower layer, or a flag that a lower layer branches on: all of these are the thing this ADR exists to prevent, and all of them are visible in review as a diff outside the capability's directory. A capability that genuinely needs one — because the registration point it needs does not exist yet — is a **plan change**: it extends this list of registration points, is argued in the plan and in a new ADR, and lands as its own change before the capability does. That path is open on purpose. What is closed is taking it silently.

4. **The removal proof.** Every capability module carries `OPTIONAL` on its `engine_module()` declaration, which creates `option(ENGINE_WITH_<UPPER_NAME> ... ON)` and skips the module, its tests, and its bench when the option is off; its schema library carries the same switch through `engine_schema_library(... CAPABILITY <name>)`, and a capability whose `CMakeLists.txt` has more to guard asks `engine_capability_enabled()`. The cache variable `ENGINE_MINIMAL=ON` turns every one of them off at once, whatever the individual options say; the `msvc-minimal` and `linux-clang-minimal` presets set it, and CI builds and tests one of them on every push. A disabled module is absent from `build/<preset>/modules.json` — not present and marked dead — and is listed in that file's `disabled_capabilities`, so the module graph stays the honest answer to what this configuration contains. The minimal build is what proves the rest of the tree does not quietly depend on any capability, and it is the same measurement as [11 §11.10](../plan/11-performance-principles.md#1110-absent-capabilities-are-free)'s "nothing enabled" benchmark frame approached from the build side: that frame measures what an unused capability costs at run time, this build proves it costs nothing at link time. Nothing in the tree today is optional — the existing modules are foundations, and a foundation that can be switched off is a foundation nobody can rely on.

5. **The scaffold is the entry point.** `tools/new-capability.ps1 -Name <name> -Layer <layer> [-Deps ...] [-WithSchema] [-WithBench] [-WithProtocol]` generates the module, the system skeleton with the registration points as a checklist in the header, the tests, the size table, the docs page, the schema stub, the bench, and the `add_subdirectory` line, and refuses to overwrite anything ([08 §8.5](../plan/08-toolchain.md#85-build-system-and-ci)). The checklist in the generated header is the same list as decision 2, so the contract travels with the code rather than living only here. A capability that was not scaffolded is fine as long as it matches what the scaffold would have produced; the scaffold exists so that matching it is easier than not.

6. **Contribution norms.** Until the repository goes public and these become `CONTRIBUTING.md`: one capability per pull request; the docs page, the tests, and the size-table entries land in the same change as the code, never as a follow-up; `AGENTS.md`'s conventions apply to humans exactly as they apply to agents; a change outside the capability's own directory is explained in the pull request description or removed; performance claims come with before and after numbers ([11 §11.8](../plan/11-performance-principles.md#118-measure-before-and-after)); and the project treats a contributed capability as something it now supports, which is the other half of the bargain and the reason the first half is worth accepting.

## Consequences

The engine's extension story becomes a checklist a competent contributor can follow without reading the whole plan, and the review question for a new capability becomes mechanical: does anything outside its directory change, and does the minimal build still pass. Both are visible in the diff and in CI, so neither depends on a reviewer's judgment about architecture.

The registration points are now a compatibility surface. Their shapes — `SystemDesc`, the graph's pass API, the content build's step table, the method table — are what third-party capabilities compile against, so changing one of them breaks capabilities the project does not own. They get the same treatment as the schema evolution rules of [03 §3.8](../plan/03-data-model.md#38-schema-evolution): additive changes are free, and anything else is an ADR with a migration note. The cost of that promise is paid early, while the surfaces are cheap to get right, which is why this ADR lands before the scheduler rather than after it.

CI gains one configuration. Today it is a duplicate of the debug build, because nothing is optional yet; it earns its place the moment the first capability lands, and it is cheaper to have the job running from the start than to discover later that four capabilities each assumed another one was present.

What is now forbidden: a capability that a foundational module links against; a virtual interface or a `std::function` in a per-entity or per-frame path as the extension mechanism; a runtime "is capability X present" flag that any hot loop reads; a capability with no LOD policy, no determinism stance, or no `docs/subsystems` page; and making an existing foundational module optional in order to make a dependency on it legal.

## Revisit when

A capability is needed that genuinely cannot attach through any registration point here and the missing point is worth adding (the expected case, handled by decision 3); the scheduler lands and `SystemDesc` needs a shape the skeleton does not have; a studio needs binary plugins badly enough to pay for an ABI, which is a different ADR and a different set of costs; or the minimal build stops being a reliable proof because capabilities have started depending on each other, at which point the grouping rule in decision 1 needs to become a declared capability graph.

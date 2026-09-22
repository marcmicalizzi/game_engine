# Agent-Native High-Performance Game Engine — Technical Plan (First Pass)

Status: first-pass plan, September 2026, revised after the project owner's clarifications on motivation, platform, language, license, and AI-usage constraints. Nothing here is implemented. Third-party facts were verified against upstream sources on 12 September 2026; everything else is judgment to be tested by the experiments in [10 §10.5](10-roadmap-risks.md#105-experiments-to-run-before-committing).

## How to read this

Read [01-critique](01-critique.md) first; it explains why the rest is shaped the way it is. Then [02-architecture](02-architecture.md) and [03-data-model](03-data-model.md), which carry the decisions that are hardest to reverse. [11-performance-principles](11-performance-principles.md) and [12-ai-usage-policy](12-ai-usage-policy.md) are standing rules every contributor applies. The remaining sections can be read independently.

| Document | Covers |
|---|---|
| [01-critique](01-critique.md) | What the brief gets right, what is re-scoped, coupling errors, missing subsystems, unnecessary complexity, the three decisions that must be right early |
| [02-architecture](02-architecture.md) | System shape, process model, subsystem boundaries and layering, frame and time model, repository organization for agent comprehension, scope, what the engine must not preclude, how a studio adds a capability without forking |
| [03-data-model](03-data-model.md) | Identity, authoring document, transactions and merges, runtime ECS, event-sourced persistent state, the dependency/invalidation substrate, spatial partition, schema evolution, canon |
| [04-renderer](04-renderer.md) | Frame architecture, cluster geometry, ray tracing and acceleration structures, destruction, extreme displays and attention-region foveation, UI coordinates, reference renderer, streaming, reuse vs. build |
| [05-simulation](05-simulation.md) | Scheduler, timing wheel, LOD tiers, tile reconciliation, NPC scale, world events and loop prevention, destruction, story protection, determinism, multiplayer readiness, deformable surfaces and soft bodies, deformable volumes, the capability inventory, characters at run time |
| [06-agent-tooling](06-agent-tooling.md) | Engine protocol, MCP bridge, transactions, permissions and leases, introspection, edit context and world mips, task ledger, day-one operations, roles and human review, playtesting, hierarchical world generation, human developer tooling |
| [07-content-pipeline](07-content-pipeline.md) | Formats, content build, validation rules, provenance, procedural and ML generation, characters, runtime LLM interface and long-tail authoring, localization |
| [08-toolchain](08-toolchain.md) | Language, scripting, graphics API, shaders, build system, dependency table with licenses, hardware characterization scaled to fit, engine license and dependency policy |
| [09-testing-profiling](09-testing-profiling.md) | Test taxonomy, determinism tooling, profiling infrastructure, benchmark corpus, agent-facing test surface, deformable-volume experiments, character validators |
| [10-roadmap-risks](10-roadmap-risks.md) | Phases with exit criteria, ordering, risks, experiments, deferred decisions, ADRs to record now |
| [11-performance-principles](11-performance-principles.md) | Budgets as gates, data layout and footprint, the engine container set, prefetcher-friendly access order, branch-free `constexpr` dispatch, pinned cache-domain thread pools, no hidden limits, GPU rules, measurement discipline, performance by default for engine users, absent capabilities are free |
| [12-ai-usage-policy](12-ai-usage-policy.md) | Rules keeping every use of AI inside provider terms: agent outputs are content and code, never training data; runtime models are unmodified third-party open weights |
| [13-reference-consumer-games](13-reference-consumer-games.md) | Two complementary consumer games: Desert Survival (sparse, effectively infinite, deforming terrain, extreme view distances) and Island City (finite, extreme density, functional interiors, crowds, consequences); the requirements they add, roadmap mapping, experiments E13–E18, and the reference interactions a third-party game would need |

How this repository goes public, and how private and experimental work continues beside it once it is, is [docs/publishing.md](../publishing.md): a public repository has no private branches, so private capabilities and content live in an additive overlay repository ([ADR-0027](../adr/0027-additive-capabilities.md)) and unreleased engine work lives in a private mirror that publishes by pull request.

How generated content is made today — the tool-level generator service of [07 §7.7](07-content-pipeline.md#77-ml-asset-generation-novelty-content), its backends, its provenance sidecar, the credit rules for paid services, and the rule that generated outputs are never committed — is [docs/content-generation.md](../content-generation.md).

Experiment write-ups live in [docs/experiments](../experiments/README.md), which indexes them and carries the skeleton every new one starts from — including the mandatory "Machine state" line: [E1 raster crossover](../experiments/e1-raster-crossover.md), [E2 cluster acceleration structures](../experiments/e2-cluster-acceleration.md), [E6 ECS and persistent store](../experiments/e6-ecs-store.md), [E10 generated props](../experiments/e10-generated-props.md), [E19 lattice cage](../experiments/e19-lattice-cage.md), [E25 deformed clusters](../experiments/e25-deformed-clusters.md).

## Executive summary

The brief's four priorities are sound, and the agent-native pillar is the genuinely new thing. The motivations behind its two most heavily worded sections, microarchitectural optimization and extreme-resolution robustness, are correct and grounded in real experience: engines that ship features at unplayable frame rates, games that render nothing past 8192 pixels or lose their UI past 4096, and a 20–30× kernel speedup that came from traversal order alone. Those motivations are adopted as engine coding standards. What the plan changes is the mechanism: rules, a tunables registry, and nightly extreme-resolution CI instead of a general calibration system and a family of compact data types.

The three decisions that determine whether agents can actually build a game with the engine, and that are hardest to reverse:

1. **The authoring data model must be text, layered, stable-ID'd, and diffable.** Canonical JSON in git, USD-like layers without USD's composition engine, structural diff and three-way merge keyed by object ID. Binary levels make every agent workflow in the brief impossible.
2. **One incremental dependency substrate** (content-addressed, tracked reads, two node classes: `derived` auto-rebuilds, `authored` is flagged for review) serves both the asset pipeline and canon/world-gen invalidation. Retrofitting this is a rewrite.
3. **The engine is a headless server.** Sim-only, offscreen-GPU, or windowed, driven by a versioned JSON-RPC protocol. The GUI editor (a complete manual development environment) and the MCP bridge are equal clients, and the headless mode is a dedicated server when multiplayer arrives.

Other conclusions:

- **Geometry and ray tracing are one decision, not two.** The cluster is the single unit of rasterization, streaming, and acceleration-structure construction.
- **Vulkan primary, Windows first, Linux second.** A close call: cross-vendor cluster acceleration structures exist only on D3D12's roadmap, but Linux is a real target, the development GPU (RTX 5090) gets cluster RT on Vulkan today, and the fallback path for AMD/Intel is needed regardless. The RHI stays API-neutral so a D3D12 backend can be added if the cross-vendor case comes to matter.
- **Real-time target is hybrid, not path traced.** Rasterized primary visibility through clusters; in-house ReSTIR direct and indirect lighting with a radiance cache; a full path tracer only as the reference, from the first renderer milestone.
- **Extreme displays**: multi-view rendering with per-monitor frusta, plus a user-configurable attention region (default: the center monitor) outside of which quality drops aggressively, because surround players use the side monitors as peripheral vision. 11520×2160 on a 5090 targets 60 fps with a 30 fps floor.
- **Apache-2.0 engine license**, attribution-only, never GPL. Consequence: no copyleft dependencies, and proprietary vendor SDKs (RTXDI, NRD, DLSS/FSR binaries) become optional developer-supplied plugins rather than engine dependencies.
- **Efficiency is first-class and the engine's job.** Footprint is measured like time: a size table for hot types, promote-on-demand objects, one engine container set (sorted-vector maps, open-addressing hash maps, slot maps) with node-based standard containers banned in engine code. The engine's fast path is its easy path, so that a shipped game's performance losses come from its own content and code, and the profiler shows exactly which.
- **C++20, decided.** Schema code generation replaces the reflection C++ does not yet have; sanitizers, fuzzing, and strict layering carry the memory-safety burden for agent-written code.
- **Persistent state is event-sourced in SQLite.** Agents and tests query world state with SQL; saves are snapshot plus tail; non-determinism is logged and replayed, never re-executed.
- **Simulation LOD is a property of systems, not entities**, via a materialization contract; temporal LOD is a hierarchical timing wheel with mandatory `SummarizeInterval` for fast-forward.
- **Agent context is the brief's mipmapped world plus an edit cell with an immutable, full-detail overlap ring**, assembled to a token budget.
- **AI usage stays well inside provider terms.** Development-time agent outputs are content and code, never training data for any model; the optional runtime model is an unmodified third-party open-weights model, on GPU when VRAM allows and otherwise on efficiency cores; the game is complete without it.
- **The world reacts physically by default.** Destruction is on unless opted out per object, per material, or globally. Snow, sand, and mud deform through a per-tile deformation layer stamped by a GPU pass and mirrored by a coarse deterministic CPU grid for gameplay. Soft bodies run in Jolt for gameplay and on the GPU for cloth and hair.
- **Volumetric deformation is a general primitive, not a character effect** (ADR-0026). An asset carries a coarse simulation cage that is never the render mesh, the solver behind it is abstracted with Jolt's XPBD first, simulation LOD is mandatory, gameplay-relevant cage state stays deterministic on the CPU, and damage is expressed as constraint edits on the cage ([05 §5.14](05-simulation.md#514-deformable-volumes)).
- **Every simulation capability of any value is roughed in, and an unused one costs nothing.** The abstraction, the asset hook, the LOD policy, and the determinism stance exist for each of the twenty-six capabilities in the inventory ([05 §5.15](05-simulation.md#515-capability-inventory)) so that none of them needs the systems around it rescaffolded later; roughing in is the interface and the data slot, not the implementation. The rule that makes the generality affordable is that a capability with no instances has no pass, no dispatch, no allocation, no per-frame work, and ideally no linked code ([11 §11.10](11-performance-principles.md#1110-absent-capabilities-are-free)).
- **A character is a parameter vector over a canonical topology, and none of it is mandatory** (ADR-0032, ADR-0033). One base mesh per family rather than one per sex, a declarative parameter rig between the game's named parameters and the engine's deformation channels, macro axes that interpolate across sculpted corners because they interact, a joint regressor instead of per-channel joint deltas, modules that replace a region of a body through the same section mask garments use, and a bake keyed by the parameter hash so the cluster DAG is per base and a character is a vector plus a cached stream. A character may equally be a plain skinned glTF with none of it, and a game that never enables the capability pays nothing. Validation is numeric and sweeps the parameter space; what may appear in any given output is a content class per category on the asset (`nudity`, `sexual` and `violence` are independent, never one scale) and a per-category cap on the output ([07 §7.11](07-content-pipeline.md#711-characters), [05 §5.16](05-simulation.md#516-characters-at-run-time)).
- **Multiplayer is kept open, not built**: deterministic fixed-step sim, an observer set instead of "the player," schema slots for replication annotations, headless mode as a dedicated server.
- **Mobile is a later consideration only**, not a first-class target. The architecture does not preclude it, but a port would need its own bandwidth-conscious renderer tier; the non-RT fallback path is kept working as the baseline tier partly for that reason.
- **Timeline**: 30–48 focused weeks with agents at full budget (7–11 months of continuous effort), or 2–3 years at side-project pacing. Estimates are recalibrated against actuals after Phases 0 and 1; the real bounds are human review bandwidth and a single GPU.

## Headline decisions

| Decision | Choice | Confidence | Confirm by |
|---|---|---|---|
| Source of truth | Text, layered, stable-ID document; canonical JSON | High | E4 |
| Persistent state | Event log + projections + snapshots in SQLite | High | E6 |
| Dependency tracking | One substrate, `derived` vs `authored` nodes, tracked reads | High | — |
| Process model | Headless engine server; editor and MCP are clients | High | E5 |
| Geometry | Cluster DAG LOD; mesh shader + software raster; visibility buffer | High | E1 |
| RT strategy | Hybrid; cluster AS where available (NVIDIA today); static multi-LOD BLAS fallback as first-class | Medium | E2 |
| Graphics API | Vulkan primary, thin API-neutral RHI; D3D12 deferred | Medium | E2 |
| Platforms | Windows first, Linux second, CI on both from Phase 1 | High | — |
| Core language | C++20 with sanitizers, codegen, strict layering | Decided | defect-rate tracking |
| Engine license | Apache-2.0 | Decided | — |
| Mobile | Later consideration only; baseline tier kept working | Decided | — |
| Efficiency | Size table, engine container set, banned node containers, fast-path-is-easy-path APIs | Decided | size table and lint in CI |
| Shaders | Slang | High | — |
| ECS | flecs (evaluate), hot systems own their data | Medium | E6 |
| Physics / nav | Jolt / Recast | High | E11 |
| Scripting | Deferred: C++ modules vs Luau vs C#; declarative data first | — | E7 |
| Hardware calibration | Detection, pinning, tunables now; calibration runner only with evidence | High | E3 |
| Runtime LLM | Tier 0 only for the first game; policy in 12 | High | E8 |
| Multiplayer | Readiness rules now; transport and replication later | High | — |

## What the brief asked for, and where it is answered

| Requested output | Location |
|---|---|
| 1 Critique of concepts | 01 |
| 2 Missing requirements and problems | 01 §1.6 |
| 3 High-level architecture | 02 |
| 4 Subsystem boundaries | 02 §2.3 |
| 5 Core data/world model | 03 |
| 6 Renderer architecture | 04 |
| 7 Simulation architecture | 05 |
| 8 Agent/developer-tool architecture | 06 |
| 9 Asset/content pipeline | 07 |
| 10 Build/toolchain | 08 §8.1–8.5 |
| 11 Dependency candidates | 08 §8.6, 8.8 |
| 12 Testing/profiling | 09, 11 |
| 13 Staged roadmap | 10 §10.2 |
| 14 Major risks | 10 §10.4 |
| 15 Experiments | 10 §10.5 |
| 16 Decisions not to make yet | 10 §10.6 |
| 17 Ordering/dependencies | 10 §10.3 |

## Immediate next steps

1. Turn the nineteen items in [10 §10.7](10-roadmap-risks.md#107-decisions-to-record-as-adrs-now) into ADRs under `docs/adr/`.
2. Initialize the repository skeleton from [02 §2.5](02-architecture.md#25-repository-organization-for-agent-comprehension) and start Phase 0 with the schema code generator, `core/containers` with the size table, and the document model, since everything else depends on them.
3. Schedule experiments E1 and E2 for the first weeks of Phase 1.
4. Record estimate versus actual for Phase 0 in the task ledger and rescale the roadmap from it ([10 §10.8](10-roadmap-risks.md#108-calibrating-the-estimates)).

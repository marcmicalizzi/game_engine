# 10 — Roadmap, Risks, Experiments, and Deferred Decisions

## 10.1 Staging principles

- Every phase ends with a demo that runs headless and windowed, a green test suite, and a batch of ADRs.
- Agent access is built in each phase for that phase's systems, never deferred to a later "tooling phase".
- No phase depends on runtime LLM tiers above 0.
- Linux builds and tests run in CI from Phase 1, so parity gaps are visible as they appear rather than discovered at the end.
- This is a long-running side project; phases are sized so that each leaves something working and documented if work pauses for months.
- Estimates are given in **focused weeks**: wall-clock time with agents working continuously against the full weekly token budget and the project owner reviewing daily. Side-project pacing stretches the same work over roughly 2–3 years. An earlier draft gave figures 2–3× longer; the owner's experience is that agent-driven work lands well under estimates of that kind, so these are deliberately tighter and will be recalibrated against actuals after Phase 0 and Phase 1 ([§10.8](#108-calibrating-the-estimates)). Rendering phases keep wider ranges because each iteration needs GPU time on a single machine plus visual judgment; foundations, tooling, validators, and content compress the most.

## 10.2 Phases

**Phase 0 — Foundations (2–4 focused weeks)**
Core: platform, memory, containers, math, topology-aware jobs, logging and telemetry, schema IDL and code generation (C++, JSON, JSON Schema, docs), serialization, IDs, hashing. Foundation: VFS and async I/O, tunables registry and bench harness, Tracy, protocol skeleton (JSON-RPC, sessions), headless runner, test infrastructure, CI (build, test, sanitizers), repository conventions (`MODULE.toml`, layer check), `AGENTS.md`. Document model v0: objects, layers, transactions, structural diff.
Exit: an agent opens a session, creates objects, commits, diffs, and rolls back through `engine-cli`. **Met 2026-09-15** by the end-to-end suite in `apps/engine_cli/tests`, one process per step; remaining Phase 0 items (Tracy, CI on both platforms, the schemac negative-test harness) follow.

**Phase 1 — Renderer core (6–10 focused weeks)**
Thin RHI over Vulkan 1.3/1.4, render graph, bindless, GPU-resident scene; Windows and Linux CI from the first commit of this phase. Cluster build pipeline (meshoptimizer clusterlod) → pages → streaming. GPU culling and LOD selection; mesh-shader visibility buffer (the software rasterizer exists but stays off the critical path after E1, ADR-0024); material resolve; simple direct lighting. The per-frame deformed-vertex pool for skinned and cage-deformed clusters ([04 §4.3](04-renderer.md#43-geometry)) with the **E25 spike** of the cage-to-cluster transfer path, because the pool's layout and the per-vertex binding slot in the cluster pages cannot be retrofitted once assets are built. Reference GPU path tracer with shared BSDFs; FLIP harness. `ViewSet` multi-view; extreme-resolution CI; capture API with entity-ID buffer. Experiments E1, E2 (month 1), E9, E25 (spike).
Exit: test scenes render through clusters at 4K and 11520×2160; reference comparisons run nightly; agents can capture and benchmark. **"Agents can capture and benchmark" met 2026-09-17**: the renderer left `apps/engine_view/main.cpp` for `systems/renderer`, and engine-host became its second consumer with `render.load`, `render.capture` (color, the entity-ID buffer, depth, normals), `render.benchmark` (GPU milliseconds per pass from the timers, CPU milliseconds per frame, visible pairs), and `render.compare` (FLIP, PSNR, SSIM) — offscreen, with no window in the process, and skipping with a distinct protocol error where there is no GPU, so the same suite runs on the hosted runners and on the GPU machines ([subsystems/renderer](../subsystems/renderer.md), [apps](../subsystems/apps.md)). The remaining two exit clauses need the reference path tracer and the scene corpus of [§4.8](04-renderer.md#48-reference-renderer-and-objective-optimization), and the nightly job that runs them.

**Phase 2 — RT lighting (6–10 focused weeks, overlaps Phase 3)**
AS management and BLAS budget manager; cluster-AS path (`VK_NV_cluster_acceleration_structure`) plus the first-class fallback; in-house ReSTIR DI; GI cache and ReSTIR GI; RT reflections and shadows; in-house baseline denoiser; plugin interface for vendor upscalers and neural reconstruction; ray budget scheduler; attention-region foveation and per-view VRS.
Exit: hybrid RT at the targets in [04 §4.1](04-renderer.md#41-goals-targets-non-goals) on at least NVIDIA hardware.

**Phase 3 — World and simulation core (4–8 focused weeks, parallel with Phase 2)**
ECS (flecs evaluation → decision), persistent store (SQLite event log, projections, snapshots), sim scheduler, timing wheel, LOD framework and materialization contract, tile streaming, Jolt, Recast, basic animation (standard skeleton, clips, blending), audio, input and replay, save/load with migration corpus, world-mutation guard, dependency substrate v1 for the content build. Experiment E6.
Exit: a tile-streamed world with 10^5 scheduled NPCs, deterministic replay, save and load.

**Phase 4 — Agent API v1 and editor (3–5 focused weeks, parallel)**
MCP bridge, the full day-one operation set, validation service, describe and query, semantic and visual mips v0, task ledger, editor client with review queue, first single-agent loop building a small scene end to end. Experiment E5.
Exit: an agent constructs a small level from a text brief using only structured feedback and captures; a human reviews through the editor queue.

**Phase 5 — Content pipeline and generation (4–6 focused weeks)**
glTF import, validators, provenance, procedural generators (terrain, scatter, roads, building kits), generator-service interface with first ML backends, the character standard (skeleton, body, slots), TTS. Experiments E10 and E7 (scripting decision).
Exit: agents produce a validated environment from generator rules and generated props.

**Phase 6 — Destruction, simulation depth, NPC routines (4–6 focused weeks)**
Support graphs, pre-fracture build, collapse simulation, damage-state persistence, nav rebuild and region graph, NPC routines at all tiers, economy and faction event consumers, story-protection policies. Deformable surfaces v1 (snow and sand deformation maps with decay and the coarse CPU grid), gameplay soft bodies via Jolt XPBD, cosmetic GPU cloth ([05 §5.13](05-simulation.md#513-deformable-surfaces-and-soft-bodies)). **Deformable volumes v1** ([05 §5.14](05-simulation.md#514-deformable-volumes), ADR-0026): SDF lattice cages as a content-build `derived` step with authored overrides, the Jolt XPBD backend behind the engine's own abstraction, attachment and layers, soft-against-rigid and soft-against-own-skeleton contact, the simulation LOD tiers with hysteresis and blending, damage as constraint edits on the cage, adhesive contact, and strain-driven surface detail. This phase is where physics and animation depth land, and the deformation work belongs with destruction because they share the constraint-edit representation. Experiments E19–E24 and E26.
Exit: destroy a building; NPCs and quests respond; leave and return; state persists correctly. Walk through snow and it stays trampled until it refills. Walk across a dune crest and it slumps; leave for a game-week and the dune has moved ([13 §13.5](13-reference-consumer-games.md#135-mapping-to-the-roadmap)). Press a creature against a rock and its flesh spreads and recovers; walk a crowd of them past every simulation LOD tier boundary without a visible pop; reach into a cavity in a wall and the tissue resists, clings on the way out, and settles back ([13 §13.7](13-reference-consumer-games.md#137-reference-interactions)).

**Phase 7 — The small game (8–12 focused weeks; bounded by human review bandwidth)**
The desert-survival consumer game ([13 Consumer A](13-reference-consumer-games.md)) ships first as the Phase 6 exit demo made playable: seeded runs, score, leaderboard-ready replays. Then thesis → world → spine → regions with the multi-role agent pipeline; playtest bots at scale; polish; ship a 1–3 hour game with runtime LLM at Tier 0 and telemetry for the long tail.
Exit: playable, reviewed, benchmarked.

Rough total: 30–48 focused weeks with Phases 2–4 overlapping (7–11 months of continuous full-budget effort), or 2–3 years at side-project pacing.

## 10.3 Ordering and dependencies

```
P0 foundations ──┬─► P1 renderer core ──► P2 RT lighting ─────────────┐
                 ├─► P3 world/sim core ───────────────────┬─► P6 destruction/NPC ─► P7 game
                 └─► P4 agent API (doc model from P0; grows with P1/P3) ─┤
                                    P5 content pipeline (P1 clusters, P3 DDC, P4 API) ─┘
```

Critical paths: P0 → P1 → P2 → P7 for fidelity; P0 → P3 → P6 → P7 for the systemic world. P4 must be usable by the end of P1, or agents cannot help build P2 and P3.

## 10.4 Major technical risks

| Risk | Impact | Mitigation |
|---|---|---|
| Cluster geometry ↔ RT integration (LOD mismatch, AS cost, vendor lock) | The core renderer promise | Resolved for NVIDIA by E2 (ADR-0025): cluster acceleration structures from the frame's cut, pixel-identical to raster, 3× less memory than KHR structures; the KHR fallback is built and tested for every other RT GPU; cluster format encodable to future formats |
| Software rasterizer plus mesh-shader pipeline complexity | Phase 1 slip | Resolved by E1 (ADR-0024): hardware-only in Phase 1; the software rasterizer is an experimental path with a test that keeps it pixel-compatible |
| Agent-written C++ defect rate | Everything | Sanitizers, fuzzing, codegen, small modules, strict layering; measure per phase; Rust as the fallback |
| Engine generality without a game | Wasted systems | The two consumer games in [13](13-reference-consumer-games.md) are the concrete consumers (Desert Survival is the recommended first shippable; Island City the density benchmark); Phases 6–7 drive requirements; no system without a consumer in the roadmap |
| ML asset quality and pass rate | Content phase | E10 early; validate-and-repair pipeline; procedural for volume; character standardization |
| Creative coherence | Game quality | Canon graph, validators, review roles, human gates; measure review rejection rates |
| Proprietary vendor SDKs vs. the permissive engine license | Distribution | In-house ReSTIR and denoiser; vendor SDKs as developer-supplied plugins ([08 §8.8](08-toolchain.md#88-engine-license-and-dependency-policy)) |
| Cross-vendor cluster AS may never come to Vulkan | AMD/Intel get fallback-quality RT geometry | Fallback path is first-class; D3D12 backend trigger in [08 §8.3](08-toolchain.md#83-graphics-api) |
| Performance-regression noise on shared GPUs | False alarms, ignored alerts | Dedicated runners, medians, significance tests |
| Persistence and migration debt | Save compatibility | Migration corpus from Phase 3 |
| Runtime LLM cost and latency | Feature creep | Tier 0 mandatory; telemetry-driven compile-to-content |
| Windows/Linux feature drift | Linux parity | Vulkan on both; CI on both from Phase 1; capability tiers make gaps explicit rather than silent |
| Deformable cage authoring cost | Content throughput; the primitive is worthless if every asset needs a simulation expert | Cages are generated from the mesh's signed distance field at content build, with authored overrides only where the default is wrong ([07 §7.10](07-content-pipeline.md#710-deformable-volume-assets)); validators name the failing vertices, attachments, and layers instead of failing vaguely; the character standard ([07 §7.7](07-content-pipeline.md#77-ml-asset-generation-novelty-content)) means one cage per body archetype is retargeted rather than authored per character; E19 and E23 measure how far the defaults get before an override is needed |
| Soft-against-soft contact cost | Crowds of deformable characters; any two-creature interaction | Out of v1 by decision (ADR-0026): v1 is soft-against-rigid and soft-against-own-skeleton, which already covers grabbing, pressing, impact, and contact with the world. Pair cost is quadratic in the interacting set, so it gets its own experiment and its own budget before it is built ([§10.6](#106-decisions-deliberately-deferred)) |

## 10.5 Experiments to run before committing

| ID | Question | Method | Decides |
|---|---|---|---|
| E1 | Where is the hardware-vs-software raster crossover for small clusters on target GPUs? | Prototype both on synthetic cluster sets; measure at 4K and 11520×2160 | Whether software raster is in Phase 1. **Done 2026-09-16** ([results](../experiments/e1-raster-crossover.md), ADR-0024): no crossover down to 0.7 px triangles on the RTX 5090; hardware wins 1.5–3×; Phase 1 is hardware-only, the software rasterizer stays experimental |
| E2 | Cluster AS on D3D12 (NVAPI) vs Vulkan (NV extension): build cost per frame, memory, image match against raster | One-month spike from `vk_lod_clusters` and RTXMG samples at Phase 1 start | Graphics API; AS strategy. **Done 2026-09-17** ([results](../experiments/e2-cluster-acceleration.md), ADR-0025): the Vulkan extension works through ray queries with no API change; cluster structures take 2.9–3.3× less memory and build 1.2–2.6× faster than KHR structures for the same cut on the RTX 5090, trace at the same speed, and match the raster picture to the pixel; per-frame GPU-driven builds are the next step |
| E3 | Do runtime-selected kernel variants beat one good default by more than 10% across two or more machines? | Tunables harness on culling, transform, and animation kernels | Whether calibration Stage 3 is built |
| E4 | Canonical-JSON layered document vs LightUSD as authoring format: agent edit, diff, merge round trips | An agent performs 20 edits and 3 merges; measure errors and diff readability | Document format |
| E5 | Can one agent build a small level using structured feedback and captures only? | Phase 4 exit demo; count tool calls, errors, human interventions | Agent API gaps |
| E6 | flecs at 10^5 entities with relationships plus 10^6 LOD3 records in SQLite | Synthetic sim; measure tick time, memory, query latency | ECS choice; store design. **Measured 2026-09-17, decision pending** ([results](../experiments/e6-ecs-store.md), draft [ADR-0028](../adr/0028-ecs-and-persistent-store.md), status Proposed): flecs holds 10^5 entities with relationships and a hierarchy at 2.79 ms a tick single-threaded, 1.63 ms at four workers, 162 B an entity; SQLite holds 10^6 LOD3 records in a 245 MB file, 22–35 µs a point lookup, 317–442 µs a tile, 61.7 k event appends a second. Parallel scaling stops at four workers and the spatial-query answer is a relationship, not a component |
| E7 | The same gameplay feature in C++ hot-reload, Luau, and C#: agent error rate, iteration time, performance | Three agent-written implementations | Scripting layer |
| E8 | A 1–3B parameter quantized open-weights model on efficiency cores or spare VRAM while rendering: latency, throughput, intent accuracy, effect on frame time | llama.cpp harness inside the engine | Whether Tier 1 is worth building |
| E9 | Three-view surround vs Panini projection at 11520×2160: cost, TAA and denoiser behavior, per-view VRS gains | Phase 1 test scenes | Multi-view design |
| E10 | Pass rate of ML-generated props through validators; repair yield | 200 generated props | Generation pipeline design |
| E11 | Recast tile rebuild latency on modern hardware at the chosen tile size under destruction load | Benchmark | Nav rebuild budget |
| E12 | Structural three-way merge of proposal layers: conflict rate with tile leases | Simulated multi-agent edits | Concurrency model |
| E13–E18 | Granular relaxation cost, dune-migration fast-forward accuracy, GI stability under a moving sun, interior materialization tiers, RT relevance through windows, procedural building grammar yield | See [13 §13.6](13-reference-consumer-games.md#136-additional-experiments) | Deformation grid resolution; slow-process summarization; temporal reuse policy; interior cell model; RT geometry LOD policy; procedural architecture pipeline |
| E19 | Does an SDF-generated lattice cage around a rigid internal structure stay stable and preserve volume under sustained compression, and at what cost per tick? | Press a 300–800-element cage around a two-bone core to 30% of its depth, hold, release; strain, volume, recovery, µs per tick ([09 §9.6](09-testing-profiling.md#96-deformable-volume-experiments)) | Whether the lattice is the v1 cage kind; the default element budget, sub-step, and iteration counts |
| E20 | Does secondary soft-tissue motion driven from the skeleton read correctly under locomotion, without ringing or self-intersection, at 60 Hz and at 30 Hz? | The standard skeleton's locomotion clip set with a bound volume; lag, settle, and interpenetration metrics | Attachment stiffness defaults; sub-step count; whether the tick rate is a content-visible parameter |
| E21 | Is localized contact and recovery under a rigid grip stable over repeated grips, and does the deformation stay local? | A rigid hand collider gripping and releasing a soft prop 20 times over 10 s | Contact model and iteration budget; the T0 interaction radius |
| E22 | Does a volume under sustained rigid load creep, oscillate, or penetrate? | A cushion under 1× and 5× its own mass for 60 s | Damping and volume-preservation defaults; whether resting volumes may sleep |
| E23 | How far apart can layer stiffnesses be before XPBD conditioning fails at the iteration budget? | Three bonded layers at 1:10:100 and 1:100:10,000 compliance ratios under poke, press, and shear | The layer stiffness-ratio validator limit in [07 §7.10](07-content-pipeline.md#710-deformable-volume-assets); whether any layer needs FEM |
| E24 | Do the simulation LOD tiers hold the CPU budget for a crowd, and can a tier change be made invisible? | 64 deformable characters over a scripted path crossing every tier boundary; cost by tier population, pop magnitude, thrash rate | Tier thresholds, hysteresis bands, blend length; the T0 entity count |
| E25 | Does cage-to-surface transfer cost the LOD cut rather than the source mesh, and do deformed cluster templates hold the RT budget? | An 8M-triangle asset on a 512-element cage from 0.5 m to 50 m; transfer ms against cut size, template instantiation ms and memory, crack and FLIP checks | The per-frame deformed-vertex pool layout and the per-vertex binding slot in the cluster pages. Runs as a **Phase 1 spike** because it constrains both. **Renderer half done 2026-09-17** ([results](../experiments/e25-deformed-clusters.md)): the pool pass costs **0.9 µs per 1,000 visible clusters on a 9.4 µs floor and agrees to 1% between a 94k-triangle mesh and a 2.1M-triangle one**, so the cost follows the cut; instantiating cluster templates is 3–5× cheaper per frame than rebuilding the same cut, for 537 B of resident template per cluster (about a third of a CLAS); the ray-traced and rasterized pictures agree under deformation, and instantiated templates trace a picture identical to rebuilt structures. The finding that constrains the asset side: **a deformer that reads a vertex normal tears the surface at hard edges and LOD-group boundaries**, so the per-vertex cage binding must be derived from position alone. The cage, the solver, the strain channel, and the FLIP and budget criteria stay open |
| E26 | Does a cavity an arm reaches into deform, resist, cling on withdrawal, and recoil as simulation at hero distance without a visible frame cost? | The 600-element closed cage of [09 §9.6](09-testing-profiling.md#96-deformable-volume-experiments) set in a rigid wall with a stiff attached outer layer and softer inner layers; a scripted kinematic arm inserts 0.3 m, holds, and withdraws at 0.5 m/s, ten times; force, cling, recoil, volume, and cost on the RTX 5090 and the Titan X baseline machine | Adhesion model defaults ([07 §7.10](07-content-pipeline.md#710-deformable-volume-assets)); whether strain-driven surface detail is needed at hero distance or a cage this size already reads correctly; the near-tier budget for a single interactive volume ([13 §13.7](13-reference-consumer-games.md#137-reference-interactions)) |

## 10.6 Decisions deliberately deferred

Scripting language (E7); game UI framework (RmlUi vs in-house); D3D12 backend timing (trigger in [08 §8.3](08-toolchain.md#83-graphics-api)); runtime LLM tiers above 0 (E8, under [12-ai-usage-policy](12-ai-usage-policy.md)); dynamic runtime fracture; soft-against-soft deformable contact (its own experiment and budget when a use case needs it, [05 §5.14](05-simulation.md#514-deformable-volumes)); finite-element deformation and tetrahedral cages (a backend and a cage kind behind the same abstraction; fTetWild if they arrive); motion matching; neural radiance cache; Bazel; tile size and number of sim LOD tiers (per game); multiplayer transport and replication (readiness rules only for now, [05 §5.12](05-simulation.md#512-multiplayer-readiness)); a mobile renderer tier (a later consideration only, not a first-class target; revisited after Phase 7); consoles (out of scope); OpenUSD as the authoring format (rejected pending E4; LightUSD for interop only).

## 10.7 Decisions to record as ADRs now

1. Headless engine server with a versioned protocol; the editor is a client.
2. Text-serialized, layered, stable-ID document as the source of truth; canonical JSON.
3. Event-sourced persistent state in SQLite with snapshots; non-determinism is logged, not re-executed.
4. One dependency substrate with `derived` and `authored` node classes.
5. The cluster as the universal geometry unit; hybrid renderer; reference path tracer from day one.
6. Vulkan primary behind a thin API-neutral RHI, Windows first and Linux second; D3D12 deferred.
7. C++20 core; schema code generation; no hand-written serialization.
8. Slang for all shaders.
9. Multi-view rendering as a first-class concept; the HUD coordinate model.
10. Fixed-step deterministic sim; LOD as a system property; the materialization contract.
11. Tunables registry now; calibration runner only with evidence.
12. Jolt, Recast, SDL3, Tracy, mimalloc, SQLite, meshoptimizer adopted; flecs pending E6.
13. C++20 as the only core language; no Rust track.
14. Apache-2.0 engine license and the dependency policy that follows: no copyleft, proprietary SDKs as developer-supplied plugins only ([08 §8.8](08-toolchain.md#88-engine-license-and-dependency-policy)).
19. Efficiency as a first-class metric: hot-type size table, the engine container set with banned node-based standard containers, and the "fast path is the easy path" API rule ([11 §11.2](11-performance-principles.md#112-data-layout-and-footprint-first), [11 §11.9](11-performance-principles.md#119-performance-by-default-for-engine-users)).
20. The world reacts physically by default: destruction on unless opted out per object, per material category, or globally; deformable surfaces as a deformation layer over terrain with a deterministic coarse CPU grid for gameplay; soft bodies via Jolt XPBD for gameplay and GPU position-based dynamics for cosmetics ([05 §5.13](05-simulation.md#513-deformable-surfaces-and-soft-bodies)).
21. Deformable volumes as a first-class physics primitive: animation and deformation are separate, the cage is separate from the render mesh, the solver is abstracted with Jolt XPBD first, simulation LOD is mandatory, gameplay-relevant cage state is deterministic CPU state, and damage is constraint edits ([05 §5.14](05-simulation.md#514-deformable-volumes)).
15. AI usage policy: agent outputs are content and code, never training data; runtime models are unmodified third-party open weights ([12-ai-usage-policy](12-ai-usage-policy.md)).
16. Multiplayer-readiness rules enforced from Phase 3 ([05 §5.12](05-simulation.md#512-multiplayer-readiness)).
17. No hidden limits: checked compact types with fallback, tile-relative positions, 64-bit time, every limit a configurable budget ([11 §11.6](11-performance-principles.md#116-no-hidden-limits)).
18. Frame budgets are merge gates from Phase 1 ([11 §11.1](11-performance-principles.md#111-culture-budgets-are-gates)).

## 10.8 Calibrating the estimates

What actually bounds the schedule, in rough order:

1. **Human review bandwidth.** The owner is the creative director and the final reviewer of visual quality, design, and taste. Once agents run ahead of review, the queue, not the agents, sets the pace. The review-queue tooling in [06 §6.10](06-agent-tooling.md#610-multi-agent-roles-review-and-the-human-director) exists to keep this bottleneck as wide as possible.
2. **One GPU for the high end.** Renderer iterations, benchmarks, golden-image runs, and experiments serialize on a single RTX 5090. A second machine (i7-980 + GTX Titan X at 2560×1440) is the minimum-hardware target and the natural self-hosted CI runner for the baseline tier once it is set up; it cannot stand in for the RT and mesh-shader work. That machine and a headless Skylake Xeon server with a GTX Titan Xp are the two baseline-tier self-hosted runners of `.github/workflows/gpu.yml`, which runs the suite on real Vulkan weekly and on demand ([docs/ci/self-hosted-runners.md](../ci/self-hosted-runners.md)). Headless `sim` work does not compete for either.
3. **Experiments that need iteration** (E1, E2, E10) and vendor features that arrive on their own schedule.
4. **Visual judgment in renderer debugging.** Captures, the entity-ID buffer, FLIP against the reference path tracer, and golden images shrink this, but do not remove it.

Mechanism: every phase records its estimate and its actual in the task ledger. After Phase 0 and again after Phase 1, the remaining estimates are rescaled by the observed ratio, separately for rendering and non-rendering work, since they are expected to compress differently.

### Actuals

| Phase | Estimate | Actual | Notes |
|---|---|---|---|
| 0 Foundations | 2–4 focused weeks | 4 calendar days (12–15 September 2026), 17 commits, intermittent sessions | Every listed item landed except the schemac negative tests (in progress) and self-hosted GPU CI (deferred until Phase 1 needs it). Exit criterion met and tested end to end on Windows and Linux. The first CI run on Linux found three real defects; two-platform CI from the first commit of Phase 1 stands. |

Tentative rescale for non-rendering work after Phase 0: the actual came in below the low end of the estimate by a factor of two to four, consistent with the owner's expectation ([§10.1](#101-staging-principles)). Non-rendering phase estimates (3, 4, 5, and the tooling halves of 6 and 7) should be read at roughly half their stated focused weeks until Phase 1 provides the rendering ratio. Rendering estimates are not rescaled yet: they depend on GPU iteration and visual judgment, which Phase 0 did not exercise.

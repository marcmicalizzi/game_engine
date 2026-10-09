# Roadmap

The live board of every development item the plan, its ADRs and its experiments name: what is done and what closed it, what is under way, what is open, what is blocked and by what, and what a recorded decision has deferred. Each row is a title and a pointer; what the item is, and why, lives at the pointer. The history of estimates against actuals stays in the ledger ([10 §10.8](plan/10-roadmap-risks.md#108-calibrating-the-estimates)); this page is the current state.

**Keeping it current.** A change that finishes, starts or blocks a roadmap item updates its row here in the same commit, and a new piece of work gets a row before it starts ([AGENTS.md](../AGENTS.md#rules-that-are-checked), "Documentation moves with the code"). `tools/docs-check.ps1` holds the page to its shape: ids are unique, every id a "Depends on" cell names is a row, a `done` row names what closed it, a `blocked` row says by what, every row has a Where link that resolves, and State, Effort and Reasoning use the words below.

## How to read it

| Column | Values |
|---|---|
| `#` | A stable id: the area's letter and a number. An id is never renumbered or reused, so a dependency can name it; a new item takes its area's next free number wherever its row goes. |
| Item | The title. |
| State | `done` (with the ADR, commit or dated status note that closed it), `in progress`, `open`, `blocked` (by what), `deferred` (a decision is recorded not to build it now). |
| Depends on | The ids it needs first. Empty means it can start now. |
| Effort | `S`: hours, one agent's stage. `M`: a day, one agent. `L`: several agents or batches. |
| Reasoning | `routine`: the pattern exists in the tree and the spec is clear, a smaller model's task. `design`: choices to make, trade-offs to weigh. `research`: unknowns, and an experiment decides. |
| Where | The plan section, ADR, experiment or page to read first. |

**Blocked and open.** A row is `blocked` only by something outside ordinary work: the owner's review or decision, hardware, a runner, a session on the owner's machine. A row that waits on other rows is `open`, and its "Depends on" says on what. The work that can start today is therefore every `open` row whose dependencies are all `done`.

**Done is a claim with a source.** A `done` row names the ADR, the commit or the dated status note that closed it. Where only part of a plan item is built, the built part is a `done` row and the named remainder an `open` one. A proposed ADR whose decision is built shows twice: the built item, `done`, and its acceptance, `blocked` on the owner's review.

**Areas**, in roughly the order the plan builds things: [R, the renderer core and ray-traced lighting](#r--the-renderer-core-and-ray-traced-lighting); [W, the world, its streaming and the ground](#w--the-world-its-streaming-and-the-ground); [D, the data model and documents](#d--the-data-model-and-documents); [S, simulation and physics](#s--simulation-and-physics); [C, characters and tissue](#c--characters-and-tissue); [G, content and generation](#g--content-and-generation); [T, tooling and CI](#t--tooling-and-ci); [A, the agent protocol and tools](#a--the-agent-protocol-and-tools); [P, the reference games](#p--the-reference-games); [F, found along the way](#f--found-along-the-way), the open defects.

Compiled on 2026-10-07 from the plan's thirteen chapters and their dated status notes and addenda, the §10.8 ledger, the [ADR index](adr/README.md), the [experiment index](experiments/README.md), the subsystem pages' "Not yet" lists, and the git log since 2026-09-05.

## R — The renderer core and ray-traced lighting

Plan Phases 1 and 2: [04](plan/04-renderer.md), [10 §10.2](plan/10-roadmap-risks.md#102-phases).

| # | Item | State | Depends on | Effort | Reasoning | Where |
|---|---|---|---|---|---|---|
| R1 | Thin Vulkan RHI, API-neutral public headers | done (ADR-0006, fb4bf7a3) | | L | design | [gfx](subsystems/gfx.md#the-rhi-surface-and-the-backend-surface) |
| R2 | D3D12 backend | deferred (10 §10.6, ADR-0006) | R1 | L | design | [08 §8.3](plan/08-toolchain.md#83-graphics-api) |
| R3 | Render graph v0 | done (0896d347) | R1 | M | routine | [04 §4.2](plan/04-renderer.md#42-frame-architecture) |
| R4 | Render graph aliasing and async compute | open | R3 | M | design | [11 §11.7](plan/11-performance-principles.md#117-gpu) |
| R5 | Bindless resources | done (ADR-0023, d47bb05d) | R1 | M | routine | [ADR-0023](adr/0023-bindless-resources.md) |
| R6 | Slang shaders, shader library, hot reload | done (ADR-0008, dd7f56e2) | | M | routine | [08 §8.4](plan/08-toolchain.md#84-shaders) |
| R7 | GPU-resident scene of instances | done (fbe4d4d6) | R5 | M | routine | [04 §4.2](plan/04-renderer.md#42-frame-architecture) |
| R8 | Cluster build, LOD DAG and pages | done (d44153df, 07089a49) | | L | design | [04 §4.3](plan/04-renderer.md#43-geometry) |
| R9 | Geometry page streaming | done (a8431bcf, 10 §10.2 2026-09-18) | R8 | L | design | [renderer](subsystems/renderer.md#geometry-streaming-the-gpu-half-of-pages-and-residency) |
| R10 | GPU culling, LOD, cones, two-pass occlusion | done (d44153df, 18d8786b) | R8 | L | design | [04 §4.3](plan/04-renderer.md#43-geometry) |
| R11 | Mesh-shader visibility buffer | done (2ce98b21, 12fda093) | R10 | M | design | [04 §4.2](plan/04-renderer.md#42-frame-architecture) |
| R12 | Software rasterizer, experimental (E1) | done (ADR-0024) | R11 | M | research | [ADR-0024](adr/0024-hardware-rasterization-first.md) |
| R13 | Baseline vertex tier | done (e09ec8f3, 410ce790) | R11 | M | design | [04 §4.3](plan/04-renderer.md#43-geometry) |
| R14 | Baseline tier measured on the Maxwell machine | blocked (the Maxwell machine is down) | R13 | M | research | [04 §4.1](plan/04-renderer.md#41-goals-targets-non-goals) |
| R15 | 32-bit visibility buffer | open | R11 | M | design | [gfx](subsystems/gfx.md#what-a-device-has-to-have) |
| R16 | Per-instance culling BVH | open | R10 | M | design | [04 §4.3](plan/04-renderer.md#43-geometry) |
| R17 | Cheaper Hi-Z pyramid | open | R10 | M | research | [E9](experiments/e9-multi-view.md#follow-ups-this-leaves) |
| R18 | Occlusion culling chosen per frame | open | R17 | M | research | [04 §4.3](plan/04-renderer.md#43-geometry) |
| R19 | Material resolve | done (fdd97447, 001ffc7b) | R11 | M | design | [renderer](subsystems/renderer.md#materials) |
| R20 | Direct lighting, shared GGX BSDF | done (fe27bab5) | R19 | M | routine | [04 §4.2](plan/04-renderer.md#42-frame-architecture) |
| R21 | Layered materials and a material graph | open | R19 | L | design | [04 §4.3](plan/04-renderer.md#43-geometry) |
| R22 | Two-sided translucent leaf BSDF | open | R21 | M | research | [04 §4.3](plan/04-renderer.md#43-geometry) |
| R23 | Deformed-vertex pool, E25 renderer spike | done (5da98e35, 10 §10.5 2026-09-17) | R7 | L | research | [renderer](subsystems/renderer.md#the-deformed-vertex-pool) |
| R24 | Morph stream and staged deform chain | done (f71244af, d6039cb9) | R23 | L | design | [gfx](subsystems/gfx.md#the-deform-chain) |
| R25 | Deform chain remainders: pose bound, static cache, control mesh | open | R24 | M | design | [renderer](subsystems/renderer.md#morphed-instances) |
| R26 | Reference GPU path tracer | done (b9ba15cf, 04 §4.8 2026-09-18) | R20 | L | design | [renderer](subsystems/renderer.md#reference-renderer) |
| R27 | FLIP harness, scene corpus, render.evaluate | done (0799b441, eb0a4b47, 25398578) | R26 | M | routine | [04 §4.8](plan/04-renderer.md#48-reference-renderer-and-objective-optimization) |
| R28 | Reference many-light sampling with MIS | open | R26 | M | design | [04 §4.8](plan/04-renderer.md#48-reference-renderer-and-objective-optimization) |
| R29 | Pathological corpus scenes | open | R28 | L | design | [04 §4.8](plan/04-renderer.md#48-reference-renderer-and-objective-optimization) |
| R30 | Flythrough benchmark and recorded sessions | done (8b4f3552, 09 §9.4 2026-09-24) | R27 | M | routine | [renderer](subsystems/renderer.md#scenes-camera-paths-and-flythroughs) |
| R31 | Representative corpus scenes | open | R30 | L | design | [09 §9.4](plan/09-testing-profiling.md#94-benchmark-scene-corpus) |
| R32 | Nightly reference comparison | blocked (no runner carries `rtx`, T35) | R27, T35 | S | routine | [10 §10.2](plan/10-roadmap-risks.md#102-phases) |
| R33 | Per-region, temporal and surround reference metrics | open | R26 | M | design | [04 §4.8](plan/04-renderer.md#48-reference-renderer-and-objective-optimization) |
| R34 | Capture API with the entity-ID buffer | done (da851a1a) | R11 | M | routine | [renderer](subsystems/renderer.md#capture-channels-and-the-id-encoding) |
| R35 | Per-pass GPU timings and the frame record | done (2ce98b21, c4011921) | R3 | M | routine | [09 §9.3](plan/09-testing-profiling.md#93-profiling-infrastructure-before-optimizing-anything) |
| R36 | Phase 1 exit | blocked (its nightly clause, R32) | R32 | M | routine | [10 §10.2](plan/10-roadmap-risks.md#102-phases) |
| R37 | ViewSet multi-view and peripheral LOD | done (ADR-0009, 136a4a2f) | R10 | L | design | [renderer](subsystems/renderer.md#multi-view-a-viewset-over-one-scene) |
| R38 | E9's open clauses: TAA, denoiser, VRS across views | open | R39, R72, R73 | M | research | [E9](experiments/e9-multi-view.md) |
| R39 | Per-view variable rate shading | open | R37 | M | design | [renderer](subsystems/renderer.md#multi-view-a-viewset-over-one-scene) |
| R40 | Per-view internal resolution and ray budgets | open | R73, R75 | M | design | [04 §4.6](plan/04-renderer.md#46-extreme-displays) |
| R41 | One view-agnostic ray tracing cut | open | R37, R51 | M | design | [04 §4.6](plan/04-renderer.md#46-extreme-displays) |
| R42 | Filtered Panini resample | open | R37 | S | routine | [renderer](subsystems/renderer.md#multi-view-a-viewset-over-one-scene) |
| R43 | Multi-window present, per-output HDR metadata | open | R60 | M | design | [04 §4.6](plan/04-renderer.md#46-extreme-displays) |
| R44 | UI coordinate model and HUD-safe region | open | | M | design | [04 §4.7](plan/04-renderer.md#47-ui-coordinate-architecture) |
| R45 | Game UI framework | deferred (10 §10.6) | R44 | L | design | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |
| R46 | Forward and transparent pass | open | R19 | L | design | [04 §4.2](plan/04-renderer.md#42-frame-architecture) |
| R47 | Participating media (E32) | open | R46 | L | research | [04 §4.2](plan/04-renderer.md#42-frame-architecture) |
| R48 | Cascaded shadow maps | done (385c41ec, 874c7a20) | R10 | M | design | [renderer](subsystems/renderer.md#cascaded-shadow-maps) |
| R49 | Cascaded map follow-ups | open | R48 | M | design | [cascaded maps](experiments/cascaded-shadow-maps.md#follow-ups) |
| R50 | Cluster AS and KHR fallback (E2) | done (ADR-0025, 683286af) | R10 | L | research | [ADR-0025](adr/0025-cluster-acceleration-structures.md) |
| R51 | Ray tracing chain sized by the frame | done (e77ea198, c4011921) | R50 | M | design | [renderer](subsystems/renderer.md#the-ray-tracing-chains-memory) |
| R52 | Ray-traced hard shadows | done (47a97be6, c5d23997) | R50 | M | design | [renderer](subsystems/renderer.md#ray-traced) |
| R53 | Shadow default for the RTX class | blocked (the owner's choice of rt or csm) | R48, R52 | S | design | [flythrough](experiments/flythrough-desert-overlook.md) |
| R54 | Off-frustum casters under traced shadows | open | R51 | M | design | [renderer](subsystems/renderer.md#shadows) |
| R55 | Sky from a provider's model | done (97c4b018) | R20 | L | design | [renderer](subsystems/renderer.md#the-sky) |
| R56 | Accept ADR-0048 | blocked (the owner's review) | R55 | S | design | [ADR-0048](adr/0048-the-sky-is-a-providers-model-drawn-by-the-renderer.md) |
| R57 | Eye adaptation, bloom and the post chain | open | R55 | M | design | [04 §4.2](plan/04-renderer.md#42-frame-architecture) |
| R58 | Picture quantized once, with dither | done (b04e0ec2) | R55 | M | research | [renderer](subsystems/renderer.md#the-output-encode) |
| R59 | Accept ADR-0052 | done (2026-10-07, the owner) | R58 | S | design | [ADR-0052](adr/0052-the-picture-is-quantized-once-with-dither.md) |
| R60 | HDR output (E39) | blocked (a measurement on the owner's surround) | R58, R79, R80 | M | research | [E39](experiments/hdr-output-proposal.md) |
| R79 | HDR output paths: colour-space extensions, PQ encode, scRGB chain, EXR capture, the probe | done (2026-10-09: stage 1 59a99ffd, the EXR capture, linear radiance and ramp in the commit after 998830a1) | R58 | M | design | [renderer](subsystems/renderer.md#hdr-output) |
| R80 | The HDR measurement script for E39 | done (2026-10-09, tools/hdr-measure.ps1, dd2c097f) | R79 | S | routine | [E39](experiments/hdr-output-proposal.md#what-the-owner-measures) |
| R61 | Top-level structure over occupied instances only | open | R51 | S | routine | [frame-thread spikes](experiments/frame-thread-spikes-2026-10-04.md#what-is-open) |
| R62 | BLAS budget manager | open | R51 | L | design | [04 §4.4](plan/04-renderer.md#44-ray-tracing) |
| R63 | Cluster structures shared across rigid instances | open | R50 | S | design | [gfx](subsystems/gfx.md#what-a-device-has-to-have) |
| R64 | KHR fallback measured on an AMD or Intel GPU | blocked (no such ray-tracing card here) | R50 | M | research | [ADR-0025](adr/0025-cluster-acceleration-structures.md) |
| R65 | Alpha-tested geometry in raster, rays, shadows | open | R51, G11 | L | design | [04 §4.3](plan/04-renderer.md#43-geometry) |
| R66 | Hierarchical instancing of parts | open | R7 | M | research | [04 §4.3](plan/04-renderer.md#43-geometry) |
| R67 | Vegetation impostors | open | R66 | L | research | [04 §4.3](plan/04-renderer.md#43-geometry) |
| R68 | In-house ReSTIR DI | open | R28, R51 | L | research | [04 §4.4](plan/04-renderer.md#44-ray-tracing) |
| R69 | Radiance cache and ReSTIR GI | open | R68 | L | research | [04 §4.4](plan/04-renderer.md#44-ray-tracing) |
| R70 | Neural radiance cache | deferred (10 §10.6) | R69 | L | research | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |
| R71 | Ray-traced reflections | open | R69 | M | design | [04 §4.4](plan/04-renderer.md#44-ray-tracing) |
| R72 | In-house baseline denoiser | open | R68 | L | research | [04 §4.4](plan/04-renderer.md#44-ray-tracing) |
| R73 | TAA and upscaling | open | R11 | L | design | [04 §4.2](plan/04-renderer.md#42-frame-architecture) |
| R74 | Vendor upscaler and reconstruction plugins | open | R73 | M | design | [04 §4.4](plan/04-renderer.md#44-ray-tracing) |
| R75 | Ray budget scheduler | open | R68, R69 | L | research | [04 §4.4](plan/04-renderer.md#44-ray-tracing) |
| R76 | Phase 2 exit: hybrid RT at the targets | open | R62, R68, R69, R71, R72, R73, R75 | L | research | [10 §10.2](plan/10-roadmap-risks.md#102-phases) |
| R77 | Destruction acceleration structures | open | R62, S48 | L | design | [04 §4.5](plan/04-renderer.md#45-destruction-and-acceleration-structures) |
| R78 | A mobile renderer tier | deferred (10 §10.6) | R13 | L | design | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |

## W — The world, its streaming and the ground

The tile ring, the ground drawn from the world's tiles, the dunes, residency, and world positions in f64: [04 §4.9](plan/04-renderer.md#49-streaming-and-residency), [05 §5.5](plan/05-simulation.md#55-reconciliation-when-a-tile-activates), [ADR-0053](adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md).

| # | Item | State | Depends on | Effort | Reasoning | Where |
|---|---|---|---|---|---|---|
| W1 | Tile ring and its consumers | done (9a7fd306, ac953df6) | | L | design | [ADR-0040](adr/0040-the-tile-ring.md) |
| W2 | Accept ADR-0040 | blocked (the owner's review) | W1 | S | design | [ADR-0040](adr/0040-the-tile-ring.md) |
| W3 | Ruins streamed tile by tile (E34, E35) | done (821c978f, 2b07dc53) | W1, G39 | L | research | [E35](experiments/e35-world-streaming.md) |
| W4 | Tile blocks in the instance table (E37) | done (e610e8d2, 0ac914a6) | W3 | M | design | [E37](experiments/e37-tile-instance-blocks.md) |
| W5 | Handover cross-fade between representations | open | W3 | L | design | [E35](experiments/e35-world-streaming.md#after-the-rubble-rule) |
| W6 | Reject a hole before the cull's search | open | W4 | S | routine | [E37](experiments/e37-tile-instance-blocks.md#what-it-does-not-decide) |
| W7 | A sink that names the tiles that changed | open | W4 | S | routine | [world](subsystems/world.md#not-yet) |
| W8 | Deformed-vertex pool for the dynamic tail | open | W4, R23 | M | design | [renderer](subsystems/renderer.md#instances-that-come-and-go) |
| W9 | Ray tracing a streamed scene's tail | open | W4, R51 | L | design | [renderer](subsystems/renderer.md#instances-that-come-and-go) |
| W10 | A kit's geometry streamed per tile | open | W4, R9 | M | design | [04 §4.9](plan/04-renderer.md#49-streaming-and-residency) |
| W11 | World seed in the document | open | W1 | S | routine | [world](subsystems/world.md#not-yet) |
| W12 | Ground drawn from the world's tiles | done (83ed5ffb, de1d04ea) | W1, G35 | L | design | [ADR-0050](adr/0050-the-ground-is-drawn-from-the-worlds-tiles.md) |
| W13 | Accept ADR-0050 | done (2026-10-07, the owner) | W12 | S | design | [ADR-0050](adr/0050-the-ground-is-drawn-from-the-worlds-tiles.md) |
| W14 | Tiles keep up with a fast camera and a window | done (7c3239bd, 4afee714) | W12 | M | design | [world tiles in a window](experiments/world-tiles-window-2026-10-03.md) |
| W15 | Far ground to the horizon | done (2efc7749, 4e862d11) | W12 | L | design | [ADR-0051](adr/0051-the-far-ground-is-the-renderers.md) |
| W16 | Accept ADR-0051 | done (2026-10-07, the owner) | W15 | S | design | [ADR-0051](adr/0051-the-far-ground-is-the-renderers.md) |
| W17 | Terrain motion, tiles, far levels over the protocol | done (c5a37706) | W15 | M | routine | [renderer](subsystems/renderer.md#one-request-two-hosts) |
| W18 | Far tiles rebuilt only where they change | open | W15 | S | design | [far ground](experiments/far-ground-2026-10-03.md#what-is-open) |
| W19 | Windowed lag with far levels, measured | open | W15 | S | routine | [far ground](experiments/far-ground-2026-10-03.md#what-is-open) |
| W20 | Cheaper tile rebuilds at speed | open | W14 | M | design | [world tiles in a window](experiments/world-tiles-window-2026-10-03.md#what-is-left) |
| W21 | One staging ring for tile uploads | open | W14 | S | routine | [world tiles in a window](experiments/world-tiles-window-2026-10-03.md#what-is-left) |
| W22 | Retire the terrain rings | open | W12 | S | routine | [ADR-0050](adr/0050-the-ground-is-drawn-from-the-worlds-tiles.md) |
| W23 | Tile-set ground provider for an authored world | open | W12 | M | routine | [world](subsystems/world.md#an-authored-world) |
| W24 | Content build of a tile set and its far lattices | open | W23 | M | design | [world](subsystems/world.md#an-authored-world) |
| W25 | Per-tile maps for tile sources | open | W24 | M | design | [world](subsystems/world.md#an-authored-world) |
| W26 | Dunes as a function of time, bounded overlay (E36) | done (fd288496, 32e89ce2, 8be5df1a) | | L | design | [ADR-0043](adr/0043-dunes-as-a-function-of-time.md) |
| W27 | Accept ADR-0043 | blocked (the owner's fly-through) | W26 | S | design | [ADR-0043](adr/0043-dunes-as-a-function-of-time.md) |
| W28 | E14: dune fast-forward accuracy | done (fd288496) | W26 | M | research | [13 §13.6](plan/13-reference-consumer-games.md#136-additional-experiments) |
| W29 | E13: granular relaxation on the coarse grid | open | W26 | M | research | [13 §13.6](plan/13-reference-consumer-games.md#136-additional-experiments) |
| W30 | Dune field shape: lag past repose, sharp calm barchans | open | W26 | S | design | [terrain](subsystems/terrain.md#not-yet) |
| W31 | Terrain rings and a smooth time-lapse | done (ebf5ad7e, 24b6ec54) | W26 | L | design | [renderer](subsystems/renderer.md#the-dunes-in-time-lapse) |
| W32 | Scene grid under time-lapse: re-base, a fourth slot | open | W31 | M | design | [renderer](subsystems/renderer.md#not-yet) |
| W33 | Dune field evaluated per vertex on the GPU | open | W31 | L | research | [time-lapse](experiments/time-lapse-smoothness-2026-09-27.md#c-evaluation) |
| W34 | Wind's day, storm gain and wind rose | done (1092d627, 500eb8e5) | W26 | M | design | [wind's day](experiments/wind-day-and-storm-gain-2026-09-28.md) |
| W35 | Lee drifts from the wind's shadow map | open | W34 | L | design | [13 §13.1](plan/13-reference-consumer-games.md#day-night-and-weather) |
| W36 | Ruins' declared drifts reach the ground | open | W26, G39 | S | routine | [world](subsystems/world.md#not-yet) |
| W37 | Walker follows the moving sand | done (e6ffa9bd) | W31, S24 | S | routine | [walk in time-lapse](experiments/walk-in-time-lapse-2026-09-29.md) |
| W38 | Ground drawn and walked with the overlay and lag | open | W12, W26 | M | design | [world](subsystems/world.md#not-yet) |
| W39 | Deformer stamps from hosts: footprints, tracks | open | W38, S34 | M | design | [terrain](subsystems/terrain.md#not-yet) |
| W40 | GPU deformation map refining the overlay | open | W38, W47 | L | design | [05 §5.13](plan/05-simulation.md#513-deformable-surfaces-and-soft-bodies) |
| W41 | Snow deformation | open | W39 | L | design | [05 §5.13](plan/05-simulation.md#513-deformable-surfaces-and-soft-bodies) |
| W42 | Sand close up: ripples, grain, grainflow | done (bb577f1a, a40d6960, e5e4aeaf) | W26 | L | design | [renderer](subsystems/renderer.md#the-sand-close-up) |
| W43 | Sand detail leftovers | open | W42 | M | design | [fifth pass](experiments/sand-fifth-pass-2026-10-04.md#seen-and-not-fixed) |
| W44 | The sand's shelter channel | open | W42 | M | design | [terrain](subsystems/terrain.md#what-the-effect-needs) |
| W45 | Blowing-sand volumetric pass | open | W34, R47 | L | research | [terrain](subsystems/terrain.md#what-the-effect-needs) |
| W46 | Ripple celerity setting | blocked (the owner's judgement, against W45) | W45 | S | design | [renderer](subsystems/renderer.md#the-sand-close-up) |
| W47 | One residency manager for three page types | open | R9 | L | design | [04 §4.9](plan/04-renderer.md#49-streaming-and-residency) |
| W48 | Texture page streaming | open | W47 | L | design | [04 §4.9](plan/04-renderer.md#49-streaming-and-residency) |
| W49 | Acceleration structures under the shared budget | open | W47, R62 | M | design | [04 §4.9](plan/04-renderer.md#49-streaming-and-residency) |
| W50 | Streaming deformed scenes and RT templates | open | R9, R24 | M | design | [04 §4.9](plan/04-renderer.md#49-streaming-and-residency) |
| W51 | Peak host footprint at load | open | R9 | S | routine | [04 §4.9](plan/04-renderer.md#49-streaming-and-residency) |
| W52 | Page decompression | open | R9 | L | design | [04 §4.9](plan/04-renderer.md#49-streaming-and-residency) |
| W53 | Prediction from observer velocity | open | R9 | M | research | [04 §4.9](plan/04-renderer.md#49-streaming-and-residency) |
| W54 | Platform async I/O: IOCP and io_uring | open | T5 | M | routine | [04 §4.9](plan/04-renderer.md#49-streaming-and-residency) |
| W55 | Tile quadtree levels and per-tile spatial index | open | W1 | M | research | [03 §3.7](plan/03-data-model.md#37-spatial-partition) |
| W56 | World positions in f64 on the CPU | done (ADR-0053, 30c4ad70, d7888c3a) | | L | design | [ADR-0053](adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md) |
| W57 | ADR-0053 stage 1: the eye is the frame's origin | done (62667b40) | W56 | L | design | [world positions](experiments/world-positions-renderer-2026-10-05.md) |
| W58 | ADR-0053 stage 2: cascades and tile corners | done (874c7a20, a88764ef) | W57 | M | design | [world positions](experiments/world-positions-renderer-2026-10-05.md#stage-2-picture-2-the-cascades-snap-to-the-world) |
| W59 | ADR-0053 stage 3: files and wire in f64 | done (5fb57ff3) | W57 | M | routine | [world positions](experiments/world-positions-renderer-2026-10-05.md#stage-3-picture-3-the-files-and-the-wire) |
| W60 | ADR-0053 stage 4: a tile's UVs from its corner | done (63ce114e, d1a2e690) | W58 | S | design | [world positions](experiments/world-positions-renderer-2026-10-05.md#stage-4-picture-4-a-tiles-uvs-from-its-corner) |
| W61 | Scatter centre and sand-drift endpoints in f64 | done (2026-10-07, `Scatter` and `SandDrift` version 2) | W59 | S | routine | [ruins](subsystems/ruins.md#far-from-the-origin) |
| W62 | Waves provider's millimetre entries | done (2026-10-07, refused tiles instead: `scene_gen::k_ground_millimetres`) | W56 | S | routine | [scene_gen](subsystems/scene_gen.md#far-from-the-origin) |
| W63 | Ring chunks built in their corner's frame | done (2026-10-07, `terrain::RingParams::chunk_frame`) | W58 | S | routine | [renderer](subsystems/renderer.md#the-grounds-tiles-are-placed-at-their-corners) |
| W64 | World tile index past 6.9e10 m | done (2026-10-07, refused where positions enter: `world::tile_reachable`) | W56 | S | design | [world](subsystems/world.md#where-an-observer-is) |
| W65 | Lights placed by cell on the GPU | open | W57 | S | routine | [renderer](subsystems/renderer.md#the-frames-origin) |

## D — The data model and documents

Identity, the layered document, persistence and schema evolution: [03](plan/03-data-model.md).

| # | Item | State | Depends on | Effort | Reasoning | Where |
|---|---|---|---|---|---|---|
| D1 | Id128 identity and content hashes | done (0d3a1ebe, 4a752ef4) | | M | routine | [03 §3.1](plan/03-data-model.md#31-identity) |
| D2 | Schema IDL and schemac, with negative tests | done (ADR-0007, 105a706e, 7c152a5c) | | L | routine | [schema](subsystems/schema.md) |
| D3 | Canonical JSON | done (0d3a1ebe) | | S | routine | [json](subsystems/json.md) |
| D4 | Layered document: commands, transactions, journal | done (b82aaf31, 834d4bc4) | D1, D3 | L | routine | [03 §3.3](plan/03-data-model.md#33-transactions-diffs-and-merges) |
| D5 | Structural diff | done (b82aaf31) | D4 | S | routine | [doc](subsystems/doc.md) |
| D6 | Structural three-way merge | done (85d72e61) | D4 | M | design | [03 §3.3](plan/03-data-model.md#33-transactions-diffs-and-merges) |
| D7 | Composed index, tile index, change feed | done (c1543d9a, bf1ba9e4, fd829fb1) | D4 | M | design | [doc](subsystems/doc.md#change-feed) |
| D8 | Tile-partitioned layer files | done (c1543d9a) | D7 | M | routine | [03 §3.7](plan/03-data-model.md#37-spatial-partition) |
| D9 | Load chosen tiles of a partitioned layer | open | D8 | M | design | [doc](subsystems/doc.md) |
| D10 | Document memory per record (1.5 KB at LOD3) | open | D4 | M | research | [E38](experiments/e38-scheduled-npcs.md#memory-per-resident) |
| D11 | Save writes only what changed | done (501a6ba2, a2c8b7cd) | D4 | M | design | [doc](subsystems/doc.md#saving) |
| D12 | Accept ADR-0041 | blocked (the owner's review) | D11 | S | design | [ADR-0041](adr/0041-a-save-is-a-log-then-files-written-in-place.md) |
| D13 | Content-addressed blobs referenced from documents | open | D4 | M | design | [03 §3.2](plan/03-data-model.md#32-the-authoring-document-world-document) |
| D14 | Git checkpoints and rollback_to | open | D4 | M | design | [06 §6.4](plan/06-agent-tooling.md#64-transactions-checkpoints-rollback) |
| D15 | E4: canonical JSON against LightUSD | open | D4 | M | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| D16 | Event log, projections, per-tile snapshots | done (02d7967a, ac953df6, ADR-0028) | | L | routine | [store](subsystems/store.md#tiles-projections-and-snapshots-the-world) |
| D17 | Projections folded from the log; trimmed saves | open | D16 | L | design | [world](subsystems/world.md#not-yet) |
| D18 | A save is the store, the document and the drivers | done (807dc07d, 3ecfd729) | D11, D16 | L | design | [world](subsystems/world.md#save-and-load) |
| D19 | Accept ADR-0042 | blocked (the owner's review) | D18 | S | design | [ADR-0042](adr/0042-a-save-is-the-store-the-document-and-the-drivers.md) |
| D20 | Live-tile edits and deletions reach the world at once | open | D7 | S | routine | [world](subsystems/world.md#not-yet) |
| D21 | Save while a call is running | open | D18 | M | design | [03 §3.5](plan/03-data-model.md#35-persistent-world-state) |
| D22 | Migration corpus of saves | done (9ebe2af1, 84e76393) | D18 | M | routine | [03 §3.8](plan/03-data-model.md#38-schema-evolution) |
| D23 | Resident save in the corpus | open | D22 | S | routine | [corpus](../content/migration-corpus/README.md#the-resident-save-written-on-the-owners-machine-not-yet-here) |
| D24 | Old documents in the migration corpus | open | D22 | S | routine | [03 §3.8](plan/03-data-model.md#38-schema-evolution) |
| D25 | Events carry their payload version | done (84e76393) | D16 | S | routine | [store](subsystems/store.md) |
| D26 | Upcast old event payloads on read | open | D25 | M | design | [03 §3.8](plan/03-data-model.md#38-schema-evolution) |
| D27 | Generated migrators for additive changes | open | D2 | M | design | [03 §3.8](plan/03-data-model.md#38-schema-evolution) |
| D28 | A rename as an alias | open | D2 | S | design | [03 §3.8](plan/03-data-model.md#38-schema-evolution) |
| D29 | Layer files name their record-type versions | done (d7888c3a) | D8 | S | design | [doc](subsystems/doc.md#a-layer-says-which-versions-it-holds) |
| D30 | Derived nodes: the content build's cache and steps | done (10 §10.8 2026-09-25) | G4 | L | routine | [content_build](subsystems/content_build.md#derived-steps) |
| D31 | Substrate v1: tracked reads, node index, function-hash versions | open | D30 | L | design | [ADR-0004](adr/0004-dependency-substrate.md) |
| D32 | Authored nodes and canon invalidation | open | D31, A23 | L | design | [03 §3.6](plan/03-data-model.md#36-dependencies-and-invalidation) |
| D33 | Canon types: canonicity, provenance, quests | open | D2 | L | design | [03 §3.9](plan/03-data-model.md#39-canon-and-narrative-representation) |
| D34 | Narrative dependency index | open | D32, D33 | M | design | [03 §3.9](plan/03-data-model.md#39-canon-and-narrative-representation) |
| D35 | Agent edits logged as agent-origin events | open | D16 | M | design | [03 §3.10](plan/03-data-model.md#310-how-deterministic-and-agent-generated-state-coexist) |
| D36 | Phase 0 exit | done (10 §10.2 2026-09-15) | D4, D5, A1 | L | routine | [10 §10.2](plan/10-roadmap-risks.md#102-phases) |

## S — Simulation and physics

The scheduler, tiers, NPCs, destruction, deformation, physics, navigation, animation, audio and input: [05 §5.1–§5.15](plan/05-simulation.md), Phases 3 and 6 of [10 §10.2](plan/10-roadmap-risks.md#102-phases).

| # | Item | State | Depends on | Effort | Reasoning | Where |
|---|---|---|---|---|---|---|
| S1 | ECS decision and flecs' seams (E6) | done (ADR-0028, ADR-0030) | | L | research | [ADR-0028](adr/0028-ecs-and-persistent-store.md) |
| S2 | Sim scheduler: phases and waves | done (7f83e862) | | M | design | [05 §5.2](plan/05-simulation.md#52-sim-scheduler) |
| S3 | Static registration table and tier filtering | open | S2 | S | routine | [sim](subsystems/sim.md#what-is-stubbed) |
| S4 | The scheduler owns the tick | done (bf1ba9e4) | S1, S2 | M | design | [ADR-0038](adr/0038-the-scheduler-owns-the-tick.md) |
| S5 | flecs systems in the waves; engine-view on the scheduler | open | S4 | M | research | [ecs](subsystems/ecs.md#not-yet) |
| S6 | Accept ADR-0038 | blocked (the owner's review) | S4 | S | design | [ADR-0038](adr/0038-the-scheduler-owns-the-tick.md) |
| S7 | Timing wheel and budgeted fast-forward | done (a8f50ca0) | S2 | M | design | [05 §5.3](plan/05-simulation.md#53-event-scheduler-temporal-lod) |
| S8 | LOD tiers and the materialization contract | done (ee8fd7fc, bf1ba9e4) | S2 | M | design | [05 §5.4](plan/05-simulation.md#54-lod-tier-assignment) |
| S9 | Tier promotion for records other than residents | open | S8 | M | design | [sim](subsystems/sim.md#what-is-stubbed) |
| S10 | SIMD trial on tier scoring, structure-of-arrays batches | open | S8 | M | research | [sim](subsystems/sim.md#positions-are-f64) |
| S11 | Persistence flush: projections, change detection | open | D16 | M | design | [ecs](subsystems/ecs.md#not-yet) |
| S12 | World event bus with runaway guards | open | S2 | M | design | [05 §5.7](plan/05-simulation.md#57-world-events-and-consequences) |
| S13 | Scheduled residents: routines as data | done (5049d289, e8844219) | S7, S8 | L | design | [npc](subsystems/npc.md) |
| S14 | Accept ADR-0045 | blocked (the owner's review) | S13 | S | design | [ADR-0045](adr/0045-routines-are-data-with-a-closed-form.md) |
| S15 | E38 remainder: the store's share | open | S13 | S | research | [E38](experiments/e38-scheduled-npcs.md#caveats) |
| S16 | Phase 3 exit | done (10 §10.2 2026-09-25) | S13, S17, D18 | L | design | [10 §10.2](plan/10-roadmap-risks.md#102-phases) |
| S17 | Input, recorded logs and deterministic replay | done (51b843e8, 3ecfd729) | | M | design | [05 §5.10](plan/05-simulation.md#510-determinism-and-replay) |
| S18 | Replay remainder: player-origin events, split cadence | open | S17 | M | design | [05 §5.10](plan/05-simulation.md#510-determinism-and-replay) |
| S19 | Force feedback v0 | done (86b520a9) | | S | routine | [window](subsystems/window.md) |
| S20 | Haptics beyond constant, spring and damper | open | S19 | M | routine | [window](subsystems/window.md) |
| S21 | Split screen and per-player devices | deferred (02 §2.7 2026-09-27) | S17 | L | design | [02 §2.7](plan/02-architecture.md#27-generality-what-the-engine-must-not-preclude) |
| S22 | Jolt physics world | done (04bb152c, 3617723a) | | M | routine | [physics](subsystems/physics.md) |
| S23 | Physics wrap remainder | open | S22 | M | routine | [physics](subsystems/physics.md#what-is-not-wrapped-yet) |
| S24 | Character controller | done (f10dd918) | S22 | M | design | [physics](subsystems/physics.md#the-character) |
| S25 | Bodies for materialized entities | open | S8, S22 | M | design | [05 §5.10](plan/05-simulation.md#510-determinism-and-replay) |
| S26 | Scene collision from streamed tiles | open | S24, W38 | M | design | [scene_collision](subsystems/scene_collision.md#not-yet) |
| S27 | Kinematics capability | done (bf1ba9e4) | S4 | S | routine | [kinematics](subsystems/kinematics.md) |
| S28 | Physics islands with local origins | deferred (ADR-0053) | S22 | L | research | [physics far out](experiments/world-positions-physics-2026-10-05.md#far-from-the-origin-being-far-costs-more-than-being-double) |
| S29 | Recast navigation, rebuild queue, region graph | done (8ab3f546) | | M | routine | [nav](subsystems/nav.md#the-region-graph-and-why-a-coarse-tier-exists) |
| S30 | E11 destruction-load half; Detour tile cache | open | S29, S48 | M | research | [nav](subsystems/nav.md#what-is-not-wrapped-yet) |
| S31 | Crowds, formations and jump links | open | S29 | M | design | [05 §5.15](plan/05-simulation.md#515-capability-inventory) |
| S32 | Standard skeleton, first cut | done (9573932e) | | M | routine | [anim](subsystems/anim.md#the-standard-skeleton-first-cut) |
| S33 | Basic animation: clips and blending | done (b228c43f) | S32 | L | design | [05 §5.11](plan/05-simulation.md#511-integration-notes) |
| S34 | IK and the standard skeleton's depth | open | S32 | L | design | [anim](subsystems/anim.md#the-standard-skeleton-first-cut) |
| S35 | Animation graphs as data | open | S33 | M | design | [animation](subsystems/animation.md#not-yet) |
| S36 | Animation through the protocol | open | S33 | S | routine | [animation](subsystems/animation.md#not-yet) |
| S37 | Motion matching | deferred (10 §10.6) | S35 | L | research | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |
| S38 | Ragdolls | open | S23, S34 | M | design | [05 §5.15](plan/05-simulation.md#515-capability-inventory) |
| S39 | Audio v0: an object-based sound stage | done (de08c8d8) | | L | design | [audio](subsystems/audio.md) |
| S40 | Audio decode stages and Steam Audio | open | S39 | L | design | [audio](subsystems/audio.md#not-yet) |
| S41 | Audio formats and bus effects | open | S39 | M | routine | [audio](subsystems/audio.md#not-yet) |
| S42 | Wind as sound | open | S39 | M | research | [05 §5.11](plan/05-simulation.md#511-integration-notes) |
| S43 | Multiplayer-readiness rules | done (ADR-0016) | | S | design | [ADR-0016](adr/0016-multiplayer-readiness.md) |
| S44 | Multiplayer transport, replication, prediction | deferred (10 §10.6, ADR-0016) | S43 | L | design | [05 §5.12](plan/05-simulation.md#512-multiplayer-readiness) |
| S45 | Tile size and sim tier count per game | deferred (10 §10.6, ADR-0010) | | S | design | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |
| S46 | Support graphs and destruction defaults | open | S22 | L | design | [05 §5.8](plan/05-simulation.md#58-destruction) |
| S47 | Pre-fracture build | open | S46 | M | design | [05 §5.8](plan/05-simulation.md#58-destruction) |
| S48 | Collapse simulation | open | S46, S47 | L | design | [05 §5.8](plan/05-simulation.md#58-destruction) |
| S49 | Damage-state persistence and variant selection | open | S48 | M | design | [05 §5.8](plan/05-simulation.md#58-destruction) |
| S50 | Dynamic runtime fracture | deferred (10 §10.6, ADR-0020) | S47 | L | research | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |
| S51 | Network graphs: power and mechanisms | open | S46 | M | design | [05 §5.15](plan/05-simulation.md#515-capability-inventory) |
| S52 | NPC routines at LOD1 and LOD0 | open | S13, S25, S29 | L | design | [npc](subsystems/npc.md#not-yet) |
| S53 | Routine overrides from world events | open | S12, S13 | M | design | [npc](subsystems/npc.md#not-yet) |
| S54 | Economy and faction event consumers | open | S12 | L | design | [05 §5.7](plan/05-simulation.md#57-world-events-and-consequences) |
| S55 | Perception: sound events and scent | open | S12 | M | design | [05 §5.15](plan/05-simulation.md#515-capability-inventory) |
| S56 | World-mutation guard | open | D34 | M | design | [05 §5.9](plan/05-simulation.md#59-narrative-dependencies-and-story-protection) |
| S57 | Story-protection policies | open | S56, D34 | M | design | [05 §5.9](plan/05-simulation.md#59-narrative-dependencies-and-story-protection) |
| S58 | XPBD soft bodies and the deformable budget | done (04bb152c, ADR-0029) | S22 | M | research | [physics](subsystems/physics.md#soft-bodies-and-how-they-map-onto-adr-0026) |
| S59 | Gameplay soft bodies system | open | S25, S58 | M | design | [05 §5.13](plan/05-simulation.md#513-deformable-surfaces-and-soft-bodies) |
| S60 | Cosmetic GPU cloth | open | | L | design | [05 §5.13](plan/05-simulation.md#513-deformable-surfaces-and-soft-bodies) |
| S61 | Ropes and cables | open | S59 | M | design | [05 §5.15](plan/05-simulation.md#515-capability-inventory) |
| S62 | Particle fluids | deferred (ADR-0020) | | L | research | [05 §5.13](plan/05-simulation.md#513-deformable-surfaces-and-soft-bodies) |
| S63 | Water surfaces, fluids and buoyancy | open | | L | design | [05 §5.15](plan/05-simulation.md#515-capability-inventory) |
| S64 | World fields: drag, heat and fire, weather | open | | L | design | [05 §5.15](plan/05-simulation.md#515-capability-inventory) |
| S65 | Vehicles | open | S23 | M | design | [05 §5.15](plan/05-simulation.md#515-capability-inventory) |
| S66 | Trees under wind as rooted bodies | open | S58, R23 | M | research | [05 §5.15](plan/05-simulation.md#515-capability-inventory) |
| S67 | Deformable volumes as a primitive, with budgets | done (ADR-0026, ADR-0029) | S58 | M | design | [ADR-0026](adr/0026-deformable-volumes-first-class.md) |
| S68 | DeformableVolume abstraction over the solver | open | S67 | M | design | [05 §5.14](plan/05-simulation.md#514-deformable-volumes) |
| S69 | Cage attachments and layers | open | S68 | M | design | [physics](subsystems/physics.md#the-two-attachment-kinds-and-why-there-are-two) |
| S70 | Soft-against-rigid and own-skeleton contact | open | S69 | M | research | [05 §5.14](plan/05-simulation.md#514-deformable-volumes) |
| S71 | Volume simulation tiers with state blending | open | S8, S68 | M | research | [05 §5.14](plan/05-simulation.md#514-deformable-volumes) |
| S72 | Damage as cage constraint edits | open | S46, S68 | M | design | [05 §5.14](plan/05-simulation.md#514-deformable-volumes) |
| S73 | Adhesive contact | open | S70 | M | research | [05 §5.14](plan/05-simulation.md#514-deformable-volumes) |
| S74 | Strain-driven surface detail | open | S68, S83 | M | research | [05 §5.14](plan/05-simulation.md#514-deformable-volumes) |
| S75 | Soft-against-soft contact | deferred (10 §10.6, ADR-0026) | S70 | L | research | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |
| S76 | Finite-element backend | deferred (10 §10.6) | S81 | L | research | [05 §5.14](plan/05-simulation.md#514-deformable-volumes) |
| S77 | E19's two failing criteria | open | | S | research | [E19](experiments/e19-lattice-cage.md#what-failed-and-why-it-is-not-the-cages-fault) |
| S78 | E20: secondary motion under locomotion | open | S69 | M | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| S79 | E21: a repeated rigid grip | open | S70 | S | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| S80 | E22: a 60 s load on a cushion with no core | open | | S | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| S81 | E23: layer stiffness ratios | open | S69 | M | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| S82 | E24: a crowd across volume tiers | open | S71 | M | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| S83 | E26: a cavity reached into at hero distance | open | S69, S73 | L | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| S84 | ADR-0029's thresholds on a quiet machine and the Titan X | blocked (the Maxwell machine is down) | S67 | S | routine | [ADR-0029](adr/0029-deformable-volume-budgets.md) |
| S85 | Phase 6 exit | open | S48, S49, S53, S57, S83, W29, W41 | L | design | [10 §10.2](plan/10-roadmap-risks.md#102-phases) |

## C — Characters and tissue

The character standard and the tissue program: [ADR-0032](adr/0032-characters-are-parameter-vectors.md), [07 §7.10–§7.11](plan/07-content-pipeline.md#711-characters), [05 §5.16](plan/05-simulation.md#516-characters-at-run-time).

| # | Item | State | Depends on | Effort | Reasoning | Where |
|---|---|---|---|---|---|---|
| C1 | Character standard decided | done (ADR-0032) | | M | design | [ADR-0032](adr/0032-characters-are-parameter-vectors.md) |
| C2 | Content classes and view policies decided | done (ADR-0033) | | M | design | [ADR-0033](adr/0033-content-classes-and-view-policies.md) |
| C3 | Content classes on assets | open | C2 | L | design | [ADR-0033](adr/0033-content-classes-and-view-policies.md) |
| C4 | Canonical vertex identity stream | done (099d426b) | | M | routine | [geometry](subsystems/geometry.md#canonical-vertex-identity) |
| C5 | Canonical ids resolved against built meshes | open | C4 | M | design | [tissue](subsystems/tissue.md#not-yet) |
| C6 | Neutral base family | open | C4 | L | design | [07 §7.11](plan/07-content-pipeline.md#711-characters) |
| C7 | Parameter model and declarative rig | open | C1, R24 | L | design | [07 §7.11](plan/07-content-pipeline.md#711-characters) |
| C8 | Macro corner axes over region-masked detail | open | C6, C7 | L | design | [ADR-0032](adr/0032-characters-are-parameter-vectors.md) |
| C9 | Joint regressor | open | C6, S32 | M | design | [07 §7.11](plan/07-content-pipeline.md#711-characters) |
| C10 | Height and proportion as skeleton parameters | open | C9, S34 | M | design | [07 §7.11](plan/07-content-pipeline.md#711-characters) |
| C11 | Section masks and graft modules | open | C6, C20 | L | design | [07 §7.11](plan/07-content-pipeline.md#711-characters) |
| C12 | Layered material compositor | open | C3 | L | design | [07 §7.11](plan/07-content-pipeline.md#711-characters) |
| C13 | Character bake as a derived node | open | C7, C9, C12 | M | design | [07 §7.11](plan/07-content-pipeline.md#711-characters) |
| C14 | Character serialization and seeded generation | open | C7 | M | routine | [ADR-0032](adr/0032-characters-are-parameter-vectors.md) |
| C15 | Character validator sweep | open | C13 | L | design | [09 §9.7](plan/09-testing-profiling.md#97-character-validators) |
| C16 | Archetype presets and bakes on promotion | open | C13, C24 | M | design | [05 §5.16](plan/05-simulation.md#516-characters-at-run-time) |
| C17 | Conform and morph-transfer tools | open | C6 | L | research | [ADR-0032](adr/0032-characters-are-parameter-vectors.md) |
| C18 | Surface binding record and Loop limit operator | done (728ea9a0) | C4 | M | design | [geometry](subsystems/geometry.md#limit-surfaces-and-surface-bindings) |
| C19 | Surface binding into production | open | C5, C18 | L | design | [geometry](subsystems/geometry.md#limit-surfaces-and-surface-bindings) |
| C20 | Cage-to-render binding stream in cluster pages | open | C38 | M | design | [07 §7.10](plan/07-content-pipeline.md#710-deformable-volume-assets) |
| C21 | E25's cage half | open | C20, S68 | M | research | [E25](experiments/e25-deformed-clusters.md) |
| C22 | Garments bound to the UV layout | open | C11, C19 | L | design | [07 §7.11](plan/07-content-pipeline.md#711-characters) |
| C23 | E27: one DAG over a base's parameter space | open | C8 | M | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| C24 | E28: what a baked character costs | open | C13 | M | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| C25 | E29: module boundary loops across LOD | open | C11 | M | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| C26 | E31: surface-bound garments against simulated | open | C22 | L | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| C27 | Tissue definition v0: schema, container, validators | done (8ea2de67) | | L | design | [tissue](subsystems/tissue.md#the-definition) |
| C28 | Interchange: ten-node cells, reference role, fixtures | done (2db7cd14, 5e7c8d1d) | C27 | M | routine | [tissue](subsystems/tissue.md#the-ten-node-cell) |
| C29 | Supine regression fixtures | done (2a209d00) | C28 | S | routine | [tissue](subsystems/tissue.md#the-supine-fixtures-imported-as-ten-node-reference-bodies) |
| C30 | P2/P1 element and the certified supine pair | done (a2efff1c) | C27 | L | research | [05 §5.16](plan/05-simulation.md#516-characters-at-run-time) |
| C31 | Certified standing equilibrium | deferred (05 §5.16 2026-09-26) | C37 | L | research | [05 §5.16](plan/05-simulation.md#516-characters-at-run-time) |
| C32 | Supine skin construction and audit | blocked (the owner's authorization of another offline run) | C29 | S | research | [05 §5.16](plan/05-simulation.md#516-characters-at-run-time) |
| C33 | Layered model records behind a capability gate | done (55628c61, 70c085b1) | C27 | M | design | [tissue](subsystems/tissue.md#the-layered-model) |
| C34 | Layered interchange remainder | open | C33 | M | design | [tissue](subsystems/tissue.md#not-yet) |
| C35 | Layered slab, S15 cycle | in progress | C33 | L | research | [05 §5.16](plan/05-simulation.md#516-characters-at-run-time) |
| C36 | Fusiform muscle matrix fixture | open | C35 | L | research | [05 §5.16](plan/05-simulation.md#516-characters-at-run-time) |
| C37 | Face-based contact with self-contact | open | C35 | L | research | [05 §5.16](plan/05-simulation.md#516-characters-at-run-time) |
| C38 | Runtime cage derived from a reference body | done (8a753dfd) | C29 | M | design | [tissue](subsystems/tissue.md#cage-derivation) |
| C39 | Soft body from a runtime region | done (573a97d0, e03f3dd4) | C38, S58 | M | routine | [tissue](subsystems/tissue.md#a-soft-body-from-a-runtime-region) |
| C40 | Settle gap to the certified supine state | open | C37 | L | research | [runtime cage](experiments/tissue-runtime-cage-2026-09-29.md) |
| C41 | Tissue runtime laws | open | C39 | L | design | [tissue](subsystems/tissue.md#not-yet) |
| C42 | Tissue element-kind registry | open | C41 | M | design | [05 §5.16](plan/05-simulation.md#516-characters-at-run-time) |
| C43 | Tissue validator rows owed | open | C27 | M | design | [tissue](subsystems/tissue.md#not-yet) |
| C44 | Equilibrium gap as a displacement | open | C41 | M | design | [tissue](subsystems/tissue.md#the-declared-energy-the-patch-test-and-the-equilibrium-gap) |
| C45 | Tissue in the deform chain | open | C13, C20, S68 | L | design | [05 §5.16](plan/05-simulation.md#516-characters-at-run-time) |
| C46 | E30: attachment-aware tissue against jiggle bones | in progress | C35, C40 | L | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| C47 | SDF adaptive lattice cage generator | open | | L | design | [07 §7.10](plan/07-content-pipeline.md#710-deformable-volume-assets) |
| C48 | Authored cage overrides | open | C47 | M | routine | [07 §7.10](plan/07-content-pipeline.md#710-deformable-volume-assets) |
| C49 | Deformable asset validators | open | C47 | M | routine | [07 §7.10](plan/07-content-pipeline.md#710-deformable-volume-assets) |
| C50 | Enforce `limits.max_velocity` | open | S58 | S | routine | [07 §7.10](plan/07-content-pipeline.md#710-deformable-volume-assets) |
| C51 | Edge compliance converted and measured | open | S58 | S | research | [tissue](subsystems/tissue.md#not-yet) |
| C52 | Tetrahedralizer for the tetrahedral cage kind | open | | M | research | [ADR-0026](adr/0026-deformable-volumes-first-class.md) |

## G — Content and generation

Import, the content build, validation, provenance, generators, scripting and text: [07 §7.1–§7.9](plan/07-content-pipeline.md), [content generation](content-generation.md), Phase 5 of [10 §10.2](plan/10-roadmap-risks.md#102-phases).

| # | Item | State | Depends on | Effort | Reasoning | Where |
|---|---|---|---|---|---|---|
| G1 | glTF import: meshes, materials, skins, animations, morphs | done (c0986bb5, 5d447bfc, 219d6690) | | M | routine | [assets](subsystems/assets.md#skins-and-animations) |
| G2 | glTF importer remainder | open | G1 | M | routine | [assets](subsystems/assets.md) |
| G3 | USD (LightUSD) and FBX interop | open | G1 | M | routine | [07 §7.2](plan/07-content-pipeline.md#72-source-formats-and-canonical-representations) |
| G4 | Parallel incremental content build and its cache | done (ad840b05) | | L | routine | [content_build](subsystems/content_build.md) |
| G5 | Edit-time rebuild and hot reload of one asset | open | D31 | L | design | [07 §7.3](plan/07-content-pipeline.md#73-content-build-the-derived-data-graph) |
| G6 | The same bytes on every toolchain | done (ADR-0035, a278fa66) | G4 | M | research | [ADR-0035](adr/0035-no-floating-point-contraction.md) |
| G7 | Repacked bytes across C libraries | open | G6, G21 | S | design | [determinism](experiments/content-build-determinism.md#follow-ups) |
| G8 | Built textures in the engine's container | done (ADR-0036, 1f760059) | G4 | L | design | [texture](subsystems/texture.md) |
| G9 | HDR textures: EXR sources and BC6H | open | G8 | M | routine | [texture](subsystems/texture.md#not-yet) |
| G10 | KTX2 exporter | deferred (ADR-0036) | G8 | S | routine | [ADR-0036](adr/0036-built-textures-in-the-engines-own-container.md) |
| G11 | Alpha coverage kept through the mips | open | G8 | S | routine | [texture](subsystems/texture.md#not-yet) |
| G12 | Texel density per asset category | open | G14 | M | design | [atlas](subsystems/atlas.md#not-yet) |
| G13 | Build validation v0 | done (ad840b05) | G4 | M | routine | [07 §7.4](plan/07-content-pipeline.md#74-validation-rules-automatic) |
| G14 | Category budgets: triangles and per-asset cost | open | G13 | M | design | [07 §7.4](plan/07-content-pipeline.md#74-validation-rules-automatic) |
| G15 | Fragmentation and collapse as build validators | open | G16, G32 | M | design | [07 §7.4](plan/07-content-pipeline.md#74-validation-rules-automatic) |
| G16 | Atlas statistics and the collapse check | done (9202a328, 780aae4c) | | M | research | [E10](experiments/e10-generated-props.md#the-collapse-check-how-far-the-lod-collapses) |
| G17 | UV-degenerate repair | done (c23f71ec) | G13 | S | routine | [geometry](subsystems/geometry.md#uv-degenerate-triangles-the-repair) |
| G18 | Geometry and material validators, the rest of §7.4 | open | G14 | M | routine | [07 §7.4](plan/07-content-pipeline.md#74-validation-rules-automatic) |
| G19 | Placement validators | open | S29 | M | design | [07 §7.4](plan/07-content-pipeline.md#74-validation-rules-automatic) |
| G20 | Reference boards and visual validators | open | | M | research | [07 §7.7](plan/07-content-pipeline.md#77-ml-asset-generation-novelty-content) |
| G21 | Atlas repack and rebake | done (9465a07f) | G16 | L | design | [atlas](subsystems/atlas.md) |
| G22 | Atlas repack improvements | open | G21 | M | research | [atlas](subsystems/atlas.md#not-yet) |
| G23 | Decimate-and-bake repair | open | G21 | M | design | [atlas](subsystems/atlas.md#not-yet) |
| G24 | De-lighting repair for generated albedos | open | | M | research | [07 §7.7](plan/07-content-pipeline.md#77-ml-asset-generation-novelty-content) |
| G25 | E10 first passes: 80 generated props | done (10 §10.5 2026-09-23) | G16 | L | research | [E10](experiments/e10-generated-props.md) |
| G26 | E10 remainder: 200 props and a repair yield | open | G23, G25 | L | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| G27 | Generator tool and its credit rules | done (5d57bb7a, 4c2cf66c) | | L | routine | [content generation](content-generation.md#meshy-and-the-credit-rules) |
| G28 | Generator service inside the engine | open | G27 | L | design | [content generation](content-generation.md#not-yet) |
| G29 | Generator tool remainder | open | G27 | S | routine | [content generation](content-generation.md#not-yet) |
| G30 | PBR material generation backend | open | G27 | M | research | [07 §7.7](plan/07-content-pipeline.md#77-ml-asset-generation-novelty-content) |
| G31 | Provenance schema and sidecars | done (5d57bb7a, 3fa9e9ef) | | M | routine | [content generation](content-generation.md#the-provenance-sidecar-enginegenerationprovenance1) |
| G32 | The build reads sidecars, enforces the allowlist | open | G31, G33 | M | routine | [07 §7.5](plan/07-content-pipeline.md#75-provenance-and-licensing-metadata) |
| G33 | Licence review of generation services and models | blocked (the owner's review) | G31 | S | design | [content generation](content-generation.md#the-local-models-licences-as-published) |
| G34 | Shipping attribution report, ship-right check | open | G32 | S | routine | [07 §7.5](plan/07-content-pipeline.md#75-provenance-and-licensing-metadata) |
| G35 | Scene-generator registry | done (ADR-0046) | | L | design | [scene_gen](subsystems/scene_gen.md#adding-a-generator) |
| G36 | Heightfield terrain: erosion, hydrology, biomes | open | G35 | L | research | [07 §7.6](plan/07-content-pipeline.md#76-procedural-generation-volume-content) |
| G37 | Scatter generator | open | G35 | M | design | [07 §7.6](plan/07-content-pipeline.md#76-procedural-generation-volume-content) |
| G38 | Roads and rivers as spline networks | open | G36 | L | design | [07 §7.6](plan/07-content-pipeline.md#76-procedural-generation-volume-content) |
| G39 | Ruin assembler and block layer | done (15b32ade, f36f1d94) | G35 | L | design | [ruins](subsystems/ruins.md#the-block-layer) |
| G40 | Hard-surface kits in Blender (E33) | done (10 §10.5 2026-09-24) | | L | research | [E33](experiments/e33-hard-surface-kits.md) |
| G41 | An ADR for where kit scripts live | open | G40 | S | design | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| G42 | Ruins' far tier: the kit's slab | open | G23, G40 | M | design | [E33](experiments/e33-hard-surface-kits.md#what-it-decides) |
| G43 | Kit schema and ashlar kit fixes | open | G39, G40 | M | design | [E33](experiments/e33-hard-surface-kits.md#what-the-assemblers-kit-schema-cannot-say) |
| G44 | Clutter and furniture generator | open | P22 | M | design | [city](subsystems/city.md#what-is-left-open) |
| G45 | E7, the Luau arm | done (0a6d80c4) | | L | research | [E7](experiments/e7-luau-spike.md) |
| G46 | E7, the C++ hot-reload arm | open | | L | research | [08 §8.2](plan/08-toolchain.md#82-gameplay-and-scripting-layer) |
| G47 | E7, the C# arm or a decision from the C++ arm | open | G46 | M | research | [08 §8.2](plan/08-toolchain.md#82-gameplay-and-scripting-layer) |
| G48 | Scripting language decision | deferred (10 §10.6) | G46, G47 | S | design | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |
| G49 | Script tooling: check, types, mutations | open | G45 | M | design | [scripting](subsystems/scripting.md#not-yet) |
| G50 | Runtime LLM interface at Tier 0 | open | | M | design | [07 §7.8](plan/07-content-pipeline.md#78-runtime-llm-interface-now-implementation-deferred) |
| G51 | E8: a runtime model on spare capacity | deferred (10 §10.6) | G50 | L | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| G52 | Player-facing strings and localization | open | | M | design | [07 §7.9](plan/07-content-pipeline.md#79-text-and-localization) |
| G53 | Build-time TTS | open | G52 | M | research | [07 §7.7](plan/07-content-pipeline.md#77-ml-asset-generation-novelty-content) |
| G54 | Procedural tree solver | open | R65, S66 | L | research | [05 §5.15](plan/05-simulation.md#515-capability-inventory) |
| G55 | Vegetation growth, ecology and placement | open | G37, S7 | L | design | [05 §5.15](plan/05-simulation.md#515-capability-inventory) |
| G56 | Phase 5 exit | open | G15, G28, G32, G37 | L | design | [10 §10.2](plan/10-roadmap-risks.md#102-phases) |

## T — Tooling and CI

Build, CI, test and profiling infrastructure, and the machinery behind the standing rules: [08](plan/08-toolchain.md), [09](plan/09-testing-profiling.md), [11](plan/11-performance-principles.md), [docs/ci](ci/what-to-run.md).

| # | Item | State | Depends on | Effort | Reasoning | Where |
|---|---|---|---|---|---|---|
| T1 | Phase 0 core modules | done (22c31122, 3141af59, 0d3a1ebe, 1895e4e2) | | L | routine | [10 §10.2](plan/10-roadmap-risks.md#102-phases) |
| T2 | Topology-aware pinned job pools | done (3141af59, ADR-0011) | | M | design | [08 §8.7](plan/08-toolchain.md#87-hardware-characterization-scaled-to-fit) |
| T3 | Linux topology detection verified on hardware | open | T2 | S | routine | [platform](subsystems/platform.md) |
| T4 | Telemetry: queryable events and budget violations | open | | M | design | [09 §9.3](plan/09-testing-profiling.md#93-profiling-infrastructure-before-optimizing-anything) |
| T5 | I/O, tunables registry and bench harness | done (7436f951, ac9ecdb1) | | M | routine | [bench](subsystems/bench.md) |
| T6 | Tracy client, loopback only | done (152ea353, 3f8e0c46) | | S | routine | [profiling](subsystems/profiling.md) |
| T7 | CI on Windows and Linux, per-test scratch | done (152ea353, 80ac9244) | | M | routine | [08 §8.5](plan/08-toolchain.md#85-build-system-and-ci) |
| T8 | Repository conventions, layer check, AGENTS.md | done (22c31122) | | M | design | [02 §2.5](plan/02-architecture.md#25-repository-organization-for-agent-comprehension) |
| T9 | Phase 0 actuals and the first rescale | done (7c152a5c) | | S | routine | [10 §10.8](plan/10-roadmap-risks.md#108-calibrating-the-estimates) |
| T10 | Phase 1 actuals and the rendering rescale | open | R36 | S | routine | [10 §10.8](plan/10-roadmap-risks.md#108-calibrating-the-estimates) |
| T11 | Sanitizer builds in CI | done (2026-10-09: linux-clang-asan 96 of 96, the job blocks) | T7 | M | routine | [what to run](ci/what-to-run.md#sanitizers) |
| T12 | clang-cl in CI, and its contraction flag | open | T7 | S | routine | [determinism](experiments/content-build-determinism.md#follow-ups) |
| T13 | Fuzzing of parsers, the protocol and saves | open | T11 | L | design | [09 §9.1](plan/09-testing-profiling.md#91-test-taxonomy) |
| T14 | Defect-rate tracking per phase | open | T11 | S | design | [08 §8.1](plan/08-toolchain.md#81-language) |
| T15 | Lint: raw new and delete, C arrays, unchecked indexing | open | | S | routine | [08 §8.1](plan/08-toolchain.md#81-language) |
| T16 | clang-tidy run by the build | open | | M | routine | [08 §8.5](plan/08-toolchain.md#85-build-system-and-ci) |
| T17 | Banned-pattern lint | done (22c31122, 2425c624, 4c2bb759, ee884230) | | M | routine | [AGENTS.md](../AGENTS.md#rules-that-are-checked) |
| T18 | Hot-type size table | done (22c31122, ADR-0019) | | S | routine | [11 §11.2](plan/11-performance-principles.md#112-data-layout-and-footprint-first) |
| T19 | Documentation check and the docs gate | done (6f64f29c, 3a852ad7) | | M | routine | [02 §2.5](plan/02-architecture.md#25-repository-organization-for-agent-comprehension) |
| T20 | Licence check in CI | done (2026-10-07, tools/license-check.ps1) | | M | design | [what to run](ci/what-to-run.md#licences) |
| T21 | CPU baseline v3, with v2 test presets | done (ADR-0031) | | M | design | [08 §8.9](plan/08-toolchain.md#89-the-cpu-baseline-and-what-each-dependency-does-about-it) |
| T22 | Retire the v2 presets | deferred (ADR-0031) | T21 | S | routine | [ADR-0031](adr/0031-minimum-cpu-x86-64-v3.md) |
| T23 | Host tools build for the build machine | done (ADR-0034) | T21 | S | design | [ADR-0034](adr/0034-host-tools-build-for-the-build-machine.md) |
| T24 | Capability scaffold, capability-graph presets | done (ADR-0027, c24677d7) | | M | design | [02 §2.8](plan/02-architecture.md#28-adding-a-capability) |
| T25 | Compiler cache; container rebuild keeps third party | open | T26 | M | design | [local Linux](ci/local-linux.md#follow-ups) |
| T26 | Local Linux container gate | done (df922466) | T7 | M | routine | [local Linux](ci/local-linux.md) |
| T27 | Remote build on the Titan Xp | done (f25ffe8e) | | M | routine | [remote Linux](ci/remote-linux.md#the-script) |
| T28 | `remote-build.ps1 -Prune` for old trees | open | T27 | S | routine | [remote Linux](ci/remote-linux.md#the-script) |
| T29 | Clear the Titan Xp's old remote trees (309 GB) | open | T28 | S | routine | [remote Linux](ci/remote-linux.md) |
| T30 | Affected-tests runner | done (fe941437) | | M | design | [what to run](ci/what-to-run.md#what-affected-means) |
| T31 | Test-link edges in modules.json | open | T30 | M | design | [what to run](ci/what-to-run.md#what-affected-means) |
| T32 | Test bundle for machines with no toolchain | done (280bc5c3) | | M | routine | [runners](ci/self-hosted-runners.md#trying-the-engine-on-a-machine-with-no-toolchain) |
| T33 | Self-hosted runner scripts and the GPU workflow | done (c25bb03c) | | M | routine | [runners](ci/self-hosted-runners.md#the-gpu-workflow) |
| T34 | Register the two baseline-tier runners | blocked (the Maxwell machine's power supply) | T33 | S | routine | [runners](ci/self-hosted-runners.md#registering-a-runner) |
| T35 | Register an RTX runner | blocked (no RTX machine offered as a runner) | T33 | S | routine | [runners](ci/self-hosted-runners.md#registering-a-runner) |
| T36 | An AMD GPU runner | blocked (no AMD machine) | T33 | S | routine | [08 §8.5](plan/08-toolchain.md#85-build-system-and-ci) |
| T37 | Extreme-resolution captures in the GPU smoke run | done (4a9845e7) | T33 | S | routine | [runners](ci/self-hosted-runners.md#the-extreme-resolutions) |
| T38 | Extreme resolutions nightly and offscreen | open | T34, T37 | S | routine | [runners](ci/self-hosted-runners.md#the-extreme-resolutions) |
| T39 | Machine-wide GPU lock, per device | done (d4867e07) | | M | design | [gpu_lock](subsystems/gpu_lock.md) |
| T40 | Accept ADR-0049 | done (2026-10-07, the owner) | T39 | S | design | [ADR-0049](adr/0049-tests-take-the-gpu-lock-per-device.md) |
| T41 | Measurements that record the machine's state | done (f174fefc) | | S | routine | [bench](subsystems/bench.md#measuring-on-a-shared-machine) |
| T42 | No-listeners check | done (6e6f1053) | | S | routine | [runners](ci/self-hosted-runners.md#toolscicheck-no-listenersps1) |
| T43 | No dialogs on failure | done (4b4c2a89, c75d4e7a) | | M | routine | [platform](subsystems/platform.md#no-engine-binary-opens-a-dialog-when-it-fails) |
| T44 | Profiling service: frame report, capture export | open | T6 | M | design | [09 §9.3](plan/09-testing-profiling.md#93-profiling-infrastructure-before-optimizing-anything) |
| T45 | Tracy zones everywhere, GPU zones, vendor counters | open | T44, R35 | M | routine | [profiling](subsystems/profiling.md) |
| T46 | Frame-budget files as merge gates | done (2026-10-07, content/budgets/frame-budgets.json) | | M | design | [bench](subsystems/bench.md#frame-budgets) |
| T47 | Nightly performance regression with significance tests | open | T35, T46 | M | design | [09 §9.1](plan/09-testing-profiling.md#91-test-taxonomy) |
| T48 | Allocation counter as a frame-loop CI metric | done (2026-10-07, engine_view.frame_loop) | | S | routine | [renderer](subsystems/renderer.md#the-frame-loop-allocates-nothing) |
| T49 | A "nothing enabled" benchmark frame | open | R30 | M | design | [11 §11.10](plan/11-performance-principles.md#1110-absent-capabilities-are-free) |
| T50 | Per-kernel instantiation and code-size report | open | | S | routine | [11 §11.4](plan/11-performance-principles.md#114-branch-free-hot-paths-and-constexpr-dispatch) |
| T51 | E3: runtime-selected kernel variants | open | T5 | M | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| T52 | Calibration runner, stage 3 | deferred (ADR-0011) | T51 | L | design | [ADR-0011](adr/0011-tunables-before-calibration.md) |
| T53 | Divergence bisector and replay methods | open | S17 | M | design | [09 §9.2](plan/09-testing-profiling.md#92-determinism-infrastructure) |
| T54 | Publishing plan | done (388cc7c4) | | S | design | [publishing](publishing.md) |
| T65 | Allocation count in a window's frame loop | open | T48 | S | routine | [renderer](subsystems/renderer.md#the-frame-loop-allocates-nothing) |
| T66 | `msvc-asan` built and tested in CI | open | T11 | S | routine | [what to run](ci/what-to-run.md#sanitizers) |
| T67 | Frame budgets on the baseline-tier machines | open | T34, T46 | S | routine | [bench](subsystems/bench.md#frame-budgets) |
| T55 | Overlay directory found by the build | open | T54 | M | design | [publishing](publishing.md#a-private-and-experimental-capabilities-and-content-an-overlay-repository) |
| T56 | History audit, private mirror, first public push | open | T55 | M | design | [publishing](publishing.md#c-before-the-first-public-push) |
| T57 | ARM64 CI target | deferred (10 §10.6 2026-09-25) | | M | design | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |
| T58 | macOS and AppleClang preset | deferred (10 §10.6 2026-09-25) | | S | routine | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |
| T59 | AI-policy review at the Phase 4 and 5 starts | open | | S | routine | [12 §12.5](plan/12-ai-usage-policy.md#125-review-points) |
| T60 | Bazel | deferred (10 §10.6) | | M | design | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |
| T61 | Console targets | deferred (10 §10.6 2026-09-25) | R2, T57 | L | design | [10 §10.6](plan/10-roadmap-risks.md#106-decisions-deliberately-deferred) |
| T62 | Linux build volumes labelled by checkout, and a stale-volume sweep | done (2026-10-07, `linux-build.ps1 -Prune -Stale`) | T26 | S | routine | [local Linux](ci/local-linux.md#volumes-that-outlive-their-checkout) |

## A — The agent protocol and tools

The engine as a server, its clients, and what agents and the human director work through: [06](plan/06-agent-tooling.md), Phase 4 of [10 §10.2](plan/10-roadmap-risks.md#102-phases).

| # | Item | State | Depends on | Effort | Reasoning | Where |
|---|---|---|---|---|---|---|
| A1 | Engine protocol: JSON-RPC over stdio, sessions | done (ADR-0001, 834d4bc4) | D4 | L | routine | [protocol](subsystems/protocol.md) |
| A2 | WebSocket transport, bulk channel, subscriptions | open | A1 | L | design | [06 §6.2](plan/06-agent-tooling.md#62-engine-protocol) |
| A3 | One host shared by several clients | open | A2 | L | design | [06 §6.3](plan/06-agent-tooling.md#63-mcp-bridge) |
| A4 | Protocol authentication | open | A3 | M | design | [protocol](subsystems/protocol.md#roles-leases-and-proposals) |
| A5 | MCP bridge (engine-mcp) | done (589795a8) | A1 | L | design | [apps](subsystems/apps.md#engine-mcp-the-mcp-bridge) |
| A6 | engine-mcp remainder | open | A5 | M | design | [apps](subsystems/apps.md#not-yet) |
| A7 | Day-one operations | done (9a34cca1) | A1 | L | routine | [protocol](subsystems/protocol.md#the-day-one-operations) |
| A8 | render.capture, benchmark, compare, evaluate | done (da851a1a, eb0a4b47) | R34 | L | routine | [protocol](subsystems/protocol.md#methods-an-app-registers-and-render) |
| A9 | More capture channels and a contact sheet | open | A8 | M | routine | [06 §6.6](plan/06-agent-tooling.md#66-introspection) |
| A10 | `profile` over the protocol | open | T44 | M | routine | [06 §6.9](plan/06-agent-tooling.md#69-day-one-operations) |
| A11 | Explicit begin, commit and rollback | open | A1 | S | routine | [06 §6.9](plan/06-agent-tooling.md#69-day-one-operations) |
| A12 | `place_asset` and `instantiate_template` | open | A1 | M | design | [06 §6.9](plan/06-agent-tooling.md#69-day-one-operations) |
| A13 | `describe` an object, tile or region by detail | open | A1 | M | design | [06 §6.6](plan/06-agent-tooling.md#66-introspection) |
| A14 | `query`: SQL over the store, filters over the document | open | D16 | M | design | [06 §6.6](plan/06-agent-tooling.md#66-introspection) |
| A15 | Semantic summaries of diffs | open | D5 | M | design | [06 §6.4](plan/06-agent-tooling.md#64-transactions-checkpoints-rollback) |
| A16 | Validation service v0 | done (9a34cca1, e0639a1c) | A7 | M | routine | [protocol](subsystems/protocol.md#enginerun_tests) |
| A17 | The content validators of §6.6 | open | A16 | L | design | [06 §6.6](plan/06-agent-tooling.md#66-introspection) |
| A18 | Roles, leases and proposal layers | done (e0639a1c, aa536d4b) | D6 | L | design | [protocol](subsystems/protocol.md#roles-leases-and-proposals) |
| A19 | E12: proposal layers and leases | done (10 §10.5 2026-09-25) | A18 | M | research | [E12](experiments/e12-proposal-layers-and-leases.md) |
| A20 | Accept ADR-0039 | blocked (the owner's review) | A18 | S | design | [ADR-0039](adr/0039-leases-and-proposals-beside-the-document.md) |
| A21 | World mips: semantic, visual, design density | open | A8, D32 | L | research | [06 §6.7](plan/06-agent-tooling.md#67-edit-context-and-world-mips) |
| A22 | Edit context: cell, overlap ring, token budget | open | A18, A21 | L | design | [06 §6.7](plan/06-agent-tooling.md#67-edit-context-and-world-mips) |
| A23 | Task ledger and review items | open | | M | design | [06 §6.8](plan/06-agent-tooling.md#68-long-running-work) |
| A24 | Editor client v0 with a review queue | open | A2, A15, A23 | L | design | [06 §6.10](plan/06-agent-tooling.md#610-multi-agent-roles-review-and-the-human-director) |
| A25 | The editor as a complete tool suite | open | A24 | L | design | [06 §6.13](plan/06-agent-tooling.md#613-human-developer-tooling) |
| A26 | View policies applied to captures and reviews | open | C3, A23 | M | design | [ADR-0033](adr/0033-content-classes-and-view-policies.md) |
| A27 | E5: one agent builds a small level | open | A12, A13, D14 | M | research | [10 §10.5](plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) |
| A28 | Phase 4 exit | open | A24, A27 | L | research | [10 §10.2](plan/10-roadmap-risks.md#102-phases) |
| A29 | Automated playtesting: bots, telemetry, fuzzing | open | A7, T4 | L | design | [06 §6.11](plan/06-agent-tooling.md#611-automated-playtesting) |
| A30 | Hierarchical world generation, boundary contracts | open | A22, A23, D32, D33 | L | research | [06 §6.12](plan/06-agent-tooling.md#612-hierarchical-world-generation) |

## P — The reference games

Desert Survival first, Island City second ([ADR-0022](adr/0022-consumer-game-order.md)): [13](plan/13-reference-consumer-games.md), Phases 6 and 7 of [10 §10.2](plan/10-roadmap-risks.md#102-phases).

| # | Item | State | Depends on | Effort | Reasoning | Where |
|---|---|---|---|---|---|---|
| P1 | Survival meters: thirst, hunger, exposure | deferred (13 §13.1 2026-09-25) | P2 | M | design | [13 §13.1](plan/13-reference-consumer-games.md#survival-mechanics) |
| P2 | Shelter query: shade and wind cover | open | W34 | M | design | [13 §13.1](plan/13-reference-consumer-games.md#survival-mechanics) |
| P3 | Water and food sources | open | P8, P9 | M | design | [13 §13.1](plan/13-reference-consumer-games.md#survival-mechanics) |
| P4 | Dead wood and the campfire | deferred (13 §13.1 2026-09-25) | S64, R47 | M | design | [13 §13.1](plan/13-reference-consumer-games.md#survival-mechanics) |
| P5 | Sand slides under the player | open | W29, W39 | L | research | [13 §13.1](plan/13-reference-consumer-games.md#dynamic-sand-the-defining-system) |
| P6 | Storm dust as gameplay visibility | open | W34 | M | design | [13 §13.1](plan/13-reference-consumer-games.md#day-night-and-weather) |
| P7 | Wreck fire, clouds and storm as one medium | open | R47 | L | research | [13 §13.1](plan/13-reference-consumer-games.md#day-night-and-weather) |
| P8 | Oases as rendering islands | open | R65, S63 | L | research | [13 §13.1](plan/13-reference-consumer-games.md#oases) |
| P9 | Desert creatures | open | | L | design | [13 §13.5](plan/13-reference-consumer-games.md#135-mapping-to-the-roadmap) |
| P10 | Ruins' gameplay roles | open | G39, P2 | M | design | [13 §13.1](plan/13-reference-consumer-games.md#ruins) |
| P11 | The game's seeded oases, ruins and climate | open | G35, P18 | M | design | [13 §13.1](plan/13-reference-consumer-games.md#concept) |
| P12 | View distance as a tunable with a cost model | open | W15 | M | design | [13 §13.1](plan/13-reference-consumer-games.md#visibility-and-long-distance-rendering) |
| P13 | First-person walk on the dunes | done (bd18812d) | S24 | M | routine | [apps](subsystems/apps.md#walking) |
| P14 | Third-person and isometric camera rigs | open | P13 | L | design | [13 §13.1](plan/13-reference-consumer-games.md#camera-modes) |
| P15 | Optional cosmetic character creator | open | C7, C12 | L | design | [13 §13.1](plan/13-reference-consumer-games.md#player-character-creator-optional) |
| P16 | Scored seeded runs, run database, leaderboards | open | S17, W11 | L | design | [13 §13.1](plan/13-reference-consumer-games.md#seeds-and-scoring) |
| P17 | Accept ADR-0047: licences, trademark, counsel | blocked (the owner's reading, trademark and counsel) | | S | design | [ADR-0047](adr/0047-reference-games-are-licensed-apart-from-the-engine.md) |
| P18 | Desert Survival's repository, pinning the engine | open | P17 | M | routine | [ADR-0047](adr/0047-reference-games-are-licensed-apart-from-the-engine.md) |
| P19 | Desert Survival's open design questions | blocked (the owner's answers) | | S | design | [13 §13.8](plan/13-reference-consumer-games.md#138-open-design-questions-for-the-owner) |
| P20 | Phase 6 exit demo, made playable | open | P1, P5, P16, S85, W39 | L | design | [10 §10.2](plan/10-roadmap-risks.md#102-phases) |
| P21 | A 1–3 hour game, shipped with telemetry | open | P20, A29, A30, T4 | L | design | [10 §10.2](plan/10-roadmap-risks.md#102-phases) |
| P22 | Whole-island plan and building grammar | done (a973f39f, a0920873) | G35 | L | design | [city](subsystems/city.md) |
| P23 | E18: the building grammar's yield | done (10 §10.5 2026-09-25) | P22 | M | research | [E18](experiments/e18-island-city-yield.md) |
| P24 | The owner's fly-through over the city proxies | blocked (the owner's session) | P22 | S | design | [ADR-0044](adr/0044-island-city-plan-and-building-grammar.md) |
| P25 | Accept ADR-0044 | blocked (the owner's review, after P24) | P24 | S | design | [ADR-0044](adr/0044-island-city-plan-and-building-grammar.md) |
| P26 | The grammar reads span and bed clearance | open | P22 | S | routine | [E18](experiments/e18-island-city-yield.md#what-it-decides) |
| P27 | City kits and a facade far tier | open | G42, G43 | L | design | [city](subsystems/city.md#what-is-left-open) |
| P28 | Point towers, courtyards, L footprints, a station | open | P22 | M | design | [city](subsystems/city.md#what-is-left-open) |
| P29 | Island mountain, wilderness and ocean | open | G36, S63 | L | design | [13 §13.2](plan/13-reference-consumer-games.md#mountain-wilderness-and-ocean) |
| P30 | Interior cells: unloaded, coarse, active | open | P22 | L | design | [13 §13.2](plan/13-reference-consumer-games.md#persistent-interior-identity-streaming-and-materialization) |
| P31 | Windows and partial interior visibility | open | P27, P30 | L | research | [13 §13.2](plan/13-reference-consumer-games.md#windows-and-partial-interior-visibility) |
| P32 | Elevators and vertical streaming | open | P30 | L | design | [13 §13.2](plan/13-reference-consumer-games.md#elevators-and-vertical-streaming) |
| P33 | Residents housed by the city's occupancy | open | P30, S13 | M | design | [13 §13.2](plan/13-reference-consumer-games.md#dense-npc-simulation) |
| P34 | Downtown density benchmark scenes | open | P22, R30 | M | routine | [13 §13.2](plan/13-reference-consumer-games.md#dense-asset-and-renderer-stress) |
| P35 | Destroying an occupied tower | open | P33, S48, S53 | L | design | [13 §13.2](plan/13-reference-consumer-games.md#destruction-and-consequences) |
| P36 | E15: GI stability under a moving sun | open | R69 | M | research | [13 §13.6](plan/13-reference-consumer-games.md#136-additional-experiments) |
| P37 | E16: interior tiers of a 60-floor tower | open | P30, P32 | M | research | [13 §13.6](plan/13-reference-consumer-games.md#136-additional-experiments) |
| P38 | E17: ray-traced relevance through 2,000 windows | open | P31, P34 | M | research | [13 §13.6](plan/13-reference-consumer-games.md#136-additional-experiments) |
| P39 | Island City's open design questions | blocked (the owner's answers) | | S | design | [13 §13.8](plan/13-reference-consumer-games.md#138-open-design-questions-for-the-owner) |
| P40 | R1: reaching into a cavity, as a scene | open | S83 | L | research | [13 §13.7](plan/13-reference-consumer-games.md#r1--reaching-into-a-fleshy-cavity-third-party-reference-lovecraftian-horror) |

## F — Found along the way

Defects that no plan section schedules: failures seen at gates, steps in a picture, a wait nobody asked for. A fixed one stays as a `done` row while a page still describes it as open.

| # | Item | State | Depends on | Effort | Reasoning | Where |
|---|---|---|---|---|---|---|
| F1 | Terrain-rings culling test fails under load | done (2026-10-07: an offscreen frame gave up at a ring's second ready field) | | M | research | [load-sensitive tests](experiments/load-sensitive-tests-2026-10-07.md#f1-one-rig-a-frame-behind) |
| F2 | Live-window marker test fails under load | done (2026-10-07: the injector ran slow frames past its events) | | S | research | [load-sensitive tests](experiments/load-sensitive-tests-2026-10-07.md#f2-a-tick-the-window-never-ran-and-a-frame-rate-the-test-assumed) |
| F3 | Sky's haze table ends at 32 km: a horizon step | open | R55 | S | design | [far ground](experiments/far-ground-2026-10-03.md#what-is-open) |
| F4 | Traced-shadow specks and shards on the far ground | done (2026-10-09: the shards were a surround's views at different LOD in one structure; every view takes the finest view's cut when the frame builds structures; the specks left are F17) | R52, W15 | M | research | [shadow shards](experiments/shadow-shards-2026-10-09.md) |
| F5 | Windowed submit waits 3.4 ms under traced shadows | open | R52 | S | research | [frame-thread spikes](experiments/frame-thread-spikes-2026-10-04.md#what-is-open) |
| F6 | Outer ring rebuilds twice the tiles with far levels on | open | W15 | M | design | [far ground](experiments/far-ground-2026-10-03.md#what-is-open) |
| F7 | Time-lapse drops a moved window's fields | open | W31 | M | design | [renderer](subsystems/renderer.md#not-yet) |
| F8 | Back-to-back sheets: a facing bit in the visibility id | open | R11 | S | design | [visible order](experiments/visible-order.md#what-it-decides) |
| F9 | schemac refused `@unit` on optional numbers | done (bbe6544b) | D2 | S | routine | [schemas](../schemas/README.md#the-language) |
| F10 | Device lost when a tight budget evicts beside the RT chain | done (7d794636) | R9 | S | research | [renderer](subsystems/renderer.md#a-pages-8-bit-indices-are-its-own) |
| F11 | Tile index narrowed to i32 unchecked in TileSampler::tile_at and scene_collision | open | W64 | S | routine | [tile.cpp](../domain/terrain/src/tile.cpp) |
| F12 | gfx.md says only world tiles carry k_instance_uv_from_corner; ring chunks do too | open | W63 | S | routine | [gfx](subsystems/gfx.md#the-frames-origin) |
| F13 | Tunables read across translation units before they are initialized | done (2026-10-09: not a defect; strict mode reports reads of initialized globals by design, and non-strict reports nothing) | | M | design | [what to run](ci/what-to-run.md#sanitizers) |
| F14 | Heap use after free in the texture tests under ASan | done (2026-10-09: the test pushed a vector's own element; llvm-symbolizer in the image named it) | | S | research | [what to run](ci/what-to-run.md#sanitizers) |
| F15 | Third-party undefined behaviour under UBSan: suppress or exclude | done (2026-10-09: cmake/sanitizer-ignorelist.txt, one file and one check per entry) | | S | routine | [what to run](ci/what-to-run.md#sanitizers) |
| F16 | Fault-probe tests under the sanitizers | done (2026-10-09: under a sanitizer the test checks that the engine stands aside) | | S | routine | [what to run](ci/what-to-run.md#sanitizers) |
| F17 | Traced-shadow specks at a far crease in one view: a ray from its own cluster | open | F4 | S | research | [shadow shards](experiments/shadow-shards-2026-10-09.md#what-is-left) |
| F18 | Surround side monitors keep the layout's field of view, not the camera's | open | | S | design | [renderer](subsystems/renderer.md#multi-view-a-viewset-over-one-scene) |
| F19 | Dune ripples' yesterday shift overflowed an `int` | done (2026-10-09: a whole day's seconds as an i64; the next day's yesterday held to the last second's shift) | | S | routine | [terrain](subsystems/terrain.md#the-time-function) |

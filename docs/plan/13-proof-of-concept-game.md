# 13 — Proof-of-Concept Game: Desert Survival (working title)

Status: concept recorded 15 September 2026 from the project owner's description. It is a design target that gives the roadmap a concrete consumer, not a commitment to build it before the engine can carry it. Nothing here is implemented.

## 13.1 The concept

Single player. You start at a plane crash in an infinite, procedurally generated desert. Sand dunes to the horizon; small ruined structures sparsely scattered; occasional sand-dwelling critters. Day turns into night. Wind slowly moves the dunes, and sand accumulates against the ruins. Walking leaves footprints and trails; walking on a dune slope sets off small, realistic sand slides. Cameras are first person, third person, and isometric, toggleable at any time. Very rare oases, visible from far away, have water. Thirst and hunger tick down; oasis water restores thirst, catching and eating critters restores hunger and a little thirst, and the ruins sometimes hold food or water. The objective is to survive; the score is how long you lasted.

It is a technical demo first, but the shape (a seeded survival run with a score) has a niche that would play it seriously.

## 13.2 Why this is the right first game

- **Small gameplay surface, large systems surface.** Almost no gameplay code, but it exercises terrain, deformation, procedural generation, streaming of an unbounded world, time-of-day lighting, wildlife at simulation LOD tiers, the observer set and camera rigs, persistence of world modifications, tunables, and per-run telemetry.
- **No narrative, no dialogue, no runtime model.** None of the hardest content problems (canon, quests, characters) and nothing that touches [12-ai-usage-policy](12-ai-usage-policy.md). The systemic-world thesis is tested without the narrative risk.
- **Bounded art.** Sand, sky, a wreck, a ruin kit, an oasis kit, a few critters, one player character. Most of it is procedural or kit-based, which is what agents are good at producing and validating ([07 §7.6](07-content-pipeline.md#76-procedural-generation-volume-content)).
- **Determinism pays off directly.** A run is (seed, input log). Same seed, same world; a score can be verified by replay ([05 §5.10](05-simulation.md#510-determinism-and-replay)), which is what a leaderboard for the serious niche needs.
- **It answers the risk row "engine generality without a game"** ([10 §10.4](10-roadmap-risks.md#104-major-technical-risks)) two phases earlier than the narrative game would.

## 13.3 What the plan already covers

| Feature | Where |
|---|---|
| Dune heightfield rendering with clipmap LOD and virtual texturing | [04 §4.3](04-renderer.md#43-geometry) |
| Footprints, trails, decay toward baseline, coarse deterministic CPU grid for gameplay | [05 §5.13](05-simulation.md#513-deformable-surfaces-and-soft-bodies) |
| Tile grid, tile-relative positions, camera-relative rendering at any world size | [03 §3.7](03-data-model.md#37-spatial-partition), [02 §2.7](02-architecture.md#27-generality-what-the-engine-must-not-preclude) |
| Critters at LOD tiers driven by the observer set | [05 §5.4](05-simulation.md#54-lod-tier-assignment) |
| Day length and game time as integer `GameTime`; timing wheel for slow processes | [02 §2.4](02-architecture.md#24-frame-and-time-model), [05 §5.3](05-simulation.md#53-event-scheduler-temporal-lod) |
| Camera modes as rigs over one observer; multi-view `ViewSet` | [05 §5.12](05-simulation.md#512-multiplayer-readiness), [04 §4.6](04-renderer.md#46-extreme-displays) |
| Per-tile persistent state, event log, saves as snapshot plus tail | [03 §3.5](03-data-model.md#35-persistent-world-state) |
| Generators as `derived` nodes with document-object parameters | [07 §7.6](07-content-pipeline.md#76-procedural-generation-volume-content) |
| Run telemetry and the run database | [06 §6.11](06-agent-tooling.md#611-automated-playtesting) |
| View distance and every other limit as a tunable | [11 §11.6](11-performance-principles.md#116-no-hidden-limits) |

## 13.4 Requirements this adds or sharpens

1. **Unbounded procedural worlds.** The plan assumes an authored world with procedural fill. Here the terrain baseline is a function of (seed, tile) and the world has no edge. The document holds generator parameters and sparse authored overrides (the crash site is an authored layer over an infinite procedural base, a good test of the layer model); it never holds the world. Requirements: tiles are generated on demand as `derived` nodes with a per-tile budget; generation is deterministic regardless of visit order and thread count; persistent state distinguishes *never generated* from *generated, then unloaded*; tile indices are wide enough that no player reaches an edge (64-bit tile coordinates, no i32 anywhere in the addressing path).

2. **A moving baseline.** The deformation layer in [05 §5.13](05-simulation.md#513-deformable-surfaces-and-soft-bodies) decays toward an authored baseline. Here the baseline itself evolves: wind transports sand over hours of game time, dunes migrate, and sand deposits in the wind shadow of structures. This is a slow, coarse **geomorphology process on the heightfield**, run on the deterministic CPU grid through the timing wheel, with the fine GPU deformation map as the visual refinement. Requirements: the heightfield is mutable persistent per-tile state, not only derived data; a `SummarizeInterval` for tiles outside the active set (a dune must have moved when the player returns after a game-week, without simulating every tick in between); sand volume conservation as a test invariant and telemetry alert (the plan's conservation checks, [05 §5.7](05-simulation.md#57-world-events-and-consequences)); wind as a world field (direction, strength, gusts) that other systems read (particles, audio, critter behaviour).

3. **Granular relaxation (sand slides).** When a slope exceeds the material's angle of repose (about 34° for dry sand), grains flow downhill until it does not. This is a local cellular-automaton relaxation on the coarse grid, triggered by deformers (a footprint on a crest) and by wind deposition, and amplified visually with particles. Requirements: deformers are **signed** (they can add material and move displaced volume to the rim or downhill, not only write a minimum height); relaxation is bounded per event by radius and by a per-tick budget, with the remainder deferred; the fine GPU map re-syncs from the coarse grid when the coarse grid changes, so the coupling is bidirectional rather than stamp-only.

4. **Long view distances over low-frequency terrain.** Oases must be visible from far away and dunes must reach the horizon. Requirements: terrain LOD to tens of kilometres from the world-gen mips, with the horizon generated coarsely at the top quadtree levels without ever materializing fine tiles; a physically based sky and atmosphere; optionally heat shimmer. View distance is a tunable with a cost model, never a constant.

5. **Time of day as a first-class world input.** Sun and moon direction, sky radiance, and temperature derive from `GameTime` with a tunable day length. Gameplay reads temperature (thirst drains faster by day; night is cold). Rendering must stay stable under a continuously moving sun: temporal reuse in ReSTIR and the GI cache under slowly changing lighting is a known difficulty and becomes a benchmark scene.

6. **Observer-relative camera rigs.** First person, third person, and isometric are three rigs over one observer. The isometric rig is orthographic (`orthographic_reversed_z` exists in `core/math`) and needs LOD and culling by projected size rather than by eye distance. The renderer must not assume a perspective camera anywhere.

7. **Sparse wildlife in an infinite world.** Few critters near the player, unbounded world: population is per-tile statistical state at LOD3 (density, last hunted, recovery rate), materialized as agents only near observers. Critters burrow: emerging from and disappearing into sand are deformation events. Catching one is a simple interaction, not a combat system.

8. **Survival stats and a scored run.** Thirst and hunger are scheduler-driven state decaying toward death; food and water sources are document objects placed by the generator. A run is (seed, input log, schema version) and yields a deterministic score. The run database stores runs and a leaderboard is a projection over it. Because a run replays, a score can be verified from its input log; this is the first place the multiplayer-readiness rules earn their keep before any multiplayer exists.

9. **Persistence across an unbounded world.** Every tile the player changed keeps its state (footprints decay, sand piled against a ruin does not); untouched tiles cost nothing. Save = seed + event log + snapshots of modified tiles. Deformation history is compressed per tile as the plan already describes.

10. **Still water.** Oasis pools: reflection and refraction, a wet shoreline that reads the deformation map's material state, drinkable volume as a gameplay resource. No ocean and no flow simulation.

## 13.5 What it deliberately does not need

Narrative, dialogue, quests, NPC routines, factions, economy, a runtime language model, large-scale destruction (ruins are static except for sand burial; the wreck can exercise destruction on a small scale), multiplayer (a shared-seed leaderboard is the natural first multiplayer-adjacent feature), mobile.

## 13.6 Mapping to the roadmap

- **Phase 1**: heightfield terrain with clipmap LOD; sky model; a "dune field, 20 km" vista benchmark scene with an oasis at distance and a moving sun.
- **Phase 3**: seeded on-demand tile generation with a per-tile budget; `GameClock` day and night; observer set with the three camera rigs; per-tile persistent state; critter population at LOD tiers.
- **Phase 5**: the desert generator (dune-field synthesis from a wind field, ruin kits, oasis and scatter rules) as `derived` nodes with document parameters.
- **Phase 6**: deformable surfaces v1 grows signed deformers, granular relaxation, the wind-transport process on the timing wheel, and `SummarizeInterval` for dune migration. Added exit criterion: walk across a dune crest and it slumps; leave for a game-week and the dune has moved.
- **Phase 7**: this game is the recommended **first shippable**, built as the Phase 6 exit demo and released before the narrative game. It is cheaper than the 1–3 hour game, tests the systemic-world thesis, and produces the calibration data for the larger project.

## 13.7 Additional experiments

| ID | Question | Method | Decides |
|---|---|---|---|
| E13 | Cost of angle-of-repose relaxation on a 25 cm coarse grid for one 128 m tile on one core | Cellular-automaton prototype; cells per millisecond; worst case after a crest stamp | Coarse grid resolution; per-tick relaxation budget; whether relaxation must stay CPU-side to remain deterministic |
| E14 | Does an analytic dune-migration `SummarizeInterval` match the step-by-step process well enough that a returning player sees no discontinuity | Simulate N game-days both ways from the same seed; compare heightfields | Fast-forward model for slow terrain processes |
| E15 | ReSTIR and GI-cache stability under a continuously moving sun at the chosen day length | The dune vista scene; measure temporal error and flicker against the reference path tracer | Temporal reuse policy; day length bounds |

## 13.8 Open design questions for the owner

Day length; death rules (one run, permadeath); whether ruins have interiors (interior cells reached through portals) or are shells; input (keyboard and mouse, controller); whether the crash site is always tile (0, 0) with an authored layer and everything else procedural (recommended); whether a shared-seed leaderboard is in the first release.

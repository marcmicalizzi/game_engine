# 05 — Simulation Architecture

## 5.1 Principles

- The simulation is fixed-step, deterministic, seeded, and replayable. Rendering never affects it.
- Work is proportional to what can matter: every system declares the LOD tiers it runs at and its update policy per tier (per tick, periodic, or event-driven).
- Persistent state changes are events. Systems react to events rather than polling.
- The engine provides mechanisms (scheduler, LOD tiers, event bus, support-graph type, world predicates); the game provides policies (what an economy does).

## 5.2 Sim scheduler

- A system graph with declared read/write component sets gives automatic parallel scheduling within a tick and deterministic ordering for conflicting systems.
- Fixed step, 60 Hz default (a game may choose 30). Physics sub-steps if needed.
- Tick phases: input → events in → LOD assignment → systems (parallel) → physics → post-physics → events out → persistence flush.

## 5.3 Event scheduler (temporal LOD)

- A hierarchical timing wheel over game time with levels for ticks, seconds, minutes, hours, and days. O(1) insert and cancel; cancellation via generation counters on the timer handle.
- Two kinds of scheduled work: `Timer(entity, kind, at)` and `Periodic(system, interval, phase)`. Economy hourly, ecology daily, settlement weekly are periodics.
- **Fast-forward.** Advancing game time by Δ processes wheel events in order under a budget. If the count exceeds the budget, systems are asked to coarsen via `SummarizeInterval(from, to)` rather than execute every event. Every LOD2/LOD3 system must implement `SummarizeInterval`.

## 5.4 LOD tier assignment

- Tiers as in the brief (LOD0 immediate, LOD1 nearby, LOD2 loaded, LOD3 distant); the *number* is configurable per game.
- Assignment is a function over the **observer set**: the tier of an entity is the minimum over observers of f(distance, entity importance, observer weight). Importance covers quest relevance, named-character status, being in combat, being audible.
- Hysteresis bands and per-tick rate limits on promotions and demotions prevent thrash and spread materialization cost.
- Systems implement the materialization contract ([03-data-model §3.4](03-data-model.md#34-the-runtime-world)).

## 5.5 Reconciliation when a tile activates

1. Load the tile's projections from the persistent store.
2. Compute elapsed game time since the tile was last active.
3. Run `SummarizeInterval` for all LOD3 systems over the gap, deterministically from the tile's stored seed plus global events that intersect the tile.
4. Materialize entities at LOD2, then promote by observer distance.
5. Apply derived visual state (rubble cleared, fence erected) by selecting damage-state variants.

Cost is bounded by construction: summary functions are O(events in gap), and the gap's event count is bounded by the periodic systems' frequencies.

## 5.6 What NPC scale is realistic

Order-of-magnitude estimate, to be validated by experiment **E6**:

| Tier | Per-entity cost | Practical count |
|---|---|---|
| LOD0 | animation + physics + perception + detailed nav: 50–200 µs per tick | 50–200 |
| LOD1 | coarse nav + routine state machine: 5–20 µs per tick | 500–2,000 |
| LOD2 | scheduled transitions only: ~1 µs per event, a few events per hour | 10^4–10^5 |
| LOD3 | a record in the store plus occasional summary: memory-bound at ~200 B each | 10^5–10^6 |

At 10^6 LOD3 NPCs with about 5 events per game-day each, running at one game-minute per real second, the scheduler sees roughly 3.5k events per second, which is trivial. The real cost is in the aggregate systems that react (economy, factions); they must be written to run in time proportional to aggregates, not to NPC count.

## 5.7 World events and consequences

- Consumers subscribe by event type and tile. Handlers may emit new events with `cause` set and `depth + 1`.
- **Runaway prevention:** a hard cap on `depth` (drop and log beyond it); a per-tick event budget per system with deferral to the next tick; consequence-driven state variables that saturate and decay toward a baseline; no zero-delay self-loops (a system's reaction to its own event type is scheduled at least one tick later); conservation checks (population, currency) as test invariants and telemetry alerts.
- Telemetry: event rates per type, depth histograms, top causal chains. Exposed to agents for balancing.

## 5.8 Destruction

**Representation.** A destructible object is: mesh variants (damage states) + a **support graph** (nodes are structural elements, edges are load paths with capacities) + a pre-fractured chunk hierarchy for elements that fracture + gameplay dependencies (nav, quest predicates, contents).

**Support graph.** Built at content-build time from authored structural tags or generation heuristics (foundation → columns → beams → floors/walls → roof). At runtime, damage reduces capacity; a local graph query finds unsupported subgraphs; those become collapse events → chunk rigid bodies (Jolt) with lifetimes. Settled chunks freeze, become static variants, then persist as damage state, never as bodies.

**Fracture.** Pre-fracture (Voronoi or authored) at build time for everything in v1. Runtime fracture is a later experiment; pre-fracture covers most gameplay and keeps acceleration-structure costs predictable.

**Persistence.** Store `{damage_state, structural_state, seed, destroyed_at}`; debris never persists. Reactivation regenerates from seed plus elapsed time ([§5.5](#55-reconciliation-when-a-tile-activates)).

**Downstream effects (event consumers):** navigation (async Recast tile rebuild for affected tiles; region-graph edge updates for LOD2/3 pathing), AI (a `StructureChanged` event plus the nav update), quests (dependency index → policy), streaming (variant selection), saves (events).

## 5.9 Narrative dependencies and story protection

- The **dependency index** ([03-data-model §3.9](03-data-model.md#39-canon-and-narrative-representation)) maps world objects to the quests and scenes that depend on them.
- Each dependency carries a designer-chosen **policy**: `Protect` (the object is invulnerable or indestructible while the dependency is live), `Alternative` (allow the change, then activate an authored alternative route or emit a long-tail request), `Authoritative` (allow; the quest may become impossible; mark it failed with a reason the player can discover).
- Enforcement point: a single **world-mutation guard** that every irreversible mutation (destroy, kill, consume, remove) passes through. It consults the index and policy and blocks, allows-and-notifies, or allows. This is decoupled from destruction: NPC death and item loss pass through the same guard.
- A per-game default policy with per-dependency overrides.

## 5.10 Determinism and replay

- Fixed step; seeded RNG streams per system and per entity (hash of ID and stream name); no wall-clock reads in the sim; consistent floating-point flags across the sim's translation units (no contraction differences). Consider fixed-point for economy and ecology where cross-machine determinism matters more than speed.
- Input log + event log ⇒ replay. CI runs replays and asserts identical persistent-state hashes.
- Physics: Jolt is deterministic for the same binary, and offers cross-platform determinism as a build option at roughly 8% cost (with caveats around broadphase query order and callback order). Cross-platform determinism is not required for v1; keep the option open.

## 5.11 Integration notes

- **Physics**: Jolt. Bodies exist for LOD0/1 only. Character controllers via Jolt's. Debris via a dedicated body pool with hard caps.
- **Navigation**: Recast/Detour tiles aligned to the world grid; an async rebuild queue; a region-connectivity graph for LOD2/3 path estimation; off-mesh links for destruction-created passages.
- **Animation**: own system with SoA poses, blend trees and state machines as data, retargeting via a **standard skeleton** with per-character offsets (this is what makes generated characters tractable), IK for feet and hands, motion matching later.
- **Audio**: miniaudio for devices and mixing; Steam Audio for spatialization and occlusion, sharing the RT scene geometry.

## 5.12 Multiplayer readiness

Single-player ships first, but the engine carries more of the multiplayer burden than game code does, so the doors must stay open now. What an engine typically provides: transport (reliable and unreliable channels over UDP, encryption), replication (which components replicate, to whom, how often), ownership and authority, interest management (who needs to know about what), client-side prediction and reconciliation, RPC plumbing, lag-compensation hooks, snapshot and delta compression, session and lobby integration with platform services, and for lockstep designs, deterministic simulation with rollback. Game code decides *what* replicates and the rules; the engine provides the machinery.

Decisions already in this plan that keep multiplayer possible:

- The deterministic fixed-step sim with an input log ([§5.10](#510-determinism-and-replay)) supports lockstep and rollback models directly. Jolt's cross-platform determinism option covers mixed-OS lockstep.
- The headless `sim` mode is a dedicated server with no extra work.
- The observer set ([§5.4](#54-lod-tier-assignment)) is interest management under another name.
- Stable IDs and schema-generated components mean replication annotations (`replicated`, `owner_only`, `reliable`, `interpolated`) can be added to the IDL and code-generated later without touching game code.
- The world event log is a natural unit for synchronizing persistent world state in co-op.

Rules enforced from the start so nothing has to be unpicked later:

- No wall-clock reads in gameplay. No "the player" singleton: there is a set of players, possibly of size one. No gameplay logic in render code.
- All state mutations go through commands or events (already required by the persistence design), so they can be routed through an authority later.
- Physics and gameplay run on sim ticks, and every event and input carries its tick number.

Deferred: transport, replication code generation, prediction. Known hard combination: a persistent destructible world plus multiplayer (state volume, authority over destruction) should be scoped as its own project when the time comes.

## 5.13 Deformable surfaces and soft bodies

Destruction is on by default, and the same principle (the world reacts physically to what happens in it) extends to surfaces and to non-rigid objects.

**Destruction defaults.** Every destructible object's material category supplies default strength and fracture behavior, so destruction is on unless something opts out: a per-object `Indestructible` flag (also set by the story-protection guard in [§5.9](#59-narrative-dependencies-and-story-protection)), a per-material-category override, and a global switch for games that want none.

**Deformable surfaces: snow, sand, mud, ash, grass.** Implemented as a **deformation layer** over terrain, not as geometry edits. This is how Batman: Arkham Origins, Rise of the Tomb Raider, and Red Dead Redemption 2 handle snow and mud.

- Each active terrain tile has a *deformation map*: a height offset (how far the surface has been pushed down), optionally a direction field (grass bending), and a material state (loose vs. compacted, wet vs. dry). Resolution on the order of 2–8 cm per texel near observers and coarser far away; it is a virtual texture under the same residency manager as everything else.
- Deformers (feet, wheels, bodies, debris, explosions) are stamped into the map each tick by a GPU compute pass that projects the deformer's footprint (capsule, box, or decal) top-down and writes the minimum height with a soft profile. Displaced rims are raised slightly so volume looks conserved.
- Rendering: the terrain displacement reads the deformation map ([04 §4.3](04-renderer.md#43-geometry)) and reconstructs normals from it; snow gets subsurface-style shading, sand gets grain sparkle, grass reads the direction field.
- Gameplay reads come from a **coarse deterministic CPU grid** (25–50 cm cells) maintained by the sim from the same deformer events: foot IK depth, movement-speed penalty, footprint tracking for stealth, audio. The GPU map is a visual refinement of that grid, so replays never depend on GPU timing.
- Persistence: the map decays toward baseline at a material rate through the event scheduler (snowfall refills, wind smooths sand, mud dries into hardened tracks). The tile's persistent state carries a compressed low-resolution copy; unloaded tiles keep that or drop to a "trampled amount" statistic.
- The same machinery serves water-surface ripples (a wave-equation step on a similar map), wetness and blood as material-state-only layers, and dust and ash.

**Soft bodies.**

- Gameplay-affecting soft bodies (objects the player pushes and deforms, ropes, nets, tents, tarps, cloth barriers that block movement): **Jolt's soft-body simulation** (XPBD, position-based dynamics, collides with rigid bodies, CPU, deterministic alongside the rest of physics). LOD0 only, with a per-tick budget.
- Cosmetic soft bodies (character cloth, hair, flags, foliage sway, secondary motion): **GPU compute position-based dynamics** driven by render-side positions, one frame late, no gameplay effect, bounded per view, falling back to skinned animation at distance.
- Volumetric soft bodies (flesh, cushions, foam, fruit, jelly-like objects) are their own primitive with their own asset description, solver abstraction, and LOD policy: [§5.14](#514-deformable-volumes).
- Interaction with destruction: soft bodies attached to destructible structures detach on `StructureChanged` events; debris collides with soft bodies as ordinary rigid bodies.
- Particle fluids (SPH or position-based fluids) are noted as a later optional system sharing the GPU PBD framework; not planned for v1.

## 5.14 Deformable volumes

Volumetric deformation is a general engine primitive, not a cosmetic character effect. An asset describes deformable volumes and their physical structure; the engine computes deformation from animation, collision, contact, pressure, acceleration, and external forces. The consumers are soft-tissue secondary motion under locomotion and acceleration; skin, muscle, and fat pressed against objects; grabbing, pushing, impact, compression, and sustained contact; cushions, mattresses, rubber, foam, and fruit; flesh around harder skeletal structure; anatomically layered regions with cavities; deformation around wounds in games that model them; and creatures whose shape answers to forces rather than only to bones. ADR-0026 records the constraints below.

**Animation and deformation are separate systems.** Skeletal animation sets the intended pose and motion. The deformable solver adds secondary motion and contact response on top, under attachment and material constraints. The flow within a tick is one-way — animation writes the skeleton, the solver reads it as a kinematic constraint and writes deformation — and a deformable volume never moves a bone. Anything that should move a bone is animation, IK, or ragdoll, not deformation.

**Representation.** An asset declares one or more *deformable volumes*. A volume is a **cage** (a coarse simulation representation) plus materials, attachments, layers, and limits, all described in the asset ([07 §7.10](07-content-pipeline.md#710-deformable-volume-assets)). The high-resolution render mesh is never the physics representation: it is bound to the cage at build time and follows it ([04 §4.3](04-renderer.md#43-geometry)). Cage kinds are an open set behind one abstraction:

| Cage kind | Status | Use |
|---|---|---|
| Adaptive lattice from the mesh's signed distance field | v1 default, generated at content build with authored overrides | Characters, creatures, props, cushions, foam |
| Particle-and-shell cage with a pressure constraint | v1, Jolt's existing closed-volume soft body | Inflatables, fruit, simple organs |
| Tetrahedral cage | Later, same abstraction | Anatomically layered regions, cavities, FEM if it is ever justified |

The lattice is first because hand-tetrahedralizing a character does not scale, and the automatic tetrahedralizers that work are either AGPL (TetGen, excluded by [ADR-0014](../adr/0014-apache-2-license-and-dependency-policy.md)) or heavy, while a lattice falls out of the signed distance field the content build produces anyway. fTetWild (MPL-2.0) is the candidate if the tetrahedral kind is built.

**Solver.** XPBD first, behind an abstraction that is not XPBD-shaped.

- The CPU, gameplay-relevant path is **Jolt's soft bodies**: skinned soft-body constraints for attachment, pressure constraints for closed volumes, run in Jolt's deterministic mode inside the fixed-step tick beside the rigid bodies they collide with ([§5.11](#511-integration-notes)). Choosing Jolt's is choosing one already-deterministic contact and broadphase implementation instead of standing up a second one.
- The engine's own type is a `DeformableVolume`: cage topology, material parameters, attachments, constraint edits, and a per-tick solve against a contact set. Jolt is one backend. Replacing it — a GPU XPBD solver, or FEM for a region that earns it — replaces the backend and not the asset format, the binding data, the LOD policy, or the renderer path.
- Finite-element deformation stays out of v1 (ADR-0020) and is not precluded: it is a backend plus a cage kind, and both are already abstracted.

**Attachment and layers.** Every cage element declares its attachment: free, bound to a bone, or bound to a rigid body. Bones and bodies are kinematic to the solver, and "bound" is a stiff spring rather than a weld, so flesh lags a fast bone instead of tracking it exactly. Underlying stiffer structure is expressed as material rather than as separate geometry: a region of the cage carries its own stiffness, damping, density, and anisotropy, so bone-adjacent flesh is stiff and the surface layer is not without a second mesh. **Layers partition the volume** — every element belongs to exactly one layer — and a layer declares its boundary behavior with its neighbours (bonded, sliding, or separable above a force). A cavity is a material-free region whose boundary carries a pressure constraint. Per-volume limits (maximum strain, maximum element velocity) are stability constraints the solver clamps to, not suggestions.

**Contact.** v1 handles **soft against rigid** (the world, props, debris, a character's hand) and **soft against its own skeleton** (the volume's own bones and attached rigid parts, which is what keeps flesh from passing through the bone it hangs on during an extreme pose). Contact geometry is the cage, or an authored coarse contact surface when the cage is too coarse; the render mesh is never a contact source. Soft against another entity's soft volume is deferred by decision: both sides' constraints enter one solve and the pair count is quadratic in the interacting set, so it gets its own experiment and its own budget before it is built ([10 §10.6](10-roadmap-risks.md#106-decisions-deliberately-deferred)). A rigid hand pressing into a soft prop is in v1; two deformable hands pressing into each other are not.

**Damage as constraint edits.** Damage has no solver of its own. It is a set of edits to the cage's constraint set, applied at a tick boundary and recorded in the persistent event log like any other world change ([03 §3.5](03-data-model.md#35-persistent-world-state)): weaken or remove elements, change or break attachments, retag a region's material, or mark a surface region for the renderer. This composes with destruction ([§5.8](#58-destruction), ADR-0020) — a `StructureChanged` event that removes a support also frees the attachments that referenced it, and a detached soft body is the same cage with its attachments released. There is no injury model in v1; games that model wounds get these edits and author what they mean.

**Simulation LOD is mandatory.** Full volumetric simulation runs only where it is perceptually or mechanically relevant.

| Tier | Representation | Relative cost | When |
|---|---|---|---|
| T0 full cage | the authored cage, full element count and materials | 1 | the observer's own character and entities inside the interaction radius |
| T1 reduced cage | a coarser level of the same generated lattice, identical materials and attachments | ~1/4 | nearby, visible, not interacting |
| T2 authored secondary motion | jiggle bones or a small mass-spring chain on the skeleton, driven by the same accelerations | ~1/50 | far enough that only gross motion reads |
| T3 skinning only | none | 0 | crowd members, distant entities |
| T4 none | the bind-shape LOD | 0 | out of view, or sim LOD3 |

Tier assignment uses the observer function of [§5.4](#54-lod-tier-assignment) and is a property of the deformation system, not of the entity (ADR-0010). Two rules make it usable: hysteresis bands on every tier boundary with a per-tick cap on promotions, and, because the tiers do not produce the same shape, a **state blend of 4–8 ticks** on every transition, fading the outgoing tier's surface offsets into the incoming tier's rather than swapping them. Without the blend, characters pop at tier boundaries; that is the failure mode E24 measures.

**Determinism and the CPU/GPU split.** Cage state that gameplay reads is CPU state in the fixed-step sim, part of the sim hash, and replays exactly ([§5.10](#510-determinism-and-replay), [09 §9.6](09-testing-profiling.md#96-deformable-volume-experiments)). GPU deformation is derived output — the per-frame transfer to render vertices and the acceleration-structure work ([04 §4.3](04-renderer.md#43-geometry)) — and is never read back into gameplay, exactly as the deformation map of [§5.13](#513-deformable-surfaces-and-soft-bodies) is a visual refinement of a coarse CPU grid. Which volumes are gameplay-relevant is a per-asset property, not a global mode: a cushion that only has to look right is a GPU-only volume and never enters the sim hash; a creature whose body blocks a passage cannot be. High-volume or high-resolution deformation (crowds of cosmetic volumes, a dense cage on a hero asset) runs on the GPU compute path, bounded per view and one frame late, like cosmetic cloth.

**Budgets.** Provisional, to be replaced by E19–E25 measurements, and carried in the frame-budget files as their own tags so they are merge gates ([11 §11.1](11-performance-principles.md#111-culture-budgets-are-gates), ADR-0018).

- **CPU: 1.5 ms per 60 Hz tick** on the performance pool for all deformable volumes, shared with the soft bodies of [§5.13](#513-deformable-surfaces-and-soft-bodies) — about 9% of the tick. At an estimated 40–120 µs for a 300–800-element T0 cage at two sub-steps of eight iterations, that is 8–16 volumes at T0 plus 30–60 at T1. The budget is the gate; the counts follow from whatever the measurement turns out to be.
- **GPU: 0.5 ms per frame at 3840×2160, 0.8 ms at 11520×2160**, split between the cage-to-vertex transfer (small, and proportional to the LOD cut rather than the source mesh) and the per-frame cluster acceleration-structure work for deformed instances ([04 §4.4](04-renderer.md#44-ray-tracing)), which E2's numbers say dominates.
- **Memory**: about 32 B per cage element for simulation state, and 12 B per render vertex of binding data living inside the cluster pages ([07 §7.10](07-content-pipeline.md#710-deformable-volume-assets)), so binding costs the resident cut and not the asset.

Renderer path in [04 §4.3 and §4.5](04-renderer.md#43-geometry); asset, cage generation, and validation in [07 §7.10](07-content-pipeline.md#710-deformable-volume-assets); experiments and pass criteria in [09 §9.6](09-testing-profiling.md#96-deformable-volume-experiments); roadmap placement, risks, and E19–E25 in [10 §10.2, §10.4, §10.5](10-roadmap-risks.md#102-phases).

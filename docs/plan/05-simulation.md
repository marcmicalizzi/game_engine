# 05 — Simulation Architecture

## 5.1 Principles

- The simulation is fixed-step, deterministic, seeded, and replayable. Rendering never affects it.
- Work is proportional to what can matter: every system declares the LOD tiers it runs at and its update policy per tier (per tick, periodic, or event-driven).
- Persistent state changes are events. Systems react to events rather than polling.
- The engine provides mechanisms (scheduler, LOD tiers, event bus, support-graph type, world predicates); the game provides policies (what an economy does).
- **Every simulation capability of any value is roughed in** — a level of abstraction, an asset hook, an LOD policy, and a determinism stance — so that it can be added or deepened later without rescaffolding the systems around it. Roughing in is the interface and the data slot, not the implementation; a niche capability does not get to eat the schedule. The inventory is [§5.15](#515-capability-inventory), and the rule that makes the generality affordable is [11 §11.10](11-performance-principles.md#1110-absent-capabilities-are-free): a capability nobody uses costs nothing at run time.

## 5.2 Sim scheduler

- A system graph with declared read/write component sets gives automatic parallel scheduling within a tick and deterministic ordering for conflicting systems.
- Fixed step, 60 Hz default (a game may choose 30). Physics sub-steps if needed.
- Tick phases: input → events in → LOD assignment → systems (parallel) → physics → post-physics → events out → persistence flush.

**Status.** Built in `domain/sim` as `SimScheduler`, 2026-09-17: the eight phases in this order on a fixed step from `core/time`, systems registered with read/write component-set masks, and a wave schedule that is a pure function of the registration — conflicting systems keep declaration order, the rest run in parallel on `core/jobs`, and eight workers produce the same bytes as one; the static link-time registration table of [ADR-0027](../adr/0027-additive-capabilities.md) and the tier filtering of `SystemDesc::tiers` are not yet built. See [sim](../subsystems/sim.md).

## 5.3 Event scheduler (temporal LOD)

- A hierarchical timing wheel over game time with levels for ticks, seconds, minutes, hours, and days. O(1) insert and cancel; cancellation via generation counters on the timer handle.
- Two kinds of scheduled work: `Timer(entity, kind, at)` and `Periodic(system, interval, phase)`. Economy hourly, ecology daily, settlement weekly are periodics.
- **Fast-forward.** Advancing game time by Δ processes wheel events in order under a budget. If the count exceeds the budget, systems are asked to coarsen via `SummarizeInterval(from, to)` rather than execute every event. Every LOD2/LOD3 system must implement `SummarizeInterval`.

**Status.** Built in `domain/sim` as `TimingWheel`, 2026-09-17: five levels, O(1) insert (68 ns at 10^6 timers) and cancel (32 ns) with generation-checked handles, delivery ordered by (due time, insertion sequence), a far list for anything past the top level's ~2.8-year span, and the budgeted fast-forward with its summarizer table — the five conditions a system's `SummarizeInterval` has to meet are written down in [sim](../subsystems/sim.md), because "implement it" was not a contract.

## 5.4 LOD tier assignment

- Tiers as in the brief (LOD0 immediate, LOD1 nearby, LOD2 loaded, LOD3 distant); the *number* is configurable per game.
- Assignment is a function over the **observer set**: the tier of an entity is the minimum over observers of f(distance, entity importance, observer weight). Importance covers quest relevance, named-character status, being in combat, being audible.
- Hysteresis bands and per-tick rate limits on promotions and demotions prevent thrash and spread materialization cost.
- Systems implement the materialization contract ([03-data-model §3.4](03-data-model.md#34-the-runtime-world)).

**Status.** Built in `domain/sim` as `TierAssignment` and the scheduler's hooks table, 2026-09-17: up to eight configurable tiers, the minimum over observers of distance over (weight × importance), hysteresis bands, per-tick promotion and demotion limits that take the nearest first, changes emitted in entity index order, and `materialize`/`promote`/`demote`/`dematerialize` driven from those changes; 10^5 entities cost 2.3 ms with no job system and 0.61 ms on eight workers, which makes this a budget row rather than a rounding error, as [ADR-0028](../adr/0028-ecs-and-persistent-store.md) warned.

## 5.5 Reconciliation when a tile activates

1. Load the tile's projections from the persistent store.
2. Compute elapsed game time since the tile was last active.
3. Run `SummarizeInterval` for all LOD3 systems over the gap, deterministically from the tile's stored seed plus global events that intersect the tile.
4. Materialize entities at LOD2, then promote by observer distance.
5. Apply derived visual state (rubble cleared, fence erected) by selecting damage-state variants.

Cost is bounded by construction: summary functions are O(events in gap), and the gap's event count is bounded by the periodic systems' frequencies.

**Status.** Built in `domain/sim` as `SimScheduler::reconcile_tile`, 2026-09-17: steps 1–4 over a `TileStore` of function pointers (so the module does not depend on the optional `foundation/store`) and the materialization hooks, with summarizers run in registration order from the tile's stored seed, and step 4's promotion by observer distance sharing the tier code of [§5.4](#54-lod-tier-assignment); step 5, the derived visual state, belongs to the renderer and is not done here, and nothing is written back yet.

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

**Status (navigation).** `domain/nav` with Recast/Detour 1.6.0, 2026-09-17: tiles built on the
world grid from triangle soup, added, replaced and removed at run time; string-pulled paths,
nearest-point and walkability queries; off-mesh links added and removed at run time, each
re-baking one tile and no voxelization; an asynchronous rebuild queue on the job system's
efficiency pool that coalesces a doubly-requested tile, orders by observer distance with a
starvation guard, and applies results in tile order on the caller's thread; the
region-connectivity graph for LOD2/LOD3 estimates, built from the same tile bytes as the detailed
mesh and measured at 1.00–1.09× its path length; and a DetourCrowd wrap sized by a cap. Tile
rebuild latency is measured — [E11](10-roadmap-risks.md#105-experiments-to-run-before-committing)'s
first data point — and it is a range rather than a number: 7.2 / 32.3 / 133.4 ms for 32, 64 and
128 m tiles at 0.25 m cells, and 2.0 / 8.4 / 42.5 ms for the same tiles at half the resolution
with monotone regions and no detail mesh. Temporary obstacles (Detour's tile cache), automatic
jump-link generation, and formation movement are pending. See [nav](../subsystems/nav.md).

**Status (physics).** `domain/physics` skeleton with Jolt 5.6, 2026-09-17: rigid bodies, shapes (box, sphere, capsule, convex hull, triangle mesh, heightfield, compound), ray and shape casts, drained contact events, the debris pool with its hard cap, and XPBD soft bodies behind the engine's own types, running Jolt's jobs on `core/jobs` with cross-platform determinism on (measured at 1.6–3.6%, not the estimated 8%); character controllers, constraints, and rollback state are pending. See [physics](../subsystems/physics.md).

**Status (animation, 2026-09-17).** The **data half** is built as `domain/anim` ([anim](../subsystems/anim.md)), and the path from a glTF file to deformed vertices on the GPU is closed end to end: `assets::load_gltf` reads skins, per-vertex influences, and the three glTF sampler modes (keeping a skinned primitive in **bind space**, since glTF places it through its joints); `geometry` carries an optional 8-byte `SkinBinding` per vertex through welding, the LOD DAG, the merges, the streaming pages, and the container; `anim` owns the SoA `Pose`, the `Clip` and its sampler, `blend`/`blend_additive`, `local_to_model`, and 3×4 skinning matrices; and `deform.slang` gains a skinning mode that fills the deformed-vertex pool of [04 §4.3](04-renderer.md#43-geometry), checked against `anim::skin_positions` on the GPU to 1e-5.

What that settles: poses are SoA as this section says, and the reason is per-channel blending rather than taste; a skinning matrix is 3×4, because the fourth row is always `(0, 0, 0, 1)` and the array is uploaded per instance per frame; and **skinning cannot crack**, because a skinned position is a function of the rest position and the binding, both of which every copy of a surface point shares byte for byte — which is the constraint [E25](../experiments/e25-deformed-clusters.md) found the hard way with a normal-direction deformer.

The **standard skeleton is a first cut**: a 23-role humanoid core, name-based `map_joints` over the spellings real rigs use, and a bind-difference retarget with per-joint offsets and a hip-height translation scale. What it does not do yet is written down beside the code (`standard_skeleton.h`) and on the subsystem page: no IK and therefore no foot locking, no limb-length compensation, no twist distribution, no joint limits, no authored mapping file, no fingers or face. **Blend trees and state machines as data**, the playback state a clip needs, and the tick that drives them are the `systems/animation` capability, which does not exist yet; nothing outside a test animates a skinned instance in `engine-view` today.

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

Volumetric deformation is a general engine primitive, not a cosmetic character effect. An asset describes deformable volumes and their physical structure; the engine computes deformation from animation, collision, contact, pressure, acceleration, and external forces. The consumers are soft-tissue secondary motion under locomotion and acceleration; skin, muscle, and fat pressed against objects; grabbing, pushing, impact, compression, and sustained contact; cushions, mattresses, rubber, foam, and fruit; flesh around harder skeletal structure; anatomically layered regions with cavities; deformation around wounds in games that model them; creatures whose shape answers to forces rather than only to bones; and cavities that a hand or an object is pushed into, held in, and withdrawn from ([13 §13.7](13-reference-consumer-games.md#137-reference-interactions)). ADR-0026 records the constraints below.

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

**Attachment and layers.** Every cage element declares its attachment: free, bound to a bone, bound to a rigid body, or bound to a static collider in the scene the volume is placed in, which is how a volume set into world geometry — a cavity in a wall — is anchored. Bones and bodies are kinematic to the solver, and "bound" is a stiff spring rather than a weld, so flesh lags a fast bone instead of tracking it exactly. Underlying stiffer structure is expressed as material rather than as separate geometry: a region of the cage carries its own stiffness, damping, density, and anisotropy, so bone-adjacent flesh is stiff and the surface layer is not without a second mesh. **Layers partition the volume** — every element belongs to exactly one layer — and a layer declares its boundary behavior with its neighbours (bonded, sliding, or separable above a force). A cavity is a material-free region whose boundary carries a pressure constraint. Per-volume limits (maximum strain, maximum element velocity) are stability constraints the solver clamps to, not suggestions.

**Contact.** v1 handles **soft against rigid** (the world, props, debris, a character's hand) and **soft against its own skeleton** (the volume's own bones and attached rigid parts, which is what keeps flesh from passing through the bone it hangs on during an extreme pose). Contact geometry is the cage, or an authored coarse contact surface when the cage is too coarse; the render mesh is never a contact source. Soft against another entity's soft volume is deferred by decision: both sides' constraints enter one solve and the pair count is quadratic in the interacting set, so it gets its own experiment and its own budget before it is built ([10 §10.6](10-roadmap-risks.md#106-decisions-deliberately-deferred)). A rigid hand pressing into a soft prop is in v1; two deformable hands pressing into each other are not.

**Adhesion is a contact model kind, not a parameter tweak.** Contact has two kinds. `frictional` is ordinary contact and is the default. `adhesive` lets cage nodes that have been in contact for a dwell time form **temporary attachment constraints to the collider**: an attachment holds up to a per-material stress (stick), breaks above it (slip), and a broken attachment's force decays over a release time instead of vanishing in one tick. A surface pulling away from a hand therefore stretches after it, lets go progressively, and recoils, which is what "clings on withdrawal" is and what separates tissue from rubber in the same solver. The per-material parameters are tack, break stress, dwell, and release time ([07 §7.10](07-content-pipeline.md#710-deformable-volume-assets)). Attachments are cage state: they enter the sim hash for a `gameplay_relevant` volume and they form and break deterministically at tick boundaries, like every other constraint edit. Zero tack is the default and creates no attachment constraints, no attachment pool, and no per-contact dwell tracking, so adhesion costs nothing on an asset that does not ask for it ([11 §11.10](11-performance-principles.md#1110-absent-capabilities-are-free)).

**Damage as constraint edits.** Damage has no solver of its own. It is a set of edits to the cage's constraint set, applied at a tick boundary and recorded in the persistent event log like any other world change ([03 §3.5](03-data-model.md#35-persistent-world-state)): weaken or remove elements, change or break attachments, retag a region's material, or mark a surface region for the renderer. This composes with destruction ([§5.8](#58-destruction), ADR-0020) — a `StructureChanged` event that removes a support also frees the attachments that referenced it, and a detached soft body is the same cage with its attachments released. There is no injury model in v1; games that model wounds get these edits and author what they mean.

**Folds appear at material resolution, not cage resolution.** The per-frame transfer that writes deformed vertices ([04 §4.3](04-renderer.md#43-geometry)) may also write each vertex's local cage strain — a scalar (compression along the largest principal direction) or a small symmetric tensor — into the deformed-vertex pool, and the material resolve may read it and blend authored wrinkle and fold normal and displacement maps by compression. A cage coarse enough to be affordable then still produces a surface that creases where it is squeezed: the cage decides where the volume goes, the material decides what the skin does about it, and a fold no longer needs a cell to sit in. This is opt-in on both sides — the asset declares a strain output, the material declares a strain input, and validation checks the pair ([07 §7.10](07-content-pipeline.md#710-deformable-volume-assets)) — so a volume that declares neither has no strain channel in the pool, no extra bytes per vertex, and no extra work in the transfer pass. Whether it is needed at hero distance, or whether cage resolution alone reads correctly there, is what E26 decides ([09 §9.6](09-testing-profiling.md#96-deformable-volume-experiments)).

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
- **Memory**: about 32 B per cage element for simulation state, and 12 B per render vertex of binding data living inside the cluster pages ([07 §7.10](07-content-pipeline.md#710-deformable-volume-assets)), so binding costs the resident cut and not the asset. A volume that asks for strain output adds 4 B per deformed vertex for the scalar form or 12 B for the packed tensor, in the per-frame pool and only for the cut.

Renderer path in [04 §4.3 and §4.5](04-renderer.md#43-geometry); asset, cage generation, and validation in [07 §7.10](07-content-pipeline.md#710-deformable-volume-assets); experiments and pass criteria in [09 §9.6](09-testing-profiling.md#96-deformable-volume-experiments); roadmap placement, risks, and E19–E26 in [10 §10.2, §10.4, §10.5](10-roadmap-risks.md#102-phases); the interaction the primitive is judged against in [13 §13.7](13-reference-consumer-games.md#137-reference-interactions).

**Status note, 2026-09-17: E19 measured the T0 cage, and the CPU budget above is out by more than an order of magnitude.** ([results](../experiments/e19-lattice-cage.md); the owner decides what moves.) A lattice cage around a rigid two-bone core, pressed to 30% of its depth and released, costs **0.225 µs per element per iteration per sub-step** on an i9-10980XE, within ±5% across element counts of 343 to 729, iteration counts of 4 to 16, and one or two sub-steps. The 512-element T0 cage this section estimates at **40–120 µs at two sub-steps of eight iterations** measures **1,801 µs** — 1.2× the entire 1.5 ms deformable budget, for one volume.

Three things follow, and they are about the shape of the tier table rather than its numbers:

- **Cage size is a parallelism decision, not only a fidelity one.** The backend splits a cage's constraint solve by vertex batch — 256 vertices to a batch — so a 512-element cage is a two-wide solve however many workers exist, and the workers that find no group to claim spin rather than idle: one cage is **1.3× to 1.8× slower on eight workers than on one**. Across cages it scales properly (8 cages of 512 are 6.45× faster on eight workers than on one, and cost less than a single cage does on the same pool, 145 µs each). **The achievable T0 population therefore depends on the deformation system stepping its volumes together in one world**, and "8–16 volumes at T0" is reachable only in that arrangement: eight 512-element cages at eight iterations and one sub-step measure 1.16 ms, inside the budget; at the two sub-steps E19 recommends for stability, about 2.3 ms, outside it.
- **Sub-steps are the stability knob and iterations are not.** At 512 elements one sub-step took a permanent set at 8 and at 16 iterations while two sub-steps recovered at every iteration count; raising iterations at 729 elements turned recoveries into failures. The mechanism is cell inversion, not under-convergence — a tetrahedron that goes inside out has a volume constraint pushing it further out, and more iterations push harder.
- **`limits.max_strain` and `limits.max_velocity` are described above and in [07 §7.10](07-content-pipeline.md#710-deformable-volume-assets) as limits "the solver clamps to, not suggestions", and nothing clamps them yet.** Measured held edge stretch under the press is 1.76–2.41 against an authored limit of 1.5, with a transient peak of 2.81 and one configuration diverging outright. The clamp is a per-element clamp in the position pass rather than a solver change, and it is the next thing the primitive needs.

One result belongs to the asset side rather than the solver: **XPBD compliance is not scale-free, and a volume constraint's compliance is not an edge constraint's.** Giving both the same number leaves volume preservation switched off — the cage takes a permanent set at two thirds of its volume — because the constraint gradient is a length for an edge and an area for a tetrahedron, four orders of magnitude apart at a 6 cm cell. Whatever turns `material.volume_preservation` into a compliance has to know the cell size. [E22](10-roadmap-risks.md#105-experiments-to-run-before-committing) also has early data from the same fixture: under 1× and 5× its own mass for 20 s the cage shows no creep, no plane penetration, 0.03 mm/s residual surface velocity, and volume within 1.6%.

## 5.15 Capability inventory

One row per simulation capability the engine roughs in ([§5.1](#51-principles)). What matters is the four middle columns — the level of abstraction and the asset hook, the LOD policy, the determinism stance, and the mechanism that makes the capability free when nothing uses it — because those are what is expensive to retrofit. The implementation is not, which is why most rows are honest about having none.

- **planned** — a hook is named and nothing else exists. Most rows.
- **roughed in** — abstraction, asset hook, LOD policy, and determinism stance are written down in this plan and a phase owns them.
- **spiked** — an experiment has returned numbers.
- **v1** — built, measured, and carried as a budget row.

Nothing is `v1`; Phase 1 is the renderer core. A `planned` row is a hook, not a commitment to build the thing: deliberate refusals live in [10 §10.6](10-roadmap-risks.md#106-decisions-deliberately-deferred). A hook marked **(proposed)** appears nowhere else in this plan yet and is this section's suggestion for where the capability attaches.

| Capability | Representative use cases | Abstraction and asset hook | LOD policy | Determinism stance | Zero-cost mechanism | Status | Exp. |
|---|---|---|---|---|---|---|---|
| Rigid bodies | Props, debris, chunks, thrown and pushed objects, doors | Jolt bodies built from the asset's collision shape; the material category supplies density, friction, restitution ([§5.11](#511-integration-notes)) | Bodies exist at LOD0/1 only; beyond, a transform and a record | In the sim hash; Jolt's deterministic mode, cross-platform mode optional | No dynamic bodies in a tile means no broadphase and no physics phase for it | roughed in | — |
| Character controllers | Player and NPC locomotion, slopes, steps, crouch, swim | Jolt's character controller behind a `CharacterBody`; the capsule comes from the character standard ([07 §7.7](07-content-pipeline.md#77-ml-asset-generation-novelty-content)) | LOD0/1 simulate; LOD2 moves along nav links with no controller | In the hash; input is tick-stamped and replays | No controllers means no post-physics phase | roughed in | — |
| Ragdolls | Death, falls, impacts, blending out of animation | A Jolt constraint set generated from the standard skeleton: **a ragdoll profile per body archetype, retargeted like the cage (proposed)** | LOD0 only, budgeted with debris; at distance a death animation | In the hash while active, then settles to a pose and is dropped | Profile is asset data; no instances means no constraint group | planned | — |
| Vehicles | Carts, boats, lifts, Island City traffic; elevators as the kinematic case | **Jolt's vehicle constraint behind a `Vehicle` component with a `vehicle` property block: wheels, suspension, drive (proposed)** | LOD0/1 simulate; LOD2 follows a spline on the timing wheel | In the hash | No vehicle instances means no per-tick vehicle step | planned | — |
| Cloth | Clothing, flags, tarps, banners, cloth barriers | Jolt XPBD for gameplay cloth, GPU PBD for cosmetic ([§5.13](#513-deformable-surfaces-and-soft-bodies)); the asset hook is a cloth region with pins reusing [07 §7.10](07-content-pipeline.md#710-deformable-volume-assets)'s `attachment.target` | Cosmetic cloth bounded per view, skinning at distance; gameplay cloth LOD0 | Gameplay cloth in the hash; GPU cloth derived and never read back | No cloth regions means no GPU PBD dispatch and no pool | roughed in | — |
| Ropes and cables | Winches, tethers, rope bridges, hanging signs, power lines | [§5.13](#513-deformable-surfaces-and-soft-bodies)'s gameplay soft bodies, plus **a two-ended `anchor` attachment kind shared with [§5.14](#514-deformable-volumes)'s attachment table (proposed)** | LOD0 simulated; a catenary fit beyond | Hashed when gameplay-relevant | None present means no constraint group | planned | — |
| Soft bodies ([§5.13](#513-deformable-surfaces-and-soft-bodies)) | Pushable objects, nets, tents, tarps | Jolt XPBD soft bodies under a per-tick budget; cosmetic ones on GPU PBD | LOD0 only, budgeted; shares the deformable budget of [§5.14](#514-deformable-volumes) | CPU path in the hash; GPU path derived | No soft bodies means no solver group and no draw on the budget | roughed in | — |
| Deformable volumes and soft tissue ([§5.14](#514-deformable-volumes)) | Flesh, cushions, foam, fruit, layered anatomy, cavities, wounds | `DeformableVolume` with a cage generated from the mesh's SDF at content build; property block and binding in [07 §7.10](07-content-pipeline.md#710-deformable-volume-assets), renderer path in [04 §4.3](04-renderer.md#43-geometry) (ADR-0026) | Five mandatory tiers with hysteresis and a 4–8-tick state blend | `gameplay_relevant` cages in the fixed-step tick and the sim hash; GPU transfer derived | An asset with no property block has no cage, no binding bytes in its pages, and no transfer dispatch | roughed in; transfer path spiked in Phase 1 | E19–E26 |
| Destruction and fracture | Collapsing structures, breakable props, debris (ADR-0020) | Damage-state variants, a support graph, and a pre-fractured chunk hierarchy, all `derived` at content build; per-material strength ([§5.8](#58-destruction)) | Collapse simulated at LOD0/1; far damage is a variant swap plus persisted state | Damage and collapse are events in the log and the hash; debris is capped and never persisted | `Indestructible` or the global switch means no support graph and no chunks are built or resident | roughed in | E11 |
| Terrain deformation | Footprints, tracks, trampled snow, sand piled against a wall | Per-tile deformation map as a virtual texture, stamped by a GPU pass, mirrored by a coarse CPU grid; deformers are events ([§5.13](#513-deformable-surfaces-and-soft-bodies)) | Map resolution by observer distance; unloaded tiles keep a compressed copy or a statistic | The coarse CPU grid is hashed; the GPU map is a visual refinement and is never read back | A tile with no deformer events allocates no map page and runs no stamp pass | roughed in | E13, E14 |
| Granular materials and sand flow | Dune slip faces, avalanches, spilled grain, gravel | Angle-of-repose relaxation on the same coarse grid, with signed deformers that move displaced volume ([13 §13.4](13-reference-consumer-games.md#134-requirements-the-consumers-add-or-sharpen)) | Bounded per event by radius and per tick by budget; distant transport is `SummarizeInterval` | CPU grid in the hash; volume conservation is a test invariant and a telemetry alert | Relaxation runs only over cells a deformer flagged unstable, so a still world iterates an empty list | planned | E13 |
| Fluids and water surfaces | Oasis pools, ocean, ripples, puddles, wetness | A wave-equation step on a deformation-map-like layer, ocean as a procedural surface, wetness as material state ([§5.13](#513-deformable-surfaces-and-soft-bodies)); particle fluids later on the GPU PBD framework | Simulated near the observer, analytic spectrum far | Surfaces are derived; gameplay reads water depth from the tile's coarse grid | No water in a tile means no water pass in the render graph | planned | — |
| Buoyancy | Boats, floating debris, swimming, a body dropped in a pool | **A `fluid volume` region (analytic plane, box, or the tile's water layer) plus a submerged-volume query against the body's hull or cage (proposed)** | LOD0/1 bodies only; beyond, "afloat" is a state | In the hash | No fluid volumes means no query and no per-body force | planned | — |
| Wind and aerodynamics | Dune migration, foliage sway, cloth, dust, gliding | Wind is a world field on the tile grid that systems read ([13 §13.4](13-reference-consumer-games.md#134-requirements-the-consumers-add-or-sharpen)); the aerodynamic half is **a drag coefficient and reference area on the material category, read by bodies and cage elements (proposed)** | One coarse field everywhere; drag applies at LOD0/1 | The field is a deterministic function of game time and seed; drag is hashed | A game that never reads the field never evaluates it; drag is off unless a category sets it | planned (field roughed in) | E14 |
| Fire, heat and burning | Campfires, burning structures, desert heat exposure, cooking | **A thermal block on the material category (ignition temperature, burn rate, heat capacity) plus heat as a world field; burning is damage — constraint edits and damage-state variants — with no solver of its own (proposed)** | Burning objects tick at LOD0/1; elsewhere a burn timer on the timing wheel with `SummarizeInterval` | Thermal state and ignition events are CPU state and hashed; flame rendering is derived | Nothing ignited means no thermal field allocated and no tick | planned | — |
| Weather and climate | Wind shifts, sandstorms, rain, snowfall, temperature | **The world-field set (wind, temperature, precipitation, humidity) on the tile grid, advanced by the timing wheel (proposed as a set; wind and wetness exist singly)** | One coarse field for the world; per-tile detail only where observed | A function of seed and game time plus logged events; hashed | A game with constant conditions evaluates a constant | planned | E14 |
| Day, night and time | Lighting, routines, temperature, nocturnal creatures | `GameTime` and the timing wheel drive sun, moon, sky radiance, and temperature; day length is a tunable ([02 §2.4](02-architecture.md#24-frame-and-time-model), [13 §13.4](13-reference-consumer-games.md#134-requirements-the-consumers-add-or-sharpen)) | Continuous for observers; periodics for everything else | Integer game time; fully deterministic | A game with a fixed sun schedules no periodic | roughed in | E15 |
| Procedural animation and IK | Foot placement in deformed sand, hand placement on a grabbed object, look-at, aim | The animation system of [§5.11](#511-integration-notes): standard skeleton, blend graphs as data, IK for feet and hands. Deformation reads the pose and never writes a bone ([§5.14](#514-deformable-volumes)) | Full IK at LOD0, foot IK at LOD1, none beyond | Poses are sim state where gameplay reads them (foot depth, grab point); otherwise per-frame | A rig with no IK chains runs no solver | planned | E20 |
| Crowds and flocking | Island City pedestrians, creature groups, birds | The tier model of [§5.4–5.6](#54-lod-tier-assignment) for the population; **local avoidance is Detour's crowd manager behind the nav wrapper (proposed)**; flocking is a game policy over the same steering output | LOD0 avoidance, LOD1 coarse steering, LOD2 scheduled transitions, LOD3 records | Hashed at LOD0/1, summarized beyond | No agents means no crowd step | planned | E6 |
| Navigation and pathfinding | Everything that moves on foot; destruction-created routes | Recast/Detour tiles aligned to the world grid, async rebuild queue, region-connectivity graph, off-mesh links ([§5.11](#511-integration-notes), [§5.8](#58-destruction)) | Detailed paths at LOD0/1, region-graph estimates beyond | Hashed; rebuilds are ordered by tile, never by completion time | Navmesh tiles are built on demand, so a game with no navigating agents builds none | roughed in | E11 |
| Hearing and acoustic propagation | Stealth, creatures reacting to the player, gunfire, footsteps in sand | "Being audible" is already an importance term in [§5.4](#54-lod-tier-assignment); **a `Sound` event carrying position, loudness, and a propagation budget, consumed by perception (proposed)**. Steam Audio's occlusion shares the RT scene and stays the audio-side answer | Gameplay propagation is a bounded flood over the region graph; spatialization is per-frame and derived | The event and its resolved loudness are hashed; DSP is not | No sound events means no propagation work; the bus is event-driven already | planned | — |
| Smell and scent propagation | Creature tracking, the player's trail, blood | **A decaying scalar field on the same coarse per-tile grid that carries deformation, written by emitters as events and advanced by the timing wheel (proposed)** | The field exists only in tiles with both an emitter and a consumer; beyond that, "the creature found it" is a scheduled transition | Coarse grid with integer decay; hashed | No emitters means no field page is allocated | planned | — |
| Electricity, power and mechanisms | Lights, elevators, doors, pumps, city utilities, puzzle machinery | **The support-graph type of [§5.1](#51-principles) and [§5.8](#58-destruction) generalized to a network graph — nodes, directed edges with capacity, sources and sinks — reused for power, water, and mechanical linkage; a break is the same local query destruction already runs (proposed)** | Solved on change, not per tick; LOD2/3 tiles keep the last solution | An integer flow solve over a graph; hashed | No network in a tile means nothing to solve | planned | — |
| Structural load | Buildings standing and falling; Island City's occupied tower ([13 §13.2](13-reference-consumer-games.md#132-consumer-b--island-city)) | The support graph built at content build from structural tags or heuristics; damage reduces capacity and unsupported subgraphs become collapse events ([§5.8](#58-destruction)) | A local graph query on damage; no per-tick work at any tier | Damage and collapse are events in the log and the hash | An object with no structural tags has no graph | roughed in | — |
| Vegetation growth and ecology | Oasis foliage, forest change, regrowth after fire, animal populations | Daily and weekly periodics with `SummarizeInterval` ([§5.3](#53-event-scheduler-temporal-lod)), scatter rules as the materialization ([07 §7.6](07-content-pipeline.md#76-procedural-generation-volume-content)), per-tile ecology state in the persistent store | LOD2/3 aggregate; instances materialize with the tile | Seeded per tile and summarized over gaps; hashed as persistent state | A game with no ecology periodic schedules nothing | planned | — |
| Persistence and aging of the world | Snow refilling, dunes migrating, ruins burying, rubble cleared, lives continuing | Event-sourced per-tile state plus reconciliation on activation ([§5.5](#55-reconciliation-when-a-tile-activates), [03 §3.5](03-data-model.md#35-persistent-world-state)); every LOD2/3 system implements `SummarizeInterval` | The mechanism is the LOD policy | Summaries are deterministic from the tile seed plus intersecting global events, and replay | A tile never visited is a record, not a simulation | roughed in | E14 |

The list is long on purpose. Ambition is the premise of the engine, and the capabilities above are the ones that let a small team get physical, systemic interaction out of rules rather than out of per-case authoring — which is the only way a small team gets it at all. The list is affordable because roughing in is cheap and because [11 §11.10](11-performance-principles.md#1110-absent-capabilities-are-free) holds: a row nobody uses must be invisible in the profile, or the inventory is a tax on every game built with the engine.

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
- Volumetric soft bodies (jelly-like objects) via Jolt's soft-body volumes. Finite-element deformation is out of scope.
- Interaction with destruction: soft bodies attached to destructible structures detach on `StructureChanged` events; debris collides with soft bodies as ordinary rigid bodies.
- Particle fluids (SPH or position-based fluids) are noted as a later optional system sharing the GPU PBD framework; not planned for v1.

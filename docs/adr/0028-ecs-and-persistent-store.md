# ADR-0028: flecs is the runtime entity store, exposed directly behind five engine-owned seams; SQLite is the persistent store, tuned by measurement

- **Status:** Accepted
- **Date:** 2026-09-18
- **Plan references:** docs/plan/03-data-model.md §3.4 and §3.5, docs/plan/05-simulation.md §5.2, §5.4, §5.5 and §5.6, docs/plan/08-toolchain.md §8.6, docs/plan/10-roadmap-risks.md §10.5 (E6), docs/experiments/e6-ecs-store.md. Builds on ADR-0003 (event-sourced persistent state in SQLite), ADR-0012 (adopted libraries; "flecs pending E6"), ADR-0010 (deterministic sim and the LOD contract), ADR-0027 (additive capabilities). Supersedes nothing.

## Context

[03 §3.4](../plan/03-data-model.md#34-the-runtime-world) recommends evaluating flecs before writing an archetype ECS, for reasons specific to this project — first-class relationships that the persistent world needs anyway, a query language that maps onto `query_world`, reflection, an explorer — and names one concern: general-purpose iteration is not the fastest possible hot loop. [ADR-0012](0012-adopted-libraries.md) adopted SQLite outright but left flecs "pending E6". [03 §3.5](../plan/03-data-model.md#35-persistent-world-state) chose SQLite for the event log and projections but fixed none of the settings that decide whether it is fast enough, and left the table shapes open.

Experiment **E6** ([results](../experiments/e6-ecs-store.md)) built both capabilities and measured them: 10^5 entities with relationships, a hierarchy and four systems a tick, and 10^6 LOD3 projection records with the reads and writes the simulation will actually issue. It answered the ECS question, answered the store's settings with numbers rather than defaults, and raised a question about the *scheduler* that is deliberately left open below.

## Decision

1. **flecs 4.1.x is the runtime entity store**, in `domain/ecs`, an optional capability under `ENGINE_WITH_ECS` ([ADR-0027](0027-additive-capabilities.md)).

2. **flecs is exposed directly. The engine does not wrap it — and the engine owns five seams.** `ecs::SimWorld::world()` hands out the `flecs::world`, and system bodies, queries, and relationships are flecs'. A wrapper would have to re-expose relationships, wildcards, change detection, staging, the query DSL, and reflection to be useful, it would cost indirection on every query forever, and it would not deliver the portability it appears to buy, because swapping an archetype ECS is a rewrite of every system's data access whatever sits in between.

   But *unwrapped* must not mean *flecs owns the engine's guarantees*. The five seams below are the ones that do, and they are enumerated in "Seams" so the list is closed rather than a matter of taste. The insurance against a bad library choice is then two things, not one: the architectural hedge [03 §3.4](../plan/03-data-model.md#34-the-runtime-world) already names — hot systems own their own data; the ECS holds identity, relationships, and gameplay components — and the fact that the type system, the scheduling contract, identity, persistence and the module graph are not flecs' to begin with.

3. **What `domain/ecs` adds beyond the seams**: the tick phases of [05 §5.2](../plan/05-simulation.md#52-sim-scheduler) as flecs phase entities in order, chained after flecs' last built-in phase so anything left in a built-in one runs before the engine's tick rather than inside it; a fixed step from `core/time` with `SimTick` and `GameTime` published as singletons (core/time's own types, not new ones); an adapter that runs flecs' workers on `core/jobs`' performance pool through `ecs_set_task_threads` and the `ecs_os_api_t` task hooks; and flecs' log routed into `core/log`. A capability names a phase with `.kind(sim.phases()[TickPhase::Lod])`, which is ADR-0027's registration point for a ticking system.

4. **SQLite is the persistent store**, in `foundation/store`, an optional capability under `ENGINE_WITH_STORE`, with these settings, each chosen by a measurement in E6 and not by taste:

   | Setting | Value | What it bought |
   |---|---|---|
   | Journal | WAL, `synchronous=NORMAL` | 2× a rollback journal at `FULL` on the same 1,000-row transaction, and readers that do not block behind the writer |
   | Page size | 8 KiB | keeps a 256-byte blob inline in a `WITHOUT ROWID` row, which spills past about 1/20 of a page |
   | Page cache | sized to the indices, not left at the 2 MiB default | 4.1× on a million-row bulk load |
   | Transactions | one per tick, never one per row | 16× between one row a transaction and a thousand |
   | `SQLITE_DQS=0`, `SQLITE_THREADSAFE=2`, `SQLITE_OMIT_DEPRECATED` | compiled in | a typo'd column name is an error rather than a constant string; no mutex per API call for a guarantee the engine does not need |

5. **The persistent tables are keyed for the access that dominates.** `events(tile, sequence)` and `projections(tile, entity, kind)` are `WITHOUT ROWID` with the tile first, because every read is "this tile" — a replay from a sequence, or a tile activating ([05 §5.5](../plan/05-simulation.md#55-reconciliation-when-a-tile-activates)) — and a tile-first key makes both one contiguous B-tree walk. `projections` carries a secondary index on `(entity, kind)` for point lookup, and an entity that moves between tiles is a delete and an insert because its tile is part of its key. `snapshots` is a rowid table whose rowid is the tile. **A persistence flush sorts its projections by tile before writing**: clustered writes measured 4.5× faster than the same rows round-robined over 1,024 tiles.

6. **Capabilities that need spatial queries put the discriminator in the archetype.** E6 measured a coarse grid held as a component to be *slower* than no grid (1.8×), and the same grid held as a relationship to be 13× faster, because an archetype ECS indexes by what an entity is and not by where it is. A query that must skip entities narrows by relationship; a component the query reads and rejects on is not an index. The cost is table count, and a capability that adds a relationship says in its docs page what it does to the archetype set.

7. **Deliberately not decided: whether flecs' pipeline stays the tick scheduler.** E6 measured the ceiling — 1.72× at four workers, falling to 1.17× at sixteen, from a serial fraction plus a per-tick worker handshake — and found that hierarchy propagation cannot be parallelized under flecs' row-splitting model at all. Those are limits of flecs' *pipeline*, not of its storage or its queries. ADR-0027 already describes a scheduler the engine intends to own (`SystemDesc`, read/write sets, phases). Seam 2 is what keeps that option open at no cost: every system's declaration already lives in a table the engine owns, so replacing the executor reads that table in a different order and touches not one line of a system's data access. Revisit when the scheduler lands.

## Seams

Five, and only five. Each is narrow, each exists because something the engine guarantees would otherwise be flecs' to guarantee, and together they are what makes the cost of replacing flecs *"rewrite the system bodies and queries"* — which no wrapper avoids either, because query semantics differ between ECS libraries — **and nothing else**. Where they live: [docs/subsystems/ecs.md](../subsystems/ecs.md).

### 1. Components come from the schema IDL

A component is declared once, in a `.schema` file, as a struct carrying `@kind(component)`. `schemac` generates the C++ struct as today, plus `<schemas/<stem>_ecs.h>`: a registration function that registers it with flecs **under the schema's qualified name** (`engine.cloth.Panel` → the flecs path `engine::cloth::Panel`), **with member reflection where the schema type maps onto flecs meta** (scalars, enums as their underlying integer, `id128` as two `u64`, `vec2/3/4` and `quat` as two to four `f32`, fixed arrays of scalars — and a `string`, a `bytes`, a nested struct, a map or a `json` payload left out rather than described wrongly, because `core/schema` is the authority on those and two serializers that have to agree is a worse problem than a partial one), and **with the schema's `transient` marking carried as a trait**: `ecs::Transient` on the component entity, and `schema::TypeFlag::transient` on the descriptor for everything that does not link an ECS.

Why: **one type system.** The protocol, persistence, migrations and agents see the same type the systems iterate. The alternative — a C++ struct with a flecs registration beside a schema declaration of the same thing — is two descriptions with one owner each, and they drift. A hand-registered flecs component is allowed only for module-private transient state (`ecs::register_private_component`), is never persisted, and is never visible to the protocol.

This also settles [03 §3.4](../plan/03-data-model.md#34-the-runtime-world)'s "transient components are marked `transient` in the schema": the marking is now a struct-level `@transient` in the IDL, refused on anything that is not a component, because a component is the only thing the store writes whole.

### 2. Systems register through the engine's descriptor

`ecs::register_system(sim, desc, make)`: `desc` is `sim::SystemDesc` — phase, read and write component sets, LOD tiers, determinism stance — declared to the engine, and the flecs system is created from the same call, by a callable that is handed the phase entity so `.kind()` cannot be forgotten or disagree. **`sim::TickPhase` becomes the one phase enum and `ecs::TickPhase` an alias of it**; `domain/ecs` therefore depends on `domain/sim`, and never the reverse, because the scheduler must stay free of flecs for decision 7 to stay answerable.

In debug builds the declared sets are checked against the query's terms and a mismatch asserts. Only in one direction: a term the descriptor does not declare is an error, because the schedule would then run the system in parallel with a writer and nothing downstream could tell; a declaration the query does not use is *not*, because that is how a system declares what it touches outside its terms — a singleton, a pool it writes through an index. Under-declaring is invisible and costs correctness; over-declaring is visible in `schedule_hash()` and costs parallelism. Relationship terms are counted and not masked: `sim::ComponentMask` addresses plain components by index, and deciding what a relationship's bit means is the scheduler's design.

v1 keeps flecs' pipeline as the executor inside the phases. The point of the seam is that the executor can change without touching any system's registration.

### 3. Persistent identity is `Id128`

An `ecs::Identity` component plus one map in `domain/ecs`, `Id128` to flecs entity and back, O(1), maintained on create and destroy. **Nothing outside `domain/ecs` stores a flecs entity id in anything that outlives a tick's working set**; the store and the protocol speak `Id128`.

The map is one direction only, because the entity carries the component and the reverse lookup is a component read. It is maintained by `OnSet`/`OnRemove` **observers** rather than by the call sites, so it is right when an entity is destroyed by something that never heard of it — a `delete_with`, a tile teardown — and so a raw `world.entity().set<Identity>({id})` joins it. That is what makes this a seam rather than a convention reviews enforce.

### 4. Mutation from outside systems is guarded

`ecs::WorldCommands`: create and destroy an entity by `Id128`, set and remove a component by schema type name from JSON (through the generated reflection) or from bytes, batched and applied at a phase boundary. Raw `flecs::world&` is for systems; this is the hook the plan's world-mutation guard, the persistence layer, the protocol and later the network use.

The payload is parsed when it is queued, not when it is applied, so a caller learns its JSON was wrong while it still has somewhere to report it and `apply()` cannot fail on a parse. It is **not a transaction** — commands apply in order and failures are counted, because the event log is what makes an edit undoable ([ADR-0003](0003-event-sourced-persistent-state.md)) — and it is not thread-safe.

### 5. Include hygiene

`flecs.h` may be included only by `domain/ecs`, `systems/*`, `game/*`, and their tests and benches. `tools/lint.ps1` enforces it per directory, under CTest, with its own tests. So `physics`, `nav`, `anim`, `gfx`, `geometry`, `sim`, `store` and everything in `core/` and `foundation/` stay ECS-free, and bridging code lives in `systems/`.

This is the seam that makes the other four's promise checkable rather than aspirational: the set of modules a flecs replacement would have to touch is the set the linter names, and it is checked on every push.

### The guard rails E6's two traps earned

- **A debug-build watchdog on the table count.** A world declares the archetype count its component design expects (`SimWorldConfig::table_watch`) and a debug build logs a warning past a configurable multiple of it. E6's measurement is the reason: relationship cross-products took the same data from 1,330 tables to 9,096, the tick from 2.79 ms to 4.71 ms and memory from 162 to 773 bytes an entity, with nothing failing. A warning and not an assert, because the number is a design estimate and a world legitimately grows tables while it loads.
- **"Where things are, not what they are."** Documented on the module's page as a rule with its measurement attached: spatial lookup is a relationship or a structure the system owns, never a per-entity cell component (13× against 1.8× *slower* than no index at all in E6), and choosing the relationship means accepting its effect on the table count, which the capability's docs page has to state.

## Docs touched

| Page | What changed |
|---|---|
| [docs/subsystems/ecs.md](../subsystems/ecs.md) | Rewritten around the five seams: what you may do with flecs directly, what goes through the engine and why, the two guard rails with E6's numbers, and where the protocol will attach |
| [docs/subsystems/sim.md](../subsystems/sim.md) | `sim::TickPhase` is the one phase enum; what the registration seam means for the scheduler's table |
| [docs/subsystems/schema.md](../subsystems/schema.md) | `TypeInfo::flags` against `tag`, and the ECS backend as schemac's fifth output |
| [schemas/README.md](../../schemas/README.md) | Struct-level `@transient`, the "Components" section, and `<stem>_ecs.h` |
| [docs/plan/03-data-model.md §3.4](../plan/03-data-model.md#34-the-runtime-world) | Status note: seams 1 and 3 settle this section's entity storage and vocabulary |
| [docs/plan/05-simulation.md §5.2](../plan/05-simulation.md#52-sim-scheduler) | Status note: the declaration seam, and that the executor is still flecs' |
| [docs/plan/10-roadmap-risks.md §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing) | E6 marked **Done** with the decision |

## Consequences

The engine gets relationships as a first-class runtime concept, which is what [03 §3.9](../plan/03-data-model.md#39-canon-and-narrative-representation)'s typed relations and [05 §5.7](../plan/05-simulation.md#57-world-events-and-consequences)'s consumers-by-type were always going to need, and it gets them without an abstraction tax. Every consumer of `domain/ecs` compiles against flecs' API directly, so a flecs major version is a change across every module the seam-5 linter names — `domain/ecs`, `systems/`, `game/` — rather than a module-local one; the pin is in `cmake/EngineEcs.cmake` and moving it is a change with a measurement attached. What that list does *not* contain is the point: it is bounded, it is checked on every push, and it is the whole cost of the decision.

The seams cost something too, and it is worth naming. A component can no longer be added by writing a C++ struct: it goes in a `.schema` file and the build regenerates. A system can no longer be registered with one flecs call: it needs a `SystemDesc` whose masks have to be right, and a debug build will stop it if they are not. Both are deliberate friction at the one moment when the information is cheapest to write down, and both are the price of the type system and the schedule being the engine's rather than emergent. The friction is bounded by `tools/new-capability.ps1`, which already scaffolds the schema file and the descriptor.

Simulation LOD stops being an optimization and becomes load-bearing. Four arithmetic systems over 100,000 entities cost 1.6 ms of a 16.6 ms frame, and the LOD assignment alone is 1.6 ms single-threaded; a real system set at that population does not fit, which is precisely what the tiers of [05 §5.4](../plan/05-simulation.md#54-lod-tier-assignment) exist for. Every capability's LOD policy is now a budget commitment, not a formality.

Relationships fragment archetypes as a cross product, and the engine now has to care: E6 measured a world whose children carried an independent faction at 9,096 tables instead of 1,330, 1.7× the tick time and 4.8× the memory, with no change to the data. Component and relationship design becomes a performance review item, and the size table's sibling metric — table count for a reference scene — belongs in the same conversation.

The store's settings are now the engine's, not SQLite's defaults, and they are compiled in where they can be (`cmake/EngineStore.cmake`) so no caller can turn them off by accident. A save file is a single 245 MB file at a million LOD3 records, readable by `sqlite3` on any machine, which is the introspection surface [03 §3.5](../plan/03-data-model.md#35-persistent-world-state) wanted. `synchronous=NORMAL` is an accepted durability trade: a crash may lose the last transactions, and the event log is what replays them.

What is now forbidden: an engine-wide ECS abstraction layer or a `std::function`-dispatched component accessor; a hand-written flecs component that is persisted or reachable from the protocol; a flecs entity id stored anywhere outside `domain/ecs` that outlives a tick's working set; `#include <flecs.h>` outside `domain/ecs`, `systems/` and `game/`; a world mutation from outside a system that does not go through `WorldCommands`; a spatial query implemented as a component filter and called an index; a per-row transaction in a persistence flush; and a projection write that is not clustered by tile.

## Revisit when

The engine's own scheduler lands and `SystemDesc` needs to drive the tick (decision 7, the expected case); a profile of a real system set shows flecs' iteration rather than the systems' own work at the top; flecs 5 changes the query or relationship API enough that the pin is a migration; a sixth seam is proposed, which is a change to this ADR and not to a module — the list is closed on purpose, and the argument for adding to it has to be that something the engine guarantees is otherwise flecs' to guarantee; `sim::ComponentMask` needs to express a relationship, which is the scheduler's design and the one gap seam 2 knowingly leaves; the store's write rate outgrows one SQLite connection, at which point per-tile connections or a write-ahead queue on an efficiency-pool thread is the next design; or a target platform appears whose storage makes WAL the wrong default.

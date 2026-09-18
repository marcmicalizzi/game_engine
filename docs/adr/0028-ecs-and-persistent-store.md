# ADR-0028: flecs is the runtime entity store, exposed directly; SQLite is the persistent store, tuned by measurement

- **Status:** Proposed
- **Date:** 2026-09-17
- **Plan references:** docs/plan/03-data-model.md §3.4 and §3.5, docs/plan/05-simulation.md §5.2, §5.4, §5.5 and §5.6, docs/plan/08-toolchain.md §8.6, docs/plan/10-roadmap-risks.md §10.5 (E6), docs/experiments/e6-ecs-store.md. Builds on ADR-0003 (event-sourced persistent state in SQLite), ADR-0012 (adopted libraries; "flecs pending E6"), ADR-0010 (deterministic sim and the LOD contract), ADR-0027 (additive capabilities). Supersedes nothing.

## Context

[03 §3.4](../plan/03-data-model.md#34-the-runtime-world) recommends evaluating flecs before writing an archetype ECS, for reasons specific to this project — first-class relationships that the persistent world needs anyway, a query language that maps onto `query_world`, reflection, an explorer — and names one concern: general-purpose iteration is not the fastest possible hot loop. [ADR-0012](0012-adopted-libraries.md) adopted SQLite outright but left flecs "pending E6". [03 §3.5](../plan/03-data-model.md#35-persistent-world-state) chose SQLite for the event log and projections but fixed none of the settings that decide whether it is fast enough, and left the table shapes open.

Experiment **E6** ([results](../experiments/e6-ecs-store.md)) built both capabilities and measured them: 10^5 entities with relationships, a hierarchy and four systems a tick, and 10^6 LOD3 projection records with the reads and writes the simulation will actually issue. It answered the ECS question, answered the store's settings with numbers rather than defaults, and raised a question about the *scheduler* that is deliberately left open below.

## Decision

1. **flecs 4.1.x is the runtime entity store**, in `domain/ecs`, an optional capability under `ENGINE_WITH_ECS` ([ADR-0027](0027-additive-capabilities.md)).

2. **flecs is exposed directly. The engine does not wrap it.** `ecs::SimWorld::world()` hands out the `flecs::world`, and systems, components, queries, and relationships are flecs'. A wrapper would have to re-expose relationships, wildcards, change detection, staging, the query DSL, and reflection to be useful, it would cost indirection on every query forever, and it would not deliver the portability it appears to buy, because swapping an archetype ECS is a rewrite of every system's data access whatever sits in between. The insurance against a bad library choice is the one [03 §3.4](../plan/03-data-model.md#34-the-runtime-world) already names — hot systems own their own data; the ECS holds identity, relationships, and gameplay components — and it is architectural, not an interface.

3. **What `domain/ecs` adds, and all it adds**: the tick phases of [05 §5.2](../plan/05-simulation.md#52-sim-scheduler) as flecs phase entities in order, chained after flecs' last built-in phase so anything left in a built-in one runs before the engine's tick rather than inside it; a fixed step from `core/time` with `SimTick` and `GameTime` published as singletons (core/time's own types, not new ones); an adapter that runs flecs' workers on `core/jobs`' performance pool through `ecs_set_task_threads` and the `ecs_os_api_t` task hooks; and flecs' log routed into `core/log`. A capability names a phase with `.kind(sim.phases()[TickPhase::Lod])`, which is ADR-0027's registration point for a ticking system.

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

7. **Deliberately not decided: whether flecs' pipeline stays the tick scheduler.** E6 measured the ceiling — 1.72× at four workers, falling to 1.17× at sixteen, from a serial fraction plus a per-tick worker handshake — and found that hierarchy propagation cannot be parallelized under flecs' row-splitting model at all. Those are limits of flecs' *pipeline*, not of its storage or its queries. ADR-0027 already describes a scheduler the engine intends to own (`SystemDesc`, read/write sets, phases). Decision 2 is what keeps that option open at no cost: nothing wraps flecs, so replacing the pipeline later touches the phase table and the systems' registration, and not one line of a system's data access. Revisit when the scheduler lands.

## Consequences

The engine gets relationships as a first-class runtime concept, which is what [03 §3.9](../plan/03-data-model.md#39-canon-and-narrative-representation)'s typed relations and [05 §5.7](../plan/05-simulation.md#57-world-events-and-consequences)'s consumers-by-type were always going to need, and it gets them without an abstraction tax. Every consumer of `domain/ecs` now compiles against flecs' API directly, so a flecs major version is an engine-wide change rather than a module-local one; the pin is in `cmake/EngineEcs.cmake` and moving it is a change with a measurement attached.

Simulation LOD stops being an optimization and becomes load-bearing. Four arithmetic systems over 100,000 entities cost 1.6 ms of a 16.6 ms frame, and the LOD assignment alone is 1.6 ms single-threaded; a real system set at that population does not fit, which is precisely what the tiers of [05 §5.4](../plan/05-simulation.md#54-lod-tier-assignment) exist for. Every capability's LOD policy is now a budget commitment, not a formality.

Relationships fragment archetypes as a cross product, and the engine now has to care: E6 measured a world whose children carried an independent faction at 9,096 tables instead of 1,330, 1.7× the tick time and 4.8× the memory, with no change to the data. Component and relationship design becomes a performance review item, and the size table's sibling metric — table count for a reference scene — belongs in the same conversation.

The store's settings are now the engine's, not SQLite's defaults, and they are compiled in where they can be (`cmake/EngineStore.cmake`) so no caller can turn them off by accident. A save file is a single 245 MB file at a million LOD3 records, readable by `sqlite3` on any machine, which is the introspection surface [03 §3.5](../plan/03-data-model.md#35-persistent-world-state) wanted. `synchronous=NORMAL` is an accepted durability trade: a crash may lose the last transactions, and the event log is what replays them.

What is now forbidden: an engine-wide ECS abstraction layer or a `std::function`-dispatched component accessor; a spatial query implemented as a component filter and called an index; a per-row transaction in a persistence flush; and a projection write that is not clustered by tile.

## Revisit when

The engine's own scheduler lands and `SystemDesc` needs to drive the tick (decision 7, the expected case); a profile of a real system set shows flecs' iteration rather than the systems' own work at the top; flecs 5 changes the query or relationship API enough that the pin is a migration; the store's write rate outgrows one SQLite connection, at which point per-tile connections or a write-ahead queue on an efficiency-pool thread is the next design; or a target platform appears whose storage makes WAL the wrong default.

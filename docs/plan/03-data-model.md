# 03 — Core Data and World Model

This is the most consequential design area in the plan (see [01-critique §1.8](01-critique.md#18-the-three-decisions-that-must-be-right-early)). It defines three data domains, how they relate, and the one dependency-tracking substrate that serves both the asset pipeline and world-generation canon.

## 3.1 Identity

- Every authored object has a **128-bit stable ID**, time-sortable with random suffix (ULID-style), assigned at creation and never reused. Names, paths, and labels are metadata, never identity.
- References between objects are by ID only. Path-based references are forbidden because agents rename and move things constantly.
- Runtime entities use a transient **handle** (32-bit index + generation). An ID↔handle map exists per session. Handles are never serialized.
- Content (meshes, textures, compiled data) is identified by **content hash** (BLAKE3 or xxHash3-128), not by ID, so identical generated outputs deduplicate automatically.

## 3.2 The authoring document ("World Document")

The authoring document is what designers and agents edit. It is *not* the runtime representation.

**Structure**

- A tree of typed **objects**, each with an ID, a schema type, a parent, and a set of typed properties. Types are declared in `schemas/` and code-generated.
- **Layers** hold sparse property assignments. A document is an ordered stack of layers; for each (object, property), the strongest layer that assigns it wins. There are no other composition operators (no inherits, variants, payloads, or references-with-overrides) in v1. Templates/prefabs are handled by an explicit *instantiate-and-record-origin* operation, not by live composition, because live composition makes "what does this edit change" non-local, which is exactly what agents handle badly.
- **Spatial partitioning of files.** A layer is stored as one file per tile: `world/<layer>/<tile>.wd`. Non-spatial objects (factions, quests, canon facts) live in `world/<layer>/global/<type>.wd`.
- **Layer roles:** `base` (generated/authored world), feature layers (a quest's placements, a settlement's props), proposal layers (an agent's in-progress work, promoted into a feature layer on acceptance), and `session` (ephemeral edits not yet committed).

**Serialization**

- Canonical JSON: sorted keys, fixed number formatting, one object per top-level array element, IDs as strings. Zero tooling cost, universally understood by agents, diffs acceptably in git. Revisit only if measured diff churn is a problem; a custom text format is the fallback, not the default.
- Large blobs (heightmaps, painted masks, splines with thousands of points) are separate content-addressed binary files referenced by hash from the JSON. The JSON stays diffable; blobs compare by hash.

**Why not OpenUSD.** USD's concepts (layers, prims, schemas, opinions) are right and are borrowed here. Its composition engine (inherits, variants, references, payloads with LIVRPS strength ordering) makes edits non-local, its C++ build is heavy, and it is unsuitable as a runtime format regardless. Adopt the vocabulary, provide USD import/export for interop with DCC tools through a dependency-free reader/writer (LightUSD, formerly tinyusdz) rather than the full OpenUSD build, and confirm with experiment **E4** ([10-roadmap-risks §10.5](10-roadmap-risks.md#105-experiments-to-run-before-committing)).

**Why not binary levels.** Agents cannot diff, review, attribute, or merge them. This is non-negotiable.

## 3.3 Transactions, diffs, and merges

- A **command** is a schema-typed message (`SetProperty`, `CreateObject`, `Reparent`, `Delete`, `ApplyPatch`, plus compound commands such as `PlaceAssetsAlongSpline`). Commands are the only way to change the document. GUI editor gestures compile to commands.
- A **transaction** groups commands, validates the result (schema, references, module invariants), and produces a **forward patch** and an **inverse patch**, both structural (keyed by object ID and property), both serializable. Undo applies the inverse. Rollback of an uncommitted transaction is free.
- Every transaction carries **attribution**: actor ID (human or agent), role, task ID, optional rationale, and the checkpoint it was based on.
- The **session journal** appends committed transactions. A **checkpoint** serializes the layers and commits to git with the journal segment as the commit body. Journals give fine-grained history; git gives coarse, branchable history.
- **Diff** between two document states is structural: per object, per property. Line diffs are never shown to agents.
- **Merge** is a structural 3-way merge: conflicts arise only when the same (object, property) differs on both sides, or when structural operations collide (delete vs. modify, both reparent). Spatial conflicts (two agents placing overlapping objects) are not merge conflicts; validators catch them after merge. Agents working in parallel use proposal layers plus tile leases ([06-agent-tooling §6.5](06-agent-tooling.md#65-permissions-concurrency-and-leases)), which makes true merge conflicts rare.

## 3.4 The runtime world

The runtime world is a *materialization* of (document + persistent state) at a chosen simulation LOD for a set of loaded tiles. It is rebuilt on load and discarded on unload; it is never the source of truth for anything persistent.

**Entity storage.** Archetype ECS. Recommendation: **evaluate flecs first** rather than writing one. Reasons specific to this project: built-in reflection and JSON serialization, a query language that maps almost directly onto `query_world`, first-class **relationships** (`(WorksAt, factory_17)`, `(MemberOf, faction_x)`) that the persistent-world design needs anyway, and a REST/explorer interface that is a ready-made introspection surface. Concern: general-purpose ECS iteration is not the fastest possible hot loop. Mitigation is architectural, not library choice: **hot systems own their own data.** Physics state lives in Jolt, render instances in GPU buffers, animation poses in the animation system's SoA pools; the ECS holds identity, relationships, and gameplay components, and hands out indices into those pools. If flecs fails measured performance targets for gameplay-component loops, EnTT or a small custom archetype store is the fallback; the interfaces above keep that swappable. Decide by experiment **E6**.

**Materialization contract.** Every system that participates in sim LOD implements:

```
Materialize(record, tier)     create runtime components/resources for this tier
Promote(entity, from, to)     add fidelity   (e.g. LOD2 -> LOD1: allocate nav agent)
Demote(entity, from, to)      summarize and drop fidelity (LOD1 -> LOD2: write coarse position)
Dematerialize(entity)         flush persistent deltas, free runtime state
```

Transient components are marked `transient` in the schema and are never persisted.

**Status.** Decided by [ADR-0028](../adr/0028-ecs-and-persistent-store.md), 2026-09-18: **flecs is the entity store, exposed directly, with five seams the engine owns** ([ecs](../subsystems/ecs.md)). The one that settles this section is seam 1 — a component is declared once in a `.schema` file as `@kind(component)`, and `schemac` emits its flecs registration alongside its C++ type, so the protocol, persistence, migrations and the systems that iterate it all see one type. "Transient components are marked `transient` in the schema" is now literally true: `@transient` on a component struct reaches C++ as `schema::TypeFlag::transient` for everything that does not link an ECS, and as the `ecs::Transient` tag on the component entity for everything that does. Seam 3 fixes the other half of this section's vocabulary — a runtime entity is named by `Id128` everywhere outside a tick's working set, and a flecs entity id is never stored anywhere that outlives one. The **materialization contract** above is built as a hooks table in [sim](../subsystems/sim.md) and is not yet driven from an ECS world. What E6 also settled: the mitigation in this section — hot systems own their own data — is not optional advice, because an archetype ECS indexes by what an entity *is* and never by where it is; a spatial discriminator is a relationship or a structure the system owns, never a per-entity cell component ([ecs](../subsystems/ecs.md), "where things are, not what they are").

**Status, the materialization (2026-09-24).** The runtime world is now a materialization of the document, and the contract above is driven. **What exists:** *the type mapping is data* — a `materialize` declaration in the IDL says which components a record type becomes, which property fills which field (with unit conversions where both sides declare `@unit`), whether the document's parent becomes `ChildOf`, at which tiers, and which fields the world may write back; schemac compiles it into a table and nothing is written per type ([schema](../subsystems/schema.md#materialization-type-mapping-as-data)); a record type with no declaration stays document-only, by design. *The driver*, `sim::Materializer` / `sim::materialize(document, world, scope)`, walks the document's live records through its composed index into the scheduler's hooks table, parents before children and then by id, whole or one tile of the partitioned layers, the first pass in full and later ones through a change feed the document now keeps, so an unchanged record costs nothing ([sim](../subsystems/sim.md#the-driver-a-document-through-the-hooks)); the entity store's hook creates each entity keyed by its record's `Id128` — a record and its entity are one identity across saves — and never cascades a deletion the document did not make ([ecs](../subsystems/ecs.md#materialization-the-entity-stores-hook)). *Write-back*: at `Persist`, the `@writeback` fields systems changed become document commands attributed to `system`, and events in the store's log. *One executor*: the engine's scheduler owns the tick ([ADR-0038](../adr/0038-the-scheduler-owns-the-tick.md), proposed). engine-host's `session.run_headless` and `session.materialize` run all of it. **What does not exist yet:** materialization *from the persistent store* — `reconcile_tile`'s `TileStore` over `foundation/store` is still only a test fake, a store record carries no source for a hook to build components from (its projections are opaque bytes), and nothing writes projections; `promote`/`demote` for materialized records (the host materializes at LOD0 and has no observer); and `Dematerialize`'s "flush persistent deltas" beyond the write-back to the document.

## 3.5 Persistent world state

Everything that has *happened* since the document was authored: destroyed buildings, NPC deaths, quest progress, economic state, player-caused changes.

**Event-sourced.** The primary record is an append-only log of `WorldEvent`:

```
WorldEvent {
  id          u64          monotonic per session
  sim_tick    u64
  game_time   i64          seconds
  type        schema-typed
  subject     ObjectId
  cause       EventId?     causal parent
  depth       u16          causal chain length; bounds cascades
  tile        TileId
  payload     type-specific, schema-typed
  origin      Deterministic | Player | Agent | LLM | Debug
}
```

**Projections.** Current state per object ("building_217 destroyed; rubble cleared at T") is a *projection* of the log, kept in memory for loaded tiles and in the persistent store for all tiles. Systems read projections during play; nothing scans the log per frame.

**Storage: SQLite** in WAL mode holding the event log, projections, and indices. Reasons: transactional and crash-safe; a single-file save game; agents and tests can run **SQL** against world state, which is the most powerful introspection surface available for nearly zero engineering cost; schema migrations are a well-trodden path. SQLite is the durability and query layer; per-frame hot reads come from in-memory projections. Expected write rates (hundreds to low thousands of events per second at peak, batched per tick) are far below SQLite's WAL-mode capacity.

**Snapshots.** Periodic full projection snapshots bound replay length. Save game = latest snapshot + events since. Load = apply snapshot, replay tail.

**Non-determinism is logged, not re-executed.** Wall-clock reads, LLM outputs, external generator outputs, and human/agent edits during play are recorded as events with `origin`. Replay reads them from the log. This is what makes the runtime-LLM tiers compatible with deterministic replay.

**Status, save and load (2026-09-25).** A running world saves and loads ([world](../subsystems/world.md#save-and-load), [ADR-0042](../adr/0042-a-save-is-the-store-the-document-and-the-drivers.md), proposed). **A save is a directory**: the document's files, a backup of the whole store (`VACUUM INTO`, with its bytes made a function of its content so a save round-trips byte for byte), the run's clock and world seed, the tile ring's observers and active tiles — history, not a function of where the observers are — the player's input log up to the save's tick, and a manifest naming and hashing every file and every format's version. **"Save game = latest snapshot + events since" is kept inside the store rather than as the save's shape**: the save carries the whole database, because the projections are still written directly when a tile goes rather than folded from the log, so the log cannot rebuild them, and because `session.events` reads the log's history. What a load rebuilds rather than reads — the runtime world's entities, tile seeds, reconciliation's work, the input state — is derived, and to make it exactly derivable the runtime world had to become what [§3.4](#34-the-runtime-world) says it is: records a system moves into another tile now follow their tile, so what is materialized is a function of the document and the live tiles. A load makes the world at the save's tick and continues the run; engine-host's `session.save_game`, `session.load_game` and `session.state_hash`, and the MCP bridge's tools of the same names, are the surface. Not done: an incremental or trimmed save; a save while a call is running (between calls nothing is pending); an agent's edit to a live tile's record between calls reaches the world only when the tile next comes in, so a save taken with one pending loads with it applied.

## 3.6 Dependencies and invalidation

One substrate serves the asset pipeline, world-generation canon, visual and semantic mips, validation caches, and playtest result caches.

**Model.** A node is identified by the hash of (function identity + version, input hashes). Two node classes:

| Class | Producer | On input change |
|---|---|---|
| `derived` | Pure function (mesh→clusters, tile→navmesh, region→semantic summary, document→dependency index) | Recompute automatically, in parallel, cache in the content-addressed store |
| `authored` | Human or agent judgment (a quest, a region's layout, a faction's history) | Mark **stale**, keep the old value live, enqueue a review item naming what changed upstream |

**Tracked reads.** A build function receives its inputs through a tracked accessor; every read is recorded as a dependency. Undeclared reads (raw file access) are blocked in the build sandbox. This is how Bazel and Salsa achieve correctness without asking humans to maintain dependency lists, and it is essential here because agents will not maintain them either.

**Storage.** Content-addressed store on disk (files named by hash) plus an SQLite index of nodes, edges, and staleness. Shareable across machines; deterministic outputs from any machine are interchangeable.

**Canon example.**
```
world_history.founding_war                 (authored)
  └─► faction.ironbound.motivation         (authored, depends on above)
        ├─► quest.q17.premise              (authored)
        └─► region.r3.semantic_summary     (derived)
```
An environment agent proposes a change to `founding_war`. On acceptance the substrate marks `motivation` and `q17.premise` stale with review items, and recomputes `r3.semantic_summary`. Nothing is regenerated without a decision, and nothing stale is silently used without being flagged.

## 3.7 Spatial partition

- A fixed **tile grid** (size chosen per game; 64–128 m is typical) with a quadtree of levels `L0..L4` over it. The same grid is the unit of: document layer files, streaming, persistent-state partitioning, nav tiles, acceleration-structure residency, agent edit leases, and world-gen mips. One partition, many consumers.
- World positions are stored as (tile index, float local offset) and rendering uses camera-relative transforms, so float32 precision holds at any world size ([02 §2.7](02-architecture.md#27-generality-what-the-engine-must-not-preclude)).
- Tiles are columns (full vertical extent). Interiors are either in the column or in **interior cells** reached through portals and streamed independently.
- Per-tile spatial index (BVH or grid) for queries below tile granularity. Cross-tile queries go through the level hierarchy.

## 3.8 Schema evolution

- Every schema type carries a version. The generator emits migrators for additive changes automatically; removals and semantic changes require a hand-written (or agent-written) migration function checked in next to the schema.
- Document files, snapshots, and events carry their schema version. Migration happens on read: events are upcast; snapshots migrated on load; documents migrated on open and rewritten on the next checkpoint.
- A **migration corpus** of old documents and saves lives in `content/migration-corpus/`; every migration is tested against it in CI. Deleting a migration is forbidden while any corpus entry needs it.
- Rename is an alias, never delete-and-add.

**Status (2026-09-25): the migration corpus exists, for saves.** `content/migration-corpus/saves/` holds one save game per version of the save format — `v1`, the first, and `v2`, the current — and `apps/engine_cli/tests/save_tests.cpp` loads every one on every build and holds it to the hashes `content/migration-corpus/README.md` records, on MSVC, Clang and GCC. Two migrations are exercised by a genuine older save: the manifest's schema follows the `@since` rule on read (a field newer than the version the save names is refused if present and takes its default if absent — version 2 added `saved_by`), and the store's tables follow `user_version` steps (table version 2 added `events.payload_version`, so **events now carry their schema version**, one of the bullets above, with every old event at 1). Migrated, `v1` is the same world as `v2` to the bit. A save names every format it is written in and is refused when one is newer than the build's or missing from it ([world](../subsystems/world.md#save-and-load)). Not yet: documents in the corpus (a document's records take their type's defaults for fields they lack, which the materialization driver does, but no old document is committed), generated migrators for additive changes (the `@since` rule is applied by the save reader, not emitted by `schemac`), and upcasting an event's payload on read — the version is stored, and nothing reads an old payload yet.

## 3.9 Canon and narrative representation

Canon is authored data in the document, not prose in a model's context.

- Object types: `Character`, `Faction`, `Place`, `HistoricalEvent`, `Item`, `Fact`, `Quest`, `QuestStage`, `Scene`, `DialogueNode`, `StyleGuide`.
- Typed relations (document references at authoring time, ECS relationships at runtime): `member_of`, `located_at`, `caused_by`, `knows`, `owns`, `contradicts`.
- Every canon object has **provenance** (who, which stage, which task, when) and a **canonicity** level: `proposed` → `accepted` → `deprecated`. Only `accepted` canon may be depended on by shipped content; validators enforce it.
- **Quests** are explicit state machines. Stage preconditions and completion conditions are **world predicates** over persistent state: `intact(bridge_217)`, `alive(npc_x)`, `has(player, item_y)`, `relation(a, b) > 0.3`. The **narrative dependency index** (a derived node) maps every world object to the quests and scenes whose predicates mention it. Story protection consults this index ([05-simulation §5.9](05-simulation.md#59-narrative-dependencies-and-story-protection)).
- Dialogue is a graph of authored nodes plus optional `generated` nodes that carry the constraint set an LLM must satisfy and a cache of accepted generations, so the runtime tier can fall back to cached text.

## 3.10 How deterministic and agent-generated state coexist

- Agent output is **authored content** compiled into the document before play. At runtime it is indistinguishable from human-authored content.
- During play only deterministic systems mutate persistent state, with two exceptions: player input and runtime-LLM outputs, both logged as events and replayed from the log.
- Agents editing a *live* session (for iteration) do so through transactions that are also logged as `Agent`-origin events, so a live-edited session remains replayable.

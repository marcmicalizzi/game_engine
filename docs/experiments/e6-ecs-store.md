# E6: flecs at 10^5 entities, and a million LOD3 records in SQLite

- **Question ([docs/plan/10-roadmap-risks.md §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)):** does flecs hold 10^5 entities with relationships and a hierarchy inside a tick budget, and does SQLite hold 10^6 LOD3 projection records at the tick time, memory, and query latency the plan assumes? Together they decide the ECS choice and the shape of the persistent store ([03 §3.4](../plan/03-data-model.md#34-the-runtime-world), [03 §3.5](../plan/03-data-model.md#35-persistent-world-state)).
- **Date:** 2026-09-17. **Machine:** Intel Core i9-10980XE, 18 cores / 36 threads, one package, one NUMA node, one cache domain, 25 MB LLC, Windows 11, NVMe. **Build:** `msvc-release` (RelWithDebInfo). **Versions:** flecs 4.1.6, SQLite 3.53.4.
- **Machine state (added 2026-09-18):** not recorded at the time; the machine is shared with GPU diffusion workloads and parallel agent builds, and the harness did not yet know how to look ([bench](../subsystems/bench.md#measuring-on-a-shared-machine)). Everything below the horizontal rule is the original run. The headline CPU numbers were re-taken on 2026-09-18 on a verified-quiet machine and the results are in "[Re-measured on a quiet machine](#re-measured-on-a-quiet-machine-2026-09-18)" at the end of this page — **the store numbers came back 7–11% faster and the ECS worker-scaling table did not reproduce at all**, which is the single clearest argument for the rule that produced it.
- **The ECS worker-scaling table below is withdrawn**, and the reason is now known: see "[What the worker scaling actually was](#what-the-worker-scaling-actually-was-2026-09-18)". The tick does scale — 1.87× at four workers, 2.32× at eight — but only with flecs' own threads; hosting flecs' workers as per-tick jobs on `core/jobs` costs a flat 1.6–2.2 ms a tick and turns the speedup into a slowdown. Every other number on this page reproduces within a few percent.
- **That is fixed.** Later the same day the mechanism was found and the hosting changed: see "[Why per-tick hosting cost what it did](#why-per-tick-hosting-cost-what-it-did-2026-09-18)" and "[Worker hosting, re-measured](#worker-hosting-re-measured-2026-09-18)". flecs' workers are now long-running jobs on the performance pool, the tick scales to within a few percent of flecs' own threads at every worker count, and the decision is [ADR-0030](../adr/0030-flecs-workers-are-long-running-pool-jobs.md).
- **Decision:** [ADR-0028](../adr/0028-ecs-and-persistent-store.md), **Accepted** 2026-09-18. flecs stays unwrapped and the engine owns five seams; the two findings below that cost the most to learn — the relationship cross-product and "a component is not a spatial index" — became a debug watchdog on the table count and a rule on [ecs](../subsystems/ecs.md) rather than staying paragraphs in this file. The worker-scaling half was **withdrawn** the same day and settled by [ADR-0030](../adr/0030-flecs-workers-are-long-running-pool-jobs.md), which refines ADR-0028 decision 3 rather than editing it.

## Method

Two modules were built, because the engine needs both whatever the answer is, and an experiment against a throwaway harness would have measured a throwaway harness:

- **`domain/ecs`** ([docs/subsystems/ecs.md](../subsystems/ecs.md)) is flecs, exposed directly, plus the engine's tick phases ([05 §5.2](../plan/05-simulation.md#52-sim-scheduler)) as flecs phase entities, a fixed step from `core/time` with `SimTick` and `GameTime` as singletons, an adapter that runs flecs' workers on `core/jobs`' performance pool, and flecs' log routed into `core/log`.
- **`foundation/store`** ([docs/subsystems/store.md](../subsystems/store.md)) is SQLite with `Status`-returning errors, a prepared-statement cache, RAII transactions, `user_version` migrations, and the event log, projections, and per-tile snapshots of [03 §3.5](../plan/03-data-model.md#35-persistent-world-state).

The benchmarks are `domain/ecs/bench/ecs_bench.cpp` and `foundation/store/bench/store_bench.cpp`, run through `foundation/bench` (seven timed repeats after calibration and a warmup; the tables below are medians). They are part of the tree and rerun on any machine with:

```powershell
pwsh tools/dev.ps1 build -Preset msvc-release
build/msvc-release/domain/ecs/engine_ecs_bench.exe --filter=ecs.*
build/msvc-release/foundation/store/engine_store_bench.exe --filter=store.*
```

Each prints the CPU topology, the build, and the scene or corpus parameters as `#` lines, so a result file says what produced it.

**The ECS world.** 100,000 entities with `Position`, `Velocity`, `Wealth`, `Importance`, `Cell`, `Tier` and a `(FactionOf, faction)` relationship to one of 32 faction entities. 10% are children of 1,000 parent entities, one level deep, with `WorldPos` propagated from the parent. Three observers. Four systems a tick, in the engine's phases:

| System | Phase | Parallel | What it does |
|---|---|---|---|
| `move` | Systems | yes | `Position += Velocity`, the trivially parallel floor |
| `propagate` | Systems | no | `WorldPos = Position + parent.WorldPos`, `.parent().cascade()` |
| `faction_aggregate` | Systems | no | sums `Wealth` per faction through the relationship, one table per faction |
| `lod_assign` | Lod | yes | tier from the minimum over observers of f(distance, importance, weight), with a two-tick hysteresis dwell ([05 §5.4](../plan/05-simulation.md#54-lod-tier-assignment)) |

**The store corpus.** 1,000,000 projection records over 1,024 tiles, 64–256 byte blobs, written tile by tile in transactions of 10,000.

## ECS results

**Tick time, 100,000 entities, four systems.** Workers count the calling thread, so *n* means one main thread and *n-1* flecs workers on the job system's performance pool.

> **Withdrawn 2026-09-18.** This table does not reproduce and the worker column cannot be matched to a configuration the engine ships: the numbers have the shape of flecs' own OS threads, which is not what `ecs::set_workers` gives a world. Read "[What the worker scaling actually was](#what-the-worker-scaling-actually-was-2026-09-18)" instead. It is left here rather than deleted because finding 1 under "What surprised me" is built on it and the correction is the interesting part.

| Workers | µs/tick | Speedup | Entity-visits/s |
|---|---|---|---|
| 1 | 2,790 | 1.00× | 36 M |
| 2 | 2,550 | 1.09× | 39 M |
| 4 | 1,625 | 1.72× | 62 M |
| 8 | 1,737 | 1.61× | 58 M |
| 16 | 2,377 | 1.17× | 42 M |

**Per system, single-threaded, over the same 100,000 entities** (measured as cached queries, which is what a system's query is):

| System | µs | ns/entity |
|---|---|---|
| `move` | 153 | 1.5 |
| `faction_aggregate` | 212 | 2.1 |
| `propagate` (by subtraction from the tick) | ~800 | 8.0 |
| `lod_assign` | 1,624 | 16.2 |

**Query: "entities of faction F within radius r of observer o"**, r = 256 over a 4,096-unit world, about 3,100 entities in the faction:

| Implementation | Latency | Against the scan |
|---|---|---|
| Scan the faction, distance test | 5.53 µs | 1.0× |
| Scan the faction, reject on a `Cell` **component** first | 10.04 µs | **1.8× slower** |
| One cached query per intersecting cell, cell as an `(InCell, cell)` **relationship** | 0.417 µs | **13× faster** |

**Churn**, 1,000 creates and 1,000 destroys a tick, entities of eight components:

| | µs | µs per entity operation |
|---|---|---|
| Plain | 2,261 | 1.13 |
| Inside `defer_begin`/`defer_end` | 1,664 | 0.83 |

**Memory and shape.** Process working set grows by **16.2 MB for 100,000 entities — 162 bytes per entity** — across **1,330 tables** and 32 component ids. Building the world from nothing (entities, components, relationships, hierarchy, four systems) takes 278 ms.

## Store results

**Corpus.** 1,000,000 records in **14.73 s (67,900 rows/s)** with a 64 MiB page cache; the file is **245 MB, 245 bytes a row** for 64–256 byte blobs, and the WAL reaches 30 MB before its checkpoint.

**Insert throughput** against rows per transaction, into a table held at ~200,000 rows:

| Rows per transaction | ms | rows/s |
|---|---|---|
| 1 | 0.087 | 11,500 |
| 100 | 2.08 | 48,200 |
| 1,000 | 5.38 | 186,000 |
| 10,000 | 47.1 | 212,000 |

**Insert order**, 1,000 rows a transaction, same rows, same table size:

| Order | ms per 1,000 | rows/s |
|---|---|---|
| Tile by tile (clustered on the primary key) | 7.48 | 134,000 |
| Round-robin over 1,024 tiles | 33.30 | 30,000 |

**Reads on the million-row corpus**, with SQLite's default 2 MiB page cache and with 64 MiB:

| Read | 2 MiB cache | 64 MiB cache |
|---|---|---|
| Point lookup by entity id (through the by-entity index) | 34.9 µs | 22.0 µs |
| Range by tile (~977 records, contiguous) | 442 µs | 317 µs |
| Hourly summarization, SQL aggregate over ~10,000 records | 7.23 ms | — |
| Hourly summarization, `UPDATE` over the same ~10,000 | 21.5 ms | — |

**Snapshots**, one tile of 10,000 records:

| | Time | Size |
|---|---|---|
| Write (pack + store) | 12.5 ms | 1.91 MB, 191 B/record |
| Read (fetch + walk) | 0.88 ms | |

**Event log.**

| | ms | events/s |
|---|---|---|
| Append, 1 event per transaction | 0.045 | 22,100 |
| Append, 64 events per transaction (about a tick's worth) | 1.04 | 61,700 |
| Append, 1,024 events per transaction | 3.59 | 285,000 |
| Replay one tile, 10,000 events | 3.94 | 2,540,000 |

**WAL against a rollback journal**, 1,000 rows in one transaction:

| Journal mode | ms | rows/s |
|---|---|---|
| `DELETE` (rollback journal), `synchronous=FULL` | 12.09 | 82,700 |
| `WAL`, `synchronous=NORMAL` | 6.08 | 165,000 |

**`upsert_projection` against `insert_projection`**, 1,000 rows: 5.49 ms against 5.38 ms — the extra seek of the by-entity index that makes a tile change correct costs about **2%**.

## What surprised me

1. **The parallel speedup stops at four workers and then goes backwards.** 1.72× at four, 1.61× at eight, 1.17× at sixteen, on a machine with eighteen cores. Two reasons, and only one of them is Amdahl's law. The serial half is real: `propagate` cannot be multi-threaded (see 3) and `faction_aggregate` writes one array, so about a third of the tick is serial and the ceiling at four workers is around 2.1×. The rest is flecs' sync protocol — each worker is created, signalled, and joined every tick, and each sync point is a condition-variable broadcast plus a spin on the main thread — which costs more than the work it hands out once the per-worker slice drops below a few hundred microseconds. **The useful number is not the speedup, it is 1.6 ms: four systems over 100,000 entities inside a 16.6 ms frame.**

   **Correction, 2026-09-18.** Half right, and the wrong half was the confident one. The serial fraction is real and so is the sync-protocol argument, but the measured ceiling was not flecs' — with flecs' own threads the tick keeps scaling past four workers (1.87× at four, **2.32× at eight**), and the falling-off shape belonged to *this engine's* way of hosting those workers. The sentence in bold survives intact and is the only part anyone should have budgeted on.

2. **The spatial grid as a *component* is slower than no grid at all** (10.0 µs against 5.53 µs), and as a *relationship* it is 13× faster (0.417 µs). This is the sharpest lesson in the whole experiment and it generalizes past flecs: an archetype ECS indexes by *what an entity is*, not by where it is, so the only way to make a query skip entities is to put the discriminator in the archetype. A component the query reads and rejects on adds a column to the iteration and saves nothing, because the distance test it replaces was already two multiplies. The cost of the relationship is table count: the same world with an 8×8 grid has 8,158 tables instead of 1,330.

3. **Hierarchy transform propagation cannot be parallelized under flecs' model.** `cascade()` orders *tables* so a parent's table is visited before its children's, but `multi_threaded()` splits each table's *rows* across workers and does not synchronize between tables, so a worker can read a parent's `WorldPos` before the worker that owns that row has written it. The system is therefore single-threaded and is the second largest cost in the tick (~800 µs). A per-depth barrier would fix it, and that is the engine's scheduler's job, not flecs'.

4. **Relationships multiply archetypes, and the multiplication is a cross product.** The first version of this benchmark gave children a random faction instead of their parent's, which is the difference between `(ChildOf, parent)` and `(ChildOf, parent) × (FactionOf, faction)`: **9,096 tables instead of 1,330**, the tick at **4.71 ms instead of 2.79 ms (1.7× slower)**, and **773 bytes per entity instead of 162 (4.8×)**. Nothing about the data changed. Relationships are the reason to choose flecs and they are also the sharpest edge in it.

5. **A million random-keyed inserts need a page cache bigger than the index.** With SQLite's default 2 MiB cache the corpus built at 16,600 rows/s; with 64 MiB it built at 67,900 — **4.1×** for one pragma. The by-entity index is keyed on random 128-bit ids, so every insert dirties a random page of a ~30 MB index, and the cache decides whether that page is in memory.

6. **Insert order matters as much as batch size.** Writing tile by tile is 4.5× faster than round-robin over 1,024 tiles for exactly the same rows (7.48 ms against 33.3 ms per thousand). The primary key is tile-first, so a clustered write appends at the right edge of the B-tree and a scattered one seeks. A persistence flush should sort by tile before it writes; that is a sort of a few thousand elements against a 4.5× write cost.

7. **Everything the plan predicted about scale was conservative, and the one thing it did not predict is the tick.** [05 §5.6](../plan/05-simulation.md#56-what-npc-scale-is-realistic) guesses "memory-bound at ~200 B each" for LOD3 records: measured, 162 B/entity in the ECS and 245 B/row on disk. It guesses hundreds to low thousands of events a second: measured, 61,700 a second at one transaction a tick. But it has no figure for what 10^5 *live* entities cost per tick, and the answer — 1.6 ms for four cheap systems, of which the LOD assignment alone is 1.6 ms single-threaded — says the LOD tiers are not an optimization, they are the design. Four systems is not a game.

8. **Building a flecs entity costs one archetype move per component.** 1.13 µs to create and destroy an entity of eight components; deferring the batch takes it to 0.83 µs. Materialization should be deferred or should set the whole archetype at once.

## What it decides

**flecs is a yes**, on the terms [03 §3.4](../plan/03-data-model.md#34-the-runtime-world) already set. It holds 100,000 entities with relationships and a hierarchy in 1.6 ms, its relationships are what makes the faction aggregate one table walk instead of a pointer chase, and 162 bytes an entity is within the plan's budget. Its weaknesses are exactly the ones the plan anticipated — general-purpose iteration is not the fastest possible hot loop, and its parallel model is coarse — and the mitigation stands: hot systems own their own data, and the ECS holds identity, relationships, and gameplay components.

**SQLite is a yes**, with four settings and one write discipline that the measurements, not taste, chose: WAL with `synchronous=NORMAL` (2× a rollback journal), 8 KiB pages, a page cache sized to the index rather than to the default, one transaction per tick, and projections written clustered by tile.

**What is not decided here.** Whether flecs' pipeline stays the scheduler. The parallel ceiling at four workers, the propagation barrier, and the per-tick worker handshake are all limits of flecs' pipeline rather than of its storage, and [ADR-0027](../adr/0027-additive-capabilities.md)'s `SystemDesc` table is a scheduler this engine intends to own. The likely end state is the engine's scheduler over flecs' storage and queries, which this module's shape already allows: nothing here wraps flecs, so replacing the pipeline does not touch a single system's data access.

## Caveats

- One machine, one storage device. The store numbers are NVMe numbers; a save file on a hard disk or a network drive changes every write row and the WAL discussion with it. The `synchronous=NORMAL` choice is a durability trade, not only a speed one: a crash can lose the last transactions, and the event log is what replays them.
- The tick has four systems, three of which are arithmetic. A real tick has perception, navigation, and state machines, and its parallel fraction and its cache behaviour will both be different.
- Worker counts above 4 oversubscribe nothing here — the machine has 18 cores — but the job system's performance pool and flecs' workers are the same threads for the length of a tick, so a tick with *n* workers makes *n-1* pool threads unavailable to anything else. Nothing else was running.
- `propagate`'s cost is by subtraction from the tick rather than measured directly, because a cascade query is only meaningful inside the pipeline that orders it.
- flecs 4.1.6 and SQLite 3.53.4. Both move; the pins are in `cmake/EngineEcs.cmake` and `cmake/EngineStore.cmake`.
- The `ecs.query.faction_radius.cell_relationship` figure uses cached queries built once. Uncached, the same query took 328 µs — 790× slower — because an uncached query re-matches every table on every iteration. That is a flecs API footgun worth knowing: `query_builder<...>().build()` is uncached, a system's query is cached.
- "Nothing else was running" (above) was an assumption, not a measurement. See the next section.

---

## Re-measured on a quiet machine (2026-09-18)

The harness now records what else the machine is doing and can refuse to measure a busy one ([bench](../subsystems/bench.md#measuring-on-a-shared-machine)). The headline CPU numbers of this experiment were re-taken with it on the same box, same build preset, same flecs and SQLite pins, with nothing in `domain/ecs`, `foundation/store` or `core/jobs` changed since — except one line, noted below. Two runs each: the first with `--wait-quiet=7200`, the second with `--require-quiet` and a retry until a run started *and* ended under 10% others' CPU with no WARNING.

**The store reads came back 7–11% faster**, and reproduce:

| Read | 2026-09-17 (state unrecorded) | quiet run A | quiet run B |
|---|---|---|---|
| Point lookup, 2 MiB page cache | 34.9 µs | 31.85 µs | 31.68 µs |
| Point lookup, 64 MiB | 22.0 µs | 20.13 µs | 19.62 µs |
| Range by tile, 2 MiB | 442 µs | *discarded* | 410.55 µs |
| Range by tile, 64 MiB | 317 µs | *discarded* | 292.66 µs |

The discarded pair is worth keeping in the record: that run began at 3.2% others' CPU and **ended at 86.3%** because somebody's parallel build started inside it, and it reported 948 µs and 584 µs — 2.3× and 2.0× the accepted figures, and 2.1× and 1.8× the ones this page recorded in September. The WARNING is the only reason anyone would have known.

**The ECS worker-scaling table did not reproduce at all.** Microseconds per tick, 100,000 entities, four systems:

| Workers | 2026-09-17 (state unrecorded) | quiet run A | quiet run B |
|---|---|---|---|
| 1 | 2,790 | 2,435 | 2,459 |
| 2 | 2,550 | 3,525 | 3,656 |
| 4 | **1,625** | 2,965 | 3,122 |
| 8 | 1,737 | 2,899 | 2,801 |
| 16 | 2,377 | 3,703 | 3,546 |

Single-threaded is **12% faster** than recorded, in line with the store's direction. Every multi-worker figure is **1.6× to 1.9× slower**, and the two quiet runs agree with each other to within 5% at every worker count. On these numbers the tick has no parallel speedup at all: one worker is the fastest configuration, and "1.72× at four workers" — the figure the "what surprised me" section is built on and the one [ADR-0028](../adr/0028-ecs-and-persistent-store.md) records — is not reproducible today.

**This is not attributable to background load**, and that is the point of saying so with a record rather than a shrug: a quiet machine made the *serial* number better and the *parallel* numbers much worse, which is not the shape of contention. Two candidates were named, neither confirmed at the time because `domain/ecs` was not that change's to edit. Both were settled on 2026-09-18 by the measurements below.

## What the worker scaling actually was (2026-09-18)

**Same machine, same build, same pins; `--wait-quiet=900`, one bench executable at a time, every figure below taken twice and agreeing within about 2% unless said otherwise.** Machine at the time of the accepted runs: others' CPU under 6%, GPU 0% at 3.0 GB of 32 GB, session unlocked.

### The suspected `lod_assign` line is not the cause

`lod_assign` reads the observer singleton once per invocation, and commit `8abcf43` changed how. Three forms of that one line were built and measured, each twice:

| Form of the singleton read in `lod_assign` | 1 worker | 4 workers |
|---|---|---|
| **A**, current: `const flecs::world w = it.world(); w.get<ObserverSet>()` | 2.382, 2.385, 2.383 ms | 2.889, 2.955, 2.987 ms |
| **B**, pre-`8abcf43`: `it.world().get<ObserverSet>()` | 2.554, 2.591 ms | 3.385, 3.514 ms |
| **C**, no wrapper: `ecs_get_id(it.c_ptr()->world, id, id)` | 2.568 ms | 3.367 ms |

Reverting the suspected commit (form B) makes the benchmark **slower**, not faster, and so does removing the `flecs::world` object altogether (form C). All three show the same shape — no configuration beats one worker, and two workers is the worst of them — so whatever causes it is present in the exact source that predates the suspicion. The 7% spread between forms is codegen around a 1.6 ms inner loop, not the wrapper: a `flecs::world` handle is one atomic increment and one atomic decrement (`flecs_poly_claim`/`release`), constructed once per system invocation, which is nanoseconds and cannot be 170 µs. **Candidate 1 is refuted.**

### The cause is the job-system task adapter

`ecs::JobOsApi` replaces only flecs' `task_new_`/`task_join_` hooks, so `ecs_set_threads` on the same world, with the same job system alive and the same scenes resident, runs flecs' own long-running OS threads and changes nothing else. That is the only difference between these two rows:

| Workers | `ecs.tick.four_systems` (task adapter — what the engine ships) | `ecs.tick.four_systems.os_threads` (flecs' own threads) | Adapter's cost |
|---|---|---|---|
| 1 | 2.459 / 2.446 ms | *same code path* | — |
| 2 | 3.804 / 3.892 ms (**0.64×**) | 1.675 / 1.677 ms (**1.46×**) | 2.17 ms |
| 4 | 2.943 / 2.955 ms (**0.83×**) | 1.373 / 1.246 ms (**1.87×**) | 1.64 ms |
| 8 | 2.919 / 2.932 ms (**0.84×**) | 1.050 / 1.064 ms (**2.32×**) | 1.87 ms |
| 16 | 4.016 / 4.079 ms (**0.61×**) | not measured | — |

Speedups are against the shared 1-worker baseline. With flecs' own threads the tick scales — **1.87× at four workers, 2.32× at eight** — which is the shape the original table recorded and slightly better than the 1.72× it published. With the adapter it does not scale at all: every worker count is slower than one worker, and the adapter's cost is a **flat 1.6–2.2 ms a tick, independent of how many workers there are**.

### And it is not job submission

The adapter's premise, written into its own header, is that "creating a 'thread' per tick costs a job submission, tens of nanoseconds, instead of an OS thread creation, tens of microseconds". The premise is *correct in isolation* and it is not what costs the time. `ecs.task.roundtrip` submits `workers - 1` empty jobs to the same pool and joins them the way `JobOsApi::join_task` does:

| Jobs submitted and joined | Round trip |
|---|---|
| 1 (2 workers) | 1.13 µs |
| 3 (4 workers) | 1.42 µs |
| 7 (8 workers) | 2.12–2.24 µs |
| 15 (16 workers) | 5.5–6.1 µs |

**Three orders of magnitude below the 1.6–2.2 ms the adapter adds to a tick.** So `core/jobs`' scheduling and wake path are not the problem and a fix aimed at them would be aimed at the wrong thing. What is left is what happens *after* a flecs worker is running on a pool thread rather than on a thread of its own: flecs parks every worker on its own condition variables at each of the pipeline's sync points, and a pool thread that is inside flecs' state machine is a thread the pool cannot use and a wait the pool's eventcount cannot see. The cost being flat in worker count rather than proportional to it says it is per *tick*, not per worker — consistent with the per-`progress()` create-and-join of the whole worker set that `ecs_set_task_threads` performs and `ecs_set_threads` does not.

### Everything else in this experiment reproduces

Taken in the same session, against the table further up this page: `move` 153 µs (was 153), `faction_aggregate` 211 µs (212), `lod_assign` 1.646 ms (1.624), the three radius queries 5.46 µs / 11.04 µs / 425 ns (5.53 / 10.04 / 417), churn 2.326 ms and 1.678 ms deferred (2.261 / 1.664), world build 275 ms (278). The per-system costs and the query and churn findings are unchanged; only the *hosting of parallel workers* moved. That is itself evidence: the systems do the same work in the same time, and the difference is entirely in how the tick's workers are obtained.

One caveat for whoever re-runs this. In a full `--filter=ecs.*` run the 1-worker tick reads 2.768 ms rather than 2.45, because by then more `Scene` objects — and more job systems' threads — are resident. The quoted figures come from `--filter=ecs.tick.*` alone, which is the [bench page](../subsystems/bench.md#measuring-on-a-shared-machine)'s "one bench executable at a time" rule applied one level further down.

### What it decides

1. **The published worker-scaling table is withdrawn and replaced by the one above.** Its shape was right and it was almost certainly measured with flecs' own threads; what the engine actually ships does not behave that way.
2. **The task-adapter design is refuted by its own experiment.** [ADR-0028](../adr/0028-ecs-and-persistent-store.md)'s consequences and [ecs](../subsystems/ecs.md) now say so, including the claim on that page that replacing `thread_new_` instead would be "strictly worse" — it would hold `workers - 1` pool threads for the life of the world, which is a real cost, and it is measurably smaller than 1.6–2.2 ms a tick.
3. **The conclusion E6 drew is unchanged**, and this is why it was drawn that way: **the number that matters is that four cheap systems over 100,000 entities fit in a 16.6 ms frame**, single-threaded, and the parallel speedup was never the reason to choose flecs. Nothing about the storage, the queries or the relationships is affected.

The pair of benchmarks that produced the finding is in the tree, so a fix has something to be checked against rather than a paragraph to be believed. **One renaming to know about when reading this section back:** at the time, `ecs.tick.four_systems` *was* the per-tick hosting, because that is what the engine shipped. Since [ADR-0030](../adr/0030-flecs-workers-are-long-running-pool-jobs.md), `ecs.tick.four_systems` is the long-running hosting the engine now ships and the per-tick one is `ecs.tick.four_systems.tasks`. Every "task adapter" column above is that second benchmark today.

---

## Why per-tick hosting cost what it did (2026-09-18)

The section above ends with "what is left is what happens after a flecs worker is running on a pool thread", which was a hypothesis with an argument attached rather than a measurement. Here is the measurement. **Same machine, same build, same pins; `--wait-quiet=900`, one bench executable at a time.**

A temporary probe was put on flecs' OS-API hooks in `ecs_bench.cpp`. It is worth writing down exactly what it did, because it is not in the tree: it replaces process-global hooks, which is a landmine to leave in a bench file that every other benchmark in it shares. It wrapped `task_new_` and `task_join_` to timestamp the main thread on either side of each call, wrapped the worker callback itself to timestamp the pool thread the moment the body started, counted every call to `mutex_lock_`, and added two no-op systems — one in the first phase, one in the last — that timestamp the main thread's entry into and exit from the pipeline. A tick then splits into **pre** (everything before the first system body), **pipe** (the systems), and **post** (everything after the last one), with the create, the wait and the join broken out of `pre` and `post`.

### One: flecs rebuilds the worker set every tick, and the main thread spins for it

`ecs_set_task_threads` makes `ecs_progress` call `flecs_create_worker_threads` on the way in and `flecs_join_worker_threads` on the way out. Between them, `flecs_workers_progress` calls `flecs_wait_for_workers`, which is a bare `lock / check / unlock` loop on one critical section with no yield, and it does not return until every one of the new workers has taken that same lock to announce itself. With `ecs_set_threads` the workers are already running and the same loop exits on its first iteration.

The count says it plainly. **Calls to `ecs_os_mutex_lock` per tick**, 100,000 entities, four systems:

| Workers | per-tick hosting | flecs' own threads |
|---|---|---|
| 2 | 5,400–5,900 | 4 |
| 4 | 9,900–11,600 | 6 |
| 8 | 17,900–18,600 | 10 |
| 16 | 33,000–50,000 | 18 |

Not "more locking": **three to four orders of magnitude** more, and it is one thread spinning on one lock. In wall clock, per tick:

| Workers | pre (create + wait) | post (join) | pipeline | pre + post, flecs' own threads |
|---|---|---|---|---|
| 2 | 0.19 ms | 0.13 ms | 3.09 ms | 4.3 µs |
| 4 | 0.34–0.39 ms | 0.19–0.22 ms | 2.23 ms | 4.6 µs |
| 8 | 0.65–0.73 ms | 0.35–0.38 ms | 1.71 ms | 4.6 µs |
| 16 | 1.21–1.71 ms | 0.61–0.76 ms | 1.48 ms | 5.0 µs |

That is **0.32 ms a tick at two workers rising to 2.5 ms at sixteen**, against 4–6 µs, for work that has nothing to do with the systems.

### Two: every tick's worker is a fresh job, and the pool worker that ran the last one is asleep

That first mechanism grows with the worker count and does not explain the rest: at two workers the *pipeline itself* is 3.09 ms against flecs' own threads' 1.70 ms. The control that settles it is the same per-tick adapter on a pool whose idle workers spin for far longer than a tick instead of sleeping (`ecs.tick.four_systems.tasks_never_sleep`, a `JobSystemConfig::spin_iterations` change and nothing else):

| Workers | per-tick, pool sleeps | per-tick, pool stays awake | flecs' own threads |
|---|---|---|---|
| 4 | pipeline 2.23 ms | pipeline **1.25 ms** | pipeline 1.25 ms |
| 8 | pipeline 1.71 ms | pipeline **0.99 ms** | pipeline 1.00 ms |

With the pool kept awake the pipeline costs what flecs' own threads cost, to within a percent, and only the per-tick protocol above is left. So the second cost is the **pool's wake path, paid once per worker per tick** — plus the fact that a pinned pool worker's core goes idle during every single-threaded stretch of the tick, which the same control removes. Both are the price of re-dispatching a long-running worker as if it were a short job.

The two costs pull in opposite directions as the worker count rises — the protocol grows, the wake shrinks — which is why the total looked *flat* at 1.6–2.2 ms and why "flat in worker count" was a misleading clue rather than a helpful one.

**And it is still not the job system.** `ecs.task.roundtrip` — the same number of empty jobs submitted to the same pool and joined the same way — reproduces at **1.16, 1.42, 2.12 and 4.4–5.7 µs** for 2, 4, 8 and 16 workers. The pool's spin-then-sleep policy is right for the short jobs it exists for; nothing in `core/jobs` was changed, and a fix aimed at it would have been aimed at the wrong thing.

### The `tasks_never_sleep` control is unstable, and that is worth saying

Its medians drift between about 1.6 ms and 3.0 ms at four workers across runs and even across repeats within a run, while its minimum sits at 1.6 ms. A pool of permanently spinning workers is not a state any machine should be in — it fights the OS for frequency and core-parking decisions — so the control is good for the qualitative split above and should not be quoted as a figure. The mutex counts and the pre/pipe/post split, which are counted rather than timed and reproduce to a few percent, are the load-bearing evidence.

## Worker hosting, re-measured (2026-09-18)

The fix is [ADR-0030](../adr/0030-flecs-workers-are-long-running-pool-jobs.md): `ecs::JobOsApi` now fills in flecs' `thread_new_`/`thread_join_` hooks as well, and `ecs::set_workers` uses `ecs_set_threads`, so a world's *n-1* workers are dispatched to the performance pool **once** and run flecs' worker loop until the world gives them back. flecs keeps its own per-tick signalling, the engine keeps one pinned thread per CPU, and the cost moves from every tick to once per world: a world holds `n-1` pool workers for its lifetime, inside a budget the application sets (`JobOsApiConfig::hosted_worker_budget`, default "all but one").

**Machine state.** Same box, same build (`msvc-release`), flecs 4.1.6, nothing else changed. Each hosting was measured **in its own process** (`--filter=*four_systems`, `--filter=*.tasks`, `--filter=*.os_threads` — the filter is a substring match unless it contains `*`, in which case it must match the whole name), twice, each run preceded by `--wait-quiet`. This was a working afternoon on the shared box with other agents compiling, and no run of the six was quiet at both ends by the loosest reading, so here is each one rather than a claim. The GPU sat at 4% of 3.4 GB of 32 GB throughout and the session was unlocked in all six.

| Run | Start (others' CPU) | End | Verdict |
|---|---|---|---|
| shipped A | 5.4% | 24.0% | WARNING; **upper bound** |
| shipped B | 9.4% | 10.2% | WARNING, marginally over the 10% default |
| per-tick A | 5.8%¹ | — | no WARNING at either end |
| per-tick B | under 10% | — | no WARNING at either end |
| os_threads A | 4.5% | 9.2% | WARNING **against a threshold I tightened**: this run used `--quiet-cpu=6`. Under the project's 10% default it was quiet at both ends |
| os_threads B | 7.8% | 13.7% | WARNING; **upper bound** |

¹ The per-tick pair are two separate `--wait-quiet=900` runs at the default threshold, both of which the harness passed at both ends. Runs "shipped A" and "os_threads A" were taken with `--quiet-cpu=6`, which is stricter than the documented default, and it is said here because the bench page asks for that whenever a threshold moves.

Four of the six numbers are therefore upper bounds. That cuts one way for the conclusion and against the other: the shipped hosting's figures can only get *better* on a quieter box, and the per-tick hosting's — the pair with no warning at all — cannot.

Microseconds per tick, 100,000 entities, four systems, median of seven repeats, two runs per cell. Workers count the calling thread, so *n* means one main thread and *n-1* workers; for the shipped hosting the pool was sized to *n*, so the world holds *n-1* of it and **one pool worker is left for everything else**.

| Workers | Shipped: long-running pool jobs | Per-tick jobs (what ADR-0030 replaced) | flecs' own OS threads |
|---|---|---|---|
| 1 | **2,424 / 2,428** | *same code path* | *same code path* |
| 2 | **1,719 / 1,678** (1.42×) | 3,506 / 3,597 (0.68×) | 1,664 / 1,732 (1.43×) |
| 4 | **1,249 / 1,287** (1.91×) | 2,791 / 2,849 (0.86×) | 1,333 / 1,384 (1.78×) |
| 8 | **1,143 / 1,072** (2.19×) | 2,689 / 2,827 (0.88×) | 1,199 / 1,230 (1.99×) |
| 16 | **1,020 / 1,002** (2.40×) | 3,501 / 3,403 (0.70×) | 1,124 / 1,087 (2.19×) |

Speedups are against the shared 2,426 µs single-threaded baseline, on the mean of the pair. The two runs of each cell agree to within 7%, and the *ratios* between the three columns — which is what the conclusion turns on — were the same in every run of the session, warned or not.

**What it says.** The tick scales, and it keeps scaling: 1.9× at four workers, 2.4× at sixteen, where the per-tick hosting was *slower than one worker at every count* and turned back up after eight. The shipped hosting lands within a few percent of flecs' own OS threads at every worker count, which was the target; it reads slightly ahead at 4, 8 and 16, and **that should not be read as the engine's hosting beating flecs' own.** The difference is inside the unpinned configuration's run-to-run spread: a third `os_threads` run in the same session, at the default threshold and with no warning at either end, read 1,696 / 1,788 / 1,102 / 1,314 — its four-worker figure visibly worse than its own two-worker one, which is not a thing a real configuration does. Unpinned threads land wherever Windows puts them and are the configuration most sensitive to what else is on the box; the engine's are pinned, and across every run in this session their spread was the smaller of the two. One session on one shared machine is not enough to claim more than that.

**A corroboration nobody asked for.** One `tasks` run started at 5.8% others' CPU and ended at 20.2%, and it came back **faster** — 2,561 / 2,295 / 2,479 / 2,579 µs against the quiet pair's 3,506 / 2,791 / 2,689 / 3,501. A busy machine making a benchmark faster is the wrong shape for contention and the right shape for mechanism two: other processes keep the cores out of deep idle, so the per-tick wake that the hosting pays is cheaper. It is a stray observation from one run, not a measurement, and it is here because it points the same way as the control that was designed.

**Determinism.** Unchanged and now pinned harder: a small world run sixteen ticks at 1, 4 and 8 workers produces bit-identical components (`domain/ecs/tests/ecs_tests.cpp`), and the conflicting-write-set test still pins system order at 1, 2 and 4 workers.

### What it decides

1. **flecs' workers are long-running jobs on `core/jobs`' performance pool**, and how many of that pool a world may hold is the application's number, refused rather than attempted when it is too large. [ADR-0030](../adr/0030-flecs-workers-are-long-running-pool-jobs.md).
2. **`core/jobs` was not the problem and was not changed.** Its submit-and-join path is microseconds and its spin-then-sleep policy is right for the short jobs it exists for. What was wrong was re-dispatching a long-running worker as if it were one.
3. **The number to plan against is still the single-threaded one** — 2.42 ms for four cheap systems over 100,000 entities — because a real frame shares this pool with rendering and streaming, and a world that holds `workers - 1` of it makes that sharing sharper. The parallel figures say the headroom exists, not that it is free.
4. **[ADR-0028](../adr/0028-ecs-and-persistent-store.md) decision 7 is unaffected.** Whether flecs' pipeline stays the tick scheduler is still open, and the two limits it rests on are untouched: flecs splits a system's matched entities evenly rather than letting the pool steal into the tick, and hierarchy propagation cannot be `multi_threaded()` at all. What has changed is that flecs' pipeline now costs what flecs' pipeline costs, which is the baseline any replacement should be measured against.

# E38: 10^5 scheduled residents — what they cost against 05 §5.6's table

- **Question ([10 §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing), the Phase 3 exit):** what a tile-streamed world of 10^5 NPCs whose routines run through the timing wheel costs per tick, per event, per resident in memory, per tile, per fast-forward and per save — against [05 §5.6](../plan/05-simulation.md#56-what-npc-scale-is-realistic)'s per-tier budget — and which cost keeps it from its budget.
- **Date:** 2026-09-25. **Machine:** a cloud container, Intel Xeon @ 2.10 GHz (4 vCPU, x86-64-v3), 15 GB, Ubuntu 24.04; no GPU. **Build:** `linux-gcc-release` (GCC 13, RelWithDebInfo, x86-64-v3), configured with `ENGINE_SHADERS=OFF` and `ENGINE_WITH_STORE=OFF` because this environment could reach neither the Slang release nor sqlite.org ("Caveats").
- **Machine state:** quiet. Every row below was taken with `--require-quiet`; the harness recorded 0–2% of the CPU used by other processes at the start and the end of each run, and nothing else was running (checked with `ps`). One row is older and says so: the document load before its fix.
- **Decision:** [ADR-0045](../adr/0045-routines-are-data-with-a-closed-form.md) (proposed); the Phase 3 exit note in [10 §10.2](../plan/10-roadmap-risks.md); [npc](../subsystems/npc.md).

## Setup

The npc capability ([npc](../subsystems/npc.md)) through the engine's own pieces: `sim::SimScheduler` at 60 Hz, `ecs::ScheduledTick` over a flecs world, `ecs::RecordMaterializer` first in the hooks table and `npc::NpcSystem` after it, `sim::Materializer` over a document the generator made. The document is `bench_world(n)` in `systems/npc/bench/npc_bench.cpp`: n residents in the default routine mix, their places at the generator's default proportions (for 10^5: 25,000 homes, 2,500 workplaces, 2,000 service places, 1,000 leisure places — 130,500 records), on a kilometre square of 32 m tiles (1,024 occupied), the scheduler's clock at 07:00 on day 0.

```
tools/dev.ps1 bench -Preset linux-gcc-release -Filter 'npc.*' -- --repeats=3 --require-quiet
engine_npc_bench --filter='npc.tick.observed/100000' --repeats=3 --require-quiet --set=npc.lod.mid_m=90,npc.lod.max_promotions=0
engine_npc_bench --filter='npc.tick.observed/100000' --repeats=3 --require-quiet --set=npc.lod.mid_m=90,npc.lod.max_promotions=0,npc.lod.every_ticks=1
```

The memory rows, the tier counts, the week's fast-forward and the flush after it were taken once with a measurement added to the bench for the purpose and not kept: glibc's `mallinfo2()` in-use bytes before and after each step (the process's resident set was useless here, since the allocator reused what an earlier step freed), and the capability's own counters. The rows the bench keeps (`npc.writeback_scan`, `npc.tile_pass`, `npc.document_load`) re-take the rest.

## Results

### Tick time

One fixed step of the world, median of three repeats, everything the scheduler runs (the wheel, flecs' phases, the write-back system's cadence check, `npc.lod` and `npc.motion`):

| Residents held | Tiers | Tick | 05 §5.6 |
|---|---|---|---|
| 10^3 | all LOD2 | **1.23 µs** | |
| 10^4 | all LOD2 | **4.54 µs** | |
| 10^5 | all LOD2 | **39.7 µs** | LOD2: ~1 µs an event, a few events an hour |
| 10^5 | one observer, default radii (16 m, 64 m) | **315 µs** | |
| 10^5 | one observer, `mid_m` 90: **92 at LOD0, 2,277 at LOD1**, 97,631 at LOD2 | **340 µs** | LOD1: 5–20 µs a tick each, 500–2,000 |
| 10^5 | the same, the tier pass every tick instead of every sixth | **1.57 ms** | |

So the tier pass over 10^5 residents is about **1.23 ms a pass** (1.57 − 0.34), single-threaded — `TierAssignment` with no job system, plus the positions it scores — which every sixth tick makes 0.2 ms a tick. LOD0–1 is cheap because it does almost nothing yet: 156 trips moved a tick at `mid_m` 90, a lerp and a transform write each; the 5–20 µs 05 §5.6 budgets is for coarse navigation, which is Phase 6.

**Transitions come on the minute.** A routine is in whole minutes, so every transition of a game minute falls on one tick: the LOD2 rows above are ticks between minutes, and a minute's tick adds its ~640 transitions (below) at 0.33–0.76 µs each, **0.2–0.5 ms once a game minute**. A game that wants the burst spread can draw seconds as well as minutes; nothing else changes.

### Events per game hour, and the wheel's cost per event

| | Value |
|---|---|
| Transitions in a game week, 10^5 residents | **6,440,422** — 9.2 a resident a day, **0.38 a resident a game hour**, ~640 a game minute |
| One transition, 10^4 residents (`npc.transition`: a game day off the wheel, per event) | **0.33 µs** |
| One transition, 10^5 residents (the week executed, per event) | **0.76 µs** |

A transition is the wheel's delivery, one closed form, three component writes through flecs and the next timer. 05 §5.6's "~1 µs per event" holds; the difference between 10^4 and 10^5 is cache: at 10^5 the resident arrays, the wheel's slab and flecs' entity index are past L2, and each event is a handful of dependent misses among them. 05 §5.6's "a few events per hour" was for all of an NPC's events; a routine alone is fewer.

### Memory per resident

| Where | Bytes a resident | |
|---|---|---|
| The capability's arrays (`bytes_per_resident()`) | **177** at size; **263** at the arrays' capacity after growth | id, handle, variation, clock, places, positions, point, timer, tier |
| The wheel | **56** | one slot |
| The runtime world, everything (flecs entity and components, identity map, write-back shadow, the wheel, the capability) | **753** a record, averaged over 100,000 residents and 30,500 places (93.7 MiB) | materialized whole |
| The document in memory — what a resident at LOD3 costs this engine | **1,517** a record (188.9 MiB for 130,500) | the composed layer, JSON properties, the document's index |
| The document on disk | **544** a record (67.7 MiB) | canonical JSON in 1,024 tile files |
| A projection in the store (a tile's record when it went) | not measured here ("Caveats") | `engine.world.TileProjection`'s canonical JSON is about 130 bytes |

**05 §5.6's ~200 B at LOD3 is not met, and the reason is not the capability.** A resident at LOD3 is its document record, and the engine keeps the whole authoring document in memory, composed: a record is a `FlatMap<std::string, JsonValue>` of properties (four place ids as 32-character hex strings, a position array, enum names), its id in the layer's map and in the document's composed index — **1.5 KB, 7.5× the budget**. The store's projection, which is what 05 §5.6 meant by "a record in the store", is near the budget; the document is not a store. A held resident is **about 750 B**, most of it flecs (an entity in a table with `Transform`, `NpcRoutine` and `NpcState`, the identity map, the write-back shadow of its three watched fields) rather than the capability's 177. What would close it: record types with a population keep their records in a typed column store rather than as JSON in the document (the document keeps authored exceptions), or the document pages its layers by tile so a tile no ring holds is not in memory at all. Neither is small, and both are the data model's (03 §3.2), so both are named here, not built.

### Tile activation and deactivation with residents in the tile

| | 10^4 residents | 10^5 residents |
|---|---|---|
| One tile's pass, materializing its ~150 residents and places (`npc.tile_pass`) | **2.4 ms** | **36.7 ms** |
| The same tile let go (dematerialize) | | **0.19 ms** |

**This is the cost that keeps 10^5 from its budget.** A tile pass classifies every live record of the document to find the tile's (sim.md, "A tile pass is a pass over the document"), so it grows with the document — 15× for 10× the records — and not with the tile, whose 150-odd records cost well under a millisecond to materialize. Under the ring's default budget of eight activations an update, a streamed world of 10^5 residents pays **0.3 s in one update** whenever eight tiles come in. The fix sim.md and world.md already name is to ask the document's layer index for a tile's records instead of classifying the document; with it a tile pass would cost its ~150 records, about 0.5 ms. Named, not built: it is `domain/sim`'s driver. (Built the same day: [the follow-up](#follow-up-2026-09-25-a-tile-pass-costs-the-tile-and-the-schedule-index), 0.58 ms a tile at 10^5 on the i9.)

Reconciliation from the store (and writing a tile's projections when it goes) was not measured here: this environment builds no store ("Caveats").

### Fast-forward of a game week, 10^5 residents

| Path | Time | What it did |
|---|---|---|
| Executed (budget 2^40) | **4.89 s** | 6,440,422 transitions delivered |
| Summarized (budget 1,000) | **33 ms** through the wheel's budgeted advance; **15.8 ms** for the summary alone (`npc.summarize`) | 100,000 residents set to the closed form at the week's end, one timer each, **nothing delivered** |
| The write-back flush after the summary | **74 ms** | 300,000 fields in one transaction |

Both end in the same bytes (the test holds them to it at 100 residents). A summary costs 158 ns a resident, whatever the gap: 150× less than executing a week, 1,000× less than a month.

### Save size and load time at 10^5

| | Value |
|---|---|
| The document on disk (the save's `document/`, before any write-back) | **67.7 MiB**, 544 B a record |
| Generating and writing it | **4.3 s** (the generator alone 1.28 s) |
| Loading it (`npc.document_load/100000`) | **3.84 s**, 34,000 records a second |
| Loading it before this change | **44.8 s** (one run, not under `--require-quiet`; nothing else was running) |

**A partitioned layer loaded in quadratic time.** Each tile's records were set into the layer one at a time, in tile order, into a sorted map, so every record landed in the middle and moved everything after it: 45 seconds for 130,500 records, which would have made every `session.open`, `session.load_game` and the replay test's three hosts pay it. The load now gathers every tile's records, sorts them by id once and sets them in order — appends — and finds a record in two files as two equal ids side by side (`domain/doc/src/document_store.cpp`; a new case in `partition_tests.cpp` holds the refusal). 12× at 10^5, and the rest of the 3.8 s is parsing 68 MB of JSON.

The rest of a save — `world.db` (the events the write-back logged, the projections and snapshots of the tiles that went) and the journal of write-back transactions — was not measured here. The replay test prints the save's total size when it runs on the owner's machine.

### The write-back scan

| | 10^4 residents | 10^5 residents |
|---|---|---|
| A flush with nothing changed (`npc.writeback_scan`) | **0.59 ms** | **6.33 ms** |
| A flush after one tick | | 5.8 ms, 0 fields |
| A flush after a whole summary | | 74 ms, 300,000 fields |

**Linear in watched entities, and almost all of it finds nothing.** The write-back compares every watched entity's writable bytes against their shadow (ecs.md's cost note): 21 bytes a resident, 10^5 residents, every flush. Residents change ~11 a game second, so at the default cadence (every 60 ticks) the scan is 0.1 ms a tick amortized and harmless; at a cadence of one it would be **38% of a 16.7 ms tick** spent confirming that 99.98% of residents did not change. It is not the bottleneck at the default cadence; the tile pass is. The change the ECS note names, proposed and not implemented: the write-back asks flecs which tables had a watched component written since its last collect (flecs' per-table change detection, `ecs_query_changed` on a query over the watched components, or the table's dirty state) and scans only those — or, for a component one capability writes, the capability hands the write-back the entities it wrote (a dirty list as a registration point on `MaterializeTarget`), which for residents is the ~11 a second that transitioned.

## What surprised me

- **The document, not the simulation, is the memory.** A resident at LOD3 costs 1.5 KB of composed JSON in memory; the simulation of a held one costs half that.
- **A layer load was quadratic,** and nothing had loaded a layer of 10^5 records before: the replay world has 54.
- **The tier pass thrashed in the first version,** from two bugs of mine that the test now catches: the change list was never cleared (every pass re-applied every change ever made, 29 ms a tick and growing), and residents were scored on where they were *drawn*, which depends on their tier — a trip is drawn at its destination at LOD2 and along its segment nearer — so a traveller near a band's edge was promoted, drawn farther, demoted, drawn nearer and promoted again every pass. They are scored on where they are now, which no tier changes.

## What it decides

- Routines as data with a closed form meet 05 §5.6's LOD2 line (0.33–0.76 µs an event) and make LOD3's summary 158 ns a resident whatever the gap ([ADR-0045](../adr/0045-routines-are-data-with-a-closed-form.md)).
- 10^5 residents held at LOD2 cost 40 µs a tick; with ~2,400 near an observer, 0.34 ms. The tick is not where 10^5 is expensive.
- **The streaming cost is the tile pass over the document** (37 ms a tile at 10^5), and **the memory cost is the document** (1.5 KB a record). Both are follow-ups outside this capability, named above.

It does not decide the store's share of a save or of a tile's activation (not measured here), nor anything about LOD0, which is LOD1 until Phase 6.

## Caveats

- **One machine, a 4-vCPU cloud container, no GPU, and a reduced configuration.** This environment's network policy refused the Slang release download and sqlite.org, so the build ran with `ENGINE_SHADERS=OFF` and `ENGINE_WITH_STORE=OFF` — no renderer, no store, no world capability (which requires the store), no engine-host. Everything above is the capability, the scheduler, flecs and the document; the store's rows and the end-to-end replay (`apps/engine_cli/tests/npc_replay_tests.cpp`) are for the owner's machine, and the replay's hash is pinned there.
- The i9-10980XE the other experiments ran on is faster per core; these rows are not comparable to theirs without a re-take.
- The tier pass ran with no job system; `TierAssignment` scales 3.6× on eight workers (sim.md), which the capability does not use yet.

## Follow-up (2026-09-25): a tile pass costs the tile, and the schedule index

The first of the two costs "What it decides" names — the streaming one — was the driver's, and it is gone: the document keeps a tile index ([doc](../subsystems/doc.md), "Tile index") and a tile pass takes its records from it ([sim](../subsystems/sim.md#the-contract-as-implemented)).

- **Machine:** the owner's i9-10980XE (18 cores, 36 threads), Windows 11, `msvc-release` (MSVC, RelWithDebInfo, x86-64-v3) — not the Xeon container above, whose rows these are not comparable to. The **before** rows were re-taken here from the code before the change (the same bench rows, built from that commit's `domain/doc` with the new rows added), so before and after are one machine.
- **Machine state:** every run under `--wait-quiet=900`, which found the machine quiet at once each time; the harness recorded 9.7% → 9.4% of the CPU used by others (start → end) for the tile rows before, 9.4% → 9.5% after, and for the document's apply rows 5.7% → 15.2% before and 8.0% → 14.5% after (a build elsewhere starting near the end of both, so those two are upper bounds); the GPU at 4–6%, no GPU lock held, the session unlocked.

```
engine_npc_bench --filter='npc.tile_*' --repeats=3 --wait-quiet=900
engine_doc_bench --filter='doc.apply.*' --repeats=3 --wait-quiet=900
```

### A tile's activation and let-go

Median of three repeats. `npc.tile_pass` and `npc.tile_leave` are E38's fixture — a world holding one tile of `bench_world(n)`, brought in and let go; the `.held` rows are the world a stream actually has, every other record already held (materialized whole first, so each is filed under its tile), where a pass or a let-go that walks everything held to find its tile's records pays for it.

| Row | 10^4 before | 10^4 after | 10^5 before | 10^5 after |
|---|---|---|---|---|
| `npc.tile_pass` (a tile of ~150 records in) | 3.98 ms | **0.49 ms** | 60.2 ms | **0.58 ms** |
| `npc.tile_pass.held` (the same, every other tile held) | 5.26 ms | **0.52 ms** | 77.7 ms | **0.73 ms** |
| `npc.tile_leave` (the tile let go) | 0.148 ms | 0.148 ms | 0.180 ms | 0.167 ms |
| `npc.tile_leave.held` (the same, every other tile held) | 0.262 ms | **0.179 ms** | 1.06 ms | **0.197 ms** |

**A tile pass is now its tile.** 0.49 ms at 10^4 and 0.58 ms at 10^5, for one tile of ~150 records, where it was 3.98 and 60.2 ms: 8× and 104×. What is left grows by a fifth for ten times the document, which is the tile's records costing more to reach in larger hash maps (the document's index, the driver's, flecs'), not a walk of anything. The `.held` rows had a second walk the first rows never showed: finding what the driver held under the tile meant visiting everything it held, which was most of a millisecond of the let-go at 10^5 (1.06 ms) and grew with the world. The driver now files what it holds per tile, so a let-go is 0.18–0.20 ms at both sizes — the flecs deletions and the hooks — and a pass in a full world is 0.73 ms.

**Under the ring's default budget** — eight activations an update — a streamed 10^5 now pays about **5 ms** in one update where it paid 0.3 s on the Xeon and would have paid 0.6 s here. That is no longer the cost that keeps 10^5 from its budget: 5 ms is a third of a 60 Hz tick, and it lands only on updates where eight tiles come in at once.

### What the index costs the document

| Row (per command) | 10^3 before | 10^3 after | 10^5 before | 10^5 after |
|---|---|---|---|---|
| `doc.apply.set_property` (an unpartitioned property) | 0.308 µs | 0.313 µs | 0.695 µs | 0.706 µs |
| `doc.apply.move` (a record of a partitioned layer to another of 16 tiles) | 0.692 µs | 0.782 µs | 1.26 µs | 1.39 µs |

A command that moves nothing between tiles costs what it did (within the noise); one that moves a record between tiles pays for its tile being read again and for a sorted insert into the new tile's list, 0.09–0.13 µs — what a resident's write-back of its anchor costs more, at 640 transitions a game minute 0.08 ms a game minute at 10^5. In memory, an index entry is 56 bytes instead of 48 and every live object is in one sorted list (a tile's or the untiled one) at 16 bytes: about 3 MB at 130,500 records, against the 189 MB the document already is.

### What moved the replay hash

Nothing: the tile index changes which records a tile pass looks at to find its own, not which records it materializes, and `npc_replay_tests.cpp` reaches `53a8c6295e6d4e78` on MSVC as before.

### The schedule index

The boundary npc.md stated — a resident whose record is in a tile nobody simulates is not materialized even when its routine has it in a live one — is closed by the npc capability's schedule index ([npc](../subsystems/npc.md#the-schedule-index)), consulted through a tile source the driver asks on every activation, and by a watch timer per resident that is not held but whose routine visits a live tile. Same machine and build as above; the harness recorded 5.6% → 8.7% of the CPU used by others for the tile rows, 6.1% → 2.8% for the watch rows, 2.9% → 8.2% for the build and 1.0% → 3.6% for the place index; the GPU at 0–1%, no lock held.

```
engine_npc_bench --filter='npc.tile_*' --repeats=3 --wait-quiet=900
engine_npc_bench --filter='npc.watch*' --repeats=3 --wait-quiet=900
engine_npc_bench --filter='npc.schedule*' --repeats=3 --wait-quiet=900
engine_npc_bench --filter='npc.places*' --repeats=3 --wait-quiet=900
```

| Row | 10^4 | 10^5 | What |
|---|---|---|---|
| `npc.tile_pass` (as above, re-taken with the source code in the driver and none registered) | 0.478 ms | 0.580 ms | a tile's activation without the index |
| `npc.tile_pass.scheduled` | 0.671 ms | 0.922 ms | the same with the capability's tile source registered, as engine-host has it |
| **The index's cost per activation** | **0.19 ms** | **0.34 ms** | 250 and 401 residents' routines visit the bench tile (137 and 154 records): a closed form each, **0.8 µs a resident** |
| `npc.watch/0`, `/1` (10^4, one tile live, a game day executed) | 1.02 ms, 2.52 ms | | the day without and with the source: 964 and 2,308 events delivered |
| **One watch firing** | **1.1 µs** | | (2.52 − 1.02 ms) over the 1,344 watches the second day fired |
| `npc.schedule.build` | 17.5 ms | 250.6 ms | the index built whole: the place index, then every resident's routine, clock and places' tiles |
| `npc.places.build` | 1.9 ms | 35.9 ms | the place index alone, which the row above includes |
| The index's memory (`bytes_scheduled()`, at 10^5) | | 15.5 MB | **155 bytes a resident of the document**, held or not |

What the rows say:

1. **An activation costs the residents whose routines visit the tile**, 0.8 µs each — the variation drawn again, the closed form, a hash lookup for where the resident is filed. At 10^5 a tile's pass is 0.92 ms with the index against 0.58 without; eight activations an update are 7.4 ms. Keeping the 60-byte variation in the index would take the draw out (a transition off the wheel, which has it, is 0.33 µs) for 6 MB at 10^5; at eight activations an update it would save, by the transition's price, under 2 ms in that update — an estimate, not a row — which is not yet worth a document-sized array.
2. **A watch is a transition that finds nothing to do**, three times the price of a held resident's (1.1 µs against 0.33 µs) for the same reason, and at the rate of one: a watched resident's watch fires at its transitions, 9.2 a day. In the replay world below, 140 residents were let go into tiles nobody simulates in the second half and are watched from then on; at 10^5 with a third of the residents watched, watches cost a few microseconds a game second.
3. **The build is once per document**, and whenever a place moves; engine-host pays it at the first call on a session (and at a load). 250 ms at 10^5 on the i9, about a fifteenth of the document's load time on the Xeon. After it, the index follows the document's change feed at the start of each call: a write-back changes none of what it keeps, so a changed resident costs a re-read and a comparison, about 2 µs.
4. **Memory is the index's real cost**: 155 bytes for every resident of the document, held or not — a tenth of what the document already spends on the same resident's record (1.5 KB), and the same order as 05 §5.6's whole LOD3 budget. It is what "the residents whose routines visit a tile" costs to know without reading the document.

### What moved the replay hash, again

Still nothing. In `npc_replay_tests.cpp`'s world every place is inside the simulated rings, so no resident's routine crosses their edge and the index changes nothing — `53a8c6295e6d4e78`, on MSVC and on Clang 18 in the Linux container. The case that exercises it end to end is new in the same file: 2 × 10^4 residents on a 1,280 m square whose corners the rings never reach, the seven o'clock transitions after the save; three runs — continuous, loaded, replayed — reach one hash, **`16a407bfad492334`** (MSVC and Clang 18), and bring in the same 8 residents from tiles nobody simulates after the save, while 140 are let go into them.

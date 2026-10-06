# npc (systems, capability)

**Purpose.** Scheduled residents: 13 §13.2's persistent population ("home: building 184, floor 17, apartment 1702; job: hospital 2, radiology; state: working; next event: shift end"), at the scale [05 §5.6](../plan/05-simulation.md#56-what-npc-scale-is-realistic) budgets. An `engine.npc.Resident` is a document record whose daily routine runs through the engine's timing wheel at the tier the observer set gives it; an `engine.npc.Place` is where it goes. The capability owns the routine model — tables of rows, data — and its closed form, the resident's transitions on the wheel, the `Summarizer` that brings any number of residents across any gap without visiting it, the promotion and demotion of residents by the observer set, the place index, the schedule index that brings a resident in where its routine has it, and the generator that writes a seeded layer of places and residents. It does **not** move a resident's body, find a path, perceive, animate or collide: at every tier a resident stands at a place or is on the straight segment between two, and locomotion, navigation and bodies are Phase 6. [ADR-0045](../adr/0045-routines-are-data-with-a-closed-form.md) (proposed) records the decisions and the alternatives; [E38](../experiments/e38-scheduled-npcs.md) is the measurement.

## Why this shape

- **A routine is data with a closed form, and everything calls the closed form.** Where a resident is at game time t is a function of its routine table, a variation drawn once, and t — at most twelve rows walked, whatever t is. The materialization hook calls it at the scheduler's time, a timer's delivery at the timer's own time, the summarizer at the end of the gap, the generator at the layer's time. So executing a week, summarizing it, and materializing a resident at the end of it from a document written at the start all give the same bytes, and the tests hold all three to each other. The two alternatives — a per-tick state machine at every tier, and the schedule expanded into events ahead of time — are in the ADR with why neither can meet sim.md's summarizer contract at 10^5.
- **One timer per scheduled resident.** 05 §5.6's LOD2 is "scheduled transitions only": the wheel holds each materialized resident's next transition and nothing ticks in between. A resident at LOD2 costs the wheel's bitmask scan and nothing else until its row ends.
- **A tier changes what runs, not what is true.** What is written back — `state`, `next_event` and the anchor `position` — is the closed form's at the transition, whatever the tier. The transform LOD0–1 moves along a trip is never written back. A resident observed and the same resident unobserved hold the same state to the byte (tested).
- **LOD3 is not an entity.** A resident in a tile the ring does not simulate is its record, in the document and — when its tile went — in the store's projections: 05 §5.6's "a record in the store plus occasional summary". Its summary is the closed form, applied when its tile comes back.
- **Tiers are this capability's pass.** Nothing drove `promote`/`demote` for document records (sim.md, world.md); the ring maps rings to representations, not to tiers, and the scheduler may not be edited (ADR-0027). So a table system at `Lod` runs `TierAssignment` over the held residents against the host's observers and applies the result through `SimScheduler::apply_tier_changes`, which every capability's hooks see.

## Owned data

The resident table: per held resident its id, entity handle, drawn variation, clock offset, four resolved places, the document's position as a fallback, its current routine point, its timer, where it is drawn, its importance and its tier — dense arrays, swap-removed, with an index from entity handle to slot (a resident's slot is not stable; the wheel's payload names the entity handle). The place index (every `Place` of the document by id, its position and role). The schedule index (every resident of the document, held or not: its routine, clock offset and places' tiles, per tile the residents whose routines visit it, the watch timers of those not held, the live tiles the driver reported, and the arrivals not yet taken). The two table systems, the summarizer registration and the wheel's event sink. Nothing else may hold or change a resident's routine state; its components are written by this capability alone.

## The records

`systems/npc/schemas/npc.schema`:

```
struct Place @version(1) @kind(record)     { name, position @unit(m), orientation, role: PlaceRole,
                                              kind: u32, capacity: u32 }
struct Resident @version(1) @kind(record)  { name, position @unit(m), home, job, service, leisure: id128,
                                              routine: Routine, state: ResidentState,
                                              next_event: i64 @unit(us), clock_offset: i64 @unit(us),
                                              appearance: f32[8] }

materialize Place {
  Transform.position = position
  Transform.orientation = orientation
  NpcPlace.role = role  NpcPlace.kind = kind  NpcPlace.capacity = capacity
  parent = ChildOf
}

materialize Resident @tiers(0, 1, 2) {
  Transform.position = position                    // where it is drawn; never written back
  NpcRoutine.home = home  NpcRoutine.job = job  NpcRoutine.service = service
  NpcRoutine.leisure = leisure  NpcRoutine.routine = routine  NpcRoutine.clock_offset = clock_offset
  NpcState.state = state @writeback
  NpcState.next_event = next_event @writeback
  NpcState.anchor = position @writeback            // the place of its row: the record's tile
}
```

- `position` fills two rows: the anchor, which is written back and so moves the record between tiles, and the transform, which a tier may move along a trip. Only the anchor row writes back (a property has one writer).
- `@tiers(0, 1, 2)`: LOD3 is not an entity.
- A place does not move and nothing may move it, so its rows are not written back and its entity is not watched.
- `appearance` is [ADR-0032](../adr/0032-characters-are-parameter-vectors.md)'s parameter vector, reserved beside home and job as a fixed array of eight and read by nothing. The generator leaves it at its default, so it costs a document nothing until something writes it.
- `clock_offset` is the routine's time of day at game time 0 ([ADR-0045](../adr/0045-routines-are-data-with-a-closed-form.md), "The clock"): a headless run starts at game time 0 and a world has no settings record yet, so a resident says what time of its day that is.

## The routine model

`routine.h`, ECS-free. A **routine table** is one archetype's two kinds of day — a work day and a day off — each a list of **rows** (state, place role, start window, duration window), in minutes:

| Archetype | Work day, in outline | Days off | Shift |
|---|---|---|---|
| `DayWorker` | up 06:30–07:30, commute, work from 09:00–09:15, lunch at work, work, shops, home, bed 22:00–23:00 | 2 | ±30 min |
| `NightWorker` | up 13:00–14:00, shops, home, commute, work from 22:00 into the next morning, home to sleep | 2 | ±30 |
| `Shopkeeper` | up 06:00–07:00, to the shop (its job is a service place), open 08:00 sharp, lunch, work, home, bed | 1 | ±15 |
| `Student` | up 07:00–07:30, school from 08:20–08:30, lunch, school, leisure, home, bed | 2 | ±20 |
| `Retiree` | up 06:00–08:00, shops, a meal out, leisure, home, bed 21:00–22:30 | 0 | ±60 |
| `Idle` | up 09:00–12:00, leisure, home, bed late | 0 | ±60 |

The day off every working archetype shares is a late morning, leisure, a meal out and shopping; the night worker has its own. A row with a start window starts at the later of the time drawn from it and the end of the row before it (so a shift starts on time however early the commute arrived); a row without one starts when the one before it ends; the last row runs to the next day's first. A `Travelling` row's place is where the trip goes; it starts wherever the row before it was. **A table is valid only if no day can run into the next** — the latest the last row can start, every window and duration at its longest and the shift at its widest, is before the earliest the next day's first row can — and `valid_table` says why one is not; the shipped tables are held to it by a test.

**The variation** is drawn once per resident from `hash(world seed, record id)`: a shift that moves the whole day, a draw inside every row's windows, and which consecutive days of the week are off. It is 60 bytes of integers (`Variation`: two `DayPlan`s of twelve minute offsets, the days-off mask and the routine), the same on every compiler — a test pins its hash, and GCC reproduces Clang's number.

**The closed form**, `routine_at(variation, t)`: the day t falls in is `floor(t / day)`, or the day before when t is earlier than that day's first row; the row is the last whose start is at or before t, a walk of at most twelve. The answer (`RoutinePoint`) is the row, its state and place, the place of the row before it, when it started and when it ends — a row is `[start, end)`, so at exactly `end` the resident is in the next row. `routine_by_stepping` gets the same answer by walking every row from a day before; the test holds the two equal to the byte at 67,000 points and at every transition of two weeks for every archetype, a decade on, and before time 0. `routine_at_offset` is the same on a clock offset from game time.

## The capability at each tier (the LOD policy)

| Tier | What it is | What runs | 05 §5.6's line |
|---|---|---|---|
| LOD0 (within `npc.lod.near_m`, 16 m) | an entity | the timer; a trip drawn on its segment every `npc.motion.every_ticks` | "animation + physics + perception + detailed nav" — none of which exists yet, so LOD0 is LOD1 |
| LOD1 (within `npc.lod.mid_m`, 64 m) | an entity | the same | "coarse nav + routine state machine" |
| LOD2 (a simulated tile, beyond `mid_m`) | an entity | the timer alone; a trip is drawn at its destination | "scheduled transitions only" |
| LOD3 (a tile the ring does not simulate) | a record | nothing; the closed form when its tile comes back | "a record in the store plus occasional summary" |

Distances are divided by importance and observer weight as for any tier ([05 §5.4](../plan/05-simulation.md#54-lod-tier-assignment)); the pass runs every `npc.lod.every_ticks` (6) with a 15% hysteresis band and rate limits of `npc.lod.max_promotions` (64) and `max_demotions` (256), nearest promoted first. A resident materialized while the host has observers starts at LOD2 and is promoted by the pass ([05 §5.5](../plan/05-simulation.md#55-reconciliation-when-a-tile-activates) step 4); with no observers (a world materialized whole) residents stay at the tier the driver materialized them at, clamped to LOD2.

**Where the tier changes are driven from.** `npc.lod`, a table system at `TickPhase::Lod`, scores every held resident's drawn position against `set_observers()`' set with the capability's own `TierAssignment` and hands the changes to `SimScheduler::apply_tier_changes`, which calls every capability's `promote`/`demote` in the hooks table — this capability's among them, which redraws the trip. engine-host points the set at the ring's observers, refreshed between ticks where the ring runs. Reconciliation's promotion (a tile coming back) lands in the same hook.

## The transitions on the wheel

`NpcSystem::install` registers the components, the two table systems (`npc.lod` at `Lod`, `npc.motion` at `Systems`), the summarizer on the scheduler's wheel, and the capability as the wheel's event sink. **The scheduler has one sink and nothing had set it**; this capability takes it and forwards every event that is not a resident's (payload kind `k_timer_kind`, "NPC1") to the `next` sink its host names, so a host that routes timers of its own keeps them. sim.md's "no event bus" is still true; the day a second capability schedules timers, the sink wants to become a table.

- **Materialization** (the hook, after the entity store's): the resident's entity is found by its id, its `NpcRoutine` read, its variation drawn, its places resolved, and it is put at the closed form at the scheduler's time — `NpcState` and the transform written — with one timer at the end of the row. A record already held is refreshed the same way, which is why materializing twice is the same resident. A record from the store (reconciliation) that is already held is refreshed; one that is not is not this capability's to make from a projection.
- **A transition**: the timer's payload names the entity handle; the resident is put at the closed form at the timer's own time (not the wheel's position, so a fast-forward that delivers several stamps each with its instant), and the next timer is armed. A stale timer — re-armed since — is ignored.
- **Dematerialization**: the timer is cancelled and the slot swap-removed.

## The summarizer

`npc.routine`, registered for LOD2 and LOD3, and `NpcSystem::summarize(from, to)`: every held resident whose current row ends at or before `to` is put at the closed form at `to` and its timer re-armed past `to`. sim.md's five conditions, one by one:

1. **Equivalence, exact.** The closed form at `to` is where executing every transition of the gap gets to, because each transition is the closed form at its own time and the rows tile time. Tested: a game week executed under a large budget (6,000-odd transitions for 100 residents) and summarized under a budget of 16 end in the same bytes, both equal to the closed form.
2. **Determinism from stated inputs only.** The summary reads `to`, the resident's variation (from the world seed and its id) and its clock offset; not how it was materialized, not the wheel's other contents, not the order residents are held in. Tested: a rig materialized tile by tile in reverse with fifty unrelated timers on its wheel summarizes to the same bytes as one materialized whole.
3. **Idempotent over a partition.** Where a resident is at c does not depend on where it was summarized to first: (a, b] then (b, c] equals (a, c], tested at uneven boundaries across nine days.
4. **No events out.** It writes the resident's own state and arms one timer past `to`; nothing of the residents is delivered from inside the gap (tested: zero delivered, and `count_due(to)` zero afterwards). A one-shot timer that is not a resident's is still delivered, as the wheel promises.
5. **Bounded cost.** One visit per held resident whatever the gap — a day and a decade visit the same 80 in the test — and never a skipped transition.

`NpcSystem::fast_forward(to, budget)` is the wheel's budgeted advance with this capability's sink: executed when the gap holds no more than the budget (`npc.fast_forward.budget`, 10^5), summarized when it does. It advances **the wheel**, not the scheduler's clock, which has no jump; a world-level fast-forward is the scheduler's to add.

## Places

`PlaceIndex::refresh(document)` reads every `Place` through the document's composed index (`objects()`, `type_of`, `property`) — never the layers — in id order, and does nothing when the document's revision has not moved. It is read from the document and not from what is materialized because a resident's job may be in a tile nobody simulates, and it still goes there. `NpcSystem::refresh_places` also re-resolves every held resident when the index's content changed (a place moved: the residents at it move with it, tested); a host calls it before it materializes, and engine-host does at the start of every call and before a save's tiles are restored.

## Records that move, and the boundary

A resident's record is filed under its anchor's tile: at home, its home's tile; at work, its job's; on a trip, its destination's. So a transition that sends it to another tile's place is a **record that moves** ([world](world.md#records-that-move)): the write-back tells the document, the driver reports it (`take_moved`), and the world's document consumer settles it — refiled under the new tile when that tile is live in its rings, let go when it is not. A resident let go is at LOD3: its state is in the document, and the closed form brings it up to date when it comes back (tested).

**The boundary, stated** (as it is since 2026-09-25; before, it was the record's tile). **A resident is materialized when the tile its routine has it in is live** — whatever tile its record is in. Its record's tile adds one case, until the write-back that moves the record: a resident whose record is in a live tile but whose routine has it elsewhere is brought in with that tile and let go at its next write-back, exactly as before, because that write-back is what brings its record up to date. So an observer standing in a residential tile at night sees the residents who live there, including the ones whose work tiles went out of simulation before they came home: they come home with their routines. What decides it is the **schedule index** (below): a tile's activation asks it which residents' routines have them in that tile now, and a resident that is not held but whose routine visits a live tile is watched, so that its own timer brings it in when its routine walks it into one. Before the index, the boundary was the record's tile: a resident that walked to work in a tile the ring did not simulate stayed there, as a record, even after its closed form had it back home in a live tile, until its record's tile came in ([ADR-0045](../adr/0045-routines-are-data-with-a-closed-form.md) named the index as the follow-up).

## The schedule index

`NpcSystem`'s **schedule index** is, for every resident of the document — held or not — what its closed form needs (routine, clock offset; the variation is drawn again from the world seed and the id when it is asked) and the tiles of its four places, and per tile the residents whose routines visit it, in id order. **All four places**, leisure included: a routine anchors a resident at its home, job, service and leisure places, and a tile any of them is in is a tile the routine can put it in. A place's tile is the resident's layer's grid applied to the place's position exactly as the write-back will write it into the resident's `position` (`doc::tile_of_position`), so the tile the index says a routine has a resident in is the tile its record will be filed under once it is written back. A resident on no grid, or on a grid that reads another property than `position`, is not indexed: its routine does not move its record between tiles. A role with no place (a retiree's job) has no tile; a place the place index does not know falls back, as a held resident does, to the record's own position, which the document already files.

**How it is kept.** Built whole from the document (`objects()`, `type_of`, `visit_records` — never the layers) the first time, for another document, and whenever the place index's content changed (a place moved: every resident at it moves tile); otherwise brought up to date from the document's change feed, re-reading each changed resident and relinking it only when its routine, clock or places' tiles changed — a write-back of a resident's state changes none of them. `refresh_places` refreshes it, and the hosts call that at the start of every call and before a load's tiles come in; within a call the document changes only by write-backs. A resident created, or given other places, between two calls is indexed at the next call but watched only from the next activation of a tile its routine visits — the same lag an edit between calls to any record in a live tile has ([world](world.md#not-yet)). The index is the capability's and nothing else writes it.

**Where it is consulted** — through the driver's tile source (`tile_source()`, which a host registers with its `sim::Materializer` beside the hooks; [sim](sim.md#records-the-document-has-not-caught-up-with)):

- **On a tile's activation.** The pass asks which residents' routines visit the tile, and puts each at its closed form at the scheduler's time: the ones it has in the tile now are the pass's to bring in (or to file there, when an earlier pass filed them by their record's tile), wherever their records are; the others, when not held, are **watched**. The driver also asks, for every record a pass meets, where the index has it now (`where`), and files it there when that tile is live.
- **On a resident's own timer.** A resident that is not held but whose routine visits a live tile has a timer of its own on the wheel — a watch, payload kind `k_watch_kind` ("NPC2") — at the end of its current row. When it fires, the resident is put at its closed form at the timer's time: if its routine has it in a live tile, it is an **arrival**, and the driver brings it in when the world's document consumer next settles (`materialize_arrivals`, between ticks); either way, while its routine visits a live tile, the next watch is armed. A resident that comes in cancels its watch; one that is let go — its tile went, or its routine took it into one that is not live — is looked at at once and watched again. A watch whose tiles have all gone lapses when it next fires.
- **In a summary.** A watch is a one-shot, which the wheel delivers through a summary rather than coarsening, so the summarizer takes the watches with the residents it holds: each is looked at once, at the end of the gap, and armed past it (tested: a day fast-forwarded under a small budget fires no watch and leaves the same residents watched as the executed day).

**Determinism.** After any sequence of tile activations at one time, a resident is held when its record's tile or the tile its routine has it in is live, filed under the second when that is live and under the first otherwise; after the next write-back and settle, it is held exactly when the tile its routine has it in is live. Neither depends on the order the tiles came in, because a pass that meets a resident an earlier pass filed by its record's tile refiles it where its routine has it (tested on five shuffled orders of the same tiles, with a tile then let go and a write-back after: the same residents held, under the same tiles, in the same state, the same residents watched). Every decision is the integer closed form at the scheduler's or the timer's time, and the order the driver brings residents in is (depth, id). A load holds what the run that never stopped holds, because its tile passes ask the same index at the same time and arm the same watches — each at the end of the resident's current row, which is where the running world's watch is too (tested at the capability, from a document written back after twelve game hours of transitions, watches, arrivals and let-goes, and end to end below).

**What it costs** ([E38's follow-up](../experiments/e38-scheduled-npcs.md#the-schedule-index)): on the i9, **0.19 ms per activation at 10^4 and 0.34 ms at 10^5** on top of the tile pass — the bench tile's 137 and 154 records are visited by 250 and 401 residents' routines, a closed form each, 0.8 µs a resident; **1.1 µs per watch firing**, three times a held resident's transition because the variation is drawn again rather than kept (60 bytes per resident of the document, which a watch firing a few times a game hour does not earn back); **250 ms to build at 10^5** (35 ms of it the place index), once per document and whenever a place moves; and **155 bytes a resident of the document** (the pinned 72-byte entry, the id map and the tiles' lists, at their capacities).

In the Phase 3 replay the simulated rings cover every place, so no resident crosses the boundary and the index changes nothing there (its hash did not move); the crossing replay below is the one that exercises it end to end.

## The generator

`generate_layer(params, jobs)` and `generate_document(vfs, dir, params, jobs)` (`generator.h`), and `engine-content npcs <params.json> --out <document dir> [--jobs <n>]` over them: a partitioned layer of places, then residents, added to the document at the directory as its **edit layer** (so the write-back's `SetProperty` lands in the layer that defines the record and the partition files a moved resident under its new tile), created when there is none. Parameters: `seed`, `residents`, `places` (homes, workplaces, services, leisure; by default a home per four residents, a workplace per 40, a service place per 50, a leisure place per 100), `extent`, `tile_size`, `layer`, `time_us`, `clock_offset_us`, and the routine `mix`. Places land on a quarter-metre grid inside the extent; a resident's home, job, service and leisure places are hashed choices among each kind; its `state`, `next_event` and `position` are the closed form at `time_us`, so a world that starts there writes nothing back until the first transition (tested). Records carry only what differs from their type's defaults and what the partition reads. **The layer does not depend on the thread count**: every record is a pure function of (params, index), the job system only splits the index range, and the layer holds its records by id; the test compares the layer's bytes at none, one and four workers, and `engine-content`'s at one and four, and pins its hash (Clang and GCC agree). Derived data: never committed; tests write it under their `TempDir`.

## Storage per resident

| Where | Bytes | What |
|---|---|---|
| this capability's arrays | 201, plus ~16 in the handle index and a 12-byte scratch the tier pass scores from | id 16, handle 8, variation 60, clock offset 8, four places 16, fallback and drawn position 48 (two `WorldPos`, f64, ADR-0053; 24 as float32 before 2026-10-05), point 32, timer 8, importance 4, tier 1 (`bytes_per_resident()`, pinned) |
| the wheel | 56 | one `TimingWheel::Slot` (and one more for a resident not held that is watched) |
| the schedule index | 155 per resident of the document, held or not | the 72-byte `ScheduledResident` (pinned), the id map, and one 4-byte entry in the list of each tile its places are in, up to four (`bytes_scheduled()` at the containers' capacity, measured at 10^5) |
| the entity's components | 160 | `Transform` 40, `NpcRoutine` 80, `NpcState` 40 (size table; `Transform` and `NpcState` each grew by an f64 position, ADR-0053) |
| flecs, the identity map, the write-back shadow | see [E38](../experiments/e38-scheduled-npcs.md) | measured as the process's resident-set growth |
| LOD3: the document | see E38 | a record's composed JSON in memory, and its bytes on disk |

05 §5.6's ~200 B is a *LOD3* figure; E38 says how far a held resident and a record each are from it, and why.

**Positions are world positions, f64** ([ADR-0053](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md)). `Place.position`, `Resident.position` and `NpcState.anchor` are `worldpos` (records and component at version 2), the place index, the fallback and the drawn position are `WorldPos`, a trip is `lerp` between two of them in f64, and the generator writes a place where its stream put it rather than on float32's grid — so a town 420 km out is drawn and written back where it is. The anchor written back is a place's position exactly as the document holds it. **The tier pass scores them where they are**: `domain/sim`'s `TierInput` and `ObserverSet` hold `WorldPos` since 2026-10-06 ([sim](sim.md#positions-are-f64)), so the pass hands over each resident's f64 position as it is and the narrowing that stood here, `tier_position()`, is gone. The test steps a traveller a second at the origin and with the same world moved to 419 km, 10,000 km and 1e8 m (the generator's quarter-metre lattice moved exactly): the same resident on the same trip, at tier 0, displaced by its 1.2 cm to within two f64 steps at the site (0.09 nm at 419 km, 0.07 µm at 1e8 m) — less than any float32 step there.

## Invariants

Each is a test in `tests/routine_tests.cpp`, `npc_tests.cpp` or `generator_tests.cpp` unless it says otherwise.

- Every shipped table is valid; a table whose day can run into the next is refused, naming why.
- The variation is a pure function of (routine, world seed, id), its rows inside their windows, its days off the table's count; its hash and the closed form's over a grid are pinned and the same on Clang and GCC.
- The closed form equals stepping row by row, at every transition and a decade on; a row ends where the next begins.
- Residents materialize at the closed form at the scheduler's time; with the document written at that time, nothing is written back.
- Through the tick, every resident is at the closed form at the scheduler's time, and the document follows at `Persist`.
- Materialized whole, tile by tile in reverse, twice over, or later from a stale document, a resident is the same bytes.
- A week executed and summarized end the same; the summarizer meets its five conditions as listed above.
- Promotion and demotion follow the observer set and change nothing that is written back.
- A resident that moves tiles is reported, refiled or let go, and comes back at the closed form.
- A place that moves moves its residents; a record that is not a resident is left to the other hooks.
- The schedule index lists, for every occupied tile, exactly the residents one of whose four places is in it, as the test reads the document itself; it follows an edit of a resident's place and a deletion through the change feed, and is built again when a place moves.
- A resident whose routine brings it into a live tile comes in there, wherever its record is — a day worker let go at work in a tile nobody simulates is home again with its routine, and its record follows at the write-back — and in that world every resident is held exactly when the tile its routine has it in is live; the same world without the index leaves the day worker at work.
- What the tiles bring in does not depend on the order they came in (five orders, a tile let go, a write-back: the same residents, filings, states and watches), and after the write-back exactly the residents whose routines are in a live tile are held.
- A world brought in from the document a running world wrote back, at its time, holds the same residents in the same states under the same tiles, with the same residents watched.
- A summary fires no watch and leaves the same residents watched as the executed gap.
- End to end (`apps/engine_cli/tests/npc_replay_tests.cpp`): 10^5 residents streamed, saved at tick 300, loaded, and replayed from tick 0 reach one persistent-state hash — see "Held to the clause" — and so do 2 × 10^4 whose routines cross the edge of the simulated tiles, with the same residents arriving after the save in all three runs.

## Held to the clause

The Phase 3 exit's last clause ([10 §10.2](../plan/10-roadmap-risks.md)) is `npc: 10^5 scheduled residents streamed, saved, loaded and replayed to one state hash`: the generator's 10^5 residents and 30,500 places on a 512 m square of 64 m tiles, their clocks five seconds before 07:00 at game time 0 (so the seven o'clock transitions are inside the run), streamed round a walking observer and a still one under rings that simulate every tile within 384 m (so every resident starts on the wheel, and the west tiles go as the walker leaves them), with the store kept. Three engine-hosts reach tick 600 — straight through with a save at 300, loaded from that save, and replayed from tick 0 in one call — and must agree on every part of `session.state_hash`, whose document part is every composed record (every resident's `state`, `next_event` and `position`) and whose events are the write-backs of the transitions. The number is pinned for MSVC, Clang and GCC as the save test's is, **once it has been taken on the owner's machine**: this test was written where the host cannot be built (no GPU toolchain, no SQLite download), and until the number is pinned the test fails on purpose, printing it — an unpinned replay proves only that three runs agree with each other.

**The boundary, end to end.** In that world every place is inside the simulated rings, so no resident crosses their edge and the schedule index changes nothing — the hash above did not move when it arrived. A second case of the same test, "residents that cross the simulated edge", is the one that does: 2 × 10^4 residents and 6,100 places on a 1,280 m square, the same rings round a still observer at the centre and a walker crossing the west, so the corners are never simulated; the routines' clock eight seconds before 07:00, so the seven o'clock transitions fall at tick 480, after the save. The three runs must reach one hash (**16a407bfad492334**, MSVC and Clang 18) and bring in the same residents from tiles nobody simulates in the second half — eight of them, from watches the loaded world armed again from its document — while 140 are let go into those tiles.

The migration corpus gains the record types through a save written by the test's helper (`ENGINE_SAVE_CORPUS_NPC_OUT`, "save corpus: write the resident save when asked"), whose directory and row are added as [content/migration-corpus/README.md](../../content/migration-corpus/README.md) says.

## Public API

- `systems/npc/routine.h` — `RoutineRow`, `RoutineDay`, `RoutineTable`, `routine_table`, `routine_tables`, `valid_table`, `DayPlan`, `Variation`, `draw_variation`, `RoutinePoint`, `routine_at`, `routine_at_offset`, `routine_by_stepping`, `is_day_off`, `plan_of`, and the time constants.
- `systems/npc/npc.h` — `k_determinism`, `k_timer_kind`, `k_watch_kind`, `k_entity_tiers`, `NpcConfig`, `config_from_tunables`, `PlaceIndex`, `NpcStats`, `ScheduleStats`, `ScheduledResident`, `ResidentView`, `NpcSystem` (`install`, `hooks`, `tile_source`, `set_observers`, `places`, `refresh_places`, `refresh_schedule`, `visiting`, `scheduled`, `watching`, `schedule_stats`, `bytes_scheduled`, `sink`, `summarize`, `fast_forward`, `find`, `tier_counts`, `stats`, `bytes_per_resident`, `bytes_held`).
- `systems/npc/generator.h` — `GeneratorParams`, `parse_params`, `GeneratorStats`, `place_id`, `resident_id`, `resolved`, `generate_layer`, `generate_document`.
- `systems/npc/schemas/npc.schema` — `engine.npc.Place`, `Resident`, `NpcPlace`, `NpcRoutine`, `NpcState`, `PlaceRole`, `ResidentState`, `Routine`, and the two mappings.

**Depends on.** `base`, `containers`, `math`, `hash`, `log`, `ids`, `jobs`, `time`, `tunables`, `json`, `io`, `sim`, `ecs`, `doc`, `schema`, `schemas`, `npc_schemas`. `ecs` is a capability: `engine_capability_requires(npc ecs)`.

## Testing

`tools/dev.ps1 test -Filter npc` — 27 cases in `systems/npc/tests/` (the routine, the capability through the driver and the entity store under the engine's scheduler at 1 Hz so a test walks game hours in thousands of ticks, the schedule index, the generator), `engine-content`'s `npcs` end to end, and engine-cli's two replays, at 10^5 and across the simulated edge. Benchmarks: `tools/dev.ps1 bench -Filter 'npc.*'` — `npc.tick` (10^3–10^5 held at LOD2), `npc.tick.observed` (the same with an observer at the centre), `npc.transition` (per event), `npc.summarize` (a game week), `npc.materialize` and `npc.generate`; and the schedule index's, `npc.tile_pass.scheduled` (a tile's activation with the tile source registered, against `npc.tile_pass` without), `npc.watch` (a game day with one tile live, without the source and with it: the difference over the watches fired is one watch) and `npc.schedule.build` beside `npc.places.build`.

## Performance notes

[E38](../experiments/e38-scheduled-npcs.md) has the tables and the machine. In short, on a 4-vCPU Xeon container, GCC release, quiet: a tick with 10^5 residents at LOD2 is **39 µs**; a transition off the wheel **0.33 µs** including its component writes (05 §5.6 budgets ~1 µs); a game week's summary of 10^5 residents **15.8 ms** against **4.9 s** to execute it; the generator 10^5 residents a second and a quarter. The costs that are not this capability's are named there: the write-back flush's scan of every watched entity, and a tile pass that classified every record of the document — which since the document keeps a tile index costs its tile, 0.58 ms at 10^5 on the i9 against 60 ms ([E38's follow-up](../experiments/e38-scheduled-npcs.md#follow-up-2026-09-25-a-tile-pass-costs-the-tile-and-the-schedule-index)). The schedule index adds 0.34 ms to an activation at 10^5, 1.1 µs a watch firing, and 250 ms once per document to build ("The schedule index" above).

## Not yet

- **Routines cannot be interrupted** by a world event; the closed form knows only the table and the variation (ADR-0045: an override row is the shape).
- **Capacity is not enforced**: a place's `capacity` is the generator's, and nothing stops a hundred residents at a home of four.
- **The clock and the world seed are not world settings**: `clock_offset` is per resident and the seed is the store's, 1 in a headless run.
- **The fast-forward is the wheel's**, not the world's: the scheduler has no clock jump.
- **LOD0 is LOD1**: animation, perception, navigation and bodies are Phase 6.
- **A trip is a straight line** between two places whose length has nothing to do with its duration, which is the table's.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way. Outside its directory it touched engine-host's runtime world (installing it, its hooks, its tile source, the place index and the ring's observers), engine-content's command table (`npcs`), and engine-cli's tests (the replays). **The schedule index needed one registration point that did not exist** (2026-09-25): a way to tell the one caller that brings records in by tile — the materialization driver — where a record is when its document does not know yet. It was added as the smallest thing that does that, `sim::TileSource` on `sim::Materializer` (four function pointers; [sim](sim.md#records-the-document-has-not-caught-up-with)), and the world capability's document consumer calls the driver's `materialize_arrivals` when it settles ([world](world.md#records-that-move)); nothing in the scheduler or the ring was edited, and no capability's code names this one.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(npc ecs)` | declared |
| Component and record types | `systems/npc/schemas/npc.schema`: `Place`, `Resident`, `NpcPlace`, `NpcRoutine`, `NpcState`, and the `materialize` declarations with `@writeback` and `@tiers` | done |
| Tick scheduler entry | `npc.lod` at `Lod`, `npc.motion` at `Systems` (table systems); the summarizer on the wheel; the materialization hooks; the wheel's sink, forwarding what is not its own | done |
| Materialization driver | `tile_source()`, the schedule index as a `sim::TileSource` a host registers beside the hooks | done |
| Render-graph passes | none: a resident is a transform | not needed |
| Content-build derived step | the generator is `engine-content npcs`, derived data outside the tree | done |
| Protocol methods | none: residents reach the world through materialization, and `session.run_headless` runs them | not needed |
| Tunables | `npc.lod.near_m`, `npc.lod.mid_m`, `npc.lod.hysteresis`, `npc.lod.max_promotions`, `npc.lod.max_demotions`, `npc.lod.every_ticks`, `npc.motion.every_ticks`, `npc.fast_forward.budget` | done |
| LOD policy | LOD0–1 draw a trip, LOD2 transitions only, LOD3 the record and the summary (above) | done |
| Determinism | `k_determinism` = `hashed`: the routine, its closed form, the variation, the transitions and the summary are integer functions of the world seed, the id and the time; the LOD0–1 transform and the tiers are `derived` (never written back) | done |
| Zero cost when unused | no linked code (`ENGINE_WITH_NPC=OFF`); a world with no residents holds no timers and the two systems return at their first comparison | done |
| Tests and size table | `tests/routine_tests.cpp`, `npc_tests.cpp`, `generator_tests.cpp`, `tests/size_table.cpp` | done |
| Bench | `bench/npc_bench.cpp` | done |
| Removal proof | `ENGINE_WITH_NPC`, off in the minimal build and with the ECS | works |

**Removing it.** `-DENGINE_WITH_NPC=OFF` (or `ENGINE_MINIMAL=ON`, or `ENGINE_WITH_ECS=OFF`) drops the module, its tests and its bench; engine-content refuses `npcs` with a sentence, engine-host materializes nothing of a resident (its type is unknown to the build, counted as such in the report), and a save naming `engine.npc.Resident` is refused naming it.

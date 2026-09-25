# npc (systems, capability)

**Purpose.** Scheduled residents: 13 §13.2's persistent population ("home: building 184, floor 17, apartment 1702; job: hospital 2, radiology; state: working; next event: shift end"), at the scale [05 §5.6](../plan/05-simulation.md#56-what-npc-scale-is-realistic) budgets. An `engine.npc.Resident` is a document record whose daily routine runs through the engine's timing wheel at the tier the observer set gives it; an `engine.npc.Place` is where it goes. The capability owns the routine model — tables of rows, data — and its closed form, the resident's transitions on the wheel, the `Summarizer` that brings any number of residents across any gap without visiting it, the promotion and demotion of residents by the observer set, the place index, and the generator that writes a seeded layer of places and residents. It does **not** move a resident's body, find a path, perceive, animate or collide: at every tier a resident stands at a place or is on the straight segment between two, and locomotion, navigation and bodies are Phase 6. [ADR-0045](../adr/0045-routines-are-data-with-a-closed-form.md) (proposed) records the decisions and the alternatives; [E38](../experiments/e38-scheduled-npcs.md) is the measurement.

## Why this shape

- **A routine is data with a closed form, and everything calls the closed form.** Where a resident is at game time t is a function of its routine table, a variation drawn once, and t — at most twelve rows walked, whatever t is. The materialization hook calls it at the scheduler's time, a timer's delivery at the timer's own time, the summarizer at the end of the gap, the generator at the layer's time. So executing a week, summarizing it, and materializing a resident at the end of it from a document written at the start all give the same bytes, and the tests hold all three to each other. The two alternatives — a per-tick state machine at every tier, and the schedule expanded into events ahead of time — are in the ADR with why neither can meet sim.md's summarizer contract at 10^5.
- **One timer per scheduled resident.** 05 §5.6's LOD2 is "scheduled transitions only": the wheel holds each materialized resident's next transition and nothing ticks in between. A resident at LOD2 costs the wheel's bitmask scan and nothing else until its row ends.
- **A tier changes what runs, not what is true.** What is written back — `state`, `next_event` and the anchor `position` — is the closed form's at the transition, whatever the tier. The transform LOD0–1 moves along a trip is never written back. A resident observed and the same resident unobserved hold the same state to the byte (tested).
- **LOD3 is not an entity.** A resident in a tile the ring does not simulate is its record, in the document and — when its tile went — in the store's projections: 05 §5.6's "a record in the store plus occasional summary". Its summary is the closed form, applied when its tile comes back.
- **Tiers are this capability's pass.** Nothing drove `promote`/`demote` for document records (sim.md, world.md); the ring maps rings to representations, not to tiers, and the scheduler may not be edited (ADR-0027). So a table system at `Lod` runs `TierAssignment` over the held residents against the host's observers and applies the result through `SimScheduler::apply_tier_changes`, which every capability's hooks see.

## Owned data

The resident table: per held resident its id, entity handle, drawn variation, clock offset, four resolved places, the document's position as a fallback, its current routine point, its timer, where it is drawn, its importance and its tier — dense arrays, swap-removed, with an index from entity handle to slot (a resident's slot is not stable; the wheel's payload names the entity handle). The place index (every `Place` of the document by id, its position and role). The two table systems, the summarizer registration and the wheel's event sink. Nothing else may hold or change a resident's routine state; its components are written by this capability alone.

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

A resident's record is filed under its anchor's tile: at home, its home's tile; at work, its job's; on a trip, its destination's. So a transition that sends it to another tile's place is a **record that moves** ([world](world.md#records-that-move)): the write-back tells the document, the driver reports it (`take_moved`), and the world's document consumer settles it — refiled under the new tile when that tile is live in its rings, let go when it is not. A resident let go is at LOD3: its state is in the document, and the closed form brings it up to date when its new tile comes in (tested).

**The boundary, stated.** What is materialized is a function of where each record's document puts it. A resident that walked to work in a tile the ring does not simulate stays there, as a record, **even after its closed form has it back home in a live tile**, until its record's tile comes in. So what is visible is always right, and what should be visible can be missing: an observer standing in a residential tile at night sees the residents whose records are there, not the ones whose work tiles went out of simulation before they came home. The fix is a schedule index — for each tile, the residents whose routines visit it — that the document consumer consults on activation and on a timer; it is named as a follow-up in [ADR-0045](../adr/0045-routines-are-data-with-a-closed-form.md), not built. In the Phase 3 replay the simulated rings cover every place, so no resident crosses the boundary there.

## The generator

`generate_layer(params, jobs)` and `generate_document(vfs, dir, params, jobs)` (`generator.h`), and `engine-content npcs <params.json> --out <document dir> [--jobs <n>]` over them: a partitioned layer of places, then residents, added to the document at the directory as its **edit layer** (so the write-back's `SetProperty` lands in the layer that defines the record and the partition files a moved resident under its new tile), created when there is none. Parameters: `seed`, `residents`, `places` (homes, workplaces, services, leisure; by default a home per four residents, a workplace per 40, a service place per 50, a leisure place per 100), `extent`, `tile_size`, `layer`, `time_us`, `clock_offset_us`, and the routine `mix`. Places land on a quarter-metre grid inside the extent; a resident's home, job, service and leisure places are hashed choices among each kind; its `state`, `next_event` and `position` are the closed form at `time_us`, so a world that starts there writes nothing back until the first transition (tested). Records carry only what differs from their type's defaults and what the partition reads. **The layer does not depend on the thread count**: every record is a pure function of (params, index), the job system only splits the index range, and the layer holds its records by id; the test compares the layer's bytes at none, one and four workers, and `engine-content`'s at one and four, and pins its hash (Clang and GCC agree). Derived data: never committed; tests write it under their `TempDir`.

## Storage per resident

| Where | Bytes | What |
|---|---|---|
| this capability's arrays | 177, plus ~16 in the handle index and a 12-byte scratch the tier pass scores from | id 16, handle 8, variation 60, clock offset 8, four places 16, fallback and drawn position 24, point 32, timer 8, importance 4, tier 1 (`bytes_per_resident()`, pinned) |
| the wheel | 56 | one `TimingWheel::Slot` |
| the entity's components | 128 | `Transform` 28, `NpcRoutine` 80, `NpcState` 24 (size table) |
| flecs, the identity map, the write-back shadow | see [E38](../experiments/e38-scheduled-npcs.md) | measured as the process's resident-set growth |
| LOD3: the document | see E38 | a record's composed JSON in memory, and its bytes on disk |

05 §5.6's ~200 B is a *LOD3* figure; E38 says how far a held resident and a record each are from it, and why.

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
- The generator's layer is the same bytes at any thread count; its parameters are read strictly; its document reads back.
- End to end (`apps/engine_cli/tests/npc_replay_tests.cpp`): 10^5 residents streamed, saved at tick 300, loaded, and replayed from tick 0 reach one persistent-state hash — see "Held to the clause".

## Held to the clause

The Phase 3 exit's last clause ([10 §10.2](../plan/10-roadmap-risks.md)) is `npc: 10^5 scheduled residents streamed, saved, loaded and replayed to one state hash`: the generator's 10^5 residents and 30,500 places on a 512 m square of 64 m tiles, their clocks at 07:00 at game time 0, streamed round a walking observer and a still one under rings that simulate every tile within 384 m (so every resident starts on the wheel, and the west tiles go as the walker leaves them), with the store kept. Three engine-hosts reach tick 600 — straight through with a save at 300, loaded from that save, and replayed from tick 0 in one call — and must agree on every part of `session.state_hash`, whose document part is every composed record (every resident's `state`, `next_event` and `position`) and whose events are the write-backs of the transitions. The number is pinned for MSVC, Clang and GCC as the save test's is, **once it has been taken on the owner's machine**: this test was written where the host cannot be built (no GPU toolchain, no SQLite download), and until the number is pinned the test fails on purpose, printing it — an unpinned replay proves only that three runs agree with each other.

The migration corpus gains the record types through a save written by the test's helper (`ENGINE_SAVE_CORPUS_NPC_OUT`, "save corpus: write the resident save when asked"), whose directory and row are added as [content/migration-corpus/README.md](../../content/migration-corpus/README.md) says.

## Public API

- `systems/npc/routine.h` — `RoutineRow`, `RoutineDay`, `RoutineTable`, `routine_table`, `routine_tables`, `valid_table`, `DayPlan`, `Variation`, `draw_variation`, `RoutinePoint`, `routine_at`, `routine_at_offset`, `routine_by_stepping`, `is_day_off`, `plan_of`, and the time constants.
- `systems/npc/npc.h` — `k_determinism`, `k_timer_kind`, `k_entity_tiers`, `NpcConfig`, `config_from_tunables`, `PlaceIndex`, `NpcStats`, `ResidentView`, `NpcSystem` (`install`, `hooks`, `set_observers`, `places`, `refresh_places`, `sink`, `summarize`, `fast_forward`, `find`, `tier_counts`, `stats`, `bytes_per_resident`, `bytes_held`).
- `systems/npc/generator.h` — `GeneratorParams`, `parse_params`, `GeneratorStats`, `place_id`, `resident_id`, `resolved`, `generate_layer`, `generate_document`.
- `systems/npc/schemas/npc.schema` — `engine.npc.Place`, `Resident`, `NpcPlace`, `NpcRoutine`, `NpcState`, `PlaceRole`, `ResidentState`, `Routine`, and the two mappings.

**Depends on.** `base`, `containers`, `math`, `hash`, `log`, `ids`, `jobs`, `time`, `tunables`, `json`, `io`, `sim`, `ecs`, `doc`, `schema`, `schemas`, `npc_schemas`. `ecs` is a capability: `engine_capability_requires(npc ecs)`.

## Testing

`tools/dev.ps1 test -Filter npc` — 22 cases in `systems/npc/tests/` (the routine, the capability through the driver and the entity store under the engine's scheduler at 1 Hz so a test walks game hours in thousands of ticks, the generator), `engine-content`'s `npcs` end to end, and engine-cli's replay at 10^5. Benchmarks: `tools/dev.ps1 bench -Filter 'npc.*'` — `npc.tick` (10^3–10^5 held at LOD2), `npc.tick.observed` (the same with an observer at the centre), `npc.transition` (per event), `npc.summarize` (a game week), `npc.materialize` and `npc.generate`.

## Performance notes

[E38](../experiments/e38-scheduled-npcs.md) has the tables and the machine. In short, on a 4-vCPU Xeon container, GCC release, quiet: a tick with 10^5 residents at LOD2 is **39 µs**; a transition off the wheel **0.33 µs** including its component writes (05 §5.6 budgets ~1 µs); a game week's summary of 10^5 residents **15.8 ms** against **4.9 s** to execute it; the generator 10^5 residents a second and a quarter. The costs that are not this capability's are named there: the write-back flush's scan of every watched entity, and a tile pass that classifies every record of the document.

## Not yet

- **Routines cannot be interrupted** by a world event; the closed form knows only the table and the variation (ADR-0045: an override row is the shape).
- **The boundary**: a resident whose record is in a tile the ring does not simulate is not materialized when its closed form has it in a live one (above); a schedule index is the fix.
- **Capacity is not enforced**: a place's `capacity` is the generator's, and nothing stops a hundred residents at a home of four.
- **The clock and the world seed are not world settings**: `clock_offset` is per resident and the seed is the store's, 1 in a headless run.
- **The fast-forward is the wheel's**, not the world's: the scheduler has no clock jump.
- **LOD0 is LOD1**: animation, perception, navigation and bodies are Phase 6.
- **A trip is a straight line** between two places whose length has nothing to do with its duration, which is the table's.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way. Outside its directory it touched engine-host's runtime world (installing it, its hooks, the place index and the ring's observers), engine-content's command table (`npcs`), and engine-cli's tests (the replay).

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(npc ecs)` | declared |
| Component and record types | `systems/npc/schemas/npc.schema`: `Place`, `Resident`, `NpcPlace`, `NpcRoutine`, `NpcState`, and the `materialize` declarations with `@writeback` and `@tiers` | done |
| Tick scheduler entry | `npc.lod` at `Lod`, `npc.motion` at `Systems` (table systems); the summarizer on the wheel; the materialization hooks; the wheel's sink, forwarding what is not its own | done |
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

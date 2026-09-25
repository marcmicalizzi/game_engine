# ADR-0045: Resident routines are data with a closed form, and their tiers are the capability's own pass

- **Status:** Proposed
- **Date:** 2026-09-25
- **Plan references:** docs/plan/05-simulation.md §5.3–5.6, §5.10; docs/plan/13-reference-consumer-games.md §13.2; docs/plan/10-roadmap-risks.md §10.2 (Phase 3 exit); docs/plan/03-data-model.md §3.4–3.5
- **Docs touched:** `docs/subsystems/npc.md` (new), `docs/subsystems/sim.md` and `docs/subsystems/world.md` ("What is stubbed", "Not yet"), `docs/subsystems/apps.md` (engine-content `npcs`, engine-host), `docs/plan/05-simulation.md` §5.3–5.6 status notes, `docs/plan/13-reference-consumer-games.md` §13.2, `docs/plan/10-roadmap-risks.md` (Phase 3 exit note, E38), `docs/experiments/e38-scheduled-npcs.md`

## Context

The last clause of the Phase 3 exit is "a tile-streamed world with 10^5 scheduled NPCs". Everything under it existed and had been measured in isolation — the timing wheel at 10^6 timers, tier assignment at 10^5 entities, the materialization driver and the entity store at 10^5 records, the ring, save and load, the replay hash — but nothing scheduled an NPC's routine through the wheel from a document, and three questions the plan leaves open had to be answered to do it:

1. **What a routine is.** 05 §5.6 budgets LOD2 at "scheduled transitions only, ~1 µs per event, a few events per hour" and LOD3 at "a record in the store plus occasional summary, ~200 B". sim.md makes a `Summarizer` mandatory for any LOD2/LOD3 system and states five conditions for it (equivalence, determinism from stated inputs, idempotence over a partition, no events out, bounded cost). 13 §13.2 wants the same resident — the same shift schedule — however and whenever it is materialized.
2. **Who drives a resident's tier.** `TierAssignment` and the hooks' `promote`/`demote` exist, but nothing drives them for document records after materialization (sim.md, "What is stubbed"; world.md, "Not yet": the ring's rings map to representations, not to the simulation's tiers). ADR-0027 forbids editing the scheduler to add a capability.
3. **What clock a routine runs on.** A headless run starts at game time 0 and has no parameter to start anywhere else; a world has no settings record to say what time of day game time 0 is.

The alternatives on the table for (1):

- **A per-tick state machine at every tier.** Every resident ticks, checks its schedule and advances. Simple, and O(N) every tick whatever happens: at 10^5 residents it spends the tick budget on residents doing nothing, and it has no summary except executing every step, so it cannot meet sim.md's fifth condition and cannot claim LOD2/LOD3 at all.
- **The schedule stored as events.** Each resident's transitions for a horizon are expanded into the wheel (or the store's log) ahead of time. Memory grows with residents × horizon, a fast-forward still visits every stored event, a change to a resident means cancelling its future, and a save carries the expansion.
- **A routine as data with a closed form.** A table of rows per archetype, a per-resident variation drawn once, and a function from (table, variation, t) to where the resident is. The wheel holds one timer per scheduled resident — its next transition — and the summary is the same function evaluated at the end of the gap.

## Decision

1. **A routine is data.** An archetype is a table of rows — (state, place role, start window, duration window) — for a work day and a day off, with a number of days off a week and a shift width (`systems/npc/src/routine.cpp`, six archetypes). A row starts when the one before it ends or, with a start window of its own, at the later of that and a time drawn from the window; the last row runs to the next day's first. A table is valid only when no day can run into the next (`valid_table`), and the shipped tables are held to it by a test.
2. **The variation is drawn once**, from the world seed hashed with the record id — a shift, a draw inside every window, which days are off — in integer minutes and microseconds, so it is the same bytes on every compiler (pinned by a test on Clang and GCC).
3. **Where a resident is at any time is a closed form** (`routine_at`): the day t falls in and the row within it, at most twelve rows walked, whatever t is. Every path that places a resident calls it: the materialization hook at the scheduler's time, a timer's delivery at the timer's own time, the summarizer at the end of the gap, the generator at the layer's time. Execution, summary and materialization are therefore one function, and the summarizer meets sim.md's five conditions **exactly**, not approximately (npc.md, "The summarizer").
4. **LOD2 is one `Timer(entity, kind, at)` per materialized resident**, due at the end of its current row; firing it puts the resident in the next row and arms the next timer. **LOD3 is not an entity**: a resident in a tile the ring does not simulate is its document record, brought up to date by the closed form when its tile comes back. **LOD1 and LOD0** draw a trip on the segment between its two places by elapsed fraction; that transform is never written back.
5. **What is written back is what the routine decided** — `state`, `next_event`, and the anchor `position` (the place of the current row; for a trip, its destination) — so it is in the document at `Persist` and in a save, and a resident's tile is its anchor place's tile. Moving between tiles uses the existing rule for records that move (world.md): refiled when its new tile is live, let go when it is not.
6. **Tiers are the capability's own pass.** A table system at `TickPhase::Lod` (`npc.lod`) runs `TierAssignment` over the residents it holds against the host's observer set — the ring's observers in engine-host — every `npc.lod.every_ticks` ticks, and applies the changes through `SimScheduler::apply_tier_changes`, so every capability's `promote`/`demote` hooks see them. A resident materialized while observers exist starts at LOD2 and is promoted by distance (05 §5.5 step 4). Nothing in the scheduler or the ring was edited.
7. **The clock.** A resident carries `clock_offset`: its routine's time of day at game time 0. It is document data, so a save and a replay carry it; it is per resident only because a document has no world settings to hold it once (the world seed has the same gap, world.md "Not yet").
8. **Places are indexed from the document**, not from what is materialized: a resident whose job is in a tile nobody simulates still goes there.
9. **The wheel's one sink.** The scheduler delivers timers to a single sink and nothing had set it; the capability installs itself there and forwards every event that is not a resident's to a `next` sink its host names.

## Consequences

- A resident costs nothing between its transitions: a tick with 10^5 held at LOD2 is the wheel's bitmask scan and two early outs (E38), and a transition is one closed form, three component writes and a timer.
- A fast-forward of any length costs one visit per held resident, and executing it instead reaches the same bytes; the tests assert both, and (a, b] then (b, c] against (a, c].
- Materializing a resident twice, in any order, at any time, from a stale document, gives the same resident: the document's `state` and `next_event` are an output, not an input.
- **A routine cannot yet be interrupted.** A world event that should send a resident home early has nowhere to go: the closed form knows only the table and the variation. The day that is needed, a resident gains an override (a row that holds until a time) that the closed form consults first; it stays a closed form.
- **The boundary is the record's tile.** A resident whose document record sits in a tile the ring does not simulate — it walked to work there — is not materialized even when its closed form has it back home in a live tile, until its record's tile comes in. What is visible is always right; what should be visible can be missing. The fix is a schedule index (which residents' routines visit a tile) that the document consumer consults, and it is a follow-up, not part of this decision. *Followed up 2026-09-25:* the index is built ([npc](../subsystems/npc.md#the-schedule-index)), consulted on a tile's activation and on a watch timer of each resident that is not held, through a registration point added to the materialization driver for it ([sim](../subsystems/sim.md#records-the-document-has-not-caught-up-with)); the boundary is now the tile the routine has the resident in, and decisions 5 and 6 stand as written.
- The write-back flush scans every watched entity, which at 10^5 residents is three watched fields each (E38 measures it).
- The fast-forward is the wheel's: the scheduler's clock has no jump, so `NpcSystem::fast_forward` advances the wheel alone; a world-level fast-forward is the scheduler's to add.
- Forbidden: a routine table that can run into the next day; a resident state computed any way but the closed form; a tier deciding anything that is written back.

## Revisit when

- A game needs routines that react to world events (the override above), or residents whose places change during a run.
- A document carries world settings: `clock_offset` and the world seed move there and residents stop carrying the clock.
- The tier pass over the held residents (E38) or the write-back scan becomes the tick's largest cost at the population a game holds, or ECS change detection lands and the scan goes.
- The ring's rings are mapped to simulation tiers, at which point the tier pass may read the ring instead of scoring every resident.

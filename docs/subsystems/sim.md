# sim (domain)

**Purpose.** The simulation core of [05 §5.2–§5.5](../plan/05-simulation.md#52-sim-scheduler): the hierarchical timing wheel over `GameTime` and its budgeted fast-forward, LOD tier assignment over the observer set, and the tick scheduler that carries the phase order, the system registration table, the materialization contract of [03 §3.4](../plan/03-data-model.md#34-the-runtime-world), and tile reconciliation. It is the registration point capabilities attach to when they tick, schedule, summarize, or materialize ([ADR-0027](../adr/0027-additive-capabilities.md)), which is why it is **not** an optional capability: a registration point that can be switched off is one nobody can register with.

**Owned data.** The timer slab and its free list, the wheel's buckets and occupancy bits, the summarizer table, the system registration table and the schedule computed from it, the materialization hooks table, and the fixed-step and game clocks of one simulation. Nothing else may hold a timer's storage; a `TimerHandle` is the only outside reference to one.

## Why a wheel and not a heap

The simulation schedules 10^5–10^6 timers whose due times are spread over game *years*, and cancels most of them before they fire: an NPC's next meal is rescheduled every time it eats, and a rescheduled timer is a cancel plus an insert. A binary heap pays O(log n) on both, with a cache miss per level of the heap, and cannot cancel at all without a side index from handle to heap position that has to be maintained on every sift. The wheel pays a bucket push and a bucket unlink, both O(1). Measured at 10^6 timers: 68 ns to insert, 32 ns to cancel, and the cost of neither grows with how many timers are asleep.

What the wheel gives up is a cheap answer to "what is the next event": it has to look. This implementation buys that back with one occupancy bit per bucket, so `advance` finds the next non-empty bucket with a `countr_zero` over at most sixteen words per level, and skipping a game month with nothing scheduled costs the same as skipping a tick — which is exactly the operation [§13.5](../plan/13-reference-consumer-games.md#135-mapping-to-the-roadmap)'s "leave for a game-week and the dune has moved" needs to be cheap.

**The level ladder is ticks, seconds, minutes, hours, days — approximately, and on purpose.** Level 0's resolution is the fixed step exactly; every level above is an integer multiple of the level below (60, 60, 60, 24, then 1024 buckets of about a day), so cascading between levels is exact integer arithmetic. The multiples are chosen so the spans land near a second, a minute, an hour and a day, *near* and not *on*, because a 60 Hz step is 16,667 µs and no ladder of integers off that hits a second. The drift is 0.002% at the day level and it is invisible: a level span is an internal bucketing choice, while a timer's due time is exact microseconds and is compared exactly. A design that forced the levels onto real seconds would have to make the tick level inexact instead, which would put the error where it is visible.

**A timer beyond the top level's span (about 2.8 game years) is not dropped.** It waits in a far list that is re-homed when the top level's block turns over, and `advance` will find it by scanning that list when every level is empty. The list is linear because it is meant to be nearly empty; a game that schedules a population of timers decades out should say so, and the wheel would then want a sixth level rather than a longer list ([ADR-0017](../adr/0017-no-hidden-limits.md): the limit is stated and handled, not hidden).

**Delivery order is (due time, insertion sequence).** Both are integers the wheel assigns, so two runs with the same inputs deliver the same events in the same order on every machine, and the order does not depend on how many timers happened to share a bucket or on what order the bucket's list is threaded in. A periodic keeps the sequence it was registered with across every firing, so a periodic registered before a timer always precedes it when they fall on the same instant, however many times it has fired since.

**A sink may schedule and cancel while it is being delivered to.** A timer the sink schedules for a time still inside the current `advance` is delivered by that same `advance`; a timer the sink cancels is skipped even if it was already in the batch being delivered (the batch holds handles, not indices, and a stale one is refused). A periodic re-arms *before* its sink runs, so a sink that cancels its own timer wins and a sink that does not gets a wheel already holding the next firing. What a sink may not do is call `advance` re-entrantly.

## Fast-forward: why the budget is the summarization trigger

`advance(from, to, budget, sink)` first *counts* the events due in the gap — walking only the buckets whose time range intersects it, and counting a periodic's firings arithmetically rather than one visit at a time — and stops counting as soon as the count passes the budget. If it did not pass, the gap is an ordinary advance and nothing is coarsened. If it did, every registered summarizer is asked to `summarize_interval(from, to)` exactly once and the periodics of summarized systems are re-armed past `to` instead of firing.

The budget is the trigger rather than, say, the gap length, because **the gap length is not what costs anything**. Thirty game days with one weekly periodic in them is 4 events and should simply run; thirty game days with an hourly economy is 720 and should too, if the caller can afford 720; thirty game days for ten thousand tiles' worth of NPC routines is tens of millions and cannot. The quantity the caller actually has to bound is the number of events it is willing to execute, so that is the quantity the API takes. It also makes the trigger *testable*: the same wheel with the same content takes the executing path under a large budget and the summarizing path under a small one, and the module's test asserts that an exact summarizer ends in the same state either way.

**The budget is a trigger, not a cap.** One-shot timers and the periodics of systems that registered no summarizer are delivered whatever the budget says, because they have no coarse form — a scheduled assassination does not have an average. That is deliberate: a budget that silently dropped them would turn a performance knob into a correctness one.

### The contract every LOD2/LOD3 system must meet

A system that runs at LOD2 or LOD3 registers a `Summarizer` ([05 §5.3](../plan/05-simulation.md#53-event-scheduler-temporal-lod) makes this mandatory) and its `summarize_interval(from, to)` must hold all of:

1. **Equivalence.** Applying the summary over `(from, to]` must leave the system in the state it would have reached by executing every one of its periodic events in that interval. Exactly, if the system's per-event effect is affine in time or otherwise closed-form (an hour counter, a stockpile that accrues at a rate, a decay toward a baseline). Approximately, with the approximation *written down on the system's own docs page*, when it is not — and then the error must not accumulate without bound across repeated summaries, because a tile can be summarized many times over a save's life.
2. **Determinism from stated inputs only.** The summary is a function of `from`, `to`, the system's own persistent state, and — for the reconciliation path — the tile's stored `seed` and the global events that intersect the tile. Never of wall-clock time, of how the gap was split into calls, or of anything the renderer produced. `SummarizeInterval` carries `tile` and `seed` for exactly this reason.
3. **Idempotence over a partition.** Summarizing `(a, b]` then `(b, c]` must equal summarizing `(a, c]`. A caller is free to fast-forward in several steps, and a system whose summary is not additive over adjacent intervals will drift apart from one that is.
4. **No events out.** A summary may write the system's own state and enqueue *future* timers; it must not emit world events dated inside the gap. Those events never happened at LOD2/LOD3 and nothing was there to observe them; emitting them retroactively is how a fast-forward turns into a cascade ([05 §5.7](../plan/05-simulation.md#57-world-events-and-consequences)).
5. **Bounded cost.** O(1) in the gap length, or at worst O(events in the gap) with a constant far below executing them. A summary that loops over the skipped events one at a time is not a summary.

A system that cannot meet (1) honestly should not claim LOD2/LOD3 at all: it should keep its periodic un-summarized, and accept that a fast-forward executes it.

## Why tiers are a function over the observer set

A tier is not a property of an entity. `TierAssignment` computes, per entity, the **minimum over observers** of `distance / (observer weight × entity importance)` and maps that score through a table of band boundaries. The observer set is the generalization that keeps working: two players in co-op, a security camera, a quest marker, an audio listener, and a dedicated server with no camera at all are all observers, and nothing in the simulation has to know which one is "the player" — [05 §5.12](../plan/05-simulation.md#512-multiplayer-readiness) calls the same set interest management, and it is the same set. A radius around a singleton camera would have to be unpicked for every one of those cases, and the unpicking would reach into every capability's LOD policy.

Importance and weight divide the distance rather than shifting the boundaries, so one table of boundaries serves every entity and every observer: a named character in a quest is "nearer" to every observer at once, and a heavyweight observer pulls everything near it up a tier without a second table. Importance covers what [05 §5.4](../plan/05-simulation.md#54-lod-tier-assignment) lists — quest relevance, named-character status, being in combat, being audible — as one number the game computes however it likes.

**Hysteresis and rate limits are what make the function usable rather than merely correct.** Promotion tests the band boundary; demotion tests the boundary widened by `hysteresis`. A crowd walking along a boundary therefore crosses once instead of oscillating, and the cost of materializing is paid once per crossing rather than every tick — the module's test walks one entity across a boundary and back with and without the band, because the difference is the whole point. On top of that, at most `max_promotions` and `max_demotions` entities change tier per call, nearest-first for promotions and farthest-first for demotions, so a camera cut that puts ten thousand entities inside the LOD0 radius spreads its materialization over ticks instead of dropping a frame. Deferred candidates are not forgotten; they are simply re-evaluated next tick.

**Changes come out in entity order**, not in the order the rate limit selected them, so a consumer walks its own arrays forwards and two runs produce identical bytes.

## The tick

`SimScheduler` runs the eight phases of [05 §5.2](../plan/05-simulation.md#52-sim-scheduler) in order — input, events in, LOD assignment, systems, physics, post-physics, events out, persistence flush — on a fixed step from `core/time`. The timing wheel is pumped at the top of `events in`, so a phase's systems see this tick's events and nothing arrives mid-phase; the persistence hook closes `persist`.

`step()` reads no clock at all, which is what lets a replay, a headless fast-forward and a live session take the same path ([05 §5.10](../plan/05-simulation.md#510-determinism-and-replay)). `advance(real_ns)` is the same machinery with a `FixedStepClock` in front of it. The scheduler keeps its own tick counter rather than the accumulator's, so `step()` called directly still counts.

**The schedule is a pure function of the registration.** Systems declare read and write component-set masks; two systems conflict when one writes something the other reads or writes (read-read is free, which is the point of declaring the sets). Within a phase, a system is placed one *wave* after the latest system it conflicts with, in declaration order — longest-path layering, which is the fewest barriers that still preserves declaration order between every conflicting pair. Systems in one wave provably do not conflict and run in parallel on `core/jobs`' performance pool; waves are separated by a wait. Nothing about this depends on the worker count, and `schedule_hash()` is a 64-bit summary a test or a tool can compare: the module's test asserts that eight workers produce the same bytes as one and as none.

A system that declares empty masks declares "I conflict with nothing", which is a promise, not an absence of information. Declaring nothing and then racing is a bug in the system, and the masks are where it is visible in review.

**`domain/sim` does not depend on `domain/ecs`.** `sim::TickPhase` is declared here with the same enumerators and the same order as `ecs::TickPhase`, because [ADR-0028 §7](../adr/0028-ecs-and-persistent-store.md) deliberately leaves open whether flecs' pipeline stays the tick scheduler. **The two should become one type in the change that settles that — `ecs::TickPhase` aliasing `sim::TickPhase`** — and not the other way round, which would make the scheduler depend on flecs. Until then the duplication is the honest representation of an open question, and the phase-order tests on both sides pin it.

## The materialization contract

[03 §3.4](../plan/03-data-model.md#34-the-runtime-world)'s four hooks — `materialize(record, tier)`, `promote(entity, from, to)`, `demote(entity, from, to)`, `dematerialize(entity)` — are a table of function pointers with a context and a tier mask, walked in registration order. They are data and not a base class for the same reason `SystemDesc` is: a capability that implements none of them contributes no row and costs nothing, and the tier mask means a hook that only exists at LOD0/LOD1 is not called for a transition that happens entirely beyond it.

`apply_tier_changes(changes, entities)` is the driver: the tier code never sees an entity id (it works on SoA indices) and the hook code never sees a position, and the caller's `entities[change.index]` is the one place they meet.

## Tile reconciliation

`reconcile_tile` is [05 §5.5](../plan/05-simulation.md#55-reconciliation-when-a-tile-activates)'s five steps over those hooks: load the tile's projections from the store, compute the elapsed game time since the tile was last active, run every LOD3 summarizer over the gap from the tile's stored seed, materialize the records at LOD2, then promote by observer distance. The plan's own fifth step — applying derived visual state by selecting damage-state variants — is the renderer's and is deliberately not done here.

The store is a pair of function pointers, not an include of `foundation/store`: reconciliation needs "give me this tile's state" and "give me this tile's records", `domain/sim` must not depend on an optional capability ([ADR-0027](../adr/0027-additive-capabilities.md) decision 1), and a test can hand it a fake. Summarizers run in registration order, so two reconciliations of the same tile produce the same summary in the same order whatever the tile contains.

## Invariants

- Every live timer sits in the bucket its due time and the wheel's current position name, its level is the one `choose_level` would pick, and the occupancy bit of every bucket agrees with whether that bucket's list is empty. `TimingWheel::validate()` checks all of this against a linear recomputation; the cascade test calls it after every one of 2,940 advances.
- A slot's generation is odd while live and even while free, so a handle from a previous occupant of a slot is refused rather than cancelling somebody else's timer.
- `advance` never moves `now` past an undelivered due timer: the skip search jumps only to the start of a bucket that has work, and a partially covered final bucket is checked for a genuinely due entry before it is returned.
- `assign_tiers` writes back exactly the transitions it emits, and emits them in ascending entity index.
- A phase's schedule is ordered by (wave, declaration index), and two systems that conflict are never in the same wave.

## Public API

- `domain/sim/timing_wheel.h`: `TimerHandle`, `TimerPayload`, `TimerEvent`, `EventSink` and `make_sink`, `SummarizeInterval`, `Summarizer`, `TimingWheelConfig`, `FastForwardResult`, `TimingWheel` (`schedule`, `schedule_periodic`, `cancel`, `is_live`, `due_time`, `advance` in both forms, `count_due`, `add_summarizer`, `reset`, `validate`, and the shape accessors).
- `domain/sim/tiers.h`: `ObserverSet`, `TierParams`, `TierChange`, `TierStats`, `TierInput`, `TierAssignment` (`assign_tiers`, `score`, `tier_of`).
- `domain/sim/scheduler.h`: `TickPhase` and `phase_name`, `ComponentMask`, `Determinism`, `Batch`, `SystemContext`, `SystemDesc`, `ScheduleEntry`, `EntityRecord`, `MaterializationHooks`, `TileState`, `TileStore`, `ReconcileParams`, `ReconcileResult`, `SimSchedulerConfig`, `SimScheduler`.

**Determinism stance ([ADR-0010](../adr/0010-deterministic-sim-and-lod-contract.md)):** `hashed`. Every ordering decision in the module is made on integers the module itself assigned (due time, insertion sequence, declaration index, entity index); the only floating point is the tier score, which is computed per entity from the inputs alone and never compared across entities except through a sort whose ties break on the index.

**LOD policy.** This module *is* one of the registration points for LOD policy, and it supplies the reference implementation the rest of the engine's capabilities are expected to agree with (`TierAssignment::score`/`tier_of` are public so a capability's own policy can be checked against them).

**Zero-cost-when-unused ([11 §11.10](../plan/11-performance-principles.md#1110-absent-capabilities-are-free)):** no instances. A wheel with no timers scheduled has 1,229 empty buckets and five zero occupancy words, and `advance` over any interval is a bitmask scan that finds nothing; a scheduler with no systems registered runs eight phases of nothing; a hooks table with no rows makes `apply_tier_changes` a loop over an empty vector. The module allocates nothing per event in steady state: timers come from a slab with a free list, and the scheduler's per-wave invocation and job arrays are reserved once when the schedule is built.

**Depends on.** `base`, `containers`, `time`, `math`, `jobs`, `log`, `ids`.

## Testing

`tools/dev.ps1 test -Preset msvc-debug -Filter sim` — 35 cases, 3,210 assertions:

- **Timing wheel**: the level ladder's exact resolutions; due order with ties in insertion order; O(1) cancel and refusal of a stale handle after slot reuse; cascading across all five levels checked with `validate()` after each of 2,940 one-minute advances; a 29-day timer found in a single jump; a periodic firing on its phase (registered at 09:13, first firing at 10:00) and keeping its handle across firings; a timer past the horizon kept in the far list and delivered 2,000 days later; a timer scheduled in the past due immediately; a sink that schedules and cancels mid-delivery; and byte-identical output from two runs of a 4,000-timer schedule with periodics and cancellations, including the same bytes when the same interval is walked in 997 advances instead of one.
- **Fast-forward**: the plan's own case — an hourly economy over a 30-game-day gap delivers 720 events under a large budget, and under a small one makes exactly one summarize call per registered summarizer and fires nothing; an exact summarizer (a counter of game hours) ends at 720 either way; a periodic with no summarizer and a one-shot timer are delivered anyway; a gap under the budget is bit-for-bit an ordinary advance; and counting stops once it passes the budget.
- **Tiers**: the minimum over observers, including what importance and observer weight do to it; hysteresis holding an entity through a boundary walk that oscillates without it; rate limits promoting nearest-first and deferring the rest to later ticks; changes emitted in entity order when selection order was the reverse; 20,000 entities over six ticks producing identical tier arrays and identical change bytes at none, one and eight workers; and a configurable tier count with out-of-range tiers repaired.
- **Scheduler**: the eight phases in plan order when the systems are registered back to front; conflicting systems in declaration order and non-conflicting ones sharing a wave; `schedule_hash()` equal for two identical registrations and different for a reordered one; sixteen systems of four batches over eight ticks producing identical results at none, one and eight workers; `begin_tick`/`end_tick` once a tick around the waves; the fixed step driving game time and the wheel; the persistence hook; tier changes driving the hooks, including a hook that only declared some tiers; `reconcile_tile`'s five steps against a fake store and a fake system, with the summary taken from the tile's seed; the same tile reconciled twice giving the same summary; an unknown tile materialized without a summary; and the component mask's stated 256-type width.

The size table pins `TimingWheel::Slot` at 56 bytes and eighteen other types; at 10^6 timers the slab is 56 MB, so a byte there is a megabyte.

## Performance notes

Measured with `tools/dev.ps1 bench -Preset msvc-release -Filter sim.*` on an i9-10980XE, release, otherwise idle.

| Benchmark | Per operation | What it covers |
|---|---|---|
| `sim.wheel.insert` | **68 ns** | 10^6 timers spread over 30 game days into an empty wheel |
| `sim.wheel.cancel` | **32 ns** | cancelling every second one of those 10^6 |
| `sim.wheel.advance` | **662 ns** | advancing 30 game days, delivering the surviving 5×10^5 |
| `sim.wheel.tick` | **106 ns** | one fixed step with 10^6 timers resident and nothing due |
| `sim.tiers.assign` | **23.0 ns** (no job system) → **6.1 ns** (8 workers) | 10^5 entities, three observers |
| `sim.scheduler.tick` | **0.97 µs** (no job system) | 16 systems × 4 batches, empty bodies |

Three things the numbers say:

1. **Insert and cancel cost what an O(1) bucket operation should**, and the dominant term is not the list surgery but the integer divisions that map a due time to a level index. Computing all six indices by successive division from level 0 — rather than dividing the due time by each level's resolution and by each level's block size separately — plus caching the wheel's own indices took insert from 176 ns to 68 ns, a 2.6× improvement with no change in behaviour. The next step, if it is ever needed, is a reciprocal-multiply table for the five divisors.
2. **`advance` is dominated by cascading, not by delivery.** With 5×10^5 timers spread over 30 days, every timer falls from level 4 to level 0 through four cascades, so 5×10^5 deliveries do about 2×10^6 re-links, each one a random access into 56 MB of slab. 662 ns per delivered event is therefore a worst case for a wheel that is being drained from empty; the operating point [05 §5.6](../plan/05-simulation.md#56-what-npc-scale-is-realistic) actually projects — 10^6 LOD3 NPCs at about 3.5k events a second — is 2.3 ms of CPU per wall-clock second, and `sim.wheel.tick` shows that holding those 10^6 timers costs 106 ns of a 16.7 ms tick when none of them is due. If the cascade ever shows up in a profile, the fix is to make a bucket a contiguous array of slot indices with the slot carrying its position, which trades the intrusive list's second random write for one.
3. **Tier assignment scales 3.7× from no job system to eight workers**, not 8×, because scoring is only part of it: the candidate selection, the two sorts and the write-back are serial. Keeping the serial part to a compare-and-append is what makes even 3.7× possible — both hysteresis bands come out of one walk of the boundary table inside the scoring pass, and moving that work the other way (two separate band lookups in the serial pass) measured 2.6× *slower* overall, which is the sharpest evidence available that the serial tail is what governs here. 0.61 ms for 10^5 entities is comfortably under the 1.6 ms [ADR-0028](../adr/0028-ecs-and-persistent-store.md) records for E6's single-threaded LOD pass, but the ADR's point stands: at that population LOD assignment is a budget item, not a rounding error, and the rate limits exist partly so it does not have to act on everything every tick.

`sim.scheduler.tick` with a job system costs 10–12 µs against 0.97 µs without one, for 64 empty invocations: that is the job submission and counter wait, about 160 ns an invocation, and it is the floor a system's body has to be worth crossing. Systems with real work are the only ones that should declare more than one batch, and a phase of cheap systems is better left to the no-job path.

## What is stubbed

- **The registration table is not yet static.** [ADR-0027](../adr/0027-additive-capabilities.md) describes `SystemDesc` as constant-initialized descriptors in a link-time table; here they are added at runtime with `add_system`. The shape matches field for field (plus `context` and `batches`, because the ADR's sketch assumes a system reaches its module's globals and a testable one has to be handed its state), so the static form is a mechanical change.
- **`SystemDesc::tiers` and `Determinism` are recorded, not enforced.** Nothing filters a system's invocation by tier yet, because nothing here owns the entity-to-tier mapping; the scheduler carries the declarations so that the tick which does can read them.
- **`Batch` is an index range the scheduler does not interpret.** It has no entity store to split, so a system that declares four batches gets four invocations numbered 0–3 and decides itself what they mean. When the scheduler drives an entity store, the batch becomes a row range.
- **No event bus.** [05 §5.7](../plan/05-simulation.md#57-world-events-and-consequences)'s subscribe-by-type-and-tile, causal depth cap, and per-system event budget are not here; the wheel delivers to one sink and the game routes.
- **No sim hash.** [05 §5.10](../plan/05-simulation.md#510-determinism-and-replay)'s persistent-state hash for replay comparison needs the store's projections; `schedule_hash()` covers only the schedule.
- **`reconcile_tile` does no writing back.** It reads the tile and materializes; persisting the summarized state is the caller's, through the `persist` phase.
- **The far list is linear** and the top level's span is about 2.8 game years; see above for when that stops being the right shape.

# sim (domain)

**Purpose.** The simulation core of [05 §5.2–§5.5](../plan/05-simulation.md#52-sim-scheduler). This first change builds the hierarchical timing wheel over `GameTime` and its budgeted fast-forward ([§5.3](../plan/05-simulation.md#53-event-scheduler-temporal-lod)); LOD tier assignment and the tick scheduler follow into the same module. It is the registration point capabilities attach to when they tick, schedule, summarize, or materialize ([ADR-0027](../adr/0027-additive-capabilities.md)), which is why it is **not** an optional capability: a registration point that can be switched off is one nobody can register with.

**Owned data.** The timer slab and its free list, the wheel's buckets and occupancy bits, and the summarizer table. Nothing else may hold a timer's storage; a `TimerHandle` is the only outside reference to one.

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

## Invariants

- Every live timer sits in the bucket its due time and the wheel's current position name, its level is the one `choose_level` would pick, and the occupancy bit of every bucket agrees with whether that bucket's list is empty. `TimingWheel::validate()` checks all of this against a linear recomputation; the cascade test calls it after every one of 2,940 advances.
- A slot's generation is odd while live and even while free, so a handle from a previous occupant of a slot is refused rather than cancelling somebody else's timer.
- `advance` never moves `now` past an undelivered due timer: the skip search jumps only to the start of a bucket that has work, and a partially covered final bucket is checked for a genuinely due entry before it is returned.

## Public API

- `domain/sim/timing_wheel.h`: `TimerHandle`, `TimerPayload`, `TimerEvent`, `EventSink` and `make_sink`, `SummarizeInterval`, `Summarizer`, `TimingWheelConfig`, `FastForwardResult`, `TimingWheel` (`schedule`, `schedule_periodic`, `cancel`, `is_live`, `due_time`, `advance` in both forms, `count_due`, `add_summarizer`, `reset`, `validate`, and the shape accessors).

**Determinism stance ([ADR-0010](../adr/0010-deterministic-sim-and-lod-contract.md)):** `hashed`. Every ordering decision here is made on integers the module itself assigned — due time and insertion sequence — so two runs with the same inputs produce the same bytes on every machine.

**Zero-cost-when-unused ([11 §11.10](../plan/11-performance-principles.md#1110-absent-capabilities-are-free)):** no instances. A wheel with no timers scheduled has 1,229 empty buckets and five zero occupancy words, and `advance` over any interval is a bitmask scan that finds nothing. Nothing is allocated per event in steady state: timers come from a slab with a free list.

**Depends on.** `base`, `containers`, `time`, `math`, `jobs`, `log`, `ids`.

## Testing

`tools/dev.ps1 test -Preset msvc-debug -Filter sim`:

- **Timing wheel**: the level ladder's exact resolutions; due order with ties in insertion order; O(1) cancel and refusal of a stale handle after slot reuse; cascading across all five levels checked with `validate()` after each of 2,940 one-minute advances; a 29-day timer found in a single jump; a periodic firing on its phase (registered at 09:13, first firing at 10:00) and keeping its handle across firings; a timer past the horizon kept in the far list and delivered 2,000 days later; a timer scheduled in the past due immediately; a sink that schedules and cancels mid-delivery; and byte-identical output from two runs of a 4,000-timer schedule with periodics and cancellations, including the same bytes when the same interval is walked in 997 advances instead of one.
- **Fast-forward**: the plan's own case — an hourly economy over a 30-game-day gap delivers 720 events under a large budget, and under a small one makes exactly one summarize call per registered summarizer and fires nothing; an exact summarizer (a counter of game hours) ends at 720 either way; a periodic with no summarizer and a one-shot timer are delivered anyway; a gap under the budget is bit-for-bit an ordinary advance; and counting stops once it passes the budget.

The size table pins `TimingWheel::Slot` at 56 bytes; at 10^6 timers the slab is 56 MB, so a byte there is a megabyte.

## What is stubbed

- **No event bus.** [05 §5.7](../plan/05-simulation.md#57-world-events-and-consequences)'s subscribe-by-type-and-tile, causal depth cap, and per-system event budget are not here; the wheel delivers to one sink and the game routes.
- **The far list is linear** and the top level's span is about 2.8 game years; see above for when that stops being the right shape.

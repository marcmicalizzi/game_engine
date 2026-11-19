# events (domain, capability)

**Purpose.** The world event bus of [05 §5.7](../plan/05-simulation.md#57-world-events-and-consequences): persistent state changes are events and systems react to them rather than polling ([05 §5.1](../plan/05-simulation.md#51-principles)). Events are schema types, raised by the scheduler's systems, by named sources (a flecs system, the timing wheel's sink, the host, a player's input) and by the authoring document's change feed, and delivered to subscribers **inside the scheduler's phases**, in an order that is a function of the world and its inputs and never of the threads, under the runaway guards the plan names. Built 2026-11-17 (roadmap S12); the consumers waiting on it are routine overrides from world events (S53), economy and faction consumers (S54), perception (S55) and the world-mutation guard of [05 §5.9](../plan/05-simulation.md#59-narrative-dependencies-and-story-protection).

**Owned data.** Two banks of 64-byte `EventRecord`s with a payload arena beside each — the *live* bank (what waits for the next delivery point) and the *carry* (what waits for the next tick) — sized once from the config; the registered event types, sources and subscribers; the delivery scratch (sort keys, the delivery order, each generation's admitted events and their counting sort by type), reserved when a type or a subscriber is added; per-tick counters (each source's deliveries, each subscriber's deliveries and wall time, the causal chains' sizes); the tick's `TickReport`, the last one, the totals and the per-type counters; the running delivery hash.

## Where it attaches

`EventBus::install(scheduler)` adds eight systems to the scheduler's table, one per phase, each a `begin_tick` hook and nothing else: no component, no resource, every tier. A phase's `begin_tick` hooks run serially before anything else in it ([sim](sim.md#the-executor-seam)), so the hook marks the phase raises are attributed to, and at the start of `EventsIn` and of `EventsOut` it **delivers**. Nothing in `domain/sim` names this module: the scheduler sees eight table systems, which is ADR-0027's registration point and the reason the capability can be switched off.

- **`EventsIn`** first takes the carry the last tick left (appended to the live bank: it was raised earlier, so it sorts first), then reads the document's change feed, then delivers everything pending: what the last tick's `EventsOut` waves and `Persist` raised, what the host raised between ticks, what the timing wheel's sink raised at the top of this phase.
- **`EventsOut`** delivers what this tick's `Input` to `PostPhysics` raised.

A host with no scheduler (a tool, the tests) calls `deliver(point, tick)` itself.

**Raising.** An `Emitter` carries a source's identity, a 64-bit key and its own sequence: `bus.emitter(context, batch)` in a table system's batch (source the system, key the batch), `bus.hook_emitter(context, end)` in its `begin_tick`/`end_tick`, `bus.emitter(source, key)` for a registered source (`add_source(name, external)`, ids in registration order). An emitter is one thread's — a stack object per invocation — and raising through different emitters is thread-safe: a raise reserves its slot and its payload's words together with one compare-exchange, only when both fit, copies the bytes and counts itself written. A payload is the generated struct itself, copied as bytes, so an event type is a `@kind(event)` struct that is trivially copyable: numbers, ids, vectors, `worldpos`, enums and fixed arrays, never a string or a list. `register_type<T>()` refuses anything else.

**The document's feed.** `attach_document(&doc)` makes the bus raise, at every `EventsIn`, one `engine.events.RecordChanged` (record, revision, its type's `stable_type_id`, live or not) per record whose composition changed since it last looked, in id order, from the source "document" — `doc::Document::changed_since`, the same feed the materialization driver reads ([sim](sim.md#the-driver-a-document-through-the-hooks)). When the feed no longer reaches back it raises one `engine.events.DocumentResynced` instead, and a subscriber that keeps state from the document rescans it. What existed when the document was attached is not news.

## The order

**A delivery point sorts what it delivers by (tick, phase), source, key, sequence**, and the position in the bank last. The first four are the raiser's and a function of the world: the tick and the phase it raised in, which system, source, subscriber or the document raised it, the key it was raised under (a batch, an entity, the cause's id), and how many that emitter had raised before. A parallel wave's batches land in the bank in whatever order the jobs finish; the sort puts them back. The position decides only between two raises with one whole key: two serial raises keep the order they ran in, which is deterministic, and two parallel ones would keep the order they landed in, which is not — so every such tie is counted (`GuardTrips::key_ties`), and a raiser that produces one is giving two emitters one key. Raises that arrive already in order (one source on one thread, the common case) skip the sort: a point checks first, and `TickReport::sorted` says when it sorted.

**Delivery is serial**, subscriber by subscriber in registration order, each hearing its own types in the delivery order, however many types it hears and in whatever order it listed them (a merge over the generation's per-type runs). Serial is the price of a single, stated order; a subscriber with heavy work should do it in its own system and keep its handler to recording.

**Ids.** An event's `EventId` is the tick its place in the order was fixed in and its rank in that tick (`tick << 24 | rank`), so ids grow along the delivery order; `cause` names the event whose handler raised it and `root` its chain's first event. A carried event keeps the id it was given.

**Consequences are the next generation of the same point.** What a handler raises is appended to the live bank at depth + 1, and once every subscriber has heard the generation, the point delivers the consequences, in the order the handlers raised them — already deterministic, since delivery is serial — and so on until a generation raises nothing or the guards stop it.

## The runaway guards

[05 §5.7](../plan/05-simulation.md#57-world-events-and-consequences) names them; each is a count or a key here, never a clock, so which event a guard stops is a function of the order and the same on every run. Each trip is counted in the tick's `TickReport::trips`, added to `total_trips()`, and logged once on the tick a guard starts tripping.

| Guard | Rule | Bound (tunable) |
|---|---|---|
| Depth | A raise past the cap is dropped at the raise and the raise returns false: "a hard cap on depth (drop and log beyond it)". | `max_depth`, 8 (`sim.events.max_depth`) |
| Cascade | Past the cap of consequences (depth one or more) delivered in a tick, the rest go to the carry and are delivered first at the next tick's `EventsIn`, where they count against that tick's cap. | `max_cascade`, 16,384 |
| Storm | Past the cap of one source's events delivered in a tick, that source's rest go to the carry; other sources are not held up. The plan's "per-tick event budget per system with deferral to the next tick". | `max_per_source`, 65,536 |
| Self-loop | A subscriber's raise of a type it hears goes to the carry, so it reaches the subscriber a tick later: "no zero-delay self-loops". | — |
| Budget | Past its deliveries in a tick, a subscriber's events are skipped for it — others still hear them — and its `overflow` hook is told how many at the end of the point: its place to coarsen, as a summarizer coarsens a fast-forward. | per subscriber, or `subscriber_budget`, 2^20 |
| Full | Between delivery points the live bank keeps **the longest run of the order that fits** its records and its arena, out of everything raised since the last point, and refuses the rest: a raise past that run returns false, and a raise the bank had taken but a lower one displaced is refused at the point. Where a parallel wave lands first decides nothing (below). Inside a point, where raises are serial, a raise that finds the live bank full is refused; a carry that finds the carry full is dropped. All counted (`full_dropped`); a carry's per type in `type_counters()`. | `capacity` 65,536 records, `payload_kib` 4,096 |
| Slow | A subscriber whose handlers took longer than the threshold in a tick is counted and the slowest named. **Reported, never acted on**: a guard that skipped a slow subscriber would make the simulation a function of the machine. The budget above is the deterministic half. | `slow_subscriber_us`, 2,000 |

**A full bank.** The first raise that does not fit closes the lock-free path (a mark in the counters' word), waits for the raises it had already let in to finish writing, and from then every raise until the next delivery point takes a mutex and goes through a *selection*: a heap of the kept slots by the order (tick, phase, source, key, sequence, then type, tile and a hash of the payload's bytes, so even two raises with one whole key are told apart by what they carry), and a *cut*, the lowest raise turned away. A raise at or past the cut is refused at once; a lower one evicts the highest kept until it fits or the highest kept is lower than it, in which case it is the new cut. What is kept is then the longest run of the whole interval's raises, in that order, that fits — the run a serial raiser in key order would have filled — on every run, at every thread count and in every arrival order, and the refusals are the rest of the raises, so their count is the same too (B5, 2026-11-18: the bank used to keep whichever raises reached its counters first). An evicted slot leaves a hole, reused in place by a payload it has room for (a list of holes per power of two of their words); a payload no hole has room for takes an evicted slot or one past the last, and words past the last, and when the arena's end is reached the kept payloads are compacted in offset order first (a sort of the kept slots, so `O(n log n)`, and only when the holes cannot place what arrives). At the delivery point the kept records are compacted to the front and the bank opens again, so a carry appended there finds the same room on every run.

**The counters cannot wrap.** A bank's record count and its arena's words share one 64-bit word, and a raise moves them only when both fit, so neither passes its bound: the records are at most `capacity` (a `u32`) and the words at most `payload_bytes / 8`, under 2^29, in a 31-bit field. A refusal moves nothing. (B6, 2026-11-18: the arena's counter used to take every raise's bytes, refused or not, and a storm of about 2^32 refused bytes in one interval wrapped it to a small offset that the next raise was copied to, over payloads already raised.)

**Nothing carried is lost while the carry has room.** A storm that raises past its cap every tick fills the carry in the end, and from then its overflow is dropped and counted — the stated end of a stated rule, not a silent one. A cascade carried tick after tick is delivered at the cap's rate, in order. The plan's other two measures are the consumers' to build on this: consequence-driven state variables that saturate and decay toward a baseline, and conservation checks (population, currency) as test invariants.

## Telemetry

The plan asks for event rates per type, depth histograms and top causal chains, exposed to agents for balancing. Per tick, `current_report()` (and `last_report()`, complete): events raised, delivered and the handler calls, what came in and went out on the carry, how many points sorted, the depth histogram (16 buckets, the last holding everything past it) and the largest depth, every guard's trips, the four largest chains of the tick by consequences delivered (ties by root id; a tick with more roots than `chain_slots` counts the excess as untracked), and the slowest subscriber with its microseconds. Since the bus was made: `total_trips()` and `type_counters()` (raised, delivered, dropped, carried per type), from which a rate is a difference over ticks. Not yet served over the protocol: that is a method in the host that owns the bus, below.

## Events and the replay

The input log replays a run bit for bit today ([input](input.md), [05 §5.10](../plan/05-simulation.md#510-determinism-and-replay)), and the bus is built not to break that. **An event the simulation raised is not an input**: given the same world, the same input and the same document commands, the same systems raise the same events under the same keys, so the same deliveries follow and nothing about them needs recording. `delivery_hash()` is the check — a running hash over every delivery (the event's id, type, source, key, sequence, depth and payload bytes, and which subscriber heard it; integer word mixes only, so every toolchain computes the same value) — and the ordering test holds it equal at no workers, one and eight.

**What does need recording is what the simulation did not compute**: a raise from a source registered `external` (a player, a network peer, a scripted agent). Each one is handed to the `ExternalLog` hook as it is raised — the tick, the phase, the source, key and sequence, the type's stable id and the payload's bytes — and `replay_external` raises a recorded one again at the same tick, which is the same key and so the same place in the order. The replay test runs a world of a system, a reacting subscriber and a player's raises for forty ticks, replays it from the log alone to the same delivery hash and the same state, and shows that leaving one raise out changes the hash.

**`ExternalRecording`** (roadmap T53, 2026-11-18) is that hook's other end for a recorded run: it copies every external raise as it is raised into one arena in raise order, writes them to a file (`ENGEXT01`, little-endian, fixed-width entries, each checked against the file's size and order on read) that a `sim::RunRecord` names beside its input log ([sim](sim.md#the-hash-log-and-the-bisector)), reads them back, and `replay_tick(bus, tick)` raises a tick's again before that tick is stepped. The test records forty ticks through it, writes and reads the file, and replays to the same delivery hash and state; a file cut short is refused whole.

That is S18's open remainder seen from this side: S18 is "input recorded into the event log as `Player`-origin events and read by a game's systems in the `Input` phase" and replay of a run split off the write-back cadence. The bus now gives it the seam (the external source, the log hook, the replay call); what S18 still owns is writing those raises into the store's event log beside the input log, reading them back in a replayed session, and the split cadence.

## LOD policy

The bus has no tier of its own and delivers at every tier: its eight systems declare all eight, and under an empty active-tier mask ([sim](sim.md#the-static-table-and-tier-filtering)) delivery waits with everything else and loses nothing. **An event carries the tile it happened in** (`RaiseOptions::tile`; `k_any_tile` for one that happened nowhere in particular), and a subscriber that cares filters with `wants_tile` — the plan's "consumers subscribe by event type and tile" — so a consumer that runs only near the observers hears only what happened there. A summarized (LOD2/LOD3) interval raises no events: the summarizer contract's condition 4 ([sim](sim.md#the-contract-every-lod2lod3-system-must-meet)) is what keeps a fast-forward from becoming a cascade, and the bus does not relax it.

## Determinism

`hashed` ([ADR-0010](../adr/0010-deterministic-sim-and-lod-contract.md)). The order is integers the raisers and the bus assign; the guards are counts over that order; the payload is copied as bytes; no clock, thread count or arrival order reaches a delivery except through a counted tie. That includes a bank a parallel wave overfills: what it keeps is the longest run of the order that fits, decided over every raise of the interval, not the raises that reached its counters first ("A full bank", above). The one clock read — a subscriber's wall time — is `derived`: reported, never read back.

## Invariants

- A point's delivery order is sorted by (tick, phase, source, key, sequence, bank position), and a tie on the first five is counted.
- Every event in the live bank at the end of a point was delivered or moved to the carry; the live bank is empty after a point.
- A carried event is delivered no earlier than the next tick's `EventsIn`, and keeps its id.
- An event's depth is its cause's plus one; a root's `root` is its own id.
- No allocation in a steady tick: everything that grows is sized at construction, registration or a tick's first raise of a new system (the guard test counts the engine's allocations over twenty ticks of 8,000 events and finds none).
- `EventRecord` has no indeterminate bytes: it has no padding at all (the payload's size is its type's, not a field), because records are hashed.

## Public API

`domain/events/events.h`: `k_any_tile`, `EventId`, `Origin`, `make_source`/`source_origin`/`source_index`, `EventRecord`, `EventType<T>`, `Event` (`as`), `RaiseOptions`, `Emitter` (`raise`, `raise_bytes`), `SubscriberDesc`, `EventBusConfig` and `config_from_tunables`, `GuardTrips`, `ChainCount`, `TickReport`, `TypeCounters`, `ExternalRaise`, `ExternalLog`, `ExternalRecording` (`hook`, `record`, `clear`, `size`, `at`, `replay_tick`, `rewind`, `save`, `load`), `EventBus` (`register_type`, `add_source`, `subscribe`, `attach_document`, `set_external_log`, `install`, `emitter`, `hook_emitter`, `replay_external`, `deliver`, `enter_phase`, `current_report`, `last_report`, `total_trips`, `type_counters`, `delivery_hash`, `pending`, `carried`, `delivering`). Schema: `domain/events/schemas/events.schema` (`engine.events.RecordChanged`, `DocumentResynced`). Tunables: `sim.events.capacity`, `payload_kib`, `max_depth`, `max_cascade`, `max_per_source`, `subscriber_budget`, `slow_subscriber_us`, `chain_slots`.

## Testing

`tools/dev.ps1 test -Preset msvc-debug -Filter events`:

- **Order** (`tests/events_tests.cpp`): a table system of eight batches raising 40–44 events each for six ticks is heard in the same order with the same ids and the same delivery hash at no workers, one and eight, by tick, then batch, then sequence, and the serial run needed no sort; a Lod system registered after a Systems one is still heard first, and a host's raise between ticks first of all, at the next `EventsIn`; a handler's raises come after their causes with depth, cause and root set; a subscriber of two types (listed out of order, one twice) hears them interleaved in the order, without a third type or an odd tile.
- **The document's feed**: nothing at attachment; a creation, another and a deletion heard as three `RecordChanged` in id order at `EventsIn`, live or not with the type's id; nothing on a tick with no change.
- **The replay**: forty ticks of a world with a player's raises replay from the external log to the same delivery hash and state, and not with one raise left out.
- **The guards** (`tests/guard_tests.cpp`): a chain bounced between two subscribers stops at depth 3 with the raise at 4 dropped and both chains in the top list; forty consequences under a cascade cap of ten arrive ten a tick over four ticks with none lost; a storm of 250 under a cap of 100 arrives 100, 100, 51 (with one more raised mid-tick) while a calm source's five arrive at once; a subscriber's own type reaches it one tick later, five ticks for five; a budget of five skips seven and says so once while another subscriber hears twelve; a bank of sixteen refuses four of twenty, and an arena of 64 bytes takes two 32-byte payloads; **a wave that overfills the bank** — eight batches of 2,000 Blasts and Pings in turn, into an arena that holds about a third of them and, separately, a bank of 2,300 records — keeps exactly the run of the order that fits and delivers the same events, refuses the same count and ends with the same delivery hash with no job system, one worker and eight, each in four submission orders (it failed on b74e9c6e, which kept what arrived first); a storm of 200,000 refusals at a full arena leaves both counters where the last accepted raise put them and the two payloads already raised intact; a 3 ms handler is reported slow and both subscribers still hear the event; two emitters with one key count a tie; and a scheduler tick of 4,000 raises from four batches and 4,000 consequences allocates nothing over twenty ticks.

The size table pins `EventRecord` at 64 bytes, `TickReport` at 208 and four others.

## Performance

Measured with `tools/dev.ps1 bench -Preset msvc-release -Filter events.*`; one iteration is one tick's raises and the delivery point that delivers them, per event.

**Machine state: not quiet, so every figure is an upper bound.** 2026-11-18, i9-10980XE, `msvc-release`, seven repeats; `--wait-quiet=60` gave up with other processes at 87% of the CPU, the Windows session locked and the GPU lock held by another agent's flights. The spreads were 1–8%.

| Benchmark | 10^3 events a tick | 10^5 events a tick | What it covers |
|---|---|---|---|
| `events.deliver.ordered` | **97 µs** (97 ns an event) | **10.5 ms** (105 ns) | one source raising in order, one subscriber: no sort |
| `events.deliver.shuffled` | **148 µs** (148 ns) | **19.4 ms** (194 ns) | the same raises from emitters whose keys arrive backwards: the sort |
| `events.deliver.fanout` | **133 µs** (133 ns) | **16.2 ms** (162 ns) | in order, four subscribers |
| `events.deliver.unhashed` | **70 µs** (70 ns) | **8.7 ms** (87 ns) | `ordered` with the delivery hash off |

What the rows say:

1. **An event costs about a hundred nanoseconds from raise to handler** on a loaded machine: a compare-exchange, an atomic add and a copy to raise it, a key, an id, the guards' counters, a counting sort by type and an indirect call to deliver it. At [05 §5.6](../plan/05-simulation.md#56-what-npc-scale-is-realistic)'s operating point — a few thousand events a second, under a hundred a tick — that is microseconds a tick; 10^3 a tick is a tenth of a millisecond, and 10^5 a tick is a stress case at a sixth of a 60 Hz tick.
2. **The delivery hash was half of it and is now a quarter.** The first version mixed every field through a full 64-bit finalizer per field (`hash_combine`) and ran 134 ns an event against 70 without the hash; a chain of one-multiply word mixes, with the payload read only to its type's size, is 97. It stays on by default because it is what a replay is checked against; a host that does not check replays can turn it off (`EventBusConfig::hash_deliveries`).
3. **The sort is paid only when raises arrive out of order**, and at 10^5 it adds about 90 ns an event — `O(n log n)` over 32-byte keys. A parallel wave whose batches finish out of order pays it; one source raising in order never does.
4. **Each further subscriber of a type costs about 12 ns a delivery** (four subscribers 133 against one 97 at 10^3).

**The full-bank mend (B5, B6) left the steady path where it was**: the raise's two atomic adds became one compare-exchange and one add, and nothing else on the path changed. Before and after on the same binary's bench, 2026-11-18, seven repeats, both loaded (other processes at 84% and 48% of the CPU, the session locked: upper bounds): `ordered` 72 and 72 µs at 10^3, 8.3 and 8.5 ms at 10^5 (within the 4% spread); `shuffled` 104 and 105 µs, 22.1 and 14.0 ms (the before run's spread was 11%); `fanout` 100 and 114 µs, 10.8 and 11.6 ms (spreads 3% and 7%). A full bank's selection is off this path and not benched: it runs only once a bank is full, and the guard test above takes it through 16,000 raises a run.

How it scales down: the banks are what it holds — 64 bytes a record and the arena, two banks of 65,536 by default, 8 MiB of records and 8 MiB of payload — and a smaller machine sets `sim.events.capacity` and `payload_kib` to the event rate it expects; the cost an event is cache-bound at 10^5 and scales with memory latency rather than cores, since delivery is serial.

## What is left open

- **No consumer yet.** S53 (routine overrides), S54 (economy and factions), S55 (perception) and the world-mutation guard of 05 §5.9 are the bus's first users and are their rows' work.
- **Not served over the protocol.** The report and the counters are a C++ API; a `session.events` beside engine-host's existing store events is the host's row.
- **The store's event log** records write-backs; the bus's external raises are handed to a hook and not yet written there — S18.
- **Parallel delivery.** Delivery is serial. Subscribers that declared which resources their handlers touch could be delivered to in waves, as systems are; nothing needs it at today's rates.
- **Not installed in a host.** engine-host's runtime world and engine-view do not make a bus yet: a host makes one when its first consumer arrives.

## Capability contract (ADR-0027)

One module in `domain/`, `OPTIONAL` (`ENGINE_WITH_EVENTS`), depending on `sim` and `doc` (neither a capability) and nothing above them. Registration points: schema types (`events.schema`, `engine_schema_library(... CAPABILITY events)`), systems in the scheduler's table (`install`), tunables (`sim.events.*`). No render pass, no derived data, no protocol method yet. LOD policy and determinism stance above. **Zero cost when unused: no instances** — a host that makes no bus has nothing of it, and a build without the module has no code from it; a bus with nothing raised costs each delivery point a load of two counters.

## Choices with costs either way

- **Serial delivery in one stated order**, rather than handlers run in parallel: costs the parallelism a heavy consumer could have had, buys an order every run and every machine agrees on without a second sort. A heavy consumer records in its handler and works in its own system.
- **The slow-subscriber guard reports and does not act**, and the deterministic budget (a count) is what skips: costs the ability to shed a pathological handler by time, buys a simulation that is not a function of the machine it runs on.
- **Overflow is carried to the next tick, not dropped**, for cascades, storms and self-loops: costs carry-bank room and a tick's latency, buys nothing lost until the carry is full, at which point drops are counted.
- **A sort key the raiser carries**, rather than per-thread staging buffers merged at the point: costs a 32-byte key per event and an `O(n log n)` sort when arrivals are out of order (none when they are not), buys raising from any thread, any executor (flecs' workers included) with one compare-exchange and one atomic add.
- **A full bank selects the lowest keys under a mutex**, rather than refusing whatever arrives once it is full: costs, only once the bank is full, a lock, a heap operation and sometimes a compaction a raise (the worst case, a storm of mixed payload sizes whose keys keep arriving below the kept and whose holes never fit what comes next, compacts `O(n log n)` a raise), and one more atomic add on every raise to count it written; buys a full bank whose contents are a function of the raises, so the delivery hash a replay is checked against holds even in a storm. The other ways cost more where it matters: per-thread staging merged at the point holds every raise of a wave however many there are, and a bound per source does not bound the bank.

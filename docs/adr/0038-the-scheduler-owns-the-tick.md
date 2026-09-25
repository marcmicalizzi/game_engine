# ADR-0038: The engine's scheduler owns the tick; flecs runs each phase's systems inside it

- **Status:** Proposed
- **Date:** 2026-09-24
- **Plan references:** docs/plan/05-simulation.md §5.2 (the sim scheduler: phases, fixed step), §5.3 (the timing wheel), §5.4 (LOD tiers), §5.10 (determinism and replay); docs/plan/03-data-model.md §3.4 (the runtime world, the materialization contract). Closes [ADR-0028](0028-ecs-and-persistent-store.md) decision 7 if accepted. Builds on [ADR-0027](0027-additive-capabilities.md) (the registration table), [ADR-0030](0030-flecs-workers-are-long-running-pool-jobs.md) (flecs' workers on the pool) and [ADR-0010](0010-deterministic-sim-and-lod-contract.md).
- **Docs touched:** [sim](../subsystems/sim.md), [ecs](../subsystems/ecs.md), [protocol](../subsystems/protocol.md), [animation](../subsystems/animation.md), [audio_system](../subsystems/audio_system.md), [kinematics](../subsystems/kinematics.md), [05 §5.2](../plan/05-simulation.md#52-sim-scheduler), [03 §3.4](../plan/03-data-model.md#34-the-runtime-world)

## Context

Two things could run the simulation's tick, and until now both did, for different callers.

`sim::SimScheduler` (`domain/sim`) was built first, to [05 §5.2](../plan/05-simulation.md#52-sim-scheduler)'s letter: the eight phases in order on a fixed step from `core/time`, a table of `SystemDesc`s with read/write sets and a wave schedule that is a pure function of the registration, the timing wheel pumped at the top of `EventsIn`, tier assignment, the materialization hooks, tile reconciliation, and a persistence hook at the end of `Persist`. `ecs::SimWorld::step()` (`domain/ecs`) runs flecs' own pipeline over the same eight phases, as phase entities chained after `PostFrame`, with a clock of its own. Every capability that ticks today — `animation`, `audio_system` — registers through `ecs::register_system`, which records its `SystemDesc` in `ecs::SystemRegistry` and hands its body to flecs; so the systems ran on flecs' clock, and the wheel, the tiers and the hooks, which live in the scheduler, were not driven by anything that ticked. [ADR-0028](0028-ecs-and-persistent-store.md) decision 7 left open which of the two owns the tick, on purpose, until the scheduler existed and something needed both.

Materializing a document into the runtime world ([03 §3.4](../plan/03-data-model.md#34-the-runtime-world); `sim::Materializer`, `ecs::RecordMaterializer`) is that something. It walks records through the scheduler's hooks table, and it writes back at a declared phase through a system in the scheduler's own table — while the systems that change what it writes back are flecs'. A host that wants both has to pick one clock.

The alternatives:

1. **The scheduler owns the tick; flecs runs each phase's systems inside it.** The scheduler advances the clock, publishes the tick and game time to the world, and in each phase runs that phase's flecs systems through a per-phase flecs pipeline (`ecs_run_pipeline`) before its own table's waves.
2. **flecs' pipeline owns the tick; the scheduler is a phase.** `SimWorld::step()` stays the tick, and the scheduler's work — the wheel, the tiers, the hooks, its table — is wrapped in flecs systems, one per phase, that call into it.
3. **Both, side by side.** Each world keeps its own executor, and a host that needs the scheduler's machinery calls it around `SimWorld::step()`.
4. **The scheduler owns the tick and runs flecs' systems itself.** flecs' pipeline is not used at all; every flecs system is a row in the scheduler's table and is run by `ecs_run`/`ecs_run_worker` inside the scheduler's waves, on its jobs.

## Decision

**Option 1.** `sim::SimScheduler` owns the tick: the clock, the fixed step, the phase order, the timing wheel, the tiers, the materialization hooks and its own table. flecs' systems run inside its phases, through `ecs::ScheduledTick`:

- The scheduler has one executor seam, `sim::TickExecutor` — three function pointers and a context, called at the start of every tick, once in each phase, and at its end — and knows nothing about what is behind it, so `domain/sim` stays free of flecs (ADR-0028 seam 2's dependency direction).
- `ecs::ScheduledTick` installs itself as the executor. At the start of a tick it publishes the scheduler's tick and game time to the world (`SimWorld::sync_clock`) and opens a flecs frame; in each phase it runs a flecs pipeline that selects that phase's systems, in creation order, which is the order `SimWorld::step()` runs them in within a phase — skipping a phase that has none; at the end it closes the frame and runs the table watchdog. flecs' worker hosting (ADR-0030), its sync points, its merges and its deferred-command rules are unchanged, because it is still flecs' pipeline code running the systems, one phase at a time.
- **Within a phase, flecs' systems run before the table's waves.** The table today holds engine systems that consume what capabilities' systems wrote in the same phase — the materialization write-back at `Persist` is the first — and none that produces for them. A phase is therefore: (at `EventsIn`, the wheel), begin-tick hooks, flecs' systems, the table's waves, end-tick hooks, (at `Persist`, the persist hook).
- **One clock, one executor.** A world a `ScheduledTick` drives refuses `SimWorld::step()` and `advance()` (an assertion), and the two must be constructed with the same step. `SimWorld::step()` remains flecs' pipeline for a world nobody schedules — engine-view's animated world and the ECS tests still use it — and is retired in the change that moves engine-view over, if this ADR is accepted.
- No capability changes: a system still registers with `ecs::register_system` and names its phase with `.kind(phase)`. `animation`, `audio_system` and `kinematics` each have a test that ticks them under both executors and compares the output byte for byte (animation's matrices and playheads, the commands audio sends, kinematics' transforms).

engine-host's runtime world (`session.run_headless`, `session.materialize`) is the first host built this way.

**If accepted, ADR-0028 decision 7 is closed**: the engine's scheduler owns the tick, and flecs' pipeline is its in-phase executor for flecs' systems.

## Why

- **One clock.** The fixed step, the tick number and game time are the scheduler's and are published to flecs, rather than two clocks that have to agree. `SimWorld` had its own `FixedStepClock` and `GameClock`; a host that also drove a `SimScheduler` would have had two tick counters.
- **The timing wheel and the LOD tiers already live in the scheduler.** The wheel is pumped at the top of `EventsIn` so a phase's systems see this tick's events and nothing arrives mid-phase; tier assignment and the materialization hooks feed the systems that run after them. Under option 2 each of those would become a flecs system calling back into the scheduler, and their ordering against capabilities' systems would be a question of flecs entity creation order rather than of the phase list.
- **Replay determinism needs one executor.** [05 §5.10](../plan/05-simulation.md#510-determinism-and-replay) wants an input log plus an event log to replay to the same persistent-state hash. With two executors a replay would have to reproduce the interleaving of two schedules; with one, the order is the scheduler's phase order and, within a phase, flecs' creation order — both functions of the registration. The tests above show the switch itself changes no bytes.
- **The declarations are already the scheduler's.** ADR-0028 seam 2 put every system's `SystemDesc` in a table the engine owns precisely so the executor could change without touching a system's data access; this is the first half of that change, and costs no system anything.

**Why not option 4, yet.** Moving flecs' systems into the scheduler's waves is where the two real limits of flecs' pipeline would be addressed — it splits a system's rows evenly instead of letting the pool steal into the tick, and hierarchy propagation cannot be `multi_threaded()` under its row-splitting (ADR-0028's consequences; [ecs](../subsystems/ecs.md)). But it means running flecs systems from job threads on flecs stages, with the merge after each wave the engine's to get right, and giving up flecs' measured worker hosting (ADR-0030) before anything has been measured against it. Option 1 is the step that settles who owns the clock, costs nothing measurable per phase that has no systems (a cached query's `is_true`), and leaves option 4 as a change inside `ScheduledTick` alone: `SystemRegistry` is already the table it would read.

**Why not option 2.** It keeps flecs' clock and makes the engine's own machinery a guest in it: the wheel, the tiers and the hooks would each become a flecs system, the scheduler's table would have no executor of its own, and a system that needs a phase-boundary guarantee (the write-back must see every write of the tick) would depend on flecs' creation order instead of a declared phase. It also forecloses option 4.

**Why not option 3.** It is the state this replaces, and the reason it has to go is the materialization driver: the hooks and the write-back are the scheduler's, the systems whose changes are written back are flecs', and a host holding both clocks would have to keep them in step by hand.

## Consequences

- A host that owns a world constructs a `sim::SimScheduler`, a `ecs::SimWorld` with the same `hz`, installs its capabilities, and then an `ecs::ScheduledTick`; it steps the scheduler. engine-host does; engine-view does not yet.
- The scheduler's table is now a live executor beside flecs': the materialization write-back (`sim.write_back`, `Persist`) is the first system in it. Anything added there runs after flecs' systems of its phase.
- flecs' `OnStart` systems are not run under the scheduler (nothing in the engine declares one), and flecs' own `world_time_total`/`frame_count` advance only through the frames `ScheduledTick` opens — which it does once per tick, so they agree.
- Cost: one `ecs_run_pipeline` per phase that has flecs systems instead of one `ecs_progress` for all eight. Each carries flecs' pipeline update check and, with workers, one wake and one join of them; an empty phase is one cached-query test. Measured on E6's four-system world (100,000 entities, systems in two phases, `Systems` and `Lod`) as `ecs.tick.four_systems.scheduled` against `ecs.tick.four_systems`, i9-10980XE, release, 2026-09-24, two pairs taken back to back in opposite orders (machine state: others' CPU 10–20% for the first pair; the second pair's scheduled run quiet and its `step()` run ending at 10.8%):

  | Workers | `step()` | scheduled | Difference |
  |---|---|---|---|
  | 1 | 2.461 / 2.503 ms | 2.493 / 2.466 ms | +1.3% / −1.5%: none resolvable |
  | 4 | 1.402 / 1.331 ms | 1.333 / 1.274 ms | −5% / −4%, inside both runs' spread (7.5–9.3%) |
  | 16 | 1.194 / 1.040 ms | 1.231 / 1.128 ms | **+3% / +8%**, the one row whose sign held |

  At one and four workers owning the tick costs nothing this machine can see. At sixteen it costs 37–88 µs a tick, about what one more wake-and-join of sixteen workers costs: every `ecs_run_pipeline` wakes the workers and waits for them, this world's systems are in two phases, and `ecs_progress` ran both in one pipeline (a likely reading, not a profiled one). It grows with the number of phases that hold multi-threaded systems, not with the entities; a world that spreads threaded systems over more phases pays it per phase. A third pass, run beside an agent's test suite at 32–39% others' CPU, read the scheduled rows 7–17% slow against a `step()` run taken minutes earlier under less load, and is left out for that reason.
- Forbidden once accepted: a second executor stepping a scheduled world, a capability that ticks outside `ecs::register_system`, and a clock other than the scheduler's published into a scheduled world.

## Revisit when

- The per-phase pipeline cost matters. It is measured (Consequences: nothing at one and four workers, 37–88 µs a tick at sixteen for a world with threaded systems in two phases); it would matter when a world holds threaded systems in most of the eight phases at a high worker count, which is also when option 4 — one dispatch per wave from the engine's own pool — starts paying for itself.
- A capability needs work stealing into a phase, or multi-threaded hierarchy propagation, badly enough to take on option 4.
- engine-view moves to the scheduler (then `SimWorld::step()` can be retired or kept only for tests).
- The scheduler's table grows a system that has to run *before* flecs' systems of its phase — the in-phase order above is then a declaration the table needs, not a rule.

# ADR-0030: flecs' workers are long-running jobs on the performance pool, inside a budget the application sets

- **Status:** Accepted
- **Date:** 2026-09-18
- **Plan references:** docs/plan/05-simulation.md §5.2, docs/plan/11-performance-principles.md §11.5, docs/plan/10-roadmap-risks.md §10.5 (E6), docs/experiments/e6-ecs-store.md. Refines [ADR-0028](0028-ecs-and-persistent-store.md) decision 3 and the paragraph of its consequences that describes worker hosting. Supersedes nothing: ADR-0028 is Accepted and stands, including its decision 7, which this makes no less open.
- **Docs touched:** [docs/subsystems/ecs.md](../subsystems/ecs.md), [docs/experiments/e6-ecs-store.md](../experiments/e6-ecs-store.md), [docs/plan/10-roadmap-risks.md §10.5](../plan/10-roadmap-risks.md#105-experiments-to-run-before-committing)

## Context

[ADR-0028](0028-ecs-and-persistent-store.md) decision 3 says `domain/ecs` provides "an adapter that runs flecs' workers on `core/jobs`' performance pool through `ecs_set_task_threads` and the `ecs_os_api_t` task hooks", and its consequences record that this was then measured to cost **a flat 1.6–2.2 ms a tick** on E6's 100,000-entity world — enough to make every worker count slower than one worker, while flecs' own OS threads on the same world scaled 1.87× at four workers and 2.32× at eight. The ADR named the cost, named the benchmarks that reproduce it, and left the fix open; [ecs](../subsystems/ecs.md) listed three candidates.

The mechanism is now measured rather than guessed ([E6](../experiments/e6-ecs-store.md#why-per-tick-hosting-cost-what-it-did-2026-09-18)), and it is entirely a property of the **per-tick** form:

1. `ecs_set_task_threads` makes flecs create the whole worker set inside every `ecs_progress` and join it again at the end. The main thread then spins in `flecs_wait_for_workers` — a bare lock/check/unlock loop on one critical section — until every new worker has announced itself. Counted: **5,400 to 50,000 critical-section round trips a tick**, against **4 to 18 in total** for a tick with `ecs_set_threads`, and **0.32 ms (two workers) to 2.5 ms (sixteen)** of main-thread time a tick spent outside the pipeline, against 4–6 µs.
2. Every tick's worker is a fresh job, and the pool worker that ran the previous tick's has gone to sleep on the pool's eventcount in between. So every tick pays a wake per worker, and every single-threaded stretch of a tick leaves a pinned core idle mid-tick.

Neither is `core/jobs`': submitting and joining the same number of empty jobs on the same pool costs 1.16–5.7 µs (`ecs.task.roundtrip`), three orders of magnitude below the cost, and the pool's sleep policy is the right one for the short jobs it exists for. What is wrong is asking the pool to re-dispatch a *long-running* worker every tick.

## Decision

1. **A world's flecs workers are long-running jobs on `core/jobs`' performance pool.** `ecs::JobOsApi` fills in flecs' `thread_new_`/`thread_join_` hooks and `ecs::set_workers` uses `ecs_set_threads`, so a world with *n* workers dispatches *n-1* jobs **once**, each of which runs flecs' worker loop until the world gives its workers back. flecs keeps its own per-tick signalling, which is two condition-variable broadcasts and two waits for the E6 world, and the engine keeps one pinned thread per CPU with no second thread set.

2. **A flecs worker occupies a pool worker for the world's lifetime, and how many may be occupied is the application's decision.** `JobOsApiConfig::hosted_worker_budget` is the number of performance-pool workers that may be lent to flecs worlds at one time; it defaults to **all but one**, so the pool always keeps a worker for everything else, and an application that needs more of its pool back says so. `JobOsApi::hosted_workers()` and `pool_workers_kept()` are the readings.

3. **A count the budget cannot give is refused, never attempted.** `ecs::set_workers` returns false, logs what was asked for against the budget, and leaves the world single-threaded but ticking. This is not a nicety: a flecs worker with no thread to run on does not make the tick slow, it makes `flecs_wait_for_workers` spin forever on a worker that will never announce itself.

4. **Workers are given back deterministically.** A world gives its workers back when it is dropped to one worker, when it is reconfigured, and in `SimWorld`'s destructor — which is required anyway, because flecs' own `ecs_fini` does not join a world's workers, it asserts that they are gone. If the adapter is destroyed while a world still holds workers, the adapter takes them back and says so in the log, leaving that world ticking single-threaded; the alternative is a pool thread parked inside flecs that the job system's destructor cannot join.

5. **The per-tick hosting stays selectable, as `WorkerHosting::Tasks` and the `ecs.tick.four_systems.tasks` benchmark**, because the finding is a comparison and a comparison nobody can re-run is a paragraph rather than evidence. It is not a supported configuration for a game.

## Consequences

The tick scales. On E6's world, four systems over 100,000 entities go from 2.42 ms single-threaded to 1.25 ms at four workers and 1.02 ms at sixteen, within a few percent of flecs' own OS threads at every count, and — unlike the per-tick form — the curve never turns back up. The numbers and the machine state are in [E6](../experiments/e6-ecs-store.md#worker-hosting-re-measured-2026-09-18).

What was a per-tick cost becomes a standing one, and it is smaller but never zero. A world with *n* workers holds *n-1* performance-pool threads from `set_workers` until teardown, whether or not it is ticking, so a frame's rendering and streaming jobs share what the budget kept back. Two live worlds share one budget, which is a behaviour change: a test or a tool that stood up two worlds and gave each the whole pool now has the second one refused. The remedy is to size the budget, or to hold one world at a time.

**ADR-0028 decision 3 is now wrong in one clause and right in the rest.** The mechanism it names — `ecs_set_task_threads` and the task hooks — is replaced by `ecs_set_threads` and the thread hooks. Everything else about it stands: the workers are still the engine's pinned pool threads, there is still no second thread set, and the reason for hosting them at all ([11 §11.5](../plan/11-performance-principles.md#115-threads-cores-and-memory-locality)) is unchanged. ADR-0028 is Accepted and therefore not edited; this ADR is where the correction lives, and [ecs](../subsystems/ecs.md) says so where a reader of the module will find it.

**ADR-0028 decision 7 is unaffected and still open.** Whether flecs' pipeline stays the tick scheduler is a separate question, and this decision neither settles nor prejudices it: it makes flecs' pipeline cost what flecs' pipeline costs, which is the honest baseline any replacement should be measured against. The two limits decision 7 rests on are untouched — hierarchy propagation still cannot be `multi_threaded()` under flecs' row-splitting model, and flecs still splits a system's matched entities evenly rather than letting the pool steal into the tick.

What is now forbidden: hosting a world's workers as per-tick tasks in anything but the comparison benchmark; calling `ecs_set_threads` or `ecs_set_task_threads` directly instead of `ecs::set_workers`, which is what performs the admission test; destroying a `flecs::world` that still holds workers; and scheduling long-running work on the performance pool beside a ticking world, which was already the rule and now has a number attached to how much of the pool is left.

## Revisit when

A world needs more workers than a budget that also leaves the pool usable can give, which is the point at which the right answer is probably decision 7's rather than a bigger budget; a profile shows the pool's remaining workers starved by a world that holds most of them across a frame; flecs changes its worker protocol so that a per-tick worker set is no longer a create-and-join (the two mechanisms above are both flecs-version-specific and both were measured against 4.1.6); the engine's own scheduler lands and drives `SystemRegistry` directly, at which point flecs' workers stop existing and this ADR goes with them; or a target platform's thread scheduler makes a pinned, parked pool thread more expensive than an unpinned one, which would reopen "flecs' own threads, and size the pool around them".

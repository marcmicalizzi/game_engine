# jobs (core)

**Purpose.** The topology-aware job system (docs/plan/11-performance-principles.md §11.5, ADR-0011). Pinned worker threads grouped into pools by efficiency class, per-worker work-stealing deques, cache-domain-local stealing, and counters for completion.

**Owned data.** Worker threads, their deques, the per-pool inboxes, and execution statistics.

**Model.**
- `Pool::Performance`: one worker per performance-class logical CPU minus one reserved for the main thread. Simulation and rendering work.
- `Pool::Efficiency`: one worker per efficiency-class logical CPU. Streaming, decompression, audio mixing, background inference. On machines without efficiency cores a small pool (a quarter of the performance CPUs, at least one) runs on performance CPUs at below-normal OS priority, so callers never special-case the topology.
- A `Job` is `{fn, data, counter}`, 24 bytes, trivially copyable; no allocation per job. `Counter` is one cache line.
- A worker pops LIFO from its own deque, then its pool's inbox, then steals FIFO from workers in its cache domain (random start), then from the rest of its pool. Pools never steal from each other.
- Submitting from a worker of the target pool pushes to that worker's deque; from anywhere else, to the pool's inbox. When both are full the submitter runs the job inline (backpressure).
- Idle workers spin briefly, then sleep on an eventcount (`epoch` + `sleepers`); submitters notify only when a sleeper exists.
- `wait(Counter)` from a worker executes other jobs of its pool; from an outside thread it helps the Performance pool for a short spin and then blocks. A Performance job may therefore run on the main thread with `current_worker() == nullptr`; Efficiency jobs run only on Efficiency workers.

**Invariants.**
- Every scheduled job executes exactly once and signals its counter once.
- `jobs_executed == steals + inbox pops + own pops + helper runs`; tests check totals.
- Workers of a pool are pinned to distinct CPUs of that pool's class when enough exist.

**Public API.** `core/jobs/job_system.h`: `Job`, `JobFn`, `Pool`, `Counter`, `JobSystemConfig`, `WorkerInfo`, `JobSystemStats`, `JobSystem` (`schedule`, `wait`, `parallel_for`, `worker_count`, `worker_info`, `current_worker`, `current`, `stats`). `core/jobs/detail/` (Chase-Lev `WorkDeque`, Vyukov `MpmcQueue`) is not public.

**Depends on.** `base`, `memory`, `containers`, `platform`.

**Testing.** `tools/dev.ps1 test -Filter jobs`. Queue tests hammer the deque with four thieves and the MPMC queue with four producers and four consumers, checking every item moves exactly once. System tests cover start/stop, exact-once execution across both pools, `parallel_for` coverage and reduction, nested scheduling with waiting inside jobs, cross-pool scheduling, pinning to the right CPU class, observable stealing under a flood, and that idle workers sleep.

**A job holds its worker until it returns**, which matters because the pool is sized for short ones: there is no preemption and a blocked job is a worker the pool cannot use. One consumer does that deliberately — `domain/ecs` hosts each flecs worker as a job that runs for the world's lifetime ([ADR-0030](../adr/0030-flecs-workers-are-long-running-pool-jobs.md)) — and it is allowed only because it bounds itself: the adapter keeps a budget of how many performance workers may be held at once and refuses a world that would exceed it. Anything else long-running belongs on `Pool::Efficiency`. The counterexample is on record: hosting those same workers as *per-tick* jobs, so the pool re-dispatched a long-running worker every tick, cost 1.6–2.2 ms a tick on a 100,000-entity world while the pool's own submit-and-join path stayed at 1.2–5.7 µs ([E6](../experiments/e6-ecs-store.md#why-per-tick-hosting-cost-what-it-did-2026-09-18)). Nothing here changed to fix it, and that is the point: the spin-then-sleep policy below is right for short jobs, and it was the usage that was wrong.

**Performance notes.** Deque and inbox capacities are configurable powers of two (default 4096). Spin-before-sleep iterations are configurable (default 2000). Candidates for the tunables harness: spin length, steal order policy, whether SMT siblings each get a worker, and whether Efficiency workers may help the Performance pool when it is saturated (currently never, to keep slow cores off the critical path).

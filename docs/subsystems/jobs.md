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
- The first outside thread to submit to a pool (normally the main thread) is given that pool's **guest deque**, a worker's deque with no worker: what it submits goes there instead of the inbox, it pops its own newest work from it while it waits, and workers steal from it after their own victims. Every other outside thread, and a worker submitting to the other pool, uses the inbox.
- `wait(Counter)` from a worker executes other jobs of its pool; from an outside thread it helps the Performance pool for a short spin and then blocks. A Performance job may therefore run on the main thread with `current_worker() == nullptr`; Efficiency jobs run only on Efficiency workers. Helping is one level deep (below).

**Invariants.**
- Every scheduled job executes exactly once and signals its counter once.
- `jobs_executed == steals + inbox pops + own pops + helper runs`; tests check totals.
- A thread's stack holds at most one **lent** job — one a wait ran that was not of the group it waited for — on top of its own work; the only way past it is `help_escapes`, a parked job that found every queue full (see "Helping is one level deep").
- Workers of a pool are pinned to distinct CPUs of that pool's class when enough exist.

**Public API.** `core/jobs/job_system.h`: `Job`, `JobFn`, `Pool`, `Counter`, `JobSystemConfig`, `WorkerInfo`, `JobSystemStats`, `JobSystem` (`schedule`, `wait`, `parallel_for`, `worker_count`, `worker_info`, `current_worker`, `current`, `stats`). `core/jobs/detail/` (Chase-Lev `WorkDeque`, Vyukov `MpmcQueue`) is not public.

**Depends on.** `base`, `memory`, `containers`, `platform`.

**Testing.** `tools/dev.ps1 test -Filter jobs`. Queue tests hammer the deque with four thieves and the MPMC queue with four producers and four consumers, checking every item moves exactly once. System tests cover start/stop, exact-once execution across both pools, `parallel_for` coverage and reduction, nested scheduling with waiting inside jobs, cross-pool scheduling, pinning to the right CPU class, observable stealing under a flood, and that idle workers sleep. `help_depth_tests.cpp` saturates the pool with 64 "texture" jobs scheduled from the main thread, each a `parallel_for` of rows whose every row is a `parallel_for` of its own, at 8 workers (the content build's `--jobs 8`) and at 1, and checks with a stack probe that no thread ever has more than two of them on its stack (its own and one lent), that no parked job escaped, and that every cell ran; without the bound the test overflows its stack (306 jobs deep, `SIGSEGV - Stack overflow`). `engine_jobs_bench` (`tools/dev.ps1 bench -Filter "jobs*"`) times a small `parallel_for`'s submit and join from the main thread and the saturated nested case.

## Helping is one level deep

**What it fixed (F26, 2026-10-29).** A cold `engine-content build-all` of the forest v22 manifest (260 entries, 1,787 textures, `--jobs 8`) died with `0xc00000fd`, a stack overflow, in the main thread. The texture step schedules every texture build and waits; the wait helped by running a queued job, a texture build; its mip chain's `parallel_for` waited and helped by running another texture build; and so on, one texture build — with stb's PNG decoder, which keeps large buffers on the stack — per level of helping, as deep as the pool had textures queued. The reproduction counted 105 jobs on one stack at the crash, 110 s in. Workers were exposed the same way (their stacks are the same 1 MB), and a warm derived-data cache hid it because nothing was built. A bigger stack would only have moved the depth at which it fails; the depth was set by the length of the queue, which no stack bounds.

**The rule.** A job a wait runs that is not of the group it is waiting for (the counter's jobs) is **lent** to that thread's stack. A wait may lend its thread to any job of its pool only while no lent job is on its stack; a job of the waited group is the waiter's own work, as a call would be, and does not count. A wait nested inside a lent job runs only its own group's jobs: it looks at the newest end of its own deque (where a `parallel_for`'s chunks are), at the inbox (cycled 64 entries a look) and, when the inbox is empty, at the oldest end of the other deques, and **parks** every other job it takes back in a queue — the inbox, or its own deque when the inbox is full. `Counter`'s `queued_`, the group's jobs that are queued rather than running, says whether there is anything to look for: a bounded waiter whose jobs are all running looks nowhere and backs off (a pause doubling to 1,024), and an outside thread blocks once nothing of its group is queued or after 256 fruitless looks (a group in the other pool). So a stack holds the thread's own work plus at most one lent job and the nesting of its own `parallel_for`s; the content build's deepest stack is now 3 jobs.

*Why it cannot deadlock.* A parked job goes back to a queue that every thread of the pool reads, so refusing it strands nothing; and a bounded waiter can reach every queued job of its own group (its own deque by popping and parking what lies above, the inbox by cycling, the others by stealing), so it never waits for a job that nobody may run. The one way past the bound is a park that finds every queue full: the job runs in place, lent, the way `schedule` runs a job on its submitter when every queue is full, and `help_escapes` counts it.

*Why a wait at the top still lends.* The stricter rule — a wait never runs anything but its own group — would leave a worker whose chunks are all running elsewhere idle until they finish. One lent job costs one job's stack, which is bounded. The fully group-scoped `parallel_for`, whose waiter claims its own chunks from an atomic index and never runs a queued job at all, was not built: the queued jobs that find nothing left to claim still point at the call's chunk descriptors after it returns, so the descriptors would have to live in a slab the system owns and recycle when the last stale job has run.

**The guest deque, and why the bound needed it.** With the bound alone the cook finished, but the main thread's own chunks were at the tail of an inbox holding ~1,800 queued texture builds: a bounded wait cycled the whole inbox to reach them (12 million parks over the cook), and the texture the main thread was building as its own work waited 114 s for its mips while its rows sat behind every texture queued before them. The main thread is a worker in all but name, so it gets a worker's deque: its rows go to the end it pops, workers steal them from the other end, and the parks went to zero.

**Measured** on the forest v22 manifest, `msvc-release`, `--jobs 8` (seven performance workers and the main thread), a fresh derived-data cache each time, on the shared machine (the merge gate's tests running beside it; the CPU was 24 % busy before the first run, 4–9 % before the others and 53 % at the end of the last, so the times are upper bounds):

| helping rule | exit | wall time | `helper_runs` | `foreign_helps` | `parked` | most jobs on one stack |
|---|---|---|---|---|---|---|
| unbounded (9f4caed4) | `0xc00000fd` at 110 s | — | — | — | — | 105 at the crash |
| one level deep, outside threads on the inbox | 0 | 219.9 s | 181,876 | 278 | 12,011,516 | 3 |
| one level deep, guest deque (this) | 0 | 214.1 s | 183,854 | 3,536 | 0 | 3 |

With the guest deque the slowest texture's mips took 545 ms (114 s without it) and the texture builds' summed times fell from 1,062,830 ms to 791,631 ms. `engine_jobs_bench`, same build, before and after (the "before" carries the new bookkeeping — `queued_` and the nesting count — under the old rule; the harness recorded 11–12 % of the CPU used by others, upper bounds): `jobs.nested.saturated/1` 17.92 → 11.96 ms, `/8` 2.835 → 2.846 ms; `jobs.parallel_for.small/1` 14.21 → 11.44 µs, `/8` 14.11 → 13.10 µs.

`JobSystemStats` says how it went: `foreign_helps` (lent jobs), `parked`, `help_escapes`, and `max_job_nesting`, the most jobs one thread has had on its stack.

## Long-running jobs and tuning

**A job holds its worker until it returns**, which matters because the pool is sized for short ones: there is no preemption and a blocked job is a worker the pool cannot use. One consumer does that deliberately — `domain/ecs` hosts each flecs worker as a job that runs for the world's lifetime ([ADR-0030](../adr/0030-flecs-workers-are-long-running-pool-jobs.md)) — and it is allowed only because it bounds itself: the adapter keeps a budget of how many performance workers may be held at once and refuses a world that would exceed it. Anything else long-running belongs on `Pool::Efficiency`. The counterexample is on record: hosting those same workers as *per-tick* jobs, so the pool re-dispatched a long-running worker every tick, cost 1.6–2.2 ms a tick on a 100,000-entity world while the pool's own submit-and-join path stayed at 1.2–5.7 µs ([E6](../experiments/e6-ecs-store.md#why-per-tick-hosting-cost-what-it-did-2026-09-18)). Nothing here changed to fix it, and that is the point: the spin-then-sleep policy below is right for short jobs, and it was the usage that was wrong.

**Performance notes.** Deque and inbox capacities are configurable powers of two (default 4096). Spin-before-sleep iterations are configurable (default 2000). Candidates for the tunables harness: spin length, steal order policy, whether SMT siblings each get a worker, and whether Efficiency workers may help the Performance pool when it is saturated (currently never, to keep slow cores off the critical path).

#pragma once

// flecs' OS hooks pointed at the engine: its worker threads at core/jobs, its log at core/log.
//
// **Why the workers are the engine's threads at all.** core/jobs pins one worker per logical CPU
// and splits them into a performance and an efficiency pool, stealing within a cache domain
// first (plan 11 §11.5). A second, unpinned set of threads for flecs would oversubscribe every
// core and undo that, so flecs' workers run on the pool.
//
// **How, and why not the other way.** flecs can be multithreaded two ways. `ecs_set_threads()`
// starts long-running workers that live for the world's lifetime and park on flecs' own
// condition variables between sync points; `ecs_set_task_threads()` creates a worker per stage
// at the start of every `progress()` and joins them at the end, through
// `ecs_os_api_t::task_new_`/`task_join_`, and exists precisely so an engine can put those
// workers on its own job system. This adapter fills in **both** pairs of hooks, and
// `WorkerHosting::Threads` — long-running workers, each one job on the performance pool — is the
// default, because the per-tick form was measured to cost 1.6-2.2 ms a tick on E6's
// 100,000-entity world and to turn a 1.9x speedup into a slowdown. Two mechanisms, both per
// tick and both measured (docs/experiments/e6-ecs-store.md):
//
//   1. flecs creates and joins the whole worker set inside every `ecs_progress`, and the main
//      thread then spins in `flecs_wait_for_workers` until every new worker has announced
//      itself — 5,000 to 50,000 critical-section round trips a tick, against 4 to 18 *in total*
//      with `ecs_set_threads`.
//   2. A per-tick worker is a fresh job, and the pool worker that ran the previous tick's has
//      gone to sleep on the pool's eventcount in between, so every tick pays a wake per worker
//      and every single-threaded stretch of the tick leaves a pinned core idle. The same
//      adapter on a pool whose idle workers never sleep runs the pipeline in the same time
//      flecs' own threads do; that control is what says the cost is the re-dispatch and not the
//      pool's scheduling, which a bare submit-and-join round trip measures at 1-6 us.
//
// **What long-running hosting trades.** A flecs worker is not a short job: it blocks on a
// condition variable between sync points and returns only when the world drops its workers. As
// one job it therefore *occupies* a pool worker for the world's lifetime rather than using it.
// That is the cost, it is paid once instead of every tick, and it is bounded by a number the
// application chooses:
//
//   - `JobOsApiConfig::hosted_worker_budget` is how many of the performance pool's workers may
//     be lent to flecs worlds at once. The default lends all but one, so the pool always keeps
//     a worker for everything else; an application that wants more back says so.
//   - `set_workers` refuses a count the budget cannot give and leaves the world alone, because
//     a flecs worker with no thread to run on does not slow the tick down, it hangs it:
//     `flecs_wait_for_workers` spins until the worker announces itself and it never does.
//   - Nothing long-running may be scheduled on the performance pool alongside a world's
//     workers. Short jobs are fine; they run on the workers the budget kept back.
//   - There is no work stealing *into* the tick: flecs splits each system's matched entities
//     evenly across its workers, so an uneven system is as slow as its slowest range. That is
//     flecs' scheduling model, not something this adapter can improve; the engine's own
//     scheduler (ADR-0027's registration table) is where that gets fixed later.
//
// A hosted worker renames its pool thread for as long as it holds it, so a profiler and a
// debugger show `ecs-worker-<n>` rather than `perf-<n>` on the threads a world has taken.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/jobs/job_system.h>

#include <atomic>
#include <flecs.h>
#include <memory>

namespace engine::ecs {

// Routes flecs' log through core/log under the "ecs" category. Idempotent, and called by
// SimWorld's constructor, so most callers never need it.
void install_log_sink();

// flecs' own verbosity: -1 is warnings and errors only (flecs' default and ours), 0 adds its
// trace, 1..3 add increasing debug detail. Independent of core/log's level for the category,
// which filters on top.
void set_flecs_log_level(i32 level);

// How a world's flecs workers are obtained. Both run on core/jobs' performance pool; they
// differ in how long one job lasts.
enum class WorkerHosting : u8 {
  // `ecs_set_threads`: one long-running job per flecs worker, for the world's lifetime. flecs
  // keeps its own cheap per-tick signalling and the pool pays one dispatch per world.
  Threads = 0,
  // `ecs_set_task_threads`: one job per worker per tick. Kept selectable because E6's finding
  // is a comparison, and a benchmark that cannot be run is not evidence — not because it is a
  // reasonable thing to choose.
  Tasks = 1,
};

struct JobOsApiConfig {
  // How many of the performance pool's workers may be lent to flecs worlds at the same time.
  // A lent worker is unavailable to everything else for the world's lifetime, so this is the
  // application's statement of how much of its pool the simulation may hold. 0 selects
  // "performance workers minus one", which always keeps one worker for everything else.
  u32 hosted_worker_budget = 0;
};

// Installs the job-system adapter for as long as it lives. One at a time; constructing a second
// one while the first is alive replaces the target job system and the destructor puts the
// previous one back, which is what lets tests stand one up per case.
//
// **Lifetime.** Every world that holds workers from this adapter must be dropped to one worker
// before the adapter is destroyed; `SimWorld` does that in its own destructor. The destructor
// takes back any workers still out — a world whose adapter goes away keeps ticking, single
// threaded — so that the job system it wraps can then be joined instead of deadlocking on a
// pool thread that is parked inside flecs.
class JobOsApi {
 public:
  explicit JobOsApi(jobs::JobSystem& jobs, const JobOsApiConfig& config = {});
  ~JobOsApi();
  ENGINE_NON_COPYABLE(JobOsApi);

  // How many performance-pool workers may be lent out at once, resolved from the config.
  u32 hosted_worker_budget() const noexcept { return budget_; }
  // How many are lent out right now.
  u32 hosted_workers() const noexcept;
  // How many of the performance pool's workers are left for everything else.
  u32 pool_workers_kept() const noexcept;

  // The largest worker count a world may be given right now. It counts the calling thread, so
  // a world of `max_workers()` puts `max_workers() - 1` workers on the pool.
  u32 max_workers() const noexcept;
  jobs::JobSystem& job_system() const noexcept { return *jobs_; }

  // How many flecs workers this adapter has put on the pool, by hosting. A world with n workers
  // adds n-1 to `workers_started` once, or n-1 to `tasks_started` every tick.
  u64 tasks_started() const noexcept;
  u64 workers_started() const noexcept;

  static JobOsApi* current() noexcept;

  // One scheduled flecs worker. Opaque; public only so the OS-API trampolines, which are free
  // functions in the implementation file, can name it.
  struct Slot;

  // Called by those trampolines. `start_worker` returns the handle flecs stores as its thread
  // identity; it never returns 0 through the supported path, because `set_workers` has already
  // refused a count the pool cannot host.
  u64 start_worker(void* (*callback)(void*), void* param, bool persistent) noexcept;
  void* join_worker(u64 handle) noexcept;

  // Admission for `set_workers`: takes `workers` pool workers for `world` or returns false.
  bool reserve(ecs_world_t* world, u32 workers) noexcept;
  void end_reservation() noexcept;

 private:
  void release_one(ecs_world_t* world) noexcept;

  jobs::JobSystem* jobs_ = nullptr;
  JobOsApi* previous_ = nullptr;
  std::unique_ptr<Slot[]> slots_;
  u32 slot_capacity_ = 0;
  u32 budget_ = 0;
  ecs_world_t* reserving_ = nullptr;
  std::atomic<u32> hosted_{0};
  std::atomic<u64> tasks_started_{0};
  std::atomic<u64> workers_started_{0};
};

// Gives the world `workers` worker threads, counting the calling thread: 1 is single-threaded,
// and n runs n-1 flecs workers on the performance pool. Whatever the world held before is given
// back first, so this is also how a world changes its worker count or its hosting.
//
// Returns false and leaves the world single-threaded when no JobOsApi is installed or when the
// adapter's budget cannot give `workers - 1` workers, because either would hang the tick rather
// than slow it down.
bool set_workers(flecs::world& world, u32 workers, WorkerHosting hosting = WorkerHosting::Threads);

// Drops the world back to one worker and gives its pool workers back, whichever hosting it was
// using. Required before a world is destroyed: flecs' own `ecs_fini` does not join a world's
// workers, it asserts that they are gone.
void stop_workers(flecs::world& world);

}  // namespace engine::ecs

#pragma once

// flecs' OS hooks pointed at the engine: its worker threads at core/jobs, its log at core/log.
//
// **Why the task API and not the thread API.** flecs can be multithreaded two ways. `ecs_set_
// threads()` starts long-running OS threads that live for the world's lifetime; `ecs_set_task_
// threads()` creates a worker per stage at the start of each `progress()` and joins them at the
// end, through `ecs_os_api_t::task_new_`/`task_join_`, and exists precisely so that an engine
// can put those workers on its own job system. The engine already owns its threads: core/jobs
// pins one worker per logical CPU, splits them into a performance and an efficiency pool, and
// steals within a cache domain first (plan 11 §11.5). A second, unpinned set of threads for
// flecs would oversubscribe every core and undo that, so the task API is the one to use.
//
// **What it trades.** A flecs worker is not a short job: it blocks on a condition variable
// between sync points and only returns at the end of the tick. Running it as a job therefore
// *occupies* a pool worker for the whole tick instead of using it, so while the sim ticks with
// N workers, N-1 of the pool's threads are unavailable for anything else. Three consequences:
//
//   - The tick's worker count must stay at or below `JobOsApi::max_workers()`, which is the
//     performance pool's worker count. Ask for more and a worker would have no thread to run on,
//     flecs would spin waiting for it, and the tick would never finish; `set_workers` refuses
//     instead.
//   - Nothing long-running may be scheduled on the performance pool during a tick. Short jobs
//     are fine — they queue behind the workers and run when the tick ends.
//   - There is no work stealing *into* the tick: flecs splits each system's matched entities
//     evenly across its workers, so an uneven system is as slow as its slowest range. That is
//     flecs' scheduling model, not something this adapter can improve; the engine's own
//     scheduler (ADR-0027's registration table) is where that gets fixed later.
//
// The alternative — replacing `thread_new_` so flecs' long-running threads *are* pool workers —
// is strictly worse: those threads would hold pool workers for the life of the world, not for
// the length of a tick.
//
// The upside beyond core residency: creating a "thread" per tick costs a job submission (tens of
// nanoseconds) instead of an OS thread creation (tens of microseconds), so the task API is the
// cheap one here even though it is the one that looks expensive.

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

// Installs the job-system adapter for as long as it lives. One at a time; constructing a second
// one while the first is alive replaces the target job system and the destructor puts the
// previous one back, which is what lets tests stand one up per case.
class JobOsApi {
 public:
  explicit JobOsApi(jobs::JobSystem& jobs);
  ~JobOsApi();
  ENGINE_NON_COPYABLE(JobOsApi);

  // The largest worker count a world may be given. It counts the calling thread, so the pool
  // hosts `max_workers() - 1` flecs workers and keeps one thread spare — the caller may itself
  // be a pool worker that is about to block in the join.
  u32 max_workers() const noexcept;
  jobs::JobSystem& job_system() const noexcept { return *jobs_; }
  // How many flecs workers this adapter has put on the pool. One tick with n workers adds n-1.
  u64 tasks_started() const noexcept;

  static JobOsApi* current() noexcept;

  // One scheduled flecs worker. Opaque; public only so the OS-API trampolines, which are free
  // functions in the implementation file, can name it.
  struct Task;

  // Called by those trampolines. `start_task` returns the handle flecs stores as its thread
  // identity; 0 means the tick cannot be run and flecs will assert.
  u64 start_task(void* (*callback)(void*), void* param) noexcept;
  void* join_task(u64 handle) noexcept;

 private:
  jobs::JobSystem* jobs_ = nullptr;
  JobOsApi* previous_ = nullptr;
  std::unique_ptr<Task[]> tasks_;
  u32 task_capacity_ = 0;
  std::atomic<u64> tasks_started_{0};
};

// Gives the world `workers` worker threads, counting the calling thread: 1 is single-threaded,
// and n runs n-1 flecs workers as jobs on the performance pool. Returns false and leaves the
// world alone when no JobOsApi is installed or `workers` is above `JobOsApi::max_workers()`,
// because either would hang the tick rather than slow it down.
bool set_workers(flecs::world& world, u32 workers);

}  // namespace engine::ecs

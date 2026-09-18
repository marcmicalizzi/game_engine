#pragma once

// Jolt's jobs, running on the engine's job system (core/jobs, performance pool).
//
// Jolt asks for four things: allocate a job, free a job, queue one or many, and say how many
// ways it may split its work. Barriers — the part with the interesting ordering rules — are
// JobSystemWithBarrier's, which is written for exactly this case: the barrier owns the
// completion semaphore, executes ready jobs on the waiting thread, and is the only place that
// blocks. So the adapter never blocks a pool worker: a queued job runs once and returns, and
// the thread that called `World::step` is the one that waits.
//
// The alternative was Jolt's own JobSystemThreadPool, which would put a second set of threads
// beside the engine's pinned pools and let the OS decide which of the two gets a P-core
// (plan 11 §11.5). This adapter is about 60 lines; the thread pool would have cost more than
// that in scheduling pathologies.
//
// The one thing the adapter must do that Jolt's thread pool does inside its own destructor is
// *drain*. A queued job holds a reference to a Job object out of a fixed-size free list, and
// the barrier hands that same job to the stepping thread if it gets there first — so when
// `PhysicsSystem::Update` returns, the queue can still hold entries for jobs that are already
// finished. Those entries keep their Job alive. Jolt's own pool owns its queue and empties it
// when it stops its threads; this adapter's queue belongs to `jobs::JobSystem`, which outlives
// the world, so nothing would ever empty it. `drain()` is that step and `World::step` calls it
// (see docs/subsystems/physics.md, "Draining the queue").

#include "jolt.h"

#include <core/base/types.h>
#include <core/jobs/job_system.h>

#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

#include <atomic>

namespace engine::physics {

class JoltJobAdapter final : public JPH::JobSystemWithBarrier {
 public:
  // `system` may be null, in which case queued jobs run inline on the calling thread (Jolt's
  // own JobSystemSingleThreaded does the same thing) and concurrency is 1.
  // `concurrency` is how many ways Jolt may split its work, not a thread count: it is what
  // changes between the 1-worker and 8-worker runs of the determinism test.
  JoltJobAdapter(jobs::JobSystem* system, u32 concurrency, u32 max_jobs, u32 max_barriers);
  ~JoltJobAdapter() override;

  int GetMaxConcurrency() const override { return concurrency_; }
  JPH::JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function,
                           JPH::uint32 dependencies) override;

  u64 queued_count() const noexcept { return queued_.load(std::memory_order_relaxed); }
  u64 worker_count() const noexcept { return on_workers_.load(std::memory_order_relaxed); }
  // Jobs handed to the pool that have not run yet, and therefore are still holding a Job
  // object out of the free list. Zero once `drain()` has returned.
  u32 pending_count() const noexcept { return pending_.remaining(); }

  // Waits until every job this adapter handed to the job system has run and released its
  // reference. Called at the end of each step and again before the adapter is destroyed, so
  // that the number of live backend jobs is bounded by one step's worth rather than by how
  // fast the pool happens to be draining. Cheap: the jobs it waits for are finished already
  // and the wait helps the pool rather than spinning on it.
  void drain();

 protected:
  void QueueJob(Job* job) override;
  void QueueJobs(Job** job_array, JPH::uint count) override;
  void FreeJob(Job* job) override;

 private:
  static void run_job(void* data);

  jobs::JobSystem* system_ = nullptr;
  int concurrency_ = 1;
  JPH::FixedSizeFreeList<Job> jobs_;
  // One outstanding-work counter for the adapter's whole life: incremented before a job is
  // handed to the pool and signalled by the pool once that job has run, so `drain()` is a
  // `JobSystem::wait` and not a hand-rolled spin.
  jobs::Counter pending_;
  std::atomic<u64> queued_{0};
  std::atomic<u64> on_workers_{0};
};

}  // namespace engine::physics

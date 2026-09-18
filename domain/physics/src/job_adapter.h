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
// (see docs/subsystems/physics.md, "The job adapter, and why the step drains the queue").
//
// It also tags each job with the backend stage it belongs to, so that "did the soft-body solve
// actually spread across workers?" is a number a test and a bench can read rather than a
// profiler session (WorldStats::soft_body_solve_jobs / soft_body_solve_workers, E19). It costs a
// name compare per job created — tens per step — and one relaxed atomic per solve job executed,
// which is nothing beside the job itself.

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
  u64 soft_body_count() const noexcept {
    return soft_body_solve_jobs_.load(std::memory_order_relaxed);
  }
  // How many distinct performance workers have executed a soft-body solve job. One word, so
  // worker 64 and up share the last bit; a pool that wide is not the case this measures.
  u32 soft_body_worker_count() const noexcept;

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
  // The backend's step is a graph of named jobs. The names are the only thing that says which
  // stage a job belongs to, and Jolt only keeps them when its profiler is compiled in (which
  // it is not here), so the tag is taken from the name at CreateJob and carried on the job.
  enum class Stage : u8 { Other, SoftBodySolve };

  // Jolt's Job plus that tag. Deriving is what makes the tag reachable from the pointer the
  // queue hands back: Job is not polymorphic and stores nothing we could hang it on.
  struct TaggedJob final : Job {
    TaggedJob(const char* name, JPH::ColorArg color, JPH::JobSystem* system,
              const JobFunction& function, JPH::uint32 dependencies, Stage job_stage)
        : Job(name, color, system, function, dependencies), stage(job_stage) {}
    Stage stage = Stage::Other;
  };

  static void run_job(void* data);

  jobs::JobSystem* system_ = nullptr;
  int concurrency_ = 1;
  JPH::FixedSizeFreeList<TaggedJob> jobs_;
  // One outstanding-work counter for the adapter's whole life: incremented before a job is
  // handed to the pool and signalled by the pool once that job has run, so `drain()` is a
  // `JobSystem::wait` and not a hand-rolled spin.
  jobs::Counter pending_;
  std::atomic<u64> queued_{0};
  std::atomic<u64> on_workers_{0};
  std::atomic<u64> soft_body_solve_jobs_{0};
  std::atomic<u64> soft_body_solve_workers_{0};
};

}  // namespace engine::physics

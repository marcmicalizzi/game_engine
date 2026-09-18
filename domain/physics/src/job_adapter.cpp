#include "job_adapter.h"

#include <core/base/assert.h>
#include <core/jobs/job_system.h>
#include <core/time/time.h>

#include <bit>
#include <cstring>

namespace engine::physics {

namespace {

// The job the backend runs its soft-body constraint solve in: one per
// PhysicsSystem::GetMaxConcurrency(), each of which claims constraint groups of every active
// cage until the step's iterations are done. Matching the name is the only handle we have on
// the stage — Jolt keeps a job's name only when its profiler is compiled in, and it is not
// (cmake/EnginePhysics.cmake) — so the name is read here, where it is still a parameter.
// A rename in a future Jolt shows up as a solve-job count of zero, which the parallelism test
// asserts against rather than leaving the counter to quietly mean nothing.
constexpr const char* k_soft_body_solve_job = "SoftBodySimulate";
// The backend's soft-body stage is four job kinds and they all begin this way: prepare, collide,
// simulate, finalize. The prefix is what opens ADR-0029's budget window, so a future Jolt that
// added a fifth would be measured too.
constexpr const char* k_soft_body_prefix = "SoftBody";
// The job that kicks the next collision sub-step. The soft-body finalize job is what removes its
// last dependency, so its queueing is the end of the phase.
constexpr const char* k_next_step_job = "StartNextStep";

bool is_soft_body_solve_job(const char* name) noexcept {
  return name != nullptr && std::strcmp(name, k_soft_body_solve_job) == 0;
}

bool is_soft_body_job(const char* name) noexcept {
  return name != nullptr && std::strncmp(name, k_soft_body_prefix, 8) == 0;
}

bool is_next_step_job(const char* name) noexcept {
  return name != nullptr && std::strcmp(name, k_next_step_job) == 0;
}

}  // namespace

JoltJobAdapter::JoltJobAdapter(jobs::JobSystem* system, u32 concurrency, u32 max_jobs,
                               u32 max_barriers)
    : system_(system), concurrency_(static_cast<int>(concurrency < 1 ? 1 : concurrency)) {
  JobSystemWithBarrier::Init(max_barriers);
  // One page holds every job: the free list never grows during a step, which is what keeps the
  // step allocation-free.
  jobs_.Init(max_jobs, max_jobs);
}

JoltJobAdapter::~JoltJobAdapter() {
  // Nothing may still be holding a Job when the free list goes away — the free list asserts on
  // it in Debug, and in Release a job that ran afterwards would touch a deleted adapter. The
  // step drains too, so this is normally a no-op; it covers a world torn down after a step
  // that failed, and it is the last line of defence either way.
  drain();
}

void JoltJobAdapter::drain() {
  if (system_ != nullptr) system_->wait(pending_);
}

u32 JoltJobAdapter::soft_body_worker_count() const noexcept {
  return static_cast<u32>(std::popcount(soft_body_solve_workers_.load(std::memory_order_relaxed)));
}

void JoltJobAdapter::close_soft_body_window() noexcept {
  u32 open = 1;
  // Only the thread that wins the 1 -> 0 exchange accumulates, so a window is counted once
  // however many jobs raced to close it.
  if (!soft_body_window_.compare_exchange_strong(open, 0, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
    return;
  }
  const i64 start = soft_body_window_start_ns_.load(std::memory_order_relaxed);
  soft_body_ns_.fetch_add(time::monotonic_ns() - start, std::memory_order_relaxed);
}

void JoltJobAdapter::reset_soft_body_ns() noexcept {
  soft_body_ns_.store(0, std::memory_order_relaxed);
}

JPH::JobHandle JoltJobAdapter::CreateJob(const char* name, JPH::ColorArg color,
                                         const JobFunction& function, JPH::uint32 dependencies) {
  Stage stage = Stage::Other;
  if (is_soft_body_solve_job(name)) {
    stage = Stage::SoftBodySolve;
  } else if (is_soft_body_job(name)) {
    stage = Stage::SoftBody;
  } else if (is_next_step_job(name)) {
    stage = Stage::NextStep;
  }
  const JPH::uint32 index = jobs_.ConstructObject(name, color, this, function, dependencies, stage);
  // Jolt sizes its job budget (cMaxPhysicsJobs) for the worst step it can produce, so running
  // out means the world was configured with fewer jobs than the backend needs, not that the
  // caller did something wrong. Failing loudly beats Jolt's own "sleep and retry" spin.
  ENGINE_VERIFY(index != JPH::FixedSizeFreeList<TaggedJob>::cInvalidObjectIndex,
                "physics: the backend ran out of jobs");
  Job* job = &jobs_.Get(index);

  // Take a reference before queueing: the job may finish (and want to free itself) before
  // this function returns.
  const JPH::JobHandle handle(job);
  if (dependencies == 0) QueueJob(job);
  return handle;
}

void JoltJobAdapter::FreeJob(Job* job) { jobs_.DestructObject(static_cast<TaggedJob*>(job)); }

void JoltJobAdapter::run_job(void* data) {
  auto* job = static_cast<TaggedJob*>(data);
  // The job knows which system created it, and it is always one of ours.
  auto* self = static_cast<JoltJobAdapter*>(job->GetJobSystem());
  const jobs::WorkerInfo* info = jobs::JobSystem::current_worker();
  if (info != nullptr) self->on_workers_.fetch_add(1, std::memory_order_relaxed);
  if (job->stage == Stage::SoftBodySolve) {
    self->soft_body_solve_jobs_.fetch_add(1, std::memory_order_relaxed);
    if (info != nullptr && info->pool == jobs::Pool::Performance) {
      const u32 bit = info->index < 63 ? info->index : 63u;
      self->soft_body_solve_workers_.fetch_or(u64{1} << bit, std::memory_order_relaxed);
    }
  }
  job->Execute();
  job->Release();
}

void JoltJobAdapter::QueueJob(Job* job) {
  queued_.fetch_add(1, std::memory_order_relaxed);
  // Every job this adapter hands out is one of ours, so the downcast is what recovers the
  // stage tag on the other side of the queue's void*.
  auto* tagged = static_cast<TaggedJob*>(job);
  // ADR-0029's window. Queueing a soft-body job opens the phase, queueing the next sub-step's
  // starter closes it. Both happen before the job runs, which is the point: this measures the
  // wall clock the phase occupies whatever thread ends up executing it.
  if (is_soft_body(tagged->stage)) {
    // The start is published before the flag, so a close can never read a start that has not
    // been written. Two jobs queued at once may both write one; they are the same instant to
    // within the burst that queued them, and only one of them wins the flag.
    if (soft_body_window_.load(std::memory_order_acquire) == 0) {
      soft_body_window_start_ns_.store(time::monotonic_ns(), std::memory_order_relaxed);
      u32 closed = 0;
      soft_body_window_.compare_exchange_strong(closed, 1, std::memory_order_acq_rel,
                                                std::memory_order_relaxed);
    }
  } else if (tagged->stage == Stage::NextStep) {
    close_soft_body_window();
  }
  if (system_ == nullptr) {
    // No job system: run it here and now. The barrier still sees a finished job. The counters
    // that run_job keeps are its own, so they stay at zero, which is the truth: nothing ran on
    // a worker.
    if (tagged->stage == Stage::SoftBodySolve)
      soft_body_solve_jobs_.fetch_add(1, std::memory_order_relaxed);
    job->Execute();
    return;
  }
  // The reference the queue holds; run_job releases it. The counter is added to *before* the
  // job is handed over, because the pool may run and signal it before schedule() returns.
  job->AddRef();
  pending_.add(1);
  system_->schedule(jobs::Pool::Performance, jobs::Job{&run_job, tagged, &pending_});
}

void JoltJobAdapter::QueueJobs(Job** job_array, JPH::uint count) {
  for (JPH::uint i = 0; i < count; ++i)
    QueueJob(job_array[i]);
}

}  // namespace engine::physics

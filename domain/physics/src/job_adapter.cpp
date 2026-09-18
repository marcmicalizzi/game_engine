#include "job_adapter.h"

#include <core/base/assert.h>
#include <core/jobs/job_system.h>

namespace engine::physics {

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

JPH::JobHandle JoltJobAdapter::CreateJob(const char* name, JPH::ColorArg color,
                                         const JobFunction& function, JPH::uint32 dependencies) {
  const JPH::uint32 index = jobs_.ConstructObject(name, color, this, function, dependencies);
  // Jolt sizes its job budget (cMaxPhysicsJobs) for the worst step it can produce, so running
  // out means the world was configured with fewer jobs than the backend needs, not that the
  // caller did something wrong. Failing loudly beats Jolt's own "sleep and retry" spin.
  ENGINE_VERIFY(index != JPH::FixedSizeFreeList<Job>::cInvalidObjectIndex,
                "physics: the backend ran out of jobs");
  Job* job = &jobs_.Get(index);

  // Take a reference before queueing: the job may finish (and want to free itself) before
  // this function returns.
  const JPH::JobHandle handle(job);
  if (dependencies == 0) QueueJob(job);
  return handle;
}

void JoltJobAdapter::FreeJob(Job* job) { jobs_.DestructObject(job); }

void JoltJobAdapter::run_job(void* data) {
  Job* job = static_cast<Job*>(data);
  // The job knows which system created it, and it is always one of ours.
  auto* self = static_cast<JoltJobAdapter*>(job->GetJobSystem());
  if (jobs::JobSystem::current_worker() != nullptr)
    self->on_workers_.fetch_add(1, std::memory_order_relaxed);
  job->Execute();
  job->Release();
}

void JoltJobAdapter::QueueJob(Job* job) {
  queued_.fetch_add(1, std::memory_order_relaxed);
  if (system_ == nullptr) {
    // No job system: run it here and now. The barrier still sees a finished job.
    job->Execute();
    return;
  }
  // The reference the queue holds; run_job releases it. The counter is added to *before* the
  // job is handed over, because the pool may run and signal it before schedule() returns.
  job->AddRef();
  pending_.add(1);
  system_->schedule(jobs::Pool::Performance, jobs::Job{&run_job, job, &pending_});
}

void JoltJobAdapter::QueueJobs(Job** job_array, JPH::uint count) {
  for (JPH::uint i = 0; i < count; ++i)
    QueueJob(job_array[i]);
}

}  // namespace engine::physics

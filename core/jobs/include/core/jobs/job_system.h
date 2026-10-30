#pragma once

// Topology-aware job system (docs/plan/11-performance-principles.md section 11.5, ADR-0011).
//
// Workers are pinned, one per logical CPU, and grouped into pools by efficiency class:
// Pool::Performance for simulation and rendering, Pool::Efficiency for streaming,
// decompression, audio mixing, and background work. Each worker owns a Chase-Lev deque
// (LIFO for the owner, FIFO for thieves); idle workers steal from workers in the same cache
// domain first and from the rest of their pool second. Pools never steal from each other,
// so a slow efficiency core cannot pick up critical-path work. Jobs submitted from outside a
// pool go through the pool's inbox, except that the first outside thread to submit (normally the
// main thread) is given the pool's guest deque, a worker's deque the workers steal from, so the
// chunks of its own parallel_for wait at the end it pops rather than behind the whole inbox.
//
// A Job is a plain function pointer plus a context pointer (no std::function, no allocation
// per job). Completion is tracked with Counters. Waiting on a Counter from a worker thread
// runs other jobs meanwhile; waiting from an outside thread spins briefly, then blocks.
//
// Machines without efficiency cores still get an Efficiency pool: a few workers pinned to
// performance CPUs at below-normal OS priority, so callers never special-case the topology.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/small_vector.h>
#include <core/platform/topology.h>

#include <atomic>
#include <concepts>
#include <span>
#include <type_traits>
#include <utility>

namespace engine::jobs {

using JobFn = void (*)(void* data);

struct Job {
  JobFn fn = nullptr;
  void* data = nullptr;
  class Counter* counter = nullptr;  // signaled once when the job finishes; may be null
};

enum class Pool : u8 { Performance = 0, Efficiency = 1, Count = 2 };

namespace detail {
template <class T>
class WorkDeque;
}  // namespace detail

// Counts outstanding jobs. add() before scheduling, the system signals as jobs finish.
//
// A counter also names a group: the jobs scheduled with it. A thread waiting on it inside a job
// it was lent (see JobSystem::wait) runs only that group's jobs, and `queued_` — how many of them
// sit in a queue rather than running — tells it whether there is any to look for.
class Counter {
 public:
  Counter() noexcept = default;
  explicit Counter(u32 initial) noexcept : remaining_(initial) {}
  ENGINE_NON_COPYABLE(Counter);

  void add(u32 n = 1) noexcept { remaining_.fetch_add(n, std::memory_order_relaxed); }
  bool done() const noexcept { return remaining_.load(std::memory_order_acquire) == 0; }
  u32 remaining() const noexcept { return remaining_.load(std::memory_order_acquire); }

  // Called by the job system when a job completes; also usable for manual completion.
  void signal() noexcept {
    if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) remaining_.notify_all();
  }

  // Blocks the calling thread without helping. Prefer JobSystem::wait.
  void wait_blocking() noexcept {
    u32 v = remaining_.load(std::memory_order_acquire);
    while (v != 0) {
      remaining_.wait(v, std::memory_order_acquire);
      v = remaining_.load(std::memory_order_acquire);
    }
  }

 private:
  friend class JobSystem;
  alignas(64) std::atomic<u32> remaining_{0};
  std::atomic<u32> queued_{0};  // scheduled with this counter and not yet started; a hint
};

struct JobSystemConfig {
  // 0 selects one worker per logical CPU of the class, minus one performance CPU reserved
  // for the calling (main) thread.
  u32 performance_workers = 0;
  u32 efficiency_workers = 0;
  bool pin_threads = true;
  u32 queue_capacity = 4096;   // per worker deque and per pool inbox; power of two
  u32 spin_iterations = 2000;  // idle spins before a worker sleeps
};

struct WorkerInfo {
  u16 index = 0;  // 0..worker_count-1 within its pool
  Pool pool = Pool::Performance;
  u16 cpu = platform::k_invalid_cpu;
  u16 cache_domain = 0;
};

struct JobSystemStats {
  u64 jobs_executed = 0;  // every execution, wherever it happened
  u64 steals_local = 0;   // stolen from a worker in the same cache domain
  u64 steals_remote = 0;  // stolen from another cache domain
  u64 inbox_pops = 0;     // taken from a pool inbox by a worker
  u64 sleeps = 0;
  u64 helper_runs = 0;  // executed by a non-worker thread while it waited (includes inline runs)
  u64 inline_runs = 0;  // executed by the submitter because every queue was full
  // Jobs a waiting thread ran that were not of the group it waited for: each is a job lent to a
  // stack that was already inside another one. At most one deep per stack (JobSystem::wait).
  u64 foreign_helps = 0;
  u64 parked = 0;        // foreign jobs a waiter inside a lent job took and put back in a queue
  u64 help_escapes = 0;  // parked jobs run in place because every queue they could go to was full
  u32 max_job_nesting = 0;  // the most jobs one thread has had on its stack at once
};

class JobSystem {
 public:
  explicit JobSystem(const JobSystemConfig& config = {});
  ~JobSystem();
  ENGINE_NON_COPYABLE(JobSystem);

  // Submits one job. `job.counter`, if set, must have been add()ed by the caller.
  void schedule(Pool pool, const Job& job);
  // Submits many jobs sharing one counter. Adds jobs.size() to the counter first.
  void schedule(Pool pool, std::span<const Job> jobs, Counter& counter);

  // Waits until the counter reaches zero. From a worker thread this executes other jobs of
  // its pool meanwhile; from any other thread it helps the Performance pool briefly and then
  // blocks. Consequently a Performance job may run on the waiting thread, where
  // current_worker() is null; Efficiency jobs only ever run on Efficiency workers.
  //
  // Helping is one level deep (docs/subsystems/jobs.md, "Helping is one level deep"): a waiting
  // thread runs any job of the pool only while no job lent to it by an earlier wait is still on
  // its stack. A wait nested inside such a lent job runs only the counter's own jobs — the chunks
  // it is waiting for — and puts back any other job it takes while looking for them. So the stack
  // a wait borrows holds at most one job that is not the waiter's own work, rather than one per
  // level of helping, as deep as the pool's queues: F26, where a cold content build's main thread
  // held a texture build per level until its 1 MB stack ran out.
  void wait(Counter& counter);

  // Runs `body(begin, end)` over [0, count) in chunks of at most `grain`, in parallel, and
  // waits. `body` is invoked on worker threads; it must be safe to call concurrently on
  // disjoint ranges.
  template <class F>
    requires std::invocable<F&, u32, u32>
  void parallel_for(Pool pool, u32 count, u32 grain, F&& body) {
    if (count == 0) return;
    if (grain == 0) grain = 1;
    struct Chunk {
      F* body;
      u32 begin;
      u32 end;
    };
    const u32 chunk_count = (count + grain - 1) / grain;
    SmallVector<Chunk, 64> chunks;
    SmallVector<Job, 64> jobs;
    chunks.reserve(chunk_count);
    jobs.reserve(chunk_count);
    F* body_ptr = &body;
    for (u32 c = 0; c < chunk_count; ++c) {
      const u32 begin = c * grain;
      const u32 end = begin + grain < count ? begin + grain : count;
      chunks.push_back(Chunk{body_ptr, begin, end});
    }
    Counter counter;
    for (u32 c = 0; c < chunk_count; ++c) {
      jobs.push_back(Job{[](void* p) {
                           auto* chunk = static_cast<Chunk*>(p);
                           (*chunk->body)(chunk->begin, chunk->end);
                         },
                         &chunks[c], &counter});
    }
    schedule(pool, std::span<const Job>(jobs.data(), jobs.size()), counter);
    wait(counter);
  }

  u32 worker_count(Pool pool) const noexcept;
  u32 total_worker_count() const noexcept;
  const WorkerInfo& worker_info(Pool pool, u32 index) const noexcept;

  // Information about the calling thread when it is one of this system's workers.
  static const WorkerInfo* current_worker() noexcept;
  static JobSystem* current() noexcept;

  JobSystemStats stats() const noexcept;
  const platform::Topology& topology() const noexcept { return topo_; }

 private:
  struct Worker;
  struct PoolState;
  using detail_deque = detail::WorkDeque<Job>;

  static void worker_main(JobSystem* self, Worker* worker);
  // `waited`, when set, is the counter the calling thread is waiting on: a job of another group
  // is then run as lent (run_helped).
  bool try_run_one(Worker* worker, const Counter* waited = nullptr);
  bool try_run_one_from_pool(PoolState& pool, Worker* self_worker, const Counter& waited);
  void run_taken(const Job& job, Worker* worker, const Counter* waited) noexcept;
  // `own` is the caller's deque: its worker's, or the guest deque it owns; may be null.
  bool help_own(Counter& counter, PoolState& pool, Worker* self_worker, detail_deque* own);
  void park(PoolState& pool, detail_deque* own, Worker* self_worker, const Job& job);
  detail_deque* guest_of_caller(PoolState& pool) noexcept;
  detail_deque* own_deque(PoolState& pool) noexcept;
  void run_job(const Job& job, Worker* worker) noexcept;
  void run_helped(const Job& job, Worker* worker, const Counter& waited) noexcept;
  void wake_one(PoolState& pool) noexcept;
  void build_workers(const JobSystemConfig& config);

  const platform::Topology& topo_;
  JobSystemConfig config_;
  PoolState* pools_ = nullptr;  // [Pool::Count]
  Worker** workers_ = nullptr;  // all workers, both pools
  u32 worker_total_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<u64> inline_runs_{0};
  std::atomic<u64> helper_runs_{0};
  std::atomic<u64> foreign_helps_{0};
  std::atomic<u64> parked_{0};
  std::atomic<u64> help_escapes_{0};
  std::atomic<u32> max_job_nesting_{0};
};

}  // namespace engine::jobs

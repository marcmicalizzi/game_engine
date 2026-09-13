#pragma once

// Topology-aware job system (docs/plan/11-performance-principles.md section 11.5, ADR-0011).
//
// Workers are pinned, one per logical CPU, and grouped into pools by efficiency class:
// Pool::Performance for simulation and rendering, Pool::Efficiency for streaming,
// decompression, audio mixing, and background work. Each worker owns a Chase-Lev deque
// (LIFO for the owner, FIFO for thieves); idle workers steal from workers in the same cache
// domain first and from the rest of their pool second. Pools never steal from each other,
// so a slow efficiency core cannot pick up critical-path work. Jobs submitted from outside a
// pool (the main thread, or a worker in the other pool) go through the pool's inbox.
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

// Counts outstanding jobs. add() before scheduling, the system signals as jobs finish.
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
  alignas(64) std::atomic<u32> remaining_{0};
};

struct JobSystemConfig {
  // 0 selects one worker per logical CPU of the class, minus one performance CPU reserved
  // for the calling (main) thread.
  u32 performance_workers = 0;
  u32 efficiency_workers = 0;
  bool pin_threads = true;
  u32 queue_capacity = 4096;  // per worker deque and per pool inbox; power of two
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
  u64 helper_runs = 0;    // executed by a non-worker thread while it waited (includes inline runs)
  u64 inline_runs = 0;    // executed by the submitter because every queue was full
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

  static void worker_main(JobSystem* self, Worker* worker);
  bool try_run_one(Worker* worker);
  bool try_run_one_from_pool(PoolState& pool, Worker* self_worker);
  void run_job(const Job& job, Worker* worker) noexcept;
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
};

}  // namespace engine::jobs

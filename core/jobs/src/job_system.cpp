#include <core/jobs/job_system.h>

#include <core/base/assert.h>
#include <core/containers/small_vector.h>
#include <core/jobs/detail/mpmc_queue.h>
#include <core/jobs/detail/work_deque.h>
#include <core/memory/memory.h>
#include <core/platform/thread.h>

#include <cstdio>
#include <memory>
#include <new>
#include <thread>

namespace engine::jobs {

namespace {

thread_local JobSystem* t_current_system = nullptr;
thread_local const WorkerInfo* t_current_worker = nullptr;

constexpr u32 k_max_pool_workers = 4096;

}  // namespace

struct alignas(64) JobSystem::Worker {
  WorkerInfo info;
  detail::WorkDeque<Job> deque;
  SmallVector<u16, 64> victims;  // indices into the pool's worker array, same-domain first
  u32 local_victim_count = 0;    // how many leading entries of `victims` share our cache domain
  u32 rng_state = 0x9E3779B9u;
  std::thread thread;
  alignas(64) u64 jobs_executed = 0;
  u64 steals_local = 0;
  u64 steals_remote = 0;
  u64 inbox_pops = 0;
  u64 sleeps = 0;

  explicit Worker(u32 queue_capacity) : deque(queue_capacity) {}

  u32 next_random() noexcept {
    // xorshift32; only used to vary the starting victim
    u32 x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
  }
};

// Sleeping uses an eventcount: a sleeper reads `epoch`, announces itself in `sleepers`,
// re-checks the queues, then waits for `epoch` to change. A submitter that sees sleepers > 0
// bumps `epoch` and notifies. Spurious notifications cost a loop iteration and nothing else,
// unlike a semaphore whose surplus tokens would accumulate.
struct alignas(64) JobSystem::PoolState {
  Pool pool = Pool::Performance;
  SmallVector<Worker*, 64> workers;
  detail::MpmcQueue<Job> inbox;
  alignas(64) std::atomic<u32> epoch{0};
  alignas(64) std::atomic<u32> sleepers{0};

  explicit PoolState(u32 inbox_capacity) : inbox(inbox_capacity) {}
};

// --- construction -----------------------------------------------------------------------------

JobSystem::JobSystem(const JobSystemConfig& config) : topo_(platform::topology()), config_(config) {
  ENGINE_VERIFY(config_.queue_capacity >= 2 && (config_.queue_capacity & (config_.queue_capacity - 1)) == 0,
                "JobSystem: queue_capacity must be a power of two");
  pools_ = static_cast<PoolState*>(mem::allocate(sizeof(PoolState) * static_cast<usize>(Pool::Count), alignof(PoolState)));
  for (u32 p = 0; p < static_cast<u32>(Pool::Count); ++p) {
    new (pools_ + p) PoolState(config_.queue_capacity);
    pools_[p].pool = static_cast<Pool>(p);
  }
  build_workers(config_);
  running_.store(true, std::memory_order_release);
  for (u32 i = 0; i < worker_total_; ++i) {
    Worker* w = workers_[i];
    w->thread = std::thread(&JobSystem::worker_main, this, w);
  }
}

JobSystem::~JobSystem() {
  running_.store(false, std::memory_order_release);
  for (u32 p = 0; p < static_cast<u32>(Pool::Count); ++p) {
    // Wake every worker in the pool, sleeping or about to sleep.
    pools_[p].epoch.fetch_add(1, std::memory_order_release);
    pools_[p].epoch.notify_all();
  }
  for (u32 i = 0; i < worker_total_; ++i) {
    if (workers_[i]->thread.joinable()) workers_[i]->thread.join();
  }
  for (u32 i = 0; i < worker_total_; ++i) {
    workers_[i]->~Worker();
    mem::deallocate(workers_[i], sizeof(Worker), alignof(Worker));
  }
  mem::deallocate(workers_, sizeof(Worker*) * worker_total_, alignof(Worker*));
  for (u32 p = 0; p < static_cast<u32>(Pool::Count); ++p) pools_[p].~PoolState();
  mem::deallocate(pools_, sizeof(PoolState) * static_cast<usize>(Pool::Count), alignof(PoolState));
}

void JobSystem::build_workers(const JobSystemConfig& config) {
  // Decide CPU assignments per pool.
  SmallVector<u16, 64> perf_cpus;
  SmallVector<u16, 64> eff_cpus;
  topo_.performance_cpus.for_each([&](u16 id) { perf_cpus.push_back(id); });
  topo_.efficiency_cpus.for_each([&](u16 id) { eff_cpus.push_back(id); });

  u32 perf_count = config.performance_workers;
  if (perf_count == 0) perf_count = perf_cpus.size() > 1 ? perf_cpus.size() - 1 : 1;  // leave one for the main thread
  if (perf_count > k_max_pool_workers) perf_count = k_max_pool_workers;

  u32 eff_count = config.efficiency_workers;
  bool eff_on_perf_cpus = false;
  if (eff_count == 0) {
    if (!eff_cpus.empty()) {
      eff_count = eff_cpus.size();
    } else {
      // No efficiency cores: a small background pool sharing the performance CPUs at lower
      // OS priority, so callers can always schedule background work to Pool::Efficiency.
      eff_count = perf_cpus.size() / 4 > 0 ? perf_cpus.size() / 4 : 1;
      eff_on_perf_cpus = true;
    }
  } else if (eff_cpus.empty()) {
    eff_on_perf_cpus = true;
  }
  if (eff_count > k_max_pool_workers) eff_count = k_max_pool_workers;

  worker_total_ = perf_count + eff_count;
  workers_ = static_cast<Worker**>(mem::allocate(sizeof(Worker*) * worker_total_, alignof(Worker*)));
  u32 next = 0;

  auto make_worker = [&](Pool pool, u16 index, u16 cpu) {
    auto* w = static_cast<Worker*>(mem::allocate(sizeof(Worker), alignof(Worker)));
    new (w) Worker(config.queue_capacity);
    w->info.index = index;
    w->info.pool = pool;
    w->info.cpu = cpu;
    w->info.cache_domain = cpu != platform::k_invalid_cpu ? topo_.cpus[cpu].cache_domain : 0;
    w->rng_state = 0x9E3779B9u ^ (static_cast<u32>(index) * 2654435761u + static_cast<u32>(pool) * 40503u);
    pools_[static_cast<u32>(pool)].workers.push_back(w);
    workers_[next++] = w;
  };

  // Performance workers take CPUs from the end of the list so CPU 0 (where the main thread
  // usually starts) is the one left free when perf_count == cpus - 1.
  for (u32 i = 0; i < perf_count; ++i) {
    const u16 cpu = perf_cpus.empty() ? platform::k_invalid_cpu
                                      : perf_cpus[(perf_cpus.size() - 1 - (i % perf_cpus.size()))];
    make_worker(Pool::Performance, static_cast<u16>(i), cpu);
  }
  for (u32 i = 0; i < eff_count; ++i) {
    u16 cpu = platform::k_invalid_cpu;
    if (eff_on_perf_cpus) {
      if (!perf_cpus.empty()) cpu = perf_cpus[perf_cpus.size() - 1 - (i % perf_cpus.size())];
    } else {
      cpu = eff_cpus[i % eff_cpus.size()];
    }
    make_worker(Pool::Efficiency, static_cast<u16>(i), cpu);
  }

  // Victim lists: same cache domain first, then the rest of the pool.
  for (u32 p = 0; p < static_cast<u32>(Pool::Count); ++p) {
    PoolState& pool = pools_[p];
    for (u32 i = 0; i < pool.workers.size(); ++i) {
      Worker* w = pool.workers[i];
      for (u32 j = 0; j < pool.workers.size(); ++j) {
        if (j != i && pool.workers[j]->info.cache_domain == w->info.cache_domain) w->victims.push_back(static_cast<u16>(j));
      }
      w->local_victim_count = w->victims.size();
      for (u32 j = 0; j < pool.workers.size(); ++j) {
        if (j != i && pool.workers[j]->info.cache_domain != w->info.cache_domain) w->victims.push_back(static_cast<u16>(j));
      }
    }
  }
}

// --- worker loop ------------------------------------------------------------------------------

void JobSystem::worker_main(JobSystem* self, Worker* worker) {
  t_current_system = self;
  t_current_worker = &worker->info;

  char name[32];
  std::snprintf(name, sizeof(name), "%s-%u", worker->info.pool == Pool::Performance ? "perf" : "eff", worker->info.index);
  platform::set_current_thread_name(name);
  if (self->config_.pin_threads && worker->info.cpu != platform::k_invalid_cpu) {
    platform::pin_current_thread(worker->info.cpu);
  }
  if (worker->info.pool == Pool::Efficiency) {
    platform::set_current_thread_priority(platform::ThreadPriority::BelowNormal);
  }

  PoolState& pool = self->pools_[static_cast<u32>(worker->info.pool)];
  const u32 spin_limit = self->config_.spin_iterations;

  while (self->running_.load(std::memory_order_acquire)) {
    if (self->try_run_one(worker)) continue;

    // Idle: spin a little, then sleep until a submitter wakes us.
    bool found = false;
    for (u32 spin = 0; spin < spin_limit; ++spin) {
      platform::pause_cpu();
      if ((spin & 63) == 63 && self->try_run_one(worker)) {
        found = true;
        break;
      }
    }
    if (found) continue;

    const u32 epoch = pool.epoch.load(std::memory_order_acquire);
    pool.sleepers.fetch_add(1, std::memory_order_acq_rel);
    // Re-check after announcing ourselves as a sleeper: a concurrent submit either sees the
    // sleeper count and bumps the epoch, or we see its job here.
    if (self->try_run_one(worker) || !self->running_.load(std::memory_order_acquire)) {
      pool.sleepers.fetch_sub(1, std::memory_order_acq_rel);
      continue;
    }
    ++worker->sleeps;
    pool.epoch.wait(epoch, std::memory_order_acquire);
    pool.sleepers.fetch_sub(1, std::memory_order_acq_rel);
  }

  t_current_worker = nullptr;
  t_current_system = nullptr;
}

void JobSystem::run_job(const Job& job, Worker* worker) noexcept {
  job.fn(job.data);
  if (job.counter != nullptr) job.counter->signal();
  if (worker != nullptr) {
    ++worker->jobs_executed;
  } else {
    helper_runs_.fetch_add(1, std::memory_order_relaxed);
  }
}

bool JobSystem::try_run_one(Worker* worker) {
  Job job;
  if (worker->deque.pop(job)) {
    run_job(job, worker);
    return true;
  }
  PoolState& pool = pools_[static_cast<u32>(worker->info.pool)];
  if (pool.inbox.try_pop(job)) {
    ++worker->inbox_pops;
    run_job(job, worker);
    return true;
  }
  // Steal: same cache domain first (random start), then remote.
  const u32 n = worker->victims.size();
  if (n == 0) return false;
  const u32 local = worker->local_victim_count;
  if (local > 0) {
    const u32 start = worker->next_random() % local;
    for (u32 k = 0; k < local; ++k) {
      Worker* victim = pool.workers[worker->victims[(start + k) % local]];
      if (victim->deque.steal(job)) {
        ++worker->steals_local;
        run_job(job, worker);
        return true;
      }
    }
  }
  const u32 remote = n - local;
  if (remote > 0) {
    const u32 start = worker->next_random() % remote;
    for (u32 k = 0; k < remote; ++k) {
      Worker* victim = pool.workers[worker->victims[local + (start + k) % remote]];
      if (victim->deque.steal(job)) {
        ++worker->steals_remote;
        run_job(job, worker);
        return true;
      }
    }
  }
  return false;
}

// Used by outside threads waiting on a counter: drain that pool's inbox and steal from its
// workers so the waiter contributes instead of idling.
bool JobSystem::try_run_one_from_pool(PoolState& pool, Worker* self_worker) {
  Job job;
  if (pool.inbox.try_pop(job)) {
    run_job(job, self_worker);
    return true;
  }
  for (u32 i = 0; i < pool.workers.size(); ++i) {
    if (pool.workers[i]->deque.steal(job)) {
      run_job(job, self_worker);
      return true;
    }
  }
  return false;
}

// --- submission -------------------------------------------------------------------------------

void JobSystem::wake_one(PoolState& pool) noexcept {
  if (pool.sleepers.load(std::memory_order_acquire) > 0) {
    pool.epoch.fetch_add(1, std::memory_order_release);
    pool.epoch.notify_one();
  }
}

void JobSystem::schedule(Pool pool_id, const Job& job) {
  ENGINE_ASSERT(job.fn != nullptr, "JobSystem::schedule: null job function");
  PoolState& pool = pools_[static_cast<u32>(pool_id)];
  const WorkerInfo* me = t_current_worker;
  if (me != nullptr && t_current_system == this && me->pool == pool_id) {
    Worker* w = pool.workers[me->index];
    if (w->deque.push(job)) {
      wake_one(pool);
      return;
    }
  }
  if (pool.inbox.try_push(job)) {
    wake_one(pool);
    return;
  }
  // Every queue is full: apply backpressure by running the job on the submitter.
  inline_runs_.fetch_add(1, std::memory_order_relaxed);
  run_job(job, nullptr);
}

void JobSystem::schedule(Pool pool_id, std::span<const Job> jobs, Counter& counter) {
  counter.add(static_cast<u32>(jobs.size()));
  PoolState& pool = pools_[static_cast<u32>(pool_id)];
  for (const Job& j : jobs) {
    Job job = j;
    job.counter = &counter;
    ENGINE_ASSERT(job.fn != nullptr, "JobSystem::schedule: null job function");
    const WorkerInfo* me = t_current_worker;
    bool queued = false;
    if (me != nullptr && t_current_system == this && me->pool == pool_id) {
      queued = pool.workers[me->index]->deque.push(job);
    }
    if (!queued) queued = pool.inbox.try_push(job);
    if (!queued) {
      inline_runs_.fetch_add(1, std::memory_order_relaxed);
      run_job(job, nullptr);
    }
  }
  // A batch wakes every sleeper; those that find nothing go back to sleep after a short spin.
  if (pool.sleepers.load(std::memory_order_acquire) > 0) {
    pool.epoch.fetch_add(1, std::memory_order_release);
    if (jobs.size() == 1) {
      pool.epoch.notify_one();
    } else {
      pool.epoch.notify_all();
    }
  }
}

// --- waiting ----------------------------------------------------------------------------------

void JobSystem::wait(Counter& counter) {
  if (counter.done()) return;
  const WorkerInfo* me = t_current_worker;
  if (me != nullptr && t_current_system == this) {
    Worker* w = pools_[static_cast<u32>(me->pool)].workers[me->index];
    while (!counter.done()) {
      if (!try_run_one(w)) {
        // Nothing runnable in our pool; the remaining jobs are in flight elsewhere.
        platform::pause_cpu();
      }
    }
    return;
  }
  // Outside thread (normally the main thread, itself a performance-class thread): help the
  // Performance pool while waiting so a parallel_for from the main thread uses it too. Never
  // run Efficiency work here; that would pull background jobs onto the critical thread.
  PoolState& perf = pools_[static_cast<u32>(Pool::Performance)];
  for (u32 spin = 0; spin < 256 && !counter.done(); ++spin) {
    if (!try_run_one_from_pool(perf, nullptr)) platform::pause_cpu();
  }
  counter.wait_blocking();
}

// --- introspection ----------------------------------------------------------------------------

u32 JobSystem::worker_count(Pool pool) const noexcept { return pools_[static_cast<u32>(pool)].workers.size(); }
u32 JobSystem::total_worker_count() const noexcept { return worker_total_; }

const WorkerInfo& JobSystem::worker_info(Pool pool, u32 index) const noexcept {
  ENGINE_ASSERT(index < worker_count(pool), "JobSystem::worker_info: index out of range");
  return pools_[static_cast<u32>(pool)].workers[index]->info;
}

const WorkerInfo* JobSystem::current_worker() noexcept { return t_current_worker; }
JobSystem* JobSystem::current() noexcept { return t_current_system; }

JobSystemStats JobSystem::stats() const noexcept {
  JobSystemStats s;
  for (u32 i = 0; i < worker_total_; ++i) {
    const Worker* w = workers_[i];
    s.jobs_executed += w->jobs_executed;
    s.steals_local += w->steals_local;
    s.steals_remote += w->steals_remote;
    s.inbox_pops += w->inbox_pops;
    s.sleeps += w->sleeps;
  }
  s.helper_runs = helper_runs_.load(std::memory_order_relaxed);
  s.jobs_executed += s.helper_runs;
  s.inline_runs = inline_runs_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace engine::jobs

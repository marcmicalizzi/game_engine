#include <core/base/assert.h>
#include <core/containers/small_vector.h>
#include <core/jobs/detail/mpmc_queue.h>
#include <core/jobs/detail/work_deque.h>
#include <core/jobs/job_system.h>
#include <core/memory/memory.h>
#include <core/platform/thread.h>
#include <core/profiling/profile.h>

#include <cstdio>
#include <memory>
#include <new>
#include <thread>

namespace engine::jobs {

namespace {

thread_local JobSystem* t_current_system = nullptr;
thread_local const WorkerInfo* t_current_worker = nullptr;
// Jobs on this thread's stack, and how many of them were lent to it by a wait for a group they do
// not belong to (at most one: JobSystem::wait).
thread_local u32 t_job_nesting = 0;
thread_local u32 t_lent_depth = 0;

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
//
// The guest deque is a worker's deque for the one outside thread that claims it first, normally
// the main thread: what it schedules goes there instead of the inbox, so a parallel_for it runs
// keeps its chunks at the end of a deque it pops itself, as a worker's are, rather than at the
// tail of an inbox behind every job queued before them (F26: a cold content build's main thread
// otherwise waited behind 1,800 queued texture builds for the rows of its own). Workers steal
// from it like any other deque. Other outside threads use the inbox.
struct alignas(64) JobSystem::PoolState {
  Pool pool = Pool::Performance;
  SmallVector<Worker*, 64> workers;
  detail::MpmcQueue<Job> inbox;
  detail::WorkDeque<Job> guest;
  alignas(64) std::atomic<const void*> guest_owner{nullptr};
  alignas(64) std::atomic<u32> epoch{0};
  alignas(64) std::atomic<u32> sleepers{0};

  explicit PoolState(u32 capacity) : inbox(capacity), guest(capacity) {}
};

namespace {
// The address that names this thread as a guest deque's owner.
thread_local char t_guest_token = 0;
}  // namespace

// The guest deque of `pool` when the calling outside thread owns it or can claim it, else null.
detail::WorkDeque<Job>* JobSystem::guest_of_caller(PoolState& pool) noexcept {
  const void* me = &t_guest_token;
  const void* owner = pool.guest_owner.load(std::memory_order_acquire);
  if (owner == me) return &pool.guest;
  if (owner == nullptr &&
      pool.guest_owner.compare_exchange_strong(owner, me, std::memory_order_acq_rel)) {
    return &pool.guest;
  }
  return nullptr;
}

// --- construction -----------------------------------------------------------------------------

JobSystem::JobSystem(const JobSystemConfig& config) : topo_(platform::topology()), config_(config) {
  ENGINE_VERIFY(
      config_.queue_capacity >= 2 && (config_.queue_capacity & (config_.queue_capacity - 1)) == 0,
      "JobSystem: queue_capacity must be a power of two");
  pools_ = static_cast<PoolState*>(
      mem::allocate(sizeof(PoolState) * static_cast<usize>(Pool::Count), alignof(PoolState)));
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
  for (u32 p = 0; p < static_cast<u32>(Pool::Count); ++p)
    pools_[p].~PoolState();
  mem::deallocate(pools_, sizeof(PoolState) * static_cast<usize>(Pool::Count), alignof(PoolState));
}

void JobSystem::build_workers(const JobSystemConfig& config) {
  // Decide CPU assignments per pool.
  SmallVector<u16, 64> perf_cpus;
  SmallVector<u16, 64> eff_cpus;
  topo_.performance_cpus.for_each([&](u16 id) { perf_cpus.push_back(id); });
  topo_.efficiency_cpus.for_each([&](u16 id) { eff_cpus.push_back(id); });

  u32 perf_count = config.performance_workers;
  if (perf_count == 0)
    perf_count = perf_cpus.size() > 1 ? perf_cpus.size() - 1 : 1;  // leave one for the main thread
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
  workers_ =
      static_cast<Worker**>(mem::allocate(sizeof(Worker*) * worker_total_, alignof(Worker*)));
  u32 next = 0;

  auto make_worker = [&](Pool pool, u16 index, u16 cpu) {
    auto* w = static_cast<Worker*>(mem::allocate(sizeof(Worker), alignof(Worker)));
    new (w) Worker(config.queue_capacity);
    w->info.index = index;
    w->info.pool = pool;
    w->info.cpu = cpu;
    w->info.cache_domain = cpu != platform::k_invalid_cpu ? topo_.cpus[cpu].cache_domain : 0;
    w->rng_state =
        0x9E3779B9u ^ (static_cast<u32>(index) * 2654435761u + static_cast<u32>(pool) * 40503u);
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
        if (j != i && pool.workers[j]->info.cache_domain == w->info.cache_domain)
          w->victims.push_back(static_cast<u16>(j));
      }
      w->local_victim_count = w->victims.size();
      for (u32 j = 0; j < pool.workers.size(); ++j) {
        if (j != i && pool.workers[j]->info.cache_domain != w->info.cache_domain)
          w->victims.push_back(static_cast<u16>(j));
      }
    }
  }
}

// --- worker loop ------------------------------------------------------------------------------

void JobSystem::worker_main(JobSystem* self, Worker* worker) {
  t_current_system = self;
  t_current_worker = &worker->info;

  char name[32];
  std::snprintf(name, sizeof(name), "%s-%u",
                worker->info.pool == Pool::Performance ? "perf" : "eff", worker->info.index);
  platform::set_current_thread_name(name);
  ENGINE_PROFILE_THREAD(name);
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
  ENGINE_PROFILE_ZONE_NAMED("job");
  // Every job reaching here was counted into its counter's `queued_` by schedule().
  if (job.counter != nullptr) job.counter->queued_.fetch_sub(1, std::memory_order_relaxed);
  const u32 nesting = ++t_job_nesting;
  u32 deepest = max_job_nesting_.load(std::memory_order_relaxed);
  while (nesting > deepest &&
         !max_job_nesting_.compare_exchange_weak(deepest, nesting, std::memory_order_relaxed)) {
  }
  job.fn(job.data);
  --t_job_nesting;
  // Count before signaling: a waiter released by the signal may read stats() at once, and the
  // counter's release/acquire pair is what makes the count visible to it.
  if (worker != nullptr) {
    ++worker->jobs_executed;
  } else {
    helper_runs_.fetch_add(1, std::memory_order_relaxed);
  }
  if (job.counter != nullptr) job.counter->signal();
}

// A job a thread took from a queue: from the worker loop (`waited` null) it is the thread's own
// work; from a wait it is lent unless it belongs to the group being waited for.
void JobSystem::run_taken(const Job& job, Worker* worker, const Counter* waited) noexcept {
  if (waited != nullptr) {
    run_helped(job, worker, *waited);
  } else {
    run_job(job, worker);
  }
}

// A job of the group being waited for is the waiter's own work and runs as a call would. Any
// other is lent to this stack, and while it is there every wait on this thread helps only its
// own group (help_own), so a stack carries at most one lent job.
void JobSystem::run_helped(const Job& job, Worker* worker, const Counter& waited) noexcept {
  if (job.counter == &waited) {
    run_job(job, worker);
    return;
  }
  foreign_helps_.fetch_add(1, std::memory_order_relaxed);
  ++t_lent_depth;
  run_job(job, worker);
  --t_lent_depth;
}

bool JobSystem::try_run_one(Worker* worker, const Counter* waited) {
  Job job;
  if (worker->deque.pop(job)) {
    run_taken(job, worker, waited);
    return true;
  }
  PoolState& pool = pools_[static_cast<u32>(worker->info.pool)];
  if (pool.inbox.try_pop(job)) {
    ++worker->inbox_pops;
    run_taken(job, worker, waited);
    return true;
  }
  // Steal: same cache domain first (random start), then remote.
  const u32 n = worker->victims.size();
  const u32 local = worker->local_victim_count;
  if (local > 0) {
    const u32 start = worker->next_random() % local;
    for (u32 k = 0; k < local; ++k) {
      Worker* victim = pool.workers[worker->victims[(start + k) % local]];
      if (victim->deque.steal(job)) {
        ++worker->steals_local;
        run_taken(job, worker, waited);
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
        run_taken(job, worker, waited);
        return true;
      }
    }
  }
  // Last, the outside thread's deque: what the main thread scheduled.
  if (pool.guest.steal(job)) {
    ++worker->steals_remote;
    run_taken(job, worker, waited);
    return true;
  }
  return false;
}

// Used by outside threads waiting on a counter: drain that pool's inbox and steal from its
// workers so the waiter contributes instead of idling.
bool JobSystem::try_run_one_from_pool(PoolState& pool, Worker* self_worker, const Counter& waited) {
  Job job;
  // The guest deque first when this thread owns it: its own newest work, a worker's order.
  detail_deque* guest = guest_of_caller(pool);
  if (guest != nullptr && guest->pop(job)) {
    run_helped(job, self_worker, waited);
    return true;
  }
  if (guest == nullptr && pool.guest.steal(job)) {
    run_helped(job, self_worker, waited);
    return true;
  }
  if (pool.inbox.try_pop(job)) {
    run_helped(job, self_worker, waited);
    return true;
  }
  for (u32 i = 0; i < pool.workers.size(); ++i) {
    if (pool.workers[i]->deque.steal(job)) {
      run_helped(job, self_worker, waited);
      return true;
    }
  }
  return false;
}

// The wait of a thread with a lent job on its stack: run `counter`'s own jobs and nothing else.
// A job of another group taken while looking for them is parked back in a queue, where every
// thread of the pool can still reach it, so refusing it can never strand it. The search goes
// where the group's jobs can be: the newest end of the caller's own deque (a parallel_for's
// chunks are pushed there, by a worker or by the guest), the pool's inbox (another outside
// thread's chunks, and any that found a deque full), cycled a bounded number of entries at a
// time, and, when the inbox is empty, the oldest end of the other deques (jobs scheduled by
// another thread).
bool JobSystem::help_own(Counter& counter, PoolState& pool, Worker* self_worker,
                         detail_deque* own) {
  constexpr u32 k_inbox_scan = 64;
  // Nothing of the group is queued: every job left is running somewhere, and there is nothing
  // this thread may take.
  if (counter.queued_.load(std::memory_order_acquire) == 0) return false;
  Job job;
  if (own != nullptr && own->pop(job)) {
    if (job.counter == &counter) {
      run_job(job, self_worker);
      return true;
    }
    park(pool, own, self_worker, job);
  }
  bool inbox_empty = false;
  for (u32 k = 0; k < k_inbox_scan; ++k) {
    if (!pool.inbox.try_pop(job)) {
      inbox_empty = true;
      break;
    }
    if (job.counter == &counter) {
      if (self_worker != nullptr) ++self_worker->inbox_pops;
      run_job(job, self_worker);
      return true;
    }
    park(pool, own, self_worker, job);
  }
  if (!inbox_empty) return false;
  const u32 victims = pool.workers.size();
  for (u32 i = 0; i <= victims; ++i) {
    detail_deque* victim = i < victims ? &pool.workers[i]->deque : &pool.guest;
    if (victim == own || !victim->steal(job)) continue;
    if (job.counter == &counter) {
      if (self_worker != nullptr) ++self_worker->steals_remote;
      run_job(job, self_worker);
      return true;
    }
    park(pool, own, self_worker, job);
  }
  return false;
}

// Puts back a job a bounded waiter took that was not its own: the inbox first, so any thread of
// the pool can take it, else the caller's own deque. When both are full it runs here, lent, the
// way schedule() runs a job on its submitter when every queue is full; that is the one way past
// the one-level bound, and `help_escapes` counts it.
void JobSystem::park(PoolState& pool, detail_deque* own, Worker* self_worker, const Job& job) {
  parked_.fetch_add(1, std::memory_order_relaxed);
  if (pool.inbox.try_push(job) || (own != nullptr && own->push(job))) {
    wake_one(pool);
    return;
  }
  help_escapes_.fetch_add(1, std::memory_order_relaxed);
  foreign_helps_.fetch_add(1, std::memory_order_relaxed);
  ++t_lent_depth;
  run_job(job, self_worker);
  --t_lent_depth;
}

// --- submission -------------------------------------------------------------------------------

void JobSystem::wake_one(PoolState& pool) noexcept {
  if (pool.sleepers.load(std::memory_order_acquire) > 0) {
    pool.epoch.fetch_add(1, std::memory_order_release);
    pool.epoch.notify_one();
  }
}

// The deque the calling thread pushes to for `pool`: its own when it is one of the pool's workers,
// the guest deque when it is an outside thread that owns it, else none (the inbox).
JobSystem::detail_deque* JobSystem::own_deque(PoolState& pool) noexcept {
  const WorkerInfo* me = t_current_worker;
  if (me != nullptr && t_current_system == this) {
    return me->pool == pool.pool ? &pool.workers[me->index]->deque : nullptr;
  }
  return guest_of_caller(pool);
}

void JobSystem::schedule(Pool pool_id, const Job& job) {
  ENGINE_ASSERT(job.fn != nullptr, "JobSystem::schedule: null job function");
  if (job.counter != nullptr) job.counter->queued_.fetch_add(1, std::memory_order_release);
  PoolState& pool = pools_[static_cast<u32>(pool_id)];
  detail_deque* own = own_deque(pool);
  if (own != nullptr && own->push(job)) {
    wake_one(pool);
    return;
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
  counter.queued_.fetch_add(static_cast<u32>(jobs.size()), std::memory_order_release);
  PoolState& pool = pools_[static_cast<u32>(pool_id)];
  detail_deque* own = own_deque(pool);
  for (const Job& j : jobs) {
    Job job = j;
    job.counter = &counter;
    ENGINE_ASSERT(job.fn != nullptr, "JobSystem::schedule: null job function");
    bool queued = own != nullptr && own->push(job);
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
  const bool is_worker = me != nullptr && t_current_system == this;
  if (t_lent_depth > 0) {
    // A lent job is on this stack: help only this counter's own group, so the stack never takes
    // a second lent job (docs/subsystems/jobs.md, "Helping is one level deep"). Backing off when
    // nothing of ours was found keeps a waiter whose jobs are all running from cycling the queues.
    PoolState& pool = pools_[static_cast<u32>(is_worker ? me->pool : Pool::Performance)];
    Worker* w = is_worker ? pool.workers[me->index] : nullptr;
    detail_deque* own = w != nullptr ? &w->deque : guest_of_caller(pool);
    u32 backoff = 1;
    u32 misses = 0;
    while (!counter.done()) {
      if (help_own(counter, pool, w, own)) {
        backoff = 1;
        continue;
      }
      // An outside thread blocks once nothing of its group is left to take, or after as many
      // fruitless looks as the unbounded wait below spins for (a group in the other pool).
      if (w == nullptr &&
          (++misses >= 256 || counter.queued_.load(std::memory_order_acquire) == 0)) {
        break;
      }
      for (u32 i = 0; i < backoff && !counter.done(); ++i)
        platform::pause_cpu();
      if (backoff < 1024) backoff *= 2;
    }
    counter.wait_blocking();
    return;
  }
  if (is_worker) {
    Worker* w = pools_[static_cast<u32>(me->pool)].workers[me->index];
    while (!counter.done()) {
      if (!try_run_one(w, &counter)) {
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
    if (!try_run_one_from_pool(perf, nullptr, counter)) platform::pause_cpu();
  }
  counter.wait_blocking();
}

// --- introspection ----------------------------------------------------------------------------

u32 JobSystem::worker_count(Pool pool) const noexcept {
  return pools_[static_cast<u32>(pool)].workers.size();
}
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
  s.foreign_helps = foreign_helps_.load(std::memory_order_relaxed);
  s.parked = parked_.load(std::memory_order_relaxed);
  s.help_escapes = help_escapes_.load(std::memory_order_relaxed);
  s.max_job_nesting = max_job_nesting_.load(std::memory_order_relaxed);
  return s;
}

}  // namespace engine::jobs

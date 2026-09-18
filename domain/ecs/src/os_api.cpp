#include <core/base/assert.h>
#include <core/log/log.h>
#include <core/platform/thread.h>
#include <domain/ecs/os_api.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace engine::ecs {

ENGINE_LOG_CATEGORY_DEFINE(log_ecs, "ecs");

// The adapter is a process-wide hook because flecs' OS API is a process-wide global; the pointer
// is atomic so that installing one while a world ticks is at least well-defined rather than a
// data race, not because doing that is a good idea.
namespace {
std::atomic<JobOsApi*> g_current{nullptr};
std::atomic<bool> g_os_api_installed{false};
}  // namespace

struct JobOsApi::Slot {
  jobs::Counter counter;
  void* (*fn)(void*) = nullptr;
  void* param = nullptr;
  void* result = nullptr;
  // The world whose worker this is, for a long-running one; null for a per-tick task, which is
  // gone again by the end of the tick and is never accounted against the budget.
  ecs_world_t* world = nullptr;
  std::atomic<bool> in_use{false};
};

namespace {

void run_worker(void* data) {
  auto* slot = static_cast<JobOsApi::Slot*>(data);
  // Name the pool thread for as long as flecs holds it, so a profiler and a debugger show what
  // is actually on it. The pool's own name goes back on when the worker returns.
  const jobs::WorkerInfo* info = jobs::JobSystem::current_worker();
  char name[32];
  if (slot->world != nullptr && info != nullptr) {
    std::snprintf(name, sizeof(name), "ecs-worker-%u", static_cast<unsigned>(info->index));
    platform::set_current_thread_name(name);
  }
  slot->result = slot->fn(slot->param);
  if (slot->world != nullptr && info != nullptr) {
    std::snprintf(name, sizeof(name), "%s-%u",
                  info->pool == jobs::Pool::Performance ? "perf" : "eff",
                  static_cast<unsigned>(info->index));
    platform::set_current_thread_name(name);
  }
}

ecs_os_thread_t thread_new(ecs_os_thread_callback_t callback, void* param) {
  JobOsApi* api = JobOsApi::current();
  if (api == nullptr) return 0;
  return static_cast<ecs_os_thread_t>(api->start_worker(callback, param, true));
}

ecs_os_thread_t task_new(ecs_os_thread_callback_t callback, void* param) {
  JobOsApi* api = JobOsApi::current();
  if (api == nullptr) return 0;
  return static_cast<ecs_os_thread_t>(api->start_worker(callback, param, false));
}

void* worker_join(ecs_os_thread_t thread) {
  JobOsApi* api = JobOsApi::current();
  if (api == nullptr) return nullptr;
  return api->join_worker(static_cast<u64>(thread));
}

// flecs levels: 0 is its trace, 1..3 are increasing debug detail, -2 warning, -3 error, -4 fatal.
log::Level level_from_flecs(i32 level) noexcept {
  if (level >= 1) return log::Level::Debug;
  if (level == 0) return log::Level::Trace;
  if (level == -2) return log::Level::Warn;
  if (level == -3) return log::Level::Error;
  return log::Level::Fatal;
}

void log_message(int32_t level, const char* file, int32_t line, const char* msg) {
  // The message is flecs' own formatted text, so it goes in a field rather than being the
  // record's message: the record's message stays the static string a query can group by.
  const log::Field fields[] = {log::field("text", msg != nullptr ? msg : "")};
  log::emit(log_ecs, level_from_flecs(static_cast<i32>(level)), file, static_cast<u32>(line),
            "flecs", fields);
}

void install_os_api() {
  bool expected = false;
  if (!g_os_api_installed.compare_exchange_strong(expected, true)) return;
  // Fills every hook flecs ships with (FLECS_OS_API_IMPL), then replaces the ones the engine
  // owns. The heap, time, mutex and condition-variable hooks stay flecs': they are thin
  // platform wrappers with nothing engine-specific to gain, and the worker threads that wait on
  // those condition variables are flecs' own state machine, not ours.
  ecs_os_init();
  // The hooks are assigned on the public `ecs_os_api` global rather than through
  // ecs_os_set_api(), which is one-shot: it refuses once anything has claimed the API, and what
  // claims it first depends on whether a flecs::world or this adapter was constructed first.
  // Assigning the fields is idempotent and order-independent, and leaves every other hook —
  // heap, time, mutexes, condition variables — as flecs shipped it.
  //
  // Both pairs are filled in. The thread pair is what `WorkerHosting::Threads` uses and is the
  // default; the task pair is what the measured-and-rejected per-tick hosting uses and is kept
  // so the comparison that rejected it can still be run.
  ecs_os_api.thread_new_ = &thread_new;
  ecs_os_api.thread_join_ = &worker_join;
  ecs_os_api.task_new_ = &task_new;
  ecs_os_api.task_join_ = &worker_join;
  ecs_os_api.log_ = &log_message;
  ecs_os_api.flags_ &=
      ~static_cast<ecs_flags32_t>(EcsOsApiLogWithColors);  // core/log does the styling
}

}  // namespace

void install_log_sink() { install_os_api(); }

void set_flecs_log_level(i32 level) {
  install_os_api();
  ecs_log_set_level(static_cast<int>(level));
}

JobOsApi::JobOsApi(jobs::JobSystem& jobs, const JobOsApiConfig& config) : jobs_(&jobs) {
  install_os_api();
  // One slot per performance worker: nothing can ever be running more flecs workers than the
  // pool has threads, because the budget is capped at the pool's worker count.
  slot_capacity_ = jobs.worker_count(jobs::Pool::Performance);
  if (slot_capacity_ == 0) slot_capacity_ = 1;
  slots_ = std::make_unique<Slot[]>(slot_capacity_);
  budget_ = config.hosted_worker_budget;
  if (budget_ == 0) budget_ = slot_capacity_ > 1 ? slot_capacity_ - 1 : 0;
  if (budget_ > slot_capacity_) budget_ = slot_capacity_;
  previous_ = g_current.exchange(this, std::memory_order_acq_rel);
  ENGINE_LOG_DEBUG(log_ecs, "flecs workers on the job system",
                   log::field("performance_workers", slot_capacity_),
                   log::field("hosted_worker_budget", budget_));
}

JobOsApi::~JobOsApi() {
  // Take back anything still out. A pool thread parked inside flecs' sync protocol cannot be
  // joined by the job system, so leaving one there turns the job system's own destructor into a
  // deadlock; dropping the world to one worker instead leaves it ticking single-threaded, which
  // is a degradation a caller can survive and read about in the log.
  for (;;) {
    ecs_world_t* world = nullptr;
    for (u32 i = 0; i < slot_capacity_; ++i) {
      if (slots_[i].in_use.load(std::memory_order_acquire) && slots_[i].world != nullptr) {
        world = slots_[i].world;
        break;
      }
    }
    if (world == nullptr) break;
    ENGINE_LOG_ERROR(log_ecs, "job adapter destroyed while a world still held pool workers",
                     log::field("hosted", hosted_.load(std::memory_order_relaxed)));
    ecs_set_threads(world, 1);
  }
  g_current.store(previous_, std::memory_order_release);
}

JobOsApi* JobOsApi::current() noexcept { return g_current.load(std::memory_order_acquire); }

u32 JobOsApi::hosted_workers() const noexcept { return hosted_.load(std::memory_order_acquire); }

u32 JobOsApi::pool_workers_kept() const noexcept {
  const u32 hosted = hosted_workers();
  return slot_capacity_ > hosted ? slot_capacity_ - hosted : 0;
}

u32 JobOsApi::max_workers() const noexcept {
  // The calling thread plus whatever the budget has left. The caller is counted because it runs
  // stage 0 itself; the pool only has to produce the other stages.
  const u32 hosted = hosted_workers();
  return 1 + (budget_ > hosted ? budget_ - hosted : 0);
}

u64 JobOsApi::tasks_started() const noexcept {
  return tasks_started_.load(std::memory_order_relaxed);
}

u64 JobOsApi::workers_started() const noexcept {
  return workers_started_.load(std::memory_order_relaxed);
}

bool JobOsApi::reserve(ecs_world_t* world, u32 workers) noexcept {
  u32 hosted = hosted_.load(std::memory_order_acquire);
  for (;;) {
    if (hosted + workers > budget_) return false;
    if (hosted_.compare_exchange_weak(hosted, hosted + workers, std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
      break;
    }
  }
  reserving_ = world;
  return true;
}

void JobOsApi::end_reservation() noexcept { reserving_ = nullptr; }

void JobOsApi::release_one(ecs_world_t* world) noexcept {
  if (world == nullptr) return;
  hosted_.fetch_sub(1, std::memory_order_acq_rel);
}

u64 JobOsApi::start_worker(void* (*callback)(void*), void* param, bool persistent) noexcept {
  if (persistent) {
    workers_started_.fetch_add(1, std::memory_order_relaxed);
  } else {
    tasks_started_.fetch_add(1, std::memory_order_relaxed);
  }
  for (u32 i = 0; i < slot_capacity_; ++i) {
    Slot& slot = slots_[i];
    bool expected = false;
    if (!slot.in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) continue;
    slot.fn = callback;
    slot.param = param;
    slot.result = nullptr;
    slot.world = persistent ? reserving_ : nullptr;
    slot.counter.add(1);
    jobs_->schedule(jobs::Pool::Performance, jobs::Job{&run_worker, &slot, &slot.counter});
    return static_cast<u64>(reinterpret_cast<std::uintptr_t>(&slot));
  }
  // Unreachable through set_workers, which refuses a count the budget cannot give. Reaching it
  // means something called ecs_set_threads() behind the adapter's back, and returning 0 would
  // hang the next tick in flecs' spin for a worker that will never announce itself. Saying so
  // and stopping is the lesser failure.
  ENGINE_LOG_ERROR(log_ecs, "no free flecs worker slot", log::field("capacity", slot_capacity_),
                   log::field("hosted", hosted_.load(std::memory_order_relaxed)));
  ENGINE_VERIFY(false,
                "ecs::JobOsApi: flecs asked for more workers than the pool can host; "
                "use ecs::set_workers, which refuses instead");
  return 0;
}

void* JobOsApi::join_worker(u64 handle) noexcept {
  auto* slot = reinterpret_cast<Slot*>(static_cast<std::uintptr_t>(handle));
  if (slot == nullptr) return nullptr;
  // Blocking rather than helping: JobSystem::wait would run other performance-pool jobs on this
  // thread while it waits, and one of those could be another flecs worker, which would put the
  // thread inside flecs' own sync protocol and deadlock it.
  slot->counter.wait_blocking();
  void* result = slot->result;
  ecs_world_t* world = slot->world;
  slot->world = nullptr;
  slot->in_use.store(false, std::memory_order_release);
  release_one(world);
  return result;
}

void stop_workers(flecs::world& world) {
  // Works for either hosting: flecs joins whichever kind of worker the world actually has, and
  // a world that already has one stage does nothing at all.
  ecs_set_threads(world.c_ptr(), 1);
}

bool set_workers(flecs::world& world, u32 workers, WorkerHosting hosting) {
  if (workers == 0) workers = 1;
  // Give back whatever this world already holds, so the admission test below is against the
  // pool's real remaining capacity and reconfiguring a world is never counted twice.
  stop_workers(world);
  if (workers == 1) return true;

  JobOsApi* api = JobOsApi::current();
  if (api == nullptr) {
    ENGINE_LOG_WARN(log_ecs, "no job adapter installed; world stays single-threaded",
                    log::field("requested", workers));
    return false;
  }
  if (workers > api->max_workers()) {
    ENGINE_LOG_WARN(log_ecs, "worker count above what the pool can host",
                    log::field("requested", workers), log::field("max", api->max_workers()),
                    log::field("budget", api->hosted_worker_budget()),
                    log::field("hosted", api->hosted_workers()));
    return false;
  }

  if (hosting == WorkerHosting::Tasks) {
    // The per-tick form. It takes no lasting budget — every worker it makes is gone by the end
    // of the tick — but it is bounded by the same number, because during a tick it occupies
    // exactly as many pool workers as the long-running form does.
    ecs_set_task_threads(world.c_ptr(), static_cast<i32>(workers));
    return true;
  }

  if (!api->reserve(world.c_ptr(), workers - 1)) {
    ENGINE_LOG_WARN(log_ecs, "performance pool cannot lend that many workers",
                    log::field("requested", workers),
                    log::field("budget", api->hosted_worker_budget()),
                    log::field("hosted", api->hosted_workers()));
    return false;
  }
  ecs_set_threads(world.c_ptr(), static_cast<i32>(workers));
  api->end_reservation();
  return true;
}

}  // namespace engine::ecs

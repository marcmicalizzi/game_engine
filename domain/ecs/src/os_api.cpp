#include <core/base/assert.h>
#include <core/log/log.h>
#include <domain/ecs/os_api.h>

#include <atomic>
#include <cstdint>

namespace engine::ecs {

ENGINE_LOG_CATEGORY_DEFINE(log_ecs, "ecs");

// The adapter is a process-wide hook because flecs' OS API is a process-wide global; the pointer
// is atomic so that installing one while a world ticks is at least well-defined rather than a
// data race, not because doing that is a good idea.
namespace {
std::atomic<JobOsApi*> g_current{nullptr};
std::atomic<bool> g_os_api_installed{false};
}  // namespace

struct JobOsApi::Task {
  jobs::Counter counter;
  void* (*fn)(void*) = nullptr;
  void* param = nullptr;
  void* result = nullptr;
  std::atomic<bool> in_use{false};
};

namespace {

void run_task(void* data) {
  auto* task = static_cast<JobOsApi::Task*>(data);
  task->result = task->fn(task->param);
}

ecs_os_thread_t task_new(ecs_os_thread_callback_t callback, void* param) {
  JobOsApi* api = JobOsApi::current();
  if (api == nullptr) return 0;
  return static_cast<ecs_os_thread_t>(api->start_task(callback, param));
}

void* task_join(ecs_os_thread_t thread) {
  JobOsApi* api = JobOsApi::current();
  if (api == nullptr) return nullptr;
  return api->join_task(static_cast<u64>(thread));
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
  // Fills every hook flecs ships with (FLECS_OS_API_IMPL), then replaces the three the engine
  // owns. The heap, time, mutex and condition-variable hooks stay flecs': they are thin
  // platform wrappers with nothing engine-specific to gain, and the worker threads that wait on
  // those condition variables are flecs' own state machine, not ours.
  ecs_os_init();
  // The three hooks are assigned on the public `ecs_os_api` global rather than through
  // ecs_os_set_api(), which is one-shot: it refuses once anything has claimed the API, and what
  // claims it first depends on whether a flecs::world or this adapter was constructed first.
  // Assigning the fields is idempotent and order-independent, and leaves every other hook —
  // heap, time, mutexes, condition variables — as flecs shipped it.
  ecs_os_api.task_new_ = &task_new;
  ecs_os_api.task_join_ = &task_join;
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

JobOsApi::JobOsApi(jobs::JobSystem& jobs) : jobs_(&jobs) {
  install_os_api();
  // One task slot per performance worker: flecs never asks for more, because set_workers()
  // refuses a count above max_workers().
  task_capacity_ = jobs.worker_count(jobs::Pool::Performance);
  if (task_capacity_ == 0) task_capacity_ = 1;
  tasks_ = std::make_unique<Task[]>(task_capacity_);
  previous_ = g_current.exchange(this, std::memory_order_acq_rel);
  ENGINE_LOG_DEBUG(log_ecs, "flecs workers on the job system",
                   log::field("performance_workers", task_capacity_));
}

JobOsApi::~JobOsApi() { g_current.store(previous_, std::memory_order_release); }

JobOsApi* JobOsApi::current() noexcept { return g_current.load(std::memory_order_acquire); }

u32 JobOsApi::max_workers() const noexcept {
  // The pool's worker count, not that plus one for the caller: the caller may itself be a pool
  // worker, and it blocks in join_task for the length of the tick, so counting it would let a
  // tick ask for one more runnable thread than the pool can actually produce.
  const u32 workers = jobs_->worker_count(jobs::Pool::Performance);
  return workers == 0 ? 1 : workers;
}

u64 JobOsApi::tasks_started() const noexcept {
  return tasks_started_.load(std::memory_order_relaxed);
}

u64 JobOsApi::start_task(void* (*callback)(void*), void* param) noexcept {
  tasks_started_.fetch_add(1, std::memory_order_relaxed);
  for (u32 i = 0; i < task_capacity_; ++i) {
    Task& task = tasks_[i];
    bool expected = false;
    if (!task.in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) continue;
    task.fn = callback;
    task.param = param;
    task.result = nullptr;
    task.counter.add(1);
    jobs_->schedule(jobs::Pool::Performance, jobs::Job{&run_task, &task, &task.counter});
    return static_cast<u64>(reinterpret_cast<std::uintptr_t>(&task));
  }
  ENGINE_LOG_ERROR(log_ecs, "no free flecs worker slot", log::field("capacity", task_capacity_));
  return 0;  // flecs asserts on this; set_workers() is what keeps it from happening
}

void* JobOsApi::join_task(u64 handle) noexcept {
  auto* task = reinterpret_cast<Task*>(static_cast<std::uintptr_t>(handle));
  if (task == nullptr) return nullptr;
  // Blocking rather than helping: JobSystem::wait would run other performance-pool jobs on this
  // thread while it waits, and one of those could be another flecs worker, which would put the
  // thread inside flecs' own sync protocol and deadlock it.
  task->counter.wait_blocking();
  void* result = task->result;
  task->in_use.store(false, std::memory_order_release);
  return result;
}

bool set_workers(flecs::world& world, u32 workers) {
  if (workers <= 1) {
    ecs_set_task_threads(world.c_ptr(), 1);
    return true;
  }
  JobOsApi* api = JobOsApi::current();
  if (api == nullptr) {
    ENGINE_LOG_WARN(log_ecs, "no job adapter installed; world stays single-threaded",
                    log::field("requested", workers));
    return false;
  }
  if (workers > api->max_workers()) {
    ENGINE_LOG_WARN(log_ecs, "worker count above the performance pool",
                    log::field("requested", workers), log::field("max", api->max_workers()));
    return false;
  }
  ecs_set_task_threads(world.c_ptr(), static_cast<i32>(workers));
  return true;
}

}  // namespace engine::ecs

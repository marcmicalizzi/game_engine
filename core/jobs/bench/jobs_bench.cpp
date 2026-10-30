// The job system's two costs a caller sees (docs/subsystems/jobs.md): a parallel_for's submit and
// join from the main thread, and a pool saturated with jobs that each wait on parallel_fors of
// their own — the content build's texture step, where how a wait helps decides both the time and
// how deep a stack goes (F26), so a change to the helping rule shows here what it cost in time.
// How deep it went is the tests' to check (tests/help_depth_tests.cpp).

#include <core/jobs/job_system.h>
#include <foundation/bench/bench.h>

#include <atomic>

using namespace engine;
using namespace engine::jobs;

namespace {

JobSystemConfig config_for(u32 performance_workers) {
  JobSystemConfig config;
  config.performance_workers = performance_workers;
  config.efficiency_workers = 1;
  return config;
}

struct Saturated {
  JobSystem* js = nullptr;
  std::atomic<u32> cells{0};
};

void cell_work(Saturated& load) {
  volatile u32 x = 0;
  for (u32 i = 0; i < 4000; ++i)
    x = x + i;
  load.cells.fetch_add(1, std::memory_order_relaxed);
}

// A texture build's shape: rows in chunks, each row a little work.
void texture_like(void* p) {
  Saturated& load = *static_cast<Saturated*>(p);
  load.js->parallel_for(Pool::Performance, 64, 4, [&load](u32 begin, u32 end) {
    for (u32 row = begin; row < end; ++row)
      cell_work(load);
  });
}

}  // namespace

// Submit and join of a small parallel_for from the main thread: the overhead floor.
ENGINE_BENCH_ARGS(jobs_parallel_for_small, "jobs.parallel_for.small", 1, 8) {
  JobSystem js(config_for(static_cast<u32>(state.arg())));
  std::atomic<u32> sum{0};
  while (state.keep_running()) {
    js.parallel_for(Pool::Performance, 1024, 16, [&sum](u32 begin, u32 end) {
      sum.fetch_add(end - begin, std::memory_order_relaxed);
    });
  }
  bench::keep(sum.load());
  state.set_items(1024);
}

// 64 texture-like jobs scheduled from the main thread and waited on, each a parallel_for of 16
// chunks: the pool saturated with jobs whose waits nest.
ENGINE_BENCH_ARGS(jobs_nested_saturated, "jobs.nested.saturated", 1, 8) {
  JobSystem js(config_for(static_cast<u32>(state.arg())));
  Saturated load;
  load.js = &js;
  constexpr u32 k_textures = 64;
  Job jobs[k_textures];
  for (Job& job : jobs)
    job = Job{texture_like, &load, nullptr};
  while (state.keep_running()) {
    Counter counter;
    js.schedule(Pool::Performance, std::span<const Job>(jobs, k_textures), counter);
    js.wait(counter);
  }
  bench::keep(load.cells.load());
  state.set_items(k_textures);
}

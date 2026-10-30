// Helping is one level deep (F26, docs/subsystems/jobs.md "Helping is one level deep").
//
// The shape is the cold content build that overflowed its main thread's stack: the main thread
// schedules many independent "texture" jobs and waits on them; each texture job runs a
// parallel_for over its rows, and each row a parallel_for of its own. A wait that lends its
// thread to *any* job of the pool runs another texture job on top of the one it is inside, whose
// own wait runs a third, and so on as deep as the pool has textures queued — one texture build
// per level until the stack is gone. The bound says a stack holds the job it is running plus at
// most one job lent to it by a wait, whatever else it runs being that job's own work.

#include <core/jobs/job_system.h>

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>

using namespace engine;
using namespace engine::jobs;

namespace {

constexpr u32 k_textures = 64;
constexpr u32 k_rows = 32;
constexpr u32 k_cells = 4;

// Texture jobs on this thread's stack, and the address of a local in the outermost one: the
// stack probe. A thread's stack grows down on every platform the engine builds for.
thread_local u32 t_textures_on_stack = 0;
thread_local std::uintptr_t t_outermost = 0;

struct Load {
  JobSystem* js = nullptr;
  std::atomic<u32> deepest{0};
  std::atomic<std::uintptr_t> widest{0};
  std::atomic<u32> cells{0};
};

void raise(std::atomic<u32>& to, u32 value) {
  u32 seen = to.load(std::memory_order_relaxed);
  while (value > seen && !to.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

void raise(std::atomic<std::uintptr_t>& to, std::uintptr_t value) {
  std::uintptr_t seen = to.load(std::memory_order_relaxed);
  while (value > seen && !to.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

// A little work per cell, so that a waiter regularly finds its own chunks taken and running
// elsewhere: the moment at which an unbounded wait reaches for the next texture.
void work(Load& load) {
  volatile u32 x = 0;
  for (u32 i = 0; i < 2000; ++i)
    x = x + i;
  load.cells.fetch_add(1, std::memory_order_relaxed);
}

void texture_job(void* p) {
  Load& load = *static_cast<Load*>(p);
  volatile char probe = 0;
  const auto here = reinterpret_cast<std::uintptr_t>(&probe);
  const u32 depth = ++t_textures_on_stack;
  if (depth == 1) {
    t_outermost = here;
  } else if (t_outermost > here) {
    raise(load.widest, t_outermost - here);
  }
  raise(load.deepest, depth);
  load.js->parallel_for(Pool::Performance, k_rows, 1, [&load](u32 begin, u32 end) {
    for (u32 row = begin; row < end; ++row) {
      load.js->parallel_for(Pool::Performance, k_cells, 1, [&load](u32 b, u32 e) {
        for (u32 c = b; c < e; ++c)
          work(load);
      });
    }
  });
  --t_textures_on_stack;
}

void run_saturated(u32 performance_workers) {
  JobSystemConfig config;
  config.performance_workers = performance_workers;
  config.efficiency_workers = 1;
  JobSystem js(config);
  Load load;
  load.js = &js;
  Job jobs[k_textures];
  for (Job& job : jobs)
    job = Job{texture_job, &load, nullptr};
  for (u32 round = 0; round < 3; ++round) {
    Counter counter;
    js.schedule(Pool::Performance, std::span<const Job>(jobs, k_textures), counter);
    js.wait(counter);
  }
  const JobSystemStats stats = js.stats();
  MESSAGE("workers " << performance_workers << ": texture jobs on one stack at most "
                     << load.deepest.load() << ", widest probe span " << load.widest.load()
                     << " bytes, foreign helps " << stats.foreign_helps << ", parked "
                     << stats.parked << ", escapes " << stats.help_escapes
                     << ", jobs on one stack at most " << stats.max_job_nesting);
  CHECK(load.cells.load() == 3 * k_textures * k_rows * k_cells);
  // The job a thread is running, plus at most one lent to it by a wait.
  CHECK(load.deepest.load() <= 2);
  CHECK(stats.help_escapes == 0);
}

}  // namespace

TEST_CASE("JobSystem: a wait inside a lent job helps only its own group (F26)") {
  SUBCASE("eight workers, as the content build runs") { run_saturated(8); }
  SUBCASE("one worker") { run_saturated(1); }
}

#include <core/jobs/job_system.h>

#include <core/platform/thread.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::jobs;

namespace {

struct CounterJob {
  std::atomic<u32>* hits;
};

void hit(void* p) { static_cast<CounterJob*>(p)->hits->fetch_add(1, std::memory_order_relaxed); }

}  // namespace

TEST_CASE("JobSystem: starts and stops cleanly, repeatedly, and reports its layout") {
  for (int round = 0; round < 3; ++round) {
    JobSystem js;
    CHECK(js.worker_count(Pool::Performance) >= 1);
    CHECK(js.worker_count(Pool::Efficiency) >= 1);
    CHECK(js.total_worker_count() == js.worker_count(Pool::Performance) + js.worker_count(Pool::Efficiency));
    if (round == 0) {
      MESSAGE("performance workers: " << js.worker_count(Pool::Performance)
                                      << ", efficiency workers: " << js.worker_count(Pool::Efficiency)
                                      << ", cache domains: " << js.topology().cache_domains.size());
    }
    for (u32 i = 0; i < js.worker_count(Pool::Performance); ++i) {
      const WorkerInfo& w = js.worker_info(Pool::Performance, i);
      CHECK(w.index == i);
      CHECK(w.pool == Pool::Performance);
    }
  }
  CHECK(JobSystem::current() == nullptr);
  CHECK(JobSystem::current_worker() == nullptr);
}

TEST_CASE("JobSystem: every scheduled job runs exactly once") {
  JobSystem js;
  for (Pool pool : {Pool::Performance, Pool::Efficiency}) {
    std::atomic<u32> hits{0};
    CounterJob ctx{&hits};
    constexpr u32 k_jobs = 20000;
    std::vector<Job> jobs(k_jobs, Job{hit, &ctx, nullptr});
    Counter counter;
    js.schedule(pool, std::span<const Job>(jobs.data(), jobs.size()), counter);
    js.wait(counter);
    CHECK(hits.load() == k_jobs);
    CHECK(counter.done());
  }
  const JobSystemStats s = js.stats();
  CHECK(s.jobs_executed == 40000);
}

TEST_CASE("JobSystem: single-job schedule with a manually managed counter") {
  JobSystem js;
  std::atomic<u32> hits{0};
  CounterJob ctx{&hits};
  Counter counter;
  for (int i = 0; i < 1000; ++i) {
    counter.add();
    js.schedule(Pool::Performance, Job{hit, &ctx, &counter});
  }
  js.wait(counter);
  CHECK(hits.load() == 1000);
}

TEST_CASE("JobSystem: parallel_for covers the range exactly once and runs on workers") {
  JobSystem js;
  constexpr u32 k_n = 1'000'003;
  std::vector<std::atomic<u8>> seen(k_n);
  std::atomic<u32> on_worker{0};
  std::atomic<u32> chunks{0};
  js.parallel_for(Pool::Performance, k_n, 4096, [&](u32 begin, u32 end) {
    chunks.fetch_add(1);
    if (JobSystem::current_worker() != nullptr) on_worker.fetch_add(1);
    for (u32 i = begin; i < end; ++i) seen[i].fetch_add(1);
  });
  for (u32 i = 0; i < k_n; ++i) REQUIRE(seen[i].load() == 1);
  CHECK(chunks.load() == (k_n + 4095) / 4096);
  // The main thread helps while waiting, so not every chunk is on a worker; most should be
  // when there is more than one worker.
  if (js.worker_count(Pool::Performance) > 1) CHECK(on_worker.load() > 0);
}

TEST_CASE("JobSystem: parallel_for reduction") {
  JobSystem js;
  constexpr u32 k_n = 1 << 20;
  std::vector<u64> partial(js.worker_count(Pool::Performance) + 1, 0);
  std::atomic<u64> total{0};
  js.parallel_for(Pool::Performance, k_n, 1024, [&](u32 begin, u32 end) {
    u64 local = 0;
    for (u32 i = begin; i < end; ++i) local += i;
    total.fetch_add(local, std::memory_order_relaxed);
  });
  CHECK(total.load() == static_cast<u64>(k_n) * (k_n - 1) / 2);
}

TEST_CASE("JobSystem: nested scheduling from inside jobs, and waiting on a worker helps") {
  JobSystem js;
  struct Outer {
    JobSystem* js;
    std::atomic<u32>* hits;
  };
  std::atomic<u32> hits{0};
  Outer outer{&js, &hits};
  auto outer_fn = [](void* p) {
    auto* o = static_cast<Outer*>(p);
    CounterJob inner{o->hits};
    std::vector<Job> inner_jobs(64, Job{hit, &inner, nullptr});
    Counter inner_counter;
    o->js->schedule(Pool::Performance, std::span<const Job>(inner_jobs.data(), inner_jobs.size()), inner_counter);
    o->js->wait(inner_counter);  // worker waits by running other jobs
    o->hits->fetch_add(1000);
  };
  std::vector<Job> outers(32, Job{outer_fn, &outer, nullptr});
  Counter counter;
  js.schedule(Pool::Performance, std::span<const Job>(outers.data(), outers.size()), counter);
  js.wait(counter);
  CHECK(hits.load() == 32 * 64 + 32 * 1000);
}

TEST_CASE("JobSystem: cross-pool scheduling works from workers and outside") {
  JobSystem js;
  struct Ctx {
    JobSystem* js;
    std::atomic<u32>* hits;
    Counter* counter;
    CounterJob inner;
  };
  std::atomic<u32> hits{0};
  Counter eff_counter;
  Ctx ctx{&js, &hits, &eff_counter, CounterJob{&hits}};
  auto perf_fn = [](void* p) {
    auto* c = static_cast<Ctx*>(p);
    // Performance jobs run on Performance workers or on the waiting main thread (null).
    const WorkerInfo* me = JobSystem::current_worker();
    CHECK((me == nullptr || me->pool == Pool::Performance));
    c->counter->add();
    c->js->schedule(Pool::Efficiency, Job{hit, &c->inner, c->counter});
  };
  std::vector<Job> perf_jobs(100, Job{perf_fn, &ctx, nullptr});
  Counter perf_counter;
  js.schedule(Pool::Performance, std::span<const Job>(perf_jobs.data(), perf_jobs.size()), perf_counter);
  js.wait(perf_counter);
  js.wait(eff_counter);
  CHECK(hits.load() == 100);
}

TEST_CASE("JobSystem: workers are pinned to distinct CPUs of the right class") {
  JobSystem js;
  const platform::Topology& t = js.topology();
  struct Probe {
    std::atomic<u32>* mismatches;
  };
  std::atomic<u32> mismatches{0};
  Probe probe{&mismatches};
  auto probe_fn = [](void* p) {
    auto* pr = static_cast<Probe*>(p);
    const WorkerInfo* me = JobSystem::current_worker();
    if (me == nullptr || me->cpu == platform::k_invalid_cpu) return;
    // Sample a few times; the OS may briefly run us elsewhere right after pinning.
    u16 cpu = platform::k_invalid_cpu;
    for (int i = 0; i < 64; ++i) {
      cpu = platform::current_cpu();
      if (cpu == me->cpu) break;
      platform::yield_thread();
    }
    if (cpu != me->cpu) pr->mismatches->fetch_add(1);
  };
  std::vector<Job> jobs(js.worker_count(Pool::Performance) * 8, Job{probe_fn, &probe, nullptr});
  Counter counter;
  js.schedule(Pool::Performance, std::span<const Job>(jobs.data(), jobs.size()), counter);
  js.wait(counter);
  CHECK(mismatches.load() == 0);

  // Performance workers sit on performance CPUs; distinct workers on distinct CPUs when possible.
  std::vector<u16> cpus;
  for (u32 i = 0; i < js.worker_count(Pool::Performance); ++i) {
    const WorkerInfo& w = js.worker_info(Pool::Performance, i);
    CHECK(t.performance_cpus.test(w.cpu));
    cpus.push_back(w.cpu);
  }
  std::sort(cpus.begin(), cpus.end());
  CHECK(std::adjacent_find(cpus.begin(), cpus.end()) == cpus.end());
  if (!t.efficiency_cpus.empty()) {
    for (u32 i = 0; i < js.worker_count(Pool::Efficiency); ++i) {
      CHECK(t.efficiency_cpus.test(js.worker_info(Pool::Efficiency, i).cpu));
    }
  }
}

TEST_CASE("JobSystem: stealing happens when one worker is flooded") {
  JobSystemConfig cfg;
  cfg.queue_capacity = 1 << 15;
  JobSystem js(cfg);
  if (js.worker_count(Pool::Performance) < 2) {
    MESSAGE("single performance worker; stealing not observable");
    return;
  }
  // A worker that schedules thousands of jobs onto its own deque forces others to steal.
  struct Flood {
    JobSystem* js;
    Counter* counter;
    CounterJob ctx;
  };
  std::atomic<u32> hits{0};
  Counter counter;
  Flood flood{&js, &counter, CounterJob{&hits}};
  auto flood_fn = [](void* p) {
    auto* f = static_cast<Flood*>(p);
    auto slow_hit = [](void* q) {
      auto* c = static_cast<CounterJob*>(q);
      volatile u32 spin = 0;
      for (u32 i = 0; i < 2000; ++i) spin = spin + i;
      c->hits->fetch_add(1);
    };
    for (int i = 0; i < 20000; ++i) {
      f->counter->add();
      f->js->schedule(Pool::Performance, Job{slow_hit, &f->ctx, f->counter});
    }
  };
  Counter flood_counter;
  const Job flood_job{flood_fn, &flood, nullptr};
  js.schedule(Pool::Performance, std::span<const Job>(&flood_job, 1), flood_counter);
  // Block without helping so the flood runs on a worker and lands in that worker's deque.
  flood_counter.wait_blocking();
  counter.wait_blocking();
  CHECK(hits.load() == 20000);
  const JobSystemStats s = js.stats();
  CHECK(s.steals_local + s.steals_remote > 0);
  MESSAGE("steals local=" << s.steals_local << " remote=" << s.steals_remote << " inbox=" << s.inbox_pops
                          << " sleeps=" << s.sleeps << " inline=" << s.inline_runs);
}

TEST_CASE("JobSystem: idle workers sleep rather than spin forever") {
  JobSystem js;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const JobSystemStats before = js.stats();
  CHECK(before.sleeps >= js.total_worker_count());  // every worker went to sleep at least once
  // Wake them with work and confirm they come back.
  std::atomic<u32> hits{0};
  CounterJob ctx{&hits};
  std::vector<Job> jobs(256, Job{hit, &ctx, nullptr});
  Counter counter;
  js.schedule(Pool::Performance, std::span<const Job>(jobs.data(), jobs.size()), counter);
  js.wait(counter);
  CHECK(hits.load() == 256);
}

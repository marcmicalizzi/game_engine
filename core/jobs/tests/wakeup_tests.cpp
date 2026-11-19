// T195: a job submitted while a worker is on its way to sleep is never slept on.
//
// A worker that finds nothing announces itself in `sleepers`, looks at the queues again and
// waits; a submitter publishes its job and then looks at `sleepers`. Without a sequentially
// consistent operation between each side's write and its read, both can read the other's old
// value (the store-buffer shape), and the worker sleeps on a queued job (docs/subsystems/jobs.md,
// "Sleeping, and why a queued job always wakes a worker"). The window is a few dozen
// nanoseconds wide, so these cases put the submit at many points of a single worker's way into
// sleep, round after round. Before the mend, on the development machine (2026-11-19), an
// `msvc-release` build lost a job at round 415,505 through the guest deque and at round 18,956
// through the inbox; `msvc-debug`, slower on both sides, lost none in 3,000,000 rounds of each.
// So this case is a regression check in a release suite and a smoke run in a debug one.
//
// Every wait here is under a deadline: a lost wakeup has to show as a failed check, never as a
// suite that does not end. After a missed deadline the case submits empty jobs until the lost one
// has run (a later submit finds the sleeper long announced and wakes it), so nothing of the case
// is left queued or running when it unwinds; and every piece of state a job touches is declared
// before the JobSystem, which joins its workers when it is destroyed.

#include <core/jobs/job_system.h>
#include <core/platform/thread.h>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

using namespace engine;
using namespace engine::jobs;

namespace {

using Clock = std::chrono::steady_clock;
// Long enough for a below-normal-priority worker on a machine the merge gate keeps busy; a lost
// wakeup is permanent, so any deadline finds it, and a passing round takes microseconds.
constexpr Clock::duration k_deadline = std::chrono::seconds(10);

// Waits for `counter` without running anything: JobSystem::wait from an outside thread helps only
// the Performance pool, so it cannot rescue a lost Efficiency job, and this wait rescues none.
// It spins first, so the next round's submit follows the job's signal by about as long as the
// worker takes to reach its sleep, then yields.
bool done_within(const Counter& counter, Clock::duration limit) {
  const Clock::time_point end = Clock::now() + limit;
  for (u32 spins = 0; !counter.done(); ++spins) {
    if (Clock::now() >= end) return false;
    if (spins < 4096) {
      platform::pause_cpu();
    } else {
      std::this_thread::yield();
    }
  }
  return true;
}

void nothing(void*) {}

// After a missed deadline: submit empty jobs until `done` holds, at most another deadline.
template <class Done>
bool rescue(JobSystem& js, Pool pool, Done done) {
  const Clock::time_point end = Clock::now() + k_deadline;
  while (!done() && Clock::now() < end) {
    js.schedule(pool, Job{nothing, nullptr, nullptr});
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done();
}

void count(void* p) { static_cast<std::atomic<u32>*>(p)->fetch_add(1, std::memory_order_relaxed); }

struct Rounds {
  u32 run = 0;   // rounds whose job ran inside the deadline
  u32 lost = 0;  // rounds whose job did not (the loop stops at the first)
};

// One job a round to the Efficiency pool, whose single worker spins not at all before it sleeps,
// so each round's submit meets that worker on its way from the last job into sleep. The gap
// before the submit sweeps 0 to 16 pauses (well under a microsecond) so the submit lands at every
// point of that way, and the rounds alternate the two submit paths (one job; a batch of one).
Rounds submit_rounds(JobSystem& js, Counter& counter, std::atomic<u32>& ran, u32 rounds) {
  Rounds r;
  const Job job{count, &ran, &counter};
  for (u32 round = 0; round < rounds; ++round) {
    const u32 gap = (round * 7u) % 17u;
    for (u32 i = 0; i < gap; ++i)
      platform::pause_cpu();
    if ((round & 1u) == 0) {
      counter.add();
      js.schedule(Pool::Efficiency, job);
    } else {
      const Job batch[1] = {Job{count, &ran, nullptr}};
      js.schedule(Pool::Efficiency, std::span<const Job>(batch, 1), counter);
    }
    if (!done_within(counter, k_deadline)) {
      ++r.lost;
      rescue(js, Pool::Efficiency, [&] { return counter.done(); });
      break;
    }
    ++r.run;
  }
  return r;
}

JobSystemConfig one_sleepy_worker() {
  JobSystemConfig config;
  config.performance_workers = 1;
  config.efficiency_workers = 1;
  config.spin_iterations = 0;  // straight from the last job into the sleep path
  config.pin_threads = false;  // a pinned below-normal worker on a busy CPU only adds latency
  return config;
}

constexpr u32 k_rounds = 200000;

}  // namespace

TEST_CASE(
    "JobSystem: a job submitted as the worker goes to sleep is run, through the guest deque") {
  // The texture prefetch's shape: an outside thread submits Efficiency work (to the guest deque,
  // which the first outside thread to submit claims) and waits without helping that pool.
  Counter counter;
  std::atomic<u32> ran{0};
  JobSystem js(one_sleepy_worker());
  const Rounds r = submit_rounds(js, counter, ran, k_rounds);
  MESSAGE("guest deque: " << r.run << " rounds ran, " << r.lost << " lost; worker slept "
                          << js.stats().sleeps << " times");
  CHECK(r.lost == 0);
  CHECK(r.run == k_rounds);
  CHECK(ran.load() == r.run + r.lost);
}

TEST_CASE("JobSystem: a job submitted as the worker goes to sleep is run, through the inbox") {
  Counter counter;
  std::atomic<u32> ran{0};
  Rounds r;
  JobSystem js(one_sleepy_worker());
  // The main thread claims the Efficiency pool's guest deque, so the submitter below, another
  // outside thread, goes through the inbox (its cells' sequence numbers publish the job).
  Counter claim;
  js.schedule(Pool::Efficiency, std::span<const Job>(), claim);
  std::thread submitter([&] { r = submit_rounds(js, counter, ran, k_rounds); });
  submitter.join();  // every round of submit_rounds ends by its deadline
  MESSAGE("inbox: " << r.run << " rounds ran, " << r.lost << " lost; worker slept "
                    << js.stats().sleeps << " times");
  CHECK(r.lost == 0);
  CHECK(r.run == k_rounds);
  CHECK(ran.load() == r.run + r.lost);
}

TEST_CASE(
    "JobSystem: an outside thread's wait on Efficiency work returns with every worker asleep") {
  // JobSystem::wait from an outside thread helps the Performance pool only and then blocks in the
  // counter, so an Efficiency job no worker wakes for would hold it for good. The wait runs on a
  // thread of its own, watched under the deadline from this one. Rounds alternate a pool left long
  // enough for every worker to be asleep with one whose workers are just going to sleep.
  struct State {
    Counter counter;
    std::atomic<u32> ran{0};
    std::atomic<bool> returned{false};
  };
  auto state = std::make_unique<State>();
  JobSystemConfig config;
  config.performance_workers = 2;
  config.efficiency_workers = 2;
  config.pin_threads = false;
  JobSystem js(config);
  // Every worker asleep before the first round (as "idle workers sleep" in job_system_tests.cpp).
  for (int i = 0; i < 400 && js.stats().sleeps < js.total_worker_count(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  REQUIRE(js.stats().sleeps >= js.total_worker_count());

  constexpr u32 k_waits = 40;
  u32 returned = 0;
  u32 started = 0;
  for (u32 round = 0; round < k_waits; ++round) {
    ++started;
    if ((round & 1u) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    state->returned.store(false, std::memory_order_relaxed);
    State* s = state.get();
    std::thread waiter([&js, s] {
      const Job jobs[2] = {Job{count, &s->ran, nullptr}, Job{count, &s->ran, nullptr}};
      js.schedule(Pool::Efficiency, std::span<const Job>(jobs, 2), s->counter);
      js.wait(s->counter);
      s->returned.store(true, std::memory_order_release);
    });
    const Clock::time_point end = Clock::now() + k_deadline;
    while (!s->returned.load(std::memory_order_acquire) && Clock::now() < end)
      std::this_thread::yield();
    const bool in_time = s->returned.load(std::memory_order_acquire);
    CHECK_MESSAGE(in_time, "round " << round << ": the wait was still blocked after the deadline");
    if (!in_time && !rescue(js, Pool::Efficiency,
                            [s] { return s->returned.load(std::memory_order_acquire); })) {
      // Not even a later submit woke a worker: leave the thread and what it reads alive (the
      // process ends with it blocked) rather than hang the suite on a join.
      waiter.detach();
      (void)state.release();
      return;
    }
    waiter.join();
    if (!in_time) break;
    ++returned;
  }
  CHECK(returned == k_waits);
  CHECK(state->ran.load() == 2 * started);  // a rescued round's jobs ran too
}

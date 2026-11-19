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
// suite that does not end. **A missed deadline diagnoses itself** (batch 70's stack: `msvc-minimal`
// on a saturated machine missed one 10 s deadline through the inbox, and the case could not say
// whether its worker was asleep on the job or awake and not scheduled; the Efficiency workers run
// below normal priority). At the deadline the case reads the pool's announced sleepers, its wake
// epoch and whether the job is still queued (`JobSystemTestPeek`, the job system's one test seam),
// then waits on, to `k_starved_bound`, *without submitting anything*. A worker asleep on an epoch
// nobody bumped never wakes on its own, so a job that runs in that wait was starved, not lost: it
// is counted and printed with the machine's CPU load, and is not a failure. A job that has not run
// by then is the lost wakeup, a failed check with what the pool looked like; only then are empty
// jobs submitted until it has run (a later submit finds the sleeper long announced and wakes it),
// so nothing of the case is left queued or running when it unwinds. Every piece of state a job
// touches is declared before the JobSystem, which joins its workers when it is destroyed.

#include <core/jobs/job_system.h>
#include <core/platform/thread.h>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace engine::jobs {

// The job system's test seam (job_system.h): read-only looks at what a missed deadline needs.
struct JobSystemTestPeek {
  static u32 sleepers(const JobSystem& js, Pool pool) { return js.pool_sleepers(pool); }
  static u32 epoch(const JobSystem& js, Pool pool) { return js.pool_epoch(pool); }
  static u32 queued(const Counter& counter) {
    return counter.queued_.load(std::memory_order_acquire);
  }
};

}  // namespace engine::jobs

using namespace engine;
using namespace engine::jobs;

namespace {

using Clock = std::chrono::steady_clock;
// Long enough for a below-normal-priority worker on a machine the merge gate keeps busy; a lost
// wakeup is permanent, so any deadline finds it, and a passing round takes microseconds.
constexpr Clock::duration k_deadline = std::chrono::seconds(10);
// How long a job past its deadline is left to run with no further submit before it is called
// lost: a starved below-normal worker gets the CPU back long before (Windows boosts a thread
// starved for about 4 s), and a worker asleep on an unbumped epoch never does.
constexpr Clock::duration k_starved_bound = std::chrono::seconds(120);

// The whole machine's busy share between two samples, 0 to 1 (negative when it cannot be read).
struct CpuSample {
  u64 idle = 0;
  u64 total = 0;
  bool ok = false;
};
CpuSample cpu_sample() {
  CpuSample s;
#if defined(_WIN32)
  FILETIME idle, kernel, user;
  if (::GetSystemTimes(&idle, &kernel, &user) != 0) {
    const auto u = [](const FILETIME& f) {
      return (static_cast<u64>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    s.idle = u(idle);
    s.total = u(kernel) + u(user);  // kernel time includes idle time
    s.ok = true;
  }
#else
  std::ifstream stat("/proc/stat");
  std::string cpu;
  u64 v[8] = {};
  if (stat >> cpu >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5] >> v[6] >> v[7]) {
    s.idle = v[3] + v[4];
    for (const u64 x : v)
      s.total += x;
    s.ok = true;
  }
#endif
  return s;
}
double busy_share(const CpuSample& a, const CpuSample& b) {
  if (!a.ok || !b.ok || b.total <= a.total) return -1.0;
  const double total = static_cast<double>(b.total - a.total);
  return 1.0 - static_cast<double>(b.idle - a.idle) / total;
}

// What a pool looked like when a job missed its deadline.
struct PoolLook {
  u32 workers = 0;
  u32 sleepers = 0;  // announced: asleep, or between the announcement and the look that follows
  u32 epoch = 0;
  u32 queued = 0;  // the counter's jobs not yet started
  u64 sleeps = 0;  // every worker's sleeps so far
};
PoolLook look(const JobSystem& js, const Counter& counter, Pool pool) {
  PoolLook l;
  l.workers = js.worker_count(pool);
  l.sleepers = JobSystemTestPeek::sleepers(js, pool);
  l.epoch = JobSystemTestPeek::epoch(js, pool);
  l.queued = JobSystemTestPeek::queued(counter);
  l.sleeps = js.stats().sleeps;
  return l;
}
std::string describe(const PoolLook& l, u32 epoch_at_submit) {
  std::ostringstream o;
  o << l.sleepers << " of " << l.workers << " workers announced asleep, " << l.queued
    << " job(s) queued, epoch " << l.epoch << " (" << epoch_at_submit << " before the submit), "
    << l.sleeps << " sleeps";
  return o.str();
}

// Waits for `done()` without running anything: JobSystem::wait from an outside thread helps only
// the Performance pool, so it cannot rescue a lost Efficiency job, and this wait rescues none.
// It spins first, so the next round's submit follows the job's signal by about as long as the
// worker takes to reach its sleep, then yields.
template <class Done>
bool done_within(Done done, Clock::duration limit) {
  const Clock::time_point end = Clock::now() + limit;
  for (u32 spins = 0; !done(); ++spins) {
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

// After a job is called lost: submit empty jobs until `done` holds, at most another deadline.
template <class Done>
bool rescue(JobSystem& js, Pool pool, Done done) {
  const Clock::time_point end = Clock::now() + k_deadline;
  while (!done() && Clock::now() < end) {
    js.schedule(pool, Job{nothing, nullptr, nullptr});
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return done();
}

// What a missed deadline turned out to be.
struct Late {
  bool starved = false;  // ran with no further submit: its worker was awake
  std::string what;      // the pool at the deadline, and how it ended
};
// Called at a missed deadline, before any rescue: looks at the pool, then waits on, submitting
// nothing, to `k_starved_bound`.
template <class Done>
Late diagnose(const JobSystem& js, const Counter& counter, Pool pool, u32 epoch_at_submit,
              Done done) {
  const PoolLook at = look(js, counter, pool);
  const CpuSample before = cpu_sample();
  const Clock::time_point t0 = Clock::now();
  Late late;
  late.starved = done_within(done, k_starved_bound - k_deadline);
  const double waited = std::chrono::duration<double>(Clock::now() - t0).count();
  const double busy = busy_share(before, cpu_sample());
  std::ostringstream o;
  o << "at the deadline " << describe(at, epoch_at_submit) << "; ";
  if (late.starved) {
    o << "ran " << waited << " s later with no further submit (starved, not lost)";
  } else {
    o << "not run " << waited << " s later with no further submit (lost); then "
      << describe(look(js, counter, pool), epoch_at_submit);
  }
  o << "; machine busy " << (busy < 0.0 ? -1.0 : 100.0 * busy) << " % over the wait, "
    << std::thread::hardware_concurrency() << " logical CPUs";
  late.what = o.str();
  return late;
}

void count(void* p) { static_cast<std::atomic<u32>*>(p)->fetch_add(1, std::memory_order_relaxed); }

struct Rounds {
  u32 run = 0;      // rounds whose job ran inside the deadline
  u32 starved = 0;  // rounds whose job ran late, with no further submit
  u32 lost = 0;     // rounds whose job needed the rescue (the loop stops at the first)
  std::string lost_what;
  std::string starved_what;  // printed by the test's own thread: the submitter may be another
};

// One job a round to the Efficiency pool, whose single worker spins not at all before it sleeps,
// so each round's submit meets that worker on its way from the last job into sleep. The gap
// before the submit sweeps 0 to 16 pauses (well under a microsecond) so the submit lands at every
// point of that way, and the rounds alternate the two submit paths (one job; a batch of one).
Rounds submit_rounds(JobSystem& js, Counter& counter, std::atomic<u32>& ran, u32 rounds) {
  Rounds r;
  const Job job{count, &ran, &counter};
  const auto done = [&] { return counter.done(); };
  for (u32 round = 0; round < rounds; ++round) {
    const u32 gap = (round * 7u) % 17u;
    for (u32 i = 0; i < gap; ++i)
      platform::pause_cpu();
    const u32 epoch = JobSystemTestPeek::epoch(js, Pool::Efficiency);
    if ((round & 1u) == 0) {
      counter.add();
      js.schedule(Pool::Efficiency, job);
    } else {
      const Job batch[1] = {Job{count, &ran, nullptr}};
      js.schedule(Pool::Efficiency, std::span<const Job>(batch, 1), counter);
    }
    if (done_within(done, k_deadline)) {
      ++r.run;
      continue;
    }
    const Late late = diagnose(js, counter, Pool::Efficiency, epoch, done);
    if (late.starved) {
      ++r.starved;
      r.starved_what += "round " + std::to_string(round) + ": " + late.what + "\n";
      continue;
    }
    ++r.lost;
    r.lost_what = "round " + std::to_string(round) + ": " + late.what;
    rescue(js, Pool::Efficiency, done);
    break;
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
  if (!r.starved_what.empty()) MESSAGE(r.starved_what);
  MESSAGE("guest deque: " << r.run << " rounds ran, " << r.starved << " starved, " << r.lost
                          << " lost; worker slept " << js.stats().sleeps << " times");
  CHECK_MESSAGE(r.lost == 0, r.lost_what);
  CHECK(r.run + r.starved == k_rounds);
  CHECK(ran.load() == r.run + r.starved + r.lost);
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
  submitter.join();  // every round of submit_rounds ends by its bounds
  if (!r.starved_what.empty()) MESSAGE(r.starved_what);
  MESSAGE("inbox: " << r.run << " rounds ran, " << r.starved << " starved, " << r.lost
                    << " lost; worker slept " << js.stats().sleeps << " times");
  CHECK_MESSAGE(r.lost == 0, r.lost_what);
  CHECK(r.run + r.starved == k_rounds);
  CHECK(ran.load() == r.run + r.starved + r.lost);
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
  u32 starved = 0;
  u32 started = 0;
  for (u32 round = 0; round < k_waits; ++round) {
    ++started;
    if ((round & 1u) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    state->returned.store(false, std::memory_order_relaxed);
    State* s = state.get();
    const auto back = [s] { return s->returned.load(std::memory_order_acquire); };
    const u32 epoch = JobSystemTestPeek::epoch(js, Pool::Efficiency);
    std::thread waiter([&js, s] {
      const Job jobs[2] = {Job{count, &s->ran, nullptr}, Job{count, &s->ran, nullptr}};
      js.schedule(Pool::Efficiency, std::span<const Job>(jobs, 2), s->counter);
      js.wait(s->counter);
      s->returned.store(true, std::memory_order_release);
    });
    const Clock::time_point end = Clock::now() + k_deadline;
    while (!back() && Clock::now() < end)
      std::this_thread::yield();
    if (back()) {
      waiter.join();
      ++returned;
      continue;
    }
    const Late late = diagnose(js, s->counter, Pool::Efficiency, epoch, back);
    if (late.starved) {
      MESSAGE("round " << round << ": " << late.what);
      waiter.join();
      ++starved;
      continue;
    }
    CHECK_MESSAGE(false, "round " << round << ": the wait was lost: " << late.what);
    if (!rescue(js, Pool::Efficiency, back)) {
      // Not even a later submit woke a worker: leave the thread and what it reads alive (the
      // process ends with it blocked) rather than hang the suite on a join.
      waiter.detach();
      (void)state.release();
      return;
    }
    waiter.join();
    break;
  }
  MESSAGE("outside waits: " << returned << " returned, " << starved << " starved");
  CHECK(returned + starved == k_waits);
  CHECK(state->ran.load() == 2 * started);  // a rescued round's jobs ran too
}

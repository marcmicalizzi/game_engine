// Determinism, and the job adapter that has to not break it.
//
// The claim being tested is the one plan 05 §5.10 leans on: the same inputs produce the same
// state, whatever the worker count. That is what makes a replay reproducible and what makes
// lockstep multiplayer possible later (ADR-0016); it is also the property most easily lost by
// a job adapter that lets results depend on which worker finished first.

#include "physics_test_support.h"

#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/physics/physics.h>

#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <thread>

using namespace engine;
using namespace engine::physics;
using namespace engine::physics::testing;

namespace {

// The same scene every time: a ground plane, a grid of boxes at jittered heights and
// orientations so that islands form, split, and merge over the run.
void build_scene(World& world, Vector<BodyId>& bodies) {
  add_ground(world);
  ShapeId box_shape;
  REQUIRE(world.create_box(Vec3(0.5f, 0.5f, 0.5f), box_shape) == Status::Ok);
  ShapeId sphere_shape;
  REQUIRE(world.create_sphere(0.4f, sphere_shape) == Status::Ok);

  u32 seed = 0x9E3779B9u;
  const auto next = [&seed]() {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return static_cast<f32>(seed & 0xFFFFu) / 65535.0f;
  };

  for (u32 i = 0; i < 8; ++i) {
    for (u32 j = 0; j < 8; ++j) {
      BodyDesc desc;
      desc.shape = ((i + j) % 3 == 0) ? sphere_shape : box_shape;
      desc.transform.position =
          Vec3(static_cast<f32>(i) * 0.9f - 3.5f, 1.0f + static_cast<f32>(i * 8 + j) * 0.35f,
               static_cast<f32>(j) * 0.9f - 3.5f);
      desc.transform.rotation = quat_from_euler(next() * 3.0f, next() * 3.0f, next() * 3.0f);
      desc.linear_velocity = Vec3(next() - 0.5f, -next(), next() - 0.5f);
      desc.friction = 0.5f;
      desc.restitution = 0.2f;
      BodyId body;
      REQUIRE(world.create_body(desc, body) == Status::Ok);
      bodies.push_back(body);
    }
  }
  world.optimize_broad_phase();
}

void run(jobs::JobSystem* job_system, u32 worker_count, u32 steps, Vector<Transform3>& out,
         WorldStats& stats) {
  WorldOptions options = small_world_options();
  options.job_system = job_system;
  options.worker_count = worker_count;
  World world;
  REQUIRE(world.init(options) == Status::Ok);

  Vector<BodyId> bodies;
  build_scene(world, bodies);
  for (u32 i = 0; i < steps; ++i)
    REQUIRE(world.step() == Status::Ok);

  out.resize(bodies.size());
  world.read_transforms(std::span<const BodyId>(bodies), std::span<Transform3>(out));
  stats = world.stats();
  CHECK(world.tick().value == steps);
}

// Occupies one pool worker until it is told to stop, so that a step has to do its own work on
// the stepping thread and the pool cannot retire the queue entries behind it.
struct WorkerHold {
  std::atomic<bool> started{false};
  std::atomic<bool> release{false};
};

void hold_worker(void* data) {
  auto* hold = static_cast<WorkerHold*>(data);
  hold->started.store(true, std::memory_order_release);
  while (!hold->release.load(std::memory_order_acquire))
    std::this_thread::yield();
}

}  // namespace

TEST_CASE("physics: 600 steps give bit-identical transforms with 1 and with 8 workers") {
  jobs::JobSystemConfig config;
  config.performance_workers = 8;
  jobs::JobSystem job_system(config);

  Vector<Transform3> one;
  Vector<Transform3> eight;
  WorldStats one_stats;
  WorldStats eight_stats;
  run(&job_system, 1, 600, one, one_stats);
  run(&job_system, 8, 600, eight, eight_stats);

  REQUIRE(one.size() == eight.size());
  REQUIRE(one.size() == 64);
  // Bit-identical, not approximately equal: "close enough" is how a replay drifts apart over a
  // few thousand ticks.
  const usize bytes = one.size() * sizeof(Transform3);
  CHECK(std::memcmp(one.data(), eight.data(), bytes) == 0);

  // The worlds really did split their work differently, or the comparison proved nothing.
  CHECK(eight_stats.backend_jobs > one_stats.backend_jobs);
  CHECK(one_stats.steps == 600);
}

TEST_CASE("physics: the job adapter runs backend jobs on core/jobs workers") {
  jobs::JobSystemConfig config;
  config.performance_workers = 4;
  jobs::JobSystem job_system(config);

  const jobs::JobSystemStats before = job_system.stats();
  // The pool sizes itself to the machine: a hosted CI runner with four virtual cores may give the
  // performance pool a single worker, and then whether it ever wins a job against the stepping
  // thread is luck, not a property of the adapter. The determinism cases above cover that shape.
  if (job_system.worker_count(jobs::Pool::Performance) < 2) {
    MESSAGE("fewer than two performance workers on this machine; worker check skipped");
    return;
  }

  Vector<Transform3> transforms;
  WorldStats stats;
  // 600 steps, not 60: the barrier wait executes ready jobs on the caller, so on a small scene a
  // worker only sees a job when it wakes before the stepping thread has drained the queue, and
  // over a few dozen steps on a slow runner that happened zero times (CI, 2026-09-18).
  run(&job_system, 4, 600, transforms, stats);

  CHECK(stats.backend_jobs > 0);
  // Some of them ran on a pinned pool worker rather than on the stepping thread. Not all of
  // them will: the barrier wait executes ready jobs on the caller, which is the whole point of
  // JobSystemWithBarrier and is why this is a "greater than zero" and not a ratio.
  CHECK(stats.backend_jobs_on_workers > 0);
  CHECK(job_system.stats().jobs_executed > before.jobs_executed);
}

TEST_CASE("physics: with no job system the backend still steps, on the calling thread") {
  Vector<Transform3> with_jobs;
  Vector<Transform3> inline_only;
  WorldStats with_jobs_stats;
  WorldStats inline_stats;

  {
    jobs::JobSystemConfig config;
    config.performance_workers = 4;
    jobs::JobSystem job_system(config);
    run(&job_system, 4, 300, with_jobs, with_jobs_stats);
  }
  run(nullptr, 0, 300, inline_only, inline_stats);

  CHECK(inline_stats.backend_jobs_on_workers == 0);
  // And the answer is the same one, which is the property that lets a tool or a headless
  // replay run without standing up a thread pool.
  const usize bytes = with_jobs.size() * sizeof(Transform3);
  REQUIRE(with_jobs.size() == inline_only.size());
  CHECK(std::memcmp(with_jobs.data(), inline_only.data(), bytes) == 0);
}

TEST_CASE("physics: a step leaves nothing of its own in the job queue, even with a busy pool") {
  // The deterministic half of the job-lifetime regression. The backend's barrier executes any
  // ready job on the thread that is waiting, so a job is routinely *finished* while the queue
  // entry that was going to run it is still outstanding — and that entry holds the last
  // reference to a Job object out of a fixed-size pool. Whether the pool gets round to
  // retiring those entries before the next step needs more is a race, and it is a race a
  // machine with spare cores always wins, which is why this failed only on CI.
  //
  // Holding the pool's single worker takes the race away: every entry a step queues is still
  // there when Update returns, so `backend_jobs_pending` is exactly the leak, and the step's
  // drain is the only thing that can make it zero. Jolt's concurrency is 1 here to keep a
  // step's job count small — the drain helps the pool rather than spinning on it, and helping
  // is what has to do the work while the worker is held.
  jobs::JobSystemConfig config;
  config.performance_workers = 1;
  jobs::JobSystem job_system(config);

  WorkerHold hold;
  jobs::Counter blocker;
  blocker.add(1);
  job_system.schedule(jobs::Pool::Performance, jobs::Job{&hold_worker, &hold, &blocker});
  while (!hold.started.load(std::memory_order_acquire))
    std::this_thread::yield();

  {
    WorldOptions options = small_world_options();
    options.job_system = &job_system;
    options.worker_count = 1;
    options.max_backend_jobs = 256;
    World world;
    REQUIRE(world.init(options) == Status::Ok);
    Vector<BodyId> bodies;
    build_scene(world, bodies);

    for (u32 i = 0; i < 20; ++i) {
      REQUIRE(world.step() == Status::Ok);
      REQUIRE(world.stats().backend_jobs_pending == 0);
    }
    const WorldStats stats = world.stats();
    INFO("backend jobs " << stats.backend_jobs << " over 20 steps");
    CHECK(stats.backend_jobs > 20);
  }

  hold.release.store(true, std::memory_order_release);
  job_system.wait(blocker);
}

TEST_CASE("physics: a job system with no performance workers is treated as none at all") {
  // Otherwise the step would queue jobs nothing can ever run and then wait for them.
  jobs::JobSystemConfig config;
  config.performance_workers = 0;
  jobs::JobSystem job_system(config);
  if (job_system.worker_count(jobs::Pool::Performance) != 0) return;  // not this machine

  WorldOptions options = small_world_options();
  options.job_system = &job_system;
  World world;
  REQUIRE(world.init(options) == Status::Ok);
  Vector<BodyId> bodies;
  build_scene(world, bodies);
  for (u32 i = 0; i < 20; ++i)
    REQUIRE(world.step() == Status::Ok);
  CHECK(world.stats().backend_jobs_on_workers == 0);
}

TEST_CASE("physics: the backend's job pool comes back whole after every step") {
  // Every job the backend creates comes out of a fixed-size free list and goes back when its
  // last reference is released. A queued job holds one of those references, and the barrier
  // will execute the same job on the stepping thread if it gets there first — so when a step
  // returns, the pool can still hold entries for jobs that are already finished. Until the
  // step drained them, the number of live jobs was a function of how fast the workers happened
  // to drain rather than of the step: a machine with few cores ran the list dry within a
  // second (a hard failure) and left it short at shutdown (a backend assert in Debug), while a
  // thirty-six-thread one never showed either.
  //
  // The pool here is small on purpose, so that 300 steps recycle it many times over; the
  // worker counts are the ones CI actually runs on.
  constexpr u32 k_pool = 256;
  const u32 worker_counts[] = {1, 2, 4};
  for (const u32 workers : worker_counts) {
    jobs::JobSystemConfig config;
    config.performance_workers = workers;
    jobs::JobSystem job_system(config);

    WorldOptions options = small_world_options();
    options.job_system = &job_system;
    options.worker_count = workers;
    options.max_backend_jobs = k_pool;

    World world;
    REQUIRE(world.init(options) == Status::Ok);
    CHECK(world.options().max_backend_jobs == k_pool);
    Vector<BodyId> bodies;
    build_scene(world, bodies);
    for (u32 i = 0; i < 300; ++i) {
      REQUIRE(world.step() == Status::Ok);
      // The invariant, checked on every step rather than hoped for: a step that returns has
      // no job of its own left in the pool. Checking the *count* instead of waiting for the
      // free list to run dry is what makes this test machine-independent — on a machine with
      // more cores than the step has work for, the leftovers are retired before the next step
      // asks for anything and the exhaustion never reproduces.
      REQUIRE(world.stats().backend_jobs_pending == 0);
    }

    const WorldStats stats = world.stats();
    INFO("workers " << workers << ", backend jobs " << stats.backend_jobs);
    // Many times the pool's size, so the pool has been handed out and returned over and over
    // rather than merely never filled.
    CHECK(stats.backend_jobs > 4 * k_pool);
    // Destroying the world here is the other half of the check: the backend's free list
    // asserts in Debug that every object it handed out has come back.
  }
}

TEST_CASE("physics: the same world stepped twice from the same start agrees with itself") {
  Vector<Transform3> first;
  Vector<Transform3> second;
  WorldStats stats;
  run(nullptr, 0, 240, first, stats);
  run(nullptr, 0, 240, second, stats);
  REQUIRE(first.size() == second.size());
  CHECK(std::memcmp(first.data(), second.data(), first.size() * sizeof(Transform3)) == 0);
}

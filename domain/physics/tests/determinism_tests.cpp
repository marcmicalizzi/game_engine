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

#include <cstring>

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

  Vector<Transform3> transforms;
  WorldStats stats;
  run(&job_system, 4, 60, transforms, stats);

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

TEST_CASE("physics: the same world stepped twice from the same start agrees with itself") {
  Vector<Transform3> first;
  Vector<Transform3> second;
  WorldStats stats;
  run(nullptr, 0, 240, first, stats);
  run(nullptr, 0, 240, second, stats);
  REQUIRE(first.size() == second.size());
  CHECK(std::memcmp(first.data(), second.data(), first.size() * sizeof(Transform3)) == 0);
}

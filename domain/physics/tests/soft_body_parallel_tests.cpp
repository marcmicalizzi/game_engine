// Where a soft body's work runs, and what decides how wide it can go.
//
// The cost of one cage is the number ADR-0026's 1.5 ms tick budget lives or dies on, so "is the
// solve spread across the pool?" has to be a fact the suite checks rather than something read
// off a profiler once. Two separate claims are tested here, because they are separate:
//
//   1. The backend's constraint-solve jobs really do land on more than one worker when the pool
//      has workers to land on. That is what `WorldStats::soft_body_solve_workers` reports, and
//      it is also what pins the job name the adapter matches on: a Jolt that renamed the job
//      would report zero solve jobs, and this test would say so.
//   2. How wide *one* cage's constraint solve can go is decided by the cage, not by the pool:
//      the backend partitions a cage's vertices into batches of `k_soft_body_constraint_batch`
//      and hands one group to a thread, so a cage under one batch is solved by one thread
//      however many workers exist (`soft_body_solve_width`, E19).
//
// The two together are the reason E19 recommends more small cages over one large one.

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

// Small enough that eight of them step quickly in a debug build, and large enough that the
// cage is a real constraint set (216 particles, 5,220 edges, 750 tetrahedra).
constexpr u32 k_cage_side = 6;
constexpr f32 k_spacing = 0.06f;

SoftBodyId add_cube(World& world, const LatticeVolume& lattice, Vec3 position, u32 iterations) {
  SoftBodyDesc desc;
  desc.vertices = std::span<const Vec3>(lattice.vertices);
  desc.inverse_masses = std::span<const f32>(lattice.inverse_masses);
  desc.edges = std::span<const SoftEdge>(lattice.edges);
  desc.volumes = std::span<const SoftVolumeConstraint>(lattice.volumes);
  desc.faces = std::span<const u32>(lattice.faces);
  desc.transform.position = place(position);
  desc.iterations = iterations;
  desc.allow_sleeping = false;
  SoftBodyId body;
  REQUIRE(world.create_soft_body(desc, body) == Status::Ok);
  return body;
}

}  // namespace

TEST_CASE("physics: the solve width of a cage is the cage's, not the pool's") {
  // The batch is a backend constant mirrored in the header and pinned to Jolt's by a static
  // assertion; this only checks the arithmetic around it, which is what a caller reasons with.
  CHECK(soft_body_solve_width(1) == 1);
  CHECK(soft_body_solve_width(k_soft_body_constraint_batch) == 1);
  CHECK(soft_body_solve_width(k_soft_body_constraint_batch + 1) == 2);
  CHECK(soft_body_solve_width(512) == 2);
  CHECK(soft_body_solve_width(800) == 4);

  // The two cages E19 sweeps: 343 particles is a two-wide solve and 729 a three-wide one, so
  // tripling the element count buys one and a half extra threads and not three.
  CHECK(soft_body_solve_width(7 * 7 * 7) == 2);
  CHECK(soft_body_solve_width(9 * 9 * 9) == 3);
}

TEST_CASE("physics: the backend's soft-body solve runs on more than one worker") {
  jobs::JobSystemConfig config;
  config.performance_workers = 8;
  jobs::JobSystem job_system(config);

  WorldOptions options = small_world_options();
  options.job_system = &job_system;
  options.worker_count = 8;
  World world;
  REQUIRE(world.init(options) == Status::Ok);
  add_ground(world);

  // Eight cages, because that is the case the backend can actually spread: its solve jobs take
  // one constraint group at a time from *any* active cage, so eight one-batch cages fill eight
  // threads where one eight-batch cage would not.
  const LatticeVolume lattice = build_lattice_volume(k_cage_side, k_spacing, 1.0e-4f, 1.0e-4f);
  const f32 side = static_cast<f32>(k_cage_side - 1) * k_spacing;
  for (u32 i = 0; i < 8; ++i) {
    const f32 x = (static_cast<f32>(i) - 3.5f) * (side + 0.1f);
    add_cube(world, lattice, Vec3(x, 0.5f * side + 0.02f, 0.0f), 4);
  }
  REQUIRE(world.soft_body_count() == 8);

  step_n(world, 40);
  const WorldStats stats = world.stats();

  // Non-zero is what pins the job name the adapter matches: the counter is derived from it, so
  // a backend that renamed the stage would land here rather than in a silent zero elsewhere.
  INFO("solve jobs " << stats.soft_body_solve_jobs << " on " << stats.soft_body_solve_workers
                     << " workers");
  CHECK(stats.soft_body_solve_jobs > 0);
  CHECK(stats.soft_body_solve_workers > 1);
  CHECK(stats.soft_body_solve_workers <= 8);
}

TEST_CASE("physics: with no job system the soft-body solve stays on the calling thread") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  add_ground(world);
  const LatticeVolume lattice = build_lattice_volume(k_cage_side, k_spacing, 1.0e-4f, 1.0e-4f);
  const f32 side = static_cast<f32>(k_cage_side - 1) * k_spacing;
  add_cube(world, lattice, Vec3(0.0f, 0.5f * side + 0.02f, 0.0f), 4);

  step_n(world, 20);
  const WorldStats stats = world.stats();
  // The jobs still exist — the backend's step is the same graph — they just run inline.
  CHECK(stats.soft_body_solve_jobs > 0);
  CHECK(stats.soft_body_solve_workers == 0);
}

TEST_CASE("physics: a cage steps to the same positions with 1 and with 8 workers") {
  jobs::JobSystemConfig config;
  config.performance_workers = 8;
  jobs::JobSystem job_system(config);

  const LatticeVolume lattice = build_lattice_volume(k_cage_side, k_spacing, 1.0e-4f, 1.0e-4f);
  const f32 side = static_cast<f32>(k_cage_side - 1) * k_spacing;
  const u32 vertex_count = k_cage_side * k_cage_side * k_cage_side;

  const auto run = [&](u32 workers, Vector<Vec3>& out) {
    WorldOptions options = small_world_options();
    options.job_system = &job_system;
    options.worker_count = workers;
    World world;
    REQUIRE(world.init(options) == Status::Ok);
    add_ground(world);
    // Two cages, so that the eight-worker run has something to split between threads and the
    // comparison is not between two serial solves.
    add_cube(world, lattice, Vec3(-0.5f * side - 0.05f, 0.5f * side + 0.02f, 0.0f), 6);
    const SoftBodyId second =
        add_cube(world, lattice, Vec3(0.5f * side + 0.05f, 0.5f * side + 0.02f, 0.0f), 6);
    step_n(world, 120);
    out.resize(vertex_count);
    REQUIRE(world.read_soft_body_vertices(second, WorldPos::origin(), std::span<Vec3>(out)) ==
            vertex_count);
  };

  Vector<Vec3> one;
  Vector<Vec3> eight;
  run(1, one);
  run(8, eight);

  // Bit-identical, like the rigid-body case: cage state enters the sim hash (ADR-0026
  // decision 5), so "close enough" is a replay that drifts.
  REQUIRE(one.size() == eight.size());
  CHECK(std::memcmp(one.data(), eight.data(), one.size() * sizeof(Vec3)) == 0);
}

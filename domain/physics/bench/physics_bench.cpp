// What a physics step costs: a pile of a thousand boxes at one, four, and eight workers, and a
// soft cube at two iteration counts. Both are reported per step, because the step is the unit
// the tick budget is written in (plan 05 §5.14 budgets 1.5 ms per 60 Hz tick for deformables).
//
// The box benchmark measures a *settled* pile with sleeping switched off, not the first second
// of free fall: a pile that is allowed to sleep costs nothing after a second and would make the
// number a measure of the sleep heuristic rather than of the solver.

#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/physics/physics.h>
#include <foundation/bench/bench.h>

using namespace engine;
using namespace engine::physics;

namespace {

constexpr u32 k_box_count = 1000;

void add_ground(World& world) {
  ShapeId shape;
  world.create_box(Vec3(60.0f, 0.5f, 60.0f), shape);
  BodyDesc desc;
  desc.shape = shape;
  desc.transform.position = Vec3(0.0f, -0.5f, 0.0f);
  desc.motion = MotionType::Static;
  desc.layer = Layer::Static;
  desc.friction = 0.8f;
  BodyId body;
  world.create_body(desc, body);
}

// A 10 x 10 x 10 grid of boxes that falls into a pile.
void fill_boxes(World& world) {
  ShapeId shape;
  world.create_box(Vec3(0.5f, 0.5f, 0.5f), shape);
  for (u32 i = 0; i < k_box_count; ++i) {
    const u32 x = i % 10;
    const u32 y = (i / 10) % 10;
    const u32 z = i / 100;
    BodyDesc desc;
    desc.shape = shape;
    desc.transform.position =
        Vec3(static_cast<f32>(x) * 1.2f - 6.0f, 0.6f + static_cast<f32>(y) * 1.3f,
             static_cast<f32>(z) * 1.2f - 6.0f);
    desc.friction = 0.5f;
    desc.allow_sleeping = false;
    BodyId body;
    world.create_body(desc, body);
  }
  world.optimize_broad_phase();
}

WorldOptions pile_options(jobs::JobSystem* job_system, u32 workers) {
  WorldOptions options;
  options.max_bodies = 4096;
  options.max_body_pairs = 65536;
  options.max_contact_constraints = 32768;
  options.job_system = job_system;
  options.worker_count = workers;
  return options;
}

}  // namespace

ENGINE_BENCH_ARGS(physics_step_boxes, "physics.step.boxes_1000", 1, 4, 8) {
  const u32 workers = static_cast<u32>(state.arg());
  jobs::JobSystemConfig config;
  config.performance_workers = workers;
  jobs::JobSystem job_system(config);

  World world;
  world.init(pile_options(&job_system, workers));
  add_ground(world);
  fill_boxes(world);
  // Let the pile land and stack before anything is timed.
  for (u32 i = 0; i < 180; ++i)
    world.step();

  while (state.keep_running())
    world.step();
  state.set_items(k_box_count);
}

// The same pile without a job system at all, so the adapter's share of the cost is visible.
ENGINE_BENCH(physics_step_boxes_serial, "physics.step.boxes_1000.serial") {
  World world;
  world.init(pile_options(nullptr, 0));
  add_ground(world);
  fill_boxes(world);
  for (u32 i = 0; i < 180; ++i)
    world.step();

  while (state.keep_running())
    world.step();
  state.set_items(k_box_count);
}

// A 512-particle lattice cube (8 x 8 x 8) resting on the ground, at two iteration counts. This
// is the shape of the per-volume cost ADR-0026's tier table is built on.
ENGINE_BENCH_ARGS(physics_step_soft_cube, "physics.step.soft_cube_512", 4, 8) {
  const u32 iterations = static_cast<u32>(state.arg());
  WorldOptions options;
  options.max_bodies = 64;
  options.max_body_pairs = 4096;
  options.max_contact_constraints = 4096;
  World world;
  world.init(options);
  add_ground(world);

  const LatticeVolume lattice = build_lattice_volume(8, 0.1f, 1.0e-6f, 1.0e-6f);
  SoftBodyDesc desc;
  desc.vertices = std::span<const Vec3>(lattice.vertices);
  desc.inverse_masses = std::span<const f32>(lattice.inverse_masses);
  desc.edges = std::span<const SoftEdge>(lattice.edges);
  desc.volumes = std::span<const SoftVolumeConstraint>(lattice.volumes);
  desc.faces = std::span<const u32>(lattice.faces);
  desc.transform.position = Vec3(0.0f, 0.4f, 0.0f);
  desc.iterations = iterations;
  desc.allow_sleeping = false;
  SoftBodyId cube;
  world.create_soft_body(desc, cube);
  // Long enough that the cube is resting on the ground with its full contact set, not still
  // falling: a falling cube touches nothing and measures half the work.
  for (u32 i = 0; i < 300; ++i)
    world.step();

  while (state.keep_running())
    world.step();
  state.set_items(static_cast<u64>(lattice.vertices.size()));
}

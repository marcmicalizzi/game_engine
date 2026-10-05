// What a physics step costs: a pile of a thousand boxes at one, four, and eight workers, one
// soft cube across a worker and iteration sweep, and eight soft cubes at once. Everything is
// reported per step, because the step is the unit the tick budget is written in (plan 05 §5.14
// budgets 1.5 ms per 60 Hz tick for deformables). The E19 lattice-cage experiment has its own
// file beside this one.
//
// The box benchmark measures a *settled* pile with sleeping switched off, not the first second
// of free fall: a pile that is allowed to sleep costs nothing after a second and would make the
// number a measure of the sleep heuristic rather than of the solver.

#include <core/base/macros.h>
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <foundation/bench/bench.h>

using namespace engine;
using namespace engine::physics;

namespace {

constexpr u32 k_box_count = 1000;

// The scenes are built round `site`: the origin, or a far site for the rows that say so, where
// every position the backend holds is f64 far from zero and the collision arithmetic is floats
// relative to the bodies (ADR-0053; docs/experiments/world-positions-physics-2026-10-05.md).
WorldPos place(Vec3 local, WorldPos site = WorldPos::origin()) { return absolute(site, local); }
// The far sites, as the tests have them: 419,072 m (the owner's), 10,000 km and 1e8 m, by number.
constexpr WorldPos k_far_sites[] = {WorldPos{419072.0, 0.0, -419072.0},
                                    WorldPos{10000000.0, 0.0, 10000000.0},
                                    WorldPos{100000000.0, 0.0, -100000000.0}};
WorldPos far_site(i64 n) { return k_far_sites[(n >= 1 && n <= 3 ? n : 2) - 1]; }

void add_ground(World& world, WorldPos site = WorldPos::origin()) {
  ShapeId shape;
  world.create_box(Vec3(60.0f, 0.5f, 60.0f), shape);
  BodyDesc desc;
  desc.shape = shape;
  desc.transform.position = place(Vec3(0.0f, -0.5f, 0.0f), site);
  desc.motion = MotionType::Static;
  desc.layer = Layer::Static;
  desc.friction = 0.8f;
  BodyId body;
  world.create_body(desc, body);
}

// A 10 x 10 x 10 grid of boxes that falls into a pile.
void fill_boxes(World& world, WorldPos site = WorldPos::origin()) {
  ShapeId shape;
  world.create_box(Vec3(0.5f, 0.5f, 0.5f), shape);
  for (u32 i = 0; i < k_box_count; ++i) {
    const u32 x = i % 10;
    const u32 y = (i / 10) % 10;
    const u32 z = i / 100;
    BodyDesc desc;
    desc.shape = shape;
    desc.transform.position =
        place(Vec3(static_cast<f32>(x) * 1.2f - 6.0f, 0.6f + static_cast<f32>(y) * 1.3f,
                   static_cast<f32>(z) * 1.2f - 6.0f),
              site);
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

// The same pile at the far sites (ADR-0053; 1: 419 km, 2: 10,000 km, 3: 1e8 m): the backend's
// positions are f64 there as everywhere, so this row says whether being far costs anything beyond
// what double precision costs by the origin, which is the row above.
ENGINE_BENCH_ARGS(physics_step_boxes_far, "physics.step.boxes_1000_far", 101, 108, 201, 208, 301) {
  // site * 100 + workers: a benchmark argument is one integer.
  const WorldPos site = far_site(state.arg() / 100);
  const u32 workers = static_cast<u32>(state.arg() % 100);
  jobs::JobSystemConfig config;
  config.performance_workers = workers;
  jobs::JobSystem job_system(config);

  World world;
  world.init(pile_options(&job_system, workers));
  add_ground(world, site);
  fill_boxes(world, site);
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

// A 512-particle lattice cube (8 x 8 x 8) resting on the ground. This is the shape of the
// per-volume cost ADR-0026's tier table is built on, and the argument sweeps both knobs that
// change it: the worker count and the iteration count, packed as workers * 100 + iterations
// because a benchmark argument is one integer.
//
// The worker dimension is here because of what the backend does with a cage. Its constraint
// solve is split by *vertex batch*, 256 vertices to a batch, so a 512-particle cage is a
// two-wide solve and the fourth and eighth workers have nothing to claim — which is exactly
// what these rows measure, and why `soft_cubes_8` below exists beside them
// (docs/experiments/e19-lattice-cage.md).

namespace {

constexpr i64 soft_key(i64 workers, i64 iterations) { return workers * 100 + iterations; }

// How long the cubes are given to land and settle before anything is timed. Debug builds only
// smoke-run benchmarks (docs/subsystems/bench.md) and there are twelve of these variants, so the
// debug figure is what keeps the CTest smoke run short; every published number is release.
#if ENGINE_DEBUG
constexpr u32 k_soft_settle_steps = 30;
#else
constexpr u32 k_soft_settle_steps = 300;
#endif

// Places `count` lattice cubes in a row, all resting on the ground, and steps them until they
// are settled with their full contact set: a falling cube touches nothing and measures half
// the work.
void fill_soft_cubes(World& world, const LatticeVolume& lattice, u32 count, u32 iterations) {
  const f32 side = 7.0f * 0.1f;
  for (u32 i = 0; i < count; ++i) {
    SoftBodyDesc desc;
    desc.vertices = std::span<const Vec3>(lattice.vertices);
    desc.inverse_masses = std::span<const f32>(lattice.inverse_masses);
    desc.edges = std::span<const SoftEdge>(lattice.edges);
    desc.volumes = std::span<const SoftVolumeConstraint>(lattice.volumes);
    desc.faces = std::span<const u32>(lattice.faces);
    desc.transform.position = place(Vec3(
        (static_cast<f32>(i) - 0.5f * static_cast<f32>(count - 1)) * (side + 0.2f), 0.4f, 0.0f));
    desc.iterations = iterations;
    desc.allow_sleeping = false;
    SoftBodyId cube;
    world.create_soft_body(desc, cube);
  }
  for (u32 i = 0; i < k_soft_settle_steps; ++i)
    world.step();
}

WorldOptions soft_options(jobs::JobSystem* job_system, u32 workers) {
  WorldOptions options;
  options.max_bodies = 64;
  options.max_body_pairs = 16384;
  options.max_contact_constraints = 16384;
  options.job_system = job_system;
  options.worker_count = workers;
  return options;
}

}  // namespace

ENGINE_BENCH_ARGS(physics_step_soft_cube, "physics.step.soft_cube_512", soft_key(1, 4),
                  soft_key(1, 8), soft_key(1, 16), soft_key(4, 4), soft_key(4, 8), soft_key(4, 16),
                  soft_key(8, 4), soft_key(8, 8), soft_key(8, 16)) {
  const u32 workers = static_cast<u32>(state.arg() / 100);
  const u32 iterations = static_cast<u32>(state.arg() % 100);
  jobs::JobSystemConfig config;
  config.performance_workers = workers;
  jobs::JobSystem job_system(config);

  World world;
  world.init(soft_options(&job_system, workers));
  add_ground(world);
  const LatticeVolume lattice = build_lattice_volume(8, 0.1f, 1.0e-6f, 1.0e-6f);
  fill_soft_cubes(world, lattice, 1, iterations);

  while (state.keep_running())
    world.step();
  state.set_items(static_cast<u64>(lattice.vertices.size()));
}

// Eight of the same cubes at once, at eight iterations. The backend's solve jobs take the next
// available constraint group from *any* active cage, so this is the case where more workers
// have something to claim: 4,096 particles as eight independent two-batch bodies rather than as
// one cage, which is the comparison E19 turns into a recommendation.
ENGINE_BENCH_ARGS(physics_step_soft_cubes_8, "physics.step.soft_cubes_8", 1, 4, 8) {
  const u32 workers = static_cast<u32>(state.arg());
  jobs::JobSystemConfig config;
  config.performance_workers = workers;
  jobs::JobSystem job_system(config);

  World world;
  world.init(soft_options(&job_system, workers));
  add_ground(world);
  const LatticeVolume lattice = build_lattice_volume(8, 0.1f, 1.0e-6f, 1.0e-6f);
  fill_soft_cubes(world, lattice, 8, 8);

  while (state.keep_running())
    world.step();
  state.set_items(8 * static_cast<u64>(lattice.vertices.size()));
}

// One step of the character (character.h) at engine-view's 240 Hz tick, walking back and forth
// over a 64 m heightfield tile at a metre and through a field of 200 standing and fallen blocks —
// the ground and a ruin's pieces, which is what scene_collision puts round a walker. The sweep is
// a few shape queries against what is within reach, so the number is per step and says what a
// walker costs a tick.
namespace {

void character_steps(bench::State& state, WorldPos site) {
  WorldOptions options;
  options.max_bodies = 1024;
  options.max_body_pairs = 4096;
  options.max_contact_constraints = 4096;
  World world;
  world.init(options);
  constexpr u32 k_side = 64;
  Vector<f32> heights(k_side * k_side);
  for (u32 j = 0; j < k_side; ++j) {
    for (u32 i = 0; i < k_side; ++i) {
      const f32 x = static_cast<f32>(i) - 32.0f;
      const f32 z = static_cast<f32>(j) - 32.0f;
      heights[j * k_side + i] = 0.003f * x * x + 0.04f * z;
    }
  }
  HeightfieldDesc field;
  field.heights = std::span<const f32>(heights.data(), heights.size());
  field.sample_count = k_side;
  field.local_offset = Vec3(-32.0f, 0.0f, -32.0f);
  ShapeId field_shape;
  world.create_heightfield(field, field_shape);
  BodyDesc ground;
  ground.shape = field_shape;
  ground.transform.position = site;
  ground.motion = MotionType::Static;
  ground.layer = Layer::Static;
  BodyId ground_body;
  world.create_body(ground, ground_body);
  ShapeId block;
  world.create_box(Vec3(0.3f, 0.15f, 0.3f), block);
  for (u32 b = 0; b < 200; ++b) {
    BodyDesc desc;
    desc.shape = block;
    desc.motion = MotionType::Static;
    desc.layer = Layer::Static;
    const f32 x = static_cast<f32>(b % 20) * 1.1f - 11.0f;
    const f32 z = static_cast<f32>(b / 20) * 1.3f - 6.5f;
    desc.transform.position = place(Vec3(x, 0.003f * x * x + 0.04f * z + 0.15f, z), site);
    BodyId body;
    world.create_body(desc, body);
  }
  world.optimize_broad_phase();
  CharacterConfig config;
  config.step_hz = 240;
  CharacterBody character;
  character.create(world, config, place(Vec3(-15.0f, 1.5f, 0.3f), site));
  u32 step = 0;
  while (state.keep_running()) {
    CharacterInput input;
    // Four seconds east, four west, over and among the blocks.
    input.move = ((step / 960) & 1) == 0 ? Vec3(1.0f, 0.0f, 0.1f) : Vec3(-1.0f, 0.0f, -0.1f);
    input.jump = (step % 700) == 350;
    character.step(input);
    ++step;
  }
  bench::keep(character.hash());
  state.set_items(1);
}

}  // namespace

ENGINE_BENCH(physics_character_step, "physics.character.step") {
  character_steps(state, WorldPos::origin());
}

// The same walk at the far sites (1: 419 km, 2: 10,000 km, 3: 1e8 m): the sweep's queries are
// floats relative to the feet there too.
ENGINE_BENCH_ARGS(physics_character_step_far, "physics.character.step_far", 1, 2, 3) {
  character_steps(state, far_site(state.arg()));
}

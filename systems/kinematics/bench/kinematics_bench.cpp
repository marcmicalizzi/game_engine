// Micro-benchmarks for the kinematics capability (docs/plan/11-performance-principles.md §11.8):
// the integrate system over a population of movers, through flecs' real tick on one worker, half of
// them turning — which is the branch that costs a quaternion product — and half not.
#include <core/ids/id128.h>
#include <domain/ecs/identity.h>
#include <domain/ecs/sim_world.h>
#include <foundation/bench/bench.h>
#include <systems/kinematics/kinematics.h>

using namespace engine;

ENGINE_BENCH_ARGS(kinematics_tick, "kinematics.tick", 1000, 10000, 100000) {
  const u32 movers = static_cast<u32>(state.arg());
  ecs::SimWorld sim;
  kinematics::KinematicsSystem system;
  system.install(sim);
  for (u32 i = 0; i < movers; ++i) {
    const flecs::entity e = ecs::create_entity(sim.world(), Id128::from_parts(0xB, i + 1u));
    e.set<world::Transform>(world::Transform{});
    kinematics::Velocity v;
    v.linear = Vec3{1.0f, 0.0f, 0.5f};
    if ((i & 1u) != 0) v.angular = Vec3{0.0f, 0.3f, 0.0f};
    e.set<kinematics::Velocity>(v);
  }
  while (state.keep_running()) {
    sim.step();
    bench::keep(system.stats().moved);
  }
  state.set_items(movers);
}

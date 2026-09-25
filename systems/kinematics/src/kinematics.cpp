#include <core/log/log.h>
#include <core/math/math.h>
#include <domain/ecs/components.h>
#include <domain/ecs/systems.h>
#include <systems/kinematics/kinematics.h>

#include <schemas/kinematics_ecs.h>
#include <schemas/world_ecs.h>

namespace engine::kinematics {

ENGINE_LOG_CATEGORY_DEFINE(log_kinematics, "kinematics");

void integrate(world::Transform& transform, const Velocity& velocity, f32 seconds) noexcept {
  transform.position.x += velocity.linear.x * seconds;
  transform.position.y += velocity.linear.y * seconds;
  transform.position.z += velocity.linear.z * seconds;
  const Vec3 w = velocity.angular;
  if (w.x == 0.0f && w.y == 0.0f && w.z == 0.0f) return;
  // dq/dt = ½ (0, ω) q, one explicit step and a renormalization: first order, which at a 60 Hz
  // step is well inside what a thing that is not simulated needs.
  const f32 h = 0.5f * seconds;
  const Quat spin{w.x * h, w.y * h, w.z * h, 0.0f};
  transform.orientation = normalize(transform.orientation + spin * transform.orientation);
}

void KinematicsSystem::install(ecs::SimWorld& sim) {
  flecs::world& world = sim.world();
  // Seam 1: the world's own components and this capability's, through the headers schemac
  // generated. Registering `Transform` here is idempotent, so a host that registered it first
  // (engine-host does, for `Node` records) and this capability agree on one component.
  world::register_world_components(world);
  register_kinematics_components(world);

  desc_ = sim::SystemDesc{};
  desc_.name = "kinematics.integrate";
  desc_.phase = k_phase;
  desc_.reads = ecs::mask_of<Velocity>(world);
  desc_.writes = ecs::mask_of<world::Transform>(world);
  desc_.tiers = 0x0Fu;  // every tier: see the header
  desc_.determinism = sim::Determinism::Hashed;
  desc_.context = this;
  KinematicsStats* stats = &stats_;
  ecs::register_system(sim, desc_, [stats](flecs::world& w, flecs::entity phase) {
    return w.system<world::Transform, const Velocity>("kinematics.integrate")
        .kind(phase)
        .multi_threaded()
        .run([stats](flecs::iter& it) {
          // Bound to a named world, as animation's systems do: GCC 13 reads a reference through a
          // temporary `flecs::world` wrapper as dangling.
          const flecs::world world_ref = it.world();
          const f32 step = it.delta_time();
          u32 moved = 0;
          while (it.next()) {
            auto transforms = it.field<world::Transform>(0);
            auto velocities = it.field<const Velocity>(1);
            for (const auto row : it) {
              integrate(transforms[row], velocities[row], step);
              ++moved;
            }
          }
          if (world_ref.get_stage_id() == 0) stats->moved = moved;
        });
  });
  ENGINE_LOG_INFO(log_kinematics, "capability installed");
}

}  // namespace engine::kinematics

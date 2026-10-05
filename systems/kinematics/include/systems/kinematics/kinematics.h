#pragma once

// kinematics capability (ADR-0027; docs/plan/02-architecture.md §2.8,
// docs/plan/05-simulation.md §5.15, docs/plan/11-performance-principles.md §11.10).
//
// Motion without forces: an entity with a `world::Transform` and a `kinematics::Velocity` moves by
// its velocity every tick. It is what a thing that moves but is not simulated needs — a cart on a
// rail, a drifting buoy, a patrol that a scheduled event will stop — and it is the first capability
// whose state a document authors (`Mover` records materialize into it) and the world writes back
// (`Transform` is a write-back row). It owns one system and no storage of its own: the two
// components are the whole of its state.
//
// What it does not do: collide, accelerate, follow a path, or rotate about anything but the
// entity's own origin. Physics is `domain/physics`'; a path is navigation's.
//
// ---- ADR-0027 decision 2, registration point by registration point ----------------------------
//
//   [x] capability graph  engine_capability_requires(kinematics ecs)
//   [x] schema types      systems/kinematics/schemas/kinematics.schema (Velocity, Mover, and the
//                         Mover mapping); Transform is schemas/world.schema's
//   [x] scheduler entry   one sim::SystemDesc, registered with ecs::register_system
//   [ ] render passes     none: it moves transforms, whoever draws reads them
//   [ ] derived data      none
//   [ ] protocol methods  none; its records reach the world through materialization
//   [ ] tunables          none: there is no parameter to tune
//   [x] LOD policy        every tier (below)
//   [x] determinism       k_determinism = "hashed"
//   [x] zero cost unused  no linked code (ENGINE_WITH_KINEMATICS=OFF) and no instances: a world
//                         with no Velocity matches no query
//   [x] docs, tests, size table, bench
//
// **LOD policy: every tier, and on purpose.** "A system that runs at every tier is usually a system
// that has not thought about it" (the scaffold's warning) — this one has: its whole cost is two
// multiply-adds and, when it turns, a quaternion product per entity, so there is nothing to
// coarsen. A record at LOD3 is not an entity at all and is not ticked; bringing it back is
// `integrate()` over the gap, which is the same function the tick calls and so meets `domain/sim`'s
// summarizer contract exactly for the linear part (an affine step is additive over a partition) and
// to float rounding for the angular part.
//
// **Determinism: hashed.** Per-entity arithmetic over the entity's own two components, no shared
// accumulator, no wall clock, floating-point contraction off tree-wide (ADR-0035): one worker and
// eight produce the same bytes, and so does the engine's scheduler against flecs' pipeline.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/scheduler.h>

#include <schemas/kinematics.h>
#include <schemas/world.h>

namespace engine::kinematics {

inline constexpr const char* k_determinism = "hashed";
inline constexpr sim::TickPhase k_phase = sim::TickPhase::Systems;

// Moves `transform` by `velocity` over `seconds`: the tick's arithmetic, and the analytic step over
// a frozen interval. Position is affine in f64 (`Transform::position` is a `WorldPos`, ADR-0053),
// so a step moves a thing as far 10,000 km out as by the origin; orientation integrates the angular
// velocity as a first-order quaternion step and renormalizes, and is left bit-for-bit alone when
// the entity does not turn — so an entity that only translates never writes its orientation back.
void integrate(world::Transform& transform, const Velocity& velocity, f32 seconds) noexcept;

// What the last tick did: stage 0's share, written once per tick, like `animation::AnimationStats`.
struct KinematicsStats {
  u32 moved = 0;
};

class KinematicsSystem {
 public:
  KinematicsSystem() = default;
  ENGINE_NON_COPYABLE(KinematicsSystem);

  // Registers `world::Transform` and `Velocity` with the world (ADR-0028 seam 1) and the integrate
  // system with the engine's descriptor (seam 2). Once per world, outside a tick.
  void install(ecs::SimWorld& sim);

  const KinematicsStats& stats() const noexcept { return stats_; }
  const sim::SystemDesc& descriptor() const noexcept { return desc_; }

 private:
  sim::SystemDesc desc_;
  KinematicsStats stats_;
};

}  // namespace engine::kinematics

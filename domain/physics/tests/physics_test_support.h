#pragma once

// Fixtures shared by the physics tests: a world, a ground plane, and a few shapes.

#include <domain/physics/physics.h>

#include <doctest/doctest.h>

namespace engine::physics::testing {

// A world with small limits: every test here is a handful of bodies, and the default limits
// would make the test binary's scratch arena the interesting number. The arena is left at 0 so
// it is derived from these, which is also what exercises that derivation.
inline WorldOptions small_world_options() {
  WorldOptions options;
  options.max_bodies = 1024;
  options.max_body_pairs = 4096;
  options.max_contact_constraints = 4096;
  return options;
}

// A static box whose top face is at y = 0.
inline BodyId add_ground(World& world, Vec3 half_extent = Vec3(50.0f, 0.5f, 50.0f)) {
  ShapeId shape;
  REQUIRE(world.create_box(half_extent, shape) == Status::Ok);
  BodyDesc desc;
  desc.shape = shape;
  desc.transform.position = Vec3(0.0f, -half_extent.y, 0.0f);
  desc.motion = MotionType::Static;
  desc.layer = Layer::Static;
  desc.friction = 0.8f;
  BodyId body;
  REQUIRE(world.create_body(desc, body) == Status::Ok);
  return body;
}

inline Vec3 position_of(const World& world, BodyId body) {
  Transform3 transform;
  REQUIRE(world.body_transform(body, transform));
  return transform.position;
}

inline void step_n(World& world, u32 count) {
  for (u32 i = 0; i < count; ++i)
    REQUIRE(world.step() == Status::Ok);
}

}  // namespace engine::physics::testing

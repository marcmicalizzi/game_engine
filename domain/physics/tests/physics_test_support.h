#pragma once

// Fixtures shared by the physics tests: a world, a ground plane, a few shapes, and the places a
// test runs at.

#include <domain/physics/physics.h>

#include <doctest/doctest.h>

#include <string>

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

// **The sites a far test runs at** (ADR-0053; physics.md, "Far from the origin"): by the origin,
// the owner's 419,072 m (cell 6,548 of the 64 m grid), 10,000 km (cell 156,250) and 1e8 m (cell
// 1,562,500), each on x and z so a scene's local axes are the world's, and each a whole number of
// 1024ths of a metre, so a position authored relative to a site is the same f64 bits plus the
// site's whichever site it is.
inline constexpr WorldPos k_sites[] = {
    WorldPos{0.0, 0.0, 0.0},
    WorldPos{419072.0, 0.0, -419072.0},
    WorldPos{10000000.0, 0.0, 10000000.0},
    WorldPos{100000000.0, 0.0, -100000000.0},
};
inline std::string site_name(u32 i) {
  constexpr const char* k_names[] = {"origin", "419 km", "10,000 km", "1e8 m"};
  return k_names[i];
}

// A point given in a site's frame, and a world point seen from one: how a test authors its scene
// once and runs it anywhere. By the origin these are the identity on the floats.
inline WorldPos place(Vec3 local, WorldPos site = WorldPos::origin()) {
  return absolute(site, local);
}
inline Vec3 local_of(WorldPos p, WorldPos site = WorldPos::origin()) { return relative(p, site); }

// A static box whose top face is at y = 0 of `site`.
inline BodyId add_ground(World& world, Vec3 half_extent = Vec3(50.0f, 0.5f, 50.0f),
                         WorldPos site = WorldPos::origin()) {
  ShapeId shape;
  REQUIRE(world.create_box(half_extent, shape) == Status::Ok);
  BodyDesc desc;
  desc.shape = shape;
  desc.transform.position = place(Vec3(0.0f, -half_extent.y, 0.0f), site);
  desc.motion = MotionType::Static;
  desc.layer = Layer::Static;
  desc.friction = 0.8f;
  BodyId body;
  REQUIRE(world.create_body(desc, body) == Status::Ok);
  return body;
}

// Where a body is, in `site`'s frame.
inline Vec3 position_of(const World& world, BodyId body, WorldPos site = WorldPos::origin()) {
  BodyTransform transform;
  REQUIRE(world.body_transform(body, transform));
  return local_of(transform.position, site);
}

inline void step_n(World& world, u32 count) {
  for (u32 i = 0; i < count; ++i)
    REQUIRE(world.step() == Status::Ok);
}

}  // namespace engine::physics::testing

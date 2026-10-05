// Rigid bodies: resting, stacking, kinematic pushing, contact events, and the debris pool.

#include "physics_test_support.h"

#include <core/containers/vector.h>
#include <domain/physics/physics.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

using namespace engine;
using namespace engine::physics;
using namespace engine::physics::testing;

TEST_CASE("physics: a sphere dropped on a static box comes to rest and falls asleep") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  add_ground(world);

  ShapeId sphere_shape;
  REQUIRE(world.create_sphere(0.5f, sphere_shape) == Status::Ok);
  BodyDesc desc;
  desc.shape = sphere_shape;
  desc.transform.position = place(Vec3(0.0f, 5.0f, 0.0f));
  BodyId sphere;
  REQUIRE(world.create_body(desc, sphere) == Status::Ok);
  CHECK(world.body_active(sphere));

  step_n(world, 600);

  const Vec3 position = position_of(world, sphere);
  // Jolt lets contacts settle inside its penetration slop (2 cm by default), so "at rest on the
  // surface" is the radius give or take that, not the radius exactly.
  CHECK(std::abs(position.y - 0.5f) < 0.03f);
  CHECK(std::abs(position.x) < 0.01f);
  CHECK(std::abs(position.z) < 0.01f);
  CHECK_FALSE(world.body_active(sphere));
  CHECK(world.active_body_count() == 0);
}

// A hundred boxes in ten towers of ten. A single hundred-high tower is not a stability test,
// it is a demonstration that sequential-impulse solvers have a stack-height limit: the bottom
// box of one carries a hundred times its own weight and the residual error at the base is
// amplified all the way up, and it falls over inside two seconds with any solver settings
// worth shipping. Ten towers of ten is the shape a game actually builds, and it also puts ten
// islands in front of the solver instead of one, which is what the drift bound is watching.
namespace {

constexpr u32 k_towers = 10;
constexpr u32 k_height = 10;
constexpr u32 k_count = k_towers * k_height;
constexpr f32 k_half = 0.5f;
// How far a settled box at a far site may stand off its place by the origin (the far case below).
constexpr f64 k_far_stack_tolerance_m = 1.0e-4;

struct Stack {
  Vector<BodyTransform> transforms;  // after the steps, in the world
  u32 active = 0;
};

// The ten towers on their ground, all of it at `site`, stepped `steps` times.
Stack settle_stack(WorldPos site, u32 steps) {
  WorldOptions options = small_world_options();
  options.max_body_pairs = 16384;
  options.max_contact_constraints = 16384;
  World world;
  REQUIRE(world.init(options) == Status::Ok);
  add_ground(world, Vec3(50.0f, 0.5f, 50.0f), site);

  ShapeId box_shape;
  REQUIRE(world.create_box(Vec3(k_half, k_half, k_half), box_shape) == Status::Ok);

  Vector<BodyId> boxes;
  boxes.reserve(k_count);
  for (u32 tower = 0; tower < k_towers; ++tower) {
    for (u32 level = 0; level < k_height; ++level) {
      BodyDesc desc;
      desc.shape = box_shape;
      desc.transform.position =
          place(Vec3(static_cast<f32>(tower) * 3.0f - 13.5f,
                     k_half + static_cast<f32>(level) * (2.0f * k_half), 0.0f),
                site);
      desc.friction = 0.8f;
      BodyId body;
      REQUIRE(world.create_body(desc, body) == Status::Ok);
      boxes.push_back(body);
    }
  }
  world.optimize_broad_phase();

  step_n(world, steps);

  Stack out;
  out.transforms.resize(k_count);
  world.read_transforms(std::span<const BodyId>(boxes), std::span<BodyTransform>(out.transforms));
  out.active = world.active_body_count();
  return out;
}

}  // namespace

TEST_CASE("physics: a 100-box stack is still standing after 600 steps") {
  const Stack stack = settle_stack(WorldPos::origin(), 600);
  const Vector<BodyTransform>& transforms = stack.transforms;

  f32 max_drift = 0.0f;
  f32 max_sink = 0.0f;
  for (u32 tower = 0; tower < k_towers; ++tower) {
    const f32 tower_x = static_cast<f32>(tower) * 3.0f - 13.5f;
    for (u32 level = 0; level < k_height; ++level) {
      const Vec3 p = local_of(transforms[tower * k_height + level].position);
      const f32 dx = p.x - tower_x;
      const f32 drift = std::sqrt(dx * dx + p.z * p.z);
      if (drift > max_drift) max_drift = drift;
      const f32 sink = k_half + static_cast<f32>(level) - p.y;
      if (sink > max_sink) max_sink = sink;
    }
  }
  // Half a box is the point at which a box is no longer over the one below it and the tower is
  // going to fall whatever the next 600 steps do, so that is the bound worth asserting.
  CHECK(max_drift < 0.5f * k_half);
  // And nothing has sunk into the stack: the contacts settle inside the penetration slop, not
  // through it.
  CHECK(max_sink < 0.2f);
  CHECK(stack.active == 0);  // ten settled towers are asleep by now
}

// **Far from the origin, the stack settles where it settles by the origin** (ADR-0053; physics.md,
// "Far from the origin"). The ground and the hundred boxes moved to each far site, stepped the
// same 600 times, come to rest at the same places from the site and fall asleep the same way. What
// can differ is only where f64 rounds an absolute position — a contact's arithmetic is floats
// relative to the bodies, the same at every site — and a stack carries a rounding forward through
// its contacts for 600 steps; the bound is what that measured, with a margin, and a float32 build
// fails it by its whole step (3.1 cm at 419 km, a metre at 10,000 km).
TEST_CASE("physics: the 100-box stack at 419 km, 10,000 km and 1e8 m settles as by the origin") {
  const Stack home = settle_stack(WorldPos::origin(), 600);
  for (u32 s = 1; s < 4; ++s) {
    const Stack far = settle_stack(k_sites[s], 600);
    f64 worst = 0.0;
    f32 worst_turn = 0.0f;
    for (u32 i = 0; i < k_count; ++i) {
      const DVec3 d =
          (far.transforms[i].position - k_sites[s]) - (home.transforms[i].position - WorldPos{});
      worst = std::max(worst, length(d));
      const Quat a = far.transforms[i].rotation;
      const Quat b = home.transforms[i].rotation;
      worst_turn = std::max(
          worst_turn, std::abs(std::abs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) - 1.0f));
    }
    MESSAGE(site_name(s) << ": a box stood off its place by the origin by at most " << worst
                         << " m; rotations agreed to " << worst_turn << " of a unit dot");
    CHECK(worst < k_far_stack_tolerance_m);
    CHECK(worst_turn < 1.0e-4f);
    CHECK(far.active == home.active);
  }
}

TEST_CASE("physics: a kinematic box pushes a dynamic one") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  add_ground(world);

  ShapeId box_shape;
  REQUIRE(world.create_box(Vec3(0.5f, 0.5f, 0.5f), box_shape) == Status::Ok);

  BodyDesc dynamic_desc;
  dynamic_desc.shape = box_shape;
  dynamic_desc.transform.position = place(Vec3(0.0f, 0.5f, 0.0f));
  dynamic_desc.friction = 0.4f;
  BodyId crate;
  REQUIRE(world.create_body(dynamic_desc, crate) == Status::Ok);

  BodyDesc pusher_desc;
  pusher_desc.shape = box_shape;
  pusher_desc.transform.position = place(Vec3(-3.0f, 0.5f, 0.0f));
  pusher_desc.motion = MotionType::Kinematic;
  pusher_desc.layer = Layer::Kinematic;
  BodyId pusher;
  REQUIRE(world.create_body(pusher_desc, pusher) == Status::Ok);

  // Walk the pusher along +x at 2 m/s. MoveKinematic sets the velocity that covers the gap in
  // one step, which is what lets the solver see it coming instead of teleporting through.
  const f32 dt = world.step_seconds();
  f32 x = -3.0f;
  for (u32 i = 0; i < 240; ++i) {
    x += 2.0f * dt;
    BodyTransform target;
    target.position = place(Vec3(x, 0.5f, 0.0f));
    REQUIRE(world.move_kinematic(pusher, target, dt));
    REQUIRE(world.step() == Status::Ok);
  }

  const Vec3 crate_position = position_of(world, crate);
  const Vec3 pusher_position = position_of(world, pusher);
  CHECK(pusher_position.x == doctest::Approx(x).epsilon(0.02));
  CHECK(crate_position.x > 1.0f);               // it was pushed
  CHECK(crate_position.x > pusher_position.x);  // and it is still in front of the pusher
  CHECK(std::abs(crate_position.y - 0.5f) < 0.1f);
}

TEST_CASE("physics: contact begin is reported once per pair") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  const BodyId ground = add_ground(world);

  ShapeId sphere_shape;
  REQUIRE(world.create_sphere(0.5f, sphere_shape) == Status::Ok);
  BodyDesc desc;
  desc.shape = sphere_shape;
  desc.transform.position = place(Vec3(0.0f, 0.7f, 0.0f));
  desc.user_data = 0xABCDu;
  // Sleeping would end the contact and start it again on the next touch; this test is about
  // the listener, not about sleep.
  desc.allow_sleeping = false;
  BodyId sphere;
  REQUIRE(world.create_body(desc, sphere) == Status::Ok);

  u32 begins = 0;
  u32 persists = 0;
  u32 ends = 0;
  Vec3 first_normal{};
  for (u32 i = 0; i < 180; ++i) {
    REQUIRE(world.step() == Status::Ok);
    for (const ContactEvent& event : world.contact_events()) {
      const bool is_pair =
          (event.a == ground && event.b == sphere) || (event.a == sphere && event.b == ground);
      CHECK(is_pair);
      switch (event.phase) {
        case ContactPhase::Begin:
          if (begins == 0) first_normal = event.a == sphere ? event.normal : -event.normal;
          ++begins;
          break;
        case ContactPhase::Persist: ++persists; break;
        case ContactPhase::End: ++ends; break;
      }
    }
  }

  CHECK(begins == 1);
  CHECK(persists > 100);
  CHECK(ends == 0);
  // The normal points from the ground towards the sphere: straight up.
  CHECK(first_normal.y > 0.99f);
  // The user data rides along, so a consumer does not have to keep its own handle table.
  bool seen_user_data = false;
  for (const ContactEvent& event : world.contact_events()) {
    if (event.a == sphere) seen_user_data = event.user_data_a == 0xABCDu;
    if (event.b == sphere) seen_user_data = event.user_data_b == 0xABCDu;
  }
  CHECK(seen_user_data);
}

TEST_CASE("physics: the debris pool recycles the oldest piece") {
  WorldOptions options = small_world_options();
  options.debris_cap = 4;
  World world;
  REQUIRE(world.init(options) == Status::Ok);
  add_ground(world);

  ShapeId chip;
  REQUIRE(world.create_box(Vec3(0.1f, 0.1f, 0.1f), chip) == Status::Ok);

  Vector<BodyId> spawned;
  for (u32 i = 0; i < 4; ++i) {
    BodyTransform at;
    at.position = place(Vec3(static_cast<f32>(i), 3.0f, 0.0f));
    BodyId piece;
    REQUIRE(world.spawn_debris(chip, at, Vec3(0.0f, 1.0f, 0.0f), piece) == Status::Ok);
    spawned.push_back(piece);
  }
  CHECK(world.debris_count() == 4);
  CHECK(world.debris_cap() == 4);
  for (const BodyId piece : spawned)
    CHECK(world.contains(piece));

  // One past the cap: the oldest goes, the newest arrives, the count does not move.
  BodyTransform at;
  at.position = place(Vec3(9.0f, 3.0f, 0.0f));
  BodyId fifth;
  REQUIRE(world.spawn_debris(chip, at, Vec3::zero(), fifth) == Status::Ok);
  CHECK_FALSE(world.contains(spawned[0]));
  CHECK(world.contains(spawned[1]));
  CHECK(world.contains(fifth));
  CHECK(world.debris_count() == 4);

  // Debris never collides with debris, so a thousand chips in one place cost nothing extra.
  Layer layer = Layer::Moving;
  REQUIRE(world.body_layer(fifth, layer));
  CHECK(layer == Layer::Debris);
  CHECK_FALSE(layers_collide(Layer::Debris, Layer::Debris));

  world.clear_debris();
  CHECK(world.debris_count() == 0);
}

TEST_CASE("physics: shapes are shared and outlive nothing that uses them") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);

  ShapeId box_shape;
  REQUIRE(world.create_box(Vec3(1.0f, 2.0f, 3.0f), box_shape) == Status::Ok);
  Aabb3 bounds;
  REQUIRE(world.shape_bounds(box_shape, bounds));
  CHECK(bounds.min.x == doctest::Approx(-1.0f));
  CHECK(bounds.max.y == doctest::Approx(2.0f));

  BodyDesc desc;
  desc.shape = box_shape;
  desc.transform.position = place(Vec3(0.0f, 10.0f, 0.0f));
  BodyId first;
  BodyId second;
  REQUIRE(world.create_body(desc, first) == Status::Ok);
  REQUIRE(world.create_body(desc, second) == Status::Ok);
  CHECK(world.shape_count() == 1);

  CHECK_FALSE(world.destroy_shape(box_shape));  // two bodies still stand on it
  CHECK(world.destroy_body(first));
  CHECK_FALSE(world.destroy_shape(box_shape));
  CHECK(world.destroy_body(second));
  CHECK(world.destroy_shape(box_shape));
  CHECK(world.shape_count() == 0);
  CHECK_FALSE(world.contains(first));
}

TEST_CASE("physics: a mesh or heightfield shape refuses to be dynamic") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);

  const Vec3 positions[4] = {Vec3(-1.0f, 0.0f, -1.0f), Vec3(1.0f, 0.0f, -1.0f),
                             Vec3(1.0f, 0.0f, 1.0f), Vec3(-1.0f, 0.0f, 1.0f)};
  const u32 indices[6] = {0, 1, 2, 0, 2, 3};
  ShapeId mesh;
  REQUIRE(world.create_mesh(std::span<const Vec3>(positions), std::span<const u32>(indices),
                            mesh) == Status::Ok);

  BodyDesc desc;
  desc.shape = mesh;
  desc.motion = MotionType::Dynamic;
  BodyId body;
  CHECK(world.create_body(desc, body) == Status::Unsupported);

  desc.motion = MotionType::Static;
  desc.layer = Layer::Static;
  CHECK(world.create_body(desc, body) == Status::Ok);

  // And the validation in front of it rejects nonsense before the backend sees it.
  const u32 bad[3] = {0, 1, 99};
  ShapeId unused;
  CHECK(world.create_mesh(std::span<const Vec3>(positions), std::span<const u32>(bad), unused) ==
        Status::InvalidArgument);
  CHECK(world.create_sphere(-1.0f, unused) == Status::InvalidArgument);
}

TEST_CASE("physics: a shape says what the backend holds for it") {
  World world;
  REQUIRE(world.init(small_world_options()) == Status::Ok);
  const Vec3 positions[4] = {Vec3(-1.0f, 0.0f, -1.0f), Vec3(1.0f, 0.0f, -1.0f),
                             Vec3(1.0f, 0.0f, 1.0f), Vec3(-1.0f, 0.0f, 1.0f)};
  const u32 indices[6] = {0, 1, 2, 0, 2, 3};
  ShapeId mesh;
  REQUIRE(world.create_mesh(std::span<const Vec3>(positions), std::span<const u32>(indices),
                            mesh) == Status::Ok);
  u64 bytes = 0;
  u32 triangles = 0;
  REQUIRE(world.shape_memory(mesh, false, bytes, triangles));
  CHECK(bytes > 0);
  CHECK(triangles == 2);
  // A compound's own bytes, and with its children: the mesh under it counted once however many
  // times it is placed.
  const CompoundChild children[2] = {
      {mesh, Transform3{}},
      {mesh, Transform3{Vec3(3.0f, 0.0f, 0.0f), Quat::identity(), Vec3(1.0f, 1.0f, 1.0f)}}};
  ShapeId compound;
  REQUIRE(world.create_compound(std::span<const CompoundChild>(children), compound) == Status::Ok);
  u64 own = 0;
  u64 all = 0;
  u32 own_triangles = 0;
  u32 all_triangles = 0;
  REQUIRE(world.shape_memory(compound, false, own, own_triangles));
  REQUIRE(world.shape_memory(compound, true, all, all_triangles));
  CHECK(all == own + bytes);
  CHECK(all_triangles == 4);  // the triangles as placed: the mesh's two, twice
  CHECK_FALSE(world.shape_memory(ShapeId{}, true, all, all_triangles));
}

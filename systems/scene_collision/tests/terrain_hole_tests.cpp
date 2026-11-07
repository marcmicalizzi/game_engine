// A terrain's hole in the collision ground (scene_collision.h, `ground_height`; terrain.md,
// "Holes"; roadmap W87): the heightfield has no surface over a hole, the ground query says none
// there, and a walker who steps in falls to what stands below — here a floor of the test's own,
// where a cave's passage would stand.
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::scene_collision;

namespace {

Vec3 local_of(WorldPos p) { return relative(p, WorldPos::origin()); }
WorldPos place(Vec3 local) { return absolute(WorldPos::origin(), local); }

// Flat ground at y = 0.
void flat_destroy(void*) noexcept {}
f32 flat_height(const void*, f32, f32) noexcept { return 0.0f; }
constexpr scene_gen::GroundOps k_flat_ops{.destroy = &flat_destroy, .height = &flat_height};

world::RingParams ring() {
  world::RingParams p;
  p.tile_size = 32.0f;
  p.ring_count = 1;
  for (f32& r : p.radius)
    r = 0.0f;
  p.radius[0] = 1.5f;
  return p;
}

sim::ObserverSet observers(f32 x, f32 z) {
  sim::ObserverSet set;
  set.add(WorldPos{static_cast<f64>(x), 0.0, static_cast<f64>(z)}, 1.0f);
  return set;
}

bool hit_below(const physics::World& world, f32 x, f32 z, f32& y) {
  physics::RayHit hit;
  if (!world.cast_ray(place(Vec3{x, 50.0f, z}), Vec3{0.0f, -100.0f, 0.0f}, hit)) return false;
  y = local_of(hit.position).y;
  return true;
}

}  // namespace

TEST_CASE(
    "scene_collision: a terrain's hole has no ground, and a walker falls to the floor below") {
  // A 6 m round hole at (16, 16), and a floor 4 m under the ground below it.
  constexpr f32 k_cx = 16.0f, k_cz = 16.0f, k_floor = -4.0f;
  constexpr f64 cx = 16.0, cz = 16.0;
  scene_gen::GroundProvider ground(&k_flat_ops, nullptr);
  renderer::SceneData scene;
  scene::TerrainHole hole;
  hole.center = WorldPos{static_cast<f64>(k_cx), 0.0, static_cast<f64>(k_cz)};
  hole.radius_m = 3.0f;
  REQUIRE(scene.terrain.holes.build({&hole, 1}));
  physics::World physics;
  physics::WorldOptions options;
  options.max_bodies = 256;
  options.max_body_pairs = 1024;
  options.max_contact_constraints = 1024;
  REQUIRE(physics.init(options) == physics::Status::Ok);
  physics::ShapeId floor_shape;
  REQUIRE(physics.create_box(Vec3{5.0f, 0.5f, 5.0f}, floor_shape) == physics::Status::Ok);
  physics::BodyDesc floor;
  floor.shape = floor_shape;
  floor.transform.position = place(Vec3{k_cx, k_floor - 0.5f, k_cz});
  floor.motion = physics::MotionType::Static;
  floor.layer = physics::Layer::Static;
  physics::BodyId floor_body;
  REQUIRE(physics.create_body(floor, floor_body) == physics::Status::Ok);

  SceneCollision collision;
  std::string error;
  REQUIRE_MESSAGE(collision.create(physics, scene, &ground, Config{}, &error), error);
  world::World world(ring());
  world.add_consumer(collision.consumer());
  world.update(observers(k_cx, k_cz), 0, true);
  REQUIRE(collision.stats().bodies >= 9);

  // The ground query: none inside (a metre from the centre, and 1.9 m), the ground outside.
  f32 y = 1.0f;
  CHECK_FALSE(collision.ground_height(WorldPos{cx, 0.0, cz}, y));
  CHECK_FALSE(collision.ground_height(WorldPos{cx + 1.9, 0.0, cz + 0.3}, y));
  REQUIRE(collision.ground_height(WorldPos{cx + 4.5, 0.0, cz}, y));
  CHECK(y == 0.0f);
  REQUIRE(collision.ground_height(WorldPos{cx - 10.0, 0.0, cz + 7.0}, y));
  // Straight down: the floor through the hole, the ground beside it.
  f32 top = 0.0f;
  REQUIRE(hit_below(physics, k_cx + 0.4f, k_cz - 0.7f, top));
  CHECK(top == doctest::Approx(k_floor).epsilon(1.0e-3));
  REQUIRE(hit_below(physics, k_cx + 4.5f, k_cz, top));
  CHECK(top == doctest::Approx(0.0f).epsilon(1.0e-3));

  // A walker 6 m west of the centre walks east into the hole, falls, and stands on the floor.
  physics::CharacterBody walker;
  physics::CharacterConfig c;
  c.step_hz = 240;
  REQUIRE(walker.create(physics, c, place(Vec3{k_cx - 6.0f, 0.02f, k_cz})) == physics::Status::Ok);
  physics::CharacterInput east;
  east.move = Vec3{1.0f, 0.0f, 0.0f};
  for (u32 k = 0; k < 4 * 240; ++k)  // 6 m at 1.5 m/s: to the centre
    REQUIRE(walker.step(east) == physics::Status::Ok);
  const physics::CharacterInput stand;
  for (u32 k = 0; k < 2 * 240; ++k)
    REQUIRE(walker.step(stand) == physics::Status::Ok);
  const Vec3 feet = local_of(walker.state().position);
  MESSAGE("walked east into the hole: feet at (" << feet.x << ", " << feet.y << ", " << feet.z
                                                 << ")");
  CHECK(feet.y == doctest::Approx(k_floor).epsilon(0.02));
  CHECK(walker.state().ground == physics::Ground::OnGround);
}

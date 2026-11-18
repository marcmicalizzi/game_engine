// A game's props stop a walker (roadmap P45; docs/subsystems/scene_collision.md, "Props"): a prop
// a host hands (`SceneCollision::set_props`) is a static body of its mesh's proxy, of the box its
// scene declares, or of nothing, while the tile it stands in is held, and goes with the tile, with
// its removal, and when it moves. No device, no generator capability: the scene is the test's own,
// one mesh the load placed nowhere — a crate a metre on a side — declared as a prop.

#include <domain/geometry/cluster_lod.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::scene_collision;

namespace {

// A crate: a box a metre on a side standing on y = 0 round the mesh's origin, faces outwards.
void crate(Vector<Vec3>& positions, Vector<u32>& indices) {
  for (u32 k = 0; k < 8; ++k)
    positions.push_back(
        Vec3{(k & 1) ? 0.5f : -0.5f, (k & 2) ? 1.0f : 0.0f, (k & 4) ? 0.5f : -0.5f});
  const u32 faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
  for (const auto& f : faces) {
    const u32 tri[6] = {f[0], f[1], f[2], f[0], f[2], f[3]};
    for (const u32 v : tri)
      indices.push_back(v);
  }
}

// The scene: the crate as mesh 0, declared as the prop "crate" and placed by nothing.
void crate_scene(renderer::SceneData& scene) {
  Vector<Vec3> positions;
  Vector<u32> indices;
  crate(positions, indices);
  geometry::ClusterLodMesh lod;
  std::string error;
  REQUIRE_MESSAGE(
      geometry::build_cluster_lod(std::span<const Vec3>(positions.data(), positions.size()),
                                  std::span<const u32>(indices.data(), indices.size()),
                                  geometry::ClusterLodOptions{}, lod, &error),
      error);
  REQUIRE_MESSAGE(geometry::merge_cluster_meshes(std::span<const geometry::ClusterLodMesh>(&lod, 1),
                                                 scene.lod, scene.parts, &error),
                  error);
  scene.mesh_fit.resize(scene.parts.size(), Mat4::identity());
  scene.terrain_mesh = ~0u;
  scene.props.push_back(renderer::SceneProp{"crate", 0});
}

WorldPos place(Vec3 local) { return absolute(WorldPos::origin(), local); }

PropBody crate_at(f32 z, u32 key = 7) {
  PropBody p;
  p.key = key;
  p.mesh = 0;
  p.origin = place(Vec3{0.0f, 0.0f, z});
  return p;
}

// A walker on a floor at y = 0 and one tile ring round it, the props handed and updated as a host
// does on the tick, after the ring's update; `walk` steps it north from z = 15.
struct Rig {
  physics::World physics;
  SceneCollision collision;
  std::unique_ptr<world::World> world;
  physics::CharacterBody walker;
  sim::ObserverSet observers;
  u64 tick = 0;

  explicit Rig(const renderer::SceneData& scene) {
    physics::WorldOptions o;
    o.max_bodies = 1024;
    o.max_body_pairs = 4096;
    o.max_contact_constraints = 4096;
    REQUIRE(physics.init(o) == physics::Status::Ok);
    physics::ShapeId floor_shape;
    REQUIRE(physics.create_box(Vec3{200.0f, 0.5f, 200.0f}, floor_shape) == physics::Status::Ok);
    physics::BodyDesc floor;
    floor.shape = floor_shape;
    floor.transform.position = place(Vec3{0.0f, -0.5f, 0.0f});
    floor.motion = physics::MotionType::Static;
    floor.layer = physics::Layer::Static;
    physics::BodyId floor_body;
    REQUIRE(physics.create_body(floor, floor_body) == physics::Status::Ok);
    std::string error;
    REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
    world::RingParams ring;
    ring.tile_size = 32.0f;
    ring.ring_count = 1;
    for (f32& r : ring.radius)
      r = 0.0f;
    ring.radius[0] = 1.5f;
    world = std::make_unique<world::World>(ring);
    world->add_consumer(collision.consumer());
    move_ring(place(Vec3{0.0f, 0.0f, 16.0f}));
    physics::CharacterConfig c;
    c.step_hz = 240;
    REQUIRE(walker.create(physics, c, place(Vec3{0.0f, 0.02f, 15.0f})) == physics::Status::Ok);
  }
  void move_ring(WorldPos at) {
    observers.clear();
    observers.add(at, 1.0f);
    world->update(observers, tick++, true);
    (void)collision.update_props();
  }
  void hand(std::initializer_list<PropBody> props) {
    const Vector<PropBody> list(props);
    collision.set_props({list.data(), list.size()});
    (void)collision.update_props();
  }
  f32 walk() {
    walker.teleport(place(Vec3{0.0f, 0.02f, 15.0f}));
    physics::CharacterInput north;
    north.move = Vec3{0.0f, 0.0f, 1.0f};
    for (u32 k = 0; k < 6 * 240; ++k)
      REQUIRE(walker.step(north) == physics::Status::Ok);
    return relative(walker.state().position, WorldPos::origin()).z;
  }
};

}  // namespace

TEST_CASE(
    "scene_collision: a game's prop stops a walker, by its mesh or its box, while its tile "
    "is held") {
  renderer::SceneData scene;
  crate_scene(scene);
  const f32 radius = physics::CharacterConfig{}.radius;
  Rig rig(scene);
  // No props: the walker goes six seconds north unstopped.
  const f32 free_z = rig.walk();
  CHECK(free_z > 23.5f);
  CHECK(rig.collision.stats().prop_bodies == 0);

  // The crate at z = 20 by its mesh: stopped at its face, 19.5, less the capsule's radius.
  rig.hand({crate_at(20.0f)});
  CHECK(rig.collision.stats().props == 1);
  CHECK(rig.collision.stats().prop_bodies == 1);
  const f32 at_mesh = rig.walk();
  MESSAGE("the crate by its mesh: stopped at z " << at_mesh);
  CHECK(at_mesh == doctest::Approx(19.5f - radius).epsilon(0.002));

  // Handed again unchanged: the body is kept, nothing is made.
  const u64 made = rig.collision.stats().prop_bodies_created;
  rig.hand({crate_at(20.0f)});
  CHECK(rig.collision.stats().prop_bodies_created == made);

  // Moved 3 m on (a module's `prop_move`): the old body goes, a new one stops the walker there.
  rig.hand({crate_at(23.0f)});
  CHECK(rig.collision.stats().prop_bodies == 1);
  CHECK(rig.collision.stats().prop_bodies_destroyed == 1);
  CHECK(rig.walk() == doctest::Approx(22.5f - radius).epsilon(0.002));

  // Twice the size: its proxy at that scale, the face a metre out.
  PropBody big = crate_at(20.0f);
  big.scale = 2.0f;
  rig.hand({big});
  CHECK(rig.walk() == doctest::Approx(19.0f - radius).epsilon(0.002));

  // A declared box, 0.25 m deep about a centre 0.5 m up: its face at 19.75.
  PropBody boxed = crate_at(20.0f);
  boxed.collider.shape = renderer::PropShape::box;
  boxed.collider.half_extents = Vec3{1.0f, 0.5f, 0.25f};
  boxed.collider.center = Vec3{0.0f, 0.5f, 0.0f};
  rig.hand({boxed});
  CHECK(rig.walk() == doctest::Approx(19.75f - radius).epsilon(0.002));

  // None: no body, the walker goes through.
  PropBody ghost = crate_at(20.0f);
  ghost.collider.shape = renderer::PropShape::none;
  rig.hand({ghost});
  CHECK(rig.collision.stats().prop_bodies == 0);
  CHECK(rig.walk() > 23.5f);

  // Removed: the body goes with it.
  rig.hand({crate_at(20.0f)});
  CHECK(rig.collision.stats().prop_bodies == 1);
  rig.hand({});
  CHECK(rig.collision.stats().props == 0);
  CHECK(rig.collision.stats().prop_bodies == 0);

  // **With the tile**: the ring moved a kilometre away lets the crate's tile go and its body with
  // it; back, the tile comes in and the body is made again.
  rig.hand({crate_at(20.0f)});
  rig.move_ring(place(Vec3{1000.0f, 0.0f, 1000.0f}));
  CHECK(rig.collision.stats().props == 1);
  CHECK(rig.collision.stats().prop_bodies == 0);
  rig.move_ring(place(Vec3{0.0f, 0.0f, 16.0f}));
  CHECK(rig.collision.stats().prop_bodies == 1);
  CHECK(rig.walk() == doctest::Approx(19.5f - radius).epsilon(0.002));

  // A cast names the prop it met by its key (beside the walker's capsule, inside the crate's side).
  physics::RayHit hit;
  REQUIRE(rig.physics.cast_ray(place(Vec3{0.45f, 0.5f, 12.0f}), Vec3{0.0f, 0.0f, 20.0f}, hit));
  const HitInfo info = rig.collision.describe_hit(hit.body, hit.child);
  CHECK(info.kind == HitKind::prop);
  CHECK(info.instance == 7);
  CHECK(info.mesh == 0);
}

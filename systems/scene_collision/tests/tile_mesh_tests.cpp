// A tile's meshes in the collision (docs/subsystems/scene_collision.md, "A tile's meshes"; roadmap
// R145): a placement generator whose tile hands its own mesh — the fixture's rock a tile
// (tests/support/tile_mesh_fixture.h) — gets a proxy keyed by the mesh's identity, built from the
// recipe with no device and no GPU scene, resident while a held tile or a build ahead names it.

#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <tile_mesh_fixture.h>

using namespace engine;
using namespace engine::scene_collision;

namespace {

WorldPos place(Vec3 local) { return absolute(WorldPos::origin(), local); }
Vec3 local_of(WorldPos p) { return relative(p, WorldPos::origin()); }

world::RingParams ring() {
  world::RingParams p;
  p.tile_size = 32.0f;
  p.ring_count = 1;
  for (f32& r : p.radius)
    r = 0.0f;
  p.radius[0] = 1.5f;
  return p;
}

sim::ObserverSet at(f64 x, f64 z) {
  sim::ObserverSet set;
  set.add(WorldPos{x, 0.0, z}, 1.0f);
  return set;
}

physics::WorldOptions physics_options() {
  physics::WorldOptions o;
  o.max_bodies = 1024;
  o.max_body_pairs = 4096;
  o.max_contact_constraints = 4096;
  return o;
}

bool hit_below(const physics::World& world, f32 x, f32 z, f32& y) {
  physics::RayHit hit;
  if (!world.cast_ray(place(Vec3{x, 500.0f, z}), Vec3{0.0f, -1000.0f, 0.0f}, hit)) return false;
  y = local_of(hit.position).y;
  return true;
}

// A streamed scene with no mesh of its own and one entry of the fixture's rocks: every tile within
// `reach` of the origin hands its rock, two placements of it (one at scale 1 at (0.3, 0.35) of the
// tile, one at 0.75 turned a quarter at (0.7, 0.65)). `ddc` empty builds every mesh in memory.
void rocks_scene(i32 reach, i32 share, const std::string& ddc, renderer::SceneData& scene) {
  test::register_tile_rocks();
  renderer::StreamedPlacements entry;
  entry.generator = "test-tile-rocks";
  const std::string params =
      "{\"reach\":" + std::to_string(reach) + ",\"n\":12,\"share\":" + std::to_string(share) + "}";
  REQUIRE(parse_json(params, entry.params).ok);
  entry.where = "test.json: placements 0";
  entry.tile_meshes = true;
  scene.streamed.push_back(entry);
  scene.tile_meshes = true;
  scene.mesh_options.ddc = ddc;
  scene.mesh_options.cache = !ddc.empty();
  scene.terrain_mesh = ~0u;
  scene.world.enabled = true;
  scene.dynamic = true;
}

// A flat floor at y = 0 for a walker (the scene has no ground provider).
void add_floor(physics::World& physics) {
  physics::ShapeId shape;
  REQUIRE(physics.create_box(Vec3{200.0f, 0.5f, 200.0f}, shape) == physics::Status::Ok);
  physics::BodyDesc floor;
  floor.shape = shape;
  floor.transform.position = place(Vec3{0.0f, -0.5f, 0.0f});
  floor.motion = physics::MotionType::Static;
  floor.layer = physics::Layer::Static;
  physics::BodyId body;
  REQUIRE(physics.create_body(floor, body) == physics::Status::Ok);
}

}  // namespace

TEST_CASE("scene_collision: a tile's own mesh has a proxy, stood on and walked into") {
  renderer::SceneData scene;
  rocks_scene(2, 1, std::string(), scene);
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  add_floor(physics);
  const u32 builds_before = test::tile_rock_builds().load();
  {
    SceneCollision collision;
    std::string error;
    REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
    CHECK(collision.streamed());
    // Nothing is built when the consumer is made: a region nobody is near costs its entry.
    CHECK(test::tile_rock_builds().load() == builds_before);
    world::World world(ring());
    world.add_consumer(collision.consumer());
    world.update(at(16.0, 16.0), 0, true);
    // The 3 x 3 tiles round (0, 0): a rock each, two placements of it at two scales.
    const Stats& s = collision.stats();
    CHECK(s.tiles == 9);
    CHECK(s.placements == 18);
    CHECK(s.bodies == 9);
    CHECK(s.tile_proxies == 18);
    CHECK(s.proxies == 18);
    CHECK(s.proxy_triangles > 0);
    CHECK(s.proxy_bytes > 0);
    // One build a rock, though each is cut at two scales.
    CHECK(test::tile_rock_builds().load() == builds_before + 9);
    MESSAGE("nine tiles' rocks: " << s.tile_proxies << " proxies of " << s.proxy_triangles
                                  << " triangles, " << s.proxy_bytes << " bytes; compounds "
                                  << s.compound_bytes << " bytes");

    // The rock of tile (0, 0) at scale 1 stands at (9.6, 11.2): a dome 6 m across, at least a
    // metre high at its middle and down to the floor at its edge.
    f32 top = 0.0f;
    REQUIRE(hit_below(physics, 9.6f, 11.2f, top));
    CHECK(top > 0.95f);
    CHECK(top < 3.0f);
    REQUIRE(hit_below(physics, 16.0f, 2.0f, top));  // between rocks: the floor
    CHECK(top == doctest::Approx(0.0f).epsilon(1.0e-3));

    // A walker dropped onto the dome comes to rest on it, above the floor.
    physics::CharacterBody stander;
    physics::CharacterConfig c;
    c.step_hz = 240;
    REQUIRE(stander.create(physics, c, place(Vec3{9.6f, 4.0f, 11.2f})) == physics::Status::Ok);
    for (u32 k = 0; k < 2 * 240; ++k)
      REQUIRE(stander.step(physics::CharacterInput{}) == physics::Status::Ok);
    const Vec3 stood = local_of(stander.state().position);
    MESSAGE("dropped onto the rock at (9.6, 11.2): came to rest at y " << stood.y);
    CHECK(stood.y > 0.9f);

    // And one walking east along the floor into the dome is stopped at its edge (the rock's side
    // reaches the floor at x = 6.6): it does not walk through the rock.
    physics::CharacterBody walker;
    REQUIRE(walker.create(physics, c, place(Vec3{4.0f, 0.02f, 11.2f})) == physics::Status::Ok);
    physics::CharacterInput east;
    east.move = Vec3{1.0f, 0.0f, 0.0f};
    f32 highest = 0.0f;
    for (u32 k = 0; k < 4 * 240; ++k) {
      REQUIRE(walker.step(east) == physics::Status::Ok);
      highest = std::max(highest, local_of(walker.state().position).y);
    }
    const Vec3 walked = local_of(walker.state().position);
    MESSAGE("walked east at the rock from x 4: at x " << walked.x << ", highest y " << highest);
    CHECK(walked.x < 8.0f);  // four seconds of walking would have carried it past x = 9.6
    CHECK(highest < 1.0f);

    // Away: the tiles go, and every proxy of theirs with them (residency by the collision's tiles).
    world.update(at(3216.0, 16.0), 1, true);
    CHECK(collision.stats().tiles == 9);
    CHECK(collision.stats().placements == 0);
    CHECK(collision.stats().tile_proxies == 0);
    CHECK(collision.stats().proxy_triangles == 0);
    CHECK(collision.stats().proxy_bytes == 0);
    // Back: built again (no cache here, so each is a build).
    world.update(at(16.0, 16.0), 2, true);
    CHECK(collision.stats().tile_proxies == 18);
    CHECK(test::tile_rock_builds().load() == builds_before + 18);
  }
  // The floor's box is the only shape left in the table; nothing built is alive.
  CHECK(physics.built_shape_count() == 0);
  CHECK(physics.shape_count() == 1);
}

TEST_CASE("scene_collision: tiles handing one recipe share one proxy, read from the cache") {
  test::TempDir scratch("collision-tile-meshes");
  const std::string ddc = scratch.file("ddc");
  renderer::SceneData scene;
  rocks_scene(3, 2, ddc, scene);  // each 2 x 2 group of tiles names one rock
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  const u32 builds_before = test::tile_rock_builds().load();
  {
    SceneCollision collision;
    std::string error;
    REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
    world::World world(ring());
    world.add_consumer(collision.consumer());
    world.update(at(16.0, 16.0), 0, true);
    // Tiles -1..1 squared fall in four groups: (-1, -1), (-1, 0), (0, -1), (0, 0).
    CHECK(collision.stats().placements == 18);
    CHECK(collision.stats().tile_proxies == 8);  // four rocks at two scales
    CHECK(test::tile_rock_builds().load() == builds_before + 4);
    // One tile east: column x = 2 comes in (group 1, a new rock), column -1 goes — and with it the
    // last tiles of the x = -1 groups, whose proxies go.
    world.update(at(48.0, 16.0), 1, true);
    CHECK(collision.stats().placements == 18);
    CHECK(collision.stats().tile_proxies == 8);
    CHECK(test::tile_rock_builds().load() == builds_before + 6);
    // Back west: the x = -1 groups' rocks again, out of the cache this time — a read, not a build.
    world.update(at(16.0, 16.0), 2, true);
    CHECK(collision.stats().tile_proxies == 8);
    CHECK(test::tile_rock_builds().load() == builds_before + 6);
  }
  CHECK(physics.built_shape_count() == 0);
  CHECK(physics.shape_count() == 0);
}

TEST_CASE("scene_collision: a tile's mesh built ahead and never activated leaks no proxy") {
  renderer::SceneData scene;
  rocks_scene(6, 1, std::string(), scene);
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  jobs::JobSystemConfig one;
  one.performance_workers = 1;
  one.efficiency_workers = 1;
  jobs::JobSystem workers(one);
  {
    SceneCollision collision;
    Config config;
    config.prefetch_radius_tiles = 3.5f;
    std::string error;
    REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, config, &error, nullptr, &workers),
                    error);
    world::World world(ring());
    world.add_consumer(collision.consumer());
    const WorldPos origin{16.0, 0.0, 16.0};
    collision.prefetch(origin);
    const u64 submitted = collision.stats().prefetch_submitted;
    CHECK(submitted > 9);
    // Every build ahead makes its tile's compound and two proxies: wait for all of them.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    while (physics.built_shape_count() < submitted * 3 && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(physics.built_shape_count() == submitted * 3);
    world.update(at(16.0, 16.0), 0, true);
    CHECK(collision.stats().tiles == 9);
    CHECK(collision.stats().placements_ahead == 9);
    CHECK(collision.stats().placements == 18);
    CHECK(collision.stats().tile_proxies == submitted * 2);  // the held tiles' and the builds'
    // Away: the builds left behind are let go and their proxies with them; then the tiles.
    const WorldPos away{3216.0, 0.0, 16.0};
    collision.prefetch(away);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    collision.prefetch(away);
    world.update(at(3216.0, 16.0), 1, true);
    CHECK(collision.stats().tile_proxies == 0);
    CHECK(physics.built_shape_count() == 0);
    CHECK(physics.shape_count() == 0);
  }
  CHECK(physics.built_shape_count() == 0);
}

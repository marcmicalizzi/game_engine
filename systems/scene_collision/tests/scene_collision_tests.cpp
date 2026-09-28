// The scene's collision round a walker (scene_collision.h; docs/subsystems/scene_collision.md),
// with no device and no generator capability: a ground provider and a placement generator of the
// test's own, and a scene built in memory from meshes whose DAGs the geometry module builds — so
// every case runs in every configuration that has the capability.

#include <core/containers/flat_map.h>
#include <core/hash/hash.h>
#include <core/json/json_value.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::scene_collision;

namespace {

// ---- a ground of the test's own
// -------------------------------------------------------------------

// A tilted plane with a gentle saddle in it, still or moving: `speed` metres a game second along
// +x for a ripple on top of it, which is what a time-lapse moves.
struct TestGround {
  f32 ripple = 0.0f;  // metres of ripple height; 0 is a still ground
  f32 speed = 0.0f;
  f64 own_time = 0.0;
};

f32 ground_at(const TestGround& g, f32 x, f32 z, f64 t) noexcept {
  const f32 base = 1.0f + 0.05f * x + 0.02f * z + 0.0005f * x * z;
  if (g.ripple == 0.0f) return base;
  const f32 phase = 0.35f * (x - g.speed * static_cast<f32>(t)) + 0.1f * z;
  return base + g.ripple * std::sin(phase);
}

void ground_destroy(void*) noexcept {}
f32 ground_height(const void* s, f32 x, f32 z) noexcept {
  const auto* g = static_cast<const TestGround*>(s);
  return ground_at(*g, x, z, g->own_time);
}
bool ground_evaluate(const void* s, f64 time, const scene_gen::Lattice& lattice, i32 i0, i32 j0,
                     u32 nx, u32 nz, u32, u32, std::span<f32> heights) noexcept {
  const auto* g = static_cast<const TestGround*>(s);
  for (u32 j = 0; j < nz; ++j)
    for (u32 i = 0; i < nx; ++i)
      heights[j * nx + i] = ground_at(*g, lattice.x(i0 + static_cast<i32>(i)),
                                      lattice.z(j0 + static_cast<i32>(j)), time);
  return true;
}
f64 ground_travel(const void* s, f64 from, f64 to) noexcept {
  return std::fabs(static_cast<f64>(static_cast<const TestGround*>(s)->speed) * (to - from));
}
constexpr scene_gen::GroundOps k_still_ops{.destroy = &ground_destroy, .height = &ground_height};
constexpr scene_gen::GroundOps k_moving_ops{.destroy = &ground_destroy,
                                            .height = &ground_height,
                                            .evaluate = &ground_evaluate,
                                            .travel_m = &ground_travel};

// ---- meshes, and a scene of them
// ------------------------------------------------------------------

struct MeshSource {
  Vector<Vec3> positions;
  Vector<u32> indices;
};

// A box on the ground: its base at y = 0, centred in x and z, faces wound outwards.
MeshSource box_mesh(Vec3 half) {
  MeshSource m;
  for (u32 k = 0; k < 8; ++k) {
    m.positions.push_back(Vec3{(k & 1) ? half.x : -half.x, (k & 2) ? 2.0f * half.y : 0.0f,
                               (k & 4) ? half.z : -half.z});
  }
  const u32 faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
  for (const auto& f : faces) {
    const u32 tri[6] = {f[0], f[1], f[2], f[0], f[2], f[3]};
    for (const u32 v : tri)
      m.indices.push_back(v);
  }
  return m;
}

// A ball of radius `r`: an octahedron subdivided `levels` times and pushed out to the sphere, which
// is only a square root a vertex — thousands of triangles, several levels of DAG.
MeshSource ball_mesh(f32 r, u32 levels) {
  MeshSource m;
  m.positions = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  m.indices = {0, 2, 4, 2, 1, 4, 1, 3, 4, 3, 0, 4, 2, 0, 5, 1, 2, 5, 3, 1, 5, 0, 3, 5};
  for (u32 l = 0; l < levels; ++l) {
    Vector<u32> next;
    FlatMap<u64, u32> midpoints;  // one vertex an edge, so the ball stays welded
    for (u32 t = 0; t + 2 < m.indices.size(); t += 3) {
      const u32 a = m.indices[t];
      const u32 b = m.indices[t + 1];
      const u32 c = m.indices[t + 2];
      auto mid = [&](u32 p, u32 q) {
        const u64 key = p < q ? (u64{p} << 32 | q) : (u64{q} << 32 | p);
        if (const u32* seen = midpoints.find_value(key)) return *seen;
        m.positions.push_back(normalize((m.positions[p] + m.positions[q]) * 0.5f));
        midpoints.insert(key, m.positions.size() - 1);
        return m.positions.size() - 1;
      };
      const u32 ab = mid(a, b);
      const u32 bc = mid(b, c);
      const u32 ca = mid(c, a);
      const u32 tris[12] = {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca};
      for (const u32 v : tris)
        next.push_back(v);
    }
    m.indices = std::move(next);
  }
  for (Vec3& p : m.positions)
    p = normalize(p) * r;
  return m;
}

// A scene as `load_scene` would leave it for the consumer: the meshes' DAGs merged with a part
// each, the instances in GPU shape, no terrain mesh.
void build_scene(std::span<const MeshSource> meshes,
                 std::span<const renderer::SceneInstance> placed, renderer::SceneData& scene) {
  Vector<geometry::ClusterLodMesh> lods;
  for (const MeshSource& m : meshes) {
    geometry::ClusterLodMesh lod;
    std::string error;
    REQUIRE_MESSAGE(
        geometry::build_cluster_lod(std::span<const Vec3>(m.positions.data(), m.positions.size()),
                                    std::span<const u32>(m.indices.data(), m.indices.size()),
                                    geometry::ClusterLodOptions{}, lod, &error),
        error);
    lods.push_back(std::move(lod));
  }
  std::string error;
  REQUIRE_MESSAGE(geometry::merge_cluster_meshes(
                      std::span<const geometry::ClusterLodMesh>(lods.data(), lods.size()),
                      scene.lod, scene.parts, &error),
                  error);
  scene.mesh_fit.resize(scene.parts.size(), Mat4::identity());
  scene.terrain_mesh = ~0u;
  u32 first = 0;
  for (const renderer::SceneInstance& i : placed) {
    gfx::InstanceDesc desc;
    REQUIRE(renderer::make_instance(scene, i, first, desc, &error));
    first += scene.parts[i.mesh].cluster_count;
    scene.instances.push_back(desc);
  }
  scene.pair_count = first;
}

world::RingParams ring(u32 budget = 0) {
  world::RingParams p;
  p.tile_size = 32.0f;
  p.ring_count = 1;
  for (f32& r : p.radius)
    r = 0.0f;
  p.radius[0] = 1.5f;
  p.max_activations = budget;
  p.max_deactivations = 2 * budget;
  return p;
}

sim::ObserverSet at(f32 x, f32 z) {
  sim::ObserverSet set;
  set.add(Vec3{x, 0.0f, z}, 1.0f);
  return set;
}

physics::WorldOptions physics_options() {
  physics::WorldOptions o;
  o.max_bodies = 1024;
  o.max_body_pairs = 4096;
  o.max_contact_constraints = 4096;
  return o;
}

// Straight down onto whatever is at (x, z).
bool hit_below(const physics::World& world, f32 x, f32 z, f32& y) {
  physics::RayHit hit;
  if (!world.cast_ray(Vec3{x, 500.0f, z}, Vec3{0.0f, -1000.0f, 0.0f}, hit)) return false;
  y = hit.position.y;
  return true;
}

}  // namespace

TEST_CASE("scene_collision: the ground is a heightfield a tile, on the provider's lattice") {
  TestGround g;
  scene_gen::GroundProvider ground(&k_still_ops, &g);
  renderer::SceneData scene;
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  SceneCollision collision;
  Config config;
  std::string error;
  REQUIRE_MESSAGE(collision.create(physics, scene, &ground, config, &error), error);
  world::World world(ring());
  world.add_consumer(collision.consumer());
  world.update(at(16.0f, 16.0f), 0, true);

  // The 3 x 3 tiles round the observer's own, a heightfield each and nothing else.
  CHECK(world.ring().active_count() == 9);
  CHECK(collision.stats().tiles == 9);
  CHECK(collision.stats().bodies == 9);
  CHECK(collision.stats().placements == 0);
  CHECK(physics.body_count() == 9);
  // 34 x 34 samples a tile: 33 a metre apart cover it, and the backend wants an even count.
  CHECK(collision.stats().ground_samples == 9u * 34u * 34u);
  MESSAGE("a 32 m tile at 1 m: " << collision.stats().ground_bytes / 9
                                 << " bytes of heightfield in the backend, "
                                 << 34u * 34u * sizeof(f32) << " of heights on the CPU");
  CHECK(collision.stats().ground_bytes > 0);

  // On a lattice point the collision is the provider's height, less the backend's quantization;
  // between them the backend's triangulation, which `ground_height` reproduces.
  f32 worst_lattice = 0.0f;
  f32 worst_between = 0.0f;
  for (i32 i = -30; i <= 60; i += 7) {
    for (i32 j = -30; j <= 60; j += 9) {
      const f32 x = static_cast<f32>(i);
      const f32 z = static_cast<f32>(j);
      f32 hit = 0.0f;
      f32 held = 0.0f;
      REQUIRE(hit_below(physics, x, z, hit));
      REQUIRE(collision.ground_height(x, z, held));
      worst_lattice = std::max(worst_lattice, std::fabs(hit - ground_at(g, x, z, 0.0)));
      CHECK(held == doctest::Approx(ground_at(g, x, z, 0.0)).epsilon(1.0e-6));
      const f32 xb = x + 0.37f;
      const f32 zb = z + 0.61f;
      REQUIRE(hit_below(physics, xb, zb, hit));
      REQUIRE(collision.ground_height(xb, zb, held));
      worst_between = std::max(worst_between, std::fabs(hit - held));
    }
  }
  MESSAGE("the ground's collision against the provider: "
          << worst_lattice << " m at worst on the lattice (quantization), " << worst_between
          << " m between the triangulation and the backend's");
  CHECK(worst_lattice < 0.01f);
  CHECK(worst_between < 0.01f);
  f32 unused = 0.0f;
  CHECK_FALSE(collision.ground_height(500.0f, 500.0f, unused));  // no tile held there
}

TEST_CASE("scene_collision: bodies come and go with the walker's ring, within its budget") {
  TestGround g;
  scene_gen::GroundProvider ground(&k_still_ops, &g);
  // A post in every tile the walk below crosses, so a tile is two bodies — the ground and its
  // compound — and the ring holds eighteen at once.
  const MeshSource meshes[] = {box_mesh(Vec3{0.5f, 1.0f, 0.5f})};
  Vector<renderer::SceneInstance> posts;
  for (i32 tx = -3; tx < 70; ++tx) {
    for (i32 tz = -1; tz <= 1; ++tz) {
      renderer::SceneInstance i;
      i.transform.position =
          Vec3{static_cast<f32>(tx) * 32.0f + 16.0f, 0.0f, static_cast<f32>(tz) * 32.0f + 16.0f};
      posts.push_back(i);
    }
  }
  renderer::SceneData scene;
  build_scene(meshes, std::span<const renderer::SceneInstance>(posts.data(), posts.size()), scene);
  physics::World physics;
  // A small world — it never holds more than twenty bodies — so its broadphase has few nodes to
  // spare, as a busy one would. (The ring-change bench, which adds six bodies an update, is what
  // ran a world's broadphase out of nodes before the consumer's `commit` rebuilt it; these walks
  // add fewer at a time and do not, but hold that thousands of changes leave nothing behind.)
  physics::WorldOptions options = physics_options();
  options.max_bodies = 256;
  REQUIRE(physics.init(options) == physics::Status::Ok);
  SceneCollision collision;
  std::string error;
  REQUIRE_MESSAGE(collision.create(physics, scene, &ground, Config{}, &error), error);
  world::World world(ring(2));
  world.add_consumer(collision.consumer());

  // Two tiles an update, the nearest first: the observer's own tile is among the first two.
  world.update(at(16.0f, 16.0f), 0);
  CHECK(collision.stats().tiles == 2);
  f32 y = 0.0f;
  CHECK(collision.ground_height(10.0f, 10.0f, y));
  for (u64 t = 1; t < 6; ++t)
    world.update(at(16.0f, 16.0f), t);
  CHECK(collision.stats().tiles == 9);

  // Two kilometres east and back at a metre an update: tiles are let go behind as they come in
  // ahead, and what the physics world holds is always what the consumer says it holds.
  u32 most = 0;
  f32 x = 16.0f;
  for (u64 t = 6; t < 4006; ++t) {
    x += t < 2006 ? 1.0f : -1.0f;
    world.update(at(x, 16.0f), t);
    most = std::max(most, collision.stats().tiles);
    REQUIRE(physics.body_count() == collision.stats().bodies);
    REQUIRE(collision.stats().tiles == world.ring().active_count());
    // The walker's own tile is always held.
    REQUIRE(collision.ground_height(x, 16.0f, y));
    REQUIRE(y == doctest::Approx(ground_at(g, x, 16.0f, 0.0)).epsilon(1.0e-5));
  }
  const Stats& s = collision.stats();
  MESSAGE("2 km east and back: at most " << most << " tiles held, " << s.activated << " activated, "
                                         << s.deactivated << " let go, " << s.bodies_created
                                         << " bodies made and " << s.bodies_destroyed
                                         << " destroyed");
  CHECK(most <= 12);
  CHECK(s.bodies_created - s.bodies_destroyed == s.bodies);
  CHECK(s.activated > 300);
  CHECK(s.max_bodies <= 24);
  CHECK(s.max_bodies >= 18);
  CHECK(s.failures == 0);

  // And a tile an update with no budget, east for sixty tiles and back, ten times over: three tiles
  // in and three out every update, seven thousand bodies made and let go.
  u64 tick = 4006;
  for (u32 lap = 0; lap < 10; ++lap) {
    for (u32 step = 0; step < 120; ++step) {
      const u32 leg = step < 60 ? step : 119 - step;
      const f32 cx = 16.0f + 32.0f * static_cast<f32>(leg);
      world.update(at(cx, 16.0f), tick++, true);
      REQUIRE(physics.body_count() == collision.stats().bodies);
      REQUIRE(collision.ground_height(cx, 16.0f, y));
    }
  }
  MESSAGE("then " << collision.stats().bodies_created << " bodies made in all, "
                  << collision.stats().bodies_destroyed << " destroyed");
  CHECK(collision.stats().bodies_created > 7000);
  CHECK(collision.stats().failures == 0);
}

TEST_CASE("scene_collision: a scene read whole collides as its instances, a proxy a mesh") {
  const MeshSource meshes[] = {box_mesh(Vec3{2.0f, 1.5f, 0.25f}), ball_mesh(1.0f, 3)};
  Vector<renderer::SceneInstance> placed;
  // A wall of five boxes along z = 20, x from -8 to 8, and three balls.
  for (u32 k = 0; k < 5; ++k) {
    renderer::SceneInstance i;
    i.mesh = 0;
    i.transform.position = Vec3{-8.0f + 4.0f * static_cast<f32>(k), 0.0f, 20.0f};
    placed.push_back(i);
  }
  for (u32 k = 0; k < 3; ++k) {
    renderer::SceneInstance i;
    i.mesh = 1;
    i.transform.position = Vec3{-20.0f + 6.0f * static_cast<f32>(k), 1.0f, -12.0f};
    placed.push_back(i);
  }
  renderer::SceneData scene;
  build_scene(meshes, std::span<const renderer::SceneInstance>(placed.data(), placed.size()),
              scene);
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  // No ground provider: flat ground stands in, as a box.
  physics::ShapeId floor_shape;
  REQUIRE(physics.create_box(Vec3{100.0f, 0.5f, 100.0f}, floor_shape) == physics::Status::Ok);
  physics::BodyDesc floor;
  floor.shape = floor_shape;
  floor.transform.position = Vec3{0.0f, -0.5f, 0.0f};
  floor.motion = physics::MotionType::Static;
  floor.layer = physics::Layer::Static;
  physics::BodyId floor_body;
  REQUIRE(physics.create_body(floor, floor_body) == physics::Status::Ok);

  SceneCollision collision;
  std::string error;
  REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
  CHECK_FALSE(collision.streamed());
  world::World world(ring());
  world.add_consumer(collision.consumer());
  world.update(at(0.0f, 8.0f), 0, true);

  // Every piece within the held tiles, counted once for every tile its bounds reach — the box
  // across the tile edge at x = 0 is in tiles (-1, 0) and (0, 0) — and one proxy a mesh however
  // many instances. Three tiles hold pieces: the wall's two and the balls' one.
  const Stats& s = collision.stats();
  CHECK(s.proxies == 2);
  CHECK(s.placements == 9);
  CHECK(s.bodies == 3);
  MESSAGE("three compounds of nine pieces: " << s.compound_bytes << " bytes; two proxies of "
                                             << s.proxy_triangles << " triangles: " << s.proxy_bytes
                                             << " bytes");
  CHECK(s.compound_bytes > 0);
  CHECK(s.proxy_bytes > 0);
  f32 top = 0.0f;
  REQUIRE(hit_below(physics, 0.3f, 20.0f, top));
  CHECK(top == doctest::Approx(3.0f).epsilon(1.0e-3));  // the wall's top
  REQUIRE(hit_below(physics, -14.0f, -12.0f, top));
  CHECK(top > 1.9f);  // the middle ball's top, at 2 m less its facets

  // And a character walking north into the wall is stopped at it.
  physics::CharacterBody walker;
  physics::CharacterConfig c;
  c.step_hz = 240;
  REQUIRE(walker.create(physics, c, Vec3{0.3f, 0.02f, 12.0f}) == physics::Status::Ok);
  physics::CharacterInput north;
  north.move = Vec3{0.0f, 0.0f, 1.0f};
  for (u32 k = 0; k < 8 * 240; ++k)
    REQUIRE(walker.step(north) == physics::Status::Ok);
  const f32 face = 20.0f - 0.25f;
  MESSAGE("walked north into the wall's face at z " << face << ": stopped at z "
                                                    << walker.state().position.z);
  CHECK(walker.state().position.z < face - c.radius + 0.03f);
  CHECK(walker.state().position.z > face - c.radius - 0.1f);
}

TEST_CASE("scene_collision: a proxy is the coarsest cut of its mesh within the error") {
  const MeshSource meshes[] = {ball_mesh(3.0f, 5)};  // 8,192 triangles
  Vector<renderer::SceneInstance> placed(1);
  placed[0].mesh = 0;
  placed[0].transform.position = Vec3{16.0f, 3.0f, 16.0f};
  renderer::SceneData scene;
  build_scene(meshes, std::span<const renderer::SceneInstance>(placed.data(), placed.size()),
              scene);
  u32 leaves = 0;
  for (u32 c = 0; c < scene.parts[0].leaf_cluster_count; ++c)
    leaves += scene.lod.mesh.clusters[scene.parts[0].first_cluster + c].triangle_count;
  CHECK(leaves == 8192);

  // The same ball at an error of 0 (the leaves) and at 5 cm, each in its own world: rays from
  // outside at the centre hit the two within 5 cm of each other.
  auto build = [&](f32 error_m, physics::World& physics, SceneCollision& collision,
                   world::World& world) {
    REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
    Config config;
    config.mesh_error_m = error_m;
    std::string error;
    REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, config, &error), error);
    REQUIRE(world.configure(ring()));
    world.add_consumer(collision.consumer());
    world.update(at(16.0f, 16.0f), 0, true);
  };
  physics::World fine_physics;
  SceneCollision fine;
  world::World fine_world;
  build(0.0f, fine_physics, fine, fine_world);
  physics::World coarse_physics;
  SceneCollision coarse;
  world::World coarse_world;
  build(0.05f, coarse_physics, coarse, coarse_world);
  MESSAGE("a 3 m ball of " << leaves << " triangles: its proxy at 0 is "
                           << fine.stats().proxy_triangles << " triangles, at 5 cm "
                           << coarse.stats().proxy_triangles);
  CHECK(fine.stats().proxy_triangles == leaves);
  CHECK(coarse.stats().proxy_triangles < leaves / 2);
  CHECK(coarse.stats().proxy_triangles > 32);
  f32 worst = 0.0f;
  const Vec3 centre{16.0f, 3.0f, 16.0f};
  const Vec3 dirs[] = {{1, 0, 0},
                       {0, 1, 0},
                       {0, 0, 1},
                       {-1, 0, 0},
                       {0, 0, -1},
                       {0.6f, 0, 0.8f},
                       {0.48f, 0.6f, 0.64f},
                       {-0.36f, 0.48f, -0.8f}};
  for (const Vec3& d : dirs) {
    physics::RayHit a;
    physics::RayHit b;
    const Vec3 from = centre + d * 10.0f;
    REQUIRE(fine_physics.cast_ray(from, d * -10.0f, a));
    REQUIRE(coarse_physics.cast_ray(from, d * -10.0f, b));
    worst = std::max(worst, std::fabs(a.fraction - b.fraction) * 10.0f);
  }
  MESSAGE("the 5 cm proxy's surface stands at most " << worst << " m off the leaves'");
  CHECK(worst <= 0.05f + 1.0e-3f);
}

namespace {

// A placement generator of the test's own: a 3 m post at the centre of every tile whose x + z is
// even, as one placement of its one resident mesh.
struct PostsState {
  u32 asked = 0;
  u8 last_ring = 0xFF;
};
bool posts_open(const JsonValue&, const scene_gen::Context&, void** state, std::string*) {
  *state = new PostsState();
  return true;
}
void posts_close(void* state) noexcept { delete static_cast<PostsState*>(state); }
bool posts_expand(void*, const scene_gen::Context&, scene_gen::Placements&, std::string*) {
  return true;
}
bool posts_meshes(void*, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
                  std::string*) {
  out.push_back(scene_gen::PlacementMesh{"post.glb", "post", 0});
  return true;
}
bool posts_tile(void* state, scene_gen::TileCoord tile, const scene_gen::Context& context,
                scene_gen::Placements& out, std::string*) {
  auto* s = static_cast<PostsState*>(state);
  ++s->asked;
  s->last_ring = context.ring;
  if (((tile.x + tile.z) & 1) != 0) return true;
  scene_gen::Placement p;
  p.mesh = 0;
  p.transform.position = Vec3{(static_cast<f32>(tile.x) + 0.5f) * context.tile_size, 0.0f,
                              (static_cast<f32>(tile.z) + 0.5f) * context.tile_size};
  out.instances.push_back(p);
  out.things = 1;
  return true;
}
bool posts_occupies(const void*, scene_gen::TileCoord) noexcept { return true; }
constexpr scene_gen::PlacementGeneratorDesc k_posts{.name = "collision_test_posts",
                                                    .open = &posts_open,
                                                    .close = &posts_close,
                                                    .expand = &posts_expand,
                                                    .meshes = &posts_meshes,
                                                    .tile = &posts_tile,
                                                    .occupies = &posts_occupies};
const scene_gen::Registrar k_posts_registrar{k_posts};

}  // namespace

TEST_CASE("scene_collision: a streamed scene collides as each generator's tile") {
  const MeshSource meshes[] = {box_mesh(Vec3{0.5f, 1.5f, 0.5f})};
  renderer::SceneData scene;
  build_scene(meshes, {}, scene);
  renderer::StreamedPlacements entry;
  entry.generator = "collision_test_posts";
  entry.params = JsonValue::object();
  entry.where = "test.json: placements 0";
  entry.meshes.push_back(0);
  scene.streamed.push_back(entry);
  scene.world.enabled = true;
  scene.dynamic = true;

  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  SceneCollision collision;
  std::string error;
  REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
  CHECK(collision.streamed());
  world::World world(ring());
  world.add_consumer(collision.consumer());
  world.update(at(16.0f, 16.0f), 0, true);
  // Of the 3 x 3 tiles round (0, 0), the five whose x + z is even have a post.
  CHECK(collision.stats().placements == 5);
  CHECK(collision.stats().bodies == 5);
  f32 top = 0.0f;
  REQUIRE(hit_below(physics, 16.0f, 16.0f, top));
  CHECK(top == doctest::Approx(3.0f).epsilon(1.0e-3));
  CHECK_FALSE(hit_below(physics, 48.0f, 16.0f, top));  // tile (1, 0): odd, nothing

  // A scene naming a generator this executable does not carry is refused with the registry's
  // sentence.
  renderer::SceneData unknown = scene;
  unknown.streamed[0].generator = "no_such_generator";
  physics::World other;
  REQUIRE(other.init(physics_options()) == physics::Status::Ok);
  SceneCollision refused;
  CHECK_FALSE(refused.create(other, unknown, nullptr, Config{}, &error));
  CHECK(error.find("no_such_generator") != std::string::npos);
}

TEST_CASE("scene_collision: a moving ground stays within its error of the drawn one") {
  // A ripple 0.6 m high travelling at 2 m a game second, drawn as a pair of fields ten game
  // seconds apart crossed by a blend of a twentieth a frame — the renderer's two-fields-and-a-blend
  // model, as plain numbers. After every update the collision heights under the walker are within
  // `ground_error_m` of the drawn ones at every sample, however many refreshes that took.
  TestGround g;
  g.ripple = 0.3f;
  g.speed = 2.0f;
  scene_gen::GroundProvider ground(&k_moving_ops, &g);
  renderer::SceneData scene;
  physics::World physics;
  REQUIRE(physics.init(physics_options()) == physics::Status::Ok);
  SceneCollision collision;
  Config config;
  config.max_refreshes = 2;
  std::string error;
  REQUIRE_MESSAGE(collision.create(physics, scene, &ground, config, &error), error);
  GroundTime time;
  time.moving = true;
  time.time_a = 0.0;
  time.time_b = 10.0;
  time.blend = 0.0;
  collision.set_ground_time(time);
  world::World world(ring());
  world.add_consumer(collision.consumer());
  const f32 wx = 10.0f;
  const f32 wz = 10.0f;
  world.update(at(wx, wz), 0, true);

  f32 worst = 0.0f;
  for (u32 frame = 1; frame <= 200; ++frame) {
    const u32 step = frame % 20;
    if (step == 0) {  // the surface reached b: the next pair, b becoming a
      time.time_a = time.time_b;
      time.time_b = time.time_a + 10.0;
    }
    time.blend = static_cast<f64>(step) / 20.0;
    collision.set_ground_time(time);
    collision.refresh(wx, wz);
    // The walker's tile, sample by sample, against the drawn heights there.
    for (i32 i = 0; i < 32; ++i) {
      for (i32 j = 0; j < 32; ++j) {
        const f32 x = static_cast<f32>(i);
        const f32 z = static_cast<f32>(j);
        f32 held = 0.0f;
        REQUIRE(collision.ground_height(x, z, held));
        const f32 a = ground_at(g, x, z, time.time_a);
        const f32 b = ground_at(g, x, z, time.time_b);
        const f32 t = static_cast<f32>(time.blend);
        worst = std::max(worst, std::fabs(held - (a * (1.0f - t) + b * t)));
      }
    }
  }
  const Stats& s = collision.stats();
  MESSAGE("200 frames of moving ground: "
          << s.refreshes << " tile refreshes, " << s.field_evaluations << " fields evaluated, the "
          << "walker's tile at most " << worst << " m off the drawn heights after an update, the "
          << "most any refresh found " << s.max_stale_m);
  CHECK(worst <= config.ground_error_m + 1.0e-5f);
  CHECK(s.refreshes > 20);
  CHECK(physics.body_count() == s.bodies);

  // The same ground standing still is never refreshed.
  TestGround still;
  scene_gen::GroundProvider still_ground(&k_still_ops, &still);
  physics::World still_physics;
  REQUIRE(still_physics.init(physics_options()) == physics::Status::Ok);
  SceneCollision still_collision;
  REQUIRE(still_collision.create(still_physics, scene, &still_ground, config, &error));
  world::World still_world(ring());
  still_world.add_consumer(still_collision.consumer());
  still_world.update(at(wx, wz), 0, true);
  still_collision.set_ground_time(time);
  CHECK(still_collision.refresh(wx, wz) == 0);
  CHECK(still_collision.stats().refreshes == 0);
}

TEST_CASE("scene_collision: the tunables' defaults are the documented ones") {
  CHECK(spacing_tunable() == 1.0f);
  CHECK(radius_tunable() == 1.5f);
  CHECK(max_tiles_tunable() == 2);
  CHECK(mesh_error_tunable() == 0.05f);
  CHECK(ground_error_tunable() == 0.05f);
  CHECK(max_refreshes_tunable() == 2);
  const world::RingParams p = ring_params_from_tunables(32.0f);
  CHECK(p.ring_count == 1);
  CHECK(p.radius[0] == 1.5f);
  CHECK(p.max_activations == 2);
  CHECK(p.max_deactivations == 4);
  CHECK(world::valid_ring_params(p));
  CHECK(std::string(k_determinism) == "hashed");
}

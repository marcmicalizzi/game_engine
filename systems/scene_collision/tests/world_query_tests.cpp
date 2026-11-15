// What a game module can ask the world, the collision's half (scene_collision.h, "what a module can
// ask"; docs/subsystems/scene_collision.md, "What a module can ask"; roadmap A43): which piece a
// cast met, and the supports near a point — a tree's limb, a perch an imported mesh declares, a
// rock's top — over a scene built in memory with no device and no generator, as the other cases in
// this suite build theirs. And the answers pinned, so every toolchain gives the same ones.

#include <core/containers/flat_map.h>
#include <core/hash/hash.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/physics/physics.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::scene_collision;

namespace {

struct MeshSource {
  Vector<Vec3> positions;
  Vector<u32> indices;
};

// A box from `lo` to `hi`, faces wound outwards, appended to `m`.
void add_box(MeshSource& m, Vec3 lo, Vec3 hi) {
  const u32 base = m.positions.size();
  for (u32 k = 0; k < 8; ++k)
    m.positions.push_back(
        Vec3{(k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y, (k & 4) ? hi.z : lo.z});
  const u32 faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
  for (const auto& f : faces) {
    const u32 tri[6] = {f[0], f[1], f[2], f[0], f[2], f[3]};
    for (const u32 v : tri)
      m.indices.push_back(base + v);
  }
}

// A ball of radius `r` centred `y` up: an octahedron subdivided twice and pushed out to the sphere.
MeshSource ball(f32 r, f32 y) {
  MeshSource m;
  m.positions = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  m.indices = {0, 2, 4, 2, 1, 4, 1, 3, 4, 3, 0, 4, 2, 0, 5, 1, 2, 5, 3, 1, 5, 0, 3, 5};
  for (u32 l = 0; l < 2; ++l) {
    Vector<u32> next;
    FlatMap<u64, u32> mid_of;
    for (u32 t = 0; t + 2 < m.indices.size(); t += 3) {
      const u32 a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
      auto mid = [&](u32 p, u32 q) {
        const u64 key = p < q ? (u64{p} << 32 | q) : (u64{q} << 32 | p);
        if (const u32* seen = mid_of.find_value(key)) return *seen;
        m.positions.push_back(normalize((m.positions[p] + m.positions[q]) * 0.5f));
        mid_of.insert(key, m.positions.size() - 1);
        return m.positions.size() - 1;
      };
      const u32 ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
      const u32 tris[12] = {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca};
      for (const u32 v : tris)
        next.push_back(v);
    }
    m.indices = std::move(next);
  }
  for (Vec3& p : m.positions)
    p = normalize(p) * r + Vec3{0.0f, y, 0.0f};
  return m;
}

// The test's world: three meshes on a floor —
//   0 a tree at (0, 0, 20): a trunk 0.4 m square and 4 m tall (part 0), a limb 0.2 m square along
//     +x from the trunk at 2.5 m up to x = 2 (part 1), and a crown's leaf slab 2 m across from 3
//     to 3.5 m up (part 2, a frond: no body);
//   1 a post at (6, 0, 16): 0.3 m square and 1.2 m tall, declaring one perch on its top centre,
//     0.04 m;
//   2 a boulder at (-6, 0, 16): a ball of 0.8 m, its centre 0.6 m up.
struct World {
  renderer::SceneData scene;
  physics::World physics;
  SceneCollision collision;
  world::World ring;
};

void build(World& w) {
  MeshSource tree;
  add_box(tree, Vec3{-0.2f, 0.0f, -0.2f}, Vec3{0.2f, 4.0f, 0.2f});
  add_box(tree, Vec3{0.2f, 2.4f, -0.1f}, Vec3{2.0f, 2.6f, 0.1f});
  add_box(tree, Vec3{-1.0f, 3.0f, -1.0f}, Vec3{1.0f, 3.5f, 1.0f});
  MeshSource post;
  add_box(post, Vec3{-0.15f, 0.0f, -0.15f}, Vec3{0.15f, 1.2f, 0.15f});
  const MeshSource boulder = ball(0.8f, 0.6f);
  const MeshSource sources[] = {tree, post, boulder};
  Vector<geometry::ClusterLodMesh> lods;
  for (const MeshSource& m : sources) {
    geometry::ClusterLodMesh lod;
    std::string error;
    REQUIRE_MESSAGE(
        geometry::build_cluster_lod(std::span<const Vec3>(m.positions.data(), m.positions.size()),
                                    std::span<const u32>(m.indices.data(), m.indices.size()),
                                    geometry::ClusterLodOptions{}, lod, &error),
        error);
    lods.push_back(std::move(lod));
  }
  renderer::SceneData& scene = w.scene;
  std::string error;
  REQUIRE_MESSAGE(geometry::merge_cluster_meshes(
                      std::span<const geometry::ClusterLodMesh>(lods.data(), lods.size()),
                      scene.lod, scene.parts, &error),
                  error);
  scene.mesh_fit.resize(scene.parts.size(), Mat4::identity());
  scene.terrain_mesh = ~0u;
  scene.sources.resize(scene.parts.size());
  // The tree's motion contract: three parts, the structural table naming all three with a radius
  // (the leaf slab's is 0, so it is no support), the trunk and the limb load-bearing.
  renderer::SourceMesh& t = scene.sources[0];
  t.species = "broadleaf";
  geometry::ClusterFileMotionPart trunk;
  trunk.pivot = Vec3{0.0f, 0.0f, 0.0f};
  trunk.direction = Vec3{0.0f, 1.0f, 0.0f};
  trunk.length = 4.0f;
  geometry::ClusterFileMotionPart limb;
  limb.pivot = Vec3{0.2f, 2.5f, 0.0f};
  limb.direction = Vec3{1.0f, 0.0f, 0.0f};
  limb.length = 1.8f;
  limb.parent = 0;
  geometry::ClusterFileMotionPart leaves;
  leaves.pivot = Vec3{0.0f, 3.0f, 0.0f};
  leaves.direction = Vec3{0.0f, 1.0f, 0.0f};
  leaves.length = 0.5f;
  leaves.parent = 0;
  t.motion_parts.push_back(trunk);
  t.motion_parts.push_back(limb);
  t.motion_parts.push_back(leaves);
  t.structural_parts.push_back(geometry::ClusterFileStructuralPart{
      0, 0.2f, 0.2f, geometry::k_structural_load_bearing | geometry::k_structural_straight});
  t.structural_parts.push_back(geometry::ClusterFileStructuralPart{
      1, 0.1f, 0.1f, geometry::k_structural_load_bearing | geometry::k_structural_straight});
  t.structural_parts.push_back(geometry::ClusterFileStructuralPart{2, 0.0f, 0.0f, 0u});
  const geometry::ClusterMeshPart& part = scene.parts[0];
  const u32 next_first =
      scene.parts.size() > 1 ? scene.parts[1].first_vertex : scene.lod.mesh.vertices.size();
  for (u32 v = part.first_vertex; v < next_first; ++v) {
    const Vec3 p = scene.lod.mesh.vertices[v];
    geometry::ClusterFileMotionVertex record;
    // The slab's corners stand a metre out; the trunk's top four stay the trunk's.
    record.part = p.y >= 2.95f && std::fabs(p.x) > 0.5f ? 2 : (p.x > 0.25f ? 1 : 0);
    t.motion_vertices.push_back(record);
  }
  REQUIRE(t.has_motion());
  // The post's perch: on its top centre, up +y, a 4 cm foot.
  geometry::ClusterFilePerch perch;
  perch.position = Vec3{0.0f, 1.2f, 0.0f};
  perch.up = Vec3{0.0f, 1.0f, 0.0f};
  perch.size_m = 0.04f;
  scene.sources[1].perches.push_back(perch);

  const Vec3 at[3] = {{0.0f, 0.0f, 20.0f}, {6.0f, 0.0f, 16.0f}, {-6.0f, 0.0f, 16.0f}};
  u32 first = 0;
  for (u32 k = 0; k < 3; ++k) {
    renderer::SceneInstance i;
    i.mesh = k;
    i.transform.position = at[k];
    gfx::InstanceDesc desc;
    REQUIRE(renderer::make_instance(scene, i, first, desc, &error));
    first += scene.parts[k].cluster_count;
    scene.instances.push_back(desc);
  }
  scene.pair_count = first;

  physics::WorldOptions o;
  o.max_bodies = 1024;
  o.max_body_pairs = 4096;
  o.max_contact_constraints = 4096;
  REQUIRE(w.physics.init(o) == physics::Status::Ok);
  physics::ShapeId floor_shape;
  REQUIRE(w.physics.create_box(Vec3{100.0f, 0.5f, 100.0f}, floor_shape) == physics::Status::Ok);
  physics::BodyDesc floor;
  floor.shape = floor_shape;
  floor.transform.position = WorldPos{0.0, -0.5, 0.0};
  floor.motion = physics::MotionType::Static;
  floor.layer = physics::Layer::Static;
  physics::BodyId floor_body;
  REQUIRE(w.physics.create_body(floor, floor_body) == physics::Status::Ok);
  REQUIRE_MESSAGE(w.collision.create(w.physics, scene, nullptr, Config{}, &error), error);
  world::RingParams p;
  p.tile_size = 32.0f;
  p.ring_count = 1;
  for (f32& r : p.radius)
    r = 0.0f;
  p.radius[0] = 1.5f;
  const char* ring_error = nullptr;
  REQUIRE(w.ring.configure(p, &ring_error));
  w.ring.add_consumer(w.collision.consumer());
  sim::ObserverSet observers;
  observers.add(WorldPos{0.0, 0.0, 16.0}, 1.0f);
  w.ring.update(observers, 0, true);
}

// A cast as a module's would be: the collision's world and its word for what was met.
struct Cast {
  bool hit = false;
  WorldPos position{};
  Vec3 normal{};
  f32 distance = 0.0f;
  HitInfo info;
};
Cast ray(const World& w, WorldPos from, Vec3 direction, f32 length) {
  Cast c;
  physics::RayHit hit;
  const physics::LayerMask statics = physics::LayerMask::of(physics::Layer::Static);
  if (!w.physics.cast_ray(from, direction * length, hit, statics)) return c;
  c.hit = true;
  c.position = hit.position;
  // Facing the cast, as the walker's cast hands a module (walk.cpp, `Walker::cast`): the boxes
  // here are wound inward, so the triangle's own normal is the other way.
  c.normal = dot(hit.normal, direction) > 0.0f ? hit.normal * -1.0f : hit.normal;
  c.distance = hit.fraction * length;
  c.info = w.collision.describe_hit(hit.body, hit.child);
  return c;
}
Cast sphere(const World& w, WorldPos from, Vec3 direction, f32 length, f32 radius) {
  Cast c;
  physics::ShapeHit hit;
  const physics::LayerMask statics = physics::LayerMask::of(physics::Layer::Static);
  if (!w.physics.cast_sphere(from, radius, direction * length, hit, statics)) return c;
  c.hit = true;
  c.distance = hit.fraction * length;
  c.position = from + DVec3{direction * c.distance};
  c.normal = hit.normal;
  c.info = w.collision.describe_hit(hit.body, hit.child);
  return c;
}

// Every bit of an answer, into one hash: what the pins below hold.
u64 bits(u64 h, f64 v) {
  u64 b = 0;
  std::memcpy(&b, &v, sizeof b);
  return hash_combine(h, b);
}
u64 bits(u64 h, f32 v) {
  u32 b = 0;
  std::memcpy(&b, &v, sizeof b);
  return hash_combine(h, b);
}

}  // namespace

TEST_CASE("scene_collision: a cast stops at a trunk, passes the leaves, and names what it met") {
  World w;
  build(w);
  // Level with the limb's underside, from the west: the trunk's west face at x = -0.2.
  const Cast trunk = ray(w, WorldPos{-4.0, 1.0, 20.0}, Vec3{1.0f, 0.0f, 0.0f}, 8.0f);
  REQUIRE(trunk.hit);
  CHECK(trunk.distance == doctest::Approx(3.8f).epsilon(1.0e-4));
  CHECK(trunk.info.kind == HitKind::piece);
  CHECK(trunk.info.instance == 0u);
  CHECK(trunk.info.mesh == 0u);
  CHECK(trunk.normal.x < -0.99f);
  // Straight down through the crown's leaf slab, clear of the trunk and the limb: the floor.
  const Cast crown = ray(w, WorldPos{-0.6, 10.0, 20.6}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f);
  REQUIRE(crown.hit);
  CHECK(crown.distance == doctest::Approx(10.0f).epsilon(1.0e-4));
  CHECK(crown.info.kind == HitKind::none);  // the floor is the test's, no tile's
  // Down onto the limb: it stops there, on the limb's top at 2.6 m.
  const Cast limb = ray(w, WorldPos{1.5, 10.0, 20.0}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f);
  REQUIRE(limb.hit);
  CHECK(limb.position.y == doctest::Approx(2.6).epsilon(1.0e-4));
  CHECK(limb.info.instance == 0u);
  // The post and the boulder are their own instances in the compound.
  const Cast post = ray(w, WorldPos{6.0, 5.0, 16.0}, Vec3{0.0f, -1.0f, 0.0f}, 10.0f);
  REQUIRE(post.hit);
  CHECK(post.info.instance == 1u);
  CHECK(post.position.y == doctest::Approx(1.2).epsilon(1.0e-4));
  // A sphere of 0.25 m rolled east at the boulder: its centre stops a radius short of the ball.
  const Cast ball = sphere(w, WorldPos{-10.0, 0.6, 16.0}, Vec3{1.0f, 0.0f, 0.0f}, 8.0f, 0.25f);
  REQUIRE(ball.hit);
  CHECK(ball.info.instance == 2u);
  CHECK(ball.position.x > -6.0 - 0.8 - 0.25 - 0.05);
  CHECK(ball.position.x < -6.0 - 0.8 - 0.25 + 0.08);  // the ball's facets lie inside the sphere
  // Coverage: the ring holds the tiles round (0, 16); a cast off past them is not covered.
  CHECK(w.collision.covers_segment(WorldPos{-4.0, 1.0, 20.0}, WorldPos{4.0, 1.0, 20.0}));
  CHECK_FALSE(w.collision.covers_segment(WorldPos{0.0, 1.0, 16.0}, WorldPos{200.0, 1.0, 16.0}));
}

TEST_CASE("scene_collision: the supports near a point are a limb, a perch and a rock's top") {
  World w;
  build(w);
  Support out[16];
  // Near the tree's limb, a 3 cm foot: the limb's top above the point of its axis nearest the
  // asker, and nothing of the trunk (vertical) or the leaves (no radius).
  SupportAnswer a = w.collision.supports_near(WorldPos{1.2, 3.2, 20.3}, 1.5f, 0.03f, out);
  REQUIRE(a.count >= 1u);
  CHECK(a.covered);
  CHECK(out[0].kind == k_support_limb);
  CHECK(out[0].part == 1u);
  CHECK(out[0].piece.instance == 0u);
  CHECK(out[0].position.x == doctest::Approx(1.2).epsilon(1.0e-4));
  CHECK(out[0].position.y == doctest::Approx(2.6).epsilon(1.0e-4));
  CHECK(out[0].position.z == doctest::Approx(20.0).epsilon(1.0e-4));
  CHECK(out[0].up.y == doctest::Approx(1.0f).epsilon(1.0e-4));
  CHECK(out[0].size_m == doctest::Approx(0.2f).epsilon(1.0e-4));
  for (u32 i = 0; i < a.count; ++i)
    CHECK(out[i].part != 0u);  // the trunk rises steeper than 60 degrees
  // A foot larger than the limb: none.
  a = w.collision.supports_near(WorldPos{1.2, 3.2, 20.3}, 1.5f, 0.5f, out);
  CHECK(a.count == 0u);
  // Near the post: its declared perch and nothing else (a perch wins over the top).
  a = w.collision.supports_near(WorldPos{6.2, 1.5, 16.1}, 1.0f, 0.0f, out);
  REQUIRE(a.count == 1u);
  CHECK(out[0].kind == k_support_perch);
  CHECK(out[0].part == 0u);
  CHECK(out[0].piece.instance == 1u);
  CHECK(out[0].position.x == doctest::Approx(6.0).epsilon(1.0e-5));
  CHECK(out[0].position.y == doctest::Approx(1.2).epsilon(1.0e-5));
  CHECK(out[0].size_m == doctest::Approx(0.04f).epsilon(1.0e-4));
  // Near the boulder: its top, found on its surface, facing up.
  a = w.collision.supports_near(WorldPos{-6.0, 2.0, 16.0}, 1.0f, 0.0f, out);
  REQUIRE(a.count == 1u);
  CHECK(out[0].kind == k_support_top);
  CHECK(out[0].piece.instance == 2u);
  // The proxy is the ball's cut within 5 cm (`mesh_error_m`): its top no more than that below
  // 1.4 m, and a facet there tilted at most the half-angle a 5 cm sagitta spans on a 0.8 m ball,
  // acos(1 - 0.05 / 0.8) = 20.4 degrees, whose cosine is 0.937.
  CHECK(out[0].position.y >= 1.35 - 1.0e-4);
  CHECK(out[0].position.y <= 1.4 + 1.0e-4);
  CHECK(out[0].up.y >= 0.937f);
  // All three from one point in the middle, nearest first, and `found` counting past `capacity`.
  a = w.collision.supports_near(WorldPos{0.0, 1.0, 17.0}, 10.0f, 0.0f, std::span<Support>(out, 2));
  CHECK(a.found == 3u);
  CHECK(a.count == 2u);
  CHECK(out[0].distance <= out[1].distance);
}

TEST_CASE("scene_collision: a module's answers are pinned, the same from every toolchain") {
  World w;
  build(w);
  u64 h = 0;
  const auto cast_into = [&](const Cast& c) {
    h = hash_combine(h, c.hit ? 1u : 0u);
    h = bits(h, c.position.x);
    h = bits(h, c.position.y);
    h = bits(h, c.position.z);
    h = bits(h, c.normal.x);
    h = bits(h, c.normal.y);
    h = bits(h, c.normal.z);
    h = bits(h, c.distance);
    h = hash_combine(h, static_cast<u64>(c.info.kind));
    h = hash_combine(h, c.info.instance);
  };
  cast_into(ray(w, WorldPos{-4.0, 1.0, 20.0}, Vec3{1.0f, 0.0f, 0.0f}, 8.0f));
  cast_into(ray(w, WorldPos{1.5, 10.0, 20.0}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f));
  cast_into(ray(w, WorldPos{-9.0, 3.0, 13.0}, normalize(Vec3{1.0f, -0.3f, 1.0f}), 12.0f));
  cast_into(sphere(w, WorldPos{-10.0, 0.6, 16.0}, Vec3{1.0f, 0.0f, 0.0f}, 8.0f, 0.25f));
  cast_into(sphere(w, WorldPos{3.0, 4.0, 17.0}, normalize(Vec3{-1.0f, -0.4f, 1.0f}), 6.0f, 0.1f));
  Support out[8];
  const WorldPos asks[3] = {{1.2, 3.2, 20.3}, {6.2, 1.5, 16.1}, {0.0, 1.0, 17.0}};
  for (const WorldPos& at : asks) {
    const SupportAnswer a = w.collision.supports_near(at, 10.0f, 0.0f, out);
    h = hash_combine(h, a.count);
    for (u32 i = 0; i < a.count; ++i) {
      h = bits(h, out[i].position.x);
      h = bits(h, out[i].position.y);
      h = bits(h, out[i].position.z);
      h = bits(h, out[i].up.x);
      h = bits(h, out[i].up.y);
      h = bits(h, out[i].up.z);
      h = bits(h, out[i].size_m);
      h = hash_combine(h, out[i].kind);
      h = hash_combine(h, out[i].part);
    }
  }
  char text[32];
  std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(h));
  MESSAGE("the answers' hash: " << text);
  // Taken on MSVC at x86-64-v3 (2026-11-14); GCC and Clang in the Linux containers reproduce it.
  CHECK(std::string(text) == "df949c8f5fc626b3");

  // And the cost, for the page: a thousand casts and a thousand supports queries in one step.
  const auto t0 = std::chrono::steady_clock::now();
  u32 hits = 0;
  for (u32 i = 0; i < 1000; ++i) {
    const f64 x = -8.0 + 0.016 * static_cast<f64>(i);
    hits += ray(w, WorldPos{x, 6.0, 18.0}, normalize(Vec3{0.2f, -1.0f, 0.3f}), 12.0f).hit ? 1u : 0u;
  }
  const auto t1 = std::chrono::steady_clock::now();
  u32 found = 0;
  for (u32 i = 0; i < 1000; ++i) {
    const f64 x = -8.0 + 0.016 * static_cast<f64>(i);
    found += w.collision.supports_near(WorldPos{x, 1.5, 17.0}, 4.0f, 0.02f, out).count;
  }
  const auto t2 = std::chrono::steady_clock::now();
  const f64 cast_us = std::chrono::duration<f64, std::micro>(t1 - t0).count();
  const f64 support_us = std::chrono::duration<f64, std::micro>(t2 - t1).count();
  MESSAGE("1000 rays: " << cast_us << " us (" << hits << " hits); 1000 supports queries: "
                        << support_us << " us (" << found << " supports)");
  CHECK(hits > 0u);
}

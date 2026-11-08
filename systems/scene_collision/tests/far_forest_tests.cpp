// The invisible wall of the owner's v35 walk (2026-11-07; docs/subsystems/scene_collision.md, "What
// gets no body"; roadmap P62): a far-forest tile's mesh — every streamed tree's coarsest form on a
// world tile merged into one rigid instance of the load's prefix, an impostor star of cards a crown
// wide through each trunk — is drawn only where the ring's tail does not draw the tile's trees, so
// never round a walker; binned as a prefix instance (W88) it stopped the walker in open ground,
// 1.55 m from a birch's trunk and 3 m from a pine's. No device, no generator capability.

#include <domain/geometry/cluster_lod.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <systems/renderer/far_forest.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::scene_collision;

namespace {

struct MeshSource {
  Vector<Vec3> positions;
  Vector<u32> indices;
};

// A box standing on y = 0 round (cx, cz), faces wound outwards.
void add_box(MeshSource& m, Vec3 half, f32 cx, f32 cz, f32 y0 = 0.0f) {
  const u32 base = m.positions.size();
  for (u32 k = 0; k < 8; ++k) {
    m.positions.push_back(Vec3{cx + ((k & 1) ? half.x : -half.x),
                               y0 + ((k & 2) ? 2.0f * half.y : 0.0f),
                               cz + ((k & 4) ? half.z : -half.z)});
  }
  const u32 faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
  for (const auto& f : faces) {
    const u32 tri[6] = {f[0], f[1], f[2], f[0], f[2], f[3]};
    for (const u32 v : tri)
      m.indices.push_back(base + v);
  }
}

void build_scene(std::span<const MeshSource> meshes, renderer::SceneData& scene) {
  Vector<geometry::ClusterLodMesh> lods;
  std::string error;
  for (const MeshSource& m : meshes) {
    geometry::ClusterLodMesh lod;
    REQUIRE_MESSAGE(
        geometry::build_cluster_lod(std::span<const Vec3>(m.positions.data(), m.positions.size()),
                                    std::span<const u32>(m.indices.data(), m.indices.size()),
                                    geometry::ClusterLodOptions{}, lod, &error),
        error);
    lods.push_back(std::move(lod));
  }
  REQUIRE_MESSAGE(geometry::merge_cluster_meshes(
                      std::span<const geometry::ClusterLodMesh>(lods.data(), lods.size()),
                      scene.lod, scene.parts, &error),
                  error);
  scene.mesh_fit.resize(scene.parts.size(), Mat4::identity());
  scene.terrain_mesh = ~0u;
  u32 first = 0;
  for (u32 mesh = 0; mesh < meshes.size(); ++mesh) {
    renderer::SceneInstance placed;  // every mesh once, at the origin
    placed.mesh = mesh;
    gfx::InstanceDesc desc;
    REQUIRE(renderer::make_instance(scene, placed, first, desc, &error));
    first += scene.parts[mesh].cluster_count;
    scene.instances.push_back(desc);
  }
  scene.pair_count = first;
}

WorldPos place(Vec3 local) { return absolute(WorldPos::origin(), local); }

struct Walk {
  f32 stopped_z = 0.0f;
  u32 far_left_out = 0;
  u32 pieces = 0;
};

// A walker from z = 12 going north for eight seconds towards a trunk whose face is at z = 19.8.
Walk walk_north(const renderer::SceneData& scene) {
  physics::World physics;
  physics::WorldOptions o;
  o.max_bodies = 1024;
  o.max_body_pairs = 4096;
  o.max_contact_constraints = 4096;
  REQUIRE(physics.init(o) == physics::Status::Ok);
  physics::ShapeId floor_shape;
  REQUIRE(physics.create_box(Vec3{100.0f, 0.5f, 100.0f}, floor_shape) == physics::Status::Ok);
  physics::BodyDesc floor;
  floor.shape = floor_shape;
  floor.transform.position = place(Vec3{0.0f, -0.5f, 0.0f});
  floor.motion = physics::MotionType::Static;
  floor.layer = physics::Layer::Static;
  physics::BodyId floor_body;
  REQUIRE(physics.create_body(floor, floor_body) == physics::Status::Ok);
  SceneCollision collision;
  std::string error;
  REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, Config{}, &error), error);
  world::RingParams ring;
  ring.tile_size = 32.0f;
  ring.ring_count = 1;
  for (f32& r : ring.radius)
    r = 0.0f;
  ring.radius[0] = 1.5f;
  world::World world(ring);
  world.add_consumer(collision.consumer());
  sim::ObserverSet observers;
  observers.add(place(Vec3{0.0f, 0.0f, 16.0f}), 1.0f);
  world.update(observers, 0, true);
  physics::CharacterBody walker;
  physics::CharacterConfig c;
  c.step_hz = 240;
  REQUIRE(walker.create(physics, c, place(Vec3{0.0f, 0.02f, 12.0f})) == physics::Status::Ok);
  physics::CharacterInput north;
  north.move = Vec3{0.0f, 0.0f, 1.0f};
  for (u32 k = 0; k < 8 * 240; ++k)
    REQUIRE(walker.step(north) == physics::Status::Ok);
  Walk out;
  out.stopped_z = relative(walker.state().position, WorldPos::origin()).z;
  out.far_left_out = collision.stats().far_forest_left_out;
  out.pieces = collision.stats().placements;
  return out;
}

}  // namespace

TEST_CASE(
    "scene_collision: a far-forest tile is never a wall; the tree's own trunk stops a walker") {
  // Mesh 0: the tree as the ring's tail draws it near, a trunk 0.4 m square and 6 m tall at z = 20.
  // Mesh 1: the far forest's tile mesh over the same tree, its coarsest form — an impostor star of
  // two crossed cards 3 m wide (1.5 m out from the trunk's axis, as the owner's walker was held
  // 1.55 m from the birch's) and 6 m tall, two centimetres thick.
  MeshSource tree;
  add_box(tree, Vec3{0.2f, 3.0f, 0.2f}, 0.0f, 20.0f);
  MeshSource star;
  add_box(star, Vec3{1.5f, 3.0f, 0.01f}, 0.0f, 20.0f);
  add_box(star, Vec3{0.01f, 3.0f, 1.5f}, 0.0f, 20.0f);
  const MeshSource meshes[] = {tree, star};
  const f32 trunk_face = 20.0f - 0.2f;
  const f32 card_face = 20.0f - 1.5f;
  const f32 radius = physics::CharacterConfig{}.radius;

  // Before: the star binned as any prefix instance is a wall 1.5 m out from the trunk.
  renderer::SceneData plain;
  build_scene(meshes, plain);
  const Walk before = walk_north(plain);
  MESSAGE("the far tile binned: stopped at z " << before.stopped_z << " (the card's face "
                                               << card_face << ")");
  CHECK(before.far_left_out == 0);
  CHECK(before.stopped_z < card_face - radius + 0.03f);

  // After: the same scene whose load built the star as a far tile — `SceneData::far_forest` names
  // its mesh and its prefix instance — leaves it out, and the walker reaches the trunk's own face.
  renderer::SceneData scene;
  build_scene(meshes, scene);
  renderer::FarForestTile far;
  far.key = renderer::far_tile_key(0, 0);
  far.mesh = 1;
  far.instance = 1;
  far.trees = 1;
  scene.far_forest.tiles.push_back(far);
  scene.far_forest.tile_size = 32.0f;
  const Walk after = walk_north(scene);
  MESSAGE("the far tile left out: stopped at z " << after.stopped_z << " (the trunk's face "
                                                 << trunk_face << ")");
  CHECK(after.far_left_out == 1);
  CHECK(after.pieces < before.pieces);  // the star was a piece of each tile it reaches
  CHECK(after.stopped_z > card_face);   // past where the star stopped it, within 1.5 m of the trunk
  CHECK(after.stopped_z < trunk_face - radius + 0.03f);
  CHECK(after.stopped_z > trunk_face - radius - 0.1f);
}

// The invisible wall of the owner's v35 walk (2026-11-07; docs/subsystems/scene_collision.md, "What
// gets no body"; roadmap P62): a far-forest tile's mesh — every streamed tree's coarsest form on a
// world tile merged into one rigid instance of the load's prefix, an impostor star of cards a crown
// wide through each trunk — is drawn only where the ring's tail does not draw the tile's trees, so
// never round a walker; binned as a prefix instance (W88) it stopped the walker in open ground,
// 1.55 m from a birch's trunk and 3 m from a pine's. No device, no generator capability.

#include <core/memory/memory.h>
#include <core/time/time.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/physics/character.h>
#include <domain/physics/physics.h>
#include <systems/renderer/far_forest.h>
#include <systems/renderer/scene.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
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

// ---- A game's props stop a walker (roadmap P45; docs/subsystems/scene_collision.md, "Props") ----
// A prop a host hands (`SceneCollision::set_props`) is a body of its mesh's proxy, of the box its
// scene declares, or of nothing, while the tile it stands in is held; it goes with the tile and
// with its removal, and is moved when it moves. The scene is the case's own: one mesh the load
// placed nowhere, a crate a metre on a side, declared as a prop.

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

// **What props cost the walker's tick** (scene_collision.md, "What a moving prop costs"): `n`
// crates standing on the held tiles round a standing walker, the first `k` moved a centimetre a
// tick as a game module moves its animals, `ticks` ticks of what the host does at 240 Hz timed one
// at a time — the ring's update, the props' list built and handed when one moved, `update_props`,
// the walker's step — with the engine allocations the tick made on this thread and the prop bodies
// it made, after a warm-up the scratch grows in.
struct TickCost {
  f64 median_us = 0.0;
  f64 p95_us = 0.0;
  f64 max_us = 0.0;
  u64 allocations = 0;
  u64 bodies_made = 0;
  u32 bodies = 0;
};

TickCost prop_tick_cost(const renderer::SceneData& scene, u32 n, u32 k, renderer::PropShape shape,
                        u32 ticks) {
  Rig rig(scene);
  Vector<PropBody> props;
  Vector<Vec3> at;
  for (u32 i = 0; i < n; ++i) {
    at.push_back(Vec3{-30.0f + 3.0f * static_cast<f32>(i % 20), 0.0f,
                      8.0f - 2.0f * static_cast<f32>(i / 20)});
    PropBody p = crate_at(0.0f, i + 1);
    p.origin = place(at.back());
    p.collider.shape = shape;
    p.collider.half_extents = Vec3{0.5f, 0.5f, 0.5f};
    p.collider.center = Vec3{0.0f, 0.5f, 0.0f};
    props.push_back(p);
  }
  rig.collision.set_props({props.data(), props.size()});
  (void)rig.collision.update_props();
  Vector<PropBody> handed;
  handed.reserve(n);
  Vector<f64> us;
  us.reserve(ticks);
  physics::CharacterInput still{};
  constexpr u32 k_warm = 20;
  TickCost out;
  u64 made = 0;
  bool stepped = true;
  for (u32 t = 0; t < k_warm + ticks; ++t) {
    if (t == k_warm) made = rig.collision.stats().prop_bodies_created;
    const u64 a0 = mem::allocations_on_thread();
    const i64 t0 = time::monotonic_ns();
    rig.world->update(rig.observers, rig.tick++, true);  // the ring, which does not move
    if (k > 0) {  // the host's hand-over (walk.cpp): the list again when a prop moved
      for (u32 i = 0; i < k; ++i)
        props[i].origin = place(at[i] + Vec3{0.01f * static_cast<f32>(t % 100), 0.0f, 0.0f});
      handed.clear();
      for (const PropBody& p : props)
        handed.push_back(p);
      rig.collision.set_props({handed.data(), handed.size()});
    }
    (void)rig.collision.update_props();
    stepped = rig.walker.step(still) == physics::Status::Ok && stepped;
    const i64 t1 = time::monotonic_ns();
    const u64 a1 = mem::allocations_on_thread();
    if (t < k_warm) continue;
    us.push_back(static_cast<f64>(t1 - t0) / 1000.0);
    out.allocations += a1 - a0;
  }
  REQUIRE(stepped);
  out.bodies_made = rig.collision.stats().prop_bodies_created - made;
  out.bodies = rig.collision.stats().prop_bodies;
  std::sort(us.begin(), us.end());
  out.median_us = us[us.size() / 2];
  out.p95_us = us[(us.size() * 95) / 100];
  out.max_us = us.back();
  return out;
}

}  // namespace

TEST_CASE(
    "scene_collision: a prop moved every tick costs the walker's tick a move, not a rebuild") {
  renderer::SceneData scene;
  crate_scene(scene);
  struct Row {
    u32 n;
    u32 k;
    renderer::PropShape shape;
    const char* name;
  };
  const Row rows[] = {
      {30, 0, renderer::PropShape::mesh, "mesh"},  {30, 1, renderer::PropShape::mesh, "mesh"},
      {30, 30, renderer::PropShape::mesh, "mesh"}, {300, 0, renderer::PropShape::mesh, "mesh"},
      {300, 1, renderer::PropShape::mesh, "mesh"}, {300, 30, renderer::PropShape::mesh, "mesh"},
      {300, 30, renderer::PropShape::box, "box"},  {300, 30, renderer::PropShape::none, "none"},
  };
  for (const Row& row : rows) {
    const TickCost c = prop_tick_cost(scene, row.n, row.k, row.shape, 300);
    MESSAGE("props " << row.n << " moving " << row.k << " by " << std::string(row.name)
                     << ": tick median " << c.median_us << " us, p95 " << c.p95_us << ", max "
                     << c.max_us << "; allocations " << c.allocations << ", bodies made "
                     << c.bodies_made << " over 300 ticks");
    CHECK(c.bodies == (row.shape == renderer::PropShape::none ? 0u : row.n));
    // A move is a move: no body made for it, and nothing allocated in steady state.
    CHECK(c.bodies_made == 0);
    CHECK(c.allocations == 0);
  }
}

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

  // Moved 3 m on (a module's `prop_move`): the body is moved, not made again, and stops the
  // walker there.
  rig.hand({crate_at(23.0f)});
  CHECK(rig.collision.stats().prop_bodies == 1);
  CHECK(rig.collision.stats().prop_bodies_destroyed == 0);
  CHECK(rig.collision.stats().prop_bodies_created == made);
  CHECK(rig.collision.stats().prop_moves == 1);
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
  // Moved, the box is made once again as a kinematic body, then only moved: it stops the walker
  // at its new place as the static one did.
  const u64 boxes_made = rig.collision.stats().prop_bodies_created;
  for (const f32 z : {21.0f, 22.0f}) {
    PropBody on = boxed;
    on.origin = place(Vec3{0.0f, 0.0f, z});
    rig.hand({on});
  }
  CHECK(rig.collision.stats().prop_bodies_created == boxes_made + 1);
  CHECK(rig.walk() == doctest::Approx(21.75f - radius).epsilon(0.002));

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

// **A moved box is met by every cast the walker and a module make** (scene_collision.md, "What a
// moving prop costs"): a box prop made kinematic at its first move is still what a module's cast
// (`Walker::cast`), the camera's boom (`cast_boom`), a teleport's highest surface below (`drop`)
// and the moving ground's standing ray meet, all of which cast with the walker's mask.
TEST_CASE("scene_collision: a moved box prop is met by the walker's and a module's casts") {
  renderer::SceneData scene;
  crate_scene(scene);
  Rig rig(scene);
  // The mask those casts take (`k_scene_surfaces`). Until 2026-11-18 it was the static layer alone,
  // and every answer after the move below met the floor under the box, or nothing.
  const physics::LayerMask walker_mask = k_scene_surfaces;
  CHECK(walker_mask.test(physics::Layer::Static));
  CHECK(walker_mask.test(physics::Layer::Kinematic));
  PropBody boxed = crate_at(20.0f);
  boxed.collider.shape = renderer::PropShape::box;
  boxed.collider.half_extents = Vec3{0.5f, 0.5f, 0.5f};
  boxed.collider.center = Vec3{0.0f, 0.5f, 0.0f};
  // Straight down onto the box's top at z (a teleport's ray, a module's ray), and a sphere swept at
  // its face from the south (a module's sphere, the boom): what each met.
  const auto down_onto = [&](f32 z, f32& top) {
    physics::RayHit hit;
    if (!rig.physics.cast_ray(place(Vec3{0.0f, 3.0f, z}), Vec3{0.0f, -10.0f, 0.0f}, hit,
                              walker_mask))
      return HitKind::none;
    top = static_cast<f32>(relative(hit.position, WorldPos::origin()).y);
    return rig.collision.describe_hit(hit.body, hit.child).kind;
  };
  const auto swept_at = [&](f32 z) {
    physics::ShapeHit hit;
    if (!rig.physics.cast_sphere(place(Vec3{0.0f, 0.5f, z - 5.0f}), 0.2f, Vec3{0.0f, 0.0f, 10.0f},
                                 hit, walker_mask))
      return HitKind::none;
    return rig.collision.describe_hit(hit.body, hit.child).kind;
  };
  rig.hand({boxed});
  f32 top = 0.0f;
  CHECK(down_onto(20.0f, top) == HitKind::prop);
  CHECK(top == doctest::Approx(1.0f).epsilon(0.002));
  CHECK(swept_at(20.0f) == HitKind::prop);
  // Moved once, made again as a kinematic body: the same answers at its new place.
  for (const f32 z : {21.0f, 22.0f}) {
    boxed.origin = place(Vec3{0.0f, 0.0f, z});
    rig.hand({boxed});
  }
  top = 0.0f;
  CHECK(down_onto(22.0f, top) == HitKind::prop);
  CHECK(top == doctest::Approx(1.0f).epsilon(0.002));
  CHECK(swept_at(22.0f) == HitKind::prop);

  // A key handed twice in one call: the first handed is kept, one body, and none is left behind
  // when the props go (two entries of one key once leaked a body nothing would let go).
  rig.hand({});
  const u32 bodies = rig.physics.body_count();
  rig.hand({crate_at(20.0f), crate_at(25.0f)});
  CHECK(rig.collision.stats().prop_duplicates == 1);
  CHECK(rig.collision.stats().prop_bodies == 1);
  CHECK(rig.physics.body_count() == bodies + 1);
  CHECK(down_onto(20.0f, top) == HitKind::prop);
  CHECK(down_onto(25.0f, top) == HitKind::none);
  rig.hand({crate_at(20.0f), crate_at(25.0f)});
  rig.hand({});
  CHECK(rig.physics.body_count() == bodies);
}

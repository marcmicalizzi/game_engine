// The invisible wall of the owner's v35 walk (2026-11-07; docs/subsystems/scene_collision.md, "What
// gets no body"; roadmap P62): a far-forest tile's mesh — every streamed tree's coarsest form on a
// world tile merged into one rigid instance of the load's prefix, an impostor star of cards a crown
// wide through each trunk — is drawn only where the ring's tail does not draw the tile's trees, so
// never round a walker; binned as a prefix instance (W88) it stopped the walker in open ground,
// 1.55 m from a birch's trunk and 3 m from a pine's. No device, no generator capability.

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
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
#include <bit>
#include <cmath>
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

  explicit Rig(const renderer::SceneData& scene, const Config& config = Config{},
               jobs::JobSystem* jobs = nullptr) {
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
    REQUIRE_MESSAGE(collision.create(physics, scene, nullptr, config, &error, nullptr, jobs),
                    error);
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

// ---- A prop's proxy (scene_collision.md, "A prop's proxy"; a review of P45) ----
// Until 2026-11-18 a mesh prop's proxy was cut and built on the walker's tick, keyed by the bits of
// a scale taken back out of its turned matrix — so a new yaw was very likely a new build — and kept
// for the consumer's life. Now it is keyed by the mesh and the module's scale rounded to a step of
// the walk's error, built at the load for a declared prop at its own size, on the job system for
// the rest with the body made at a promised update, and let go when no prop names it.

namespace {

// A boulder: a sphere of `radius` standing on y = 0 round the mesh's origin, `rings` by `segments`,
// faces outwards (counter-clockwise seen from outside, y up), welded at the poles and the seam.
void boulder(Vector<Vec3>& positions, Vector<u32>& indices, f32 radius, u32 rings, u32 segments) {
  positions.push_back(Vec3{0.0f, 2.0f * radius, 0.0f});  // the top
  for (u32 i = 1; i < rings; ++i) {
    const f32 theta = k_pi * static_cast<f32>(i) / static_cast<f32>(rings);
    for (u32 j = 0; j < segments; ++j) {
      const f32 phi = 2.0f * k_pi * static_cast<f32>(j) / static_cast<f32>(segments);
      positions.push_back(Vec3{radius * std::sin(theta) * std::cos(phi),
                               radius + radius * std::cos(theta),
                               radius * std::sin(theta) * std::sin(phi)});
    }
  }
  positions.push_back(Vec3{0.0f, 0.0f, 0.0f});  // the bottom
  const u32 bottom = positions.size() - 1;
  const auto at = [&](u32 i, u32 j) -> u32 {
    if (i == 0) return 0;
    if (i == rings) return bottom;
    return 1 + (i - 1) * segments + (j % segments);
  };
  for (u32 i = 0; i < rings; ++i) {
    for (u32 j = 0; j < segments; ++j) {
      // Along phi then along theta is outwards (d/dphi x d/dtheta points away from the centre).
      if (i > 0) {
        indices.push_back(at(i, j));
        indices.push_back(at(i, j + 1));
        indices.push_back(at(i + 1, j));
      }
      if (i + 1 < rings) {
        indices.push_back(at(i, j + 1));
        indices.push_back(at(i + 1, j + 1));
        indices.push_back(at(i + 1, j));
      }
    }
  }
}

// The crate as mesh 0 and a boulder 3 m across as mesh 1, both declared as props: a prop's mesh is
// one the scene declares (the collision's own copy holds only the meshes a load placed or
// declared).
void crate_and_boulder_scene(renderer::SceneData& scene) {
  Vector<geometry::ClusterLodMesh> lods;
  std::string error;
  for (u32 m = 0; m < 2; ++m) {
    Vector<Vec3> positions;
    Vector<u32> indices;
    if (m == 0) {
      crate(positions, indices);
    } else {
      boulder(positions, indices, 1.5f, 48, 96);
    }
    geometry::ClusterLodMesh lod;
    REQUIRE_MESSAGE(
        geometry::build_cluster_lod(std::span<const Vec3>(positions.data(), positions.size()),
                                    std::span<const u32>(indices.data(), indices.size()),
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
  scene.props.push_back(renderer::SceneProp{"crate", 0});
  scene.props.push_back(renderer::SceneProp{"boulder", 1});
}

// **What a prop of a mesh no prop used costs the tick** when it arrives mid-run: `ticks` ticks of
// 300 crates, 30 of them moved a centimetre a tick, timed one at a time as `prop_tick_cost` does,
// and at tick 100 a prop of `mesh` at `scale` handed with them. The arrival's tick and the worst of
// the ticks after it, the update its body was made at (counted from the arrival), and the engine
// allocations of every tick outside the arrival and the build's promised update.
struct Arrival {
  f64 median_us = 0.0;
  f64 arrival_us = 0.0;
  f64 worst_after_us = 0.0;
  u32 body_at = ~0u;
  u64 steady_allocations = 0;
  u64 builds = 0;
  u64 inline_builds = 0;
  f64 wait_ms = 0.0;
};

Arrival arrival_cost(const renderer::SceneData& scene, const Config& config, jobs::JobSystem* jobs,
                     u32 mesh, f32 scale, u32 ticks) {
  Rig rig(scene, config, jobs);
  constexpr u32 n = 300;
  constexpr u32 k = 30;
  constexpr u32 k_arrive = 100;
  Vector<PropBody> props;
  Vector<Vec3> at;
  for (u32 i = 0; i < n; ++i) {
    at.push_back(Vec3{-30.0f + 3.0f * static_cast<f32>(i % 20), 0.0f,
                      8.0f - 2.0f * static_cast<f32>(i / 20)});
    PropBody p = crate_at(0.0f, i + 1);
    p.origin = place(at.back());
    props.push_back(p);
  }
  PropBody arriving = crate_at(30.0f, 1000);
  arriving.mesh = mesh;
  arriving.scale = scale;
  props.reserve(n + 1);
  rig.collision.set_props({props.data(), props.size()});
  (void)rig.collision.update_props();
  Vector<PropBody> handed;
  handed.reserve(n + 1);
  Vector<f64> us;
  us.reserve(ticks);
  physics::CharacterInput still{};
  constexpr u32 k_warm = 20;
  Arrival out;
  const u64 builds = rig.collision.stats().prop_proxy_builds;
  bool stepped = true;
  for (u32 t = 0; t < k_warm + ticks; ++t) {
    const u32 since = t - k_warm - k_arrive;  // wraps before the arrival: never a small number
    if (t == k_warm + k_arrive) props.push_back(arriving);  // the host's list: not the tick's
    const u64 a0 = mem::allocations_on_thread();
    const i64 t0 = time::monotonic_ns();
    rig.world->update(rig.observers, rig.tick++, true);
    for (u32 i = 0; i < k; ++i)
      props[i].origin = place(at[i] + Vec3{0.01f * static_cast<f32>(t % 100), 0.0f, 0.0f});
    handed.clear();
    for (const PropBody& p : props)
      handed.push_back(p);
    rig.collision.set_props({handed.data(), handed.size()});
    (void)rig.collision.update_props();
    stepped = rig.walker.step(still) == physics::Status::Ok && stepped;
    const i64 t1 = time::monotonic_ns();
    const u64 a1 = mem::allocations_on_thread();
    if (t < k_warm) continue;
    const f64 tick_us = static_cast<f64>(t1 - t0) / 1000.0;
    us.push_back(tick_us);
    if (since == 0) out.arrival_us = tick_us;
    if (since < 0x80000000u && since > 0)
      out.worst_after_us = std::max(out.worst_after_us, tick_us);
    if (since < 0x80000000u && out.body_at == ~0u && rig.collision.stats().prop_bodies == n + 1)
      out.body_at = since;
    // Steady: before the arrival, and after the promised update and the one after it (the props'
    // two lists, swapped at each hand-over, each grow once to hold the newcomer).
    if (since >= 0x80000000u || since > config.prop_build_updates + 1)
      out.steady_allocations += a1 - a0;
  }
  REQUIRE(stepped);
  out.builds = rig.collision.stats().prop_proxy_builds - builds;
  out.inline_builds = rig.collision.stats().prop_proxy_inline;
  out.wait_ms = static_cast<f64>(rig.collision.stats().prop_proxy_wait_ns) / 1e6;
  std::sort(us.begin(), us.end());
  out.median_us = us[us.size() / 2];
  return out;
}

}  // namespace

// ---- Doors that open (roadmap S91; docs/subsystems/scene_collision.md, "Doors"; ADR-0070) ----
// A door is a kinematic box on its hinge, made while its tile is held, turned toward its state by
// integer millidegrees a tick, toggled at a tick, held where its leaf would enter the walker.

namespace {

// A doorway in a wall across z = 20: the wall from x = -5 to 5 with a gap 0.9 m wide at x in
// [-0.45, 0.45], 0.1 m thick; the door's hinge at x = -0.45 on the wall's south face. Closed, the
// leaf runs along +x across the gap; open, it has swung 90 degrees to run north (+z), away from a
// walker coming from the south.
struct DoorRig {
  physics::World physics;
  renderer::SceneData scene;
  SceneCollision collision;
  std::unique_ptr<world::World> world;
  physics::CharacterBody walker;
  sim::ObserverSet observers;
  u64 tick = 0;

  DoorRig() {
    physics::WorldOptions o;
    o.max_bodies = 1024;
    o.max_body_pairs = 4096;
    o.max_contact_constraints = 4096;
    REQUIRE(physics.init(o) == physics::Status::Ok);
    wall(Vec3{0.0f, -0.5f, 0.0f}, Vec3{200.0f, 0.5f, 200.0f});     // the floor
    wall(Vec3{-2.725f, 1.0f, 20.05f}, Vec3{2.275f, 1.0f, 0.05f});  // west of the gap
    wall(Vec3{2.725f, 1.0f, 20.05f}, Vec3{2.275f, 1.0f, 0.05f});   // east of it
    scene.terrain_mesh = ~0u;
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
    REQUIRE(walker.create(physics, c, place(Vec3{0.0f, 0.02f, 17.0f})) == physics::Status::Ok);
  }
  void wall(Vec3 centre, Vec3 half) {
    physics::ShapeId shape;
    REQUIRE(physics.create_box(half, shape) == physics::Status::Ok);
    physics::BodyDesc body;
    body.shape = shape;
    body.transform.position = place(centre);
    body.motion = physics::MotionType::Static;
    body.layer = physics::Layer::Static;
    physics::BodyId id;
    REQUIRE(physics.create_body(body, id) == physics::Status::Ok);
  }
  void move_ring(WorldPos at) {
    observers.clear();
    observers.add(at, 1.0f);
    world->update(observers, tick++, true);
  }
  // `seconds` of ticks: the doors stepped before the walker, as a host does.
  f32 walk(f32 seconds, Vec3 move) {
    physics::CharacterInput in;
    in.move = move;
    for (u32 k = 0; k < static_cast<u32>(seconds * 240.0f); ++k) {
      (void)collision.step_doors(&walker);
      REQUIRE(walker.step(in) == physics::Status::Ok);
    }
    return relative(walker.state().position, WorldPos::origin()).z;
  }
};

DoorBody doorway() {
  DoorBody d;
  d.key = 3;
  d.hinge = place(Vec3{-0.45f, 0.0f, 19.975f});
  d.closed_mdeg = 0;
  d.open_mdeg = -90000;  // swings away from the walker coming from the south: toward +z
  d.half_extents = Vec3{0.45f, 1.0f, 0.025f};
  return d;
}

}  // namespace

TEST_CASE(
    "scene_collision: a mesh prop's proxy is one a mesh and rounded scale, whatever its yaw, and "
    "goes when no prop names it") {
  renderer::SceneData scene;
  crate_scene(scene);
  Rig rig(scene);
  // The declared crate's proxy at its own size was built at the load.
  CHECK(rig.collision.stats().prop_proxies == 1);
  const u64 built = rig.collision.stats().prop_proxy_builds;
  CHECK(built == 1);
  // Where the walker stops at the crate at its own size (a couple of centimetres short of the face
  // less the capsule's radius: the character's own padding).
  rig.hand({crate_at(20.0f)});
  const f32 at_one = rig.walk();
  // One crate turned to ten yaws at its own size and at 1.1: one build, for 1.1, not twenty.
  for (const f32 scale : {1.0f, 1.1f}) {
    for (u32 k = 1; k <= 10; ++k) {
      PropBody p = crate_at(20.0f);
      p.rotation = quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, 0.1f * static_cast<f32>(k));
      p.scale = scale;
      rig.hand({p});
      CHECK(rig.collision.stats().prop_bodies == 1);
    }
  }
  CHECK(rig.collision.stats().prop_proxy_builds == built + 1);
  CHECK(rig.collision.stats().prop_proxies == 2);
  // A scale within the step of 1.1 (the walk's 5 cm over twice the crate's reach, about 0.02) is
  // the same proxy and keeps the body; the step itself is a new one.
  const u64 made = rig.collision.stats().prop_bodies_created;
  PropBody nearby = crate_at(20.0f);
  nearby.scale = 1.103f;
  rig.hand({nearby});
  CHECK(rig.collision.stats().prop_bodies_created == made);
  CHECK(rig.collision.stats().prop_proxy_builds == built + 1);
  // The rounded scale stands within a quarter of the walk's error: the 1.103 crate's face is 5 cm
  // farther out than the unit crate's (0.0515 m), and the walker stops that much sooner.
  CHECK(std::fabs((at_one - rig.walk()) - 0.0515f) < 0.0125f + 0.003f);
  // Handed none: the scale's proxy goes, the declared one stays.
  rig.hand({});
  CHECK(rig.collision.stats().prop_proxies == 1);
  CHECK(rig.collision.stats().prop_proxies_freed == 1);
  // Handed again, it is built again: one more build, and the walker is stopped as before.
  rig.hand({nearby});
  CHECK(rig.collision.stats().prop_proxy_builds == built + 2);
  CHECK(rig.collision.stats().prop_bodies == 1);
}

TEST_CASE(
    "scene_collision: a prop's proxy is built off the tick, its body made at the update promised") {
  renderer::SceneData scene;
  crate_and_boulder_scene(scene);
  Config config;
  config.prop_build_updates = 12;
  jobs::JobSystemConfig workers;
  workers.performance_workers = 2;
  workers.pin_threads = false;
  jobs::JobSystem jobs(workers);
  const f32 radius = physics::CharacterConfig{}.radius;
  // With workers and without: the body at the twelfth update after the hand-over either way, so a
  // replay makes it at the same tick whatever the workers did.
  for (jobs::JobSystem* with : {&jobs, static_cast<jobs::JobSystem*>(nullptr)}) {
    CAPTURE(with != nullptr);
    Rig rig(scene, config, with);
    const u64 built = rig.collision.stats().prop_proxy_builds;  // the crate's and the boulder's
    CHECK(built == 2);
    PropBody rock = crate_at(20.0f, 9);
    rock.mesh = 1;
    rock.scale = 1.2f;  // the boulder at a size of its own: nothing was built for it at the load
    rig.hand({rock});
    u32 updates = 1;
    while (rig.collision.stats().prop_bodies == 0 && updates < 40) {
      (void)rig.collision.update_props();
      ++updates;
    }
    CHECK(updates == 1 + config.prop_build_updates);
    CHECK(rig.collision.stats().prop_proxy_builds == built + 1);
    if (with == nullptr) CHECK(rig.collision.stats().prop_proxy_inline == 1);
    MESSAGE("workers " << (with != nullptr) << ": built on the tick "
                       << rig.collision.stats().prop_proxy_inline << ", waited for "
                       << rig.collision.stats().prop_proxy_waits << " ("
                       << static_cast<f64>(rig.collision.stats().prop_proxy_wait_ns) / 1e6
                       << " ms)");
    // The boulder's face, 1.8 m south of its origin at 1.2: the walker stops there, within the
    // cut's 5 cm and the character's own couple of centimetres.
    CHECK(std::fabs(rig.walk() - (20.0f - 1.8f - radius)) < 0.08f);
    // Gone with its prop: the two declared stay.
    rig.hand({});
    CHECK(rig.collision.stats().prop_proxies == 2);
    CHECK(rig.collision.stats().prop_proxies_freed == 1);
  }
}

TEST_CASE(
    "scene_collision: a drop takes every pending prop's proxy now, so it lands on a boulder") {
  // A drop (engine-view's `Walker::drop`, a teleport too) updates the props and casts straight down
  // for the highest surface. Until 2026-11-18 that update waited for a new size's proxy like any
  // other, so a drop onto a boulder of a size never placed met the floor under it and left the
  // walker inside its shell for twelve ticks. `update_props(true)` takes every pending proxy in
  // reach: one a job has is brought forward and built or waited for, a new one built on the spot.
  renderer::SceneData scene;
  crate_and_boulder_scene(scene);
  Config config;
  config.prop_build_updates = k_walk_prop_build_updates;
  jobs::JobSystemConfig workers;
  workers.performance_workers = 2;
  workers.pin_threads = false;
  jobs::JobSystem jobs(workers);
  PropBody rock = crate_at(20.0f, 9);
  rock.mesh = 1;
  rock.scale = 1.5f;  // the 3 m boulder at 4.5 m: nothing was built for it at the load
  const f32 top = 2.0f * 1.5f * 1.5f;
  const auto down_at = [](Rig& rig, f32& y) {
    physics::RayHit hit;
    if (!rig.physics.cast_ray(place(Vec3{0.0f, 10.0f, 20.0f}), Vec3{0.0f, -20.0f, 0.0f}, hit))
      return false;
    y = static_cast<f32>(relative(hit.position, WorldPos::origin()).y);
    return true;
  };
  for (jobs::JobSystem* with : {&jobs, static_cast<jobs::JobSystem*>(nullptr)}) {
    CAPTURE(with != nullptr);
    // Handed on a walking tick: the body waits for its promised update, and a ray down meets the
    // floor. The drop's update after it takes the same proxy now: the ray meets the boulder's top.
    {
      Rig rig(scene, config, with);
      rig.hand({rock});
      f32 y = -1.0f;
      REQUIRE(down_at(rig, y));
      CHECK(std::fabs(y) < 0.01f);
      CHECK(rig.collision.stats().prop_bodies == 0);
      (void)rig.collision.update_props(true);
      CHECK(rig.collision.stats().prop_bodies == 1);
      REQUIRE(down_at(rig, y));
      CHECK(std::fabs(y - top) < 0.08f);  // the cut's 5 cm, and the rounded scale's quarter of it
      CHECK(rig.collision.stats().prop_proxy_builds == 3);  // the two declared, and this one once
      // What they keep resident (scene_collision.md, "What props and doors keep resident").
      MESSAGE("resident: " << rig.collision.stats().prop_proxies << " proxies, "
                           << rig.collision.stats().prop_proxy_bytes << " bytes");
      CHECK(rig.collision.stats().prop_proxy_bytes > 0);
    }
    // Handed at the drop itself: built on the spot, the body made at this update.
    {
      Rig rig(scene, config, with);
      const Vector<PropBody> list{rock};
      rig.collision.set_props({list.data(), list.size()});
      (void)rig.collision.update_props(true);
      CHECK(rig.collision.stats().prop_bodies == 1);
      CHECK(rig.collision.stats().prop_proxy_inline == 1);
      f32 y = -1.0f;
      REQUIRE(down_at(rig, y));
      CHECK(std::fabs(y - top) < 0.08f);
      // A walking update after it changes nothing: the body stays, nothing is built again.
      (void)rig.collision.update_props();
      CHECK(rig.collision.stats().prop_bodies == 1);
      CHECK(rig.collision.stats().prop_proxy_builds == 3);
    }
  }
}

TEST_CASE("scene_collision: a restored walk's first update has the bodies the saved walk had") {
  // A save holds no proxy state (scene_collision.md, "A prop's proxy"): a walk restored from one
  // is a fresh consumer handed the same props, and begins with a drop, which takes every proxy in
  // reach. So from that update it holds what the walk that was saved held after its props' builds
  // came due: the same bodies, the same proxies resident, a ray down meeting the same top.
  renderer::SceneData scene;
  crate_and_boulder_scene(scene);
  Config config;
  config.prop_build_updates = k_walk_prop_build_updates;
  jobs::JobSystemConfig workers;
  workers.performance_workers = 2;
  workers.pin_threads = false;
  jobs::JobSystem jobs(workers);
  Vector<PropBody> props;
  for (u32 i = 0; i < 4; ++i) {
    PropBody p = crate_at(20.0f + 4.0f * static_cast<f32>(i), 10 + i);
    p.mesh = i % 2;
    p.scale = 1.0f + 0.2f * static_cast<f32>(i);  // the first at its own size, three of their own
    props.push_back(p);
  }
  const auto tops = [&](Rig& rig, Vector<f32>& out) {
    out.clear();
    for (u32 i = 0; i < 4; ++i) {
      physics::RayHit hit;
      const f32 z = 20.0f + 4.0f * static_cast<f32>(i);
      REQUIRE(rig.physics.cast_ray(place(Vec3{0.0f, 10.0f, z}), Vec3{0.0f, -20.0f, 0.0f}, hit));
      out.push_back(static_cast<f32>(relative(hit.position, WorldPos::origin()).y));
    }
  };
  // The walk that was saved: handed on a walking update, twenty updates on.
  Rig saved(scene, config, &jobs);
  saved.collision.set_props({props.data(), props.size()});
  for (u32 k = 0; k < 20; ++k)
    (void)saved.collision.update_props();
  // The restored one: a fresh consumer, the same props, a drop's update.
  Rig restored(scene, config, &jobs);
  restored.collision.set_props({props.data(), props.size()});
  (void)restored.collision.update_props(true);
  CHECK(saved.collision.stats().prop_bodies == 4);
  CHECK(restored.collision.stats().prop_bodies == saved.collision.stats().prop_bodies);
  CHECK(restored.collision.stats().prop_proxies == saved.collision.stats().prop_proxies);
  Vector<f32> a;
  Vector<f32> b;
  tops(saved, a);
  tops(restored, b);
  for (u32 i = 0; i < 4; ++i) {
    CAPTURE(i);
    CHECK(a[i] > 0.5f);   // a prop's top, not the floor
    CHECK(a[i] == b[i]);  // the same proxy at the same pose: the same float
  }
}

TEST_CASE("scene_collision: a prop's proxy goes with its tile, and comes back with it") {
  // Until 2026-11-18 a proxy was counted over every prop handed, so one built while its prop was
  // near stayed resident after the walker left, until the game stopped handing that prop: the
  // table grew with the ground walked. Now it is counted over the props whose tile is held.
  renderer::SceneData scene;
  crate_and_boulder_scene(scene);
  Rig rig(scene);  // `prop_build_updates` 0: built at the update that wants it
  PropBody rock = crate_at(20.0f, 9);
  rock.mesh = 1;
  rock.scale = 1.2f;
  rig.hand({rock});
  CHECK(rig.collision.stats().prop_bodies == 1);
  CHECK(rig.collision.stats().prop_proxies == 3);  // the two declared, and the boulder at 1.2
  const u64 builds = rig.collision.stats().prop_proxy_builds;
  const u64 three = rig.collision.stats().prop_proxy_bytes;
  // The walker a kilometre away: the tile goes, the body with it, and the proxy no prop in reach
  // names; the declared two stay. The prop is still handed.
  rig.move_ring(place(Vec3{1000.0f, 0.0f, 1000.0f}));
  MESSAGE("resident proxies: the crate and the boulder declared "
          << rig.collision.stats().prop_proxy_bytes << " bytes; the boulder at 1.2 "
          << three - rig.collision.stats().prop_proxy_bytes << " more");
  CHECK(rig.collision.stats().prop_proxy_bytes < three);
  CHECK(rig.collision.stats().prop_bodies == 0);
  CHECK(rig.collision.stats().prop_proxies == 2);
  CHECK(rig.collision.stats().prop_proxies_freed == 1);
  // Back: built again, and the body made again.
  rig.move_ring(place(Vec3{0.0f, 0.0f, 16.0f}));
  CHECK(rig.collision.stats().prop_bodies == 1);
  CHECK(rig.collision.stats().prop_proxies == 3);
  CHECK(rig.collision.stats().prop_proxy_builds == builds + 1);
}

TEST_CASE(
    "scene_collision: a prop of a new mesh arriving mid-run costs the walker's tick no build") {
  // The tick's cost round the arrival of a boulder prop among 300 crates of which 30 move (P2's
  // measurement; scene_collision.md, "A prop's proxy"). At its own size: built at the load. At a
  // scale of its own: built on the job system, the body twelve updates later; with no job system,
  // built on the tick at that update. Zero allocations on every tick but the arrival's, the one
  // after it and the promised update's. The ticks here run back to back, not at 240 Hz, so the
  // twelve updates give a worker far less time than a walk's 50 ms: a wait here is an upper bound.
  jobs::JobSystemConfig workers;
  workers.performance_workers = 2;
  workers.pin_threads = false;
  jobs::JobSystem jobs(workers);
  Config config;
  config.prop_build_updates = k_walk_prop_build_updates;
  struct Row {
    f32 scale;
    bool with_jobs;
    const char* name;
  };
  const Row rows[] = {{1.0f, true, "at its own size"},
                      {1.37f, true, "at a scale of its own, on the job system"},
                      {1.37f, false, "at a scale of its own, no job system"}};
  renderer::SceneData scene;
  crate_and_boulder_scene(scene);
  for (const Row& row : rows) {
    const Arrival a =
        arrival_cost(scene, config, row.with_jobs ? &jobs : nullptr, 1, row.scale, 200);
    MESSAGE("a boulder " << std::string(row.name) << ": tick median " << a.median_us
                         << " us, the arrival's " << a.arrival_us << ", the worst after "
                         << a.worst_after_us << "; body at update " << a.body_at << ", builds "
                         << a.builds << " (on the tick " << a.inline_builds << ", waited "
                         << a.wait_ms << " ms), steady allocations " << a.steady_allocations);
    CHECK(a.steady_allocations == 0);
    const bool built_at_load = row.scale == 1.0f;
    CHECK(a.builds == (built_at_load ? 0u : 1u));
    CHECK(a.body_at == (built_at_load ? 0u : config.prop_build_updates));
  }
}

TEST_CASE("scene_collision: a closed door stops a walker, an open one lets it through") {
  DoorRig rig;
  const f32 radius = physics::CharacterConfig{}.radius;
  const DoorBody d = doorway();
  rig.collision.set_doors({&d, 1});
  (void)rig.collision.step_doors(nullptr);
  CHECK(rig.collision.door_stats().bodies == 1);
  // Closed: the walker going north stops at the leaf's south face less its radius.
  const f32 stopped = rig.walk(3.0f, Vec3{0.0f, 0.0f, 1.0f});
  MESSAGE("at the closed door: z " << stopped);
  CHECK(stopped == doctest::Approx(19.95f - radius).epsilon(0.002));
  // Toggled with the walker standing against the leaf: the leaf swings away from it (to +z), so
  // it is not held; 90 degrees at 375 a tick is 240 ticks, a second.
  rig.walker.teleport(place(Vec3{0.0f, 0.02f, 17.0f}));
  REQUIRE(rig.collision.toggle_door(3));
  (void)rig.walk(1.0f, Vec3{});
  DoorBody now;
  REQUIRE(rig.collision.door(3, now));
  CHECK(now.open);
  CHECK(now.angle_mdeg == -90000);
  CHECK(rig.collision.door_stats().moves == 240);
  // Open: the walker goes through the doorway.
  CHECK(rig.walk(3.0f, Vec3{0.0f, 0.0f, 1.0f}) > 21.0f);
}

// A door's turn is a function of its integer angle's bits on every compiler (docs: "Doors"): the
// half angle's sine and cosine are core/math's deterministic ones. Pinned over a spread of angles,
// two turns either way at an eighth of a degree; taken on MSVC (msvc-debug, 2026-11-18) and to be
// confirmed by a Linux container. The C library's `std::sin` and `std::cos`, which the turn used
// before, differ from these in the last bit at some of the angles, and from each other between
// MSVC's UCRT and glibc.
TEST_CASE("scene_collision: a door's turn is the same bits on every compiler") {
  constexpr u64 k_turn_pin = 0x65366023bb079f78ull;
  u64 pin = 0x9e3779b97f4a7c15ull;
  for (i32 mdeg = -720000; mdeg <= 720000; mdeg += 125) {
    const Quat q = door_rotation(mdeg);
    const u32 words[4] = {std::bit_cast<u32>(q.x), std::bit_cast<u32>(q.y), std::bit_cast<u32>(q.z),
                          std::bit_cast<u32>(q.w)};
    pin = hash_combine(pin, hash_bytes(words, sizeof(words)));
    CHECK(q.x == 0.0f);
    CHECK(q.z == 0.0f);
    CHECK(q.y * q.y + q.w * q.w == doctest::Approx(1.0f).epsilon(1e-6));
  }
  MESSAGE("door turn pin: 0x" << std::hex << pin);
  CHECK(pin == k_turn_pin);
  // A whole turn is no turn, and a quarter turn is the half angle's exact values.
  CHECK(door_rotation(0) == Quat::identity());
  CHECK(door_rotation(360000) == door_rotation(0));
  CHECK(door_rotation(-270000) == door_rotation(90000));
  CHECK(door_rotation(90000).y == doctest::Approx(0.70710678f));
  CHECK(door_rotation(90000).w == doctest::Approx(0.70710678f));
}

// `set_doors` brings every number a tick adds to into range, so the integer swing cannot overflow,
// and keeps one door a key: the first given, the later one refused and counted.
TEST_CASE("scene_collision: doors of one key are refused, and a door's numbers are kept in range") {
  DoorRig rig;
  DoorBody first = doorway();
  DoorBody second = doorway();
  second.hinge = place(Vec3{5.0f, 0.0f, 20.0f});
  DoorBody wild = doorway();
  wild.key = 9;
  wild.speed_mdeg = 2000000000;
  wild.open_mdeg = 2000000000;
  wild.angle_mdeg = -2000000000;
  DoorBody idle = doorway();
  idle.key = 11;
  idle.speed_mdeg = 0;
  const DoorBody doors[4] = {first, wild, second, idle};
  rig.collision.set_doors({doors, 4});
  CHECK(rig.collision.door_stats().doors == 3);
  CHECK(rig.collision.door_stats().refused == 1);
  DoorBody now;
  REQUIRE(rig.collision.door(first.key, now));
  CHECK(std::fabs(relative(now.hinge, first.hinge).x) < 1e-6);  // the first given of the key
  REQUIRE(rig.collision.door(9, now));
  CHECK(now.speed_mdeg == 180000);
  CHECK(now.open_mdeg == 360000);
  CHECK(now.angle_mdeg == 0);
  REQUIRE(rig.collision.door(11, now));
  CHECK(now.speed_mdeg == k_door_speed_mdeg);
  // The wild door swings a whole turn in two ticks and stops there, no overflow on the way.
  REQUIRE(rig.collision.toggle_door(9));
  (void)rig.collision.step_doors(nullptr);
  (void)rig.collision.step_doors(nullptr);
  (void)rig.collision.step_doors(nullptr);
  REQUIRE(rig.collision.door(9, now));
  CHECK(now.angle_mdeg == 360000);
}

TEST_CASE("scene_collision: a door holds its angle where its leaf would enter the walker") {
  DoorRig rig;
  const DoorBody d = doorway();
  rig.collision.set_doors({&d, 1});
  (void)rig.collision.step_doors(nullptr);
  // Open it, walk the walker into the doorway and stand it in the leaf's arc, then close it.
  REQUIRE(rig.collision.toggle_door(3));
  (void)rig.walk(1.0f, Vec3{});
  rig.walker.teleport(place(Vec3{0.0f, 0.02f, 20.5f}));  // north of the wall, in the leaf's arc
  REQUIRE(rig.collision.toggle_door(3));
  (void)rig.walk(1.5f, Vec3{});
  DoorBody now;
  REQUIRE(rig.collision.door(3, now));
  MESSAGE("held at " << now.angle_mdeg << " mdeg, the walker at z "
                     << relative(rig.walker.feet(), WorldPos::origin()).z);
  CHECK_FALSE(now.open);
  CHECK(now.angle_mdeg < 0);       // it closed only as far as the walker
  CHECK(now.angle_mdeg > -90000);  // and moved some way
  CHECK(rig.collision.door_stats().holds > 0);
  // The walker steps away north: the door finishes closing.
  rig.walker.teleport(place(Vec3{0.0f, 0.02f, 23.0f}));
  (void)rig.walk(1.0f, Vec3{});
  REQUIRE(rig.collision.door(3, now));
  CHECK(now.angle_mdeg == 0);
}

TEST_CASE("scene_collision: a fast door is held along its arc, never swung through the walker") {
  // Until 2026-11-18 the hold tested only the end of a tick's swing, so a door a game gave a
  // quarter turn a tick went from shut to open through a walker standing in its arc. The walker
  // here stands north of the wall, 0.6 m from the hinge at 45 degrees into the swing: clear of the
  // leaf shut (0.42 m off it) and open (0.42 m), in its path between.
  DoorRig rig;
  DoorBody d = doorway();
  d.speed_mdeg = 90000;
  rig.collision.set_doors({&d, 1});
  (void)rig.collision.step_doors(nullptr);
  REQUIRE(rig.walker.teleport(place(Vec3{-0.45f + 0.4243f, 0.02f, 19.975f + 0.4243f})) ==
          physics::Status::Ok);
  REQUIRE(rig.collision.toggle_door(d.key));
  (void)rig.collision.step_doors(&rig.walker);
  DoorBody now;
  REQUIRE(rig.collision.door(d.key, now));
  MESSAGE("a quarter turn a tick, the walker in the arc: the leaf at " << now.angle_mdeg
                                                                       << " mdeg");
  // The leaf comes within the radius and the margin (0.35 m) of the walker 6 to 7 degrees in: it
  // goes the whole degrees short of that and holds there.
  CHECK(now.angle_mdeg < 0);
  CHECK(now.angle_mdeg > -10000);
  CHECK(rig.collision.door_stats().holds == 1);
  const i32 held = now.angle_mdeg;
  for (u32 k = 0; k < 10; ++k)
    (void)rig.collision.step_doors(&rig.walker);
  REQUIRE(rig.collision.door(d.key, now));
  CHECK(now.angle_mdeg == held);
  // With nobody in the arc it opens in a tick.
  (void)rig.collision.step_doors(nullptr);
  REQUIRE(rig.collision.door(d.key, now));
  CHECK(now.angle_mdeg == d.open_mdeg);
}

TEST_CASE("scene_collision: a door's body goes with its tile, and its swing replays") {
  // The same toggles at the same ticks reach the same angles; the tile let go takes the body, and
  // the door's state stays (it is the world's).
  DoorRig a;
  DoorRig b;
  const DoorBody d = doorway();
  a.collision.set_doors({&d, 1});
  b.collision.set_doors({&d, 1});
  for (u32 t = 0; t < 300; ++t) {
    if (t == 10 || t == 130) {
      REQUIRE(a.collision.toggle_door(3));
      REQUIRE(b.collision.toggle_door(3));
    }
    (void)a.collision.step_doors(&a.walker);
    (void)b.collision.step_doors(&b.walker);
  }
  DoorBody da;
  DoorBody db;
  REQUIRE(a.collision.door(3, da));
  REQUIRE(b.collision.door(3, db));
  CHECK(da.angle_mdeg == db.angle_mdeg);
  CHECK(da.angle_mdeg == 0);  // opened 120 ticks from tick 10, closed again by tick 250
  a.move_ring(place(Vec3{1000.0f, 0.0f, 1000.0f}));
  (void)a.collision.step_doors(nullptr);
  CHECK(a.collision.door_stats().bodies == 0);
  CHECK(a.collision.door_stats().doors == 1);
  a.move_ring(place(Vec3{0.0f, 0.0f, 16.0f}));
  (void)a.collision.step_doors(nullptr);
  CHECK(a.collision.door_stats().bodies == 1);
}

// **What a swinging door costs the walker's tick** (scene_collision.md, "Doors"): 30 doors in a row
// north of a standing walker, 300 static boxes beside them as a held tile's bodies stand for, and
// the first `k` kept swinging — each toggled the tick it reaches its state — for `ticks` ticks of
// the ring's update, `step_doors` and the walker's step, timed one at a time after a warm-up, with
// the engine allocations the tick made on this thread. A swing is a move of a kinematic body, as a
// prop's move is: the bodies are what they were, so nothing about the broadphase is rebuilt.
namespace {

struct DoorCost {
  f64 median_us = 0.0;
  f64 p95_us = 0.0;
  u64 allocations = 0;
  u64 moves = 0;
};

DoorCost door_tick_cost(u32 k, u32 ticks) {
  DoorRig rig;
  for (u32 i = 0; i < 300; ++i)
    rig.wall(Vec3{-30.0f + 3.0f * static_cast<f32>(i % 20), 0.5f,
                  -10.0f - 2.0f * static_cast<f32>(i / 20)},
             Vec3{0.4f, 0.5f, 0.4f});
  Vector<DoorBody> doors;
  for (u32 i = 0; i < 30; ++i) {
    DoorBody d = doorway();
    d.key = 100 + i;
    d.hinge = place(Vec3{-30.0f + 2.0f * static_cast<f32>(i), 0.0f, 30.0f});
    doors.push_back(d);
  }
  rig.collision.set_doors({doors.data(), doors.size()});
  (void)rig.collision.step_doors(nullptr);
  REQUIRE(rig.collision.door_stats().bodies == 30);
  physics::CharacterInput still{};
  Vector<f64> us;
  us.reserve(ticks);
  constexpr u32 k_warm = 20;
  DoorCost out;
  u64 moves = 0;
  for (u32 t = 0; t < k_warm + ticks; ++t) {
    if (t == k_warm) moves = rig.collision.door_stats().moves;
    const u64 a0 = mem::allocations_on_thread();
    const i64 t0 = time::monotonic_ns();
    rig.world->update(rig.observers, rig.tick++, true);
    for (u32 i = 0; i < k; ++i) {  // a module's toggles: the door that came to rest swings back
      DoorBody now;
      if (rig.collision.door(100 + i, now) && now.angle_mdeg == (now.open ? now.open_mdeg : 0))
        (void)rig.collision.toggle_door(100 + i);
    }
    (void)rig.collision.step_doors(&rig.walker);
    REQUIRE(rig.walker.step(still) == physics::Status::Ok);
    const i64 t1 = time::monotonic_ns();
    const u64 a1 = mem::allocations_on_thread();
    if (t < k_warm) continue;
    us.push_back(static_cast<f64>(t1 - t0) / 1000.0);
    out.allocations += a1 - a0;
  }
  out.moves = rig.collision.door_stats().moves - moves;
  std::sort(us.begin(), us.end());
  out.median_us = us[us.size() / 2];
  out.p95_us = us[(us.size() * 95) / 100];
  return out;
}

}  // namespace

TEST_CASE("scene_collision: a swinging door costs the walker's tick a move, not a rebuild") {
  for (const u32 k : {0u, 1u, 30u}) {
    const DoorCost c = door_tick_cost(k, 300);
    MESSAGE("doors swinging " << k << " of 30: tick median " << c.median_us << " us, p95 "
                              << c.p95_us << "; moves " << c.moves << ", allocations "
                              << c.allocations << " over 300 ticks");
    CHECK(c.allocations == 0);
    CHECK(c.moves == 300u * k);  // a swinging door moves every tick
  }
}

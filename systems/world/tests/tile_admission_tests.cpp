// When a tile is admitted, released and built, and on which thread (T131 with W118;
// docs/subsystems/world.md, "Builds off the frame's thread", "No trading at the pair cap", "A
// tile's meshes: residency by reference"), over fixture generators of the test's own: a field of
// tiles each holding a few instances of one resident mesh, as many as the tile's hash and its ring
// say, and a tile that hands two meshes of its own whose recipes change with its ring.
//
// What the cases hold: a walk at the pair cap trades no tile — none the budget let go is drawn
// again while it is still held — where the rule it replaced traded on the same walk; a budgeted
// walk with a job system asks no generator on the walking thread and settles on the tail a consumer
// that built every tile in its event holds, instance for instance and bit for bit; and a room
// smaller than two tiles' meshes, after a ring step, draws the observer's tile (W118).
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <systems/renderer/scene.h>
#include <systems/world/placement_tiles.h>
#include <systems/world/scene_consumers.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <tile_mesh_fixture.h>

using namespace engine;
using namespace engine::world;

namespace {

// ---- the field: every tile within `reach` holds 2 + (hash % 5) instances of mesh 0, three times
// that in ring 0 and twice in ring 1, so a ring change redraws the tile and changes its pairs.
std::atomic<std::thread::id>& walking_thread() {
  static std::atomic<std::thread::id> id{};
  return id;
}
std::atomic<u32>& tiles_on_walking_thread() {
  static std::atomic<u32> n{0};
  return n;
}
std::atomic<u32>& tiles_elsewhere() {
  static std::atomic<u32> n{0};
  return n;
}

struct Field {
  i32 reach = 12;
};

bool field_open(const JsonValue& params, const scene_gen::Context&, void** state, std::string*) {
  auto* f = new Field;
  i64 n = 0;
  if (const JsonValue* v = params.find("reach"); v != nullptr && v->get_i64(n))
    f->reach = static_cast<i32>(n);
  *state = f;
  return true;
}
void field_close(void* state) noexcept { delete static_cast<Field*>(state); }
bool field_occupies(const void* state, scene_gen::TileCoord t) noexcept {
  const i32 r = static_cast<const Field*>(state)->reach;
  return t.x >= -r && t.x <= r && t.z >= -r && t.z <= r;
}
u8 field_representation(const void*, const scene_gen::Context& context) noexcept {
  return context.ring == scene_gen::k_no_ring ? 0 : context.ring;
}
u32 field_count(scene_gen::TileCoord t, u8 ring) {
  const i32 words[2] = {t.x, t.z};
  const u32 base = 2u + static_cast<u32>(hash_bytes(words, sizeof(words)) % 5u);
  return ring == 0 ? 3u * base : ring == 1 ? 2u * base : base;
}
bool field_tile(void* state, scene_gen::TileCoord t, const scene_gen::Context& context,
                scene_gen::Placements& out, std::string*) {
  if (std::this_thread::get_id() == walking_thread().load()) {
    tiles_on_walking_thread().fetch_add(1);
  } else {
    tiles_elsewhere().fetch_add(1);
  }
  if (!field_occupies(state, t)) return true;
  const u32 n = field_count(t, context.ring == scene_gen::k_no_ring ? 0 : context.ring);
  const f64 size = static_cast<f64>(context.tile_size);
  for (u32 k = 0; k < n; ++k) {
    scene_gen::Placement p;
    p.mesh = 0;
    p.position = WorldPos{(static_cast<f64>(t.x) + (static_cast<f64>(k) + 0.5) / n) * size, 0.0,
                          (static_cast<f64>(t.z) + 0.5) * size};
    p.scale = Vec3{1.0f + 0.01f * static_cast<f32>(k), 1.0f, 1.0f};
    p.id = k + 1;
    out.instances.push_back(p);
  }
  out.things += 1;
  return true;
}
// A whole read is not what these cases ask; the registry wants one.
bool no_expand(void*, const scene_gen::Context&, scene_gen::Placements&, std::string*) {
  return true;
}

constexpr scene_gen::PlacementGeneratorDesc k_field{.name = "test-admission-field",
                                                    .open = &field_open,
                                                    .close = &field_close,
                                                    .expand = &no_expand,
                                                    .tile = &field_tile,
                                                    .occupies = &field_occupies,
                                                    .representation = &field_representation};

// The scene the reader would leave for the ring: one resident mesh of one cluster, so a tile's
// pairs are its instances, and the field's entry.
renderer::SceneData field_scene(i32 reach) {
  (void)scene_gen::GeneratorRegistry::global().add(k_field);
  renderer::SceneData scene;
  scene.parts.resize(1);
  scene.parts[0].first_cluster = 0;
  scene.parts[0].cluster_count = 1;
  renderer::StreamedPlacements entry;
  entry.generator = "test-admission-field";
  char text[48];
  std::snprintf(text, sizeof(text), "{\"reach\":%d}", reach);
  REQUIRE(parse_json(text, entry.params).ok);
  entry.where = "placements 0";
  entry.meshes.push_back(0);
  scene.streamed.push_back(std::move(entry));
  scene.world.tile_size = 32.0f;
  scene.dynamic = true;
  scene.pair_count = 0;
  return scene;
}

RingParams field_rings() {
  RingParams p;
  p.tile_size = 32.0f;
  p.ring_count = 3;
  p.radius[0] = 1.5f;
  p.radius[1] = 3.5f;
  p.radius[2] = 6.5f;
  p.max_activations = 0;
  p.max_deactivations = 0;
  return p;
}

sim::ObserverSet at(f64 x_tiles, f64 z_tiles) {
  sim::ObserverSet set;
  set.add(WorldPos{x_tiles * 32.0, 0.0, z_tiles * 32.0}, 1.0f);
  return set;
}

// The drawn tiles of each commit, by key: what a trade is counted from.
struct Drawn {
  Vector<u64> keys;
  u32 commits = 0;
  static bool take(void* c, std::span<const PlacementTiles::TileRange> runs, std::string*) {
    auto* self = static_cast<Drawn*>(c);
    self->keys.clear();
    for (const PlacementTiles::TileRange& run : runs)
      self->keys.push_back(run.key);
    ++self->commits;
    return true;
  }
};

// Two instances to the bit, member by member (the struct has padding a copy need not keep).
bool same_instance(const renderer::SceneInstance& a, const renderer::SceneInstance& b) {
  return a.mesh == b.mesh && std::memcmp(&a.transform, &b.transform, sizeof(a.transform)) == 0 &&
         std::memcmp(&a.origin, &b.origin, sizeof(a.origin)) == 0 && a.motion_key == b.motion_key &&
         a.handoff == b.handoff && a.grounded == b.grounded;
}

bool contains(const Vector<u64>& sorted, u64 key) {
  return std::binary_search(sorted.begin(), sorted.end(), key);
}

// A walk east across the field at the pair cap, a quarter tile an update with two builds a
// commit, counting **trades**: a tile drawn, then not drawn while it was still held, then drawn
// again. A tile that leaves the ring and comes back is not one; a walk east comes back to nothing.
struct Walked {
  u32 trades = 0;
  u32 released = 0;
  u32 withheld_steps = 0;  // updates that ended with a tile withheld: at the cap
  u64 returned = 0;
};

Walked walk_at_cap(const renderer::SceneData& scene, PlacementTilesConfig config) {
  PlacementTiles placements;
  std::string error;
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  Drawn drawn;
  placements.set_run_sink(&Drawn::take, &drawn);
  World world(field_rings());
  add_scene_consumers(world, nullptr, &placements);
  placements.set_ring(&world.ring());
  // Per tile: 1 drawn, 2 let go while held, and what was drawn last commit.
  FlatMap<u64, u8> state;
  Vector<u64> before;
  Walked out;
  u64 tick = 0;
  placements.begin_update(0);
  world.update(at(0.5, 0.5), tick++, true);
  (void)placements.end_update();
  // East an eighth of a tile an update, a quarter of a tile to either side of the line by turns: a
  // walker's sway, which reorders tiles a few metres apart in score at every step.
  for (u32 step = 0; step < 96; ++step) {
    const f64 sway = step % 2 == 0 ? 0.25 : -0.25;
    placements.begin_update(2);
    world.update(at(0.5 + 0.125 * static_cast<f64>(step) + sway, 0.5 + sway), tick++);
    (void)placements.end_update();
    Vector<u64> now = drawn.keys;
    std::sort(now.begin(), now.end());
    for (u32 i = 0; i < world.ring().active_count(); ++i) {
      const u64 key = world.ring().active_keys()[i];
      u8& s = state[key];
      const bool is = contains(now, key);
      if (is && s == 2) ++out.trades;
      if (is) s = 1;
      if (!is && s == 1 && contains(before, key)) s = 2;  // let go while it is held
    }
    // A tile the ring let go forgets what it was.
    for (u32 i = 0; i < state.size();) {
      const u64 key = state.key_at(i);
      bool active = false;
      for (u32 k = 0; k < world.ring().active_count() && !active; ++k)
        active = world.ring().active_keys()[k] == key;
      if (!active) {
        state.erase(key);
      } else {
        ++i;
      }
    }
    before = now;
    out.withheld_steps += placements.stats().withheld != 0 ? 1u : 0u;
  }
  out.released = static_cast<u32>(placements.stats().released);
  out.returned = placements.stats().returned;
  return out;
}

}  // namespace

TEST_CASE("tile admission: a walk at the pair cap trades no tile") {
  const renderer::SceneData scene = field_scene(24);
  // The pairs every tile of the rings holds round the start, with no budget: the cap is set to
  // the inner two rings and half the outer one, so the walk stays at it.
  u32 pairs[3] = {};
  {
    PlacementTiles all;
    PlacementTilesConfig config;
    config.ring_count = 3;
    std::string error;
    REQUIRE_MESSAGE(all.create(scene, config, &error), error);
    World world(field_rings());
    add_scene_consumers(world, nullptr, &all);
    world.update(at(0.5, 0.5), 0, true);
    for (u32 i = 0; i < world.ring().active_count(); ++i) {
      const auto n =
          static_cast<u32>(all.tile_instances(tile_of_key(world.ring().active_keys()[i])).size());
      pairs[world.ring().active_rings()[i]] += renderer::reserved_tile_pairs(n);
    }
  }
  REQUIRE(pairs[2] > 0);
  PlacementTilesConfig config;
  config.ring_count = 3;
  config.fixed_pairs = 1000;
  config.max_pairs = config.fixed_pairs + pairs[0] + pairs[1] + pairs[2] / 3;
  // The rule it replaced: no bias, no hold-off, (almost) no spare.
  PlacementTilesConfig before = config;
  before.admit_bias = 0.0f;
  before.admit_hold_off = 0;
  before.admit_spare_pairs = 1;
  const Walked old = walk_at_cap(scene, before);
  const Walked now = walk_at_cap(scene, config);
  MESSAGE("a walk at the cap: " << old.trades << " trades and " << old.released
                                << " tiles let go by the rule it replaced, " << now.trades
                                << " and " << now.released << " by this one; at the cap "
                                << old.withheld_steps << " and " << now.withheld_steps
                                << " updates of 96");
  CHECK(old.trades > 0);  // the walk has teeth
  CHECK(now.trades == 0);
  CHECK(now.returned == 0);
  CHECK(now.withheld_steps > 48);  // it walked at the cap
}

TEST_CASE(
    "tile admission: built on a job, no tile is built on the walking thread, and the tail "
    "settles bit for bit on the one built in place") {
  const renderer::SceneData scene = field_scene(8);
  jobs::JobSystem pool(jobs::JobSystemConfig{
      .performance_workers = 1, .efficiency_workers = 2, .pin_threads = false});
  walking_thread().store(std::this_thread::get_id());
  PlacementTilesConfig config;
  config.ring_count = 3;
  PlacementTiles jobbed;
  PlacementTiles placed;
  std::string error;
  REQUIRE_MESSAGE(jobbed.create(scene, config, &error), error);
  REQUIRE_MESSAGE(placed.create(scene, config, &error), error);
  jobbed.set_build_jobs(&pool);
  World wa(field_rings());
  World wb(field_rings());
  add_scene_consumers(wa, nullptr, &jobbed);
  add_scene_consumers(wb, nullptr, &placed);
  jobbed.set_ring(&wa.ring());
  placed.set_ring(&wb.ring());
  // A walk across the field, budgeted: every tile's build is the job's. Beside it the same walk
  // with every tile built in its event, so the two rings are one ring, tile for tile.
  const u32 on_walker = tiles_on_walking_thread().load();
  const u32 off_walker = tiles_elsewhere().load();
  u64 tick = 0;
  for (u32 step = 0; step < 40; ++step) {
    const sim::ObserverSet where = at(-3.0 + 0.2 * static_cast<f64>(step), 0.5);
    jobbed.begin_update(3);
    wa.update(where, tick);
    (void)jobbed.end_update();
    walking_thread().store(std::thread::id{});  // the one built in place counts nowhere
    placed.begin_update(0);
    wb.update(where, tick++);
    (void)placed.end_update();
    walking_thread().store(std::this_thread::get_id());
  }
  // It stops: the jobs drain, a commit at a time.
  for (u32 tries = 0; tries < 20000; ++tries) {
    jobbed.begin_update(3);
    wa.update(at(5.0, 0.5), tick);
    (void)jobbed.end_update();
    if (jobbed.stats().pending == 0) break;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  walking_thread().store(std::thread::id{});
  placed.begin_update(0);
  wb.update(at(5.0, 0.5), tick);
  (void)placed.end_update();
  CHECK(jobbed.stats().pending == 0);
  CHECK(tiles_on_walking_thread().load() == on_walker);  // none on this thread
  CHECK(tiles_elsewhere().load() > off_walker);
  CHECK(jobbed.stats().jobs > 0);
  CHECK(jobbed.stats().collected > 0);
  CHECK(jobbed.stats().worker_ns > 0);
  MESSAGE("jobs " << jobbed.stats().jobs << ", tiles taken " << jobbed.stats().collected
                  << ", let go " << jobbed.stats().discarded << ", the hand-over "
                  << static_cast<f64>(jobbed.stats().max_collect_ns) / 1e6
                  << " ms at most a commit, the generators "
                  << static_cast<f64>(jobbed.stats().worker_ns) / 1e6 << " ms on the worker");
  // The same place, every tile built in its event.
  REQUIRE(wa.ring().active_count() == wb.ring().active_count());
  u32 compared = 0;
  for (u32 i = 0; i < wb.ring().active_count(); ++i) {
    const TileCoord t = tile_of_key(wb.ring().active_keys()[i]);
    const std::span<const renderer::SceneInstance> a = jobbed.tile_instances(t);
    const std::span<const renderer::SceneInstance> b = placed.tile_instances(t);
    CAPTURE(t.x);
    CAPTURE(t.z);
    REQUIRE(a.size() == b.size());
    for (u32 k = 0; k < a.size(); ++k) {
      CHECK(same_instance(a[k], b[k]));
      ++compared;
    }
  }
  CHECK(compared > 100);
  walking_thread().store(std::thread::id{});
}

// ---- W118: a tile that hands two meshes of its own, whose recipes are its ring's
namespace {

scene_gen::PlacementMesh ring_rock(scene_gen::TileCoord t, u8 ring, u32 m) {
  const i32 gx = t.x * 16 + static_cast<i32>(ring) * 4 + static_cast<i32>(m);
  scene_gen::PlacementMesh out;
  out.path = test::tile_rock_path(gx, t.z);
  out.name = "rock";
  out.recipe = test::tile_rock_recipe(gx, t.z, 6);
  out.reach = test::k_tile_rock_reach;
  return out;
}
bool ring_rocks_open(const JsonValue&, const scene_gen::Context&, void** state, std::string*) {
  *state = new int(0);
  return true;
}
void ring_rocks_close(void* state) noexcept { delete static_cast<int*>(state); }
bool ring_rocks_occupies(const void*, scene_gen::TileCoord t) noexcept {
  return t.x >= -4 && t.x <= 4 && t.z >= -4 && t.z <= 4;
}
bool ring_rocks_tile(void* state, scene_gen::TileCoord t, const scene_gen::Context& context,
                     scene_gen::Placements& out, std::string*) {
  if (!ring_rocks_occupies(state, t)) return true;
  const u8 ring = context.ring == scene_gen::k_no_ring ? 0 : context.ring;
  for (u32 m = 0; m < 2; ++m) {
    out.meshes.push_back(ring_rock(t, ring, m));
    scene_gen::Placement p;
    p.mesh = m;  // past an empty resident list: the tile's own
    p.position = WorldPos{(static_cast<f64>(t.x) + 0.3 + 0.4 * m) * 32.0, 0.0,
                          (static_cast<f64>(t.z) + 0.5) * 32.0};
    p.id = m + 1;
    out.instances.push_back(p);
  }
  out.things += 1;
  return true;
}
constexpr scene_gen::PlacementGeneratorDesc k_ring_rocks{.name = "test-ring-rocks",
                                                         .open = &ring_rocks_open,
                                                         .close = &ring_rocks_close,
                                                         .expand = &no_expand,
                                                         .tile = &ring_rocks_tile,
                                                         .occupies = &ring_rocks_occupies,
                                                         .tile_meshes = true};

// A room that holds `most` meshes, whatever their clusters, and remembers each mesh's path.
struct Room {
  struct Held {
    u32 mesh = 0;
    std::string path;
  };
  Vector<Held> held;
  u32 most = 3;
  u32 violations = 0;
  const Held* find(u32 mesh) const {
    for (const Held& h : held)
      if (h.mesh == mesh) return &h;
    return nullptr;
  }
  static bool fits(void* c, u32, u32) {
    const auto* self = static_cast<const Room*>(c);
    return self->held.size() < self->most;
  }
  static bool admit(void* c, u32 mesh, const renderer::PlacementMeshData& data, std::string*) {
    static_cast<Room*>(c)->held.push_back(Held{mesh, data.source.path});
    return true;
  }
  static void retire(void* c, u32 mesh) {
    auto* self = static_cast<Room*>(c);
    for (u32 k = 0; k < self->held.size(); ++k) {
      if (self->held[k].mesh != mesh) continue;
      self->held.erase(self->held.begin() + k);
      return;
    }
    ++self->violations;
  }
  static bool runs(void* c, std::span<const PlacementTiles::TileRange> ranges, std::string*) {
    auto* self = static_cast<Room*>(c);
    for (const PlacementTiles::TileRange& range : ranges) {
      for (u32 k = 0; k < range.count; ++k)
        if (self->find(range.instances[k].mesh) == nullptr) ++self->violations;
    }
    return true;
  }
  TileMeshSink sink() {
    return TileMeshSink{this, &Room::fits, &Room::admit, &Room::retire, nullptr, nullptr, nullptr};
  }
};

}  // namespace

TEST_CASE(
    "tile admission: a room smaller than two tiles' meshes draws the observer's tile after a "
    "ring step (W118)") {
  test::TempDir tmp{"engine_world_tile_admission_w118"};
  test::register_tile_rocks();
  (void)scene_gen::GeneratorRegistry::global().add(k_ring_rocks);
  renderer::SceneData scene;
  renderer::StreamedPlacements entry;
  entry.generator = "test-ring-rocks";
  REQUIRE(parse_json("{}", entry.params).ok);
  entry.where = "placements 0";
  entry.tile_meshes = true;
  scene.streamed.push_back(std::move(entry));
  scene.tile_meshes = true;
  scene.world.tile_size = 32.0f;
  scene.dynamic = true;
  scene.mesh_options.ddc = tmp.file("ddc");
  scene.mesh_options.cache = true;
  Room room;
  room.most = 3;  // a tile and a half
  PlacementTiles placements;
  PlacementTilesConfig config;
  config.ring_count = 2;
  std::string error;
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  TileMeshConfig meshes;
  meshes.first_mesh = 7;
  meshes.slots = 64;
  meshes.clusters = 1u << 20;
  meshes.max_clusters = 4096;
  meshes.cluster_bytes = 3000;
  meshes.linger = 0;
  placements.set_meshes(meshes, room.sink());
  placements.set_run_sink(&Room::runs, &room);
  RingParams rings;
  rings.tile_size = 32.0f;
  rings.ring_count = 2;
  rings.radius[0] = 0.5f;  // the observer's own tile
  rings.radius[1] = 1.5f;
  rings.max_activations = 0;
  rings.max_deactivations = 0;
  World world(rings);
  add_scene_consumers(world, nullptr, &placements);
  placements.set_ring(&world.ring());
  const auto step = [&](f64 x, u64 tick) {
    placements.begin_update(0);
    world.update(at(x + 0.5, 0.5), tick);
    (void)placements.end_update();
  };
  // The first fill: the observer's tile in its two ring-0 meshes, and the next tile's first mesh.
  step(0.0, 0);
  REQUIRE(placements.tile_instances(TileCoord{0, 0}).size() == 2);
  // A ring step east: tile (1, 0) moves into ring 0 and asks for its ring-0 meshes; (0, 0) moves
  // out to ring 1 and waits for its ring-1 meshes, still drawing its ring-0 ones.
  for (u64 tick = 1; tick < 5; ++tick)
    step(1.0, tick);
  const std::span<const renderer::SceneInstance> own = placements.tile_instances(TileCoord{1, 0});
  CHECK(own.size() == 2);  // until W118: nothing, forever
  for (const renderer::SceneInstance& instance : own) {
    const Room::Held* mesh = room.find(instance.mesh);
    REQUIRE(mesh != nullptr);
    const bool ring0_form = mesh->path.find("rock_16_0") != std::string::npos ||
                            mesh->path.find("rock_17_0") != std::string::npos;
    CHECK(ring0_form);
  }
  CHECK(placements.stats().mesh_yielded > 0);
  CHECK(room.violations == 0);
}

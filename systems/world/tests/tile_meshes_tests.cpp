// A tile's meshes: residency by reference (docs/subsystems/world.md, "A tile's meshes: residency by
// reference"; scene_gen.md, "A tile's meshes"; roadmap R142), with the fixture generators of
// tests/support/tile_mesh_fixture.h — a placement generator whose tile hands its own rock — and a
// sink that records what a renderer would be told, in order, and checks the one rule a renderer
// relies on: no tail names a mesh that is not admitted.
//
// What the cases hold: nothing is built when the scene is opened, a tile's mesh is built and
// admitted when its tile is first held, kept while any tile held names it, and retired after the
// last — after the linger, or at once with none — with the room's bytes counted at each step; two
// tiles that hand one recipe share one mesh; the room's refusal withholds the farthest tiles and
// keeps every tile drawn nearer than every tile withheld as the observer moves; two walks to one
// place hold the same tail; and with builds on a job system and a budgeted update, a tile waits
// for its mesh and no frame is handed an instance of a mesh that is not there.
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <systems/renderer/scene.h>
#include <systems/world/placement_tiles.h>
#include <systems/world/scene_consumers.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <tile_mesh_fixture.h>

using namespace engine;
using namespace engine::world;

namespace {

constexpr u32 k_first_mesh = 7;  // where a renderer would say its mesh slots start
constexpr u64 k_cluster_bytes = 3000;

// What a renderer would be told, as it was told: the meshes admitted now (each with the path it
// was built from and its clusters), every event in order, and the tails checked against them.
struct Recorder {
  struct Held {
    u32 mesh = 0;
    std::string path;
    u32 clusters = 0;
  };
  Vector<Held> held;
  std::string log;
  u32 admissions = 0;
  u32 retirements = 0;
  u32 violations = 0;  // a tail instance of a mesh not admitted
  u32 clusters = 0;
  u32 room = ~0u;  // clusters the room holds
  u32 most = ~0u;  // meshes the room holds, for a test that counts in rocks
  Vector<renderer::SceneInstance> tail;

  const Held* find(u32 mesh) const {
    for (const Held& h : held)
      if (h.mesh == mesh) return &h;
    return nullptr;
  }
  static bool fits(void* context, u32 mesh_clusters, u32) {
    const auto* self = static_cast<const Recorder*>(context);
    return u64{self->clusters} + mesh_clusters <= self->room && self->held.size() < self->most;
  }
  static bool admit(void* context, u32 mesh, const renderer::PlacementMeshData& data,
                    std::string* error) {
    auto* self = static_cast<Recorder*>(context);
    if (self->find(mesh) != nullptr) {
      if (error != nullptr) *error = "the slot holds a mesh";
      return false;
    }
    Held h;
    h.mesh = mesh;
    h.path = data.source.path;
    h.clusters = data.lod.mesh.clusters.size();
    self->held.push_back(h);
    self->clusters += h.clusters;
    ++self->admissions;
    self->log += "+" + std::to_string(mesh) + " ";
    return true;
  }
  static void retire(void* context, u32 mesh) {
    auto* self = static_cast<Recorder*>(context);
    for (u32 k = 0; k < self->held.size(); ++k) {
      if (self->held[k].mesh != mesh) continue;
      self->clusters -= self->held[k].clusters;
      self->held.erase(self->held.begin() + k);
      ++self->retirements;
      self->log += "-" + std::to_string(mesh) + " ";
      return;
    }
    ++self->violations;  // retired what was never admitted
  }
  static bool runs(void* context, std::span<const PlacementTiles::TileRange> ranges, std::string*) {
    auto* self = static_cast<Recorder*>(context);
    self->tail.clear();
    self->log += "t ";
    for (const PlacementTiles::TileRange& range : ranges) {
      for (u32 k = 0; k < range.count; ++k) {
        const renderer::SceneInstance& instance = range.instances[k];
        if (self->find(instance.mesh) == nullptr) ++self->violations;
        self->tail.push_back(instance);
      }
    }
    return true;
  }
  TileMeshSink sink() {
    // No texture hooks: this host draws nothing, and its builds leave the textures out.
    return TileMeshSink{this,    &Recorder::fits, &Recorder::admit, &Recorder::retire,
                        nullptr, nullptr,         nullptr};
  }
};

renderer::SceneData scene_of(const test::TempDir& tmp, const char* params) {
  test::register_tile_rocks();
  renderer::SceneData scene;
  renderer::StreamedPlacements entry;
  entry.generator = "test-tile-rocks";
  REQUIRE(parse_json(params, entry.params).ok);
  entry.where = "placements 0";
  entry.tile_meshes = true;
  scene.streamed.push_back(std::move(entry));
  scene.tile_meshes = true;
  scene.world.tile_size = 32.0f;
  scene.dynamic = true;
  scene.mesh_options.ddc = tmp.file("ddc");
  scene.mesh_options.cache = true;
  return scene;
}

RingParams ring(f32 radius) {
  RingParams params;
  params.ring_count = 1;
  params.radius[0] = radius;
  params.max_activations = 0;
  params.max_deactivations = 0;
  return params;
}

sim::ObserverSet at(f64 tile_x, f64 tile_z) {
  sim::ObserverSet set;
  set.add(WorldPos{(tile_x + 0.5) * 32.0, 0.0, (tile_z + 0.5) * 32.0}, 1.0f);
  return set;
}

TileMeshConfig room(u32 slots, u32 clusters, u32 linger) {
  TileMeshConfig config;
  config.first_mesh = k_first_mesh;
  config.slots = slots;
  config.clusters = clusters;
  config.max_clusters = 4096;
  config.cluster_bytes = k_cluster_bytes;
  config.linger = linger;
  return config;
}

// One update as a host makes it: whole (`budget` 0) or budgeted.
void step(World& world, PlacementTiles& placements, const sim::ObserverSet& observers, u64 tick,
          u32 budget = 0) {
  placements.begin_update(budget);
  world.update(observers, tick);
  (void)placements.end_update();
}

}  // namespace

TEST_CASE("tile meshes: nothing exists before its tile, and nothing stays after its last") {
  test::TempDir tmp{"engine_world_tile_meshes"};
  const renderer::SceneData scene = scene_of(tmp, R"({"reach":4,"n":10})");
  Recorder told;
  PlacementTiles placements;
  PlacementTilesConfig config;
  config.ring_count = 1;
  std::string error;
  const u32 before = test::tile_rock_builds().load();
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  placements.set_meshes(room(16, 1u << 20, 2), told.sink());
  placements.set_run_sink(&Recorder::runs, &told);
  World world(ring(0.5f));  // the observer's tile alone
  add_scene_consumers(world, nullptr, &placements);
  // Opened, and nobody near anything: no mesh was built, none is known, the room holds nothing.
  CHECK(test::tile_rock_builds().load() == before);
  CHECK(placements.meshes().stats().known == 0);
  CHECK(placements.meshes().stats().bytes == 0);
  CHECK(told.admissions == 0);

  step(world, placements, at(0, 0), 0);
  CHECK(test::tile_rock_builds().load() == before + 1);  // its tile's, and no other
  REQUIRE(told.held.size() == 1);
  CHECK(told.held[0].mesh == k_first_mesh);
  CHECK(told.held[0].path.find("rock_0_0") != std::string::npos);
  const u32 clusters = told.held[0].clusters;
  REQUIRE(clusters > 0);
  CHECK(placements.meshes().stats().resident == 1);
  CHECK(placements.meshes().stats().clusters == clusters);
  CHECK(placements.meshes().stats().bytes == u64{clusters} * k_cluster_bytes);
  CHECK(placements.meshes().stats().host_bytes == 0);  // the scene has it: the host's copy went
  // The tile's instances name it, and the mesh was admitted before the tail that draws it.
  const std::span<const renderer::SceneInstance> held = placements.tile_instances(TileCoord{0, 0});
  REQUIRE(held.size() == 2);
  CHECK(held[0].mesh == k_first_mesh);
  CHECK(told.log == "+7 t ");
  CHECK(placements.stats().pairs == 2 * clusters);

  // The observer moves two tiles on: the first tile is let go, and its mesh lingers, unnamed.
  step(world, placements, at(2, 0), 1);
  CHECK(test::tile_rock_builds().load() == before + 2);
  CHECK(told.held.size() == 2);
  CHECK(placements.meshes().stats().lingering == 1);
  CHECK(placements.tile_instances(TileCoord{0, 0}).empty());
  CHECK(told.retirements == 0);
  // Two commits later it is gone — after a tail that no longer names it — and the bytes with it.
  step(world, placements, at(2, 0), 2);
  CHECK(told.retirements == 0);
  step(world, placements, at(2, 0), 3);
  CHECK(told.retirements == 1);
  REQUIRE(told.held.size() == 1);
  CHECK(told.held[0].path.find("rock_2_0") != std::string::npos);
  CHECK(placements.meshes().stats().resident == 1);
  CHECK(placements.meshes().stats().lingering == 0);
  CHECK(placements.meshes().stats().bytes == u64{told.held[0].clusters} * k_cluster_bytes);
  CHECK(told.log.find("t -7 ") != std::string::npos);

  // Back within the linger of the next: the mesh that was let go is built again (a cache hit: the
  // generator does not run) and takes the slot that came free.
  step(world, placements, at(0, 0), 4);
  CHECK(test::tile_rock_builds().load() == before + 2);
  CHECK(told.held.size() == 2);
  // And when the world lets every tile go, every mesh follows.
  world.clear(5);
  for (u64 tick = 6; tick < 10; ++tick)
    step(world, placements, sim::ObserverSet{}, tick);
  CHECK(told.held.empty());
  CHECK(told.admissions == told.retirements);
  CHECK(placements.meshes().stats().known == 0);
  CHECK(placements.meshes().stats().bytes == 0);
  CHECK(told.violations == 0);
}

TEST_CASE("tile meshes: a mesh goes with its last tile at once with no linger, and is shared") {
  test::TempDir tmp{"engine_world_tile_meshes_shared"};
  // Every 2 x 2 group of tiles names one rock.
  const renderer::SceneData scene = scene_of(tmp, R"({"reach":4,"n":10,"share":2})");
  Recorder told;
  PlacementTiles placements;
  PlacementTilesConfig config;
  config.ring_count = 1;
  std::string error;
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  placements.set_meshes(room(16, 1u << 20, 0), told.sink());
  placements.set_run_sink(&Recorder::runs, &told);
  World world(ring(0.5f));
  add_scene_consumers(world, nullptr, &placements);
  const u32 before = test::tile_rock_builds().load();
  step(world, placements, at(0, 0), 0);
  CHECK(told.admissions == 1);
  // The next tile of the group: the same mesh, by reference — nothing built, nothing admitted.
  step(world, placements, at(1, 0), 1);
  CHECK(test::tile_rock_builds().load() == before + 1);
  CHECK(told.admissions == 1);
  CHECK(told.retirements == 0);
  CHECK(told.held.size() == 1);
  REQUIRE(placements.tile_instances(TileCoord{1, 0}).size() == 2);
  CHECK(placements.tile_instances(TileCoord{1, 0})[0].mesh == k_first_mesh);
  // The next group's tile: the first group's mesh has no tile left, and with no linger it goes in
  // the commit that let its last tile go.
  step(world, placements, at(2, 0), 2);
  CHECK(told.admissions == 2);
  CHECK(told.retirements == 1);
  REQUIRE(told.held.size() == 1);
  CHECK(told.held[0].path.find("rock_1_0") != std::string::npos);
  CHECK(told.violations == 0);
}

TEST_CASE("tile meshes: the room's refusal withholds the farthest tiles, whichever came first") {
  test::TempDir tmp{"engine_world_tile_meshes_budget"};
  const renderer::SceneData scene = scene_of(tmp, R"({"reach":6,"n":10})");
  // A room that holds three rocks, whatever their clusters.
  std::string error;
  Recorder told;
  told.most = 3;
  PlacementTiles placements;
  PlacementTilesConfig config;
  config.ring_count = 1;
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  placements.set_meshes(room(32, 1u << 20, 0), told.sink());
  placements.set_run_sink(&Recorder::runs, &told);
  World world(ring(1.5f));  // nine tiles round the observer
  add_scene_consumers(world, nullptr, &placements);

  // Every tile drawn is at least as near as every tile the room withholds.
  const auto ordered = [&](f64 ox, f64 oz) {
    f64 farthest_drawn = 0.0;
    f64 nearest_withheld = 1e30;
    u32 drawn = 0;
    for (i32 z = -8; z <= 8; ++z) {
      for (i32 x = -8; x <= 8; ++x) {
        const f64 dx = (static_cast<f64>(x) + 0.5) - (ox + 0.5);
        const f64 dz = (static_cast<f64>(z) + 0.5) - (oz + 0.5);
        const f64 d = dx * dx + dz * dz;
        if (d > 1.5 * 1.5) continue;  // not in the ring
        if (!placements.tile_instances(TileCoord{x, z}).empty()) {
          ++drawn;
          farthest_drawn = d > farthest_drawn ? d : farthest_drawn;
        } else {
          nearest_withheld = d < nearest_withheld ? d : nearest_withheld;
        }
      }
    }
    CHECK(drawn == 3);
    CHECK(farthest_drawn <= nearest_withheld);
    return drawn;
  };
  step(world, placements, at(0, 0), 0);
  CHECK(told.held.size() == 3);
  CHECK(placements.stats().mesh_starved >= 5);
  CHECK(placements.stats().mesh_waiting == placements.stats().mesh_starved);
  ordered(0, 0);
  // The observer walks: the tiles that are now nearest take the room from the ones that were, the
  // farthest first, and nothing that is drawn is farther than anything withheld.
  u64 tick = 1;
  for (const f64 x : {1.0, 2.0, 3.0, 2.0}) {
    // A yielded tile is built again at the next commit, so the order settles a commit later.
    step(world, placements, at(x, 0), tick++);
    step(world, placements, at(x, 0), tick++);
    step(world, placements, at(x, 0), tick++);
    CHECK(told.held.size() == 3);
    ordered(x, 0);
  }
  CHECK(placements.stats().mesh_yielded > 0);
  CHECK(told.violations == 0);
}

// W51-5 (world.md, "A withheld mesh is let go and built again"): standing where the room is full,
// the withheld tiles' meshes hold no host bytes and nothing is built again while the room stays
// full; when it frees, each is built again — on the job system under budgeted updates, so the
// committing thread only collects — and the world holds the tail a room that was never full holds.
TEST_CASE("tile meshes: a withheld mesh is let go, and built again when the room can take it") {
  test::TempDir tmp{"engine_world_tile_meshes_shelved"};
  const renderer::SceneData scene = scene_of(tmp, R"({"reach":6,"n":10})");
  // Each instance as what it is, as the walks case reads a tail.
  const auto tail_of = [](const Recorder& told) {
    Vector<std::string> out;
    for (const renderer::SceneInstance& instance : told.tail) {
      const Recorder::Held* mesh = told.find(instance.mesh);
      REQUIRE(mesh != nullptr);
      out.push_back(mesh->path + " " + std::to_string(instance.origin.x) + " " +
                    std::to_string(instance.origin.z) + " " +
                    std::to_string(instance.transform.scale.x));
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  // The tail a room that holds every rock draws, standing at (0, 0).
  Vector<std::string> whole_room;
  {
    Recorder told;
    PlacementTiles placements;
    PlacementTilesConfig config;
    config.ring_count = 1;
    std::string error;
    REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
    placements.set_meshes(room(32, 1u << 20, 0), told.sink());
    placements.set_run_sink(&Recorder::runs, &told);
    World world(ring(1.5f));
    add_scene_consumers(world, nullptr, &placements);
    for (u64 tick = 0; tick < 3; ++tick)
      step(world, placements, at(0, 0), tick);
    CHECK(told.held.size() == 9);
    whole_room = tail_of(told);
  }
  REQUIRE(whole_room.size() == 18);

  for (const bool on_jobs : {false, true}) {
    CAPTURE(on_jobs);
    jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
    Recorder told;
    told.most = 3;  // a room of three rocks, whatever their clusters
    PlacementTiles placements;
    PlacementTilesConfig config;
    config.ring_count = 1;
    std::string error;
    REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
    placements.set_meshes(room(32, 1u << 20, 0), told.sink(), on_jobs ? &pool : nullptr);
    placements.set_run_sink(&Recorder::runs, &told);
    World world(ring(1.5f));
    add_scene_consumers(world, nullptr, &placements);
    // Whole updates until the room is full and the order has settled.
    u64 tick = 0;
    for (; tick < 3; ++tick)
      step(world, placements, at(0, 0), tick);
    const TileMeshStats& s = placements.meshes().stats();
    CHECK(told.held.size() == 3);
    const u32 withheld = placements.stats().mesh_starved;
    CHECK(withheld >= 5);
    // The withheld rocks are known by their counts alone: no host bytes, none waiting.
    CHECK(s.shelved == withheld);
    CHECK(s.waiting == 0);
    CHECK(s.host_bytes == 0);
    CHECK(s.rebuilds == 0);
    // Standing with the room full, nothing is built again however many commits pass.
    const u64 builds = s.builds;
    for (u32 k = 0; k < 8; ++k)
      step(world, placements, at(0, 0), tick++, on_jobs ? 4u : 0u);
    CHECK(s.builds == builds);
    CHECK(s.rebuilds == 0);
    CHECK(s.shelved == withheld);
    // The room grows: every shelved rock is built again and admitted. Whole updates do it in the
    // commit; budgeted ones on the job system collect the builds over the commits after.
    told.most = 9;
    for (u32 tries = 0; tries < 4000 && told.held.size() < 9; ++tries) {
      step(world, placements, at(0, 0), tick++, on_jobs ? 4u : 0u);
      if (on_jobs) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    step(world, placements, at(0, 0), tick++, 0);  // a whole one, so the tail is the settled one
    CHECK(told.held.size() == 9);
    CHECK(s.rebuilds == withheld);
    CHECK(s.shelved == 0);
    CHECK(s.waiting == 0);
    CHECK(s.host_bytes == 0);
    CHECK(placements.stats().mesh_waiting == 0);
    CHECK(s.failures == 0);
    CHECK(told.violations == 0);
    const Vector<std::string> settled = tail_of(told);
    REQUIRE(settled.size() == whole_room.size());
    for (u32 k = 0; k < settled.size(); ++k)
      CHECK(settled[k] == whole_room[k]);
  }
}

namespace {

// A room that also holds textures as a renderer's scene does (`GpuScene::acquire_texture`): by
// key, with a count of who holds each — the table while a mesh waits, the mesh once it is in — and
// a texture nobody holds is gone, so a mesh that lets its references go uploads again.
struct TexturedRoom : Recorder {
  struct Texture {
    u64 key = 0;
    u32 refs = 0;
  };
  struct Mesh {
    u32 mesh = 0;
    Vector<u64> keys;
  };
  Vector<Texture> textures;
  Vector<Mesh> meshes;
  u32 uploads = 0;

  static TexturedRoom* of(void* context) {
    return static_cast<TexturedRoom*>(static_cast<Recorder*>(context));
  }
  Texture* texture_of(u64 key) {
    for (Texture& t : textures)
      if (t.key == key) return &t;
    return nullptr;
  }
  void drop_texture(u64 key) {
    for (u32 k = 0; k < textures.size(); ++k) {
      if (textures[k].key != key) continue;
      if (--textures[k].refs == 0) textures.erase(textures.begin() + k);
      return;
    }
    ++violations;
  }
  static bool admit_textured(void* context, u32 mesh, const renderer::PlacementMeshData& data,
                             std::string* error) {
    TexturedRoom* self = of(context);
    if (!Recorder::admit(context, mesh, data, error)) return false;
    Mesh held;
    held.mesh = mesh;
    for (const renderer::PlacementTexture& texture : data.textures) {
      Texture* there = self->texture_of(texture.key);
      if (there == nullptr) {
        ++self->violations;  // admitted naming a texture the scene does not hold
        continue;
      }
      ++there->refs;
      held.keys.push_back(texture.key);
    }
    self->meshes.push_back(std::move(held));
    return true;
  }
  static void retire_textured(void* context, u32 mesh) {
    TexturedRoom* self = of(context);
    Recorder::retire(context, mesh);
    for (u32 k = 0; k < self->meshes.size(); ++k) {
      if (self->meshes[k].mesh != mesh) continue;
      for (const u64 key : self->meshes[k].keys)
        self->drop_texture(key);
      self->meshes.erase(self->meshes.begin() + k);
      return;
    }
  }
  static TileMeshSink::Texture texture(void* context, const renderer::PlacementTexture& wanted,
                                       std::string*) {
    TexturedRoom* self = of(context);
    if (Texture* there = self->texture_of(wanted.key)) {
      ++there->refs;
      return TileMeshSink::Texture::held;
    }
    if (wanted.data.levels.empty()) return TileMeshSink::Texture::need_bytes;
    self->textures.push_back(Texture{wanted.key, 1});
    ++self->uploads;
    return TileMeshSink::Texture::held;
  }
  static void release(void* context, u64 key) { of(context)->drop_texture(key); }
  static bool resident(void* context, u64 key) { return of(context)->texture_of(key) != nullptr; }
  TileMeshSink textured_sink() {
    return TileMeshSink{static_cast<Recorder*>(this),  &Recorder::fits,
                        &TexturedRoom::admit_textured, &TexturedRoom::retire_textured,
                        &TexturedRoom::texture,        &TexturedRoom::release,
                        &TexturedRoom::resident};
  }
};

// Two entries of the fixture's rocks: small textured rocks every 2 x 2 tiles share, dealt among
// five base colour maps, and one large untextured rock at tile (0, 0) alone.
renderer::SceneData shared_and_large_scene(const test::TempDir& tmp) {
  renderer::SceneData scene = scene_of(tmp, R"({"reach":3,"n":8,"share":2,"look":1,"sets":5})");
  renderer::StreamedPlacements large;
  large.generator = "test-tile-rocks";
  REQUIRE(parse_json(R"({"reach":0,"n":96})", large.params).ok);
  large.where = "placements 1";
  large.tile_meshes = true;
  scene.streamed.push_back(std::move(large));
  return scene;
}

}  // namespace

// W51-5b, H1 (world.md, "A withheld mesh is let go and built again"): with the room full, a tile
// nearer than a commit's first refusal fills in a bounded number of commits whatever farther tiles
// ask for. Tile (-1, 0)'s rock is shared with the tiles of its 2 x 2 group, one of them a corner
// farther than tile (0, 0), whose large rock the room can never take; the rock's base colour map
// takes a commit of its own under the allowance. Until this case a farther tile of the group
// shelved the rock the nearest tile was admitting — its texture let go — in every commit the large
// rock was refused, so the nearest tile never filled and the rock was built again every other
// commit.
TEST_CASE("tile meshes: a farther tile never shelves a mesh a nearer one is admitting") {
  test::TempDir tmp{"engine_world_tile_meshes_nearer"};
  const renderer::SceneData scene = shared_and_large_scene(tmp);
  // The rocks' clusters, from a room that holds everything.
  u32 large = 0;
  u32 small = 0;
  {
    Recorder told;
    PlacementTiles placements;
    PlacementTilesConfig config;
    config.ring_count = 1;
    std::string error;
    REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
    placements.set_meshes(room(32, 1u << 20, 0), told.sink());
    placements.set_run_sink(&Recorder::runs, &told);
    World world(ring(1.5f));
    add_scene_consumers(world, nullptr, &placements);
    for (u64 tick = 0; tick < 2; ++tick)
      step(world, placements, at(0, 0), tick);
    for (const Recorder::Held& h : told.held) {
      if (h.clusters > large) {
        small = std::max(small, large);
        large = h.clusters;
      } else {
        small = std::max(small, h.clusters);
      }
    }
  }
  REQUIRE(small > 0);
  const u32 room_clusters = 12 * small;  // every small rock in reach, and not the large one
  REQUIRE(large > room_clusters);

  TexturedRoom told;
  told.room = room_clusters;
  PlacementTiles placements;
  PlacementTilesConfig config;
  config.ring_count = 1;
  std::string error;
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  TileMeshConfig meshes = room(32, 1u << 20, 0);
  meshes.max_clusters = large;  // admissible, so refused for room rather than failed
  meshes.admissions = 0;
  meshes.admission_bytes = 1500;  // a 32 x 32 base colour map (1,392 staged bytes) fills a commit
  placements.set_meshes(meshes, told.textured_sink());
  placements.set_run_sink(&Recorder::runs, &told);
  World world(ring(1.5f));
  add_scene_consumers(world, nullptr, &placements);
  // Standing where tile (0, 0) is in reach and none of the group of (-1, 0): the large rock is
  // refused and shelved; the small ones arrive.
  u64 tick = 0;
  for (; tick < 16; ++tick)
    step(world, placements, at(1, 0), tick, 64);
  const TileMeshStats& s = placements.meshes().stats();
  REQUIRE(s.shelved >= 1);
  for (const Recorder::Held& h : told.held)
    REQUIRE(h.clusters <= small);
  // One tile to the west: tile (-1, 0) is the nearest, and its rock is new.
  u32 commits = 0;
  bool drawn = false;
  const u64 rebuilds = s.rebuilds;
  for (; commits < 40 && !drawn; ++commits) {
    step(world, placements, at(-1, 0), tick++, 64);
    drawn = !placements.tile_instances(TileCoord{-1, 0}).empty();
  }
  MESSAGE("the nearest tile filled after " << commits << " commits (drawn " << drawn
                                           << "); shelved meshes built again "
                                           << s.rebuilds - rebuilds << " times");
  CHECK(drawn);
  CHECK(commits <= 4);  // its texture, then the mesh, and a commit's slack
  CHECK(told.violations == 0);
}

TEST_CASE("tile meshes: two walks to one place hold the same tail") {
  test::TempDir tmp{"engine_world_tile_meshes_walks"};
  const renderer::SceneData scene = scene_of(tmp, R"({"reach":6,"n":10})");
  // Each walk ends at tile (1, 1) and settles there; its tail is read as what each instance is.
  const auto tail_at = [&](std::initializer_list<std::pair<f64, f64>> path, Recorder& told,
                           Vector<std::string>& out) {
    PlacementTiles placements;
    PlacementTilesConfig config;
    config.ring_count = 1;
    std::string error;
    REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
    placements.set_meshes(room(64, 1u << 20, 3), told.sink());
    placements.set_run_sink(&Recorder::runs, &told);
    World world(ring(1.5f));
    add_scene_consumers(world, nullptr, &placements);
    u64 tick = 0;
    for (const auto& [x, z] : path)
      step(world, placements, at(x, z), tick++);
    for (u32 k = 0; k < 6; ++k)
      step(world, placements, at(1, 1), tick++);
    // Each instance as what it is: the recipe its mesh was built from, and where it stands.
    out.clear();
    for (const renderer::SceneInstance& instance : told.tail) {
      const Recorder::Held* mesh = told.find(instance.mesh);
      REQUIRE(mesh != nullptr);
      out.push_back(mesh->path + " " + std::to_string(instance.origin.x) + " " +
                    std::to_string(instance.origin.z) + " " +
                    std::to_string(instance.transform.scale.x));
    }
    CHECK(told.held.size() == 9);  // the nine tiles' rocks, and nothing that lingered
  };
  Recorder a;
  Recorder b;
  Vector<std::string> first;
  Vector<std::string> second;
  tail_at({{-3.0, 0.0}, {-2.0, 0.0}, {-1.0, 0.0}, {0.0, 1.0}, {1.0, 1.0}}, a, first);
  tail_at({{4.0, 4.0}, {3.0, 3.0}, {2.0, 2.0}, {1.0, 1.0}}, b, second);
  REQUIRE(first.size() == 18);
  REQUIRE(second.size() == first.size());
  for (u32 k = 0; k < first.size(); ++k)
    CHECK(first[k] == second[k]);
  // The walks gave their rocks different slots; nothing the tail says depends on which.
  CHECK(a.log != b.log);
  CHECK(a.violations + b.violations == 0);
}

TEST_CASE("tile meshes: built on the job system, a tile waits and no tail names a missing mesh") {
  test::TempDir tmp{"engine_world_tile_meshes_jobs"};
  const renderer::SceneData scene = scene_of(tmp, R"({"reach":6,"n":16})");
  jobs::JobSystem pool(jobs::JobSystemConfig{.performance_workers = 2, .pin_threads = false});
  Recorder told;
  PlacementTiles placements;
  PlacementTilesConfig config;
  config.ring_count = 1;
  std::string error;
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  TileMeshConfig meshes = room(64, 1u << 20, 2);
  meshes.admissions = 1;  // one a commit: the walk's tiles arrive over several
  placements.set_meshes(meshes, told.sink(), &pool);
  placements.set_run_sink(&Recorder::runs, &told);
  World world(ring(1.5f));
  add_scene_consumers(world, nullptr, &placements);
  u64 tick = 0;
  u32 most_a_commit = 0;
  u32 waited = 0;
  for (const f64 x : {0.0, 1.0, 2.0, 3.0}) {
    // Budgeted updates until the tiles round `x` are all in: each admits at most one mesh.
    for (u32 tries = 0; tries < 4000; ++tries) {
      const u32 had = told.admissions;
      step(world, placements, at(x, 0), tick++, 4);
      most_a_commit = told.admissions - had > most_a_commit ? told.admissions - had : most_a_commit;
      waited += placements.stats().mesh_waiting != 0 ? 1u : 0u;
      if (placements.stats().mesh_waiting == 0 && placements.stats().pending == 0 &&
          placements.meshes().stats().building == 0 && placements.meshes().stats().waiting == 0) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(placements.stats().mesh_waiting == 0);
    u32 drawn = 0;
    for (i32 z = -1; z <= 1; ++z) {
      for (i32 dx = -1; dx <= 1; ++dx)
        drawn += placements.tile_instances(TileCoord{static_cast<i32>(x) + dx, z}).size() == 2;
    }
    CHECK(drawn == 9);
  }
  CHECK(most_a_commit == 1);  // the allowance
  CHECK(waited > 0);          // some commit had a tile waiting, drawn as it was
  CHECK(told.violations == 0);
  CHECK(placements.meshes().stats().failures == 0);
  // A whole update is whole: it waits for its builds and admits them all.
  step(world, placements, at(8, 0), tick++, 0);  // past the entry's reach: every tile leaves
  step(world, placements, at(-4, 0), tick++, 0);
  CHECK(placements.stats().mesh_waiting == 0);
  CHECK(placements.meshes().stats().building == 0);
  u32 drawn = 0;
  for (i32 z = -1; z <= 1; ++z) {
    for (i32 dx = -1; dx <= 1; ++dx)
      drawn += placements.tile_instances(TileCoord{-4 + dx, z}).size() == 2;
  }
  CHECK(drawn == 9);
  CHECK(told.violations == 0);
}

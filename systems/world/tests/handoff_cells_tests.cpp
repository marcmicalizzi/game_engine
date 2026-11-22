// The pairs a hand-off placement holds (G205; world.md, "A hand-off placement is held by its
// band"): the placements consumer hands a tile's near-role placements on by cells, and only the
// cells the hand-off's band can reach from an eye. A placement generator of this test's own that
// places every patch as a near form and a far form, and the procedural grid's DAG as its mesh: no
// capability, so it runs in every configuration with the world. The last case adds the tile rocks
// of tests/support/tile_mesh_fixture.h, so the mesh room yields tiles that hold near placements
// (T214).
#include <core/json/json.h>
#include <core/math/world.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>
#include <systems/world/placement_tiles.h>
#include <systems/world/scene_consumers.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <fstream>
#include <string>
#include <tile_mesh_fixture.h>
#include <unordered_map>
#include <unordered_set>

using namespace engine;
using namespace engine::world;

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

constexpr f32 k_patch_scale = 0.05f;  // the grid is 20 m across: a patch of a metre
constexpr u8 k_tag_near = 1;
constexpr u8 k_tag_far = 2;

// A patch every metre over the tile, each placed as its near form and then its far form, in every
// ring: what a hand-authored scatter with a near form is to the consumer.
struct Patches {
  std::string path;
};
bool patches_open(const JsonValue& params, const scene_gen::Context&, void** state,
                  std::string* error) {
  const JsonValue* path = params.find("path");
  std::string_view text;
  if (path == nullptr || !path->get_string(text)) {
    if (error != nullptr) *error = "handoff-patches needs a path";
    return false;
  }
  auto* p = new Patches;
  p->path = std::string(text);
  *state = p;
  return true;
}
void patches_close(void* state) noexcept { delete static_cast<Patches*>(state); }
void patches_place(f64 x0, f64 z0, f64 size, scene_gen::Placements& out) {
  const i32 n = static_cast<i32>(size);
  for (i32 j = 0; j < n; ++j) {
    for (i32 i = 0; i < n; ++i) {
      scene_gen::Placement p;
      p.mesh = 0;
      p.position = WorldPos{x0 + static_cast<f64>(i) + 0.5, 0.0, z0 + static_cast<f64>(j) + 0.5};
      p.scale = Vec3{k_patch_scale, k_patch_scale, k_patch_scale};
      p.handoff = scene_gen::k_handoff_near;
      p.tag = k_tag_near;
      out.instances.push_back(p);
      p.handoff = scene_gen::k_handoff_far;
      p.tag = k_tag_far;
      out.instances.push_back(p);
    }
  }
}
bool patches_expand(void* state, const scene_gen::Context&, scene_gen::Placements& out,
                    std::string*) {
  out.meshes.push_back(
      scene_gen::PlacementMesh{static_cast<const Patches*>(state)->path, "handoff-patch"});
  patches_place(0.0, 0.0, 4.0, out);
  return true;
}
bool patches_meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
                    std::string*) {
  out.push_back(
      scene_gen::PlacementMesh{static_cast<const Patches*>(state)->path, "handoff-patch"});
  return true;
}
bool patches_tile(void*, scene_gen::TileCoord tile, const scene_gen::Context& context,
                  scene_gen::Placements& out, std::string*) {
  const f64 t = static_cast<f64>(context.tile_size);
  patches_place(t * tile.x, t * tile.z, t, out);
  return true;
}
u8 patches_representation(const void*, const scene_gen::Context&) noexcept { return 0; }
constexpr scene_gen::PlacementGeneratorDesc k_patches{.name = "handoff-patches",
                                                      .open = &patches_open,
                                                      .close = &patches_close,
                                                      .expand = &patches_expand,
                                                      .meshes = &patches_meshes,
                                                      .tile = &patches_tile,
                                                      .representation = &patches_representation};
const scene_gen::Registrar k_patches_registrar{k_patches};

std::string json_path(std::string path) {
  for (char& c : path)
    if (c == '\\') c = '/';
  return path;
}

TileEvent arrival(i32 x, i32 z) {
  TileEvent e;
  e.tile = TileCoord{x, z};
  e.kind = TileEventKind::Activate;
  e.from = k_inactive;
  e.to = 0;
  return e;
}

// The scene: the patches over tiles of 32 m, the grid's DAG as their mesh.
void make_scene(const test::TempDir& tmp, renderer::SceneData& scene) {
  std::string error;
  {
    renderer::SceneDesc grid;
    grid.meshes.push_back("");
    grid.heightfield_grid = 33;
    grid.cache = false;
    renderer::SceneData made;
    REQUIRE_MESSAGE(renderer::load_scene(grid, made, error), error);
    geometry::ClusterFileData file;
    file.mesh = std::move(made.lod);
    REQUIRE_MESSAGE(geometry::write_cluster_file(tmp.file("patch.clusters"), file, &error), error);
  }
  const std::string text =
      std::string(R"({"format":"engine.scene.v1","name":"handoff-cells",)") +
      R"("placements":[{"generator":"handoff-patches","params":{"path":")" +
      json_path(tmp.file("patch.clusters")) +
      R"("}}],"world":{"tile_size":32,"rings":[{"radius":2.5},{"radius":6}]},)" +
      R"("terrain":{"size":17,"extent":40,"seed":3,"dune_height":0.0}})";
  REQUIRE(write_text(tmp.file("scene.json"), text));
  renderer::SceneDesc desc;
  REQUIRE_MESSAGE(renderer::read_scene_file(tmp.file("scene.json"), desc, error), error);
  desc.cache = false;
  REQUIRE_MESSAGE(renderer::load_scene(desc, scene, error), error);
  REQUIRE(scene.streamed.size() == 1u);
}

// What the cull pass can find of a placement of `mesh` at scale 1: its clusters' spheres' reach
// from its origin (the mesh does not sway, so nothing pads them).
f32 mesh_reach(const renderer::SceneData& scene, u32 mesh) {
  const geometry::ClusterMeshPart& part = scene.parts[mesh];
  f32 reach = 0.0f;
  for (u32 i = part.first_cluster; i < part.first_cluster + part.cluster_count; ++i) {
    const geometry::ClusterDesc& c = scene.lod.mesh.clusters[i];
    reach = std::max(reach, length(c.center) + c.radius);
  }
  return reach;
}

u64 place_key(const renderer::SceneInstance& instance) {
  const i64 x = static_cast<i64>(std::floor(instance.origin.x * 2.0));
  const i64 z = static_cast<i64>(std::floor(instance.origin.z * 2.0));
  return (static_cast<u64>(x) << 32) ^ (static_cast<u64>(z) & 0xffffffffu);
}

f64 ground_distance(const renderer::SceneInstance& instance, const WorldPos& eye) {
  const f64 dx = instance.origin.x - eye.x;
  const f64 dz = instance.origin.z - eye.z;
  return std::sqrt(dx * dx + dz * dz);
}

bool take_runs(void*, std::span<const PlacementTiles::TileRange>, std::string*) { return true; }

PlacementTilesConfig cells_config() {
  PlacementTilesConfig config;
  config.tile_size = 32.0f;
  config.handoff_to_m = 10.0f;
  config.handoff_cell_m = 4.0f;
  config.handoff_margin_m = 1.0f;
  config.handoff_hold_m = 2.0f;
  config.handoff_lead_updates = 2;
  return config;
}

}  // namespace

TEST_CASE(
    "handoff cells: a tile's near placements are handed where the band reaches, and hold "
    "pairs only there") {
  const test::TempDir tmp("world_handoff_cells");
  REQUIRE(tmp.ok());
  renderer::SceneData scene;
  make_scene(tmp, scene);
  std::string error;
  const u32 mesh = scene.streamed[0].meshes[0];
  const f64 reach = static_cast<f64>(k_patch_scale * mesh_reach(scene, mesh));
  const PlacementTilesConfig config = cells_config();
  const f64 to = static_cast<f64>(config.handoff_to_m);

  // The same nine tiles in the inner ring, handed by cells and handed whole.
  PlacementTiles cells;
  REQUIRE_MESSAGE(cells.create(scene, config, &error), error);
  cells.set_run_sink(&take_runs, nullptr);
  PlacementTilesConfig whole_config = config;
  whole_config.handoff_cell_m = 0.0f;
  PlacementTiles whole;
  REQUIRE_MESSAGE(whole.create(scene, whole_config, &error), error);
  whole.set_run_sink(&take_runs, nullptr);
  const WorldPos eye{16.0, 1.7, 16.0};
  cells.set_handoff_eyes(std::span<const WorldPos>(&eye, 1));
  whole.set_handoff_eyes(std::span<const WorldPos>(&eye, 1));
  for (PlacementTiles* tiles : {&cells, &whole}) {
    const TileConsumer consumer = tiles->consumer();
    tiles->begin_update(0);
    for (i32 z = -1; z <= 1; ++z)
      for (i32 x = -1; x <= 1; ++x)
        REQUIRE(consumer.activate(consumer.context, arrival(x, z)));
    consumer.commit(consumer.context);
    tiles->end_update();
    // A second update with the eye where it was: the lead of an eye first seen is gone.
    tiles->begin_update(0);
    tiles->end_update();
  }
  const std::span<const renderer::SceneInstance> all = whole.tail();
  const std::span<const renderer::SceneInstance> handed = cells.tail();
  REQUIRE(all.size() == 9u * 2u * 1024u);
  // Handed whole, a tile is one run of everything on it, as it always was.
  CHECK(whole.runs().size() == 9u);
  CHECK(whole.stats().handoff_cells == 0u);

  // Every far form is handed, and every near form the cull pass could draw: one whose reach comes
  // within the band's end of the eye. None is handed twice.
  std::unordered_set<u64> near_handed;
  u32 far_handed = 0;
  f64 farthest = 0.0;
  for (const renderer::SceneInstance& instance : handed) {
    if (instance.handoff == scene_gen::k_handoff_near) {
      CHECK(near_handed.insert(place_key(instance)).second);
      farthest = std::max(farthest, ground_distance(instance, eye) - reach);
    } else {
      ++far_handed;
    }
  }
  CHECK(far_handed == 9u * 1024u);
  u32 drawable = 0;
  u32 missing = 0;
  for (const renderer::SceneInstance& instance : all) {
    if (instance.handoff != scene_gen::k_handoff_near) continue;
    if (ground_distance(instance, eye) - reach >= to) continue;
    ++drawable;
    missing += near_handed.count(place_key(instance)) == 0 ? 1u : 0u;
  }
  CHECK(missing == 0u);
  CHECK(drawable > 0u);
  // And no more than the band, the margin, the hold and a cell with its patches' reach past it.
  const f64 slack = static_cast<f64>(config.handoff_margin_m + config.handoff_hold_m) +
                    static_cast<f64>(config.handoff_cell_m) * 1.4143 + 2.0 * reach;
  CHECK(farthest <= to + slack);
  CHECK(near_handed.size() < 9u * 1024u / 4u);

  // The pairs follow: the near forms out of reach hold none.
  const u32 clusters = scene.parts[mesh].cluster_count;
  CHECK(whole.stats().ring_pairs[0] == 9u * 2u * 1024u * clusters);
  CHECK(cells.stats().ring_pairs[0] ==
        (9u * 1024u + static_cast<u32>(near_handed.size())) * clusters);
  CHECK(cells.stats().pairs == cells.stats().ring_pairs[0]);
  CHECK(cells.stats().handoff_instances == 9u * 1024u);
  CHECK(cells.stats().handoff_pairs_out ==
        u64{9u * 1024u - static_cast<u32>(near_handed.size())} * clusters);
  CHECK(cells.stats().handoff_cells == 9u * 64u);
  MESSAGE("nine tiles of 1,024 patches, a band to "
          << to << " m: " << drawable << " near forms the cull could draw, " << near_handed.size()
          << " handed in " << cells.stats().handoff_cells_in << " of "
          << cells.stats().handoff_cells << " cells; " << cells.stats().ring_pairs[0]
          << " pairs against " << whole.stats().ring_pairs[0]);

  // The runs: a tile's own run under its key, then its cells that are in, each under a key no
  // tile has and naming its tile; they cover the tail in order, and the tags go with it.
  const std::span<const PlacementTiles::TileRange> runs = cells.runs();
  std::unordered_set<u64> keys;
  u32 next = 0;
  u32 cell_runs = 0;
  for (const PlacementTiles::TileRange& run : runs) {
    CHECK(keys.insert(run.key).second);
    CHECK(run.first == next);
    CHECK(run.version != 0u);
    REQUIRE(run.instances != nullptr);
    next += run.count;
    const bool cell = PlacementTiles::cell_key_reserved(run.key);
    cell_runs += cell ? 1u : 0u;
    CHECK((run.part_of != 0u) == cell);
    for (u32 i = 0; i < run.count; ++i) {
      CHECK((run.instances[i].handoff == scene_gen::k_handoff_near) == cell);
      CHECK(place_key(run.instances[i]) == place_key(handed[run.first + i]));
    }
  }
  CHECK(next == handed.size());
  CHECK(cell_runs == cells.stats().handoff_cells_in);
  CHECK(runs.size() == 9u + cell_runs);
  const std::span<const u8> tags = cells.tail_tags();
  REQUIRE(tags.size() == handed.size());
  for (u32 i = 0; i < handed.size(); ++i)
    CHECK(tags[i] == (handed[i].handoff == scene_gen::k_handoff_near ? k_tag_near : k_tag_far));
  // A tile still holds everything placed on it, and a range of it covers what it hands.
  CHECK(cells.tile_instances(TileCoord{0, 0}).size() == 2u * 1024u);
  Vector<PlacementTiles::TileRange> ranges;
  cells.tile_ranges(ranges);
  REQUIRE(ranges.size() == 9u);
  u32 covered = 0;
  for (const PlacementTiles::TileRange& range : ranges) {
    CHECK(range.first == covered);
    covered += range.count;
  }
  CHECK(covered == handed.size());
  // `tile_own` (S112-3, the second review's L-a; world_sound's canopy table reads it): each tile's
  // own placements and the version they were handed under, the hand-off's near forms left out —
  // exactly the tile's own run, and never a version of 0.
  Vector<PlacementTiles::TileOwn> own;
  cells.tile_own(own);
  CHECK(own.size() == 9u);
  for (const PlacementTiles::TileOwn& t : own) {
    const PlacementTiles::TileRange* match = nullptr;
    for (const PlacementTiles::TileRange& run : runs)
      match = run.key == t.key ? &run : match;
    REQUIRE(match != nullptr);
    CHECK_FALSE(PlacementTiles::cell_key_reserved(t.key));
    CHECK(t.count == match->count);
    CHECK(t.version == match->version);
    CHECK(t.version != 0u);
    REQUIRE(t.instances != nullptr);
    for (u32 i = 0; i < t.count; ++i) {
      CHECK(t.instances[i].handoff != scene_gen::k_handoff_near);
      CHECK(place_key(t.instances[i]) == place_key(match->instances[i]));
    }
  }
}

TEST_CASE(
    "handoff cells: along a walk every near form the band reaches is handed, and nothing "
    "else of a tile is handed again") {
  const test::TempDir tmp("world_handoff_walk");
  REQUIRE(tmp.ok());
  renderer::SceneData scene;
  make_scene(tmp, scene);
  std::string error;
  const u32 mesh = scene.streamed[0].meshes[0];
  const f64 reach = static_cast<f64>(k_patch_scale * mesh_reach(scene, mesh));
  const PlacementTilesConfig config = cells_config();
  const f64 to = static_cast<f64>(config.handoff_to_m);
  PlacementTiles cells;
  REQUIRE_MESSAGE(cells.create(scene, config, &error), error);
  cells.set_run_sink(&take_runs, nullptr);
  PlacementTilesConfig whole_config = config;
  whole_config.handoff_cell_m = 0.0f;
  PlacementTiles whole;
  REQUIRE_MESSAGE(whole.create(scene, whole_config, &error), error);
  whole.set_run_sink(&take_runs, nullptr);
  WorldPos eye{16.0, 1.7, 16.0};
  cells.set_handoff_eyes(std::span<const WorldPos>(&eye, 1));
  for (PlacementTiles* tiles : {&cells, &whole}) {
    const TileConsumer consumer = tiles->consumer();
    tiles->begin_update(0);
    for (i32 z = -1; z <= 1; ++z)
      for (i32 x = -1; x <= 3; ++x)
        REQUIRE(consumer.activate(consumer.context, arrival(x, z)));
    consumer.commit(consumer.context);
    tiles->end_update();
  }
  const std::span<const renderer::SceneInstance> all = whole.tail();
  // The versions the first hand-over named: a tile's own run keeps its version for the whole
  // walk (the renderer reads none of it again), and a cell's while it is handed.
  std::unordered_map<u64, u64> versions;
  for (const PlacementTiles::TileRange& run : cells.runs())
    versions[run.key] = run.version;
  const u64 handed_before = cells.stats().handed;
  const u64 built_before = cells.stats().tiles_built;
  u32 missing = 0;
  u32 most = 0;
  u32 fewest = ~0u;
  u32 came = 0;
  std::unordered_set<u64> near_handed;
  std::unordered_set<u64> seen;
  // From rest to 0.5 m an update (a run at 60 Hz is 0.15), east for 60 m, a stop, and back.
  f64 speed = 0.0;
  for (u32 update = 0; update < 300; ++update) {
    speed = update < 120 ? 0.5 : (update < 140 ? 0.0 : -0.5);
    eye.x += speed;
    cells.set_handoff_eyes(std::span<const WorldPos>(&eye, 1));
    cells.begin_update(0);
    cells.end_update();
    near_handed.clear();
    seen.clear();
    for (const PlacementTiles::TileRange& run : cells.runs()) {
      seen.insert(run.key);
      const auto held = versions.find(run.key);
      if (held == versions.end()) {
        versions[run.key] = run.version;
        ++came;
      } else {
        CHECK(held->second == run.version);
      }
      if (!PlacementTiles::cell_key_reserved(run.key)) continue;
      for (u32 i = 0; i < run.count; ++i)
        near_handed.insert(place_key(run.instances[i]));
    }
    // A cell that went may come back under a new key; a tile's own run never goes.
    for (auto it = versions.begin(); it != versions.end();) {
      if (seen.count(it->first) != 0) {
        ++it;
        continue;
      }
      CHECK(PlacementTiles::cell_key_reserved(it->first));
      it = versions.erase(it);
    }
    for (const renderer::SceneInstance& instance : all) {
      if (instance.handoff != scene_gen::k_handoff_near) continue;
      if (ground_distance(instance, eye) - reach >= to) continue;
      missing += near_handed.count(place_key(instance)) == 0 ? 1u : 0u;
    }
    const u32 held_now = static_cast<u32>(near_handed.size());
    most = held_now > most ? held_now : most;
    fewest = held_now < fewest ? held_now : fewest;
  }
  CHECK(missing == 0u);
  CHECK(cells.stats().tiles_built == built_before);  // no generator asked: the tiles keep them
  CHECK(most < 15u * 1024u / 4u);
  CHECK(came > 0u);
  // A commit only when a cell came or went: fewer than the updates, none while the eye stood.
  const u64 commits = cells.stats().handed - handed_before;
  CHECK(commits < 300u);
  CHECK(commits > 0u);
  MESSAGE("a 60 m walk and back, 300 updates: "
          << commits << " commits, " << came << " cell arrivals, " << cells.stats().handoff_flips
          << " flips, " << fewest << " to " << most << " near forms handed of 15,360; "
          << cells.stats().handoff_ns / 1000 << " us in the cells over the run");
}

TEST_CASE("handoff cells: with no eye named every cell is handed, and with no band none is made") {
  const test::TempDir tmp("world_handoff_off");
  REQUIRE(tmp.ok());
  renderer::SceneData scene;
  make_scene(tmp, scene);
  std::string error;
  // No eye and no ring: the consumer cannot say where the band is, so it hands every cell.
  PlacementTiles blind;
  REQUIRE_MESSAGE(blind.create(scene, cells_config(), &error), error);
  blind.set_run_sink(&take_runs, nullptr);
  TileConsumer consumer = blind.consumer();
  REQUIRE(consumer.activate(consumer.context, arrival(0, 0)));
  consumer.commit(consumer.context);
  CHECK(blind.tail().size() == 2u * 1024u);
  CHECK(blind.stats().handoff_cells == 64u);
  CHECK(blind.stats().handoff_cells_in == 64u);
  CHECK(blind.stats().handoff_pairs_out == 0u);
  // No band (the host named none): the tile is one run in the generator's order, as before.
  PlacementTilesConfig none = cells_config();
  none.handoff_to_m = 0.0f;
  PlacementTiles plain;
  REQUIRE_MESSAGE(plain.create(scene, none, &error), error);
  plain.set_run_sink(&take_runs, nullptr);
  consumer = plain.consumer();
  REQUIRE(consumer.activate(consumer.context, arrival(0, 0)));
  consumer.commit(consumer.context);
  REQUIRE(plain.runs().size() == 1u);
  const std::span<const renderer::SceneInstance> held = plain.tile_instances(TileCoord{0, 0});
  REQUIRE(held.size() == 2u * 1024u);
  for (u32 i = 0; i < held.size(); ++i)
    CHECK(held[i].handoff == (i % 2u == 0 ? scene_gen::k_handoff_near : scene_gen::k_handoff_far));
  CHECK(plain.stats().handoff_cells == 0u);
}

namespace {

// A room of three meshes, whatever their clusters, for the tile rocks.
struct ThreeRocks {
  u32 held = 0;
  static bool fits(void* context, u32, u32) { return static_cast<ThreeRocks*>(context)->held < 3; }
  static bool admit(void* context, u32, const renderer::PlacementMeshData&, std::string*) {
    ++static_cast<ThreeRocks*>(context)->held;
    return true;
  }
  static void retire(void* context, u32) { --static_cast<ThreeRocks*>(context)->held; }
};

}  // namespace

// T214 (the owner's walk, 2026-11-22: an access violation in engine-view's world sound): a tile the
// mesh room yields (`yield_farthest`) draws nothing, and it held near placements in cells. Until
// the mend its hand-off state outlived the yield, and `tile_own` listed the tile with
// `0 - handoff.count` instances — about four billion — which the sound's table walked off the end
// of mapped memory. Every step of a walk holds `tile_own` to its contract, and the stats and the
// tail to the runs.
TEST_CASE("handoff cells: a tile the mesh room yields lists nothing of its own and holds no cell") {
  const test::TempDir tmp("world_handoff_yield");
  REQUIRE(tmp.ok());
  renderer::SceneData scene;
  make_scene(tmp, scene);
  // Beside the patches (near placements of a resident mesh on every tile), tiles that hand their
  // own rock: so a tile names a mesh in the room, and the room's refusal yields drawn tiles.
  test::register_tile_rocks();
  renderer::StreamedPlacements rocks;
  rocks.generator = "test-tile-rocks";
  REQUIRE(parse_json(R"({"reach":6,"n":10})", rocks.params).ok);
  rocks.where = "placements 1";
  rocks.tile_meshes = true;
  scene.streamed.push_back(std::move(rocks));
  scene.tile_meshes = true;
  scene.dynamic = true;
  scene.mesh_options.ddc = tmp.file("ddc");
  scene.mesh_options.cache = true;

  std::string error;
  PlacementTilesConfig config = cells_config();
  config.ring_count = 1;
  PlacementTiles placements;
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  ThreeRocks room;
  TileMeshConfig meshes;
  meshes.first_mesh = static_cast<u32>(scene.parts.size());
  meshes.slots = 32;
  meshes.clusters = 1u << 20;
  meshes.max_clusters = 4096;
  meshes.cluster_bytes = 3000;
  meshes.linger = 0;
  placements.set_meshes(meshes, TileMeshSink{&room, &ThreeRocks::fits, &ThreeRocks::admit,
                                             &ThreeRocks::retire, nullptr, nullptr, nullptr});
  placements.set_run_sink(&take_runs, nullptr);
  RingParams ring;
  ring.ring_count = 1;
  ring.radius[0] = 1.5f;  // nine tiles round the observer, room for three tiles' rocks
  ring.max_activations = 0;
  ring.max_deactivations = 0;
  World world(ring);
  add_scene_consumers(world, nullptr, &placements);

  // The tiles drawing near placements before a step, by key.
  std::unordered_set<u64> drew_near;
  u32 near_yields = 0;  // tiles that drew near placements, are still in the ring, and draw nothing
  u64 tick = 0;
  for (const f64 x : {0.0, 1.0, 2.0, 3.0, 2.0, 1.0, 0.0}) {
    for (u32 repeat = 0; repeat < 3; ++repeat) {
      const u64 yielded = placements.stats().mesh_yielded;
      sim::ObserverSet observers;
      observers.add(WorldPos{(x + 0.5) * 32.0, 0.0, 16.0}, 1.0f);
      placements.begin_update(0);
      world.update(observers, tick++);
      (void)placements.end_update();

      // `tile_own`: each tile listed draws something of its own, `count` of what it draws, none of
      // them a near placement its cells hold.
      Vector<PlacementTiles::TileOwn> own;
      placements.tile_own(own);
      for (const PlacementTiles::TileOwn& t : own) {
        const std::span<const renderer::SceneInstance> drawn =
            placements.tile_instances(tile_of_key(t.key));
        INFO("tile key " << t.key << " at x " << x);
        REQUIRE(t.count <= drawn.size());
        REQUIRE(t.count != 0u);
        REQUIRE(t.instances != nullptr);
        CHECK(t.instances == drawn.data());
        CHECK(t.version != 0u);
        for (u32 i = 0; i < t.count; ++i)
          CHECK(t.instances[i].handoff != scene_gen::k_handoff_near);
      }
      // The stats count what the runs hand, and the tail is what they hand: a yielded tile's cells
      // are neither (it reserved and counted pairs for cells it no longer drew, and the tail of a
      // commit with no run sink walked `0 - count` of its instances).
      u32 handed = 0;
      for (const PlacementTiles::TileRange& run : placements.runs())
        handed += run.count;
      REQUIRE(placements.stats().instances == handed);
      CHECK(placements.tail().size() == handed);

      // Which yields took a tile that drew near placements.
      if (placements.stats().mesh_yielded != yielded) {
        for (const u64 key : drew_near) {
          const TileCoord t = tile_of_key(key);
          const f64 dx = static_cast<f64>(t.x) - x;
          const f64 dz = static_cast<f64>(t.z);
          if (dx * dx + dz * dz > 1.5 * 1.5) continue;  // let go by the ring, not yielded
          near_yields += placements.tile_instances(t).empty() ? 1u : 0u;
        }
      }
      drew_near.clear();
      for (i32 z = -2; z <= 2; ++z) {
        for (i32 tx = -2; tx <= 6; ++tx) {
          for (const renderer::SceneInstance& instance :
               placements.tile_instances(TileCoord{tx, z})) {
            if (instance.handoff != scene_gen::k_handoff_near) continue;
            drew_near.insert(tile_key(TileCoord{tx, z}));
            break;
          }
        }
      }
    }
  }
  CHECK(placements.stats().mesh_yielded > 0u);
  CHECK(near_yields > 0u);
  MESSAGE("a walk at a room of three rocks: " << placements.stats().mesh_yielded << " yields, "
                                              << near_yields
                                              << " of them tiles that drew near placements");
}

// The far forest (renderer.md, "The far forest"; ADR-0059; roadmap R111): a streamed world's own
// trees past what the ring's tail draws, merged per world tile into prefix meshes of their coarsest
// forms. On the CPU: the tile key is the world's, the hand-off draws exactly the tiles the tail
// does not, and a streamed load builds one far tile per occupied tile whose cache entries are the
// same bytes at any job count and are read back by a second load. On a GPU: a camera outside the
// ring sees the trees past it, a tile the tail draws is never drawn by the far forest too, and a
// parked far tile draws nothing.
//
// The load half needs the flora capability (the trees) and the authored generator, as
// authored_scene_tests.cpp does; without them only the hand-off's cases run.
#include "shadow_fixture.h"

#include <core/json/json.h>
#include <domain/gfx/device.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/far_forest.h>
#include <systems/renderer/far_forest_handoff.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

TEST_CASE("far forest: a far tile's key is the world's tile key") {
  // `world::tile_key` (systems/world/tile_ring.h), spelled out: each coordinate's sign bit flipped,
  // x in the high word. The tail's blocks are named by it.
  CHECK(far_tile_key(0, 0) == 0x8000000080000000ull);
  CHECK(far_tile_key(-1, 2) == 0x7FFFFFFF80000002ull);
  CHECK(far_tile_key(3, -4) == 0x800000037FFFFFFCull);
  CHECK(far_tile_key(-1, 0) < far_tile_key(0, 0));
}

TEST_CASE("far forest: the hand-off draws every tile the tail does not, and only those") {
  FarForestData data;
  data.tile_size = 32.0f;
  for (i32 x = 0; x < 3; ++x) {
    FarForestTile tile;
    tile.x = x;
    tile.z = -1;
    tile.key = far_tile_key(x, -1);
    tile.mesh = 10 + static_cast<u32>(x);
    tile.instance = 5 + static_cast<u32>(x);
    tile.corner = WorldPos{32.0 * x, 0.0, -32.0};
    data.tiles.push_back(tile);
  }
  FarForestHandOff handoff;
  handoff.reset(&data);
  REQUIRE(handoff.active());
  CHECK(handoff.drawn_count() == 3);
  // The tail holds tile 0 with instances, tile 1 with none (a tile the world holds with nothing on
  // it), and a tile with no far trees: only tile 0 is the tail's.
  const DynamicBlock blocks[] = {{far_tile_key(0, -1), 0, 4, 2},
                                 {far_tile_key(1, -1), 4, 0, 2},
                                 {far_tile_key(7, 7), 4, 3, 1}};
  REQUIRE(handoff.hand_over(std::span<const DynamicBlock>(blocks, 3)) == 1);
  CHECK(handoff.indices()[0] == 5);
  CHECK(handoff.placements()[0].mesh == 10);
  CHECK(handoff.placements()[0].origin.y == k_far_park_depth_m);
  CHECK(handoff.placements()[0].origin.x == 0.0);
  handoff.commit();
  CHECK(handoff.drawn_count() == 2);
  CHECK_FALSE(handoff.drawn(0));
  CHECK(handoff.drawn(1));
  // The same tail again moves nothing; a tail without it puts tile 0 back at its corner.
  CHECK(handoff.hand_over(std::span<const DynamicBlock>(blocks, 3)) == 0);
  REQUIRE(handoff.hand_over(std::span<const DynamicBlock>(blocks + 1, 2)) == 1);
  CHECK(handoff.placements()[0].origin.y == 0.0);
  CHECK(handoff.placements()[0].origin.z == -32.0);
  // A refused move is not committed: the drawn state stays the old one.
  CHECK_FALSE(handoff.drawn(0));
  handoff.commit();
  CHECK(handoff.drawn(0));
  CHECK(handoff.drawn_count() == 3);
  // An empty forest is no hand-off at all.
  FarForestData empty;
  FarForestHandOff none;
  none.reset(&empty);
  CHECK_FALSE(none.active());
  CHECK(none.hand_over(std::span<const DynamicBlock>(blocks, 3)) == 0);
}

#if ENGINE_RENDERER_TESTS_FLORA

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// authored_scene_tests.cpp's trees: a small broadleaf with its card levels and its star, and a
// spruce with a decimated chain only; sixty of each over 160 m, tiles of 32 m in three rings.
constexpr const char* k_oak =
    R"({"species":"broadleaf","broadleaf":{"seed":3,"height_m":8,"trunk_radius_m":0.25,)"
    R"("triangle_budget":8000,"card_bake_px":16,"impostor_px":32}})";
constexpr const char* k_spruce =
    R"({"species":"conifer","conifer":{"seed":5,"height_m":6,"trunk_radius_m":0.1,)"
    R"("whorls":6,"branches_per_whorl":4,"crown_radius_m":1.5,"triangle_budget":8000}})";
constexpr const char* k_scene =
    R"({"format":"engine.scene.v1","name":"far-forest-test",)"
    R"("terrain":{"size":65,"extent":256,"seed":7,"dune_height":0.2,"dune_wavelength":40,)"
    R"("generator":"Waves"},)"
    R"("meshes":[{"name":"oak","path":"oak.flora.json"},{"name":"spruce","path":"spruce.flora.json"}],)"
    R"("scatters":[{"mesh":0,"center":[0,0,0],"radius_min":5,"radius_max":80,"count":60,"seed":11,)"
    R"("scale_min":0.8,"scale_max":1.2},)"
    R"({"mesh":1,"center":[0,0,0],"radius_min":5,"radius_max":80,"count":60,"seed":12}],)"
    R"("world":{"tile_size":32,"rings":[{"radius":1.5},{"radius":4},{"radius":9}]}})";

bool generator_present() {
  return scene_gen::GeneratorRegistry::global().find_placement("authored") != nullptr;
}

bool prepare(const test::TempDir& tmp) {
  return write_text(tmp.file("oak.flora.json"), k_oak) &&
         write_text(tmp.file("spruce.flora.json"), k_spruce) &&
         write_text(tmp.file("scene.json"), k_scene);
}

bool load(const test::TempDir& tmp, const std::string& ddc, bool far_forest, u32 jobs,
          SceneDesc& desc, SceneData& data, std::string& error) {
  if (!read_scene_file(tmp.file("scene.json"), desc, error)) return false;
  desc.ddc = ddc;
  desc.cache = true;
  desc.load_jobs = jobs;
  desc.authored.far_forest = far_forest;
  return load_scene(desc, data, error);
}

// Every far tile entry under `ddc`, by name, with its bytes.
void entries(const std::string& ddc, Vector<std::pair<std::string, std::string>>& out) {
  out.clear();
  Vector<io::DirEntry> list;
  if (io::list_directory(io::join_path(ddc, "far_forest"), list) != io::Status::Ok) return;
  for (const io::DirEntry& e : list) {
    std::string bytes;
    REQUIRE(io::read_file(io::join_path(io::join_path(ddc, "far_forest"), e.name), bytes) ==
            io::Status::Ok);
    out.push_back({e.name, std::move(bytes)});
  }
  std::sort(out.begin(), out.end());
}

}  // namespace

TEST_CASE(
    "far forest: a streamed load builds a far tile per occupied tile, the same bytes at any "
    "job count, read back by the next load") {
  if (!generator_present()) {
    MESSAGE("no authored generator in this build");
    return;
  }
  const test::TempDir tmp("renderer_far_forest");
  REQUIRE(tmp.ok());
  REQUIRE(prepare(tmp));
  std::string error;
  SceneDesc off_desc;
  SceneData off;
  REQUIRE_MESSAGE(load(tmp, tmp.file("ddc_off"), false, 1, off_desc, off, error), error);
  CHECK(off.far_forest.empty());
  SceneDesc one_desc;
  SceneData one;
  REQUIRE_MESSAGE(load(tmp, tmp.file("ddc_one"), true, 1, one_desc, one, error), error);
  SceneDesc many_desc;
  SceneData many;
  REQUIRE_MESSAGE(load(tmp, tmp.file("ddc_many"), true, 6, many_desc, many, error), error);

  const FarForestData& far = one.far_forest;
  const FarForestStats& stats = far.stats;
  MESSAGE("far forest: " << stats.tiles << " tiles, " << stats.trees << " trees, "
                         << stats.triangles << " triangles, " << stats.clusters << " clusters, "
                         << stats.skipped_small << " small, " << stats.skipped_heavy << " heavy");
  // Every streamed tree of 4 m or more is in it (the scatter's oaks are 6.4–9.6 m, the spruces 6
  // m), one far tile per world tile that holds one, each a mesh after every other and an instance
  // after the prefix, standing at its tile's corner.
  CHECK(stats.trees + stats.skipped_small + stats.skipped_heavy + stats.skipped_textures ==
        one.authored.streamed);
  CHECK(stats.trees > 0);
  REQUIRE(stats.tiles == far.tiles.size());
  CHECK(stats.tiles > 4);
  CHECK(one.parts.size() == off.parts.size() + stats.tiles);
  CHECK(one.instances.size() == off.instances.size() + stats.tiles);
  CHECK(one.pair_count == off.pair_count + stats.clusters);
  u64 clusters = 0;
  u32 crossed = 0;
  for (u32 t = 0; t < far.tiles.size(); ++t) {
    const FarForestTile& tile = far.tiles[t];
    CAPTURE(t);
    if (t > 0) CHECK(far.tiles[t - 1].key < tile.key);
    CHECK(tile.key == far_tile_key(tile.x, tile.z));
    REQUIRE(tile.instance < one.instances.size());
    CHECK(one.instances[tile.instance].mesh == tile.mesh);
    CHECK(one.instances[tile.instance].cell.x == static_cast<i32>(std::floor(tile.x * 0.5)));
    CHECK(one.sources[tile.mesh].species.empty());  // no species: rigid, the trees' casting
    const geometry::ClusterMeshPart& part = one.parts[tile.mesh];
    clusters += part.cluster_count;
    bool cross = false;
    for (u32 c = part.first_cluster; c < part.first_cluster + part.cluster_count; ++c) {
      // Two-sided planes: no cone may cull a face that is seen from behind.
      CHECK(one.lod.mesh.clusters[c].cone == geometry::k_cone_none);
      cross = cross || one.lod.lod[c].level > 0;
    }
    crossed += cross ? 1u : 0u;
    CHECK(one.sources[tile.mesh].part_of_cluster.size() == part.cluster_count);
  }
  CHECK(clusters == stats.clusters);
  // The oaks' stars are crossed past their measured error; every tile with an oak has the level.
  CHECK(crossed > 0);
  CHECK(stats.cache_misses == stats.tiles);

  // The entries are the same bytes whatever the job count: a tile is one job's, in a fixed order.
  Vector<std::pair<std::string, std::string>> a;
  Vector<std::pair<std::string, std::string>> b;
  entries(tmp.file("ddc_one"), a);
  entries(tmp.file("ddc_many"), b);
  REQUIRE(a.size() == stats.tiles);
  REQUIRE(b.size() == a.size());
  for (u32 i = 0; i < a.size(); ++i) {
    CAPTURE(a[i].first);
    CHECK(a[i].first == b[i].first);
    CHECK(a[i].second == b[i].second);
  }
  CHECK(many.far_forest.stats.clusters == stats.clusters);

  // A second load reads every tile back and draws the same scene.
  SceneDesc again_desc;
  SceneData again;
  REQUIRE_MESSAGE(load(tmp, tmp.file("ddc_one"), true, 3, again_desc, again, error), error);
  CHECK(again.far_forest.stats.cache_hits == stats.tiles);
  CHECK(again.far_forest.stats.cache_misses == 0);
  CHECK(again.pair_count == one.pair_count);
  REQUIRE(again.lod.mesh.vertices.size() == one.lod.mesh.vertices.size());
  CHECK(std::equal(
      again.lod.mesh.vertices.begin(), again.lod.mesh.vertices.end(), one.lod.mesh.vertices.begin(),
      [](const Vec3& p, const Vec3& q) { return p.x == q.x && p.y == q.y && p.z == q.z; }));
}

namespace {

constexpr u32 k_w = 320;
constexpr u32 k_h = 180;

// The tail the world would hand over for a camera whose ring holds `held` tiles: each tile's
// innermost placements from the `authored` generator, a block per tile by its key.
bool make_tail(const SceneData& data, std::span<const std::pair<i32, i32>> held,
               Vector<SceneInstance>& tail, Vector<DynamicBlock>& blocks, std::string& error) {
  tail.clear();
  blocks.clear();
  const StreamedPlacements& entry = data.streamed.back();
  const scene_gen::PlacementGeneratorDesc* g =
      scene_gen::GeneratorRegistry::global().find_placement("authored");
  scene_gen::Context context;
  context.tile_size = data.world.tile_size;
  context.ring = 0;
  void* state = nullptr;
  if (!g->open(entry.params, context, &state, &error)) return false;
  for (const auto& [x, z] : held) {
    scene_gen::Placements tile;
    if (!g->tile(state, scene_gen::TileCoord{x, z}, context, tile, &error)) {
      g->close(state);
      return false;
    }
    DynamicBlock block{far_tile_key(x, z), static_cast<u32>(tail.size()),
                       static_cast<u32>(tile.instances.size()), 0};
    for (const scene_gen::Placement& p : tile.instances) {
      SceneInstance instance;
      instance.mesh = entry.meshes[p.mesh];
      instance.transform = p.turn();
      instance.origin = p.position;
      tail.push_back(instance);
    }
    if (block.count > 0) blocks.push_back(block);
  }
  g->close(state);
  return true;
}

struct FarView {
  CapturedFrame frame;
  u32 far_pixels = 0;   // pixels of a far tile's instance
  u32 tree_pixels = 0;  // pixels of any tree: the tail's or the far forest's
  u32 far_in_held = 0;  // far pixels whose tile the tail holds: must be none
  u32 drawn_tiles = 0;
};

FarView draw(const gfx::Device& device, SceneRenderer& renderer, const SceneData& data,
             std::span<const std::pair<i32, i32>> held) {
  FarView out;
  std::string error;
  Vector<SceneInstance> tail;
  Vector<DynamicBlock> blocks;
  REQUIRE_MESSAGE(make_tail(data, held, tail, blocks, error), error);
  REQUIRE_MESSAGE(renderer.set_dynamic_instances(
                      std::span<const SceneInstance>(tail.data(), tail.size()),
                      std::span<const DynamicBlock>(blocks.data(), blocks.size()), &error),
                  error);
  (void)device;
  FrameDesc frame;
  frame.camera.position = WorldPos{10.0, 45.0, -230.0};
  frame.camera.target = WorldPos{0.0, 4.0, 0.0};
  CaptureChannels channels;
  channels.ids = true;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame, &error), error);
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame, &error), error);
  const u32 prefix = data.instances.size();
  for (u32 px = 0; px < k_w * k_h; ++px) {
    const u32 instance = out.frame.ids[u64{px} * k_id_words];
    if (instance == ~0u) continue;  // nothing
    bool far = false;
    for (const FarForestTile& tile : data.far_forest.tiles) {
      if (tile.instance != instance) continue;
      far = true;
      for (const DynamicBlock& block : blocks)
        out.far_in_held += block.key == tile.key ? 1u : 0u;
    }
    out.far_pixels += far ? 1u : 0u;
    out.tree_pixels += far || instance >= prefix ? 1u : 0u;  // the terrain is in the prefix
  }
  return out;
}

}  // namespace

TEST_CASE(
    "far forest: past the ring the trees are drawn, and a tile the tail draws is not drawn "
    "twice") {
  if (!generator_present()) {
    MESSAGE("no authored generator in this build");
    return;
  }
  shadow_fixture::Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("renderer_far_forest_gpu");
  REQUIRE(tmp.ok());
  REQUIRE(prepare(tmp));
  std::string error;
  // The ring holds the four tiles round the origin; the far forest is every other.
  const std::pair<i32, i32> centre[] = {{-1, -1}, {0, -1}, {-1, 0}, {0, 0}};
  Vector<std::pair<i32, i32>> all;
  for (i32 z = -4; z <= 3; ++z)
    for (i32 x = -4; x <= 3; ++x)
      all.push_back({x, z});

  FarView with_far;
  FarView without_far;
  FarView everything_held;
  FarView nothing_held;
  for (const bool far_forest : {true, false}) {
    SceneDesc desc;
    SceneData data;
    REQUIRE_MESSAGE(load(tmp, tmp.file("ddc"), far_forest, 2, desc, data, error), error);
    RenderSettings settings;
    settings.raster = RasterMode::Hardware;
    settings.shadows = ShadowMode::Off;
    ResolvedSettings resolved;
    resolve_settings(settings, gpu.device.features(), &data, resolved);
    if (check_availability(resolved, gpu.device.features()) != RenderAvailability::Ok) {
      MESSAGE("the hardware raster path is unavailable here");
      return;
    }
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(gpu.device, data, resolved, &error), error);
    SceneRenderer renderer;
    SceneRenderer::Desc rdesc;
    rdesc.width = k_w;
    rdesc.height = k_h;
    REQUIRE_MESSAGE(renderer.create(gpu.device, scene, resolved, rdesc, &error), error);
    if (far_forest) {
      REQUIRE(!data.far_forest.empty());
      with_far = draw(gpu.device, renderer, data, centre);
      everything_held = draw(gpu.device, renderer, data,
                             std::span<const std::pair<i32, i32>>(all.data(), all.size()));
      nothing_held = draw(gpu.device, renderer, data, {});
    } else {
      without_far = draw(gpu.device, renderer, data, centre);
    }
    renderer.destroy();
    scene.destroy();
  }
  MESSAGE("far pixels " << with_far.far_pixels << ", trees " << with_far.tree_pixels
                        << " with the far forest and " << without_far.tree_pixels
                        << " without; all held: " << everything_held.far_pixels
                        << " far pixels; none held: " << nothing_held.far_pixels);
  // Past the ring the trees are drawn where nothing was.
  CHECK(with_far.far_pixels > 200);
  CHECK(without_far.far_pixels == 0);
  CHECK(with_far.tree_pixels > without_far.tree_pixels);
  // Never twice: no far pixel belongs to a tile the tail draws, and with every tile held none.
  CHECK(with_far.far_in_held == 0);
  CHECK(everything_held.far_pixels == 0);
  // And with no tile held the far forest draws the four as well.
  CHECK(nothing_held.far_pixels > with_far.far_pixels);
}

#endif

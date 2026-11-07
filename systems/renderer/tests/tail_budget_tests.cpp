// The streamed trees' budget (docs/subsystems/renderer.md, "A commit past the tail's budget
// withholds its farthest tiles"; roadmap R137). The device's share on the CPU; then, where the
// flora capability is linked, a generated palm streamed in tiles: a record a tree gives back and
// the next takes in the same commit is counted once, so the bound's room is the room the tables
// grow to; a commit past the budget admits its nearest tiles and withholds the rest, which a later
// commit with room admits; the tile the eye stands in is never the one withheld; and a single tile
// past the whole budget is still refused, with a sentence, and the scene takes the next commit.
#include <domain/gfx/device.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <bit>
#include <fstream>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

TEST_CASE("tail budget: a sixteenth of the device, 1 to 4 GiB, the tunable over it") {
  CHECK(flora_tail_device_budget_mib(0) == k_flora_tail_floor_mib);  // a device that says nothing
  CHECK(flora_tail_device_budget_mib(u64{8} << 30) == 1024);
  CHECK(flora_tail_device_budget_mib(u64{16} << 30) == 1024);  // the old default, unchanged
  CHECK(flora_tail_device_budget_mib(u64{24} << 30) == 1536);
  CHECK(flora_tail_device_budget_mib(u64{32} << 30) == 2048);
  // The RTX 5090's heaps as the chain's budget reads them (rt_capacity.h: 2,025 MiB a sixteenth).
  CHECK(flora_tail_device_budget_mib(u64{32400} << 20) == 2025);
  CHECK(flora_tail_device_budget_mib(u64{128} << 30) == k_flora_tail_ceiling_mib);
  const FloraTailBudget named = resolve_flora_tail_budget(1, u64{32} << 30);
  CHECK(named.mib == 1);
  CHECK_FALSE(named.device);
  const FloraTailBudget device = resolve_flora_tail_budget(0, u64{32} << 30);
  CHECK(device.mib == 2048);
  CHECK(device.device);
}

#if ENGINE_RENDERER_TESTS_FLORA

namespace {

// The flora tests' young palm (flora_motion_tests.cpp): solved and clustered in a moment.
constexpr const char* k_palm =
    R"({"species":"date_palm","palm":{"seed":4,"height_m":3.5,"frond_count":10,)"
    R"("leaflet_count":12,"dead_skirt_fronds":3,"triangle_budget":9000}})";

struct Rig {
  gfx::Device device;
  SceneData data;
  ResolvedSettings resolved;
  std::string why;
  u32 cls = 0;      // the palm's run of the pose table is 2^cls parts
  u32 reserve = 0;  // palms the first room holds
  bool ok = false;

  explicit Rig(const test::TempDir& tmp) {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      why = "no Vulkan device: " + error;
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    const std::string path = tmp.file("young.flora.json");
    {
      std::ofstream f(path, std::ios::binary);
      f << k_palm;
    }
    SceneDesc desc;
    desc.meshes.push_back(path);
    desc.ddc = tmp.file("ddc");
    desc.cache = true;
    if (!load_scene(desc, data, error)) {
      why = "the palm did not load: " + error;
      return;
    }
    data.dynamic = true;
    RenderSettings settings;
    settings.shadows = ShadowMode::Cascaded;
    resolve_settings(settings, device.features(), &data, resolved);
    if (!resolved.flora_motion) {
      why = "the device draws no swaying trees";
      return;
    }
    FloraMesh tree;
    REQUIRE(flora_mesh(
        std::span<const geometry::ClusterFileMotionPart>(data.sources[0].motion_parts.data(),
                                                         data.sources[0].motion_parts.size()),
        std::span<const geometry::ClusterFileStructuralPart>(
            data.sources[0].structural_parts.data(), data.sources[0].structural_parts.size()),
        data.sources[0].species, tree));
    const u32 parts = tree.part_count();
    cls = parts <= 1 ? 0u : static_cast<u32>(std::bit_width(parts - 1u));
    reserve = std::min(k_tail_deform_reserve, k_tail_part_reserve >> cls);
    ok = reserve >= 2;
    if (!ok) why = "the palm's run is larger than the first room";
  }
};

// `count` palms in a row from (x, 0, 0), a metre apart; the tests' tiles start a kilometre apart.
void palms(Vector<SceneInstance>& tail, f64 x, u32 count) {
  for (u32 i = 0; i < count; ++i) {
    SceneInstance placed;
    placed.mesh = 0;
    placed.origin = WorldPos{x + static_cast<f64>(i), 0.0, 0.0};
    tail.push_back(placed);
  }
}

struct Tile {
  u64 key = 0;
  f64 x = 0.0;
  u32 count = 0;
  u32 ring = 0;
};

// Hands `tiles` over in the order given, each its own block; grows the tables as
// `SceneRenderer::set_dynamic_instances` does.
bool commit(GpuScene& scene, std::span<const Tile> tiles, GpuScene::TileChange& change,
            std::string& error) {
  Vector<SceneInstance> tail;
  Vector<DynamicBlock> blocks;
  for (const Tile& tile : tiles) {
    DynamicBlock block;
    block.key = tile.key;
    block.first = tail.size();
    block.count = tile.count;
    block.ring = tile.ring;
    blocks.push_back(block);
    palms(tail, tile.x, tile.count);
  }
  if (!scene.set_dynamic_instances(std::span<const SceneInstance>(tail.data(), tail.size()),
                                   std::span<const DynamicBlock>(blocks.data(), blocks.size()),
                                   change, nullptr, &error)) {
    return false;
  }
  return !scene.relayout_pending() || scene.relayout(&error);
}

bool placed(const GpuScene& scene, u64 key) { return scene.tile_layout().find(key) != nullptr; }

}  // namespace

TEST_CASE("tail budget: a record given back and taken again in one commit is counted once") {
  const test::TempDir tmp("renderer_tail_budget_once");
  REQUIRE(tmp.ok());
  Rig rig(tmp);
  if (!rig.ok) {
    MESSAGE("skipped: " << rig.why);
    return;
  }
  std::string error;
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(rig.device, rig.data, rig.resolved, &error), error);
  REQUIRE(scene.flora_tail());
  // Three quarters of the first room: two tiles' worth would pass it, so a bound that counted the
  // moved trees as new records would grow the room where the tables do not.
  const u32 n = rig.reserve - rig.reserve / 4;
  GpuScene::TileChange change;
  const Tile first[] = {{1, 0.0, n, 0}};
  REQUIRE_MESSAGE(commit(scene, first, change, error), error);
  CHECK(scene.flora_stats().tail_records == n);
  const u64 room = scene.flora_stats().tail_bytes;
  CHECK(scene.flora_stats().tail_bound_bytes == room);
  // The n trees move to another tile: given back and taken again in the one commit, no record is
  // made, and the bound says the room stays what it is. Before R137 it counted n new records.
  const Tile moved[] = {{2, 1000.0, n, 0}};
  REQUIRE_MESSAGE(commit(scene, moved, change, error), error);
  CHECK(scene.flora_stats().tail_records == n);
  CHECK(scene.flora_stats().tail_bound_bytes == room);
  CHECK(scene.flora_stats().tail_bytes == room);
  // Past the first room: the tile's trees move again and `reserve` more arrive. The bound is the
  // room the tables grow to, to the byte.
  const Tile grown[] = {{3, 2000.0, n + rig.reserve, 0}};
  REQUIRE_MESSAGE(commit(scene, grown, change, error), error);
  CHECK(scene.flora_stats().tail_records == n + rig.reserve);
  CHECK(scene.flora_stats().tail_bytes > room);
  CHECK(scene.flora_stats().tail_bound_bytes == scene.flora_stats().tail_bytes);
  MESSAGE("the palm's run is " << (1u << rig.cls) << " parts; the first room holds " << rig.reserve
                               << " palms in " << room << " bytes, grown to "
                               << scene.flora_stats().tail_bytes);
  scene.destroy();
}

TEST_CASE("tail budget: a commit past it admits the nearest tiles and withholds the farthest") {
  const test::TempDir tmp("renderer_tail_budget_withhold");
  REQUIRE(tmp.ok());
  Rig rig(tmp);
  if (!rig.ok) {
    MESSAGE("skipped: " << rig.why);
    return;
  }
  std::string error;
  const u32 n = rig.reserve / 2;  // two tiles fill the first room; a third would grow it

  SUBCASE("the nearest first, the rest withheld and admitted once there is room") {
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(rig.device, rig.data, rig.resolved, &error), error);
    scene.set_tail_budget_bytes(scene.flora_stats().tail_bytes);  // the first room, exactly
    scene.set_tail_eye(WorldPos{0.0, 0.0, 0.0});
    // Handed over farthest first, so the order admitted is the budget's and not the caller's.
    const Tile tiles[] = {{4, 3000.0, n, 0}, {3, 2000.0, n, 0}, {2, 1000.0, n, 0}, {1, 0.0, n, 0}};
    GpuScene::TileChange change;
    REQUIRE_MESSAGE(commit(scene, tiles, change, error), error);
    CHECK(change.withheld_tiles == 2);
    CHECK(change.withheld_instances == 2 * n);
    CHECK(placed(scene, 1));
    CHECK(placed(scene, 2));
    CHECK_FALSE(placed(scene, 3));
    CHECK_FALSE(placed(scene, 4));
    CHECK(scene.dynamic_instance_count() == 2 * n);
    CHECK(scene.flora_stats().tail_withheld_tiles == 2);
    CHECK(scene.flora_stats().tail_withheld_instances == 2 * n);
    CHECK(scene.flora_stats().tail_withholding_commits == 1);
    CHECK(scene.flora_stats().tail_refused == 0);
    CHECK(scene.flora_stats().tail_bytes <= scene.flora_stats().tail_budget_bytes);
    // What the commit placed, in the caller's order, for whatever follows the tail.
    const std::span<const DynamicBlock> admitted =
        scene.admitted_blocks(std::span<const DynamicBlock>());
    REQUIRE(admitted.size() == 2);
    CHECK(admitted[0].key == 2);
    CHECK(admitted[1].key == 1);
    // The eye moves on and the near tiles leave: the withheld ones are admitted, records reused.
    scene.set_tail_eye(WorldPos{3000.0, 0.0, 0.0});
    const Tile later[] = {{4, 3000.0, n, 0}, {3, 2000.0, n, 0}};
    REQUIRE_MESSAGE(commit(scene, later, change, error), error);
    CHECK(change.withheld_tiles == 0);
    CHECK(placed(scene, 3));
    CHECK(placed(scene, 4));
    CHECK(scene.dynamic_instance_count() == 2 * n);
    CHECK(scene.flora_stats().tail_withheld_tiles == 0);
    CHECK(scene.flora_stats().tail_records == 2 * n);
    scene.destroy();
  }

  SUBCASE("the tile the eye stands in is never the one withheld, whatever its ring") {
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(rig.device, rig.data, rig.resolved, &error), error);
    scene.set_tail_budget_bytes(scene.flora_stats().tail_bytes);
    scene.set_tail_eye(WorldPos{3001.0, 0.0, 0.0});  // among tile 4's palms
    const Tile tiles[] = {{1, 0.0, n, 0}, {2, 1000.0, n, 0}, {3, 2000.0, n, 0}, {4, 3000.0, n, 2}};
    GpuScene::TileChange change;
    REQUIRE_MESSAGE(commit(scene, tiles, change, error), error);
    CHECK(change.withheld_tiles == 2);
    CHECK(placed(scene, 4));  // the eye's, though its ring is the farthest
    CHECK(placed(scene, 3));  // then ring 0 by distance
    CHECK_FALSE(placed(scene, 2));
    CHECK_FALSE(placed(scene, 1));
    scene.destroy();
  }

  SUBCASE("a single tile past the whole budget is refused, and the scene takes the next commit") {
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(rig.device, rig.data, rig.resolved, &error), error);
    scene.set_tail_budget_bytes(scene.flora_stats().tail_bytes);
    scene.set_tail_eye(WorldPos{0.0, 0.0, 0.0});
    const Tile big[] = {{1, 0.0, rig.reserve + 1, 0}, {2, 1000.0, 1, 0}};
    GpuScene::TileChange change;
    CHECK_FALSE(commit(scene, big, change, error));
    MESSAGE(error);
    CHECK(error.find("renderer.flora.tail_budget_mib") != std::string::npos);
    CHECK(error.find("refused") != std::string::npos);
    CHECK(scene.flora_stats().tail_refused == 1);
    CHECK(scene.dynamic_instance_count() == 0);  // nothing changed
    CHECK(scene.flora_stats().tail_records == 0);
    const Tile fits[] = {{1, 0.0, rig.reserve, 0}};
    error.clear();
    REQUIRE_MESSAGE(commit(scene, fits, change, error), error);
    CHECK(placed(scene, 1));
    CHECK(scene.flora_stats().tail_records == rig.reserve);
    scene.destroy();
  }
}

#endif

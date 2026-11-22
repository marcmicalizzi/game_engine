// The streamed trees' budget (docs/subsystems/renderer.md, "A commit past the tail's budget
// withholds its farthest tiles"; roadmap R137). The device's share on the CPU; then, where the
// flora capability is linked, a generated palm streamed in tiles: a record a tree gives back and
// the next takes in the same commit is counted once, so the bound's room is the room the tables
// grow to; a commit past the budget admits its nearest tiles and withholds the rest, which a later
// commit with room admits; the tile the eye stands in is never the one withheld; and a single tile
// past the whole budget is still refused, with a sentence, and the scene takes the next commit.
#include "test_device.h"

#include <domain/geometry/cluster.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resources.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <bit>
#include <cstring>
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
    if (!device.create(renderer_test::device_options(), &error)) {
      why = "no Vulkan device: " + error;
      if (!renderer_test::device_profile().empty()) {
        FAIL_CHECK("ENGINE_GFX_TEST_DEVICE=" << renderer_test::device_profile() << ": " << why);
      }
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

// T152 (renderer.md, "A relayout is swapped in"): the trees' tables grow beside the old ones. A
// commit past the first room leaves the relayout pending; swapped, it makes the deform table, its
// copies and the pose table again without a device wait or an upload on this thread, hands the old
// ones back for the frames in flight, and leaves the chain — which a growth of the trees alone does
// not outgrow — as it was.
TEST_CASE("tail budget: the trees' tables grow beside the old ones and wait for nothing") {
  const test::TempDir tmp("renderer_tail_budget_swapped");
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
  GpuScene::TileChange change;
  const Tile first[] = {{1, 0.0, rig.reserve / 2, 0}};
  REQUIRE_MESSAGE(commit(scene, first, change, error), error);
  const u64 table = scene.deform_table.size;
  const u64 table_address = scene.deform_table.address;
  const u64 poses = scene.flora_stats().pose_table_bytes;
  // Past the first room: the trees stay and `reserve` more arrive.
  const Tile grown[] = {{1, 0.0, rig.reserve / 2, 0}, {2, 1000.0, rig.reserve, 0}};
  Vector<SceneInstance> tail;
  Vector<DynamicBlock> blocks;
  for (const Tile& tile : grown) {
    DynamicBlock block;
    block.key = tile.key;
    block.first = tail.size();
    block.count = tile.count;
    blocks.push_back(block);
    palms(tail, tile.x, tile.count);
  }
  REQUIRE_MESSAGE(
      scene.set_dynamic_instances(std::span<const SceneInstance>(tail.data(), tail.size()),
                                  std::span<const DynamicBlock>(blocks.data(), blocks.size()),
                                  change, nullptr, &error),
      error);
  REQUIRE(scene.relayout_pending());
  const u64 waits = gfx::device_waits_on_thread();
  GpuScene::RelayoutRetired retired;
  REQUIRE_MESSAGE(scene.relayout(&error, &retired), error);
  CHECK(gfx::device_waits_on_thread() == waits);
  CHECK_FALSE(retired.must_wait);
  CHECK_FALSE(scene.relayout_pending());
  CHECK_FALSE(retired.tlas.handle.valid());
  CHECK(retired.chain.count == 0);
  CHECK(scene.flora_stats().tail_records == rig.reserve / 2 + rig.reserve);
  // The first room is the smaller of the records' and the parts' reserves (`Rig::reserve`), so the
  // one that bound it grew; the deform table is made again whichever did.
  CHECK((scene.deform_table.size > table || scene.flora_stats().pose_table_bytes > poses));
  CHECK(scene.deform_table.address != table_address);
  // The old table at least, and every buffer handed back is one the scene no longer names.
  CHECK(retired.buffers.size() >= 1);
  for (const gfx::BufferResource& old : retired.buffers) {
    CHECK(old.buffer.valid());
    CHECK(old.address != scene.deform_table.address);
  }
  MESSAGE("grown beside: " << retired.buffers.size() << " buffers handed back; the table " << table
                           << " to " << scene.deform_table.size << " bytes, the poses " << poses
                           << " to " << scene.flora_stats().pose_table_bytes);
  for (gfx::BufferResource& old : retired.buffers)
    gfx::destroy_buffer(rig.device, old);
  scene.destroy();
}

// T163 (renderer.md, "A relayout is swapped in"): the trees' tables' growth cannot fail part way.
// The seam fails it after the pose table is made and before the deform table: the swapped call
// hands nothing back, names the same table at the same capacity, and stays pending; the waited call
// the renderer makes next grows the tables to exactly what a growth that never failed makes.
TEST_CASE("tail budget: a growth of the trees' tables that fails is put back and retried whole") {
  const test::TempDir tmp("renderer_tail_budget_fault");
  REQUIRE(tmp.ok());
  Rig rig(tmp);
  if (!rig.ok) {
    MESSAGE("skipped: " << rig.why);
    return;
  }
  const Tile first[] = {{1, 0.0, rig.reserve / 2, 0}};
  const Tile grown[] = {{1, 0.0, rig.reserve / 2, 0}, {2, 1000.0, rig.reserve, 0}};
  std::string error;
  // The same growth straight through, waited (`commit`), for what the retry must come to.
  GpuScene straight;
  REQUIRE_MESSAGE(straight.create(rig.device, rig.data, rig.resolved, &error), error);
  {
    GpuScene::TileChange change;
    REQUIRE_MESSAGE(commit(straight, first, change, error), error);
    REQUIRE_MESSAGE(commit(straight, grown, change, error), error);
  }
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(rig.device, rig.data, rig.resolved, &error), error);
  GpuScene::TileChange change;
  REQUIRE_MESSAGE(commit(scene, first, change, error), error);
  const u64 table = scene.deform_table.size;
  const u64 table_address = scene.deform_table.address;
  const u64 poses = scene.flora_stats().pose_table_bytes;
  const u64 tail_bytes = scene.flora_stats().tail_bytes;
  Vector<SceneInstance> tail;
  Vector<DynamicBlock> blocks;
  for (const Tile& tile : grown) {
    DynamicBlock block;
    block.key = tile.key;
    block.first = tail.size();
    block.count = tile.count;
    blocks.push_back(block);
    palms(tail, tile.x, tile.count);
  }
  REQUIRE_MESSAGE(
      scene.set_dynamic_instances(std::span<const SceneInstance>(tail.data(), tail.size()),
                                  std::span<const DynamicBlock>(blocks.data(), blocks.size()),
                                  change, nullptr, &error),
      error);
  REQUIRE(scene.relayout_pending());
  scene.fail_next(GpuScene::Fault::flora_tables);
  GpuScene::RelayoutRetired retired;
  CHECK_FALSE(scene.relayout(&error, &retired));
  MESSAGE("the swapped growth failed: " << error);
  CHECK(scene.relayout_pending());
  CHECK(retired.buffers.empty());
  CHECK(retired.chain.count == 0);
  CHECK_FALSE(retired.tlas.handle.valid());
  CHECK(scene.deform_table.address == table_address);
  CHECK(scene.deform_table.size == table);
  CHECK(scene.flora_stats().pose_table_bytes == poses);
  CHECK(scene.flora_stats().tail_bytes == tail_bytes);
  // The waited call, as `SceneRenderer::set_dynamic_instances` makes it after waiting.
  error.clear();
  REQUIRE_MESSAGE(scene.relayout(&error), error);
  CHECK_FALSE(scene.relayout_pending());
  CHECK(scene.deform_table.size == straight.deform_table.size);
  CHECK(scene.flora_stats().pose_table_bytes == straight.flora_stats().pose_table_bytes);
  CHECK(scene.flora_stats().tail_bytes == straight.flora_stats().tail_bytes);
  CHECK(scene.flora_stats().tail_records == straight.flora_stats().tail_records);
  CHECK(scene.flora_stats().tail_records == rig.reserve / 2 + rig.reserve);
  // Every record the CPU holds is in the new table, as many as the straight scene's.
  REQUIRE(scene.deform_table.mapped != nullptr);
  CHECK(scene.deform_count() == straight.deform_count());
  const usize used = usize{scene.deform_count()} * sizeof(gfx::DeformDesc);
  CHECK(std::memcmp(scene.deform_table.mapped, scene.deform_descs().data(), used) == 0);
  MESSAGE("retried waited: the table " << table << " to " << scene.deform_table.size
                                       << " bytes, the poses " << poses << " to "
                                       << scene.flora_stats().pose_table_bytes);
  scene.destroy();
  straight.destroy();
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

// T213 (renderer.md, "Streamed trees sway"): every per-frame copy of the deform table holds every
// record the table holds, a skinned scene's too. A skinned frame rewrites its own copy whole, so
// until T213 a tail tree's record went into the table alone; the motion pool's second write reads
// last frame's copy (`MotionPass::previous`), which then held, at a record the tail had just made,
// whatever the buffer held when it was made — and the owner's forest, skinned by its player and its
// animals, lost the device on the flight that brought in more trees than had ever been resident.
TEST_CASE("tail budget: a skinned scene's every copy of the deform table holds the tail's trees") {
  const test::TempDir tmp("renderer_tail_budget_skinned_copies");
  REQUIRE(tmp.ok());
  Rig rig(tmp);
  if (!rig.ok) {
    MESSAGE("skipped: " << rig.why);
    return;
  }
  // The palm skinned as well, one joint for every vertex, and its loaded instance naming it: what
  // makes the scene keep copies that each frame rewrites whole.
  const u32 joints[4] = {0, 0, 0, 0};
  const f32 weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  const geometry::SkinBinding binding = geometry::make_skin_binding(joints, weights);
  rig.data.lod.mesh.skin.resize(rig.data.lod.mesh.vertices.size());
  for (geometry::SkinBinding& b : rig.data.lod.mesh.skin)
    b = binding;
  if (rig.data.instances.empty()) {
    SceneInstance standing;
    standing.mesh = 0;
    gfx::InstanceDesc desc;
    std::string why;
    REQUIRE_MESSAGE(make_instance(rig.data, standing, rig.data.pair_count, desc, &why), why);
    rig.data.pair_count += rig.data.parts[0].cluster_count;
    rig.data.instances.push_back(desc);
  }
  rig.data.instance_joints.resize(rig.data.instances.size(), 0u);
  rig.data.instance_joints[0] = 1;
  rig.data.skinned_instances = 1;
  rig.data.max_joints = 1;
  REQUIRE(rig.data.skinned());
  RenderSettings settings;
  settings.shadows = ShadowMode::Cascaded;
  resolve_settings(settings, rig.device.features(), &rig.data, rig.resolved);
  std::string error;
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(rig.device, rig.data, rig.resolved, &error), error);
  REQUIRE(scene.skinned());
  REQUIRE(scene.has_frame_table());
  REQUIRE(scene.flora_tail());
  const u32 before = scene.deform_count();
  // Records past every other, inside the first room: written at once, no relayout between.
  const u32 n = rig.reserve / 2;
  GpuScene::TileChange change;
  const Tile tile[] = {{1, 0.0, n, 0}};
  REQUIRE_MESSAGE(commit(scene, tile, change, error), error);
  REQUIRE(scene.deform_count() == before + n);
  // The records of the tail's trees that some copy does not hold, over every copy.
  u32 missed = 0;
  const std::span<const gfx::DeformDesc> table = scene.deform_descs();
  for (u32 slot = 0; slot < k_joint_slots; ++slot) {
    const gfx::DeformDesc* copy = scene.deform_frame(slot);
    for (u32 d = before; d < scene.deform_count(); ++d)
      missed += std::memcmp(&copy[d], &table[d], sizeof(gfx::DeformDesc)) != 0 ? 1u : 0u;
  }
  MESSAGE(n << " trees' records in " << k_joint_slots << " copies: " << missed << " missed");
  CHECK(missed == 0);  // before T213: n * k_joint_slots
  scene.destroy();
}

#endif

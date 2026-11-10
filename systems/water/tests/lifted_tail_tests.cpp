// A lifted water body beside a streamed tail of swaying trees (S100; water.md, "The mesh"): the
// scene keeps per-frame copies of the deform table for the water's clock, and a frame reads every
// record from its copy, so the copies must hold every record the table holds. They did not when the
// tail's growth made the table again without the copies — the part-pose table outgrowing its room
// before the records did — which left a tree's record uninitialised in every copy and lost the
// device in the forest proof of concept's first frames (2026-11-09).
#include <core/math/world.h>
#include <domain/gfx/device.h>
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

namespace {

// The flora tests' young palm (renderer's tail_budget_tests.cpp): solved and clustered in a moment.
constexpr const char* k_palm =
    R"({"species":"date_palm","palm":{"seed":4,"height_m":3.5,"frond_count":10,)"
    R"("leaflet_count":12,"dead_skirt_fronds":3,"triangle_budget":9000}})";

// The renderer's own ground and a pool with a wind on it, which the pool lifts.
constexpr const char* k_scene = R"({
  "format": "engine.scene.v1",
  "name": "lifted-tail",
  "terrain": {"size": 65, "extent": 32, "seed": 4, "dune_height": 0.6, "dune_wavelength": 12},
  "water": [{"name": "pool", "level_m": 0.2,
    "shape": {"circle": {"center": [3, 0, -2], "radius_m": 6}}, "wind_follow": false,
    "direction_deg": 90, "wind_m_s": 4}]
}
)";

bool write_text(const std::string& path, const char* text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
  return static_cast<bool>(out);
}

// Every record of the table, byte for byte, in every slot's copy.
u32 copies_that_differ(GpuScene& scene) {
  const std::span<const gfx::DeformDesc> statics = scene.deform_descs();
  u32 differ = 0;
  for (u32 slot = 0; slot < k_joint_slots; ++slot) {
    const gfx::DeformDesc* copy = scene.deform_frame(slot);
    for (u32 d = 0; d < statics.size(); ++d)
      differ += std::memcmp(&copy[d], &statics[d], sizeof(gfx::DeformDesc)) != 0 ? 1u : 0u;
  }
  return differ;
}

}  // namespace

TEST_CASE("water: a lifted body's frame copies hold every record a streamed tree's growth makes") {
  const test::TempDir tmp("water_lifted_tail");
  REQUIRE(tmp.ok());
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no Vulkan device: " << error);
    return;
  }
  if (!device.features().buffer_int64_atomics) {
    MESSAGE("no 64-bit buffer atomics here");
    device.destroy();
    return;
  }
  const std::string scene_path = tmp.file("scene.json");
  const std::string palm_path = tmp.file("young.flora.json");
  REQUIRE(write_text(scene_path, k_scene));
  REQUIRE(write_text(palm_path, k_palm));
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(scene_path, desc, error), error);
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  const u32 palm = desc.meshes.size();
  desc.meshes.push_back(palm_path);
  desc.mesh_info.push_back(SceneMeshInfo{});
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  data.dynamic = true;
  RenderSettings settings;
  settings.shadows = ShadowMode::Cascaded;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  if (!resolved.flora_motion) {
    MESSAGE("the device draws no swaying trees");
    device.destroy();
    return;
  }
  FloraMesh tree;
  REQUIRE(flora_mesh(
      std::span<const geometry::ClusterFileMotionPart>(data.sources[palm].motion_parts.data(),
                                                       data.sources[palm].motion_parts.size()),
      std::span<const geometry::ClusterFileStructuralPart>(
          data.sources[palm].structural_parts.data(), data.sources[palm].structural_parts.size()),
      data.sources[palm].species, tree));
  const u32 parts = tree.part_count();
  const u32 cls = parts <= 1 ? 0u : static_cast<u32>(std::bit_width(parts - 1u));
  // The palms the part-pose table's first room holds, which must be fewer than the records' room
  // for the growth this case is about: the parts' alone.
  const u32 part_room = k_tail_part_reserve >> cls;
  REQUIRE_MESSAGE(part_room < k_tail_deform_reserve,
                  "the palm's run of " << (1u << cls) << " parts does not outgrow the parts first");
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  REQUIRE(scene.water_lifted());
  REQUIRE(scene.has_frame_table());
  REQUIRE(scene.flora_tail());
  const u32 records = scene.deform_capacity();
  CHECK(copies_that_differ(scene) == 0u);
  // More palms than the parts' room and fewer than the records': the pose table grows, the deform
  // table keeps its size and is written again, and the copies with it.
  const u32 count = part_room + part_room / 2;
  Vector<SceneInstance> tail;
  for (u32 i = 0; i < count; ++i) {
    SceneInstance placed;
    placed.mesh = palm;
    placed.origin = WorldPos{100.0 + static_cast<f64>(i), 0.0, 0.0};
    tail.push_back(placed);
  }
  DynamicBlock block;
  block.key = 1;
  block.first = 0;
  block.count = count;
  const DynamicBlock blocks[] = {block};
  GpuScene::TileChange change;
  REQUIRE_MESSAGE(scene.set_dynamic_instances(
                      std::span<const SceneInstance>(tail.data(), tail.size()),
                      std::span<const DynamicBlock>(blocks, 1), change, nullptr, &error),
                  error);
  REQUIRE(scene.relayout_pending());
  REQUIRE_MESSAGE(scene.relayout(&error), error);
  MESSAGE("palms " << count << " of " << parts << " parts (a run of " << (1u << cls)
                   << "), records " << scene.deform_descs().size() << " in room for " << records
                   << " then " << scene.deform_capacity());
  CHECK(scene.deform_capacity() == records);  // the records kept their room
  CHECK(scene.deform_descs().size() > scene.load_deform_count());
  CHECK(copies_that_differ(scene) == 0u);
  scene.destroy();
  device.destroy();
}

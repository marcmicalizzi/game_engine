// Meshes that come and go (docs/subsystems/renderer.md, "Meshes that come and go"; roadmap R142):
// the GPU scene's room for a tile's mesh, against the fixture rock generator (tests/support/
// tile_mesh_fixture.h), which builds a different rock for every recipe.
//
// What the cases hold: a mesh is not in the room before it is admitted and is gone after it is
// retired, in bytes; a tail may not name a slot with no mesh; a streamed scene whose rocks arrived
// one by one is the same scene read whole, pixel for pixel — depth and colour to the byte, and the
// ids once each is turned into (instance of the read, cluster of its mesh, triangle) — and is the
// same again when the rocks arrived in another order, through other slots, after a rock that came
// and went; the room's refusal is a sentence that names its bound and changes nothing; and an
// admitted rock is traced like the prefix (the ray path's ids against the rasterizer's).
#include <domain/gfx/device.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <span>
#include <string>
#include <tile_mesh_fixture.h>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 320;
constexpr u32 k_height = 200;
constexpr u32 k_grid = 24;  // a rock's quads a side: 1,152 triangles, a dozen clusters and a DAG

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    test::register_tile_rocks();
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      why = "no Vulkan device: " + error;
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    ok = true;
  }
  bool traces() const {
    return device.features().cluster_acceleration_structure && device.features().ray_query;
  }
};

// The scene's own mesh, and its one instance: a rock stretched into a slab the tiles' rocks stand
// on, large enough that the scene's bounds — what the lights and every bias scale with — are its
// own whether or not the rocks are in the load.
scene_gen::PlacementMesh slab() {
  scene_gen::PlacementMesh mesh;
  mesh.path = test::tile_rock_path(100, 100);
  mesh.name = "slab";
  mesh.recipe = test::tile_rock_recipe(100, 100, k_grid);
  return mesh;
}

scene_gen::PlacementMesh rock(u32 tile) {
  const i32 x = static_cast<i32>(tile % 2);
  const i32 z = static_cast<i32>(tile / 2);
  scene_gen::PlacementMesh mesh;
  mesh.path = test::tile_rock_path(x, z);
  mesh.name = "rock";
  mesh.recipe = test::tile_rock_recipe(x, z, k_grid);
  return mesh;
}

// Tile `tile`'s two instances of `mesh`: a function of the tile alone.
void place(u32 tile, u32 mesh, Vector<SceneInstance>& out) {
  const f64 x = static_cast<f64>(tile % 2) * 9.0 - 4.5;
  const f64 z = static_cast<f64>(tile / 2) * 9.0 - 4.5;
  for (u32 k = 0; k < 2; ++k) {
    SceneInstance placed;
    placed.mesh = mesh;
    placed.origin = WorldPos{x + 3.0 * k, 0.4, z + 2.5 * k};
    if (k == 1) {
      placed.transform.rotation = quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, 0.7f);
      placed.transform.scale = Vec3{0.6f, 0.6f, 0.6f};
    }
    out.push_back(placed);
  }
}

constexpr u32 k_tiles = 4;

// The scene's description: the slab alone (a streamed scene's load), or with every tile's rock and
// its instances (the same scene read whole).
SceneDesc describe(const test::TempDir& tmp, bool whole) {
  SceneDesc desc;
  const scene_gen::PlacementMesh base = slab();
  desc.meshes.push_back(base.path);
  desc.recipes.push_back(MeshRecipe{base.path, base.recipe});
  // A second mesh the load places nowhere, in both scenes: a scene of one mesh is not merged
  // (its clusters keep their container's order), so without it the slab's clusters would be
  // numbered one way in the streamed scene's load and another in the whole read's.
  const scene_gen::PlacementMesh spare = rock(k_tiles);
  desc.meshes.push_back(spare.path);
  desc.recipes.push_back(MeshRecipe{spare.path, spare.recipe});
  SceneInstance ground;
  ground.mesh = 0;
  ground.transform.scale = Vec3{9.0f, 0.15f, 9.0f};
  desc.instances.push_back(ground);
  if (whole) {
    for (u32 t = 0; t < k_tiles; ++t) {
      const scene_gen::PlacementMesh mesh = rock(t);
      desc.meshes.push_back(mesh.path);
      desc.recipes.push_back(MeshRecipe{mesh.path, mesh.recipe});
      place(t, 2 + t, desc.instances);
    }
  }
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  return desc;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;
  // What the tail last handed over names, in the tail's order: each instance's rock.
  Vector<SceneInstance> tail;
  Vector<DynamicBlock> blocks;
  u32 slot_of_tile[k_tiles] = {~0u, ~0u, ~0u, ~0u};

  bool build(const gfx::Device& device, const SceneDesc& desc, RenderSettings settings,
             bool streamed) {
    if (!load_scene(desc, data, error)) return false;
    if (streamed) {
      // What the reader sets for a world one of whose generators hands meshes with its tiles.
      data.dynamic = true;
      data.tile_meshes = true;
    }
    resolve_settings(settings, device.features(), &data, resolved);
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    return renderer.create(device, scene, resolved, rd, &error);
  }

  bool admit(u32 tile, u32 slot) {
    PlacementMeshData built;
    if (!load_placement_mesh(rock(tile), data.mesh_options, built, error)) return false;
    if (!renderer.admit_mesh(scene.mesh_room().first_mesh + slot, built, &error)) return false;
    slot_of_tile[tile] = slot;
    return true;
  }

  // The tail: every tile in `order` whose rock is admitted, a block a tile keyed by it.
  bool hand_over(std::span<const u32> order) {
    tail.clear();
    blocks.clear();
    for (const u32 t : order) {
      DynamicBlock block;
      block.key = 1000 + t;
      block.first = tail.size();
      place(t, scene.mesh_room().first_mesh + slot_of_tile[t], tail);
      block.count = tail.size() - block.first;
      blocks.push_back(block);
    }
    return renderer.set_dynamic_instances(
        std::span<const SceneInstance>(tail.data(), tail.size()),
        std::span<const DynamicBlock>(blocks.data(), blocks.size()), &error);
  }

  bool shoot(CapturedFrame& out) {
    FrameDesc frame;
    frame.camera.position = WorldPos{-9.0, 13.0, 17.0};
    frame.camera.target = WorldPos{1.0, 0.5, 1.0};
    CaptureChannels channels;
    channels.color = true;
    channels.ids = true;
    channels.depth = true;
    return renderer.capture(frame, channels, out, &error);
  }
};

struct Difference {
  u32 depth = 0;
  u32 color = 0;
  u32 ids = 0;
  u32 rock_pixels = 0;
};

// A captured id as (instance of the whole read, cluster of its mesh, triangle): the whole read's
// instances are the slab and then the tiles' in tile order, which is the order every tail here is
// handed in; a streamed scene's slot is turned through the tail's own index.
struct Names {
  const Rig* rig = nullptr;
  Vector<u32> tail_index;
  explicit Names(const Rig& r) : rig(&r) {
    if (r.scene.dynamic()) r.scene.tail_indices(tail_index);
  }
  bool name(const u32* id, u32 out[3]) const {
    out[0] = out[1] = out[2] = k_no_id;
    if (id[0] == k_no_id) return true;
    const u32 prefix = rig->scene.static_instance_count();
    u32 mesh = 0;
    if (!rig->scene.dynamic() || id[0] < prefix) {
      if (id[0] >= rig->data.instances.size()) return false;
      out[0] = id[0];
      mesh = rig->data.instances[id[0]].mesh;
    } else {
      if (id[0] >= tail_index.size() || tail_index[id[0]] == ~0u) return false;
      const u32 at = tail_index[id[0]];
      if (at >= rig->tail.size()) return false;
      out[0] = prefix + at;
      mesh = rig->tail[at].mesh;
    }
    const u32 first = rig->scene.mesh_first_cluster(mesh);
    if (id[1] < first || id[1] - first >= rig->scene.mesh_cluster_count(mesh)) return false;
    out[1] = id[1] - first;
    out[2] = id[2];
    return true;
  }
};

Difference compare(const CapturedFrame& a, const Rig& ra, const CapturedFrame& b, const Rig& rb) {
  Difference d;
  const Names na(ra);
  const Names nb(rb);
  for (u32 p = 0; p < a.width * a.height; ++p) {
    d.depth += a.depth[p] != b.depth[p] ? 1u : 0u;
    bool colour = false;
    for (u32 c = 0; c < 4; ++c)
      colour = colour || a.color[u64{p} * 4 + c] != b.color[u64{p} * 4 + c];
    d.color += colour ? 1u : 0u;
    u32 ia[3];
    u32 ib[3];
    const bool named =
        na.name(&a.ids[u64{p} * k_id_words], ia) && nb.name(&b.ids[u64{p} * k_id_words], ib);
    d.ids += !named || ia[0] != ib[0] || ia[1] != ib[1] || ia[2] != ib[2] ? 1u : 0u;
    d.rock_pixels += named && ia[0] != k_no_id && ia[0] >= 1 ? 1u : 0u;
  }
  return d;
}

RenderSettings plain() {
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.occlusion = false;
  return settings;
}

}  // namespace

TEST_CASE("mesh room: a mesh is not resident before it is admitted and is gone after, in bytes") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_mesh_room"};
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, describe(tmp, false), plain(), true), rig.error);
  const GpuScene::MeshRoom room = rig.scene.mesh_room();
  REQUIRE(room.slots != 0);
  REQUIRE(room.clusters != 0);
  CHECK(room.first_mesh == rig.data.parts.size());
  // Nothing of a rock exists yet: the room is reserved, and empty.
  GpuScene::MeshUse use = rig.scene.mesh_use();
  CHECK(use.meshes == 0);
  CHECK(use.clusters == 0);
  CHECK(use.bytes == 0);
  CHECK(use.largest_free == room.clusters);
  CHECK_FALSE(rig.scene.mesh_resident(room.first_mesh));

  // A tail may not name a slot no mesh was admitted into.
  rig.slot_of_tile[0] = 0;
  const u32 one[1] = {0};
  CHECK_FALSE(rig.hand_over(one));
  CHECK(rig.error.find("no mesh admitted") != std::string::npos);
  rig.slot_of_tile[0] = ~0u;

  PlacementMeshData built;
  REQUIRE_MESSAGE(load_placement_mesh(rock(0), rig.data.mesh_options, built, rig.error), rig.error);
  const u32 clusters = built.lod.mesh.clusters.size();
  REQUIRE(clusters > 1);
  REQUIRE_MESSAGE(rig.admit(0, 0), rig.error);
  use = rig.scene.mesh_use();
  CHECK(use.meshes == 1);
  CHECK(use.clusters == clusters);
  CHECK(use.bytes == u64{clusters} * room.cluster_bytes);
  CHECK(use.payload_bytes > 0);
  CHECK(use.payload_bytes <= use.bytes + 4096);
  CHECK(rig.scene.mesh_resident(room.first_mesh));
  CHECK(rig.scene.mesh_cluster_count(room.first_mesh) == clusters);
  MESSAGE("a rock of " << clusters << " clusters: " << use.payload_bytes << " bytes staged, "
                       << use.bytes << " reserved of the room's " << room.bytes);
  // A slot holds one mesh.
  CHECK_FALSE(rig.renderer.admit_mesh(room.first_mesh, built, &rig.error));
  CHECK(rig.scene.mesh_use().meshes == 1);

  REQUIRE_MESSAGE(rig.hand_over(one), rig.error);
  CapturedFrame with;
  REQUIRE_MESSAGE(rig.shoot(with), rig.error);
  u32 drawn = 0;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    const u32 id = with.ids[u64{p} * k_id_words];
    drawn += id != k_no_id && id >= rig.scene.static_instance_count() ? 1u : 0u;
  }
  CHECK(drawn > 500);
  std::string why;
  CHECK_MESSAGE(rig.scene.validate_tables(&why), why);

  // Its tile goes, and then the mesh: the room is as it was, to the cluster.
  REQUIRE_MESSAGE(rig.hand_over({}), rig.error);
  REQUIRE(rig.renderer.retire_mesh(room.first_mesh, &rig.error));
  use = rig.scene.mesh_use();
  CHECK(use.meshes == 0);
  CHECK(use.clusters == 0);
  CHECK(use.bytes == 0);
  CHECK(use.payload_bytes == 0);
  CHECK(use.materials == 0);
  CHECK(use.largest_free == room.clusters);
  CHECK(use.admitted == 1);
  CHECK(use.retired == 1);
  CapturedFrame without;
  REQUIRE_MESSAGE(rig.shoot(without), rig.error);
  for (u32 p = 0; p < k_width * k_height; ++p) {
    const u32 id = without.ids[u64{p} * k_id_words];
    REQUIRE((id == k_no_id || id < rig.scene.static_instance_count()));
  }
}

TEST_CASE(
    "mesh room: a streamed scene is the scene read whole, whatever order its meshes came in") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  test::TempDir tmp{"engine_renderer_mesh_room_whole"};
  Rig whole;
  REQUIRE_MESSAGE(whole.build(gpu.device, describe(tmp, true), plain(), false), whole.error);
  Rig first;  // the rocks in tile order, into the slots in order
  REQUIRE_MESSAGE(first.build(gpu.device, describe(tmp, false), plain(), true), first.error);
  Rig second;  // another walk to the same place
  REQUIRE_MESSAGE(second.build(gpu.device, describe(tmp, false), plain(), true), second.error);
  const u32 in_order[k_tiles] = {0, 1, 2, 3};

  for (u32 t = 0; t < k_tiles; ++t)
    REQUIRE_MESSAGE(first.admit(t, t), first.error);
  REQUIRE_MESSAGE(first.hand_over(in_order), first.error);

  // The other walk: tile 3 first, drawn; then tile 1 comes and goes and its room is taken by tile
  // 0's rock; tiles 2 and 1 last, into whatever is free — other slots, other runs of the arena.
  CapturedFrame scratch;
  const u32 only3[1] = {3};
  REQUIRE_MESSAGE(second.admit(3, 2), second.error);
  REQUIRE_MESSAGE(second.hand_over(only3), second.error);
  REQUIRE_MESSAGE(second.shoot(scratch), second.error);
  REQUIRE_MESSAGE(second.admit(1, 0), second.error);
  const u32 three_one[2] = {1, 3};
  REQUIRE_MESSAGE(second.hand_over(three_one), second.error);
  REQUIRE_MESSAGE(second.shoot(scratch), second.error);
  REQUIRE_MESSAGE(second.hand_over(only3), second.error);
  REQUIRE(second.renderer.retire_mesh(second.scene.mesh_room().first_mesh + 0, &second.error));
  REQUIRE_MESSAGE(second.admit(0, 0), second.error);
  REQUIRE_MESSAGE(second.admit(2, 5), second.error);
  REQUIRE_MESSAGE(second.admit(1, 1), second.error);
  REQUIRE_MESSAGE(second.hand_over(in_order), second.error);
  // The two walks hold their rocks in different places.
  CHECK(
      first.scene.mesh_first_cluster(first.scene.mesh_room().first_mesh + first.slot_of_tile[3]) !=
      second.scene.mesh_first_cluster(second.scene.mesh_room().first_mesh +
                                      second.slot_of_tile[3]));

  CapturedFrame w;
  CapturedFrame a;
  CapturedFrame b;
  REQUIRE_MESSAGE(whole.shoot(w), whole.error);
  REQUIRE_MESSAGE(first.shoot(a), first.error);
  REQUIRE_MESSAGE(second.shoot(b), second.error);
  const Difference streamed = compare(w, whole, a, first);
  MESSAGE("read whole against streamed: " << streamed.depth << " depths, " << streamed.color
                                          << " colours, " << streamed.ids << " ids over "
                                          << streamed.rock_pixels << " rock pixels");
  CHECK(streamed.rock_pixels > 4000);
  CHECK(streamed.depth == 0);
  CHECK(streamed.color == 0);
  CHECK(streamed.ids == 0);
  const Difference walks = compare(a, first, b, second);
  MESSAGE("one walk against the other: " << walks.depth << " depths, " << walks.color
                                         << " colours, " << walks.ids << " ids");
  CHECK(walks.depth == 0);
  CHECK(walks.color == 0);
  CHECK(walks.ids == 0);
  std::string why;
  CHECK_MESSAGE(first.scene.validate_tables(&why), why);
  CHECK_MESSAGE(second.scene.validate_tables(&why), why);
}

TEST_CASE("mesh room: the room's refusal names its bound and changes nothing") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  struct Restore {
    ~Restore() {
      (void)tunables::apply_overrides("renderer.meshes.budget_mib=256");
      (void)tunables::apply_overrides("renderer.meshes.max_clusters=4096");
    }
  } restore;
  test::TempDir tmp{"engine_renderer_mesh_room_budget"};
  {
    // A megabyte holds a few rocks and no more.
    REQUIRE(tunables::apply_overrides("renderer.meshes.budget_mib=1"));
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, describe(tmp, false), plain(), true), rig.error);
    const GpuScene::MeshRoom room = rig.scene.mesh_room();
    CHECK(room.bytes <= (u64{1} << 20) + u64{room.slots} * 64 + u64{room.materials} * 112);
    PlacementMeshData built;
    REQUIRE_MESSAGE(load_placement_mesh(rock(0), rig.data.mesh_options, built, rig.error),
                    rig.error);
    const u32 clusters = built.lod.mesh.clusters.size();
    const u32 fit = room.clusters / clusters;
    REQUIRE(fit >= 1);
    REQUIRE(fit + 1 < room.slots);
    for (u32 s = 0; s < fit; ++s) {
      CHECK(rig.scene.mesh_fits(clusters, GpuScene::mesh_materials(built)));
      REQUIRE_MESSAGE(rig.renderer.admit_mesh(room.first_mesh + s, built, &rig.error), rig.error);
    }
    const GpuScene::MeshUse full = rig.scene.mesh_use();
    CHECK(full.meshes == fit);
    CHECK_FALSE(rig.scene.mesh_fits(clusters, GpuScene::mesh_materials(built)));
    std::string refused;
    CHECK_FALSE(rig.renderer.admit_mesh(room.first_mesh + fit, built, &refused));
    MESSAGE("refused: " << refused);
    CHECK(refused.find("renderer.meshes.budget_mib") != std::string::npos);
    const GpuScene::MeshUse after = rig.scene.mesh_use();
    CHECK(after.meshes == full.meshes);
    CHECK(after.clusters == full.clusters);
    CHECK(after.materials == full.materials);
    CHECK_FALSE(rig.scene.mesh_resident(room.first_mesh + fit));
    // One goes, and the refused one has room.
    REQUIRE(rig.renderer.retire_mesh(room.first_mesh, &rig.error));
    CHECK_MESSAGE(rig.renderer.admit_mesh(room.first_mesh + fit, built, &refused), refused);
  }
  {
    // A mesh past the most one may have is refused by name, whatever is free.
    REQUIRE(tunables::apply_overrides("renderer.meshes.budget_mib=256"));
    REQUIRE(tunables::apply_overrides("renderer.meshes.max_clusters=2"));
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, describe(tmp, false), plain(), true), rig.error);
    PlacementMeshData built;
    REQUIRE_MESSAGE(load_placement_mesh(rock(1), rig.data.mesh_options, built, rig.error),
                    rig.error);
    std::string refused;
    CHECK_FALSE(rig.renderer.admit_mesh(rig.scene.mesh_room().first_mesh, built, &refused));
    CHECK(refused.find("renderer.meshes.max_clusters") != std::string::npos);
    CHECK(rig.scene.mesh_use().meshes == 0);
  }
  {
    // A scene none of whose generators hands a mesh with a tile reserves nothing, and says so.
    Rig rig;
    SceneDesc desc = describe(tmp, false);
    REQUIRE(load_scene(desc, rig.data, rig.error));
    rig.data.dynamic = true;
    resolve_settings(plain(), gpu.device.features(), &rig.data, rig.resolved);
    REQUIRE_MESSAGE(rig.scene.create(gpu.device, rig.data, rig.resolved, &rig.error), rig.error);
    CHECK(rig.scene.mesh_room().slots == 0);
    CHECK(rig.scene.mesh_room().bytes == 0);
    PlacementMeshData built;
    REQUIRE_MESSAGE(load_placement_mesh(rock(1), rig.data.mesh_options, built, rig.error),
                    rig.error);
    std::string refused;
    CHECK_FALSE(rig.scene.admit_mesh(rig.data.parts.size(), built, &refused));
    CHECK(refused.find("no room") != std::string::npos);
  }
}

TEST_CASE("mesh room: an admitted mesh is traced like the prefix") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  if (!gpu.traces()) {
    MESSAGE(gpu.device.adapter().name << " has no cluster acceleration structures; skipped");
    return;
  }
  test::TempDir tmp{"engine_renderer_mesh_room_rt"};
  RenderSettings raster;
  raster.raster = RasterMode::Hardware;
  raster.shadows = ShadowMode::RayTraced;
  raster.shadow_casters = true;
  raster.occlusion = false;
  RenderSettings traced = raster;
  traced.raster = RasterMode::RayTrace;
  Rig r;
  REQUIRE_MESSAGE(r.build(gpu.device, describe(tmp, false), raster, true), r.error);
  Rig t;
  REQUIRE_MESSAGE(t.build(gpu.device, describe(tmp, false), traced, true), t.error);
  REQUIRE(r.resolved.rt_chain);
  REQUIRE(t.resolved.ray_path);
  const u32 in_order[k_tiles] = {0, 1, 2, 3};
  for (Rig* rig : {&r, &t}) {
    for (u32 tile = 0; tile < k_tiles; ++tile)
      REQUIRE_MESSAGE(rig->admit(tile, tile), rig->error);
    REQUIRE_MESSAGE(rig->hand_over(in_order), rig->error);
  }
  CapturedFrame rs;
  CapturedFrame ts;
  REQUIRE_MESSAGE(r.shoot(rs), r.error);
  REQUIRE_MESSAGE(t.shoot(ts), t.error);
  // The ray path's cut over the admitted rocks is the rasterizer's, held as the streamed tail's
  // is (streamed_tail_tests.cpp): where both hit a rock, the same instance and cluster; coverage
  // within 1 % of the rocks' pixels and triangles within 2 %, which is what a ray and a rasterized
  // edge disagree about on any mesh.
  u32 pixels = 0;
  u32 coverage = 0;
  u32 words = 0;
  u32 triangles = 0;
  const u32 prefix = r.scene.static_instance_count();
  for (u32 p = 0; p < k_width * k_height; ++p) {
    const u32* ri = &rs.ids[u64{p} * k_id_words];
    const u32* ti = &ts.ids[u64{p} * k_id_words];
    const bool r_rock = ri[0] != k_no_id && ri[0] >= prefix;
    const bool t_rock = ti[0] != k_no_id && ti[0] >= prefix;
    if (!r_rock && !t_rock) continue;
    ++pixels;
    if (r_rock != t_rock) {
      ++coverage;
    } else if (ri[0] != ti[0] || ri[1] != ti[1]) {
      ++words;
    } else if (ri[2] != ti[2]) {
      ++triangles;
    }
  }
  MESSAGE("admitted rocks traced against rasterized over "
          << pixels << " pixels: " << coverage << " coverage, " << words << " instance or cluster, "
          << triangles << " triangle differences");
  CHECK(pixels > 4000);
  CHECK(words == 0);
  CHECK(static_cast<f64>(coverage) < 0.01 * static_cast<f64>(pixels));
  CHECK(static_cast<f64>(triangles) < 0.02 * static_cast<f64>(pixels));
  // And they cast: with traced shadows on, some of the slab is darker than the slab alone.
  Rig bare;
  REQUIRE_MESSAGE(bare.build(gpu.device, describe(tmp, false), raster, true), bare.error);
  CapturedFrame ground;
  REQUIRE_MESSAGE(bare.shoot(ground), bare.error);
  u32 darker = 0;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    const u32 id = rs.ids[u64{p} * k_id_words];
    if (id == k_no_id || id >= prefix) continue;
    const u32 with = rs.color[u64{p} * 4] + rs.color[u64{p} * 4 + 1] + rs.color[u64{p} * 4 + 2];
    const u32 without =
        ground.color[u64{p} * 4] + ground.color[u64{p} * 4 + 1] + ground.color[u64{p} * 4 + 2];
    darker += with + 30 < without ? 1u : 0u;
  }
  MESSAGE("shadowed slab pixels: " << darker);
  CHECK(darker > 20);
}

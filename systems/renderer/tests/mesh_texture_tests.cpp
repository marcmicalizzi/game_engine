// Textures that arrive after the load (docs/subsystems/texture.md, "Textures that arrive after the
// load"; roadmap R147): a tile's mesh admitted with its textures, against the fixture rock
// generator's looks (tests/support/tile_mesh_fixture.h) — a base colour map and a normal map on a
// rock's first material, an alpha-tested map on its second.
//
// What the cases hold: a streamed scene whose textured rocks arrived one by one is the scene read
// whole, to the byte, rasterized, whatever order they came in, and is not the same scene drawn
// from its materials' constants; a texture two meshes share is uploaded once and leaves after the
// second, its image a few recorded frames later, in bytes; a texture let go and wanted again
// before then comes back without an upload; the room's refusal names its bound and the mesh draws
// its constants; an alpha-tested rock draws its holes, as the whole read does, only in a scene
// told to expect one; and on the ray path the textured rocks are the whole read's.
#include <core/memory/memory.h>
#include <core/time/time.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resources.h>
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
constexpr u32 k_grid = 24;
constexpr u32 k_tiles = 4;

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
    if (!device.features().texture_compression_bc) {
      why = std::string(device.adapter().name) + " has no textureCompressionBC";
      return;
    }
    ok = true;
  }
  bool traces() const {
    return device.features().cluster_acceleration_structure && device.features().ray_query;
  }
};

scene_gen::PlacementMesh slab() {
  scene_gen::PlacementMesh mesh;
  mesh.path = test::tile_rock_path(100, 100);
  mesh.name = "slab";
  mesh.recipe = test::tile_rock_recipe(100, 100, k_grid);
  return mesh;
}

// Tile `tile`'s rock with `look`: tiles 0 and 2 are dealt one base colour map and 1 and 3 another,
// and all four share the normal map — three textures for four rocks.
scene_gen::PlacementMesh rock(u32 tile, u32 look) {
  const i32 x = static_cast<i32>(tile % 2);
  const i32 z = static_cast<i32>(tile / 2);
  scene_gen::PlacementMesh mesh;
  mesh.path = test::tile_rock_path(x, z);
  mesh.name = "rock";
  const u32 dealt = (look & test::k_tile_rock_textured) != 0
                        ? look | ((tile % 2) << test::k_tile_rock_set_shift)
                        : look;
  mesh.recipe = test::tile_rock_recipe(x, z, k_grid, dealt);
  return mesh;
}

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

// The slab and a spare (so both loads are merged, as mesh_room_tests.cpp explains), and with
// `whole` every tile's rock and its instances.
SceneDesc describe(const test::TempDir& tmp, bool whole, u32 look) {
  SceneDesc desc;
  const scene_gen::PlacementMesh base = slab();
  desc.meshes.push_back(base.path);
  desc.recipes.push_back(MeshRecipe{base.path, base.recipe});
  scene_gen::PlacementMesh spare;
  spare.path = test::tile_rock_path(50, 50);
  spare.recipe = test::tile_rock_recipe(50, 50, k_grid);
  desc.meshes.push_back(spare.path);
  desc.recipes.push_back(MeshRecipe{spare.path, spare.recipe});
  SceneInstance ground;
  ground.mesh = 0;
  ground.transform.scale = Vec3{9.0f, 0.15f, 9.0f};
  desc.instances.push_back(ground);
  if (whole) {
    for (u32 t = 0; t < k_tiles; ++t) {
      const scene_gen::PlacementMesh mesh = rock(t, look);
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
  u32 look = 0;
  Vector<SceneInstance> tail;
  Vector<DynamicBlock> blocks;
  u32 slot_of_tile[k_tiles] = {~0u, ~0u, ~0u, ~0u};

  bool build(const gfx::Device& device, const SceneDesc& desc, RenderSettings settings,
             bool streamed, u32 rocks_look, bool masked = false) {
    look = rocks_look;
    if (!load_scene(desc, data, error)) return false;
    if (streamed) {
      // What the reader sets for a world one of whose generators hands meshes with its tiles —
      // and, with `masked`, says they may be alpha-tested (`tile_meshes_masked`).
      data.dynamic = true;
      data.tile_meshes = true;
      data.tile_meshes_masked = masked;
    }
    resolve_settings(settings, device.features(), &data, resolved);
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    return renderer.create(device, scene, resolved, rd, &error);
  }

  // A tile's rock built with its textures, off any device.
  bool make(u32 tile, PlacementMeshData& built) {
    PlacementMeshOptions options = data.mesh_options;
    options.textures = true;
    return load_placement_mesh(rock(tile, look), options, built, error);
  }

  // The world's order: every texture in, then the mesh, then the caller's own references back.
  // `textures` false admits the mesh without them, as a refusal leaves it.
  bool admit(u32 tile, u32 slot, bool textures = true) {
    PlacementMeshData built;
    if (!make(tile, built)) return false;
    if (!built.textures_why.empty()) {
      error = built.textures_why;
      return false;
    }
    if (!textures) built.textures.clear();
    for (const PlacementTexture& texture : built.textures) {
      if (renderer.acquire_texture(texture, &error) != GpuScene::TextureAdmit::held) return false;
    }
    const bool ok = renderer.admit_mesh(scene.mesh_room().first_mesh + slot, built, &error);
    for (const PlacementTexture& texture : built.textures)
      renderer.release_texture(texture.key);
    if (ok) slot_of_tile[tile] = slot;
    return ok;
  }

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
  u32 rock_color = 0;  // colour differences on pixels both name a rock at
};

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
    const bool rock = named && ia[0] != k_no_id && ia[0] >= 1;
    d.rock_pixels += rock ? 1u : 0u;
    d.rock_color += rock && ib[0] == ia[0] && colour ? 1u : 0u;
  }
  return d;
}

// Pixels of `frame` that name a rock (anything but the slab, instance 0).
u32 rock_pixels(const CapturedFrame& frame) {
  u32 count = 0;
  for (u32 p = 0; p < frame.width * frame.height; ++p) {
    const u32 id = frame.ids[u64{p} * k_id_words];
    count += id != k_no_id && id >= 1 ? 1u : 0u;
  }
  return count;
}

RenderSettings plain() {
  RenderSettings settings;
  settings.shadows = ShadowMode::Off;
  settings.occlusion = false;
  return settings;
}

const u32 k_in_order[k_tiles] = {0, 1, 2, 3};

}  // namespace

TEST_CASE("mesh textures: a textured tile mesh is the scene read whole, in any order") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const u32 look = test::k_tile_rock_textured;
  test::TempDir tmp{"engine_renderer_mesh_textures_whole"};
  Rig whole;
  REQUIRE_MESSAGE(whole.build(gpu.device, describe(tmp, true, look), plain(), false, look),
                  whole.error);
  // The whole read drew built textures: three distinct ones for its four rocks.
  CHECK(whole.scene.textures_built() == 3);
  CHECK(whole.scene.textures_decoded() == 0);
  Rig first;
  REQUIRE_MESSAGE(first.build(gpu.device, describe(tmp, false, look), plain(), true, look),
                  first.error);
  Rig second;
  REQUIRE_MESSAGE(second.build(gpu.device, describe(tmp, false, look), plain(), true, look),
                  second.error);
  REQUIRE(first.scene.texture_room().pairs != 0);

  for (u32 t = 0; t < k_tiles; ++t)
    REQUIRE_MESSAGE(first.admit(t, t), first.error);
  REQUIRE_MESSAGE(first.hand_over(k_in_order), first.error);
  // Four rocks, eight references, three uploads: two base colour maps and the one normal map.
  const GpuScene::TextureUse use = first.scene.texture_use();
  CHECK(use.textures == 3);
  CHECK(use.uploaded == 3);
  CHECK(use.references == 8);
  CHECK(use.bytes > 0);

  // The other walk: tile 3 first and drawn; tile 1 comes and goes — its base colour map with it,
  // since nobody else holds that one yet — and comes back last, after its room went to tile 0.
  CapturedFrame scratch;
  const u32 only3[1] = {3};
  REQUIRE_MESSAGE(second.admit(3, 2), second.error);
  REQUIRE_MESSAGE(second.hand_over(only3), second.error);
  REQUIRE_MESSAGE(second.shoot(scratch), second.error);
  REQUIRE_MESSAGE(second.admit(0, 0), second.error);
  const u32 zero_three[2] = {0, 3};
  REQUIRE_MESSAGE(second.hand_over(zero_three), second.error);
  REQUIRE_MESSAGE(second.shoot(scratch), second.error);
  REQUIRE_MESSAGE(second.hand_over(only3), second.error);
  REQUIRE(second.renderer.retire_mesh(second.scene.mesh_room().first_mesh + 0, &second.error));
  CHECK(second.scene.texture_use().leaving == 1);  // tile 0's base colour map
  for (u32 f = 0; f < 5; ++f)
    REQUIRE_MESSAGE(second.shoot(scratch), second.error);
  CHECK(second.scene.texture_use().leaving == 0);
  REQUIRE_MESSAGE(second.admit(2, 0), second.error);
  REQUIRE_MESSAGE(second.admit(1, 5), second.error);
  REQUIRE_MESSAGE(second.admit(0, 1), second.error);
  REQUIRE_MESSAGE(second.hand_over(k_in_order), second.error);
  CHECK(second.scene.texture_use().textures == 3);
  CHECK(second.scene.texture_use().uploaded == 4);  // tile 0's map went and came back

  CapturedFrame w;
  CapturedFrame a;
  CapturedFrame b;
  REQUIRE_MESSAGE(whole.shoot(w), whole.error);
  REQUIRE_MESSAGE(first.shoot(a), first.error);
  REQUIRE_MESSAGE(second.shoot(b), second.error);
  const Difference streamed = compare(w, whole, a, first);
  MESSAGE("textured, read whole against streamed: "
          << streamed.depth << " depths, " << streamed.color << " colours, " << streamed.ids
          << " ids over " << streamed.rock_pixels << " rock pixels");
  CHECK(streamed.rock_pixels > 4000);
  CHECK(streamed.depth == 0);
  CHECK(streamed.color == 0);
  CHECK(streamed.ids == 0);
  const Difference walks = compare(a, first, b, second);
  MESSAGE("textured, one walk against the other: " << walks.depth << " depths, " << walks.color
                                                   << " colours, " << walks.ids << " ids");
  CHECK(walks.depth == 0);
  CHECK(walks.color == 0);
  CHECK(walks.ids == 0);

  // And it is the textures that are drawn: the same rocks admitted without them draw their
  // materials' constants, which is another picture on the textured halves and the same geometry.
  Rig bare;
  REQUIRE_MESSAGE(bare.build(gpu.device, describe(tmp, false, look), plain(), true, look),
                  bare.error);
  for (u32 t = 0; t < k_tiles; ++t)
    REQUIRE_MESSAGE(bare.admit(t, t, false), bare.error);
  REQUIRE_MESSAGE(bare.hand_over(k_in_order), bare.error);
  CHECK(bare.scene.texture_use().textures == 0);
  CapturedFrame c;
  REQUIRE_MESSAGE(bare.shoot(c), bare.error);
  const Difference constants = compare(a, first, c, bare);
  MESSAGE("textured against its constants: " << constants.rock_color << " of "
                                             << constants.rock_pixels << " rock pixels differ");
  CHECK(constants.depth == 0);
  CHECK(constants.ids == 0);
  CHECK(constants.rock_color > 1000);
  std::string why;
  CHECK_MESSAGE(first.scene.validate_tables(&why), why);
  CHECK_MESSAGE(second.scene.validate_tables(&why), why);
}

TEST_CASE("mesh textures: a shared texture is uploaded once and leaves after the last mesh") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const u32 look = test::k_tile_rock_textured;
  test::TempDir tmp{"engine_renderer_mesh_textures_shared"};
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, describe(tmp, false, look), plain(), true, look),
                  rig.error);
  const GpuScene::TextureRoom room = rig.scene.texture_room();
  REQUIRE(room.pairs != 0);
  const u32 first_mesh = rig.scene.mesh_room().first_mesh;
  // Before: nothing of a texture.
  GpuScene::TextureUse use = rig.scene.texture_use();
  CHECK(use.textures == 0);
  CHECK(use.bytes == 0);
  CHECK(use.staged_bytes == 0);

  // Tiles 0 and 2 are dealt the same base colour map, and every rock the one normal map.
  PlacementMeshData built;
  REQUIRE_MESSAGE(rig.make(0, built), rig.error);
  REQUIRE(built.textures.size() == 2);
  CHECK_FALSE(rig.scene.texture_resident(built.textures[0].key));
  const u64 keys[2] = {built.textures[0].key, built.textures[1].key};
  REQUIRE_MESSAGE(rig.admit(0, 0), rig.error);
  use = rig.scene.texture_use();
  CHECK(use.textures == 2);
  CHECK(use.uploaded == 2);
  CHECK(use.references == 2);  // the mesh's; the caller's own went back
  CHECK(rig.scene.texture_resident(keys[0]));
  CHECK(rig.scene.texture_resident(keys[1]));
  const u64 with_bytes = use.bytes;
  const u64 staged = use.staged_bytes;
  CHECK(with_bytes > 0);
  // A build that asks finds them resident and reads nothing.
  {
    PlacementMeshOptions options = rig.data.mesh_options;
    options.textures = true;
    options.texture_context = &rig.scene;
    options.texture_resident = [](void* context, u64 key) {
      return static_cast<const GpuScene*>(context)->texture_resident(key);
    };
    PlacementMeshData asked;
    REQUIRE_MESSAGE(load_placement_mesh(rock(2, look), options, asked, rig.error), rig.error);
    REQUIRE(asked.textures.size() == 2);
    CHECK(asked.textures[0].key == keys[0]);
    CHECK(asked.textures[0].data.levels.empty());
    CHECK(asked.textures[0].staged_bytes() == 0);
  }
  // What an admission costs the committing thread — the frame's, in a windowed host — counted as
  // the frame loop's allocation test counts (renderer.md, "The frame loop allocates nothing"): an
  // admission is not steady state and has its allowance, so these are stated, and bounded by what
  // the code makes (the material records, the slot's keys, the key list's growth): no device
  // buffer while the staging ring has room, since an image is not one.
  PlacementMeshData second_rock;
  REQUIRE_MESSAGE(rig.make(2, second_rock), rig.error);
  const u64 allocations_before = mem::allocations_on_thread();
  const u64 buffers_before = gfx::buffers_created_on_thread();
  for (const PlacementTexture& texture : second_rock.textures) {
    REQUIRE(rig.renderer.acquire_texture(texture, &rig.error) == GpuScene::TextureAdmit::held);
  }
  REQUIRE_MESSAGE(rig.renderer.admit_mesh(first_mesh + 1, second_rock, &rig.error), rig.error);
  for (const PlacementTexture& texture : second_rock.textures)
    rig.renderer.release_texture(texture.key);
  const u64 admission_allocations = mem::allocations_on_thread() - allocations_before;
  const u64 admission_buffers = gfx::buffers_created_on_thread() - buffers_before;
  rig.slot_of_tile[2] = 1;
  MESSAGE("admitting a rock whose two textures are resident: "
          << admission_allocations << " allocations, " << admission_buffers << " device buffers");
  CHECK(admission_buffers == 0);
  CHECK(admission_allocations <= 16);
  use = rig.scene.texture_use();
  CHECK(use.textures == 2);
  CHECK(use.uploaded == 2);  // once
  CHECK(use.references == 4);
  CHECK(use.bytes == with_bytes);
  CHECK(use.staged_bytes == staged);
  MESSAGE("two rocks sharing a base colour map and a normal map: "
          << use.textures << " textures, " << use.bytes << " device bytes, " << use.staged_bytes
          << " staged");
  const u32 both[2] = {0, 2};
  REQUIRE_MESSAGE(rig.hand_over(both), rig.error);
  CapturedFrame frame;
  REQUIRE_MESSAGE(rig.shoot(frame), rig.error);

  // The first mesh goes: the textures stay, held by the second.
  const u32 only2[1] = {2};
  REQUIRE_MESSAGE(rig.hand_over(only2), rig.error);
  REQUIRE(rig.renderer.retire_mesh(first_mesh + 0, &rig.error));
  use = rig.scene.texture_use();
  CHECK(use.textures == 2);
  CHECK(use.references == 2);
  CHECK(use.leaving == 0);
  // The second goes: out of the table at once, their images until the frames in flight are done.
  REQUIRE_MESSAGE(rig.hand_over({}), rig.error);
  REQUIRE(rig.renderer.retire_mesh(first_mesh + 1, &rig.error));
  use = rig.scene.texture_use();
  CHECK(use.textures == 0);
  CHECK(use.references == 0);
  CHECK(use.bytes == 0);
  CHECK(use.leaving == 2);
  CHECK(use.leaving_bytes == with_bytes);
  CHECK(use.released == 2);
  CHECK_FALSE(rig.scene.texture_resident(keys[0]));
  // Wanted again before its image went: back without an upload.
  REQUIRE_MESSAGE(rig.shoot(frame), rig.error);
  REQUIRE_MESSAGE(rig.admit(0, 0), rig.error);
  use = rig.scene.texture_use();
  CHECK(use.textures == 2);
  CHECK(use.uploaded == 2);
  CHECK(use.revived == 2);
  CHECK(use.leaving == 0);
  CHECK(use.bytes == with_bytes);
  REQUIRE(rig.renderer.retire_mesh(first_mesh + 0, &rig.error));
  CHECK(rig.scene.texture_use().leaving == 2);
  // After: one more recorded frame than can be in flight, and nothing of them is left.
  for (u32 f = 0; f < k_joint_slots; ++f) {
    REQUIRE_MESSAGE(rig.shoot(frame), rig.error);
    CHECK(rig.scene.texture_use().leaving == 2);
  }
  REQUIRE_MESSAGE(rig.shoot(frame), rig.error);
  use = rig.scene.texture_use();
  CHECK(use.leaving == 0);
  CHECK(use.leaving_bytes == 0);
  CHECK(use.bytes == 0);
  // Their pairs are free again: the same rocks come back into them.
  REQUIRE_MESSAGE(rig.admit(0, 0), rig.error);
  CHECK(rig.scene.texture_use().uploaded == 4);
  CHECK(rig.scene.texture_use().bytes == with_bytes);
}

TEST_CASE("mesh textures: the room's refusal names its bound and the mesh draws its constants") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  struct Restore {
    ~Restore() {
      (void)tunables::apply_overrides("renderer.textures.slots=1024");
      (void)tunables::apply_overrides("renderer.textures.budget_mib=1024");
    }
  } restore;
  const u32 look = test::k_tile_rock_textured;
  test::TempDir tmp{"engine_renderer_mesh_textures_refused"};
  {
    // One pair: the rock's base colour map takes it, and its normal map is refused by name.
    REQUIRE(tunables::apply_overrides("renderer.textures.slots=1"));
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, describe(tmp, false, look), plain(), true, look),
                    rig.error);
    CHECK(rig.scene.texture_room().pairs == 1);
    PlacementMeshData built;
    REQUIRE_MESSAGE(rig.make(0, built), rig.error);
    REQUIRE(built.textures.size() == 2);
    std::string refused;
    CHECK(rig.renderer.acquire_texture(built.textures[0], &refused) ==
          GpuScene::TextureAdmit::held);
    CHECK(rig.renderer.acquire_texture(built.textures[1], &refused) ==
          GpuScene::TextureAdmit::refused);
    MESSAGE("refused: " << refused);
    CHECK(refused.find("renderer.textures.slots") != std::string::npos);
    CHECK(rig.scene.texture_use().textures == 1);
    CHECK(rig.scene.texture_use().refused == 1);
    // What the world does then: the references back, and the mesh in with its constants.
    rig.renderer.release_texture(built.textures[0].key);
    built.textures.clear();
    built.textures_why = refused;
    REQUIRE_MESSAGE(rig.renderer.admit_mesh(rig.scene.mesh_room().first_mesh, built, &rig.error),
                    rig.error);
    rig.slot_of_tile[0] = 0;
    const u32 one[1] = {0};
    REQUIRE_MESSAGE(rig.hand_over(one), rig.error);
    CapturedFrame frame;
    REQUIRE_MESSAGE(rig.shoot(frame), rig.error);
    CHECK(rock_pixels(frame) > 1000);  // it is drawn
    CHECK(rig.scene.texture_use().references == 0);
  }
  {
    // No budget at all: no room, said with both of its bounds.
    REQUIRE(tunables::apply_overrides("renderer.textures.slots=1024"));
    REQUIRE(tunables::apply_overrides("renderer.textures.budget_mib=0"));
    Rig rig;
    REQUIRE_MESSAGE(rig.build(gpu.device, describe(tmp, false, look), plain(), true, look),
                    rig.error);
    CHECK(rig.scene.texture_room().pairs == 0);
    PlacementMeshData built;
    REQUIRE_MESSAGE(rig.make(0, built), rig.error);
    std::string refused;
    CHECK(rig.renderer.acquire_texture(built.textures[0], &refused) ==
          GpuScene::TextureAdmit::refused);
    CHECK(refused.find("renderer.textures.budget_mib") != std::string::npos);
  }
}

namespace {

// A built texture of `size` x `size` in BC7 with its whole chain, its blocks a pattern nobody
// looks at: what a 2K map costs to admit is its bytes, not its picture.
PlacementTexture synthetic(u32 size, u64 key) {
  PlacementTexture texture;
  texture.key = key;
  texture::TextureData& t = texture.data;
  t.format = texture::TextureFormat::bc7;
  t.color_space = texture::ColorSpace::srgb;
  t.width = t.height = size;
  t.build_key = key;
  u64 offset = 0;
  for (u32 s = size;; s /= 2) {
    texture::TextureFileLevel level;
    level.width = level.height = s;
    level.offset = offset;
    level.bytes = texture::texture_level_bytes(t.format, s, s);
    offset += level.bytes;
    t.levels.push_back(level);
    if (s == 1) break;
  }
  t.data.resize(static_cast<u32>(offset));
  for (u32 i = 0; i < t.data.size(); ++i)
    t.data[i] = static_cast<u8>(i * 31u + (i >> 9));
  return texture;
}

}  // namespace

TEST_CASE("mesh textures: what a 2K texture costs a commit, and the byte budget's refusal") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  struct Restore {
    ~Restore() { (void)tunables::apply_overrides("renderer.textures.budget_mib=1024"); }
  } restore;
  // Eight MiB holds one 2K map (5.33 MiB with its chain) and not two.
  REQUIRE(tunables::apply_overrides("renderer.textures.budget_mib=8"));
  test::TempDir tmp{"engine_renderer_mesh_textures_2k"};
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, describe(tmp, false, 0), plain(), true, 0), rig.error);
  const PlacementTexture first = synthetic(2048, 0x2001);
  const PlacementTexture second = synthetic(2048, 0x2002);
  CHECK(first.staged_bytes() == 5'592'432);  // texture.md's table: 2048 squared, 12 levels
  CapturedFrame frame;
  REQUIRE_MESSAGE(rig.shoot(frame), rig.error);
  // Timed three times over: the first admission of a scene also pays for what it grows.
  i64 best = 0;
  for (u32 turn = 0; turn < 3; ++turn) {
    const i64 start = time::monotonic_ns();
    std::string why;
    REQUIRE_MESSAGE(rig.renderer.acquire_texture(first, &why) == GpuScene::TextureAdmit::held, why);
    const i64 took = time::monotonic_ns() - start;
    best = turn == 0 || took < best ? took : best;
    // The byte budget: a second 2K map is past it, and is refused by its name.
    if (turn == 0) {
      CHECK(rig.renderer.acquire_texture(second, &why) == GpuScene::TextureAdmit::refused);
      MESSAGE("refused: " << why);
      CHECK(why.find("renderer.textures.budget_mib") != std::string::npos);
      CHECK(rig.scene.texture_use().textures == 1);
    }
    REQUIRE_MESSAGE(rig.shoot(frame), rig.error);  // the frame that copies it
    rig.renderer.release_texture(first.key);
    for (u32 f = 0; f <= k_joint_slots; ++f)
      REQUIRE_MESSAGE(rig.shoot(frame), rig.error);
    CHECK(rig.scene.texture_use().leaving == 0);
  }
  const GpuScene::TextureUse use = rig.scene.texture_use();
  CHECK(use.uploaded == 3);
  CHECK(use.staged_bytes == 3 * first.staged_bytes());
  MESSAGE("admitting one 2K BC7 texture (an image made, "
          << first.staged_bytes() << " bytes staged) took the committing thread "
          << static_cast<f64>(best) / 1.0e6 << " ms at best of three, in this build");
}

TEST_CASE(
    "mesh textures: an alpha-tested tile mesh draws its coverage where the scene expects one") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const u32 look = test::k_tile_rock_textured | test::k_tile_rock_masked;
  test::TempDir tmp{"engine_renderer_mesh_textures_masked"};
  Rig whole;
  REQUIRE_MESSAGE(whole.build(gpu.device, describe(tmp, true, look), plain(), false, look),
                  whole.error);
  REQUIRE(whole.scene.has_masked());
  Rig told;  // its generator said its tiles may hand an alpha-tested mesh
  REQUIRE_MESSAGE(told.build(gpu.device, describe(tmp, false, look), plain(), true, look, true),
                  told.error);
  CHECK(told.scene.has_masked());
  Rig untold;
  REQUIRE_MESSAGE(untold.build(gpu.device, describe(tmp, false, look), plain(), true, look),
                  untold.error);
  CHECK_FALSE(untold.scene.has_masked());
  for (Rig* rig : {&told, &untold}) {
    for (u32 t = 0; t < k_tiles; ++t)
      REQUIRE_MESSAGE(rig->admit(t, t), rig->error);
    REQUIRE_MESSAGE(rig->hand_over(k_in_order), rig->error);
  }
  CapturedFrame w;
  CapturedFrame a;
  CapturedFrame o;
  REQUIRE_MESSAGE(whole.shoot(w), whole.error);
  REQUIRE_MESSAGE(told.shoot(a), told.error);
  REQUIRE_MESSAGE(untold.shoot(o), untold.error);
  const Difference streamed = compare(w, whole, a, told);
  MESSAGE("alpha-tested, read whole against streamed: "
          << streamed.depth << " depths, " << streamed.color << " colours, " << streamed.ids
          << " ids over " << streamed.rock_pixels << " rock pixels");
  CHECK(streamed.depth == 0);
  CHECK(streamed.color == 0);
  CHECK(streamed.ids == 0);
  // The holes are holes: a scene not told to expect one draws the same rocks solid.
  const u32 cut = rock_pixels(a);
  const u32 solid = rock_pixels(o);
  MESSAGE("rock pixels with the coverage drawn: " << cut << ", drawn opaque: " << solid);
  CHECK(cut > 3000);
  CHECK(cut + 150 < solid);
}

TEST_CASE("mesh textures: a textured tile mesh on the ray path is the whole read's") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  if (!gpu.traces()) {
    MESSAGE(gpu.device.adapter().name << " has no cluster acceleration structures; skipped");
    return;
  }
  test::TempDir tmp{"engine_renderer_mesh_textures_rt"};
  RenderSettings traced;
  traced.raster = RasterMode::RayTrace;
  traced.shadows = ShadowMode::RayTraced;
  traced.shadow_casters = true;
  traced.occlusion = false;
  {
    const u32 look = test::k_tile_rock_textured;
    Rig whole;
    REQUIRE_MESSAGE(whole.build(gpu.device, describe(tmp, true, look), traced, false, look),
                    whole.error);
    Rig streamed;
    REQUIRE_MESSAGE(streamed.build(gpu.device, describe(tmp, false, look), traced, true, look),
                    streamed.error);
    REQUIRE(streamed.resolved.ray_path);
    for (u32 t = 0; t < k_tiles; ++t)
      REQUIRE_MESSAGE(streamed.admit(t, t), streamed.error);
    REQUIRE_MESSAGE(streamed.hand_over(k_in_order), streamed.error);
    CapturedFrame w;
    CapturedFrame s;
    REQUIRE_MESSAGE(whole.shoot(w), whole.error);
    REQUIRE_MESSAGE(streamed.shoot(s), streamed.error);
    const Difference d = compare(w, whole, s, streamed);
    MESSAGE("traced and textured, read whole against streamed: "
            << d.depth << " depths, " << d.color << " colours, " << d.ids << " ids over "
            << d.rock_pixels << " rock pixels");
    CHECK(d.rock_pixels > 4000);
    // The picture is a function of the scene and the eye on the ray path too: the same triangles
    // under the same tie rule, shaded from the same texels, wherever the rocks' clusters and
    // textures landed.
    CHECK(d.depth == 0);
    CHECK(d.color == 0);
    CHECK(d.ids == 0);
  }
  {
    // Alpha-tested on the ray path: the traced cut over the rocks is the rasterizer's, holes
    // included, and neither is the solid rock.
    const u32 look = test::k_tile_rock_textured | test::k_tile_rock_masked;
    RenderSettings raster = traced;
    raster.raster = RasterMode::Hardware;
    Rig r;
    REQUIRE_MESSAGE(r.build(gpu.device, describe(tmp, false, look), raster, true, look, true),
                    r.error);
    Rig t;
    REQUIRE_MESSAGE(t.build(gpu.device, describe(tmp, false, look), traced, true, look, true),
                    t.error);
    Rig solid;
    REQUIRE_MESSAGE(solid.build(gpu.device, describe(tmp, false, look), traced, true, look),
                    solid.error);
    for (Rig* rig : {&r, &t, &solid}) {
      for (u32 tile = 0; tile < k_tiles; ++tile)
        REQUIRE_MESSAGE(rig->admit(tile, tile), rig->error);
      REQUIRE_MESSAGE(rig->hand_over(k_in_order), rig->error);
    }
    CapturedFrame rs;
    CapturedFrame ts;
    CapturedFrame ss;
    REQUIRE_MESSAGE(r.shoot(rs), r.error);
    REQUIRE_MESSAGE(t.shoot(ts), t.error);
    REQUIRE_MESSAGE(solid.shoot(ss), solid.error);
    const u32 rastered = rock_pixels(rs);
    const u32 rayed = rock_pixels(ts);
    const u32 opaque = rock_pixels(ss);
    MESSAGE("alpha-tested rock pixels: " << rastered << " rasterized, " << rayed << " traced, "
                                         << opaque << " traced as solid");
    const u32 apart = rastered > rayed ? rastered - rayed : rayed - rastered;
    CHECK(static_cast<f64>(apart) < 0.03 * static_cast<f64>(rastered));
    CHECK(rayed + 150 < opaque);
  }
}

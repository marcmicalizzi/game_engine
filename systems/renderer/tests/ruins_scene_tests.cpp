// A scene file's `ruins` entries (docs/subsystems/ruins.md, "Where it runs"; renderer.md, "Scenes,
// camera paths and flythroughs"). Compiled only where the ruins capability is, and linked with it
// the way a host is, since the reader reaches the ruins through the scene-generator registry
// (scene_gen.md): the kit of boxes is written into the test's own scratch directory, and a scene
// with a terrain and a ruins scatter is read and loaded with no device — the kit's meshes follow
// the file's own and the terrain stays last, the instances are the assembler's, standing on the
// terrain, and a second read gives the same ones; the same entry in the generic `placements` list
// reads the same; and, where the terrain capability is too, the buildings stand on the dunes'
// floor a year apart. The picture is engine-view's end-to-end test (apps/engine_view/tests).
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/ruins/assembler.h>
#include <domain/ruins/blocks.h>
#include <domain/ruins/kit.h>
#include <domain/ruins/synthetic_kit.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// The scene the cases read: a 60 m terrain with a ridge, a cube the file places itself, and four
// buildings on the tiles around the origin.
std::string scene_text(const std::string& extra = std::string()) {
  return R"({"format":"engine.scene.v1","name":"ruins",)"
         R"("meshes":[{"name":"marker","path":"kit/debris-0.glb"}],)"
         R"("instances":[{"mesh":0,"translation":[0,0,0],"ground":true}],)"
         R"("terrain":{"size":65,"extent":60,"seed":3,"dune_height":1.5,)"
         R"("ridges":[{"from":[-50,40],"to":[50,40],"height":6,"width":10}]},)"
         R"("ruins":[{"name":"town","kit":"kit/kit.json","seed":11,"tile_size":24,)"
         R"("tile_min":[-1,-1],"tile_max":[0,0],"count":4,"wind_deg":45)" +
         extra + "}]}";
}

// The ground the generator stands its buildings on, as it asks it: the scene ground's floor at the
// millimetre (`scene_gen::Ground`, ADR-0053). The context is a `TerrainSampler`.
f64 floor_as_generator(const void* context, i64 x_mm, i64 z_mm) noexcept {
  return static_cast<const TerrainSampler*>(context)->provider().view().floor_mm(x_mm, z_mm);
}

}  // namespace

TEST_CASE("renderer: a scene's ruins are the assembler's buildings, on the terrain") {
  const test::TempDir tmp("renderer_ruins_scene");
  std::string error;
  std::string kit_path;
  REQUIRE_MESSAGE(
      ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error, &kit_path),
      error);
  const std::string scene_path = tmp.file("scene.json");
  REQUIRE(write_text(scene_path, scene_text()));

  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(scene_path, desc, error), error);
  ruins::Kit kit;
  REQUIRE_MESSAGE(ruins::read_kit_file(kit_path, kit, error), error);
  // The file's mesh, then the kit's, then the terrain.
  REQUIRE(desc.meshes.size() == 1 + kit.meshes.size() + 1);
  REQUIRE(desc.mesh_info.size() == desc.meshes.size());
  CHECK(desc.meshes.back().empty());
  CHECK(std::string(desc.mesh_info.back().origin) == "terrain");
  for (u32 m = 0; m < kit.meshes.size(); ++m) {
    CHECK(desc.meshes[1 + m] == kit.meshes[m]);
    CHECK(desc.mesh_info[1 + m].name.find(kit.name + "/") == 0);
  }
  CHECK(desc.placed_buildings == 4);

  // The same buildings the assembler gives for these fields, one instance each piece, between the
  // file's own instance and the terrain's.
  Vector<ruins::TileCoord> tiles;
  ruins::choose_tiles(11, ruins::TileCoord{-1, -1}, ruins::TileCoord{0, 0}, 4, 0.0f, tiles);
  REQUIRE(tiles.size() == 4);
  ruins::Placement placement;
  placement.world_seed = 11;
  placement.tile_cm = 2400;
  placement.wind_step = 2;
  const TerrainSampler sampler(desc.terrain);
  placement.ground = ruins::Ground{&floor_as_generator, &sampler};
  ruins::Output built;
  REQUIRE(ruins::assemble_tiles(kit, placement,
                                std::span<const ruins::TileCoord>(tiles.data(), tiles.size()),
                                nullptr, built, &error));
  REQUIRE(desc.placed_instances == built.instances.size());
  REQUIRE(desc.instances.size() == 1 + built.instances.size() + 1);
  for (u32 i = 0; i < built.instances.size(); ++i) {
    const ruins::Instance& piece = built.instances[i];
    const SceneInstance& instance = desc.instances[1 + i];
    CHECK(instance.mesh == 1 + kit.members[piece.member].mesh_index);
    const WorldPos t = ruins::instance_translation(kit, piece);
    CHECK(instance.origin == t);
    CHECK(instance.transform.position == Vec3{});
    // Every piece is on the terrain: a debris block at the height under it, a wall at or below
    // the lowest ground under its building.
    const f64 ground = static_cast<f64>(terrain_height(
        desc.terrain, static_cast<f32>(piece.position.x), static_cast<f32>(piece.position.z)));
    if (piece.kind == static_cast<u8>(ruins::PieceKind::debris)) {
      CHECK(std::fabs(piece.position.y - ground) <= 0.07);
    } else {
      CHECK(piece.position.y <= ground + 0.01);
    }
  }
  CHECK(desc.instances.back().mesh == desc.meshes.size() - 1);  // the terrain's, last

  // Read again: the same instances, to the bit.
  SceneDesc again;
  REQUIRE(read_scene_file(scene_path, again, error));
  REQUIRE(again.instances.size() == desc.instances.size());
  for (u32 i = 0; i < desc.instances.size(); ++i) {
    CHECK(again.instances[i].mesh == desc.instances[i].mesh);
    CHECK(again.instances[i].transform == desc.instances[i].transform);
    CHECK(again.instances[i].origin == desc.instances[i].origin);
  }

  // And it loads: the kit's GLBs import and cluster like any mesh, into a cache in scratch.
  desc.ddc = tmp.file("ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  CHECK(data.instances.size() == desc.instances.size());
  CHECK(data.parts.size() == desc.meshes.size());
}

TEST_CASE("renderer: a scene's ruins drawn in blocks are the block layer's, on any thread count") {
  const test::TempDir tmp("renderer_ruins_blocks");
  std::string error;
  std::string kit_path;
  std::string blocks_path;
  REQUIRE_MESSAGE(
      ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error, &kit_path),
      error);
  REQUIRE_MESSAGE(ruins::write_synthetic_block_kit(
                      tmp.file("blocks"), ruins::SyntheticBlockOptions{}, &error, &blocks_path),
                  error);
  // Forty buildings: past the count the reader shares out on the job system's pool, so this is
  // also the check that a pooled read gives the single thread's instances.
  const std::string scene_path = tmp.file("scene.json");
  REQUIRE(write_text(scene_path,
                     R"({"format":"engine.scene.v1","name":"ruin blocks",)"
                     R"("terrain":{"size":65,"extent":200,"seed":3,"dune_height":1.5},)"
                     R"("ruins":[{"name":"town","kit":"kit/kit.json","seed":11,"tile_size":24,)"
                     R"("tile_min":[-6,-6],"tile_max":[5,5],"count":40,"wind_deg":45,)"
                     R"("representation":"Blocks","block_kit":"blocks/block-kit.json"}]})"));
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(scene_path, desc, error), error);
  ruins::Kit kit;
  REQUIRE_MESSAGE(ruins::read_kit_file(kit_path, kit, error), error);
  ruins::BlockKit blocks;
  REQUIRE_MESSAGE(ruins::read_block_kit_file(blocks_path, blocks, error), error);
  // The block kit's meshes and then the terrain: the section kit's are not drawn, so not loaded.
  REQUIRE(desc.meshes.size() == blocks.meshes.size() + 1);
  for (u32 m = 0; m < blocks.meshes.size(); ++m)
    CHECK(desc.meshes[m] == blocks.meshes[m]);
  CHECK(desc.placed_buildings == 40);

  Vector<ruins::TileCoord> tiles;
  ruins::choose_tiles(11, ruins::TileCoord{-6, -6}, ruins::TileCoord{5, 5}, 40, 0.0f, tiles);
  ruins::Placement placement;
  placement.world_seed = 11;
  placement.tile_cm = 2400;
  placement.wind_step = 2;
  const TerrainSampler sampler(desc.terrain);
  placement.ground = ruins::Ground{&floor_as_generator, &sampler};
  ruins::BlockOutput built;
  REQUIRE(ruins::assemble_block_tiles(kit, blocks, placement,
                                      std::span<const ruins::TileCoord>(tiles.data(), tiles.size()),
                                      nullptr, built, &error));
  REQUIRE(desc.placed_instances == built.blocks.size());
  REQUIRE(desc.instances.size() == built.blocks.size() + 1);
  bool all_same = true;
  for (u32 i = 0; i < built.blocks.size() && all_same; ++i) {
    const ruins::Block& block = built.blocks[i];
    const SceneInstance& instance = desc.instances[i];
    const WorldPos t = ruins::block_translation(blocks, block);
    all_same = instance.mesh == blocks.blocks[block.block].mesh_index && instance.origin == t;
    CHECK_MESSAGE(all_same, "block " << i);
  }
  CHECK(desc.instances.back().mesh == desc.meshes.size() - 1);  // the terrain's, last

  // And it loads: the block GLBs cluster like any mesh, one cluster each at the low fidelity, so
  // a block is one pair.
  desc.ddc = tmp.file("ddc");
  SceneData data;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  CHECK(data.instances.size() == desc.instances.size());
  for (u32 m = 0; m < blocks.meshes.size(); ++m)
    CHECK(data.parts[m].cluster_count == 1);
}

TEST_CASE("renderer: a scene's ruins drawn in blocks refuse a scatter with no block kit") {
  const test::TempDir tmp("renderer_ruins_blocks_refuse");
  std::string error;
  REQUIRE_MESSAGE(ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error),
                  error);
  const std::string scene_path = tmp.file("scene.json");
  REQUIRE(write_text(scene_path,
                     R"({"format":"engine.scene.v1","terrain":{"size":17,"extent":20},)"
                     R"("ruins":[{"kit":"kit/kit.json","count":1,"representation":"Blocks"}]})"));
  SceneDesc desc;
  CHECK_FALSE(read_scene_file(scene_path, desc, error));
  CHECK(error.find("ruins 0") != std::string::npos);
  CHECK(error.find("block_kit") != std::string::npos);
}

TEST_CASE("renderer: a scene's ruins refuse a kit that is not there, naming the entry") {
  const test::TempDir tmp("renderer_ruins_refuse");
  const std::string scene_path = tmp.file("scene.json");
  REQUIRE(write_text(scene_path, R"({"format":"engine.scene.v1","terrain":{"size":17,"extent":20},)"
                                 R"("ruins":[{"kit":"missing/kit.json","count":1}]})"));
  SceneDesc desc;
  std::string error;
  CHECK_FALSE(read_scene_file(scene_path, desc, error));
  CHECK(error.find("ruins 0") != std::string::npos);
  CHECK(error.find("kit.json") != std::string::npos);
}

TEST_CASE("renderer: a placements entry naming the ruins reads as the ruins field does") {
  // ADR-0046: the `ruins` field is the placement generator "ruins" with the entry as its
  // parameters, so the generic list gives the same meshes and the same instances.
  const test::TempDir tmp("renderer_ruins_placements");
  std::string error;
  REQUIRE_MESSAGE(ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error),
                  error);
  const std::string entry = R"({"name":"town","kit":"kit/kit.json","seed":11,"tile_size":24,)"
                            R"("tile_min":[-1,-1],"tile_max":[0,0],"count":4,"wind_deg":45})";
  const std::string terrain = R"("terrain":{"size":33,"extent":60,"seed":3,"dune_height":1.5})";
  const std::string by_field = tmp.file("field.json");
  const std::string by_list = tmp.file("list.json");
  REQUIRE(write_text(by_field, "{" + terrain + R"(,"ruins":[)" + entry + "]}"));
  REQUIRE(write_text(
      by_list, "{" + terrain + R"(,"placements":[{"generator":"ruins","params":)" + entry + "}]}"));
  SceneDesc a, b;
  REQUIRE_MESSAGE(read_scene_file(by_field, a, error), error);
  REQUIRE_MESSAGE(read_scene_file(by_list, b, error), error);
  CHECK(a.placed_buildings == 4);
  CHECK(b.placed_buildings == 4);
  REQUIRE(a.meshes == b.meshes);
  REQUIRE(a.instances.size() == b.instances.size());
  for (u32 i = 0; i < a.instances.size(); ++i) {
    CHECK(a.instances[i].mesh == b.instances[i].mesh);
    CHECK(a.instances[i].transform == b.instances[i].transform);
    CHECK(a.instances[i].origin == b.instances[i].origin);
  }
  // The list's entries are named by their place in it.
  const std::string bad = tmp.file("bad.json");
  REQUIRE(write_text(
      bad, "{" + terrain + R"(,"placements":[{"generator":"ruins","params":{"count":1}}]})"));
  SceneDesc refused;
  CHECK_FALSE(read_scene_file(bad, refused, error));
  CHECK(error.find("placements 0 names no kit") != std::string::npos);
}

TEST_CASE("renderer: ruins over the dune generator stand on its ground") {
  // The dunes are the terrain capability's, which this test links where the configuration has it;
  // without it they are a name the reader does not know, and there is nothing to hold.
  TerrainDesc dunes;
  dunes.provider = "dunes";
  if (!terrain_provider_known(dunes)) {
    MESSAGE("skipped: this build has no terrain capability");
    return;
  }
  const test::TempDir tmp("renderer_terrain_generator_ruins");
  std::string error;
  std::string kit_path;
  REQUIRE_MESSAGE(
      ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error, &kit_path),
      error);
  const auto scene = [](const char* time) {
    return std::string(R"({"format":"engine.scene.v1","name":"ruins-on-dunes",)"
                       R"("terrain":{"size":65,"extent":60,"seed":3,"dune_height":2,)"
                       R"("generator":"Dunes","time":)") +
           time +
           R"(},"ruins":[{"name":"town","kit":"kit/kit.json","seed":11,"tile_size":24,)"
           R"("tile_min":[-1,-1],"tile_max":[0,0],"count":4,"wind_deg":45}]})";
  };
  const std::string early = tmp.file("early.json");
  const std::string late = tmp.file("late.json");
  REQUIRE(write_text(early, scene("0")));
  REQUIRE(write_text(late, scene("31536000")));  // a year on
  SceneDesc a, b;
  REQUIRE_MESSAGE(read_scene_file(early, a, error), error);
  REQUIRE_MESSAGE(read_scene_file(late, b, error), error);
  CHECK(a.placed_buildings == 4);
  // The same buildings a year apart, to the bit: they stand on the floor, which does not move.
  REQUIRE(a.instances.size() == b.instances.size());
  for (u32 i = 0; i + 1 < a.instances.size(); ++i) {
    CHECK(a.instances[i].transform == b.instances[i].transform);
    CHECK(a.instances[i].origin == b.instances[i].origin);
  }
  // And on the ground: nothing of a building floats above the floor at its origin.
  const TerrainSampler ground(a.terrain);
  u32 floating = 0;
  for (u32 i = 0; i + 1 < a.instances.size(); ++i) {
    const Vec3 p = relative(a.instances[i].origin, WorldPos::origin());
    floating += p.y > ground.ground(p.x, p.z) + 1.0f;
  }
  CHECK(floating == 0);
}

namespace {

// Flat ground 0.75 m up, in whole millimetres: the same under a building anywhere.
f64 flat_ground(const void*, i64, i64) noexcept { return 0.75; }

// Every piece of `tiles`' buildings, sections with their debris and then the blocks, made the GPU's
// instance the way the reader makes one of a placement (`SceneInstance::origin` the placement's
// place, the yaw its turn, `make_instance`), with the grid's corner at `origin_cm`.
Vector<gfx::InstanceDesc> ruin_instances(const ruins::Kit& kit, const ruins::BlockKit& blocks,
                                         std::span<const ruins::TileCoord> tiles, i64 origin_cm) {
  ruins::Placement placement;
  placement.world_seed = 11;
  placement.tile_cm = 3200;
  placement.wind_step = 2;
  placement.ground = ruins::Ground{&flat_ground, nullptr};
  placement.origin_x_cm = origin_cm;
  placement.origin_z_cm = origin_cm;
  std::string error;
  ruins::Output built;
  REQUIRE_MESSAGE(ruins::assemble_tiles(kit, placement, tiles, nullptr, built, &error), error);
  ruins::BlockOutput laid;
  REQUIRE_MESSAGE(ruins::assemble_block_tiles(kit, blocks, placement, tiles, nullptr, laid, &error),
                  error);
  SceneData scene;
  scene.parts.resize(1);
  scene.mesh_fit.resize(1, Mat4::identity());
  const auto make = [&](WorldPos at, u32 yaw_step) {
    SceneInstance source;
    source.origin = at;
    source.transform.rotation =
        quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, radians(22.5f * static_cast<f32>(yaw_step)));
    gfx::InstanceDesc instance;
    std::string why;
    REQUIRE_MESSAGE(make_instance(scene, source, 0, instance, &why), why);
    return instance;
  };
  Vector<gfx::InstanceDesc> out;
  for (const ruins::Instance& piece : built.instances)
    out.push_back(
        make(ruins::instance_translation(kit, piece), ruins::instance_yaw_step(kit, piece)));
  for (const ruins::Block& block : laid.blocks)
    out.push_back(
        make(ruins::block_translation(blocks, block), ruins::block_yaw_step(blocks, block)));
  return out;
}

}  // namespace

// **A ruin moved by whole cells is the same instances, its cells moved** (ADR-0053 decision 6;
// ruins.md, "Far from the origin"). A building's shape is a function of its tile — its seed is the
// tile's — so the generator is tested with an origin-relative input: the same tiles' buildings with
// the grid's corner 10,000 km out (`Placement::origin_x_cm`, 156,250 cells on x and z), on the same
// flat ground. Every piece and every block the reader would make an instance of has the same
// 3x4 to the bit and a cell moved by exactly 156,250 — the GPU's instance records are the
// origin's but for the whole cells, which the renderer's translation suite
// (`world_translation_tests.cpp`) holds draws the same bytes under a camera moved by them. The
// places are integer centimetres in f64, so the far one rounds at f64's step there (2 nm) where the
// origin's does not; a float32 local would differ only where a centimetre lies within that of a
// float's midpoint, and none of these does.
TEST_CASE("renderer: a ruin moved by whole cells is the same instances, its cells moved") {
  const test::TempDir tmp("renderer_ruins_translation");
  std::string error;
  std::string kit_path;
  std::string blocks_path;
  REQUIRE_MESSAGE(
      ruins::write_synthetic_kit(tmp.file("kit"), ruins::SyntheticKitOptions{}, &error, &kit_path),
      error);
  REQUIRE_MESSAGE(ruins::write_synthetic_block_kit(
                      tmp.file("blocks"), ruins::SyntheticBlockOptions{}, &error, &blocks_path),
                  error);
  ruins::Kit kit;
  REQUIRE_MESSAGE(ruins::read_kit_file(kit_path, kit, error), error);
  ruins::BlockKit blocks;
  REQUIRE_MESSAGE(ruins::read_block_kit_file(blocks_path, blocks, error), error);
  const ruins::TileCoord tiles[] = {{0, 0}, {-1, 0}, {0, -1}, {-1, -1}, {1, 1}, {-2, 1}};
  const i64 far_cm = 1'000'000'000;  // 10,000 km: 156,250 cells of 64 m
  const i32 cells = static_cast<i32>(far_cm / 6400);
  const Vector<gfx::InstanceDesc> home = ruin_instances(kit, blocks, tiles, 0);
  const Vector<gfx::InstanceDesc> far = ruin_instances(kit, blocks, tiles, far_cm);
  REQUIRE(home.size() == far.size());
  REQUIRE(home.size() > 100);
  u32 cell_off = 0;
  u32 rows_off = 0;
  for (u32 i = 0; i < home.size(); ++i) {
    cell_off += far[i].cell.x == home[i].cell.x + cells && far[i].cell.y == home[i].cell.y &&
                        far[i].cell.z == home[i].cell.z + cells
                    ? 0u
                    : 1u;
    rows_off += std::memcmp(far[i].rows, home[i].rows, sizeof(home[i].rows)) == 0 ? 0u : 1u;
  }
  MESSAGE(home.size() << " instances: " << cell_off << " cells and " << rows_off
                      << " 3x4s not the origin's moved by whole cells");
  CHECK(cell_off == 0);
  CHECK(rows_off == 0);
}

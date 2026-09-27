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
#include <domain/ruins/assembler.h>
#include <domain/ruins/blocks.h>
#include <domain/ruins/kit.h>
#include <domain/ruins/synthetic_kit.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
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
  placement.ground =
      ruins::Ground{[](const void* context, f32 x, f32 z) noexcept {
                      return terrain_height(*static_cast<const TerrainDesc*>(context), x, z);
                    },
                    &desc.terrain};
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
    const Vec3 t = ruins::instance_translation(kit, piece);
    CHECK(instance.transform.position.x == t.x);
    CHECK(instance.transform.position.y == t.y);
    CHECK(instance.transform.position.z == t.z);
    // Every piece is on the terrain: a debris block at the height under it, a wall at or below
    // the lowest ground under its building.
    const f32 ground = terrain_height(desc.terrain, piece.position.x, piece.position.z);
    if (piece.kind == static_cast<u8>(ruins::PieceKind::debris)) {
      CHECK(std::fabs(piece.position.y - ground) <= 0.07f);
    } else {
      CHECK(piece.position.y <= ground + 0.01f);
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
  placement.ground =
      ruins::Ground{[](const void* context, f32 x, f32 z) noexcept {
                      return terrain_height(*static_cast<const TerrainDesc*>(context), x, z);
                    },
                    &desc.terrain};
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
    const Vec3 t = ruins::block_translation(blocks, block);
    all_same = instance.mesh == blocks.blocks[block.block].mesh_index &&
               instance.transform.position.x == t.x && instance.transform.position.y == t.y &&
               instance.transform.position.z == t.z;
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
  for (u32 i = 0; i + 1 < a.instances.size(); ++i)
    CHECK(a.instances[i].transform == b.instances[i].transform);
  // And on the ground: nothing of a building floats above the floor at its origin.
  const TerrainSampler ground(a.terrain);
  u32 floating = 0;
  for (u32 i = 0; i + 1 < a.instances.size(); ++i) {
    const Vec3 p = a.instances[i].transform.position;
    floating += p.y > ground.ground(p.x, p.z) + 1.0f;
  }
  CHECK(floating == 0);
}

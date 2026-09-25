// A scene file's `ruins` entries (docs/subsystems/ruins.md, "Where it runs"; renderer.md, "Scenes,
// camera paths and flythroughs"). Compiled only where the ruins capability is: the kit of boxes is
// written into the test's own scratch directory, and a scene with a terrain and a ruins scatter is
// read and loaded with no device — the kit's meshes follow the file's own and the terrain stays
// last, the instances are the assembler's, standing on the terrain, and a second read gives the
// same ones. The picture is engine-view's end-to-end test (apps/engine_view/tests).
#include <core/math/math.h>
#include <domain/ruins/assembler.h>
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
  CHECK(desc.ruin_buildings == 4);

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
  REQUIRE(desc.ruin_instances == built.instances.size());
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

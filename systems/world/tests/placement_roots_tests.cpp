// **The streamed consumer opens an entry under the whole read's root** (T119; scene_gen.md, "The
// derived-data root"; the forest's engine-needs 46 and 61): a streamed deposits entry, read with a
// host root named, is opened under that root by the reader and again by the world's placements
// consumer, and nothing of it lands under the scene's own root. Before T119 the consumer opened it
// with no root, so the deposits walked from the scene's directory and wrote every recipe a second
// time under `<scene>/ddc/deposits` (the forest's 707 cliff, 143 ledge and 479 deposit recipes).
// Since G191 the deposits write no recipe (a recipe travels with its tile; rocks.md, "A tile
// solves"): what an open leaves is its directory, made under the root it was handed, so the case
// holds where each open was rooted by where that directory is, and that neither wrote a file.
// Linked with the rocks capability, as a host is.
#include <systems/renderer/scene.h>
#include <systems/world/placement_tiles.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::world;

namespace {

// The files anywhere under `dir`, 0 when it is not there.
u32 files_in(const std::string& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return 0;
  u32 n = 0;
  for (const auto& e : std::filesystem::recursive_directory_iterator(dir, ec))
    n += e.is_regular_file() ? 1u : 0u;
  return n;
}

// What a load hands the consumer, without a mesh built: one cluster a mesh.
void fake_load(const renderer::SceneDesc& desc, renderer::SceneData& out) {
  out.parts.resize(desc.meshes.size());
  for (u32 m = 0; m < desc.meshes.size(); ++m) {
    out.parts[m].first_cluster = m;
    out.parts[m].cluster_count = 1;
  }
  out.streamed = desc.streamed;
  out.terrain = desc.terrain;
  out.world = desc.world;
  out.dynamic = desc.world.enabled;
  out.pair_count = 100;
}

}  // namespace

TEST_CASE(
    "placement tiles: a streamed entry's recipes go under the host's root, never the scene's") {
  test::TempDir tmp{"engine_world_roots"};
  const std::string scene_dir = tmp.file("world"), host = tmp.file("host/ddc");
  std::filesystem::create_directories(scene_dir);
  const std::string path = scene_dir + "/scene.json";
  {
    std::ofstream f(path, std::ios::binary);
    f << R"({"format":"engine.scene.v1","name":"roots",)"
         R"("terrain":{"size":65,"extent":64,"seed":3},"world":{"tile_size":16},)"
         R"("placements":[{"generator":"deposits","params":{"region_max":[32,32],)"
         R"("patch":{"fragments":8,"triangle_budget":10000}}}]})";
    REQUIRE(f.good());
  }
  renderer::SceneDesc desc;
  desc.ddc = host;
  std::string error;
  REQUIRE_MESSAGE(renderer::read_scene_file(path, desc, error), error);
  REQUIRE(desc.streamed.size() == 1);
  CHECK(desc.streamed[0].ddc == host);
  // The reader's open was rooted at the host's root, and wrote nothing (G191).
  CHECK(std::filesystem::is_directory(host + "/deposits"));
  CHECK(files_in(host) == 0);
  CHECK_FALSE(std::filesystem::exists(scene_dir + "/ddc"));
  CHECK(files_in(scene_dir) == 1);  // the scene file alone

  // The consumer opens the entry again: rooted at the same root (its directory is made again
  // there, and nowhere beside the scene), and it too writes nothing.
  std::filesystem::remove_all(host);
  renderer::SceneData scene;
  fake_load(desc, scene);
  PlacementTilesConfig config;
  config.tile_size = 16.0f;
  PlacementTiles tiles;
  REQUIRE_MESSAGE(tiles.create(scene, config, &error), error);
  CHECK(std::filesystem::is_directory(host + "/deposits"));
  CHECK(files_in(host) == 0);
  CHECK_FALSE(std::filesystem::exists(scene_dir + "/ddc"));
  CHECK(files_in(scene_dir) == 1);
}

// The scene reader through the scene-generator registry (docs/subsystems/scene_gen.md; renderer.md,
// "Scenes, camera paths and flythroughs"; ADR-0046), with generators of this test's own registered
// from this file and no capability needed: compiled in every configuration, the minimal one
// included, where it is the proof that the reader expands placements and draws a terrain through
// the registry alone. A test ground and two test placement generators log what they are asked, in
// the order they are asked; a scene naming them reads as instances of their meshes standing on the
// test ground; the ground is made before any placement entry is opened, and the entries are
// expanded in the scene's order — swapping them swaps the log and not the ground's place in it; a
// streamed read opens each entry and expands nothing; and names the executable does not carry are
// refused with the registry's sentence.
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

// What the test generators were asked, in order.
std::string g_log;

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// A plane at the entry's dune height, with a floor half a metre under it.
struct Plane {
  f32 level = 0.0f;
};
f32 plane_height(const void* state, f32, f32) noexcept {
  return static_cast<const Plane*>(state)->level;
}
f32 plane_floor(const void* state, f32, f32) noexcept {
  return static_cast<const Plane*>(state)->level - 0.5f;
}
void plane_destroy(void* state) noexcept { delete static_cast<Plane*>(state); }
constexpr scene_gen::GroundOps k_plane_ops{
    .destroy = &plane_destroy, .height = &plane_height, .floor = &plane_floor};
bool plane_make(const scene::Terrain& entry, const scene_gen::Context&,
                scene_gen::GroundProvider& out, std::string*) {
  g_log += "ground;";
  out = scene_gen::GroundProvider(&k_plane_ops, new Plane{entry.dune_height});
  return true;
}
constexpr scene_gen::GroundProviderDesc k_plane{.name = "scene-gen-test-plane",
                                                .make = &plane_make};
const scene_gen::Registrar k_plane_registrar{k_plane};

// A placement generator of one mesh placed `count` times along x at `step`, on the ground's floor;
// its name is its tag, so two of them tell themselves apart in the log.
struct Row {
  std::string name;
  u32 count = 0;
  f32 step = 1.0f;
};
bool row_open(const JsonValue& params, const scene_gen::Context& context, void** state,
              std::string* error) {
  auto* row = new Row;
  const JsonValue* name = params.find("name");
  const JsonValue* count = params.find("count");
  std::string_view text;
  u64 n = 0;
  if (name == nullptr || !name->get_string(text) || count == nullptr || !count->get_u64(n)) {
    delete row;
    if (error != nullptr) *error = std::string(context.where) + ": a row needs a name and a count";
    return false;
  }
  row->name = std::string(text);
  row->count = static_cast<u32>(n);
  g_log += "open " + row->name + ";";
  *state = row;
  return true;
}
void row_close(void* state) noexcept { delete static_cast<Row*>(state); }
bool row_expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
                std::string*) {
  const Row& row = *static_cast<const Row*>(state);
  g_log += "expand " + row.name + ";";
  out.meshes.push_back(
      scene_gen::PlacementMesh{"rows/" + row.name + ".glb", "rows/" + row.name, 0});
  for (u32 i = 0; i < row.count; ++i) {
    scene_gen::Placement p;
    p.mesh = 0;
    const f32 x = row.step * static_cast<f32>(i);
    p.transform.position = Vec3{x, context.ground.floor(x, 0.0f), 0.0f};
    p.tag = static_cast<u8>(i % 2);
    out.instances.push_back(p);
  }
  out.things = row.count;
  return true;
}
bool row_meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
                std::string*) {
  const Row& row = *static_cast<const Row*>(state);
  out.push_back(scene_gen::PlacementMesh{"rows/" + row.name + ".glb", "rows/" + row.name, 0});
  return true;
}
bool row_tile(void*, scene_gen::TileCoord, const scene_gen::Context&, scene_gen::Placements&,
              std::string*) {
  return true;
}
constexpr scene_gen::PlacementGeneratorDesc k_row{.name = "scene-gen-test-row",
                                                  .open = &row_open,
                                                  .close = &row_close,
                                                  .expand = &row_expand,
                                                  .meshes = &row_meshes,
                                                  .tile = &row_tile};
const scene_gen::Registrar k_row_registrar{k_row};

std::string scene_with(const std::string& placements, const std::string& world = std::string()) {
  return R"({"format":"engine.scene.v1","name":"rows",)"
         R"("meshes":[{"name":"marker","path":"marker.glb"}],)"
         R"("instances":[{"mesh":0,"translation":[0,1,0],"ground":true}],)"
         R"("terrain":{"size":17,"extent":20,"dune_height":3,"provider":"scene-gen-test-plane"},)"
         R"("placements":[)" +
         placements + "]" + world + "}";
}

const char* k_a = R"({"generator":"scene-gen-test-row","params":{"name":"a","count":3}})";
const char* k_b = R"({"generator":"scene-gen-test-row","params":{"name":"b","count":2}})";

}  // namespace

TEST_CASE("renderer: a scene's placements expand through the registry, standing on its ground") {
  const test::TempDir tmp("renderer_scene_gen");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_text(path, scene_with(std::string(k_a) + "," + k_b)));
  g_log.clear();
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  // The file's mesh, each generator's, then the terrain's.
  REQUIRE(desc.meshes.size() == 4);
  CHECK(desc.mesh_info[1].name == "rows/a");
  CHECK(desc.mesh_info[2].name == "rows/b");
  CHECK(desc.meshes[1] == "rows/a.glb");  // a generator's path is taken as it gives it
  CHECK(std::string(desc.mesh_info[3].origin) == "terrain");
  CHECK(desc.placed_buildings == 5);
  CHECK(desc.placed_instances == 5);
  // The file's instance on the ground's surface, the rows on its floor, the terrain last.
  REQUIRE(desc.instances.size() == 1 + 3 + 2 + 1);
  CHECK(desc.instances[0].transform.position.y == 4.0f);
  for (u32 i = 0; i < 3; ++i) {
    CHECK(desc.instances[1 + i].mesh == 1);
    CHECK(desc.instances[1 + i].transform.position == Vec3{static_cast<f32>(i), 2.5f, 0.0f});
  }
  CHECK(desc.instances[4].mesh == 2);
  CHECK(desc.instances.back().mesh == 3);
  // The terrain's provider is the test's, and the terrain's hash names it.
  CHECK(terrain_provider(desc.terrain) == "scene-gen-test-plane");
  CHECK(terrain_height(desc.terrain, 5.0f, -7.0f) == 3.0f);
  TerrainDesc waves = desc.terrain;
  waves.provider.clear();
  CHECK(terrain_hash(waves) != terrain_hash(desc.terrain));
}

TEST_CASE("renderer: the ground is made first, and the placements follow in the scene's order") {
  const test::TempDir tmp("renderer_scene_gen_order");
  std::string error;
  const std::string ab = tmp.file("ab.json");
  const std::string ba = tmp.file("ba.json");
  REQUIRE(write_text(ab, scene_with(std::string(k_a) + "," + k_b)));
  REQUIRE(write_text(ba, scene_with(std::string(k_b) + "," + k_a)));
  g_log.clear();
  SceneDesc first;
  REQUIRE_MESSAGE(read_scene_file(ab, first, error), error);
  CHECK(g_log == "ground;open a;expand a;open b;expand b;");
  // Swapped in the file: the entries swap, the ground is still made before either.
  g_log.clear();
  SceneDesc second;
  REQUIRE_MESSAGE(read_scene_file(ba, second, error), error);
  CHECK(g_log == "ground;open b;expand b;open a;expand a;");
  // And the meshes follow the entries: b's is appended first now.
  CHECK(second.mesh_info[1].name == "rows/b");
  CHECK(second.mesh_info[2].name == "rows/a");
}

TEST_CASE("renderer: a streamed scene opens each entry and expands none") {
  const test::TempDir tmp("renderer_scene_gen_streamed");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_text(path, scene_with(std::string(k_a) + "," + k_b, R"(,"world":{})")));
  g_log.clear();
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  CHECK(g_log == "ground;open a;open b;");
  REQUIRE(desc.streamed.size() == 2);
  CHECK(desc.streamed[0].generator == "scene-gen-test-row");
  CHECK(desc.streamed[0].meshes.size() == 1);
  CHECK(desc.streamed[0].meshes[0] == 1);
  CHECK(desc.streamed[1].meshes[0] == 2);
  CHECK(desc.placed_instances == 0);
  CHECK(desc.instances.size() == 2);  // the file's and the terrain's
}

TEST_CASE(
    "renderer: a name this executable does not carry is refused with the registry's sentence") {
  const test::TempDir tmp("renderer_scene_gen_unknown");
  std::string error;
  const std::string placement = tmp.file("placement.json");
  REQUIRE(
      write_text(placement, scene_with(R"({"generator":"scene-gen-test-nothing","params":{}})")));
  SceneDesc desc;
  CHECK_FALSE(read_scene_file(placement, desc, error));
  CHECK(error.find("placements 0 names the placement generator \"scene-gen-test-nothing\", which "
                   "this build does not have") != std::string::npos);
  const std::string ground = tmp.file("ground.json");
  REQUIRE(write_text(ground, R"({"terrain":{"size":17,"extent":20,"provider":"snowfield"}})"));
  CHECK_FALSE(read_scene_file(ground, desc, error));
  CHECK(error.find("the terrain names the ground provider \"snowfield\", which this build does "
                   "not have") != std::string::npos);
  // A generator's own refusal comes back with where the entry is.
  const std::string bad = tmp.file("bad.json");
  REQUIRE(write_text(bad, scene_with(R"({"generator":"scene-gen-test-row","params":{}})")));
  CHECK_FALSE(read_scene_file(bad, desc, error));
  CHECK(error.find("placements 0: a row needs a name and a count") != std::string::npos);
}

// The scene reader through the scene-generator registry (docs/subsystems/scene_gen.md; renderer.md,
// "Scenes, camera paths and flythroughs"; ADR-0046), with generators of this test's own registered
// from this file and no capability needed: compiled in every configuration, the minimal one
// included, where it is the proof that the reader expands placements and draws a terrain through
// the registry alone. A test ground and two test placement generators log what they are asked, in
// the order they are asked; a scene naming them reads as instances of their meshes standing on the
// test ground; the ground is made before any placement entry is opened, and the entries are
// expanded in the scene's order — swapping them swaps the log and not the ground's place in it; a
// streamed read opens each entry and expands nothing; and names the executable does not carry are
// refused with the registry's sentence. Far from the origin: a placement lands where its generator
// put it, two pieces laid to touch leave no gap, and a scene file's scatter is its twin by the
// origin, moved by its centre.
#include <core/math/world.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstdio>
#include <cstring>
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
    p.position = WorldPos{static_cast<f64>(row.step) * static_cast<f64>(i), 0.0, 0.0};
    p.position.y = context.ground.floor(p.position);
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
  // The file's translation is the instance's f64 origin (Instance version 3).
  CHECK(desc.instances[0].origin.y == 4.0);
  CHECK(desc.instances[0].transform.position == Vec3{});
  for (u32 i = 0; i < 3; ++i) {
    // A placement's place is the instance's origin, in f64; its transform is its turn alone.
    CHECK(desc.instances[1 + i].mesh == 1);
    CHECK(desc.instances[1 + i].origin == WorldPos{static_cast<f64>(i), 2.5, 0.0});
    CHECK(desc.instances[1 + i].transform.position == Vec3{});
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

// ---- far from the origin (ADR-0053; scene_gen.md, "Far from the origin") ------------------------

namespace {

// A ground that speaks millimetres: a sawtooth a metre long along x for its surface and its floor,
// so a placement stands on the height of the millimetre it is asked at, and a neighbour's
// millimetre is another height.
f32 saw_of(i64 x_mm) noexcept { return static_cast<f32>(((x_mm % 1000) + 1000) % 1000) * 0.001f; }
f32 saw_height(const void*, f32 x, f32) noexcept {
  return saw_of(scene_gen::nearest_mm(static_cast<f64>(x)));
}
f32 saw_mm(const void*, i64 x_mm, i64) noexcept { return saw_of(x_mm); }
// The same at any time: what the scene reader grounds a file's instance and a scatter's on.
f32 saw_at(const void*, f64, i64 x_mm, i64) noexcept { return saw_of(x_mm); }
constexpr scene_gen::GroundOps k_saw_ops{
    .height = &saw_height, .height_mm = &saw_at, .surface_mm = &saw_mm, .floor_mm = &saw_mm};
bool saw_make(const scene::Terrain&, const scene_gen::Context&, scene_gen::GroundProvider& out,
              std::string*) {
  out = scene_gen::GroundProvider(&k_saw_ops, nullptr);
  return true;
}
constexpr scene_gen::GroundProviderDesc k_saw{.name = "scene-gen-test-saw", .make = &saw_make};
const scene_gen::Registrar k_saw_registrar{k_saw};

// A placement generator that puts a box where the test asks, `lift` above the ground's floor
// there, `width` wide along its own x and turned `yaw` degrees about +y.
struct Piece {
  WorldPos at;
  f32 lift = 0.0f;
  f32 width = 1.0f;
  f32 yaw = 0.0f;
};
Vector<Piece> g_pieces;
bool pieces_open(const JsonValue&, const scene_gen::Context&, void** state, std::string*) {
  *state = nullptr;
  return true;
}
void pieces_close(void*) noexcept {}
scene_gen::Placement piece_placement(const Piece& piece, const scene_gen::Ground& ground) {
  scene_gen::Placement p;
  p.position = piece.at;
  p.position.y = ground.floor(piece.at) + static_cast<f64>(piece.lift);
  p.rotation = quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, radians(piece.yaw));
  p.scale = Vec3{piece.width, 1.0f, 1.0f};
  return p;
}
bool pieces_expand(void*, const scene_gen::Context& context, scene_gen::Placements& out,
                   std::string*) {
  out.meshes.push_back(scene_gen::PlacementMesh{"pieces/box.glb", "pieces/box", 0});
  for (const Piece& piece : g_pieces)
    out.instances.push_back(piece_placement(piece, context.ground));
  out.things = g_pieces.size();
  return true;
}
constexpr scene_gen::PlacementGeneratorDesc k_pieces{.name = "scene-gen-test-pieces",
                                                     .open = &pieces_open,
                                                     .close = &pieces_close,
                                                     .expand = &pieces_expand};
const scene_gen::Registrar k_pieces_registrar{k_pieces};

// `g_pieces` read through the scene reader over the sawtooth, and each made the GPU's instance the
// way `load_scene` makes one — `make_instance`: the mesh's `fit`, then the placement, composed in
// f64 by `instance_translation` and stored as a cell and a local. The instance records are the
// bytes the GPU scene uploads.
void read_pieces(const test::TempDir& tmp, const Mat4& fit, Vector<SceneInstance>& sources,
                 Vector<gfx::InstanceDesc>& out) {
  const std::string path = tmp.file("pieces.json");
  REQUIRE(write_text(path, R"({"format":"engine.scene.v1","name":"pieces",)"
                           R"("terrain":{"size":17,"extent":20,"provider":"scene-gen-test-saw"},)"
                           R"("placements":[{"generator":"scene-gen-test-pieces","params":{}}]})"));
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  SceneData scene;
  scene.parts.resize(desc.meshes.size());
  scene.mesh_fit.resize(desc.meshes.size(), fit);
  for (const SceneInstance& source : desc.instances) {
    if (desc.mesh_info[source.mesh].name != "pieces/box") continue;  // the terrain
    gfx::InstanceDesc instance;
    REQUIRE_MESSAGE(make_instance(scene, source, 0, instance, &error), error);
    sources.push_back(source);
    out.push_back(instance);
  }
  REQUIRE(out.size() == g_pieces.size());
}

}  // namespace

// **A placement lands within 2 µm of where its generator put it, anywhere** (ADR-0053;
// scene_gen.md, "Far from the origin"). A box 16.123456789 m by -7.654321098 m from each far site
// (and one 0.4 µm either side of a cell's edge there), turned and lifted, under a fit like a
// content mesh's: the generator's `WorldPos` and the mesh's fit composed in f64 is where the
// instance's cell and local put the mesh's origin, to the local's own rounding (half a float step
// at 64 m, 1.9 µm), and the floor it stands on is the ground's at the millimetre it means:
// measured 0.83 µm and 1.12 µm at every site (2026-10-06, MSVC). Until then a placement was a
// float32 world position, and the same boxes stood 3.2 mm off at 419 km and 0.39 m at 10,000 km and
// 1e8 m (docs/experiments/far-from-origin-2026-10-04.md, "Part 3: placements").
TEST_CASE("renderer: a placement lands within 2 um of where its generator put it, far out too") {
  const test::TempDir tmp("renderer_scene_gen_far");
  const Mat4 fit = scaling(Vec3{0.75f, 0.75f, 0.75f}) * translation(Vec3{-0.3125f, 0.5f, 0.1875f});
  const f64 sites[] = {0.0, 419072.0, 10000000.0, 100000000.0};
  const DVec3 offsets[] = {DVec3{16.123456789, 0.0, -7.654321098},
                           DVec3{-0.0000004, 0.0, 0.0000004}};  // either side of a cell's edge
  g_pieces.clear();
  for (const f64 site : sites) {
    for (const DVec3& offset : offsets)
      g_pieces.push_back(Piece{WorldPos{site, 0.0, -site} + offset, 1.25f, 1.0f, 30.0f});
  }
  Vector<SceneInstance> sources;
  Vector<gfx::InstanceDesc> got;
  read_pieces(tmp, fit, sources, got);
  for (u32 k = 0; k < g_pieces.size(); ++k) {
    const Piece& piece = g_pieces[k];
    // The frame where the generator put it — on the sawtooth's floor at its millimetre — and the
    // fit's offset turned and scaled, added in f64.
    const WorldPos frame{
        piece.at.x, static_cast<f64>(saw_of(scene_gen::nearest_mm(piece.at.x))) + 1.25, piece.at.z};
    CHECK(sources[k].origin == frame);
    const scene_gen::Placement p = piece_placement(piece, scene_gen::Ground{});
    const WorldPos want = frame + DVec3{(mat4_from_transform(p.turn()) * fit).c[3].xyz()};
    const f64 off = length(to_world(gfx::instance_cell(got[k])) - want);
    MESSAGE(sites[k / 2] << " m" << std::string(k % 2 == 1 ? " at a cell's edge" : "")
                         << ": the mesh's origin " << off << " m from where the generator put it");
    CHECK(off <= 2.0e-6);
  }
}

// **Two pieces laid to touch far out leave no gap** (ADR-0053). A box 0.6 m wide and one 0.8 m
// wide, their faces meeting 0.3 m short of a cell's edge with the boxes on either side of it, at
// 419 km, 10,000 km and 1e8 m; the faces measured as every pass measures a vertex, in the frame of
// an eye at the seam (`gfx::instance_matrix`, `instance_point` in shaders/scene.slang). The gap is
// within a local's rounding at the size of the cell (two half steps at 64 m, 3.8 µm): measured
// 1.55 µm closed at 419 km and 10,000 km and 1.58 µm at 1e8 m. When the generator placed in float32
// world coordinates (the old arithmetic, put back to measure it) the faces were 12.5 mm closed at
// 419 km, 0.3 m open at 10,000 km and 0.7 m closed at 1e8 m
// (docs/experiments/far-from-origin-2026-10-04.md, "Part 3: placements").
TEST_CASE("renderer: two pieces laid to touch far out leave no gap") {
  const test::TempDir tmp("renderer_scene_gen_touch");
  const f64 edges[] = {419072.0, 10000000.0, 100000000.0};
  g_pieces.clear();
  for (const f64 edge : edges) {
    g_pieces.push_back(Piece{WorldPos{edge - 0.6, 0.0, 3.7}, 0.5f, 0.6f, 0.0f});
    g_pieces.push_back(Piece{WorldPos{edge + 0.1, 0.0, 3.7}, 0.5f, 0.8f, 0.0f});
  }
  Vector<SceneInstance> sources;
  Vector<gfx::InstanceDesc> got;
  read_pieces(tmp, Mat4::identity(), sources, got);
  for (u32 e = 0; e < 3; ++e) {
    const WorldEye eye = to_eye(WorldPos{edges[e] - 0.3, 1.0, 3.7});
    const f32 east =
        transform_point(gfx::instance_matrix(got[2 * e], eye), Vec3{0.5f, 0.0f, 0.0f}).x;
    const f32 west =
        transform_point(gfx::instance_matrix(got[2 * e + 1], eye), Vec3{-0.5f, 0.0f, 0.0f}).x;
    const f32 gap = west - east;
    MESSAGE(edges[e] << " m: the faces laid to touch are " << gap << " m apart");
    CHECK(std::fabs(gap) <= 4.0e-6f);
  }
}

// **A scene file's scatter far out is its twin by the origin, moved** (ADR-0053; renderer.md,
// "Positions in the files and on the wire"; `engine.scene.Scatter` version 2). The centre is a
// `worldpos` and each instance's place is the centre plus the float32 offset the seed draws, added
// in f64, so a scatter at 419 km, 10,000 km and 1e8 m — and one whose centre is a 1024th of a metre
// short of a cell's edge 10,000 km out, so its instances stand either side of it — puts every
// instance exactly where the same scatter by the origin puts it, moved by the centre; and a
// grounded one stands on the sawtooth at the millimetre it is at. Until 2026-10-07 the centre was a
// float32 `vec2` and the place float32 world metres: at 10,000 km a metre's grid.
TEST_CASE("renderer: a scene file's scatter far out is its twin by the origin, moved") {
  const test::TempDir tmp("renderer_scatter_far");
  struct Centre {
    f64 x;
    f64 z;
  };
  const Centre centres[] = {{0.0, 0.0},
                            {419072.0, -419072.0},
                            {10000000.0, 10000000.0},
                            {100000000.0, -100000000.0},
                            {10000000.0 + 64.0 - 0.0009765625, 10000000.0}};
  constexpr u32 k_count = 16;
  Vector<SceneInstance> read[5];
  for (u32 c = 0; c < 5; ++c) {
    // The same two scatters, in the same places in the file (a scatter's seed is its own and its
    // index's): one grounded, one at a height of its own. The centre's y is not read.
    char centre[128];
    std::snprintf(centre, sizeof(centre), "[%.17g, 123.5, %.17g]", centres[c].x, centres[c].z);
    const std::string path = tmp.file("scatter-" + std::to_string(c) + ".json");
    REQUIRE(write_text(
        path,
        std::string(R"({"format":"engine.scene.v1","name":"scatter",)") +
            R"("meshes":[{"name":"marker","path":"marker.glb"}],)" +
            R"("terrain":{"size":17,"extent":20,"provider":"scene-gen-test-saw"},)" +
            R"("scatters":[{"mesh":0,"center":)" + centre +
            R"(,"radius_min":2,"radius_max":6,"count":16,"seed":9,"y":0.25},)" +
            R"({"mesh":0,"center":)" + centre +
            R"(,"radius_min":2,"radius_max":6,"count":16,"seed":9,"ground":false,"y":1.5}]})"));
    SceneDesc desc;
    std::string error;
    REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
    REQUIRE(desc.instances.size() == 2 * k_count + 1);  // and the terrain's
    for (u32 k = 0; k < 2 * k_count; ++k)
      read[c].push_back(desc.instances[k]);
  }
  // By the origin each place is the offset itself, exactly: the centre is zero.
  const Vector<SceneInstance>& home = read[0];
  u32 off = 0, lifted = 0, floated = 0, west = 0, east = 0;
  for (u32 c = 1; c < 5; ++c) {
    for (u32 k = 0; k < 2 * k_count; ++k) {
      const SceneInstance& got = read[c][k];
      // The centre plus the offset in f64, as the reader adds them, and nothing in between.
      off += got.origin.x == centres[c].x + home[k].origin.x &&
                     got.origin.z == centres[c].z + home[k].origin.z &&
                     got.transform.position == Vec3{} &&
                     std::memcmp(&got.transform.rotation, &home[k].transform.rotation,
                                 sizeof(Quat)) == 0 &&
                     got.transform.scale == home[k].transform.scale
                 ? 0u
                 : 1u;
      if (k < k_count) {
        // Grounded: `y` above the sawtooth at the millimetre the instance is at.
        const f64 want = 0.25 + static_cast<f64>(saw_of(scene_gen::nearest_mm(got.origin.x)));
        lifted += got.origin.y == want ? 0u : 1u;
      } else {
        floated += got.origin.y == 1.5 ? 0u : 1u;
      }
      if (c == 4) {
        west += got.origin.x < 10000064.0 ? 1u : 0u;
        east += got.origin.x > 10000064.0 ? 1u : 0u;
      }
    }
  }
  MESSAGE("scatters far out: " << off << " places off their twins', " << lifted
                               << " grounded off the millimetre's height, " << floated
                               << " floated off theirs; at the cell's edge " << west
                               << " instances west of it and " << east << " east");
  CHECK(off == 0u);
  CHECK(lifted == 0u);
  CHECK(floated == 0u);
  CHECK(west > 0u);
  CHECK(east > 0u);
}

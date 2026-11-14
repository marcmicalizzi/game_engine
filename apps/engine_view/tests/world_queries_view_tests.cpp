// End to end: what a game module can ask the world (hotload ABI 10; roadmap A43; docs/subsystems/
// apps.md, "What a module can ask the world"). The committed oasis pool is copied into the test's
// scratch directory and given three meshes — an orchard tree solved from its recipe at (0, 18), a
// post written here as a GLB whose node `perch` declares a perch on its top at (6, 16), a boulder
// (the unit box, a metre tall) at (-6, 16) — and the prop `bird`. The fixture module
// `walk_poc_world` (game/walk_poc/world_probe.cpp) is replayed **offscreen** from a session
// standing at the pool's south shore and asks, once the collision holds its tiles: the supports
// near the tree's crown, the post and the boulder (and lands a bird on each), a ray at the trunk,
// sixteen rays straight down through the crown, a sphere at the boulder, a ray into the pond that
// stops at the water, and the water at five points of the pond, whose depths are held to the depth
// map the scene's reader cooked beside the pond's mesh. Needs a device for the run, and skips
// without.

#include "../fly_camera.h"
#include "view_process.h"

#include <core/json/json.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>
#include <systems/water/water.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace engine::view_test {
bool write_box_glb(const std::string& path);  // game_props_tests.cpp
}

#if defined(WALK_POC_WORLD_PATH)

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run run_view(const std::vector<std::string>& args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  Run run;
  run.exit_code = view_test::run_program(
      std::span<const std::string_view>(argv.data(), argv.size()), run.output);
  return run;
}

bool summary_of(const Run& run, JsonValue& out) {
  usize end = run.output.size();
  while (end > 0) {
    usize begin = run.output.find_last_of('\n', end - 1);
    begin = begin == std::string::npos ? 0 : begin + 1;
    const std::string line = run.output.substr(begin, end - begin);
    if (!line.empty() && line[0] == '{' && parse_json(line, out).ok && out.is_object()) return true;
    if (begin == 0) break;
    end = begin - 1;
  }
  return false;
}

f64 num(const JsonValue* v, const char* key, f64 fallback = -1.0e9) {
  const JsonValue* f = v != nullptr ? v->find(key) : nullptr;
  f64 out = fallback;
  if (f != nullptr) (void)f->get_f64(out);
  return out;
}
f64 element(const JsonValue* v, const char* key, u32 i) {
  const JsonValue* f = v != nullptr ? v->find(key) : nullptr;
  f64 out = -1.0e9;
  if (f != nullptr && f->is_array() && f->size() > i) (void)(*f)[i].get_f64(out);
  return out;
}

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>(v >> (8 * i)));
}
void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

// A post 0.3 m square and 1.2 m tall standing on its origin, and beside its mesh's node a node
// `perch` on its top centre whose extras declare a 4 cm perch (assets.md, "Perches"): what an
// author's empty in Blender exports as.
bool write_post_glb(const std::string& path) {
  std::vector<u8> bin;
  const Vec3 lo{-0.15f, 0.0f, -0.15f};
  const Vec3 hi{0.15f, 1.2f, 0.15f};
  for (u32 k = 0; k < 8; ++k) {
    put_f32(bin, (k & 1) ? hi.x : lo.x);
    put_f32(bin, (k & 2) ? hi.y : lo.y);
    put_f32(bin, (k & 4) ? hi.z : lo.z);
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  const u32 faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                           {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
  for (const auto& f : faces) {
    const u32 tri[6] = {f[0], f[1], f[2], f[0], f[2], f[3]};
    for (const u32 v : tri)
      put_u32(bin, v);
  }
  const u32 index_bytes = static_cast<u32>(bin.size()) - index_offset;
  const auto n = [](u32 v) { return std::to_string(v); };
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0,1]}],"
      "\"nodes\":[{\"mesh\":0,\"name\":\"post\"},{\"name\":\"perch\",\"translation\":[0,1.2,0],"
      "\"extras\":{\"perch\":{\"size_m\":0.04}}}],"
      "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,"
      "\"material\":0}]}],\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":"
      "[0.5,0.35,0.2,1],\"metallicFactor\":0,\"roughnessFactor\":0.8}}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":8,\"type\":\"VEC3\","
      "\"min\":[-0.15,0,-0.15],\"max\":[0.15,1.2,0.15]},{\"bufferView\":1,\"componentType\":5125,"
      "\"count\":36,\"type\":\"SCALAR\"}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,"
      "\"byteLength\":" +
      n(index_offset) + "},{\"buffer\":0,\"byteOffset\":" + n(index_offset) +
      ",\"byteLength\":" + n(index_bytes) +
      "}],\"buffers\":[{\"byteLength\":" + n(static_cast<u32>(bin.size())) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';
  std::vector<u8> glb;
  put_u32(glb, 0x46546c67u);
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);
  glb.insert(glb.end(), json.begin(), json.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);
  glb.insert(glb.end(), bin.begin(), bin.end());
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return file.good();
}

// The pond's depth map as the scene's reader cooked it (water.md, "The depth map"): the one
// `.depth` file under the run's derived-data root.
bool read_cooked_map(const std::string& ddc, water::DepthMap& out) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(ddc, ec)) {
    if (entry.path().extension() != ".depth") continue;
    std::string bytes;
    if (io::read_file(entry.path().string(), bytes) != io::Status::Ok) return false;
    std::string error;
    return water::read_depth_map(
        std::span<const u8>(reinterpret_cast<const u8*>(bytes.data()), bytes.size()), out, &error);
  }
  return false;
}

// The map's bilinear depth at body-local (x, z), as the water query reads it (water/query.cpp).
f32 map_depth(const water::DepthMap& map, f32 x, f32 z) {
  const f32 u = (x - map.origin.x) / map.texel_m;
  const f32 v = (z - map.origin.y) / map.texel_m;
  const u32 i = static_cast<u32>(u);
  const u32 j = static_cast<u32>(v);
  const f32 fu = u - static_cast<f32>(i);
  const f32 fv = v - static_cast<f32>(j);
  const f32 a = water::depth_map_depth(map, i, j), b = water::depth_map_depth(map, i + 1, j);
  const f32 c = water::depth_map_depth(map, i, j + 1);
  const f32 d = water::depth_map_depth(map, i + 1, j + 1);
  const f32 top = a + (b - a) * fu;
  return top + ((c + (d - c) * fu) - top) * fv;
}

}  // namespace

TEST_CASE("engine-view: a module asks the world for casts, supports and the water") {
  const test::TempDir tmp("engine_view_world_queries");
  const std::string committed =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/oasis-pool/scene.json",
                      "content/test-scenes/oasis-pool/scene.json");
  const std::string recipe =
      test::data_path(ENGINE_SOURCE_DIR "/content/flora/orchard-apple.flora.json",
                      "content/flora/orchard-apple.flora.json");
  if (!test::path_exists(committed) || !test::path_exists(recipe)) {
    MESSAGE("the oasis-pool scene or the orchard recipe is not in this bundle");
    return;
  }
  // The scene: the pool, and the three meshes and the prop.
  std::string text;
  REQUIRE(io::read_file(committed, text) == io::Status::Ok);
  JsonValue scene;
  REQUIRE(parse_json(text, scene).ok);
  std::string tree;
  REQUIRE(io::read_file(recipe, tree) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("orchard-apple.flora.json"), tree) == io::Status::Ok);
  REQUIRE(write_post_glb(tmp.file("post.glb")));
  REQUIRE(view_test::write_box_glb(tmp.file("box.glb")));
  JsonValue extra;
  REQUIRE(parse_json(R"({
    "meshes": [
      {"path": "orchard-apple.flora.json", "name": "tree"},
      {"path": "post.glb", "name": "post"},
      {"path": "box.glb", "name": "boulder", "fit": {"height": 1}}
    ],
    "instances": [
      {"mesh": 0, "ground": true, "translation": [0, 0, 18], "name": "tree"},
      {"mesh": 1, "ground": true, "translation": [6, 0, 16], "name": "post"},
      {"mesh": 2, "ground": true, "translation": [-6, 0, 16], "name": "boulder"}
    ],
    "props": [{"name": "bird", "mesh": "box.glb", "fit": {"height": 0.15}}]
  })",
                     extra)
              .ok);
  for (const char* key : {"meshes", "instances", "props"})
    scene.set(key, *extra.find(key));
  const std::string scene_path = tmp.file("scene.json");
  REQUIRE(io::write_file(scene_path, write_json(scene)) == io::Status::Ok);

  // The session: standing at the pool's south shore looking north, nothing pressed.
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  view::SessionHeader header;
  header.ticks = 720;
  header.scene = scene_path;
  header.start.position = WorldPos{0.0, 3.0, 12.5};
  header.start.yaw = 3.14159265f;
  header.start.pitch = 0.0f;
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  const std::string session = tmp.file("session.jsonl");
  REQUIRE(keys.save(session) == io::Status::Ok);
  const std::string ddc = tmp.file("ddc");
  const Run run =
      run_view({"--replay-input", session, "--scene", scene_path, "--offscreen", "--width", "160",
                "--height", "96", "--ddc", ddc, "--game", WALK_POC_WORLD_PATH});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* interactive = summary.find("interactive");
  REQUIRE(interactive != nullptr);
  const JsonValue* game = interactive->find("game");
  REQUIRE(game != nullptr);
  const JsonValue* result = game->find("result");
  const bool has_result = result != nullptr && result->is_object();
  REQUIRE_MESSAGE(has_result, write_json(*game));
  MESSAGE("the module's answers: " << write_json(*result));
  if (const JsonValue* asked = game->find("world_queries"))
    MESSAGE("what the asking cost: " << write_json(*asked));

  // A bird on the tree's limb: a part of its structural table, on its top, landed.
  const JsonValue* t = result->find("tree");
  REQUIRE(t != nullptr);
  CHECK(num(t, "result") == 0.0);
  CHECK(num(t, "covered") == 1.0);
  REQUIRE(num(t, "count") >= 1.0);
  CHECK(num(t, "kind") == 2.0);  // k_support_limb
  CHECK(num(t, "what") == 3.0);  // WorldHit::Instance
  CHECK(num(t, "size_m") >= 0.03);
  CHECK(element(t, "up", 1) > 0.5);
  CHECK(num(t, "landed") == 1.0);
  // On the post's declared perch: its top centre, 4 cm.
  const JsonValue* p = result->find("post");
  REQUIRE(p != nullptr);
  REQUIRE(num(p, "count") >= 1.0);
  CHECK(num(p, "kind") == 1.0);  // k_support_perch
  CHECK(num(p, "part") == 0.0);
  CHECK(element(p, "position", 0) == doctest::Approx(6.0).epsilon(1.0e-4));
  CHECK(element(p, "position", 2) == doctest::Approx(16.0).epsilon(1.0e-4));
  CHECK(num(p, "size_m") == doctest::Approx(0.04).epsilon(1.0e-4));
  CHECK(num(p, "landed") == 1.0);
  // On the boulder's top: a flat face, facing up.
  const JsonValue* b = result->find("boulder");
  REQUIRE(b != nullptr);
  REQUIRE(num(b, "count") >= 1.0);
  CHECK(num(b, "kind") == 3.0);  // k_support_top
  CHECK(element(b, "up", 1) > 0.99);
  CHECK(num(b, "landed") == 1.0);
  // The ray at the trunk stops at it, short of its axis: an instance of the scene.
  const JsonValue* trunk = result->find("trunk");
  REQUIRE(trunk != nullptr);
  CHECK(num(trunk, "result") == 0.0);
  CHECK(num(trunk, "what") == 3.0);
  CHECK(num(trunk, "distance") > 1.0);  // from 1.5 m west of the axis, less the bark's radius
  CHECK(num(trunk, "distance") < 1.5);
  // The crown: rays straight down 1.5 m round the trunk pass its leaves to the ground; a branch may
  // stop some, never the leaves.
  const JsonValue* crown = result->find("crown");
  REQUIRE(crown != nullptr);
  REQUIRE(crown->is_array());
  u32 ground = 0;
  u32 wood = 0;
  for (u32 k = 0; k < crown->size(); ++k) {
    f64 what = -1.0;
    REQUIRE((*crown)[k].is_array());
    (void)(*crown)[k][1].get_f64(what);
    ground += what == 1.0 ? 1u : 0u;
    wood += what == 3.0 ? 1u : 0u;
  }
  MESSAGE("down through the crown: " << ground << " of 16 to the ground, " << wood << " on wood");
  CHECK(ground >= 8u);
  // The sphere at the boulder's west face (x = -6.5): its centre a radius (0.1 m) short of it.
  const JsonValue* sphere = result->find("sphere");
  REQUIRE(sphere != nullptr);
  CHECK(num(sphere, "what") == 3.0);
  CHECK(std::fabs(element(sphere, "position", 0) - -6.6) < 0.01);
  // Into the pond: it stops at the water's surface, the pool's level.
  const JsonValue* pond = result->find("pond");
  REQUIRE(pond != nullptr);
  CHECK(num(pond, "result") == 0.0);
  CHECK(num(pond, "what") == 4.0);  // WorldHit::Water
  CHECK(element(pond, "position", 1) == doctest::Approx(-0.45).epsilon(0.01));
  // The water at five points: wet at the pool's level, and the depth the cooked map's.
  water::DepthMap map;
  REQUIRE(read_cooked_map(ddc, map));
  const JsonValue* waters = result->find("water");
  REQUIRE(waters != nullptr);
  REQUIRE(waters->size() == 5u);
  for (u32 k = 0; k < 5; ++k) {
    const JsonValue& w = (*waters)[k];
    CHECK(num(&w, "result") == 0.0);
    CHECK(num(&w, "wet") == 1.0);
    CHECK(num(&w, "level") == doctest::Approx(-0.45).epsilon(1.0e-6));
    const f32 want = map_depth(map, static_cast<f32>(num(&w, "x")), static_cast<f32>(num(&w, "z")));
    MESSAGE("water at (" << num(&w, "x") << ", " << num(&w, "z") << "): " << num(&w, "depth")
                         << " m, the map " << want << " m");
    CHECK(num(&w, "depth") == doctest::Approx(static_cast<f64>(want)).epsilon(1.0e-6));
    CHECK(element(&w, "velocity", 0) == 0.0);
  }
}

#endif

// The game's props with no GPU and no module (game_props.h; docs/subsystems/apps.md "The game
// contract", roadmap P43): the table's services and their refusals, its ids across removal and
// reuse, the nearest query, the tail it hands the renderer, the state file's round trip — and the
// scene reader's `props` block, whose meshes load with the scene and are placed by nothing. The
// end-to-end cases (game_props_view_tests.cpp) draw them.

#include "../game_props.h"
#include "../game_runner.h"

#include <core/json/json.h>
#include <foundation/input/input.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace engine;
using hotload::Result;

namespace {

Vector<renderer::SceneProp> declared(std::initializer_list<const char*> names) {
  Vector<renderer::SceneProp> out;
  for (const char* name : names)
    out.push_back(renderer::SceneProp{name, 0});
  return out;
}

// A scene as the loader leaves it, with `n` props naming meshes 10.. (the table reads the index).
renderer::SceneData attached_scene(std::initializer_list<const char*> names) {
  renderer::SceneData scene;
  u32 mesh = 10;
  for (const char* name : names)
    scene.props.push_back(renderer::SceneProp{name, mesh++});
  return scene;
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

}  // namespace

namespace engine::view_test {

// A unit cube as a GLB (one quad a face): the prop every case here places.
bool write_box_glb(const std::string& path) {
  std::vector<Vec3> positions;
  std::vector<Vec3> normals;
  std::vector<u32> indices;
  const Vec3 face[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                        Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
  const Vec3 tangent[6] = {Vec3{0, 1, 0}, Vec3{0, 0, 1}, Vec3{0, 0, 1},
                           Vec3{1, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 1, 0}};
  constexpr u32 n = 4;
  for (u32 f = 0; f < 6; ++f) {
    const Vec3 nrm = face[f];
    const Vec3 t = tangent[f];
    const Vec3 b = cross(nrm, t);
    const u32 base = static_cast<u32>(positions.size());
    for (u32 j = 0; j <= n; ++j) {
      for (u32 i = 0; i <= n; ++i) {
        const f32 s = static_cast<f32>(i) / static_cast<f32>(n) - 0.5f;
        const f32 r = static_cast<f32>(j) / static_cast<f32>(n) - 0.5f;
        positions.push_back(nrm * 0.5f + t * s + b * r);
        normals.push_back(nrm);
      }
    }
    for (u32 j = 0; j < n; ++j) {
      for (u32 i = 0; i < n; ++i) {
        const u32 a = base + j * (n + 1) + i;
        const u32 quad[6] = {a, a + 1, a + n + 2, a, a + n + 2, a + n + 1};
        for (const u32 q : quad)
          indices.push_back(q);
      }
    }
  }
  std::vector<u8> bin;
  for (const Vec3& p : positions) {
    put_f32(bin, p.x);
    put_f32(bin, p.y);
    put_f32(bin, p.z);
  }
  const u32 normal_offset = static_cast<u32>(bin.size());
  for (const Vec3& v : normals) {
    put_f32(bin, v.x);
    put_f32(bin, v.y);
    put_f32(bin, v.z);
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  for (const u32 i : indices)
    put_u32(bin, i);
  const u32 index_bytes = static_cast<u32>(bin.size()) - index_offset;
  const auto num = [](u32 v) { return std::to_string(v); };
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,"
      "\"NORMAL\":1},\"indices\":2,\"material\":0}]}],\"materials\":[{\"pbrMetallicRoughness\":"
      "{\"baseColorFactor\":[0.1,0.2,0.9,1],\"metallicFactor\":0,\"roughnessFactor\":0.6}}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      num(static_cast<u32>(positions.size())) +
      ",\"type\":\"VEC3\",\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
      num(static_cast<u32>(normals.size())) +
      ",\"type\":\"VEC3\"},{\"bufferView\":2,\"componentType\":5125,\"count\":" +
      num(static_cast<u32>(indices.size())) +
      ",\"type\":\"SCALAR\"}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      num(normal_offset) + "},{\"buffer\":0,\"byteOffset\":" + num(normal_offset) +
      ",\"byteLength\":" + num(index_offset - normal_offset) +
      "},{\"buffer\":0,\"byteOffset\":" + num(index_offset) +
      ",\"byteLength\":" + num(index_bytes) +
      "}],\"buffers\":[{\"byteLength\":" + num(static_cast<u32>(bin.size())) + "}]}";
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

}  // namespace engine::view_test

TEST_CASE("game props: a declared prop is placed, moved, found, removed, and its id stays dead") {
  view::GameProps props;
  props.declare(declared({"can", "marker"}));
  CHECK(props.declared());
  CHECK_FALSE(props.attached());
  u32 can = 99;
  u32 marker = 99;
  CHECK(props.mesh_of("can", can) == Result::Ok);
  CHECK(props.mesh_of("marker", marker) == Result::Ok);
  CHECK(can == 0);
  CHECK(marker == 1);
  u32 none = 0;
  CHECK(props.mesh_of("cup", none) == Result::NotFound);
  // Before the scene is attached nothing can be placed: the instances would name no mesh.
  const f64 at[3] = {10.0, 1.0, -4.0};
  u32 id = 0;
  CHECK(props.place(can, at, 0.0f, 1.0f, id) == Result::Unsupported);
  std::string error;
  REQUIRE_MESSAGE(props.attach(attached_scene({"can", "marker"}), &error), error);

  REQUIRE(props.place(can, at, 0.5f, 1.0f, id) == Result::Ok);
  CHECK(id >= view::k_max_props);  // generation 1 in the upper bits: never zero, never a bare slot
  CHECK(props.live() == 1);
  CHECK(props.changed());
  const Vector<renderer::SceneInstance>& tail = props.tail();
  REQUIRE(tail.size() == 1);
  CHECK(tail[0].mesh == 10);
  CHECK(tail[0].origin.x == 10.0);
  CHECK(tail[0].origin.z == -4.0);
  CHECK(tail[0].transform.position.x == 0.0f);  // the world position whole in the origin
  CHECK(tail[0].transform.scale.x == 1.0f);
  props.handed();
  CHECK_FALSE(props.changed());
  CHECK(props.stats().rewrites == 1);

  // Nearest: over the ground, from where the walker stands.
  u32 found = 0;
  WorldPos where{};
  f64 distance = 0.0;
  CHECK(props.nearest(can, WorldPos{10.0, 50.0, -1.0}, 5.0, found, where, distance) == Result::Ok);
  CHECK(found == id);
  CHECK(distance == doctest::Approx(3.0));
  CHECK(where.y == 1.0);
  CHECK(props.nearest(can, WorldPos{10.0, 0.0, -1.0}, 2.0, found, where, distance) ==
        Result::NotFound);
  CHECK(props.nearest(marker, WorldPos{10.0, 0.0, -4.0}, 100.0, found, where, distance) ==
        Result::NotFound);
  CHECK(props.nearest(view::k_any_prop, WorldPos{10.0, 0.0, -4.0}, 1.0, found, where, distance) ==
        Result::Ok);

  const f64 moved_to[3] = {20.0, 1.0, -4.0};
  CHECK(props.move(id, moved_to, 1.0f) == Result::Ok);
  CHECK(props.changed());
  CHECK(props.tail()[0].origin.x == 20.0);
  CHECK(props.remove(id) == Result::Ok);
  CHECK(props.live() == 0);
  CHECK(props.tail().empty());
  CHECK(props.remove(id) == Result::NotFound);
  CHECK(props.move(id, at, 0.0f) == Result::NotFound);
  // The slot is reused; the old id is not.
  u32 again = 0;
  REQUIRE(props.place(marker, at, 0.0f, 2.0f, again) == Result::Ok);
  CHECK(again != id);
  CHECK((again & (view::k_max_props - 1)) == (id & (view::k_max_props - 1)));
  CHECK(props.remove(id) == Result::NotFound);
  CHECK(props.tail()[0].mesh == 11);
  CHECK(props.tail()[0].transform.scale.y == 2.0f);
  const JsonValue summary = props.summary_json();
  f64 n = 0.0;
  REQUIRE(summary.find("placed")->get_f64(n));
  CHECK(n == 2.0);
  REQUIRE(summary.find("removed")->get_f64(n));
  CHECK(n == 1.0);
  REQUIRE(summary.find("moved")->get_f64(n));
  CHECK(n == 1.0);
}

TEST_CASE("game props: the refusals, and the table's cap") {
  view::GameProps props;
  // No props declared: the services say so, and nothing is remembered against the module.
  u32 out = 0;
  CHECK(props.mesh_of("can", out) == Result::Unsupported);
  const f64 at[3] = {0.0, 0.0, 0.0};
  CHECK(props.place(0, at, 0.0f, 1.0f, out) == Result::Unsupported);
  props.declare(declared({"can"}));
  std::string error;
  REQUIRE(props.attach(attached_scene({"can"}), &error));
  CHECK(props.place(7, at, 0.0f, 1.0f, out) == Result::NotFound);
  const f64 nan[3] = {0.0, std::numeric_limits<f64>::quiet_NaN(), 0.0};
  CHECK(props.place(0, nan, 0.0f, 1.0f, out) == Result::InvalidArgument);
  CHECK(props.place(0, at, std::numeric_limits<f32>::infinity(), 1.0f, out) ==
        Result::InvalidArgument);
  CHECK(props.place(0, at, 0.0f, 0.0f, out) == Result::InvalidArgument);
  CHECK(props.place(0, at, 0.0f, -1.0f, out) == Result::InvalidArgument);
  const f64 far[3] = {k_world_extent_m * 2.0, 0.0, 0.0};
  CHECK(props.place(0, far, 0.0f, 1.0f, out) == Result::InvalidArgument);
  CHECK(props.live() == 0);
  for (u32 i = 0; i < view::k_max_props; ++i) {
    const f64 p[3] = {static_cast<f64>(i), 0.0, 0.0};
    REQUIRE(props.place(0, p, 0.0f, 1.0f, out) == Result::Ok);
  }
  CHECK(props.live() == view::k_max_props);
  CHECK(props.place(0, at, 0.0f, 1.0f, out) == Result::OutOfRange);
  CHECK(props.tail().size() == view::k_max_props);
  // The attach refuses a scene whose props are not the declared ones.
  view::GameProps other;
  other.declare(declared({"can", "cup"}));
  CHECK_FALSE(other.attach(attached_scene({"can"}), &error));
  CHECK(error.find("declared") != std::string::npos);
  CHECK_FALSE(other.attach(attached_scene({"can", "mug"}), &error));
  CHECK(error.find("\"mug\"") != std::string::npos);
}

TEST_CASE("game props: the state file's props read back to the same slots, ids and bits") {
  view::GameProps props;
  props.declare(declared({"can", "marker"}));
  std::string error;
  REQUIRE(props.attach(attached_scene({"can", "marker"}), &error));
  u32 a = 0;
  u32 b = 0;
  u32 c = 0;
  const f64 pa[3] = {1.0 / 3.0, 2.0 / 7.0, -1234567.25};
  const f64 pb[3] = {5.0, 0.0, 5.0};
  const f64 pc[3] = {6.0, 0.0, 6.0};
  REQUIRE(props.place(0, pa, 0.1f, 0.75f, a) == Result::Ok);
  REQUIRE(props.place(1, pb, 2.0f, 1.0f, b) == Result::Ok);
  REQUIRE(props.place(1, pc, 3.0f, 1.0f, c) == Result::Ok);
  REQUIRE(props.remove(b) == Result::Ok);  // a hole between the two
  const std::string text = write_json(props.to_json());
  JsonValue parsed;
  REQUIRE(parse_json(text, parsed).ok);

  view::GameProps again;
  again.declare(declared({"can", "marker"}));
  REQUIRE_MESSAGE(again.from_json(parsed, &error), error);
  REQUIRE(again.attach(attached_scene({"can", "marker"}), &error));
  CHECK(again.live() == 2);
  CHECK(again.changed());  // restored props reach the renderer on the first frame
  // The same ids answer, and the hole is still a hole.
  const f64 moved[3] = {0.0, 0.0, 0.0};
  CHECK(again.move(a, moved, 0.0f) == Result::Ok);
  CHECK(again.move(c, moved, 0.0f) == Result::Ok);
  CHECK(again.move(b, moved, 0.0f) == Result::NotFound);
  // The bits: a position written as doubles comes back as the same doubles.
  view::GameProps bits;
  bits.declare(declared({"can", "marker"}));
  REQUIRE(bits.from_json(parsed, &error));
  REQUIRE(bits.attach(attached_scene({"can", "marker"}), &error));
  const Vector<renderer::SceneInstance>& tail = bits.tail();
  REQUIRE(tail.size() == 2);
  CHECK(tail[0].origin.x == pa[0]);
  CHECK(tail[0].origin.y == pa[1]);
  CHECK(tail[0].origin.z == pa[2]);
  CHECK(tail[0].transform.scale.x == 0.75f);
  CHECK(write_json(bits.to_json()) == text);
  // A file naming a prop the scene does not declare is refused by name.
  view::GameProps wrong;
  wrong.declare(declared({"cup"}));
  CHECK_FALSE(wrong.from_json(parsed, &error));
  CHECK(error.find("\"can\"") != std::string::npos);
}

TEST_CASE("scene file: props are meshes loaded with the scene and placed by nothing") {
  const test::TempDir tmp("scene_props");
  REQUIRE(view_test::write_box_glb(tmp.file("box.glb")));
  const std::string scene_path = tmp.file("scene.json");
  REQUIRE(io::write_file(scene_path, R"({"format":"engine.scene.v1","name":"props",
      "meshes":[{"name":"ground","path":"box.glb"}],
      "instances":[{"mesh":0,"translation":[0,0,0],"scale":[20,0.1,20]}],
      "props":[{"name":"can","mesh":"box.glb","fit":{"height":0.4}},
               {"name":"marker","mesh":"box.glb"}]})") == io::Status::Ok);
  // The names alone, before anything is loaded: what the module's `on_load` is answered from.
  Vector<renderer::SceneProp> names;
  std::string error;
  REQUIRE_MESSAGE(renderer::read_scene_props(scene_path, names, error), error);
  REQUIRE(names.size() == 2);
  CHECK(names[0].name == "can");
  CHECK(names[1].name == "marker");
  // The whole read: the props' meshes after the file's own, no instance of them.
  renderer::SceneDesc desc;
  REQUIRE_MESSAGE(renderer::read_scene_file(scene_path, desc, error), error);
  REQUIRE(desc.meshes.size() == 3);
  REQUIRE(desc.props.size() == 2);
  CHECK(desc.props[0].mesh == 1);
  CHECK(desc.props[1].mesh == 2);
  CHECK(desc.mesh_info[1].name == "can");
  CHECK(std::string_view(desc.mesh_info[1].origin) == "prop");
  CHECK(desc.mesh_info[1].fit.height == 0.4f);
  REQUIRE(desc.instances.size() == 1);
  CHECK(desc.instances[0].mesh == 0);
  desc.cache = false;
  renderer::SceneData data;
  REQUIRE_MESSAGE(renderer::load_scene(desc, data, error), error);
  CHECK(data.parts.size() == 3);
  CHECK(data.instances.size() == 1);
  REQUIRE(data.props.size() == 2);
  CHECK(data.props[1].mesh == 2);
  CHECK_FALSE(data.dynamic);  // a scene with props is not dynamic by itself: --game makes it so
  // The reader's refusals: a second prop of one name, a prop with no mesh.
  REQUIRE(io::write_file(scene_path, R"({"meshes":[{"path":"box.glb"}],
      "props":[{"name":"can","mesh":"box.glb"},{"name":"can","mesh":"box.glb"}]})") ==
          io::Status::Ok);
  CHECK_FALSE(renderer::read_scene_file(scene_path, desc, error));
  CHECK(error.find("a second prop named \"can\"") != std::string::npos);
  REQUIRE(io::write_file(scene_path, R"({"meshes":[{"path":"box.glb"}],
      "props":[{"name":"can"}]})") == io::Status::Ok);
  CHECK_FALSE(renderer::read_scene_file(scene_path, desc, error));
  CHECK(error.find("no \"mesh\"") != std::string::npos);
}

// ---- the services through the runner and the fixture (ABI 3), no GPU
// -----------------------------
#if defined(WALK_POC_PATH) && defined(WALK_POC_NO_PROPS_PATH)

namespace {

bool in_bundle() {
  if (test::bundle_root().empty()) return false;
  MESSAGE("game props: game modules are not part of a test bundle; skipped");
  return true;
}

std::string place_module(const test::TempDir& dir, const char* built, std::string_view name) {
  const std::filesystem::path from(built);
  const std::string to = dir.file(std::string(name) + from.extension().string());
  std::error_code ec;
  std::filesystem::copy_file(from, std::filesystem::path(to),
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_MESSAGE(!ec, "copying " << built << ": " << ec.message());
  return to;
}

view::GameRunner::Config config_for(const std::string& module, const test::TempDir& tmp,
                                    const char* shadow = "shadow") {
  view::GameRunner::Config c;
  c.module = module;
  c.shadow_directory = tmp.file(shadow);
  return c;
}

f64 state_number(const view::GameRunner& runner, const char* field) {
  const JsonValue state = runner.state_json();
  const JsonValue* v = state.find(field);
  f64 out = -1.0;
  return v != nullptr && v->get_f64(out) ? out : -1.0;
}

// A bool field of the state: 1, 0, or -1 when it is not there or not a bool.
i32 state_flag(const view::GameRunner& runner, const char* field) {
  const JsonValue state = runner.state_json();
  const JsonValue* v = state.find(field);
  bool out = false;
  return v != nullptr && v->get_bool(out) ? (out ? 1 : 0) : -1;
}

// One tick with the walker's feet at `feet`, looking along -z, on the ground, `input` for the
// actions.
void tick_at(view::GameRunner& runner, u64& tick, WorldPos feet, f64 moved = 1.5 / 240.0,
             const input::InputState* input = nullptr) {
  view::GameTickInput in;
  in.tick = ++tick;
  in.feet = feet;
  in.eye = WorldPos{feet.x, feet.y + 1.65, feet.z};
  in.walking = true;
  in.on_ground = true;
  in.moved_m = moved;
  in.input = input;
  runner.tick(in);
}

// A press of key `code` on tick `at`, and its release, as `state` sees them.
void press(input::InputState& state, u64 at, u32 code) {
  state.begin_tick(SimTick{at});
  state.feed(input::RawEvent{SimTick{at}, input::Source::Key, code, 1.0f, 0});
  state.end_tick();
}
void release(input::InputState& state, u64 at, u32 code) {
  state.begin_tick(SimTick{at});
  state.feed(input::RawEvent{SimTick{at}, input::Source::Key, code, 0.0f, 0});
  state.end_tick();
}

constexpr u32 k_key_e = 8;   // interact
constexpr u32 k_key_q = 20;  // use

}  // namespace

TEST_CASE("game props: the fixture places the can at the spring, picks it up in reach, and marks") {
  if (in_bundle()) return;
  const test::TempDir tmp("game_props_fixture");
  const input::ActionMap map = view::default_fly_map();
  view::GameRunner runner;
  runner.props().declare(declared({"can", "marker"}));
  std::string error;
  REQUIRE_MESSAGE(
      runner.start(config_for(place_module(tmp, WALK_POC_PATH, "walk_poc"), tmp), map, 240, &error),
      error);
  REQUIRE(runner.props().attach(attached_scene({"can", "marker"}), &error));
  u64 tick = 0;
  // The first tick, standing at the origin looking along -z: the spring is 25 m along -z, and the
  // can is placed there at the feet's height.
  tick_at(runner, tick, WorldPos{0.0, 0.5, 0.0}, 0.0);
  CHECK(runner.props().live() == 1);
  CHECK(state_flag(runner, "can_placed") == 1);
  CHECK(state_number(runner, "can") >= static_cast<f64>(view::k_max_props));
  const Vector<renderer::SceneInstance>& tail = runner.props().tail();
  REQUIRE(tail.size() == 1);
  CHECK(tail[0].mesh == 10);
  CHECK(tail[0].origin.z == doctest::Approx(-25.0));
  CHECK(tail[0].origin.y == 0.5);
  // E far from it: "nothing here", the can stays.
  input::InputState state(map);
  press(state, 2, k_key_e);
  tick_at(runner, tick, WorldPos{0.0, 0.5, -1.0}, 0.0, &state);
  release(state, 3, k_key_e);
  tick_at(runner, tick, WorldPos{0.0, 0.5, -1.0}, 0.0, &state);
  CHECK(runner.props().live() == 1);
  CHECK(state_number(runner, "interactions") == 1.0);
  // Within two metres of it, thirsty: the HUD says so, and E picks it up and fills the meter.
  tick_at(runner, tick, WorldPos{0.0, 0.5, -23.5}, 22.5);
  CHECK(std::string_view(runner.hud().lines[1].text) == "a water can: E to pick it up");
  // (The can stands at the spring, so the walker is at the water here too; what the pick-up proves
  // is the removal and the full meter.)
  press(state, 5, k_key_e);
  tick_at(runner, tick, WorldPos{0.0, 0.5, -23.5}, 0.0, &state);
  release(state, 6, k_key_e);
  CHECK(runner.props().live() == 0);
  CHECK(state_number(runner, "can") == 0.0);
  CHECK(state_flag(runner, "can_taken") == 1);
  CHECK(state_number(runner, "thirst") == 100.0);
  CHECK(std::string_view(runner.hud().lines[3].text).find("can taken") == 0);
  // Q places a marker where the walker stands.
  press(state, 7, k_key_q);
  tick_at(runner, tick, WorldPos{3.0, 0.5, -20.0}, 0.0, &state);
  release(state, 8, k_key_q);
  CHECK(state_number(runner, "markers") == 1.0);
  REQUIRE(runner.props().live() == 1);
  CHECK(runner.props().tail()[0].mesh == 11);
  CHECK(runner.props().tail()[0].origin.x == 3.0);
  const JsonValue summary = runner.summary_json();
  f64 n = 0.0;
  REQUIRE(summary.find("props")->find("placed")->get_f64(n));
  CHECK(n == 2.0);
  REQUIRE(summary.find("props")->find("removed")->get_f64(n));
  CHECK(n == 1.0);
  REQUIRE(summary.find("props")->find("found")->get_f64(n));
  CHECK(n >= 2.0);

  // The walker's scale, from a run saved at a quarter of its water: a session with no walker takes
  // the call, and the summary says what was set.
  view::FlySession session;
  const std::string state_file = tmp.file("thirsty.json");
  REQUIRE(io::write_file(state_file,
                         R"({"format":"engine.game.state","version":1,"module":"walk_poc",
      "type":"engine.walk_poc.Survivor","state":{"thirst":25.0,"started":true,"can_placed":true}})") ==
          io::Status::Ok);
  view::GameRunner thirsty;
  thirsty.props().declare(declared({"can", "marker"}));
  view::GameRunner::Config c = config_for(place_module(tmp, WALK_POC_PATH, "walk_poc2"), tmp, "s2");
  c.state_file = state_file;
  REQUIRE_MESSAGE(thirsty.start(c, map, 240, &error), error);
  REQUIRE(thirsty.props().attach(attached_scene({"can", "marker"}), &error));
  thirsty.set_session(&session);
  u64 t2 = 0;
  tick_at(thirsty, t2, WorldPos{100.0, 0.0, 100.0});  // away from the saved spring
  const JsonValue s2 = thirsty.summary_json();
  f64 scale = 0.0;
  REQUIRE(s2.find("walker")->find("speed_scale")->get_f64(scale));
  CHECK(scale == doctest::Approx(0.75).epsilon(1e-3));  // 0.5 + 0.5 * 25 / 50, a tick walked
  REQUIRE(s2.find("walker")->find("sets")->get_f64(n));
  CHECK(n == 1.0);
}

TEST_CASE(
    "game props: a scene with no props runs the fixture, which places nothing; a scene with "
    "other props refuses it by name") {
  if (in_bundle()) return;
  const test::TempDir tmp("game_props_none");
  const input::ActionMap map = view::default_fly_map();
  // No props declared: prop_mesh answers Unsupported, the load goes through, nothing is placed.
  view::GameRunner none;
  std::string error;
  REQUIRE_MESSAGE(
      none.start(config_for(place_module(tmp, WALK_POC_PATH, "walk_poc"), tmp), map, 240, &error),
      error);
  REQUIRE(none.props().attach(renderer::SceneData{}, &error));
  u64 tick = 0;
  tick_at(none, tick, WorldPos{0.0, 0.0, 0.0});
  CHECK(none.props().live() == 0);
  CHECK(state_flag(none, "can_placed") == 0);
  // Props declared but not the fixture's: refused when it loads, naming the prop and the scene's.
  view::GameRunner other;
  other.props().declare(declared({"can", "cup"}));
  CHECK_FALSE(other.start(config_for(place_module(tmp, WALK_POC_PATH, "walk_poc_b"), tmp, "s2"),
                          map, 240, &error));
  CHECK(error.find("no prop 'marker' in the scene (it declares can cup)") != std::string::npos);
  CHECK_FALSE(other.started());
  // The no-props build on a scene with props: declared, never placed.
  view::GameRunner quiet;
  quiet.props().declare(declared({"can", "marker"}));
  REQUIRE_MESSAGE(
      quiet.start(config_for(place_module(tmp, WALK_POC_NO_PROPS_PATH, "walk_poc_c"), tmp, "s3"),
                  map, 240, &error),
      error);
  REQUIRE(quiet.props().attach(attached_scene({"can", "marker"}), &error));
  tick = 0;
  tick_at(quiet, tick, WorldPos{0.0, 0.0, 0.0});
  CHECK(quiet.props().live() == 0);
  CHECK(quiet.props().declared_count() == 2);
}

#endif  // WALK_POC_PATH && WALK_POC_NO_PROPS_PATH

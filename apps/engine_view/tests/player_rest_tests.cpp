// End to end: a worn body whose skin carries no animation clip stands at its bind pose instead of
// being refused (roadmap P61; docs/subsystems/apps.md "The player's body"). An authored export (a
// module assembly, a textured preset) arrives with a skin and morph targets and no `animations`
// array at all; `--player` used to stop at load with "the skin has no animation to play".
//
// Two bodies are worn: the skinned bar of mesh_view_tests.cpp (written by the test, so the case
// runs everywhere) and the Khronos RiggedFigure (tools/fetch-samples.ps1; skipped with a message
// where it has not been fetched). Each is copied with its `animations` array removed: the glb's
// JSON chunk is parsed with the engine's json module, the member erased and the chunk written back
// padded, the binary chunk copied as it was. Every run is offscreen (view_process.h) and small.

#include "../fly_camera.h"
#include "view_process.h"

#include <core/json/json.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstring>
#include <string>
#include <vector>

using namespace engine;

#if ENGINE_VIEW_GAME && ENGINE_VIEW_ANIMATION

namespace engine::view_test {
bool write_skinned_bar_glb(const std::string& path);  // mesh_view_tests.cpp
}  // namespace engine::view_test

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

const JsonValue* at(const JsonValue& v, std::initializer_list<const char*> path) {
  const JsonValue* p = &v;
  for (const char* key : path) {
    p = p != nullptr && p->is_object() ? p->find(key) : nullptr;
  }
  return p;
}

f64 number(const JsonValue* v) {
  f64 out = -1.0;
  return v != nullptr && v->get_f64(out) ? out : -1.0;
}

std::string text(const JsonValue* v) {
  return v != nullptr && v->is_string() ? std::string(v->as_string()) : std::string();
}

u32 read_u32(const std::string& bytes, usize at_byte) {
  u32 v = 0;
  std::memcpy(&v, bytes.data() + at_byte, sizeof(v));
  return v;
}

void put_u32(std::string& out, u32 v) { out.append(reinterpret_cast<const char*>(&v), sizeof(v)); }

// `from` copied to `to` with the glTF's `animations` member erased. A .glb is a 12-byte header, a
// JSON chunk and a binary chunk; the JSON chunk is rewritten (padded with spaces to four bytes)
// and the header's total length follows it. Returns false, with a message, if `from` is not one.
bool write_without_animations(const std::string& from, const std::string& to, std::string& why) {
  std::string bytes;
  if (io::read_file(from, bytes) != io::Status::Ok || bytes.size() < 28) {
    why = "cannot read " + from;
    return false;
  }
  const u32 json_length = read_u32(bytes, 12);
  if (read_u32(bytes, 0) != 0x46546c67u || read_u32(bytes, 16) != 0x4e4f534au ||
      20 + usize{json_length} > bytes.size()) {
    why = from + " is not a .glb with a JSON chunk first";
    return false;
  }
  JsonValue gltf;
  if (!parse_json(bytes.substr(20, json_length), gltf).ok || !gltf.is_object()) {
    why = "the JSON chunk of " + from + " does not parse";
    return false;
  }
  if (gltf.as_object().erase("animations") == 0) {
    why = from + " carries no animations to remove";
    return false;
  }
  std::string json = write_json(gltf);
  while (json.size() % 4 != 0)
    json += ' ';
  const std::string rest = bytes.substr(20 + json_length);  // the binary chunk, as it was
  std::string out;
  put_u32(out, 0x46546c67u);
  put_u32(out, 2u);
  put_u32(out, static_cast<u32>(12 + 8 + json.size() + rest.size()));
  put_u32(out, static_cast<u32>(json.size()));
  put_u32(out, 0x4e4f534au);
  out += json;
  out += rest;
  return io::write_file(to, out) == io::Status::Ok;
}

// The waves of player_view_tests.cpp, 20 cm over 64 m, with nothing on them.
std::string ground_scene(const test::TempDir& tmp) {
  const std::string scene = tmp.file("ground.json");
  REQUIRE(io::write_file(scene, R"({"format":"engine.scene.v1","name":"player",
      "terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30}})") ==
          io::Status::Ok);
  return scene;
}

// From (0, 3, 0), facing 0.6 rad; with `walk`, W held from tick 10 to tick 150, else nothing.
std::string session(const test::TempDir& tmp, const std::string& scene, bool walk,
                    const char* name) {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  if (walk) {
    keys.record(input::RawEvent{SimTick{10}, input::Source::Key, 26, 1.0f, 0});  // W
    keys.record(input::RawEvent{SimTick{150}, input::Source::Key, 26, 0.0f, 0});
  }
  view::SessionHeader header;
  header.ticks = 240;
  header.scene = scene;
  header.start.position = WorldPos{0.0, 3.0, 0.0};
  header.start.yaw = 0.6f;
  header.start.pitch = -0.1f;
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  const std::string path = tmp.file(name);
  REQUIRE(keys.save(path) == io::Status::Ok);
  return path;
}

std::vector<std::string> replay(const test::TempDir& tmp, const std::string& log,
                                const std::string& scene, const std::string& rig) {
  return {"--replay-input",
          log,
          "--scene",
          scene,
          "--offscreen",
          "--width",
          "320",
          "--height",
          "200",
          "--ddc",
          tmp.file("ddc"),
          "--anim-lod-scale",
          "100000",
          "--player",
          rig};
}

bool unavailable(const Run& run) {
  if (run.exit_code != 3) return false;
  MESSAGE("engine-view unavailable here: " << run.output.substr(0, 300));
  return true;
}

// The body's first animated instance in the summary: its clip, and the bytes of the pose it drew.
const JsonValue* animated_instance(const JsonValue& summary) {
  const JsonValue* list = at(summary, {"animation", "animated"});
  return list != nullptr && list->is_array() && list->size() > 0 ? &(*list)[0] : nullptr;
}

void wear_without_clip(const std::string& source, const char* label) {
  const test::TempDir tmp(std::string("engine_view_player_rest_") + label);
  const std::string clipless = tmp.file("clipless.glb");
  std::string why;
  REQUIRE_MESSAGE(write_without_animations(source, clipless, why), why);
  const std::string scene = ground_scene(tmp);
  const std::string walking = session(tmp, scene, true, "walk.jsonl");
  const std::string standing = session(tmp, scene, false, "stand.jsonl");
  const std::vector<std::string> at_rest = {
      "--tunable", "view.player.walk_clip=none,view.player.idle_clip=none"};
  const auto run_with = [&](const std::string& log, const std::string& rig,
                            std::vector<std::string> extra) {
    std::vector<std::string> args = replay(tmp, log, scene, rig);
    args.insert(args.end(), extra.begin(), extra.end());
    return run_view(args);
  };

  // (a) The body with no clip, with the defaults (`walk` and `idle`, which no rig is refused for
  // lacking): it loads, is placed on every tick of the walk, refuses nothing and plays no clip.
  const Run worn = run_with(walking, clipless, {});
  if (unavailable(worn)) return;
  REQUIRE_MESSAGE(worn.exit_code == 0, worn.output);
  JsonValue a;
  REQUIRE_MESSAGE(summary_of(worn, a), worn.output);
  const JsonValue* player = at(a, {"interactive", "player"});
  REQUIRE_MESSAGE(player != nullptr, worn.output.substr(0, 3000));
  CHECK(number(player->find("places")) == 240.0);
  CHECK(number(player->find("refused")) == 0.0);
  CHECK(number(player->find("walking_ticks")) > 100.0);
  CHECK(text(player->find("walk_clip")) == "none");
  CHECK(text(player->find("idle_clip")) == "none");
  CHECK(text(at(a, {"interactive", "player", "actor", "clip"})).empty());
  const JsonValue* body = animated_instance(a);
  REQUIRE_MESSAGE(body != nullptr, worn.output.substr(0, 3000));
  CHECK(text(body->find("clip")).empty());

  // The same body standing still, and the original (with its clip) told to stand at rest while it
  // walks, which is what a clip-less skin has always been able to do by being a base body:
  // the same walk, and the pose hash (the bytes of the skinning matrices drawn) the same.
  const Run stood = run_with(standing, clipless, {});
  REQUIRE_MESSAGE(stood.exit_code == 0, stood.output);
  const Run original = run_with(walking, source, at_rest);
  REQUIRE_MESSAGE(original.exit_code == 0, original.output);
  JsonValue b;
  JsonValue c;
  REQUIRE(summary_of(stood, b));
  REQUIRE(summary_of(original, c));
  const JsonValue* standing_body = animated_instance(b);
  const JsonValue* rest_body = animated_instance(c);
  REQUIRE(standing_body != nullptr);
  REQUIRE(rest_body != nullptr);
  CHECK(number(at(b, {"interactive", "player", "walking_ticks"})) == 0.0);
  CHECK(text(body->find("pose_hash")) == text(standing_body->find("pose_hash")));
  CHECK(text(body->find("pose_hash")) == text(rest_body->find("pose_hash")));
  // The trajectory and the placements are the original's: nothing about the walk changed.
  CHECK(text(at(a, {"interactive", "trajectory", "hash"})) ==
        text(at(c, {"interactive", "trajectory", "hash"})));
  CHECK(write_json(*at(a, {"interactive", "player", "actor", "position"})) ==
        write_json(*at(c, {"interactive", "player", "actor", "position"})));
  // And the whole animation block (clip, playhead and pose hashes together) is the original's at
  // rest, which is the strongest thing to say about it.
  CHECK(text(at(a, {"animation", "hash"})) == text(at(c, {"animation", "hash"})));

  // (b) A clip *named* that the skin lacks is still an error, before a device, naming what the
  // skin has. (`walk` is the default and not a choice, so it is `stride` here.)
  for (const char* name : {"view.player.walk_clip=stride", "view.player.idle_clip=stride"}) {
    const Run named = run_with(walking, clipless, {"--tunable", name});
    CHECK(named.exit_code == 1);
    CHECK_MESSAGE(named.output.find("no clip named 'stride'") != std::string::npos, named.output);
    CHECK_MESSAGE(named.output.find("the skin has no clips") != std::string::npos, named.output);
  }
}

}  // namespace

TEST_CASE(
    "engine-view: --player wears a skin with no animation clip at its bind pose, and a clip it "
    "is told to play that the skin lacks is refused") {
  const test::TempDir tmp("engine_view_player_rest_bar");
  const std::string bar = tmp.file("bar.glb");
  REQUIRE(view_test::write_skinned_bar_glb(bar));
  wear_without_clip(bar, "bar");
}

TEST_CASE("engine-view: --player wears RiggedFigure without its clip at its bind pose") {
  const std::string path =
      test::data_path(ENGINE_SOURCE_DIR "/content/samples/RiggedFigure/RiggedFigure.glb",
                      "content/samples/RiggedFigure/RiggedFigure.glb");
  if (!test::path_exists(path)) {
    MESSAGE("not fetched (tools/fetch-samples.ps1): " << path);
    return;
  }
  wear_without_clip(path, "figure");
}

#endif  // ENGINE_VIEW_GAME && ENGINE_VIEW_ANIMATION

// End to end: the player's body (player.h; docs/subsystems/apps.md "The player's body"; roadmap
// P52). `--player` with the Khronos RiggedFigure (tools/fetch-samples.ps1; the cases say so and
// pass when it has not been fetched) over the waves ground of game_actors_view_tests.cpp, every run
// offscreen (view_process.h), small: the body stands at the walker's feet turned to its heading, is
// seen looking down, is not seen looking ahead with its head collapsed and is seen without the
// collapse, replays to the same block, refuses a mesh with no skin before a device, and stays in a
// streamed world.

#include "../fly_camera.h"
#include "view_process.h"

#include <core/json/json.h>
#include <foundation/image/decode.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>
#include <vector>

using namespace engine;

#if ENGINE_VIEW_GAME && ENGINE_VIEW_ANIMATION

namespace engine::view_test {
bool write_box_glb(const std::string& path);  // game_props_tests.cpp
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

f64 element(const JsonValue* v, u32 i) {
  return v != nullptr && v->is_array() && i < v->size() ? number(&(*v)[i]) : -1e30;
}

// The sample the cases stand, or empty (and said) when it has not been fetched.
std::string rigged_figure() {
  const std::string path =
      test::data_path(ENGINE_SOURCE_DIR "/content/samples/RiggedFigure/RiggedFigure.glb",
                      "content/samples/RiggedFigure/RiggedFigure.glb");
  if (!test::path_exists(path)) {
    MESSAGE("not fetched (tools/fetch-samples.ps1): " << path);
    return {};
  }
  return path;
}

// The waves of game_actors_view_tests.cpp, 20 cm over 64 m, with nothing on them; `world` makes it
// a streamed world over a scatter of boxes (the streamed placements the body must not join).
std::string ground_scene(const test::TempDir& tmp, bool world = false) {
  const std::string scene = tmp.file(world ? "streamed.json" : "ground.json");
  std::string body = R"({"format":"engine.scene.v1","name":"player",
      "terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30})";
  if (world) {
    REQUIRE(view_test::write_box_glb(tmp.file("box.glb")));
    body += R"(,"meshes":[{"path":"box.glb","fit":{"height":0.5}}],
      "scatters":[{"mesh":0,"center":[0,0,0],"radius_min":3,"radius_max":28,"count":40,"seed":5}],
      "world":{"tile_size":16,"rings":[{"radius":1.5},{"radius":3}]})";
  }
  body += "}";
  REQUIRE(io::write_file(scene, body) == io::Status::Ok);
  return scene;
}

// Walking from the start at (0, 3, 0), facing `yaw` and looking at `pitch` radians; with `walk`,
// W held from tick 10 to tick 150 (1.5 m/s, 2.1 m) and released for the rest, else nothing pressed.
std::string session(const test::TempDir& tmp, const std::string& scene, u64 ticks, f32 yaw,
                    f32 pitch, bool walk, const char* name) {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  if (walk) {
    keys.record(input::RawEvent{SimTick{10}, input::Source::Key, 26, 1.0f, 0});  // W
    keys.record(input::RawEvent{SimTick{150}, input::Source::Key, 26, 0.0f, 0});
  }
  view::SessionHeader header;
  header.ticks = ticks;
  header.scene = scene;
  header.start.position = WorldPos{0.0, 3.0, 0.0};
  header.start.yaw = yaw;
  header.start.pitch = pitch;
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  const std::string path = tmp.file(name);
  REQUIRE(keys.save(path) == io::Status::Ok);
  return path;
}

std::vector<std::string> replay(const test::TempDir& tmp, const std::string& log,
                                const std::string& scene) {
  return {"--replay-input", log,   "--scene", scene,           "--offscreen",      "--width", "320",
          "--height",       "200", "--ddc",   tmp.file("ddc"), "--anim-lod-scale", "100000"};
}

struct Diff {
  u64 total = 0;
  u64 upper = 0;    // rows above the picture's middle
  u64 lower = 0;    // and below it
  u64 central = 0;  // the central third, both ways
};

Diff diff(const std::string& a_path, const std::string& b_path) {
  image::Image a;
  image::Image b;
  REQUIRE(image::read_image(a_path, a) == io::Status::Ok);
  REQUIRE(image::read_image(b_path, b) == io::Status::Ok);
  REQUIRE(a.width == b.width);
  REQUIRE(a.height == b.height);
  REQUIRE(a.channels == b.channels);
  const u32 c = a.channels;
  Diff out;
  for (u32 y = 0; y < a.height; ++y) {
    for (u32 x = 0; x < a.width; ++x) {
      const u32 i = (y * a.width + x) * c;  // a 320 x 200 capture
      bool same = true;
      for (u32 k = 0; k < c && k < 3; ++k)
        same = same && a.pixels[i + k] == b.pixels[i + k];
      if (same) continue;
      ++out.total;
      (y < a.height / 2 ? out.upper : out.lower) += 1;
      if (3 * y >= a.height && 3 * y < 2 * a.height && 3 * x >= a.width && 3 * x < 2 * a.width)
        ++out.central;
    }
  }
  return out;
}

bool unavailable(const Run& run) {
  if (run.exit_code != 3) return false;
  MESSAGE("engine-view unavailable here: " << run.output.substr(0, 300));
  return true;
}

}  // namespace

TEST_CASE(
    "engine-view: --player stands the body at the walker's feet turned to its heading, walks it "
    "and a replay ends on the same block") {
  const std::string rig = rigged_figure();
  if (rig.empty()) return;
  const test::TempDir tmp("engine_view_player");
  const std::string scene = ground_scene(tmp);
  const std::string log = session(tmp, scene, 240, 0.6f, -0.1f, true, "walk.jsonl");
  std::vector<std::string> args = replay(tmp, log, scene);
  args.insert(args.end(), {"--player", rig});
  const Run first = run_view(args);
  if (unavailable(first)) return;
  REQUIRE_MESSAGE(first.exit_code == 0, first.output);
  JsonValue a;
  REQUIRE_MESSAGE(summary_of(first, a), first.output);
  const JsonValue* player = at(a, {"interactive", "player"});
  REQUIRE_MESSAGE(player != nullptr, first.output.substr(0, 3000));
  MESSAGE("player: " << write_json(*player).substr(0, 1500));
  CHECK(text(player->find("mesh")) == rig);
  CHECK(number(player->find("height_m")) == doctest::Approx(1.8).epsilon(1e-6));
  CHECK(number(player->find("places")) == 240.0);  // walking from the first tick
  CHECK(number(player->find("refused")) == 0.0);
  CHECK(number(player->find("walking_ticks")) > 100.0);
  CHECK(player->find("collapse_head")->as_bool());
  const JsonValue* actor = player->find("actor");
  REQUIRE(actor != nullptr);
  CHECK(text(actor->find("name")) == "player");
  // Where the walker ended: its eye less the header's eye height (1.65 as a float).
  const JsonValue* last = at(a, {"interactive", "trajectory", "last"});
  REQUIRE(last != nullptr);
  const JsonValue* eye = last->find("position");
  const f64 eye_height = static_cast<f64>(1.65f);
  const JsonValue* position = actor->find("position");
  CHECK(std::fabs(element(position, 0) - element(eye, 0)) < 1e-3);
  CHECK(std::fabs(element(position, 1) - (element(eye, 1) - eye_height)) < 1e-3);
  CHECK(std::fabs(element(position, 2) - element(eye, 2)) < 1e-3);
  // Turned to the heading, a half turn past it (the rig faces +z, the heading 0 looks along -z).
  const f64 heading = number(last->find("yaw"));
  f64 turn = number(actor->find("yaw")) - heading - 3.14159265358979;
  turn = std::remainder(turn, 6.28318530717959);
  CHECK(std::fabs(turn) < 1e-5);
  // It stood, walked 140 ticks (0.875 m at 1.5 m/s) and stood: standing at tick 1 (the cut),
  // walking from tick 10, standing from 150, the clip faded to the rest pose by the end.
  CHECK(number(at(a, {"interactive", "walk", "distance_m"})) > 0.8);
  CHECK(number(player->find("gait_changes")) == 2.0);
  CHECK(text(actor->find("clip")).empty());

  // (d) A second replay: the same actor block, the same trajectory, the same animation hash.
  const Run second = run_view(args);
  REQUIRE_MESSAGE(second.exit_code == 0, second.output);
  JsonValue b;
  REQUIRE(summary_of(second, b));
  const JsonValue* again = at(b, {"interactive", "player", "actor"});
  REQUIRE(again != nullptr);
  CHECK(write_json(*again) == write_json(*actor));
  CHECK(text(at(b, {"interactive", "trajectory", "hash"})) ==
        text(at(a, {"interactive", "trajectory", "hash"})));
  CHECK(text(at(b, {"animation", "hash"})) == text(at(a, {"animation", "hash"})));
}

TEST_CASE(
    "engine-view: --player's body is seen looking down, its collapsed head is not seen looking "
    "ahead, and the head is without the collapse") {
  const std::string rig = rigged_figure();
  if (rig.empty()) return;
  const test::TempDir tmp("engine_view_player_see");
  const std::string scene = ground_scene(tmp);
  // One standing session, two markers: at tick 60 looking ahead (pitch 0), and at tick 160 looking
  // 80 degrees down (the pointer 634.66 pixels down at tick 100, at 0.0022 radians a pixel). Each
  // capture is drawn from its marker's own camera; three runs (no body, the body, the body with its
  // head) give every comparison.
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  const auto key = [&](u64 tick, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, 16, down ? 1.0f : 0.0f, 0});
  };
  key(60, true);  // M
  key(61, false);
  keys.record(input::RawEvent{SimTick{100}, input::Source::MouseAxis, 1, 634.66f, 0});
  key(160, true);
  key(161, false);
  view::SessionHeader header;
  header.ticks = 200;
  header.scene = scene;
  header.start.position = WorldPos{0.0, 3.0, 0.0};
  header.start.yaw = 0.6f;
  header.start.pitch = 0.0f;
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  const std::string log = tmp.file("see.jsonl");
  REQUIRE(keys.save(log) == io::Status::Ok);
  const auto capture = [&](const char* name, std::vector<std::string> extra) {
    std::vector<std::string> args = replay(tmp, log, scene);
    args.insert(args.end(), {"--marker-captures", tmp.file(name)});
    args.insert(args.end(), extra.begin(), extra.end());
    return run_view(args);
  };
  const auto png = [&](const char* name, const char* tick) {
    return tmp.file(std::string(name) + "/tick-" + tick + ".png");
  };
  const Run bare = capture("bare", {});
  if (unavailable(bare)) return;
  REQUIRE_MESSAGE(bare.exit_code == 0, bare.output);
  const Run collapsed = capture("body", {"--player", rig});
  REQUIRE_MESSAGE(collapsed.exit_code == 0, collapsed.output);
  const Run whole =
      capture("whole", {"--player", rig, "--tunable", "view.player.collapse_head=false"});
  REQUIRE_MESSAGE(whole.exit_code == 0, whole.output);
  JsonValue s;
  REQUIRE(summary_of(collapsed, s));
  MESSAGE("the rig: " << write_json(*at(s, {"interactive", "player"})).substr(0, 900));
  CHECK(std::fabs(number(at(s, {"interactive", "trajectory", "last", "pitch"})) + 1.3962634) <
        1e-4);

  // (b) Looking down, 80 degrees: the body against the ground alone.
  const Diff seen = diff(png("bare", "0000160"), png("body", "0000160"));
  MESSAGE("looking down, the body changes " << seen.total << " of 64,000 pixels: " << seen.lower
                                            << " in the lower half, " << seen.upper
                                            << " in the upper");
  CHECK(seen.total > 500);

  // (c) Looking ahead: the collapsed head changes nothing in the central third; without the
  // collapse the camera is inside the head, and it does.
  const Diff headless = diff(png("bare", "0000060"), png("body", "0000060"));
  const Diff headed = diff(png("bare", "0000060"), png("whole", "0000060"));
  MESSAGE("looking ahead, the collapsed head changes "
          << headless.central << " pixels of the central third (" << headless.total
          << " in all); the whole "
          << "head " << headed.central << " (" << headed.total << ")");
  CHECK(headless.central == 0);
  CHECK(headed.central > 0);
  JsonValue w;
  REQUIRE(summary_of(whole, w));
  CHECK_FALSE(at(w, {"interactive", "player", "collapse_head"})->as_bool());
  // The collapse is in the matrices the renderer is handed, not the pose: the hashes are the same.
  CHECK(text(at(w, {"animation", "hash"})) == text(at(s, {"animation", "hash"})));
}

TEST_CASE("engine-view: --player refuses a mesh with no skin by name before a device") {
  const test::TempDir tmp("engine_view_player_rigid");
  const std::string scene = ground_scene(tmp);
  REQUIRE(view_test::write_box_glb(tmp.file("box.glb")));
  const std::string log = session(tmp, scene, 8, 0.0f, 0.0f, false, "still.jsonl");
  std::vector<std::string> args = replay(tmp, log, scene);
  args.insert(args.end(), {"--player", tmp.file("box.glb")});
  const Run run = run_view(args);
  if (unavailable(run)) return;
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("box.glb has no skin") != std::string::npos);
  // Refused before the device: nothing a device or a load prints.
  CHECK(run.output.find("adapter") == std::string::npos);
  CHECK(run.output.find("scene_loaded") == std::string::npos);
}

TEST_CASE("engine-view: --player's body stays in a streamed world and walks with the walker") {
  const std::string rig = rigged_figure();
  if (rig.empty()) return;
  const test::TempDir tmp("engine_view_player_world");
  const std::string scene = ground_scene(tmp, true);
  const std::string log = session(tmp, scene, 240, 0.6f, -0.1f, true, "walk.jsonl");
  std::vector<std::string> args = replay(tmp, log, scene);
  args.insert(args.end(), {"--player", rig});
  const Run run = run_view(args);
  if (unavailable(run)) return;
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue a;
  REQUIRE(summary_of(run, a));
  const JsonValue* player = at(a, {"interactive", "player"});
  REQUIRE_MESSAGE(player != nullptr, run.output.substr(0, 3000));
  CHECK(number(player->find("places")) == 240.0);
  CHECK(number(player->find("refused")) == 0.0);
  const JsonValue* actor = player->find("actor");
  REQUIRE(actor != nullptr);
  const JsonValue* eye = at(a, {"interactive", "trajectory", "last", "position"});
  const JsonValue* position = actor->find("position");
  CHECK(std::fabs(element(position, 0) - element(eye, 0)) < 1e-3);
  CHECK(std::fabs(element(position, 2) - element(eye, 2)) < 1e-3);
  // The world streamed the scatter, and the body was not among what it streamed.
  MESSAGE("world: " << (a.find("world") != nullptr ? write_json(*a.find("world")).substr(0, 600)
                                                   : std::string("none")));
}

#endif  // ENGINE_VIEW_GAME && ENGINE_VIEW_ANIMATION

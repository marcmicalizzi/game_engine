// End to end: the orbit mode (roadmap P14 stage 2; apps.md "Third person"). `--player` with the
// Khronos RiggedFigure (tools/fetch-samples.ps1; the cases say so and pass when it has not been
// fetched) over the waves ground of player_view_tests.cpp, every run offscreen (view_process.h).
// One log — walk ahead, stop, turn the mouse a quarter turn, walk ahead again — replayed as a
// `follow` session (the body faces the look, as every session before P14) and as an `orbit` one
// (the mouse turns the camera round the body, W walks away from the camera, and the body turns to
// the way it walks at a bounded rate), and the follow session's trajectory held to the hash `main`
// flew it to before the orbit mode existed.
//
// The logs are written through `session_to_json` with the orbit block added as JSON, so this file
// reads nothing of the new interface and the pinned hash was taken by the same code at 85eda802.

#include "../fly_camera.h"
#include "view_process.h"

#include <core/json/json.h>
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

// The difference of two angles, wrapped into [-pi, pi].
f64 angle_between(f64 a, f64 b) { return std::remainder(a - b, 6.28318530717959); }

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

bool unavailable(const Run& run) {
  if (run.exit_code != 3) return false;
  MESSAGE("engine-view unavailable here: " << run.output.substr(0, 300));
  return true;
}

std::string ground_scene(const test::TempDir& tmp) {
  const std::string scene = tmp.file("ground.json");
  REQUIRE(io::write_file(scene, R"({"format":"engine.scene.v1","name":"orbit",
      "terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30}})") ==
          io::Status::Ok);
  return scene;
}

// A quarter turn of the pointer at the map's 0.0022 radians a pixel.
constexpr f32 k_quarter_px = 714.0f;

// Walking from (0, 3, 0), facing 0.6 rad: W held from tick 10 to 100, the pointer a quarter turn
// across at tick 120, M at tick 130 (standing), W held again from tick 140 to 300 with M at 299,
// 320 ticks. `orbit` adds the header's orbit block at 180 degrees a second.
std::string orbit_log(const test::TempDir& tmp, const std::string& scene, const char* name,
                      bool orbit) {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  const auto key = [&](u64 tick, u32 code, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0});
  };
  key(10, 26, true);  // W
  key(100, 26, false);
  keys.record(input::RawEvent{SimTick{120}, input::Source::MouseAxis, 0, k_quarter_px, 0});
  key(130, 16, true);  // M
  key(131, 16, false);
  key(140, 26, true);
  key(299, 16, true);
  key(300, 26, false);
  key(300, 16, false);
  view::SessionHeader header;
  header.ticks = 320;
  header.scene = scene;
  header.start.position = WorldPos{0.0, 3.0, 0.0};
  header.start.yaw = 0.6f;
  header.start.pitch = 0.0f;
  header.has_walk = true;
  header.walking = true;
  JsonValue session = view::session_to_json(header);
  if (orbit) {
    JsonValue block = JsonValue::object();
    block.set("turn_rate_deg_s", JsonValue(180.0f));
    session.set("orbit", std::move(block));
  }
  keys.set_session(std::move(session));
  const std::string path = tmp.file(name);
  REQUIRE(keys.save(path) == io::Status::Ok);
  return path;
}

}  // namespace

TEST_CASE(
    "engine-view: the orbit mode turns the camera round the body, walks away from the camera and "
    "turns the body at a bounded rate; follow is the trajectory main flew") {
  const std::string rig = rigged_figure();
  if (rig.empty()) return;
  const test::TempDir tmp("engine_view_player_orbit");
  const std::string scene = ground_scene(tmp);
  const std::string follow_log = orbit_log(tmp, scene, "follow.jsonl", false);
  const std::string orbit_log_path = orbit_log(tmp, scene, "orbit.jsonl", true);
  const auto run_with = [&](const std::string& log, std::vector<std::string> extra) {
    std::vector<std::string> args = {"--replay-input",
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
    args.insert(args.end(), extra.begin(), extra.end());
    return run_view(args);
  };
  const Run follow = run_with(follow_log, {});
  if (unavailable(follow)) return;
  REQUIRE_MESSAGE(follow.exit_code == 0, follow.output);
  JsonValue f;
  REQUIRE_MESSAGE(summary_of(follow, f), follow.output);
  const std::string follow_hash = text(at(f, {"interactive", "trajectory", "hash"}));
  MESSAGE("follow: trajectory " << follow_hash << ", animation "
                                << text(at(f, {"animation", "hash"})));

  // (b) The default is untouched: the follow session flies the trajectory main (85eda802) flew
  // from this log, to the bit, and a replay reads its mode from the header, never the tunable.
  CHECK(follow_hash == "10ea7b120c6a8e1c");
  const Run tunable = run_with(follow_log, {"--tunable", "view.camera.mode=orbit"});
  REQUIRE_MESSAGE(tunable.exit_code == 0, tunable.output);
  JsonValue t;
  REQUIRE(summary_of(tunable, t));
  CHECK(text(at(t, {"interactive", "trajectory", "hash"})) == follow_hash);

  // (a) Follow: the body's yaw is the look's at the end (a half turn past it: the rig faces +z).
  const JsonValue* f_last = at(f, {"interactive", "trajectory", "last"});
  const JsonValue* f_actor = at(f, {"interactive", "player", "actor"});
  REQUIRE(f_last != nullptr);
  REQUIRE(f_actor != nullptr);
  CHECK(std::fabs(angle_between(number(f_actor->find("yaw")) - 3.14159265358979,
                                number(f_last->find("yaw")))) < 1e-5);

  // Orbit.
  const Run orbit = run_with(orbit_log_path, {});
  REQUIRE_MESSAGE(orbit.exit_code == 0, orbit.output);
  JsonValue o;
  REQUIRE_MESSAGE(summary_of(orbit, o), orbit.output);
  CHECK(text(at(o, {"interactive", "camera_mode"})) == "orbit");
  CHECK(text(at(f, {"interactive", "camera_mode"})) == "follow");
  const JsonValue* markers = at(o, {"interactive", "trajectory", "markers"});
  REQUIRE(markers != nullptr);
  REQUIRE(markers->size() == 2);
  const JsonValue& standing = (*markers)[0];  // tick 130: the quarter turn made, standing
  const JsonValue& walked = (*markers)[1];    // tick 299: at the end of the second leg
  const f64 turned_look = number(standing.find("look_yaw"));
  MESSAGE("orbit at tick 130: heading " << number(standing.find("yaw")) << ", camera "
                                        << turned_look);
  // The mouse turned the camera a quarter turn, and the standing body not at all.
  CHECK(std::fabs(std::fabs(angle_between(turned_look, 0.6)) - 1.5708) < 0.01);
  CHECK(std::fabs(angle_between(number(standing.find("yaw")), 0.6)) < 1e-6);
  // The second leg heads the way the camera looks: W is away from the camera.
  const JsonValue* p0 = standing.find("position");
  const JsonValue* p1 = walked.find("position");
  const f64 dx = element(p1, 0) - element(p0, 0);
  const f64 dz = element(p1, 2) - element(p0, 2);
  const f64 leg = std::sqrt(dx * dx + dz * dz);
  MESSAGE("the second leg: " << leg << " m along (" << dx / leg << ", " << dz / leg
                             << "); the camera's forward (" << -std::sin(turned_look) << ", "
                             << -std::cos(turned_look) << ")");
  CHECK(leg > 0.8);
  CHECK(std::fabs(dx / leg + std::sin(turned_look)) < 1e-3);
  CHECK(std::fabs(dz / leg + std::cos(turned_look)) < 1e-3);
  // After it the body faces the camera's yaw: the heading is the look's, and so is the actor's.
  CHECK(std::fabs(angle_between(number(walked.find("yaw")), turned_look)) < 1e-5);
  const JsonValue* o_actor = at(o, {"interactive", "player", "actor"});
  REQUIRE(o_actor != nullptr);
  CHECK(std::fabs(angle_between(number(o_actor->find("yaw")) - 3.14159265358979, turned_look)) <
        1e-5);
  // The turn was bounded: a quarter turn at 180 degrees a second is half a second, 120 ticks at
  // 240 Hz held short of the heading, never one tick's snap.
  const f64 limited = number(at(o, {"interactive", "turn_limited_ticks"}));
  MESSAGE("the body turned over " << limited << " ticks");
  CHECK(number(at(o, {"interactive", "turn_rate_deg_s"})) == doctest::Approx(180.0));
  CHECK(limited >= 110.0);
  CHECK(limited <= 125.0);
  CHECK(text(at(o, {"interactive", "trajectory", "hash"})) != follow_hash);

  // (c) A second replay of the orbit log, from its header alone: the same trajectory, the same
  // body and the same pose.
  const Run again = run_with(orbit_log_path, {"--tunable", "view.camera.mode=follow"});
  REQUIRE_MESSAGE(again.exit_code == 0, again.output);
  JsonValue r;
  REQUIRE(summary_of(again, r));
  CHECK(text(at(r, {"interactive", "trajectory", "hash"})) ==
        text(at(o, {"interactive", "trajectory", "hash"})));
  CHECK(write_json(*at(r, {"interactive", "player", "actor"})) == write_json(*o_actor));
  CHECK(text(at(r, {"animation", "hash"})) == text(at(o, {"animation", "hash"})));
}

#endif  // ENGINE_VIEW_GAME && ENGINE_VIEW_ANIMATION

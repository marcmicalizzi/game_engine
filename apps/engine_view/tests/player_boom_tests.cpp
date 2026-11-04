// End to end: the third-person boom pulls in against the scene (roadmap P14 stage 1; apps.md
// "Third person"). `--player` with the Khronos RiggedFigure (tools/fetch-samples.ps1; the case says
// so and passes when it has not been fetched) over the waves ground of player_view_tests.cpp with a
// box standing 1 m behind the walker, every run offscreen (view_process.h): under
// `view.camera.collision=pull` (the default) the camera stands against the box's face, under `off`
// it stands 2.5 m back inside the box, the walk and the pose are the same either way, and a replay
// draws the same camera.

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

// The face of the wall nearest the walker, metres along +z: the box stands from here to z = 4,
// 6 m wide and 4 m tall, so a camera 2.5 m back from an eye at z = 0 is inside it.
constexpr f64 k_wall_z = 1.0;

// The waves of player_view_tests.cpp with a unit box scaled into a wall behind the start.
std::string wall_scene(const test::TempDir& tmp) {
  REQUIRE(view_test::write_box_glb(tmp.file("box.glb")));
  const std::string scene = tmp.file("wall.json");
  const std::string body = R"({"format":"engine.scene.v1","name":"boom",
      "terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30},
      "meshes":[{"path":"box.glb"}],
      "instances":[{"mesh":0,"translation":[0,1.5,2.5],"scale":[6,4,3]}]})";
  REQUIRE(io::write_file(scene, body) == io::Status::Ok);
  return scene;
}

// Standing at (0, 3, 0) looking along -z (yaw 0), level; V at tick 20 for third person, 60 ticks.
std::string standing_log(const test::TempDir& tmp, const std::string& scene) {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  keys.record(input::RawEvent{SimTick{20}, input::Source::Key, 25, 1.0f, 0});  // V
  keys.record(input::RawEvent{SimTick{21}, input::Source::Key, 25, 0.0f, 0});
  view::SessionHeader header;
  header.ticks = 60;
  header.scene = scene;
  header.start.position = WorldPos{0.0, 3.0, 0.0};
  header.start.yaw = 0.0f;
  header.start.pitch = 0.0f;
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  const std::string path = tmp.file("boom.jsonl");
  REQUIRE(keys.save(path) == io::Status::Ok);
  return path;
}

}  // namespace

TEST_CASE(
    "engine-view: the third-person boom pulls in to a wall behind the walker, off leaves it "
    "inside, and the walk and a replay are the same") {
  const std::string rig = rigged_figure();
  if (rig.empty()) return;
  const test::TempDir tmp("engine_view_player_boom");
  const std::string scene = wall_scene(tmp);
  const std::string log = standing_log(tmp, scene);
  const auto run_with = [&](std::vector<std::string> extra) {
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
  const Run pull = run_with({});
  if (unavailable(pull)) return;
  REQUIRE_MESSAGE(pull.exit_code == 0, pull.output);
  const Run off = run_with({"--tunable", "view.camera.collision=off"});
  REQUIRE_MESSAGE(off.exit_code == 0, off.output);
  JsonValue a;
  JsonValue b;
  REQUIRE_MESSAGE(summary_of(pull, a), pull.output);
  REQUIRE_MESSAGE(summary_of(off, b), off.output);
  const JsonValue* boom = at(a, {"interactive", "player", "boom"});
  const JsonValue* still = at(b, {"interactive", "player", "boom"});
  REQUIRE_MESSAGE(boom != nullptr, pull.output.substr(0, 3000));
  REQUIRE(still != nullptr);
  MESSAGE("pull: " << write_json(*boom));
  MESSAGE("off: " << write_json(*still));
  CHECK(text(at(a, {"interactive", "player", "camera"})) == "third");
  CHECK(text(boom->find("collision")) == "pull");
  CHECK(text(still->find("collision")) == "off");
  // The physical walker, whose world the cast reads.
  CHECK(text(boom->find("cast")) == "physics");
  CHECK(number(boom->find("radius_m")) == doctest::Approx(0.25));
  CHECK(number(boom->find("casts")) > 0.0);
  CHECK(number(boom->find("pulled")) > 0.0);
  CHECK(number(still->find("pulled")) == 0.0);

  const JsonValue* eye = at(a, {"interactive", "trajectory", "last", "position"});
  REQUIRE(eye != nullptr);
  const JsonValue* camera = boom->find("last");
  const JsonValue* inside = still->find("last");
  REQUIRE(camera != nullptr);
  REQUIRE(inside != nullptr);
  // (a) Pulled in: the camera stands in front of the wall's face, within the sphere's radius of
  // it, straight behind the eye and 0.4 m above it.
  const f64 gap = k_wall_z - element(camera, 2);
  MESSAGE("the eye at z " << element(eye, 2) << "; the camera at z " << element(camera, 2) << ", "
                          << gap << " m in front of the wall, the boom "
                          << number(boom->find("last_m")) << " m; off: z " << element(inside, 2));
  CHECK(gap > 0.0);
  CHECK(gap <= 0.25 + 1e-3);
  CHECK(std::fabs(element(camera, 0) - element(eye, 0)) < 1e-3);
  CHECK(element(camera, 1) - element(eye, 1) == doctest::Approx(0.4).epsilon(1e-4));
  CHECK(number(boom->find("last_m")) < 1.0);
  CHECK(number(boom->find("last_m")) >= 0.3);
  // Off: P53's boom, 2.5 m back inside the box.
  CHECK(element(inside, 2) - element(eye, 2) == doctest::Approx(2.5).epsilon(1e-4));
  CHECK(element(inside, 2) > k_wall_z);
  MESSAGE("the cast: " << number(boom->find("cast_us_mean")) << " us a frame on average, "
                       << number(boom->find("cast_us_max")) << " us at most, over "
                       << number(boom->find("casts")) << " casts");

  // (b) Drawn, not simulated: the same trajectory and the same pose with the cast and without.
  const std::string hash = text(at(a, {"interactive", "trajectory", "hash"}));
  CHECK(!hash.empty());
  CHECK(text(at(b, {"interactive", "trajectory", "hash"})) == hash);
  CHECK(text(at(b, {"animation", "hash"})) == text(at(a, {"animation", "hash"})));
  CHECK(write_json(*at(b, {"interactive", "player", "actor"})) ==
        write_json(*at(a, {"interactive", "player", "actor"})));

  // (c) A second replay draws the camera at the same place, to the millimetre.
  const Run again = run_with({});
  REQUIRE_MESSAGE(again.exit_code == 0, again.output);
  JsonValue c;
  REQUIRE(summary_of(again, c));
  const JsonValue* replayed = at(c, {"interactive", "player", "boom", "last"});
  REQUIRE(replayed != nullptr);
  for (u32 i = 0; i < 3; ++i)
    CHECK(std::fabs(element(replayed, i) - element(camera, i)) < 1e-3);
  CHECK(text(at(c, {"interactive", "trajectory", "hash"})) == hash);
}

#endif  // ENGINE_VIEW_GAME && ENGINE_VIEW_ANIMATION

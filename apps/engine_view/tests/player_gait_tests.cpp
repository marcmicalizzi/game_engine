// End to end: the player's body wears the procedural gait (roadmap C60; docs/subsystems/apps.md
// "The procedural gait"; systems/animation/gait.h). `--player` with the Khronos RiggedFigure
// (tools/fetch-samples.ps1; the case says so and passes when it has not been fetched) under
// `view.player.walk_clip=gait`: the walk is generated and named, it poses the body differently
// from the rest pose and from standing, the playhead stands still while the walker does, a sprint
// plays the run, a replay reproduces the animation hash, and a session without the tunable is the
// session it was. Every run is offscreen (view_process.h) and small.

#include "../fly_camera.h"
#include "view_process.h"

#include <core/json/json.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

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

bool ends_with(const std::string& s, const char* tail) {
  const std::string t(tail);
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

// The waves of player_view_tests.cpp, 20 cm over 64 m, with nothing on them.
std::string ground_scene(const test::TempDir& tmp) {
  const std::string scene = tmp.file("ground.json");
  REQUIRE(io::write_file(scene, R"({"format":"engine.scene.v1","name":"player",
      "terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30}})") ==
          io::Status::Ok);
  return scene;
}

constexpr u32 k_key_w = 26;
constexpr u32 k_key_lshift = 225;  // the `fast` action (fly_camera.cpp)

// From (0, 3, 0), facing 0.6 rad, `ticks` long; with `walk`, W held from tick 10 to `release` (0:
// to the end, so the run ends mid-stride and its pose is the gait's), and with `sprint` the left
// shift beside it.
std::string session(const test::TempDir& tmp, const std::string& scene, bool walk, bool sprint,
                    u64 ticks, u64 release, const char* name) {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  if (walk) {
    keys.record(input::RawEvent{SimTick{10}, input::Source::Key, k_key_w, 1.0f, 0});
    if (sprint) keys.record(input::RawEvent{SimTick{10}, input::Source::Key, k_key_lshift, 1.0f, 0});
    if (release != 0) {
      keys.record(input::RawEvent{SimTick{release}, input::Source::Key, k_key_w, 0.0f, 0});
      if (sprint)
        keys.record(input::RawEvent{SimTick{release}, input::Source::Key, k_key_lshift, 0.0f, 0});
    }
  }
  view::SessionHeader header;
  header.ticks = ticks;
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

bool unavailable(const Run& run) {
  if (run.exit_code != 3) return false;
  MESSAGE("engine-view unavailable here: " << run.output.substr(0, 300));
  return true;
}

const JsonValue* animated_instance(const JsonValue& summary) {
  const JsonValue* list = at(summary, {"animation", "animated"});
  return list != nullptr && list->is_array() && list->size() > 0 ? &(*list)[0] : nullptr;
}

}  // namespace

TEST_CASE("engine-view: --player walks and runs RiggedFigure on the procedural gait") {
  const std::string rig =
      test::data_path(ENGINE_SOURCE_DIR "/content/samples/RiggedFigure/RiggedFigure.glb",
                      "content/samples/RiggedFigure/RiggedFigure.glb");
  if (!test::path_exists(rig)) {
    MESSAGE("not fetched (tools/fetch-samples.ps1): " << rig);
    return;
  }
  const test::TempDir tmp("engine_view_player_gait");
  const std::string scene = ground_scene(tmp);
  const std::string walking = session(tmp, scene, true, false, 240, 0, "walk.jsonl");
  const std::string stopping = session(tmp, scene, true, false, 240, 150, "stop.jsonl");
  const std::string stopping_long = session(tmp, scene, true, false, 400, 150, "stop-long.jsonl");
  const std::string standing = session(tmp, scene, false, false, 240, 0, "stand.jsonl");
  const std::string sprinting = session(tmp, scene, true, true, 240, 0, "sprint.jsonl");
  const auto run_with = [&](const std::string& log, std::vector<std::string> extra) {
    std::vector<std::string> args = {"--replay-input", log,       "--scene",  scene,
                                     "--offscreen",    "--width", "320",      "--height",
                                     "200",            "--ddc",   tmp.file("ddc"),
                                     "--anim-lod-scale", "100000", "--player", rig};
    args.insert(args.end(), extra.begin(), extra.end());
    return run_view(args);
  };
  const std::vector<std::string> gait = {"--tunable", "view.player.walk_clip=gait"};

  // The walk: generated, named, timed for the walker's speed, and played while it moves.
  const Run walked = run_with(walking, gait);
  if (unavailable(walked)) return;
  REQUIRE_MESSAGE(walked.exit_code == 0, walked.output);
  JsonValue w;
  REQUIRE_MESSAGE(summary_of(walked, w), walked.output);
  const JsonValue* player = at(w, {"interactive", "player"});
  REQUIRE_MESSAGE(player != nullptr, walked.output.substr(0, 3000));
  CHECK(ends_with(text(player->find("walk_clip")), "gait-walk"));
  CHECK(ends_with(text(player->find("run_clip")), "gait-run"));
  CHECK(number(player->find("walking_ticks")) > 100.0);
  CHECK(number(player->find("running_ticks")) == 0.0);
  CHECK(number(player->find("refused")) == 0.0);
  const f64 stride = number(at(w, {"interactive", "player", "gait", "walk", "stride_m"}));
  const f64 leg = number(at(w, {"interactive", "player", "gait", "leg_length_m"}));
  const f64 cycles = number(at(w, {"interactive", "player", "gait", "cycles"}));
  MESSAGE("RiggedFigure on the gait: leg " << leg << " m, walk stride " << stride << " m, "
                                           << cycles << " cycles walked");
  CHECK(leg > 0.5);
  CHECK(leg < 1.2);
  CHECK(stride > 1.0);
  CHECK(stride < 2.0);
  // The phase is the distance walked over the stride.
  const f64 distance = number(at(w, {"interactive", "walk", "distance_m"}));
  if (distance > 0.0) CHECK(cycles == doctest::Approx(distance / stride).epsilon(0.02));
  CHECK(cycles > 0.8);  // 230 ticks at 240 Hz and 1.5 m/s: 1.4 m
  const JsonValue* body = animated_instance(w);
  REQUIRE(body != nullptr);

  // At rest while walking, and standing on the gait: both pose the body differently.
  const Run rest = run_with(walking, {"--tunable",
                                      "view.player.walk_clip=none,view.player.idle_clip=none"});
  const Run stood = run_with(standing, gait);
  REQUIRE_MESSAGE(rest.exit_code == 0, rest.output);
  REQUIRE_MESSAGE(stood.exit_code == 0, stood.output);
  JsonValue r;
  JsonValue s;
  REQUIRE(summary_of(rest, r));
  REQUIRE(summary_of(stood, s));
  REQUIRE(animated_instance(r) != nullptr);
  REQUIRE(animated_instance(s) != nullptr);
  const std::string walk_pose = text(body->find("pose_hash"));
  CHECK(!walk_pose.empty());
  CHECK(walk_pose != text(animated_instance(r)->find("pose_hash")));
  CHECK(walk_pose != text(animated_instance(s)->find("pose_hash")));
  // The walker's own walk is the same with or without the gait: the trajectory is untouched.
  CHECK(text(at(w, {"interactive", "trajectory", "hash"})) ==
        text(at(r, {"interactive", "trajectory", "hash"})));

  // Standing, the playhead does not advance: no cycle walked, and a walk that stands 250 ticks
  // after it stops ends on the same phase and the same playhead as one that stands 90.
  CHECK(number(at(s, {"interactive", "player", "gait", "cycles"})) == 0.0);
  CHECK(number(at(s, {"interactive", "player", "walking_ticks"})) == 0.0);
  const Run stopped = run_with(stopping, gait);
  const Run longer = run_with(stopping_long, gait);
  REQUIRE_MESSAGE(stopped.exit_code == 0, stopped.output);
  REQUIRE_MESSAGE(longer.exit_code == 0, longer.output);
  JsonValue o;
  JsonValue l;
  REQUIRE(summary_of(stopped, o));
  REQUIRE(summary_of(longer, l));
  CHECK(number(at(o, {"interactive", "player", "gait", "phase"})) > 0.0);
  CHECK(number(at(l, {"interactive", "player", "gait", "phase"})) ==
        number(at(o, {"interactive", "player", "gait", "phase"})));
  CHECK(number(at(l, {"interactive", "player", "actor", "playhead"})) ==
        number(at(o, {"interactive", "player", "actor", "playhead"})));

  // A sprint plays the run.
  const Run ran = run_with(sprinting, gait);
  REQUIRE_MESSAGE(ran.exit_code == 0, ran.output);
  JsonValue n;
  REQUIRE(summary_of(ran, n));
  CHECK(number(at(n, {"interactive", "player", "running_ticks"})) > 100.0);
  CHECK(ends_with(text(at(n, {"interactive", "player", "run_clip"})), "gait-run"));
  CHECK(number(at(n, {"interactive", "player", "gait", "run", "stride_m"})) > stride);
  CHECK(text(at(n, {"animation", "hash"})) != text(at(w, {"animation", "hash"})));

  // A replay reproduces the animation: the same hash from a second run of the same recording.
  const Run again = run_with(walking, gait);
  REQUIRE_MESSAGE(again.exit_code == 0, again.output);
  JsonValue a;
  REQUIRE(summary_of(again, a));
  CHECK(text(at(a, {"animation", "hash"})) == text(at(w, {"animation", "hash"})));
  CHECK(!text(at(w, {"animation", "hash"})).empty());

  // Without the tunable the session is the one it was: RiggedFigure's own clip, no gait block, and
  // the hashes those of the default spelled out.
  const Run plain = run_with(walking, {});
  const Run spelled = run_with(walking, {"--tunable", "view.player.walk_clip=walk"});
  REQUIRE_MESSAGE(plain.exit_code == 0, plain.output);
  REQUIRE_MESSAGE(spelled.exit_code == 0, spelled.output);
  JsonValue p;
  JsonValue q;
  REQUIRE(summary_of(plain, p));
  REQUIRE(summary_of(spelled, q));
  CHECK(text(at(p, {"interactive", "player", "walk_clip"})) == "RiggedFigure/animation0");
  CHECK(at(p, {"interactive", "player", "gait"}) == nullptr);
  CHECK(at(p, {"interactive", "player", "run_clip"}) == nullptr);
  CHECK(text(at(p, {"animation", "hash"})) == text(at(q, {"animation", "hash"})));
  CHECK(text(at(p, {"animation", "hash"})) != text(at(w, {"animation", "hash"})));
}

TEST_CASE("engine-view: view.player.walk_clip=gait refuses a rig with no legs before a device") {
  // The skinned bar of mesh_view_tests.cpp maps no standard-skeleton leg: refused at load, naming
  // the roles it lacks.
  const test::TempDir tmp("engine_view_player_gait_bar");
  const std::string bar = tmp.file("bar.glb");
  REQUIRE(view_test::write_skinned_bar_glb(bar));
  const std::string scene = ground_scene(tmp);
  const std::string log = session(tmp, scene, true, false, 60, 0, "walk.jsonl");
  const Run run = run_view({"--replay-input", log, "--scene", scene, "--offscreen", "--width", "320",
                            "--height", "200", "--ddc", tmp.file("ddc"), "--player", bar,
                            "--tunable", "view.player.walk_clip=gait"});
  CHECK(run.exit_code == 1);
  CHECK_MESSAGE(run.output.find("view.player.walk_clip=gait needs the hips and both legs") !=
                    std::string::npos,
                run.output);
  CHECK_MESSAGE(run.output.find("this rig maps no ") != std::string::npos, run.output);
  CHECK_MESSAGE(run.output.find("LeftUpperLeg") != std::string::npos, run.output);
}

#endif  // ENGINE_VIEW_GAME && ENGINE_VIEW_ANIMATION

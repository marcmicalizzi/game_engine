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
  // The face's eye is the default (P58), and under it the head is drawn whole.
  CHECK(text(player->find("eye")) == "face");
  CHECK_FALSE(player->find("collapse_head")->as_bool());
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

  // (c, P58) The walker's eye with its collapse: the same actor block, the same trajectory and the
  // same animation hash as the face's: the eye and the look are the drawn camera and the matrices
  // the renderer is handed, never the pose or the walk.
  std::vector<std::string> walker_args = args;
  walker_args.insert(walker_args.end(), {"--tunable", "view.player.eye=walker"});
  const Run third = run_view(walker_args);
  REQUIRE_MESSAGE(third.exit_code == 0, third.output);
  JsonValue c;
  REQUIRE(summary_of(third, c));
  CHECK(text(at(c, {"interactive", "player", "eye"})) == "walker");
  CHECK(at(c, {"interactive", "player", "collapse_head"})->as_bool());
  const JsonValue* walker_actor = at(c, {"interactive", "player", "actor"});
  REQUIRE(walker_actor != nullptr);
  CHECK(write_json(*walker_actor) == write_json(*actor));
  CHECK(text(at(c, {"interactive", "trajectory", "hash"})) ==
        text(at(a, {"interactive", "trajectory", "hash"})));
  CHECK(text(at(c, {"animation", "hash"})) == text(at(a, {"animation", "hash"})));
}

TEST_CASE(
    "engine-view: view.player.walk_clip=none keeps the body at rest while it walks, and a name the "
    "rig lacks plays its first clip") {
  const std::string rig = rigged_figure();
  if (rig.empty()) return;
  const test::TempDir tmp("engine_view_player_clip");
  const std::string scene = ground_scene(tmp);
  const std::string walking = session(tmp, scene, 240, 0.6f, -0.1f, true, "walk.jsonl");
  const std::string standing = session(tmp, scene, 240, 0.6f, -0.1f, false, "stand.jsonl");
  const auto run_with = [&](const std::string& log, std::vector<std::string> extra) {
    std::vector<std::string> args = replay(tmp, log, scene);
    args.insert(args.end(), {"--player", rig});
    args.insert(args.end(), extra.begin(), extra.end());
    return run_view(args);
  };
  const Run played = run_with(walking, {});
  if (unavailable(played)) return;
  REQUIRE_MESSAGE(played.exit_code == 0, played.output);
  const Run rest = run_with(walking, {"--tunable", "view.player.walk_clip=none"});
  REQUIRE_MESSAGE(rest.exit_code == 0, rest.output);
  const Run stood = run_with(standing, {});
  REQUIRE_MESSAGE(stood.exit_code == 0, stood.output);
  const Run lacking = run_with(walking, {"--tunable", "view.player.walk_clip=Sprint"});
  REQUIRE_MESSAGE(lacking.exit_code == 0, lacking.output);
  JsonValue a;
  JsonValue b;
  JsonValue c;
  JsonValue d;
  REQUIRE(summary_of(played, a));
  REQUIRE(summary_of(rest, b));
  REQUIRE(summary_of(stood, c));
  REQUIRE(summary_of(lacking, d));
  const JsonValue* with_clip = at(a, {"interactive", "player"});
  const JsonValue* no_clip = at(b, {"interactive", "player"});
  REQUIRE(with_clip != nullptr);
  REQUIRE(no_clip != nullptr);
  // The summary names the clip played; `none` where none plays (RiggedFigure has no idle).
  CHECK(text(with_clip->find("walk_clip")) == "RiggedFigure/animation0");
  CHECK(text(no_clip->find("walk_clip")) == "none");
  CHECK(text(with_clip->find("idle_clip")) == "none");
  // A name the rig lacks is today's behaviour: the first clip.
  CHECK(text(at(d, {"interactive", "player", "walk_clip"})) == "RiggedFigure/animation0");
  CHECK(write_json(*at(d, {"interactive", "player"})) == write_json(*with_clip));
  // The walk is the same walk: the same placements, the same distance, the same ticks counted
  // walking, and the body at the same place; only the pose differs.
  CHECK(number(no_clip->find("places")) == 240.0);
  CHECK(number(no_clip->find("places")) == number(with_clip->find("places")));
  CHECK(number(no_clip->find("walking_ticks")) == number(with_clip->find("walking_ticks")));
  CHECK(number(no_clip->find("walking_ticks")) > 100.0);
  CHECK(write_json(*at(b, {"interactive", "player", "actor", "position"})) ==
        write_json(*at(a, {"interactive", "player", "actor", "position"})));
  CHECK(write_json(*at(b, {"interactive", "player", "actor", "yaw"})) ==
        write_json(*at(a, {"interactive", "player", "actor", "yaw"})));
  CHECK(text(at(b, {"interactive", "trajectory", "hash"})) ==
        text(at(a, {"interactive", "trajectory", "hash"})));
  CHECK(text(at(b, {"interactive", "player", "actor", "clip"})).empty());
  // No clip advanced: the walking body's animation hash is a standing body's.
  CHECK(number(at(c, {"interactive", "player", "walking_ticks"})) == 0.0);
  CHECK(text(at(b, {"animation", "hash"})) == text(at(c, {"animation", "hash"})));
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
  // The walker's eye and its collapse (P58's "eye=walker"): what this case held before the face's
  // eye became the default, drawn exactly as it was.
  const Run collapsed = capture("body", {"--player", rig, "--tunable", "view.player.eye=walker"});
  REQUIRE_MESSAGE(collapsed.exit_code == 0, collapsed.output);
  const Run whole =
      capture("whole", {"--player", rig, "--tunable", "view.player.collapse_head=false",
                        "--tunable", "view.player.eye=walker"});
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

// (P58) The eye at the face: one standing session with two markers — at tick 60 looking ahead, at
// tick 160 looking 60 degrees down (the pointer 476 pixels down at tick 100) — replayed with the
// body under the face's eye (the default) and with its head forced collapsed, and the bare scene
// flown from each marker's face camera (a flying session started where the summary says the face
// camera stood), so every comparison is from the same camera.
TEST_CASE(
    "engine-view: --player's eye at the face sees no head ahead, draws the head whole, and looking "
    "down carries the eye forward and down") {
  const std::string rig = rigged_figure();
  if (rig.empty()) return;
  const test::TempDir tmp("engine_view_player_face");
  const std::string scene = ground_scene(tmp);
  const f32 yaw = 0.6f;
  const auto standing = [&](const char* name, const WorldPos& start, f32 pitch, bool walking,
                            bool look_down) {
    input::InputLog keys;
    keys.set_map(view::default_fly_map());
    const auto key = [&](u64 tick, bool down) {
      keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, 16, down ? 1.0f : 0.0f, 0});
    };
    key(60, true);  // M
    key(61, false);
    if (look_down)
      keys.record(input::RawEvent{SimTick{100}, input::Source::MouseAxis, 1, 476.0f, 0});
    key(160, true);
    key(161, false);
    view::SessionHeader header;
    header.ticks = 200;
    header.scene = scene;
    header.start.position = start;
    header.start.yaw = yaw;
    header.start.pitch = pitch;
    header.has_walk = true;
    header.walking = walking;
    keys.set_session(view::session_to_json(header));
    const std::string log = tmp.file(name);
    REQUIRE(keys.save(log) == io::Status::Ok);
    return log;
  };
  const std::string log = standing("face.jsonl", WorldPos{0.0, 3.0, 0.0}, 0.0f, true, true);
  const auto capture = [&](const std::string& session, const char* name,
                           std::vector<std::string> extra) {
    std::vector<std::string> args = replay(tmp, session, scene);
    args.insert(args.end(), {"--marker-captures", tmp.file(name)});
    args.insert(args.end(), extra.begin(), extra.end());
    return run_view(args);
  };
  const auto png = [&](const char* name, const char* tick) {
    return tmp.file(std::string(name) + "/tick-" + tick + ".png");
  };
  const Run face = capture(log, "face", {"--player", rig});
  if (unavailable(face)) return;
  REQUIRE_MESSAGE(face.exit_code == 0, face.output);
  const Run collapsed =
      capture(log, "collapsed", {"--player", rig, "--tunable", "view.player.collapse_head=true"});
  REQUIRE_MESSAGE(collapsed.exit_code == 0, collapsed.output);
  JsonValue s;
  REQUIRE(summary_of(face, s));
  const JsonValue* player = at(s, {"interactive", "player"});
  REQUIRE(player != nullptr);
  MESSAGE("the face's eye: " << write_json(*player).substr(0, 2500));
  CHECK(text(player->find("eye")) == "face");
  CHECK(text(player->find("eye_from")) == "head");
  CHECK_FALSE(player->find("collapse_head")->as_bool());
  const JsonValue* look = player->find("look");
  REQUIRE(look != nullptr);
  CHECK(look->find("on")->as_bool());
  const JsonValue* markers = look->find("markers");
  REQUIRE(markers != nullptr);
  REQUIRE(markers->size() == 2);
  const JsonValue& ahead = (*markers)[0];
  const JsonValue& down = (*markers)[1];
  CHECK(number(ahead.find("tick")) == 60.0);
  CHECK(number(down.find("tick")) == 160.0);
  const JsonValue* trajectory_markers = at(s, {"interactive", "trajectory", "markers"});
  REQUIRE(trajectory_markers != nullptr);
  REQUIRE(trajectory_markers->size() == 2);
  const f64 down_pitch = number((*trajectory_markers)[1].find("pitch"));
  CHECK(std::fabs(down_pitch + 1.0471976) < 1e-4);

  // (b) Looking 60 degrees down the eye goes forward and down by the arithmetic of the ranges:
  // the rest eye turned in the body's y-z plane by the torso's share about the torso, then the
  // neck's about the neck where the torso left it, then the head's about the head.
  const JsonValue* e0 = ahead.find("eye_m");
  const JsonValue* e1 = down.find("eye_m");
  CHECK(std::fabs(element(e0, 1) - element(player->find("eye_point_m"), 1)) < 1e-4);
  CHECK(std::fabs(element(e0, 2) - element(player->find("eye_point_m"), 2)) < 1e-4);
  f64 eye[2] = {element(e0, 1), element(e0, 2)};
  f64 pivots[3][2] = {{element(look->find("torso_m"), 1), element(look->find("torso_m"), 2)},
                      {element(look->find("neck_m"), 1), element(look->find("neck_m"), 2)},
                      {element(look->find("head_m"), 1), element(look->find("head_m"), 2)}};
  const JsonValue* shares = down.find("down_deg");
  REQUIRE(shares != nullptr);
  const f64 flexion = number(look->find("flexion_deg"));
  const f64 deg = 60.0;
  // P60: the eyes take the first 30 degrees (the ocular share), so the neck and head turn the
  // remaining 30, inside the cervical range, and the torso none. The remainder goes to the neck and
  // head (24% the head's).
  const f64 ocular = number(down.find("ocular_pitch_deg"));
  CHECK(ocular == doctest::Approx(30.0).epsilon(1e-3));
  const f64 remainder = deg - ocular;
  CHECK(remainder < flexion);
  CHECK(element(shares, 0) == doctest::Approx(0.0).scale(1.0).epsilon(1e-3));
  CHECK(element(shares, 2) == doctest::Approx(remainder * 0.24).epsilon(1e-3));
  CHECK(element(shares, 1) == doctest::Approx(remainder * 0.76).epsilon(1e-3));
  for (u32 i = 0; i < 3; ++i) {
    const f64 a = element(shares, i) * 3.14159265358979 / 180.0;
    const f64 c = std::cos(a);
    const f64 sn = std::sin(a);
    const f64 py = pivots[i][0];
    const f64 pz = pivots[i][1];
    // The point and the pivots inside this joint's subtree turn: y' = y cos - z sin, z' = y sin
    // + z cos about the pivot (ahead toward down).
    const auto turn = [&](f64* p) {
      const f64 y = p[0] - py;
      const f64 z = p[1] - pz;
      p[0] = py + y * c - z * sn;
      p[1] = pz + y * sn + z * c;
    };
    turn(eye);
    for (u32 j = i + 1; j < 3; ++j)
      turn(pivots[j]);
  }
  MESSAGE("the eye looking ahead (y, z) "
          << element(e0, 1) << ", " << element(e0, 2) << "; 60 degrees down " << element(e1, 1)
          << ", " << element(e1, 2) << "; the ranges say " << eye[0] << ", " << eye[1]);
  CHECK(element(e1, 1) < element(e0, 1) - 0.02);  // down
  CHECK(element(e1, 2) > element(e0, 2) + 0.02);  // and forward
  CHECK(std::fabs(element(e1, 1) - eye[0]) < 2e-3);
  CHECK(std::fabs(element(e1, 2) - eye[1]) < 2e-3);

  // The bare scene from each face camera: a flying session started where it stood.
  const auto start_of = [&](const JsonValue& m) {
    const JsonValue* p = m.find("position");
    return WorldPos{element(p, 0), element(p, 1), element(p, 2)};
  };
  const Run bare_ahead =
      capture(standing("bare-ahead.jsonl", start_of(ahead), 0.0f, false, false), "bare-ahead", {});
  REQUIRE_MESSAGE(bare_ahead.exit_code == 0, bare_ahead.output);
  const Run bare_down = capture(
      standing("bare-down.jsonl", start_of(down), static_cast<f32>(down_pitch), false, false),
      "bare-down", {});
  REQUIRE_MESSAGE(bare_down.exit_code == 0, bare_down.output);

  // (a) The dome test (P57), generalised: looking ahead, the central third with the body is the
  // bare scene's to the byte — nothing of the head is in front of the camera — and so it is with
  // the head forced collapsed.
  const Diff ahead_bare = diff(png("bare-ahead", "0000060"), png("face", "0000060"));
  const Diff ahead_head = diff(png("collapsed", "0000060"), png("face", "0000060"));
  MESSAGE("looking ahead, the body changes "
          << ahead_bare.central << " pixels of the central third (" << ahead_bare.total
          << " in all); the whole head " << ahead_head.total << " against the collapsed one");
  CHECK(ahead_bare.central == 0);
  CHECK(ahead_head.central == 0);
  // The head is drawn: with the head whole and with it collapsed, from the same camera, the
  // pictures differ only where the head's shadow falls (the camera sees none of the head).
  const Diff down_head = diff(png("collapsed", "0000160"), png("face", "0000160"));
  MESSAGE("looking down, the whole head against the collapsed one: "
          << down_head.total << " pixels, " << down_head.central << " of the central third");
  CHECK(down_head.total > 0);
  // (b) Looking down, the chest and the shoulders are seen from the face, as they were from the
  // walker's eye: the body changes the lower half against the bare scene from the same camera.
  const Diff down_bare = diff(png("bare-down", "0000160"), png("face", "0000160"));
  MESSAGE("looking down, the body changes " << down_bare.total << " pixels: " << down_bare.lower
                                            << " in the lower half, " << down_bare.central
                                            << " of the central third");
  CHECK(down_bare.lower > 500);
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

// Third person (P53): a standing walking session that marks at tick 60, presses V (scancode 25) at
// tick 100 and marks again at tick 160, under `map` (the latest revision's, or an earlier one).
static std::string third_person_log(const test::TempDir& tmp, const std::string& scene,
                                    const char* name, const input::ActionMap& map, bool press_v,
                                    const WorldPos& start, f32 pitch, bool walking) {
  input::InputLog keys;
  keys.set_map(map);
  const auto key = [&](u64 tick, u32 code, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0});
  };
  key(60, 16, true);  // M
  key(61, 16, false);
  if (press_v) {
    key(100, 25, true);  // V
    key(101, 25, false);
  }
  key(160, 16, true);
  key(161, 16, false);
  view::SessionHeader header;
  header.ticks = 200;
  header.scene = scene;
  header.start.position = start;
  header.start.yaw = 0.6f;
  header.start.pitch = pitch;
  header.has_walk = true;
  header.walking = walking;
  keys.set_session(view::session_to_json(header));
  const std::string path = tmp.file(name);
  REQUIRE(keys.save(path) == io::Status::Ok);
  return path;
}

TEST_CASE(
    "engine-view: V switches --player to third person: the camera stands back along the look, the "
    "body is in front of it, and the trajectory is the first person's") {
  const std::string rig = rigged_figure();
  if (rig.empty()) return;
  const test::TempDir tmp("engine_view_player_third");
  const std::string scene = ground_scene(tmp);
  const WorldPos start{0.0, 3.0, 0.0};
  const input::ActionMap latest = view::default_fly_map();
  const std::string log =
      third_person_log(tmp, scene, "third.jsonl", latest, true, start, 0.0f, true);
  const auto capture = [&](const std::string& session_log, const char* name,
                           std::vector<std::string> extra) {
    std::vector<std::string> args = replay(tmp, session_log, scene);
    args.insert(args.end(), {"--marker-captures", tmp.file(name)});
    args.insert(args.end(), extra.begin(), extra.end());
    return run_view(args);
  };
  const auto png = [&](const char* name, const char* tick) {
    return tmp.file(std::string(name) + "/tick-" + tick + ".png");
  };
  const Run third = capture(log, "third", {"--player", rig});
  if (unavailable(third)) return;
  REQUIRE_MESSAGE(third.exit_code == 0, third.output);
  JsonValue s;
  REQUIRE(summary_of(third, s));
  const JsonValue* player = at(s, {"interactive", "player"});
  REQUIRE(player != nullptr);
  // The mode at the end, the presses that switched it and the ticks walked in it.
  CHECK(text(player->find("camera")) == "third");
  CHECK(number(player->find("camera_toggles")) == 1.0);
  const f64 third_ticks = number(player->find("third_person_ticks"));
  MESSAGE("third-person ticks: " << third_ticks << " of 200; boom "
                                 << number(player->find("boom_m")) << " m, "
                                 << number(player->find("boom_height_m")) << " m up");
  CHECK(third_ticks >= 99.0);
  CHECK(third_ticks <= 101.0);
  CHECK(number(player->find("boom_m")) == doctest::Approx(2.5));
  CHECK(number(player->find("boom_height_m")) == doctest::Approx(0.4));
  const JsonValue* markers = at(*player, {"look", "markers"});
  REQUIRE(markers != nullptr);
  REQUIRE(markers->size() == 2);
  const JsonValue& before = (*markers)[0];  // tick 60: first person (P58's face camera)
  const JsonValue& after = (*markers)[1];   // tick 160: third person
  CHECK(before.find("third_person") == nullptr);
  REQUIRE(after.find("third_person") != nullptr);
  // The boom: the walker's eye (the trajectory's marker) 2.5 m back along the level look and 0.4 m
  // up.
  const JsonValue* trajectory_markers = at(s, {"interactive", "trajectory", "markers"});
  REQUIRE(trajectory_markers != nullptr);
  REQUIRE(trajectory_markers->size() == 2);
  const JsonValue* eye = (*trajectory_markers)[1].find("position");
  const JsonValue* boom = after.find("position");
  const f64 dx = element(boom, 0) - element(eye, 0);
  const f64 dz = element(boom, 2) - element(eye, 2);
  CHECK(std::sqrt(dx * dx + dz * dz) == doctest::Approx(2.5).epsilon(1e-4));
  CHECK(element(boom, 1) - element(eye, 1) == doctest::Approx(0.4).epsilon(1e-4));
  // ...behind it: the displacement from the eye to the camera is the look's own forward reversed,
  // `fly_forward` on the level look of the marker's yaw (-sin yaw, 0, -cos yaw): the convention the
  // boom and the trajectory share, held here through the same function rather than re-derived.
  view::FlyState level;
  level.yaw = static_cast<f32>(number((*trajectory_markers)[1].find("yaw")));
  const auto forward = view::fly_forward(level);
  CHECK(std::fabs(dx + 2.5 * static_cast<f64>(forward.x)) < 1e-3);
  CHECK(std::fabs(dz + 2.5 * static_cast<f64>(forward.z)) < 1e-3);

  // (a) The bare scene from each camera: before V the central third is the bare scene's to the
  // pixel (P58's face), after it the body stands in front of the boom and the central third
  // differs.
  const auto start_of = [&](const JsonValue& m) {
    const JsonValue* p = m.find("position");
    return WorldPos{element(p, 0), element(p, 1), element(p, 2)};
  };
  const Run bare_before = capture(third_person_log(tmp, scene, "bare-before.jsonl", latest, false,
                                                   start_of(before), 0.0f, false),
                                  "bare-before", {});
  REQUIRE_MESSAGE(bare_before.exit_code == 0, bare_before.output);
  const Run bare_after = capture(
      third_person_log(tmp, scene, "bare-after.jsonl", latest, false, start_of(after), 0.0f, false),
      "bare-after", {});
  REQUIRE_MESSAGE(bare_after.exit_code == 0, bare_after.output);
  const Diff first_person = diff(png("bare-before", "0000060"), png("third", "0000060"));
  const Diff behind = diff(png("bare-after", "0000160"), png("third", "0000160"));
  MESSAGE("first person at tick 60: "
          << first_person.central << " central-third pixels differ from "
          << "the bare scene (" << first_person.total << " in all); "
          << "third person at tick 160: " << behind.central << " (" << behind.total << " in all)");
  CHECK(first_person.central == 0);
  CHECK(behind.central > 100);

  // (b) The mode is the drawn camera alone: the trajectory and animation hashes of the same log
  // with no body, and of the same standing without the press, are the third-person run's.
  const Run bodiless = run_view(replay(tmp, log, scene));
  REQUIRE_MESSAGE(bodiless.exit_code == 0, bodiless.output);
  JsonValue n;
  REQUIRE(summary_of(bodiless, n));
  const std::string hash = text(at(s, {"interactive", "trajectory", "hash"}));
  CHECK(!hash.empty());
  CHECK(text(at(n, {"interactive", "trajectory", "hash"})) == hash);
  std::vector<std::string> first_args = replay(
      tmp, third_person_log(tmp, scene, "first.jsonl", latest, false, start, 0.0f, true), scene);
  first_args.insert(first_args.end(), {"--player", rig});
  const Run first = run_view(first_args);
  REQUIRE_MESSAGE(first.exit_code == 0, first.output);
  JsonValue f;
  REQUIRE(summary_of(first, f));
  CHECK(text(at(f, {"interactive", "trajectory", "hash"})) == hash);
  CHECK(text(at(f, {"animation", "hash"})) == text(at(s, {"animation", "hash"})));
  CHECK(text(at(f, {"interactive", "player", "camera"})) == "first");
  CHECK(number(at(f, {"interactive", "player", "camera_toggles"})) == 0.0);
  CHECK(number(at(f, {"interactive", "player", "third_person_ticks"})) == 0.0);

  // (c) A log recorded against the map's fifth revision replays with it, and its V presses nothing.
  const Run old = capture(third_person_log(tmp, scene, "fifth.jsonl", view::default_fly_map(5),
                                           true, start, 0.0f, true),
                          "fifth", {"--player", rig});
  REQUIRE_MESSAGE(old.exit_code == 0, old.output);
  JsonValue o;
  REQUIRE(summary_of(old, o));
  REQUIRE(at(o, {"interactive", "map"}) != nullptr);
  CHECK(write_json(*at(o, {"interactive", "map"})) != write_json(*at(s, {"interactive", "map"})));
  CHECK(text(at(o, {"interactive", "player", "camera"})) == "first");
  CHECK(number(at(o, {"interactive", "player", "camera_toggles"})) == 0.0);
  CHECK(number(at(o, {"interactive", "player", "third_person_ticks"})) == 0.0);
  CHECK(at(o, {"interactive", "player", "look", "markers"})->size() == 2);
  CHECK((*at(o, {"interactive", "player", "look", "markers"}))[1].find("third_person") == nullptr);
}

TEST_CASE(
    "engine-view: third person draws the head whole under the walker's eye whatever "
    "view.player.collapse_head says") {
  const std::string rig = rigged_figure();
  if (rig.empty()) return;
  const test::TempDir tmp("engine_view_player_third_head");
  const std::string scene = ground_scene(tmp);
  // Looking down at the ground ahead, so the head's shadow and the body fall in view.
  const std::string log = third_person_log(tmp, scene, "head.jsonl", view::default_fly_map(), true,
                                           WorldPos{0.0, 3.0, 0.0}, -0.5f, true);
  const auto capture = [&](const char* name, const char* collapse) {
    std::vector<std::string> args = replay(tmp, log, scene);
    args.insert(args.end(), {"--marker-captures", tmp.file(name), "--player", rig, "--tunable",
                             "view.player.eye=walker", "--tunable",
                             std::string("view.player.collapse_head=") + collapse});
    return run_view(args);
  };
  const Run collapsed = capture("collapsed", "true");
  if (unavailable(collapsed)) return;
  REQUIRE_MESSAGE(collapsed.exit_code == 0, collapsed.output);
  const Run whole = capture("whole", "false");
  REQUIRE_MESSAGE(whole.exit_code == 0, whole.output);
  const auto png = [&](const char* name, const char* tick) {
    return tmp.file(std::string(name) + "/tick-" + tick + ".png");
  };
  // First person under the walker's eye: the collapsed head and the whole one are different
  // pictures (the control, so the test cannot pass because the rule does nothing). Third person:
  // the rule is off, and the two are the same pixels.
  const Diff first_person = diff(png("collapsed", "0000060"), png("whole", "0000060"));
  const Diff third_person = diff(png("collapsed", "0000160"), png("whole", "0000160"));
  MESSAGE("collapse_head true against false, first person: "
          << first_person.total << " pixels; third person: " << third_person.total);
  CHECK(first_person.total > 0);
  CHECK(third_person.total == 0);
}

#endif  // ENGINE_VIEW_GAME && ENGINE_VIEW_ANIMATION

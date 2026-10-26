// End to end: the actors a game moves (animation.h `GameActors`, module_abi.h ABI 5; docs/
// subsystems/apps.md "Actors: the characters a game moves", roadmap S88). A scene of this test's
// own — a gentle waves ground and the skinned bar of mesh_view_tests.cpp standing on it, named
// `bar` — replayed offscreen with the actor build of the fixture (`walk_poc_actor`), which walks
// the bar round a circle on the ground, plays `bend`, doubles its rate and fades it to rest: the
// report carries its path and pose hashes, a second replay the same ones, the picture has the bar
// where the module put it, and a cold restore from `--game-state` stands it where the state says.
// Every run is offscreen (view_process.h).

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
#include <filesystem>
#include <string>
#include <vector>

using namespace engine;

#if ENGINE_VIEW_GAME && ENGINE_VIEW_ANIMATION && defined(WALK_POC_PATH) && \
    defined(WALK_POC_ACTOR_PATH)

namespace engine::view_test {
bool write_skinned_bar_glb(const std::string& path);  // mesh_view_tests.cpp
bool write_box_glb(const std::string& path);          // game_props_tests.cpp
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

std::string fixture(const test::TempDir& tmp, const char* built, const char* name) {
  const std::filesystem::path from(built);
  const std::string to = tmp.file(std::string(name) + from.extension().string());
  std::error_code ec;
  std::filesystem::copy_file(from, std::filesystem::path(to),
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_MESSAGE(!ec, "copying " << built << ": " << ec.message());
  return to;
}

// The waves of game_props_view_tests.cpp, 20 cm over 128 m, and the bar standing on them 10 m
// ahead of the walker, named `bar` and playing `bend` from its block: the one actor. With
// `props`, the scene also declares the fixture's two props (`can` and `marker`, the box of
// game_props_tests.cpp), which makes it **dynamic** — its instances are the renderer's prefix and a
// tail of props that come and go — and the fixture places the can on its first tick.
std::string actor_scene(const test::TempDir& tmp, bool props = false) {
  REQUIRE(view_test::write_skinned_bar_glb(tmp.file("bar.glb")));
  if (props) REQUIRE(view_test::write_box_glb(tmp.file("box.glb")));
  const std::string scene = tmp.file(props ? "actors_props.json" : "actors.json");
  std::string body = R"({"format":"engine.scene.v1","name":"actors",
      "terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30},
      "meshes":[{"path":"bar.glb"}],)";
  if (props) {
    body += R"("props":[{"name":"can","mesh":"box.glb","fit":{"height":1}},
                        {"name":"marker","mesh":"box.glb","fit":{"height":0.5}}],)";
  }
  body += R"("instances":[{"mesh":0,"name":"bar","translation":[0,0,0],"ground":true,
                    "animation":{"clip":"bend"}}]})";
  REQUIRE(io::write_file(scene, body) == io::Status::Ok);
  return scene;
}

// Standing at (0, 3, 10) looking along -z at the bar, `ticks` long, nothing pressed.
std::string stand_session(const test::TempDir& tmp, const std::string& scene, u64 ticks,
                          const char* name) {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  view::SessionHeader header;
  header.ticks = ticks;
  header.scene = scene;
  header.start.position = WorldPos{0.0, 3.0, 10.0};
  header.start.yaw = 0.0f;
  header.start.pitch = -0.15f;
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  const std::string path = tmp.file(name);
  REQUIRE(keys.save(path) == io::Status::Ok);
  return path;
}

const JsonValue* bar_of(const JsonValue& summary) {
  const JsonValue* actors = at(summary, {"interactive", "game", "actors"});
  return actors != nullptr && actors->is_array() && actors->size() == 1 ? &(*actors)[0] : nullptr;
}

u64 differing_pixels(const std::string& a_path, const std::string& b_path) {
  image::Image a;
  image::Image b;
  REQUIRE(image::read_image(a_path, a) == io::Status::Ok);
  REQUIRE(image::read_image(b_path, b) == io::Status::Ok);
  REQUIRE(a.width == b.width);
  REQUIRE(a.height == b.height);
  u64 n = 0;
  for (u32 i = 0; i + 3 < a.pixels.size(); i += 4) {
    if (a.pixels[i] != b.pixels[i] || a.pixels[i + 1] != b.pixels[i + 1] ||
        a.pixels[i + 2] != b.pixels[i + 2]) {
      ++n;
    }
  }
  return n;
}

}  // namespace

TEST_CASE(
    "engine-view: a game walks the scene's actor round a circle on the ground, plays and stops "
    "its clip, and a replay ends on the same path and poses") {
  const test::TempDir tmp("engine_view_actors");
  const std::string scene = actor_scene(tmp);
  const std::string session = stand_session(tmp, scene, 480, "stand.jsonl");
  const std::string module = fixture(tmp, WALK_POC_ACTOR_PATH, "walk_poc_actor");
  const std::string plain = fixture(tmp, WALK_POC_PATH, "walk_poc");
  // The finest animation band at every distance, so a tier change cannot move a hash.
  const std::vector<std::string> common = {
      "--replay-input", session, "--scene", scene,           "--offscreen",      "--width", "320",
      "--height",       "200",   "--ddc",   tmp.file("ddc"), "--anim-lod-scale", "100000"};
  std::vector<std::string> args = common;
  args.insert(args.end(), {"--game", module, "--capture", tmp.file("walked.png")});
  const Run first = run_view(args);
  if (first.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << first.output.substr(0, 300));
    return;
  }
  REQUIRE_MESSAGE(first.exit_code == 0, first.output);
  JsonValue a;
  REQUIRE_MESSAGE(summary_of(first, a), first.output);
  const JsonValue* bar = bar_of(a);
  REQUIRE_MESSAGE(bar != nullptr, first.output.substr(0, 2000));
  CHECK(text(bar->find("name")) == "bar");
  CHECK(number(bar->find("instance")) == 0.0);
  const JsonValue* calls = at(a, {"interactive", "game", "actor_calls"});
  REQUIRE(calls != nullptr);
  CHECK(number(calls->find("places")) == 480.0);  // a place every tick
  CHECK(number(calls->find("plays")) == 1.0);
  CHECK(number(calls->find("speeds")) == 1.0);
  CHECK(number(calls->find("stops")) == 1.0);
  CHECK(number(calls->find("refused")) == 0.0);
  CHECK(number(calls->find("ground_queries")) == 480.0);
  CHECK(calls->find("can_move")->as_bool());
  CHECK(number(calls->find("flushes")) > 0.0);
  // Stopped: the fade from tick 240 was over by 360, the clip cleared, the rest pose drawn.
  CHECK(text(bar->find("clip")).empty());
  CHECK_FALSE(bar->find("playing")->as_bool());
  CHECK(number(at(a, {"interactive", "game", "state", "actor_ticks"})) == 480.0);

  // The path: a sample a quarter-second, each on the 3 m circle round where the bar stood, its
  // height the ground's (the waves are 20 cm, the circle's centre on them), playing `bend` before
  // the stop and at rest after the fade.
  const JsonValue* path = bar->find("path");
  REQUIRE(path != nullptr);
  REQUIRE(path->size() == 8);  // ticks 60, 120, ..., 480
  const JsonValue* first_at = (*path)[0].find("position");
  REQUIRE(first_at != nullptr);
  std::string playing_hash;
  for (u32 i = 0; i < path->size(); ++i) {
    const JsonValue& sample = (*path)[i];
    CHECK(number(sample.find("tick")) == 60.0 * (i + 1));
    const JsonValue* p = sample.find("position");
    REQUIRE(p != nullptr);
    const f64 x = number(&(*p)[0]);
    const f64 y = number(&(*p)[1]);
    const f64 z = number(&(*p)[2]);
    CHECK(std::sqrt(x * x + z * z) == doctest::Approx(3.0).epsilon(1e-9));
    CHECK(std::abs(y) < 0.25);
    const std::string clip = text(sample.find("clip"));
    if (i < 4) {
      CHECK(clip == "bar/bend");
      CHECK(text(sample.find("pose_hash")) != playing_hash);  // the clip moves the pose
      playing_hash = text(sample.find("pose_hash"));
    } else if (i >= 5) {
      CHECK(clip.empty());
      CHECK(text(sample.find("pose_hash")) == text((*path)[5].find("pose_hash")));  // at rest
    }
  }
  CHECK(text((*path)[7].find("pose_hash")) != text((*path)[0].find("pose_hash")));
  MESSAGE("the bar's path: " << write_json(*path).substr(0, 600));

  // The same recording again: the same path and poses, to the bit, and the same world.
  const Run second = run_view(args);
  REQUIRE_MESSAGE(second.exit_code == 0, second.output);
  JsonValue b;
  REQUIRE(summary_of(second, b));
  REQUIRE(bar_of(b) != nullptr);
  CHECK(write_json(*bar_of(b)) == write_json(*bar));
  CHECK(text(at(b, {"animation", "hash"})) == text(at(a, {"animation", "hash"})));

  // The picture: the plain fixture leaves the bar where the scene stands it, so the two captures
  // differ where the bar was and where the module walked it.
  args = common;
  args.insert(args.end(), {"--game", plain, "--capture", tmp.file("still.png")});
  const Run still = run_view(args);
  REQUIRE_MESSAGE(still.exit_code == 0, still.output);
  const u64 moved = differing_pixels(tmp.file("walked.png"), tmp.file("still.png"));
  MESSAGE("the walked bar changed " << moved << " pixels of 64,000");
  CHECK(moved > 20);
}

TEST_CASE(
    "engine-view: in a scene with props the actor walks too, beside the props, and a replay ends "
    "on the same path and poses") {
  // The scene declares props, so it is dynamic, and the fixture places the can on its first tick:
  // the renderer takes the tail and the bar's moves through the same table flips (renderer.md,
  // "Instances that come and go"). Until 2026-10-19 every place here was refused (`Unsupported`,
  // `can_move` false) and the bar stood where the scene put it.
  const test::TempDir tmp("engine_view_actors_props");
  const std::string scene = actor_scene(tmp, true);
  const std::string session = stand_session(tmp, scene, 480, "stand.jsonl");
  const std::string module = fixture(tmp, WALK_POC_ACTOR_PATH, "walk_poc_actor");
  const std::string plain = fixture(tmp, WALK_POC_PATH, "walk_poc");
  const std::vector<std::string> common = {
      "--replay-input", session, "--scene", scene,           "--offscreen",      "--width", "320",
      "--height",       "200",   "--ddc",   tmp.file("ddc"), "--anim-lod-scale", "100000"};
  std::vector<std::string> args = common;
  args.insert(args.end(), {"--game", module, "--capture", tmp.file("walked.png")});
  const Run first = run_view(args);
  if (first.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << first.output.substr(0, 300));
    return;
  }
  REQUIRE_MESSAGE(first.exit_code == 0, first.output);
  JsonValue a;
  REQUIRE_MESSAGE(summary_of(first, a), first.output);
  CHECK(number(at(a, {"interactive", "game", "props", "live"})) == 1.0);  // the can
  const JsonValue* bar = bar_of(a);
  REQUIRE_MESSAGE(bar != nullptr, first.output.substr(0, 2000));
  const JsonValue* calls = at(a, {"interactive", "game", "actor_calls"});
  REQUIRE(calls != nullptr);
  CHECK(calls->find("can_move")->as_bool());
  CHECK(number(calls->find("places")) == 480.0);
  CHECK(number(calls->find("refused")) == 0.0);
  CHECK(number(calls->find("flushes")) > 0.0);
  MESSAGE("actor calls: " << write_json(*calls));
  CHECK(first.output.find("actor_place hears Unsupported") == std::string::npos);
  // The path moves: every sample on the 3 m circle, no two at one place.
  const JsonValue* path = bar->find("path");
  REQUIRE(path != nullptr);
  REQUIRE(path->size() == 8);
  for (u32 i = 0; i < path->size(); ++i) {
    const JsonValue* p = (*path)[i].find("position");
    REQUIRE(p != nullptr);
    const f64 x = number(&(*p)[0]);
    const f64 z = number(&(*p)[2]);
    CHECK(std::sqrt(x * x + z * z) == doctest::Approx(3.0).epsilon(1e-9));
    if (i > 0) CHECK(write_json(*p) != write_json(*(*path)[i - 1].find("position")));
  }
  MESSAGE("the bar's path beside the props: " << write_json(*path).substr(0, 600));

  // The same recording again: the same path and poses, to the bit, and the same world.
  const Run second = run_view(args);
  REQUIRE_MESSAGE(second.exit_code == 0, second.output);
  JsonValue b;
  REQUIRE(summary_of(second, b));
  REQUIRE(bar_of(b) != nullptr);
  CHECK(write_json(*bar_of(b)) == write_json(*bar));
  CHECK(text(at(b, {"animation", "hash"})) == text(at(a, {"animation", "hash"})));

  // The picture: the plain fixture places the same can and leaves the bar where the scene stands
  // it, so the two captures differ where the bar was and where the module walked it.
  args = common;
  args.insert(args.end(), {"--game", plain, "--capture", tmp.file("still.png")});
  const Run still = run_view(args);
  REQUIRE_MESSAGE(still.exit_code == 0, still.output);
  JsonValue c;
  REQUIRE(summary_of(still, c));
  CHECK(number(at(c, {"interactive", "game", "props", "live"})) == 1.0);
  const u64 moved = differing_pixels(tmp.file("walked.png"), tmp.file("still.png"));
  MESSAGE("beside the props, the walked bar changed " << moved << " pixels of 64,000");
  CHECK(moved > 20);
}

#if ENGINE_VIEW_WORLD
// The actor scene with six rigid boxes before the bar, so the bar is the file's instance 6: read
// whole, or with a `world` block that streams the boxes (renderer.md, "Authored instances per
// tile") and keeps the bar — the loaded prefix is then the bar and the terrain, and the bar's
// loaded index is 0 where its handle is 6 (renderer.md, "The whole read's index"; roadmap W78).
std::string streamed_actor_scene(const test::TempDir& tmp, bool world) {
  REQUIRE(view_test::write_skinned_bar_glb(tmp.file("bar.glb")));
  REQUIRE(view_test::write_box_glb(tmp.file("box.glb")));
  const std::string scene = tmp.file(world ? "streamed.json" : "whole.json");
  std::string body = R"({"format":"engine.scene.v1","name":"actors-streamed",
      "terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30},
      "meshes":[{"path":"bar.glb"},{"path":"box.glb"}],
      "instances":[{"mesh":1,"translation":[-6,0,-4],"ground":true},
                   {"mesh":1,"translation":[6,0,-4],"ground":true},
                   {"mesh":1,"translation":[-10,0,6],"ground":true},
                   {"mesh":1,"translation":[10,0,6],"ground":true},
                   {"mesh":1,"translation":[-3,0,-12],"ground":true},
                   {"mesh":1,"translation":[3,0,-12],"ground":true},
                   {"mesh":0,"name":"bar","translation":[0,0,0],"ground":true,
                    "animation":{"clip":"bend"}}])";
  if (world) body += R"(,"world":{"tile_size":32,"rings":[{"radius":4}]})";
  REQUIRE(io::write_file(scene, body + "}") == io::Status::Ok);
  return scene;
}

TEST_CASE(
    "engine-view: in a streamed world the game's actor is kept, bound by its whole-read index, and "
    "walks and poses as it does in the whole read") {
  // Until W78 the bind looked the handle (6) up among the loaded instances, found the terrain or
  // nothing, and the game failed at its first `actor_state` ("forest: actor_state failed").
  const test::TempDir tmp("engine_view_actors_streamed");
  const std::string whole_scene = streamed_actor_scene(tmp, false);
  const std::string streamed_scene = streamed_actor_scene(tmp, true);
  const std::string module = fixture(tmp, WALK_POC_ACTOR_PATH, "walk_poc_actor");
  JsonValue summaries[2];
  for (u32 s = 0; s < 2; ++s) {
    const std::string& scene = s == 0 ? whole_scene : streamed_scene;
    const std::string session =
        stand_session(tmp, scene, 60, s == 0 ? "whole.jsonl" : "streamed.jsonl");
    const Run run = run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--width",
                              "320", "--height", "200", "--ddc", tmp.file("ddc"),
                              "--anim-lod-scale", "100000", "--game", module});
    if (run.exit_code == 3) {
      MESSAGE("engine-view unavailable here: " << run.output.substr(0, 300));
      return;
    }
    REQUIRE_MESSAGE(run.exit_code == 0, run.output);
    REQUIRE_MESSAGE(summary_of(run, summaries[s]), run.output);
    CHECK(run.output.find("does not animate") == std::string::npos);
  }
  const JsonValue& whole = summaries[0];
  const JsonValue& streamed = summaries[1];
  // The world streamed the six boxes and kept the bar.
  CHECK(number(at(streamed, {"world", "authored_streamed"})) == 6.0);
  CHECK(number(at(streamed, {"world", "authored_kept"})) == 1.0);
  const JsonValue* bar = bar_of(streamed);
  REQUIRE_MESSAGE(bar != nullptr, write_json(streamed).substr(0, 2000));
  CHECK(number(bar->find("instance")) == 6.0);  // the handle is the whole read's
  const JsonValue* calls = at(streamed, {"interactive", "game", "actor_calls"});
  REQUIRE(calls != nullptr);
  CHECK(number(calls->find("actors")) == 1.0);
  CHECK(number(calls->find("places")) == 60.0);
  CHECK(number(calls->find("refused")) == 0.0);
  CHECK(number(at(streamed, {"interactive", "game", "state", "actor_ticks"})) == 60.0);
  // The same actor, to the bit: its place, clip, playhead, pose hash and path — and the same world.
  REQUIRE(bar_of(whole) != nullptr);
  CHECK(write_json(*bar) == write_json(*bar_of(whole)));
  CHECK(text(at(streamed, {"animation", "hash"})) == text(at(whole, {"animation", "hash"})));
  MESSAGE("the streamed bar: " << write_json(*bar).substr(0, 600));
}
#endif  // ENGINE_VIEW_WORLD

TEST_CASE("engine-view: a cold restore from --game-state stands the actor where the state says") {
  // 200 ticks of the actor build: the bar mid-circle, playing `bend` at twice its rate. The state
  // file carries where it stood and its player; the plain fixture, which moves nothing, run from
  // that file has the bar there, in that clip, at that rate.
  const test::TempDir tmp("engine_view_actor_restore");
  const std::string scene = actor_scene(tmp);
  const std::string session = stand_session(tmp, scene, 200, "stand.jsonl");
  const std::string module = fixture(tmp, WALK_POC_ACTOR_PATH, "walk_poc_actor");
  const std::string plain = fixture(tmp, WALK_POC_PATH, "walk_poc");
  const std::string state = tmp.file("state.json");
  const std::vector<std::string> common = {"--replay-input",
                                           session,
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
                                           "--game-state",
                                           state};
  std::vector<std::string> args = common;
  args.insert(args.end(), {"--game", module});
  const Run first = run_view(args);
  if (first.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << first.output.substr(0, 300));
    return;
  }
  REQUIRE_MESSAGE(first.exit_code == 0, first.output);
  std::string saved_text;
  REQUIRE(io::read_file(state, saved_text) == io::Status::Ok);
  JsonValue saved;
  REQUIRE(parse_json(saved_text, saved).ok);
  const JsonValue* actors = saved.find("actors");
  REQUIRE_MESSAGE(actors != nullptr, saved_text);
  REQUIRE(actors->size() == 1);
  const JsonValue& entry = (*actors)[0];
  CHECK(text(entry.find("name")) == "bar");
  const JsonValue* saved_at = entry.find("position");
  REQUIRE(saved_at != nullptr);
  CHECK(number(at(entry, {"player", "speed"})) == 2.0);
  MESSAGE("saved: " << write_json(entry));

  args = common;
  args.insert(args.end(), {"--game", plain});
  const Run resumed = run_view(args);
  REQUIRE_MESSAGE(resumed.exit_code == 0, resumed.output);
  JsonValue b;
  REQUIRE(summary_of(resumed, b));
  CHECK(at(b, {"interactive", "game", "restored"})->as_bool());
  const JsonValue* bar = bar_of(b);
  REQUIRE(bar != nullptr);
  CHECK(write_json(*bar->find("position")) == write_json(*saved_at));
  CHECK(number(bar->find("yaw")) == number(entry.find("yaw")));
  CHECK(text(bar->find("clip")) == "bar/bend");
  CHECK(number(bar->find("speed")) == 2.0);
  CHECK(bar->find("playing")->as_bool());
  CHECK(number(at(b, {"interactive", "game", "actor_calls", "places"})) == 0.0);  // the restore
}

#endif

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

#if defined(WALK_POC_ALIGNED_PATH) && defined(WALK_POC_LEVEL_PATH) && defined(WALK_POC_ABI7_PATH)
namespace {

constexpr f64 k_pi = 3.14159265358979323846;

f64 deg(f64 degrees) { return degrees * k_pi / 180.0; }

// An `up` array of a path sample, or three NaNs when the sample is level (the report writes `up`
// only for a tilted actor).
void up_of(const JsonValue& sample, f64 (&out)[3]) {
  const JsonValue* up = sample.find("up");
  for (u32 k = 0; k < 3; ++k)
    out[k] = up != nullptr && up->is_array() && up->size() == 3 ? number(&(*up)[k]) : -99.0;
}

void check_up(const JsonValue& sample, f64 x, f64 y, f64 z) {
  f64 up[3];
  up_of(sample, up);
  CHECK(up[0] == doctest::Approx(x).epsilon(1e-5).scale(1.0));
  CHECK(up[1] == doctest::Approx(y).epsilon(1e-5).scale(1.0));
  CHECK(up[2] == doctest::Approx(z).epsilon(1e-5).scale(1.0));
}

}  // namespace

TEST_CASE(
    "engine-view: actor_place_aligned stands the actor on the slope the game names, holds the tilt "
    "to its maximum, refuses a bad up and leaves it where it was, and ground_sample answers the "
    "height and a unit normal") {
  const test::TempDir tmp("engine_view_actors_aligned");
  const std::string scene = actor_scene(tmp);
  const std::string session = stand_session(tmp, scene, 480, "stand.jsonl");
  const std::string module = fixture(tmp, WALK_POC_ALIGNED_PATH, "walk_poc_aligned");
  const std::vector<std::string> args = {"--replay-input",
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
                                         "--game",
                                         module};
  const Run first = run_view(args);
  if (first.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << first.output.substr(0, 300));
    return;
  }
  REQUIRE_MESSAGE(first.exit_code == 0, first.output);
  JsonValue a;
  REQUIRE_MESSAGE(summary_of(first, a), first.output);
  CHECK(number(at(a, {"interactive", "game", "abi"})) == 8.0);
  const JsonValue* bar = bar_of(a);
  REQUIRE_MESSAGE(bar != nullptr, first.output.substr(0, 2000));
  const JsonValue* path = bar->find("path");
  REQUIRE(path != nullptr);
  REQUIRE(path->size() == 8);  // the end of each one-second phase: ticks 60, 120, ..., 480
  MESSAGE("the bar's path: " << write_json(*path).substr(0, 1500));

  // Phase 0 stood `up` = (0,1,0): level, the report names no `up`. Phase 1, a 20 degree slope
  // toward +x: the actor's +y is the slope's normal. Phase 2, a 60 degree wall against the default
  // limit: held to 30. Phase 3, the same wall with a limit of 70: 60. Phase 4, four refusals: the
  // actor stays as phase 3 left it. Phase 5, the ground's own normal. Phases 6 and 7: level again.
  CHECK((*path)[0].find("up") == nullptr);
  check_up((*path)[1], -std::sin(deg(20.0)), std::cos(deg(20.0)), 0.0);
  check_up((*path)[2], -std::sin(deg(30.0)), std::cos(deg(30.0)), 0.0);
  check_up((*path)[3], -std::sin(deg(60.0)), std::cos(deg(60.0)), 0.0);
  REQUIRE((*path)[4].find("up") != nullptr);
  CHECK(write_json(*(*path)[4].find("up")) == write_json(*(*path)[3].find("up")));
  CHECK((*path)[6].find("up") == nullptr);
  CHECK((*path)[7].find("up") == nullptr);
  CHECK(bar->find("up") == nullptr);  // it ends level, so its block is as it always was

  // The module's own record: the answer of every call (four bits a call: 0 Ok, 6 InvalidArgument),
  // the tilt `actor_state` reported after the first four placements, and the clamp flag.
  const JsonValue* state = at(a, {"interactive", "game", "state"});
  REQUIRE(state != nullptr);
  u64 expected_codes = 0;
  for (const u32 refused : {4u, 5u, 6u, 7u, 11u, 12u})
    expected_codes |= u64{6} << (4u * refused);
  CHECK(number(state->find("align_codes")) == static_cast<f64>(expected_codes));
  const JsonValue* tilt = state->find("align_tilt");
  REQUIRE(tilt != nullptr);
  REQUIRE(tilt->size() == 4);
  CHECK(number(&(*tilt)[0]) == 0.0);
  CHECK(number(&(*tilt)[1]) == doctest::Approx(deg(20.0)).epsilon(1e-5));
  CHECK(number(&(*tilt)[2]) == doctest::Approx(deg(30.0)).epsilon(1e-5));
  CHECK(number(&(*tilt)[3]) == doctest::Approx(deg(60.0)).epsilon(1e-5));
  CHECK(number(state->find("align_clamped")) == 1.0);  // the 60 against 30

  // The ground sample: the height is `ground_height`'s to the bit, the normal is unit and upward,
  // and phase 5's placement with it stands the actor on it (the dunes are gentle, so the tilt is
  // under the limit and the actor's +y is the normal).
  CHECK(number(state->find("align_sample_height_delta")) == 0.0);
  const JsonValue* normal = state->find("align_sample_normal");
  REQUIRE(normal != nullptr);
  const f64 nx = number(&(*normal)[0]);
  const f64 ny = number(&(*normal)[1]);
  const f64 nz = number(&(*normal)[2]);
  CHECK(std::sqrt(nx * nx + ny * ny + nz * nz) == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(ny > 0.99);
  CHECK(ny < 1.0);  // the waves are not flat there
  check_up((*path)[5], nx, ny, nz);

  const JsonValue* calls = at(a, {"interactive", "game", "actor_calls"});
  REQUIRE(calls != nullptr);
  CHECK(number(calls->find("aligned_places")) == 5.0);  // phases 0, 1, 2, 3 and 5
  CHECK(number(calls->find("places")) == 6.0);          // and phase 6's level one
  CHECK(number(calls->find("tilt_clamped")) == 1.0);
  CHECK(number(calls->find("refused")) == 4.0);
  CHECK(number(calls->find("ground_samples")) ==
        61.0);  // phase 5's, and phase 7's sixty (the refused footprints are not answered)
  CHECK(number(calls->find("flushes")) > 0.0);

  // The same recording again: the same actor block, to the bit.
  const Run second = run_view(args);
  REQUIRE_MESSAGE(second.exit_code == 0, second.output);
  JsonValue b;
  REQUIRE(summary_of(second, b));
  REQUIRE(bar_of(b) != nullptr);
  CHECK(write_json(*bar_of(b)) == write_json(*bar));
}

TEST_CASE(
    "engine-view: ground_height costs the walker one ground query and ground_sample five, and the "
    "frame records say what they cost") {
  // The aligned build asks `ground_height` once a tick through its sixth second and `ground_sample`
  // once a tick through its eighth (and the height once, on that phase's first tick). Three replays
  // of 360, 420 and 480 ticks differ by those phases and by the walker's own queries, which a
  // standing session makes at the same rate in each: so the second difference of the frame records'
  // `walk.ground_queries` is the samples' extra, 60 x (5 - 1) plus the one height: 241. Before ABI
  // 8 there was no sample and the height was the one query; this pins that it still is.
  const test::TempDir tmp("engine_view_actors_ground_cost");
  const std::string scene = actor_scene(tmp);
  const std::string module = fixture(tmp, WALK_POC_ALIGNED_PATH, "walk_poc_aligned");
  struct Total {
    f64 queries = 0.0;
    f64 ms = 0.0;
  };
  Total totals[3];
  const u64 lengths[3] = {360, 420, 480};
  for (u32 i = 0; i < 3; ++i) {
    const std::string name = "cost" + std::to_string(lengths[i]);
    const std::string session = stand_session(tmp, scene, lengths[i], (name + ".jsonl").c_str());
    const Run run =
        run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--width", "320",
                  "--height", "200", "--ddc", tmp.file("ddc"), "--anim-lod-scale", "100000",
                  "--game", module, "--benchmark", tmp.file(name + ".bench.jsonl")});
    if (run.exit_code == 3) {
      MESSAGE("engine-view unavailable here: " << run.output.substr(0, 300));
      return;
    }
    REQUIRE_MESSAGE(run.exit_code == 0, run.output);
    std::string text_out;
    REQUIRE(io::read_file(tmp.file(name + ".bench.jsonl"), text_out) == io::Status::Ok);
    usize begin = 0;
    while (begin < text_out.size()) {
      usize end = text_out.find('\n', begin);
      if (end == std::string::npos) end = text_out.size();
      JsonValue record;
      if (end > begin && parse_json(text_out.substr(begin, end - begin), record).ok) {
        if (const JsonValue* walk = record.find("walk"); walk != nullptr && walk->is_object()) {
          totals[i].queries += number(walk->find("ground_queries"));
          totals[i].ms += number(walk->find("ground_query_ms"));
        }
      }
      begin = end + 1;
    }
    MESSAGE(lengths[i] << " ticks: " << totals[i].queries << " ground queries, " << totals[i].ms
                       << " ms in them");
  }
  REQUIRE(totals[0].queries > 0.0);  // the records carry the walk
  const f64 height_phase = totals[1].queries - totals[0].queries;
  const f64 sample_phase = totals[2].queries - totals[1].queries;
  MESSAGE("phase 6 (a ground_height a tick): "
          << height_phase << " queries, " << totals[1].ms - totals[0].ms
          << " ms; phase 7 (a ground_sample a tick): " << sample_phase << " queries, "
          << totals[2].ms - totals[1].ms << " ms");
  CHECK(sample_phase - height_phase == 241.0);
}

TEST_CASE(
    "engine-view: the identity up draws the level placement's picture to the pixel, and a 20 "
    "degree slope draws a different one") {
  const test::TempDir tmp("engine_view_actors_aligned_pictures");
  const std::string scene = actor_scene(tmp);
  const std::string aligned = fixture(tmp, WALK_POC_ALIGNED_PATH, "walk_poc_aligned");
  const std::string level = fixture(tmp, WALK_POC_LEVEL_PATH, "walk_poc_level");
  const auto capture = [&](const std::string& module, u64 ticks, const char* name) {
    const std::string session =
        stand_session(tmp, scene, ticks, (std::string(name) + ".jsonl").c_str());
    const std::string png = tmp.file(std::string(name) + ".png");
    const Run run = run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--width",
                              "320", "--height", "200", "--ddc", tmp.file("ddc"),
                              "--anim-lod-scale", "100000", "--game", module, "--capture", png});
    return std::pair<int, std::string>(run.exit_code, run.exit_code == 0 ? png : run.output);
  };
  // 60 ticks: phase 0 only, `up` = (0,1,0) against `actor_place`. 120: phase 1 has tipped the
  // aligned build's actor 20 degrees, and the level build's stands.
  const auto aligned_60 = capture(aligned, 60, "aligned60");
  if (aligned_60.first == 3) {
    MESSAGE("engine-view unavailable here: " << aligned_60.second.substr(0, 300));
    return;
  }
  REQUIRE_MESSAGE(aligned_60.first == 0, aligned_60.second);
  const auto level_60 = capture(level, 60, "level60");
  REQUIRE_MESSAGE(level_60.first == 0, level_60.second);
  const auto aligned_120 = capture(aligned, 120, "aligned120");
  REQUIRE_MESSAGE(aligned_120.first == 0, aligned_120.second);
  const auto level_120 = capture(level, 120, "level120");
  REQUIRE_MESSAGE(level_120.first == 0, level_120.second);
  CHECK(differing_pixels(aligned_60.second, level_60.second) == 0);
  const u64 tipped = differing_pixels(aligned_120.second, level_120.second);
  MESSAGE("the tipped bar changed " << tipped << " pixels of 64,000");
  CHECK(tipped > 20);
}

TEST_CASE(
    "engine-view: a module that declares ABI 7 loads on the ABI 8 host, places the actor level, "
    "and its 136-byte ActorState is served without a byte past it") {
  const test::TempDir tmp("engine_view_actors_abi7");
  const std::string scene = actor_scene(tmp);
  const std::string session = stand_session(tmp, scene, 240, "stand.jsonl");
  const std::string old_module = fixture(tmp, WALK_POC_ABI7_PATH, "walk_poc_abi7");
  const std::string module = fixture(tmp, WALK_POC_ACTOR_PATH, "walk_poc_actor");
  JsonValue summaries[2];
  for (u32 i = 0; i < 2; ++i) {
    const Run run =
        run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--width", "320",
                  "--height", "200", "--ddc", tmp.file("ddc"), "--anim-lod-scale", "100000",
                  "--game", i == 0 ? old_module : module});
    if (run.exit_code == 3) {
      MESSAGE("engine-view unavailable here: " << run.output.substr(0, 300));
      return;
    }
    REQUIRE_MESSAGE(run.exit_code == 0, run.output);
    REQUIRE_MESSAGE(summary_of(run, summaries[i]), run.output);
  }
  const JsonValue& old_run = summaries[0];
  CHECK(number(at(old_run, {"interactive", "game", "abi"})) == 7.0);
  CHECK(number(at(summaries[1], {"interactive", "game", "abi"})) == 8.0);
  // It circled the bar 240 ticks with `actor_place` and read its state each tick without the host
  // writing past the 136 bytes it owns (the guard behind the struct is counted in its state).
  CHECK(number(at(old_run, {"interactive", "game", "state", "actor_ticks"})) == 240.0);
  CHECK(number(at(old_run, {"interactive", "game", "state", "abi7_overrun"})) == 0.0);
  CHECK(number(at(old_run, {"interactive", "game", "actor_calls", "places"})) == 240.0);
  CHECK(number(at(old_run, {"interactive", "game", "actor_calls", "aligned_places"})) == 0.0);
  const JsonValue* bar = bar_of(old_run);
  REQUIRE(bar != nullptr);
  CHECK(bar->find("up") == nullptr);  // level
  // The same circle as the current build of the module walks: the same actor block to the bit, so
  // the older ABI changed nothing about where the actor stood or which pose it drew.
  REQUIRE(bar_of(summaries[1]) != nullptr);
  CHECK(write_json(*bar) == write_json(*bar_of(summaries[1])));
  CHECK(text(at(old_run, {"animation", "hash"})) == text(at(summaries[1], {"animation", "hash"})));
}
#endif  // the ABI 8 builds

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

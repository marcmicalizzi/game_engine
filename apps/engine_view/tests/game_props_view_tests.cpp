// End to end: a game's props in the picture (game_props.h; docs/subsystems/apps.md "The game
// contract", roadmap P43). A scene of this test's own — a gentle waves ground and a box declared as
// the prop `can` — replayed offscreen with the fixture module: a prop placed is drawn where it
// stands and nowhere else changes; a scene whose props nothing placed draws, outside the HUD's
// corner, the picture a run without the game draws; the state file carries the props. Every run is
// offscreen (view_process.h).

#include "../fly_camera.h"
#include "view_process.h"

#include <core/json/json.h>
#include <foundation/image/decode.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace engine;

namespace engine::view_test {
bool write_box_glb(const std::string& path);  // game_props_tests.cpp
}

#if ENGINE_VIEW_GAME && defined(WALK_POC_PATH)

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

f64 number_at(const JsonValue& v, std::initializer_list<const char*> path) {
  const JsonValue* p = at(v, path);
  f64 out = -1.0;
  return p != nullptr && p->get_f64(out) ? out : -1.0;
}

std::string fixture(const test::TempDir& tmp, const char* built, const char* name = "walk_poc") {
  const std::filesystem::path from(built);
  const std::string to = tmp.file(std::string(name) + from.extension().string());
  std::error_code ec;
  std::filesystem::copy_file(from, std::filesystem::path(to),
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_MESSAGE(!ec, "copying " << built << ": " << ec.message());
  return to;
}

// The test's scene: the renderer's own waves, 20 cm high over 128 m, and the box as the prop
// `can`, a metre tall (and, with `marker`, a second prop). No capability is needed to draw it.
std::string props_scene(const test::TempDir& tmp, bool with_props = true) {
  REQUIRE(view_test::write_box_glb(tmp.file("box.glb")));
  const std::string scene = tmp.file(with_props ? "props.json" : "plain.json");
  std::string text = R"({"format":"engine.scene.v1","name":"props",
      "terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30})";
  if (with_props) {
    text += R"(,"props":[{"name":"can","mesh":"box.glb","fit":{"height":1}},
                         {"name":"marker","mesh":"box.glb","fit":{"height":0.5}}])";
  }
  text += "}";
  REQUIRE(io::write_file(scene, text) == io::Status::Ok);
  return scene;
}

// A walk to replay: standing at (0, 3, 10) looking along -z and a little down, W held from tick
// 10, `ticks` long, E pressed at tick 60.
std::string walk_session(const test::TempDir& tmp, const std::string& scene, u64 ticks,
                         const char* name = "walk.jsonl") {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  const auto key = [&](u64 tick, u32 code, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0});
  };
  key(10, 26, true);  // W
  key(60, 8, true);   // E: interact
  key(61, 8, false);
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

struct Diff {
  u64 top_left = 0;                                    // the HUD's quarter
  u64 elsewhere = 0;                                   // everything else
  u32 min_x = ~0u, min_y = ~0u, max_x = 0, max_y = 0;  // the box round the differing pixels
};

Diff diff_images(const std::string& a_path, const std::string& b_path) {
  image::Image a;
  image::Image b;
  REQUIRE(image::read_image(a_path, a) == io::Status::Ok);
  REQUIRE(image::read_image(b_path, b) == io::Status::Ok);
  REQUIRE(a.width == b.width);
  REQUIRE(a.height == b.height);
  Diff d;
  for (u32 y = 0; y < a.height; ++y) {
    for (u32 x = 0; x < a.width; ++x) {
      const u32 i = (y * a.width + x) * 4;
      if (std::memcmp(&a.pixels[i], &b.pixels[i], 3) == 0) continue;
      if (x < a.width / 2 && y < a.height / 2) {
        ++d.top_left;
      } else {
        ++d.elsewhere;
      }
      d.min_x = x < d.min_x ? x : d.min_x;
      d.max_x = x > d.max_x ? x : d.max_x;
      d.min_y = y < d.min_y ? y : d.min_y;
      d.max_y = y > d.max_y ? y : d.max_y;
    }
  }
  return d;
}

const JsonValue* game_of(const JsonValue& summary) { return at(summary, {"interactive", "game"}); }

}  // namespace

TEST_CASE(
    "engine-view: a prop restored from the state file is drawn where it stands, and nothing "
    "else changes") {
  // Three captures of one 120-tick replay: with the game and a state file holding one `can` 8 m
  // ahead, with the game and no props, and without the game. The can is the only difference
  // between the first two; the HUD's corner the only one between the second and the third — so a
  // scene made dynamic for its props draws, where no prop is, the picture it drew before.
  const test::TempDir tmp("engine_view_props_tail");
  const std::string scene = props_scene(tmp);
  const std::string session = walk_session(tmp, scene, 120);
  // The build that places nothing itself, so the state file's can is the only prop.
#if defined(WALK_POC_NO_PROPS_PATH)
  const std::string module = fixture(tmp, WALK_POC_NO_PROPS_PATH);
#else
  const std::string module = fixture(tmp, WALK_POC_PATH);
#endif
  const std::string state = tmp.file("state.json");
  REQUIRE(io::write_file(state, R"({"format":"engine.game.state","version":1,"module":"walk_poc",
      "type":"engine.walk_poc.Survivor","state":{},
      "props":[{"id":1024,"prop":"can","position":[0,0.3,2],"yaw":0.4,"scale":1}]})") ==
          io::Status::Ok);
  const std::vector<std::string> common = {
      "--replay-input", session, "--scene", scene,          "--offscreen", "--width", "800",
      "--height",       "450",   "--ddc",   tmp.file("ddc")};
  std::vector<std::string> args = common;
  args.insert(args.end(),
              {"--game", module, "--game-state", state, "--capture", tmp.file("can.png")});
  const Run with_can = run_view(args);
  if (with_can.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << with_can.output);
    return;
  }
  REQUIRE_MESSAGE(with_can.exit_code == 0, with_can.output);
  args = common;
  args.insert(args.end(), {"--game", module, "--capture", tmp.file("none.png")});
  const Run without_can = run_view(args);
  REQUIRE_MESSAGE(without_can.exit_code == 0, without_can.output);
  args = common;
  args.insert(args.end(), {"--capture", tmp.file("plain.png")});
  const Run plain = run_view(args);
  REQUIRE_MESSAGE(plain.exit_code == 0, plain.output);

  const Diff can = diff_images(tmp.file("can.png"), tmp.file("none.png"));
  MESSAGE("the can changed " << can.top_left + can.elsewhere << " pixels, in x " << can.min_x
                             << ".." << can.max_x << " y " << can.min_y << ".." << can.max_y);
  CHECK(can.top_left + can.elsewhere > 50);  // a metre of box 8 m ahead at 800 x 450
  // One object (and its shadow on the sand): its pixels fit in a box half the picture each way.
  CHECK(can.max_x - can.min_x < 800 / 2);
  CHECK(can.max_y - can.min_y < 450 / 2);
  const Diff hud = diff_images(tmp.file("none.png"), tmp.file("plain.png"));
  MESSAGE("the HUD changed " << hud.top_left << " pixels in the top-left quarter, " << hud.elsewhere
                             << " elsewhere");
  CHECK(hud.top_left > 500);
  CHECK(hud.elsewhere == 0);
  if (hud.elsewhere != 0 || can.top_left + can.elsewhere == 0) tmp.keep();

  JsonValue a;
  REQUIRE(summary_of(with_can, a));
  REQUIRE(game_of(a) != nullptr);
  CHECK(number_at(*game_of(a), {"props", "declared"}) == 2.0);
  CHECK(number_at(*game_of(a), {"props", "live"}) == 1.0);
  CHECK(number_at(*game_of(a), {"props", "rewrites"}) == 1.0);  // the restore, before frame 1
  JsonValue b;
  REQUIRE(summary_of(without_can, b));
  CHECK(number_at(*game_of(b), {"props", "live"}) == 0.0);
  CHECK(number_at(*game_of(b), {"props", "rewrites"}) == 0.0);
  // The state file still holds the can after the run.
  std::string saved_text;
  JsonValue saved;
  REQUIRE(io::read_file(state, saved_text) == io::Status::Ok);
  REQUIRE(parse_json(saved_text, saved).ok);
  const JsonValue* props = saved.find("props");
  REQUIRE(props != nullptr);
  REQUIRE(props->is_array());
  CHECK(props->size() == 1);
  // A state file naming a prop the scene does not declare is refused before any device.
  REQUIRE(io::write_file(state, R"({"format":"engine.game.state","version":1,"module":"walk_poc",
      "type":"engine.walk_poc.Survivor","state":{},
      "props":[{"id":1024,"prop":"cup","position":[0,0,0],"yaw":0,"scale":1}]})") ==
          io::Status::Ok);
  args = common;
  args.insert(args.end(), {"--game", module, "--game-state", state});
  const Run refused = run_view(args);
  CHECK(refused.exit_code == 1);
  CHECK(refused.output.find("\"cup\"") != std::string::npos);
}

TEST_CASE(
    "engine-view: a scene with no props draws the same picture with --game and without, "
    "outside the HUD's corner") {
  const test::TempDir tmp("engine_view_props_none");
  const std::string scene = props_scene(tmp, /*with_props=*/false);
  const std::string session = walk_session(tmp, scene, 120);
#if defined(WALK_POC_NO_PROPS_PATH)
  const std::string module = fixture(tmp, WALK_POC_NO_PROPS_PATH);
#else
  const std::string module = fixture(tmp, WALK_POC_PATH);
#endif
  const std::vector<std::string> common = {
      "--replay-input", session, "--scene", scene,          "--offscreen", "--width", "800",
      "--height",       "450",   "--ddc",   tmp.file("ddc")};
  std::vector<std::string> args = common;
  args.insert(args.end(), {"--game", module, "--capture", tmp.file("game.png")});
  const Run game = run_view(args);
  if (game.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << game.output);
    return;
  }
  REQUIRE_MESSAGE(game.exit_code == 0, game.output);
  args = common;
  args.insert(args.end(), {"--capture", tmp.file("plain.png")});
  const Run plain = run_view(args);
  REQUIRE_MESSAGE(plain.exit_code == 0, plain.output);
  const Diff d = diff_images(tmp.file("game.png"), tmp.file("plain.png"));
  MESSAGE("the HUD changed " << d.top_left << " pixels in the top-left quarter, " << d.elsewhere
                             << " elsewhere");
  CHECK(d.elsewhere == 0);
  if (d.elsewhere != 0) tmp.keep();
  JsonValue summary;
  REQUIRE(summary_of(game, summary));
  REQUIRE(game_of(summary) != nullptr);
  CHECK(number_at(*game_of(summary), {"props", "declared"}) == 0.0);
  CHECK(number_at(*game_of(summary), {"props", "live"}) == 0.0);
}

TEST_CASE(
    "engine-view: the fixture's pick-up prompt stands below the picture's centre while the can is "
    "in reach, and the report says where") {
  // ABI 4's first consumer (apps.md "The HUD, provisional"): a state file puts the fixture's can a
  // metre in front of the walker, so from the first tick `nearest_prop` finds it in reach and the
  // module shows its `[E] Pick up` element — outlined text, no panel, anchored at the centre and
  // 80 px (at 1080p) below it. The run's report lists it with the bounds the last frame drew.
  const test::TempDir tmp("engine_view_props_prompt");
  const std::string scene = props_scene(tmp);
  const std::string session = walk_session(tmp, scene, 40, "near.jsonl");  // E is never reached
  const std::string module = fixture(tmp, WALK_POC_PATH);
  const std::string state = tmp.file("state.json");
  REQUIRE(io::write_file(state, R"({"format":"engine.game.state","version":1,"module":"walk_poc",
      "type":"engine.walk_poc.Survivor","state":{"can":1024,"can_placed":true},
      "props":[{"id":1024,"prop":"can","position":[0,0.3,9],"yaw":0,"scale":1}]})") ==
          io::Status::Ok);
  const std::string report = tmp.file("report.json");
  const Run run =
      run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--width", "800",
                "--height", "450", "--ddc", tmp.file("ddc"), "--game", module, "--game-state",
                state, "--report", report, "--capture", tmp.file("prompt.png")});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  std::string text;
  REQUIRE(io::read_file(report, text) == io::Status::Ok);
  JsonValue r;
  REQUIRE(parse_json(text, r).ok);
  const JsonValue* elements = at(r, {"game", "hud_elements"});
  REQUIRE(elements != nullptr);
  REQUIRE(elements->is_array());
  const JsonValue* prompt = nullptr;
  for (usize i = 0; i < elements->size(); ++i) {
    u64 id = 0;
    const JsonValue& e = (*elements)[i];
    if (e.find("id") != nullptr && e.find("id")->get_u64(id) && id == 1) prompt = &e;
  }
  REQUIRE_MESSAGE(prompt != nullptr, write_json(*elements));
  MESSAGE("the prompt: " << write_json(*prompt));
  CHECK(prompt->find("text")->as_string() == "[E] Pick up");
  CHECK(prompt->find("anchor")->as_string() == "centre");
  CHECK(prompt->find("visible")->as_bool());
  const JsonValue& b = *prompt->find("bounds_px");
  REQUIRE(b.size() == 4);
  f64 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  REQUIRE(b[usize{0}].get_f64(x0));
  REQUIRE(b[1].get_f64(y0));
  REQUIRE(b[2].get_f64(x1));
  REQUIRE(b[3].get_f64(y1));
  // Centred across; below the middle by 80 px at 1080p, 33 px at 450; 28 px a 12 px font here.
  CHECK((x0 + x1) * 0.5 == doctest::Approx(400.0).epsilon(0.01));
  CHECK((y0 + y1) * 0.5 == doctest::Approx(225.0 + 80.0 * 450.0 / 1080.0).epsilon(0.03));
  CHECK(y1 - y0 >= 12.0);
  CHECK(y1 - y0 <= 20.0);
  // The stack is listed beside it under its reserved ids, the meter first.
  u64 first_id = 0;
  REQUIRE((*elements)[usize{0}].find("id")->get_u64(first_id));
  CHECK(first_id == 0xFFFF0100u);
  // And in the summary the same.
  JsonValue summary;
  REQUIRE(summary_of(run, summary));
  REQUIRE(game_of(summary) != nullptr);
  REQUIRE(at(*game_of(summary), {"hud_elements"}) != nullptr);
  CHECK(at(*game_of(summary), {"hud_elements"})->size() == elements->size());
  CHECK(number_at(*game_of(summary), {"abi"}) ==
        7.0);  // the fixture is built against this header (ABI 7)
  // ABI 4's graphics state, as the fixture read it on its first tick, is the session's: the same
  // raster path and shadows the report's machine block names; its one request the host can make
  // is recorded, and the one it cannot (no such raster path) refused.
  const JsonValue* graphics = at(r, {"game", "graphics"});
  REQUIRE(graphics != nullptr);
  MESSAGE("graphics: " << write_json(*graphics));
  CHECK(graphics->find("known")->as_bool());
  CHECK(graphics->find("raster")->as_string() == at(r, {"machine", "raster"})->as_string());
  CHECK(graphics->find("shadows")->as_string() == at(r, {"machine", "shadows"})->as_string());
  CHECK(graphics->find("present")->as_string() == "offscreen");
  CHECK(graphics->find("display")->as_string() == "sdr");
  CHECK(number_at(*graphics, {"width"}) == 800.0);
  CHECK(number_at(*graphics, {"requests"}) == 1.0);
  CHECK(number_at(*graphics, {"refused_requests"}) == 1.0);
  REQUIRE(graphics->find("pending") != nullptr);
  CHECK(at(*graphics, {"pending", "shadows"})->as_string() == "csm");
}

#if defined(WALK_POC_NO_PROPS_PATH) && defined(WALK_POC_TELEPORT_PATH)

TEST_CASE(
    "engine-view: the fixture's can is drawn at the spring, on the frames it changed, and "
    "the state file carries it") {
  // The 120-tick replay with the fixture: the can placed on the first tick 25 m ahead is the only
  // difference, outside the HUD's corner, from the same replay with the build that places nothing;
  // the block went over once; and a second run from the state file has the can without placing it.
  const test::TempDir tmp("engine_view_props_fixture");
  const std::string scene = props_scene(tmp);
  const std::string session = walk_session(tmp, scene, 120);
  const std::string module = fixture(tmp, WALK_POC_PATH);
  const std::string quiet = fixture(tmp, WALK_POC_NO_PROPS_PATH, "walk_poc_quiet");
  const std::string state = tmp.file("state.json");
  const std::vector<std::string> common = {
      "--replay-input", session, "--scene", scene,          "--offscreen", "--width", "800",
      "--height",       "450",   "--ddc",   tmp.file("ddc")};
  std::vector<std::string> args = common;
  args.insert(args.end(),
              {"--game", module, "--game-state", state, "--capture", tmp.file("can.png")});
  const Run with_can = run_view(args);
  if (with_can.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << with_can.output);
    return;
  }
  REQUIRE_MESSAGE(with_can.exit_code == 0, with_can.output);
  args = common;
  args.insert(args.end(), {"--game", quiet, "--capture", tmp.file("none.png")});
  const Run without = run_view(args);
  REQUIRE_MESSAGE(without.exit_code == 0, without.output);
  const Diff can = diff_images(tmp.file("can.png"), tmp.file("none.png"));
  MESSAGE("the can changed " << can.top_left + can.elsewhere << " pixels, in x " << can.min_x
                             << ".." << can.max_x << " y " << can.min_y << ".." << can.max_y);
  CHECK(can.elsewhere > 20);  // a metre of box 25 m ahead
  CHECK(can.max_x - can.min_x < 800 / 2);
  CHECK(can.max_y - can.min_y < 450 / 2);
  JsonValue a;
  REQUIRE(summary_of(with_can, a));
  REQUIRE(game_of(a) != nullptr);
  CHECK(number_at(*game_of(a), {"props", "placed"}) == 1.0);
  CHECK(number_at(*game_of(a), {"props", "live"}) == 1.0);
  CHECK(number_at(*game_of(a), {"props", "rewrites"}) == 1.0);  // one tick changed a prop
  REQUIRE(at(*game_of(a), {"state", "can_placed"}) != nullptr);
  CHECK(at(*game_of(a), {"state", "can_placed"})->as_bool());
  // The state file: the can beside the state; a second run has it without placing it again.
  const Run resumed = run_view(args = [&] {
    std::vector<std::string> r = common;
    r.insert(r.end(), {"--game", module, "--game-state", state});
    return r;
  }());
  REQUIRE_MESSAGE(resumed.exit_code == 0, resumed.output);
  JsonValue b;
  REQUIRE(summary_of(resumed, b));
  REQUIRE(game_of(b) != nullptr);
  CHECK(at(*game_of(b), {"restored"})->as_bool());
  CHECK(number_at(*game_of(b), {"props", "placed"}) == 0.0);
  CHECK(number_at(*game_of(b), {"props", "live"}) == 1.0);
  CHECK(number_at(*game_of(b), {"props", "rewrites"}) == 1.0);  // the restore, before frame 1
}

TEST_CASE(
    "engine-view: a live --game walk with a teleport replays to its trajectory with the "
    "same module, and is refused without it or with another") {
  // A window on the test's desktop, walking from an injected log with the teleporting build and
  // recording itself; the recording replayed offscreen with the same module reaches the same
  // trajectory hash, without --game it is refused, and with the plain fixture it is refused too.
  const test::TempDir tmp("engine_view_props_teleport");
  const std::string scene = props_scene(tmp);
  const std::string inject = walk_session(tmp, scene, 240, "inject.jsonl");
  const std::string recorded = tmp.file("recorded.jsonl");
  const std::string teleporter = fixture(tmp, WALK_POC_TELEPORT_PATH, "walk_poc_teleport");
  const std::string plain = fixture(tmp, WALK_POC_PATH);
  const std::string ddc = tmp.file("ddc");
  const Run live = run_view({"--interactive", "--walk",         "--start", "0,3,10",
                             "0,-8.6",        "--inject-input", inject,    "--record-input",
                             recorded,        "--scene",        scene,     "--game",
                             teleporter,      "--width",        "320",     "--height",
                             "180",           "--no-vsync",     "--ddc",   ddc});
  if (live.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << live.output);
    return;
  }
  REQUIRE_MESSAGE(live.exit_code == 0, live.output);
  JsonValue a;
  REQUIRE_MESSAGE(summary_of(live, a), live.output);
  REQUIRE(game_of(a) != nullptr);
  CHECK(number_at(*game_of(a), {"walker", "teleports"}) == 1.0);
  CHECK(number_at(a, {"interactive", "walk", "teleports"}) == 1.0);
  // The header names the module.
  input::InputLog log;
  std::string error;
  REQUIRE(log.load(recorded, &error) == io::Status::Ok);
  view::SessionHeader header;
  REQUIRE_MESSAGE(view::session_from_json(log.session(), header, &error), error);
  CHECK(header.has_game);
  CHECK(header.game_module == "walk_poc_teleport");
  CHECK(header.game_build == 1);

  const Run replay = run_view({"--replay-input", recorded, "--offscreen", "--game", teleporter,
                               "--width", "320", "--height", "180", "--ddc", ddc});
  REQUIRE_MESSAGE(replay.exit_code == 0, replay.output);
  JsonValue b;
  REQUIRE_MESSAGE(summary_of(replay, b), replay.output);
  REQUIRE(at(a, {"interactive", "trajectory", "hash"}) != nullptr);
  REQUIRE(at(b, {"interactive", "trajectory", "hash"}) != nullptr);
  CHECK(write_json(*at(a, {"interactive", "trajectory", "hash"})) ==
        write_json(*at(b, {"interactive", "trajectory", "hash"})));
  CHECK(number_at(*game_of(b), {"walker", "teleports"}) == 1.0);
  // The walker stands ten metres further on than it would have: the live walk covers more ground
  // than the teleporter's own distance count (a teleport is not walking).
  CHECK(number_at(a, {"interactive", "walk", "distance_m"}) ==
        doctest::Approx(number_at(b, {"interactive", "walk", "distance_m"})));
  MESSAGE("live and replayed: " << write_json(*at(a, {"interactive", "trajectory", "hash"})));

  const Run without = run_view({"--replay-input", recorded, "--offscreen", "--ddc", ddc});
  CHECK(without.exit_code == 1);
  CHECK(without.output.find("was recorded with the game module 'walk_poc_teleport' (build 1), "
                            "whose calls can move the walker; replay it with --game and that "
                            "module") != std::string::npos);
  const Run other =
      run_view({"--replay-input", recorded, "--offscreen", "--game", plain, "--ddc", ddc});
  CHECK(other.exit_code == 1);
  CHECK(other.output.find("was recorded with the game module 'walk_poc_teleport' and --game "
                          "names 'walk_poc'; another module would walk the recording somewhere "
                          "else, so it is refused") != std::string::npos);
}

#endif  // WALK_POC_NO_PROPS_PATH && WALK_POC_TELEPORT_PATH

TEST_CASE("engine-view: a prop that declares a fire lights the frame where the game places it") {
  // R85 (renderer.md, "Fires"; apps.md, "Props"): the scene declares the box twice, as `can` and as
  // `campfire` with a fire 1.2 m over its origin, and two runs restore one of them at the same
  // place from the state file — the runner's table, the path `prop_place` takes. The box is the
  // same mesh, so what differs is the fire's light on the sand round it, and it only brightens.
  const test::TempDir tmp("engine_view_props_fire");
  REQUIRE(view_test::write_box_glb(tmp.file("box.glb")));
  const std::string scene = tmp.file("fire.json");
  REQUIRE(io::write_file(scene, R"({"format":"engine.scene.v1","name":"fire",
      "terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30},
      "props":[{"name":"can","mesh":"box.glb","fit":{"height":1}},
               {"name":"campfire","mesh":"box.glb","fit":{"height":1},
                "fire":{"name":"camp","position":[0,1.2,0],"intensity":4,"flicker":0}}]})") ==
          io::Status::Ok);
  const std::string session = walk_session(tmp, scene, 60);
#if defined(WALK_POC_NO_PROPS_PATH)
  const std::string module = fixture(tmp, WALK_POC_NO_PROPS_PATH);
#else
  const std::string module = fixture(tmp, WALK_POC_PATH);
#endif
  const auto state_with = [&](const char* prop, const char* name) {
    const std::string path = tmp.file(name);
    REQUIRE(io::write_file(path, std::string(R"({"format":"engine.game.state","version":1,
        "module":"walk_poc","type":"engine.walk_poc.Survivor","state":{},
        "props":[{"id":1024,"prop":")") +
                                     prop + R"(","position":[0,0.3,4],"yaw":0,"scale":1}]})") ==
            io::Status::Ok);
    return path;
  };
  const std::vector<std::string> common = {
      "--replay-input", session, "--scene", scene,           "--offscreen", "--width", "800",
      "--height",       "450",   "--ddc",   tmp.file("ddc"), "--game",      module};
  std::vector<std::string> args = common;
  args.insert(args.end(), {"--game-state", state_with("campfire", "fire.state.json"), "--capture",
                           tmp.file("fire.png")});
  const Run fire = run_view(args);
  if (fire.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << fire.output);
    return;
  }
  REQUIRE_MESSAGE(fire.exit_code == 0, fire.output);
  args = common;
  args.insert(args.end(), {"--game-state", state_with("can", "can.state.json"), "--capture",
                           tmp.file("can.png")});
  const Run can = run_view(args);
  REQUIRE_MESSAGE(can.exit_code == 0, can.output);

  image::Image a;
  image::Image b;
  REQUIRE(image::read_image(tmp.file("fire.png"), a) == io::Status::Ok);
  REQUIRE(image::read_image(tmp.file("can.png"), b) == io::Status::Ok);
  REQUIRE(a.pixels.size() == b.pixels.size());
  u32 brighter = 0, darker = 0;
  const u32 bytes = static_cast<u32>(a.pixels.size());
  for (u32 i = 0; i + 3 < bytes; i += 4) {
    const i32 d = (i32{a.pixels[i]} + a.pixels[i + 1] + a.pixels[i + 2]) -
                  (i32{b.pixels[i]} + b.pixels[i + 1] + b.pixels[i + 2]);
    brighter += d > 0 ? 1u : 0u;
    darker += d < 0 ? 1u : 0u;
  }
  MESSAGE("the placed fire brightened " << brighter << " pixels and darkened " << darker);
  CHECK(brighter > 2000);
  CHECK(darker == 0);
  if (brighter <= 2000 || darker != 0) tmp.keep();
}

#endif  // ENGINE_VIEW_GAME && WALK_POC_PATH

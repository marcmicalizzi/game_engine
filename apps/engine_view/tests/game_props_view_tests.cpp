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
  const std::string module = fixture(tmp, WALK_POC_PATH);
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

#endif  // ENGINE_VIEW_GAME && WALK_POC_PATH

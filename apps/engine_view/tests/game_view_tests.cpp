// End to end: what a playable proof of concept needs from engine-view (docs/subsystems/apps.md,
// "--report: what a tester sends back" and "--game: running a game module"; roadmap P21). The
// report a run leaves behind whichever way it ended, and — where this build carries the hotload
// capability — a game module run from the walking session: ticked every tick, its HUD drawn, its
// result in the summary and the report, its state saved and restored, its refusals clean, and a
// recorded session replaying to the same module state. Every run is offscreen or on the test's own
// desktop (view_process.h).

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

bool read_json_file(const std::string& path, JsonValue& out) {
  std::string text;
  return io::read_file(path, text) == io::Status::Ok && parse_json(text, out).ok;
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

std::string text_at(const JsonValue& v, std::initializer_list<const char*> path) {
  const JsonValue* p = at(v, path);
  return p != nullptr && p->is_string() ? std::string(p->as_string()) : std::string();
}

}  // namespace

TEST_CASE("engine-view: --report holds the summary, the machine and the frames of a run") {
  const test::TempDir tmp("engine_view_report");
  const std::string report = tmp.file("report.json");
  const Run run = run_view({"--width", "160", "--height", "96", "--frames", "8", "--grid", "33",
                            "--no-vsync", "--report", report});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue r;
  REQUIRE_MESSAGE(read_json_file(report, r), run.output);
  CHECK(text_at(r, {"format"}) == "engine.view.report");
  CHECK(number_at(r, {"exit", "code"}) == 0.0);
  CHECK(text_at(r, {"exit", "reason"}) == "frames");
  CHECK(!text_at(r, {"machine", "adapter"}).empty());
  CHECK(!text_at(r, {"machine", "driver"}).empty());
  CHECK(!text_at(r, {"machine", "tier"}).empty());
  CHECK(!text_at(r, {"machine", "raster"}).empty());
  CHECK(!text_at(r, {"machine", "os"}).empty());
  CHECK(!text_at(r, {"machine", "cpu_baseline"}).empty());
  // Seven frame-to-frame times from eight frames, each one somewhere in the spread.
  CHECK(number_at(r, {"frames", "count"}) == 7.0);
  CHECK(number_at(r, {"frames", "median_ms"}) > 0.0);
  CHECK(number_at(r, {"frames", "p99_ms"}) >= number_at(r, {"frames", "median_ms"}));
  CHECK(number_at(r, {"frames", "worst_ms"}) >= number_at(r, {"frames", "p95_ms"}) - 0.1);
  // The summary is the line the run printed, parsed: the same frames.
  CHECK(number_at(r, {"summary", "frames"}) == 8.0);
  MESSAGE("report: " << write_json(r, JsonWriteOptions{.pretty = false}).substr(0, 600));
}

TEST_CASE("engine-view: --report is written when the run fails, with the failure") {
  const test::TempDir tmp("engine_view_report_fail");
  const std::string report = tmp.file("report.json");
  const Run run = run_view({"--offscreen", "--scene", tmp.file("no-such-scene.json"), "--frames",
                            "2", "--report", report});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code != 0, run.output);
  JsonValue r;
  REQUIRE_MESSAGE(read_json_file(report, r), run.output);
  CHECK(number_at(r, {"exit", "code"}) == static_cast<f64>(run.exit_code));
  CHECK(text_at(r, {"exit", "reason"}) == "error");
  CHECK(text_at(r, {"exit", "error"}).find("scene") == 0);
  CHECK(at(r, {"summary"}) != nullptr);
  CHECK(at(r, {"summary"})->is_null());
  CHECK(!text_at(r, {"machine", "os"}).empty());
}

// ---- --game -------------------------------------------------------------------------------------
#if ENGINE_VIEW_GAME && defined(WALK_POC_PATH)

namespace {

#ifndef ENGINE_VIEW_TESTS_DUNES
#define ENGINE_VIEW_TESTS_DUNES 0
#endif
bool refused_without_dunes(const Run& run) {
  return ENGINE_VIEW_TESTS_DUNES == 0 && run.exit_code == 1 &&
         run.output.find("which this build does not have") != std::string::npos;
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

// The fixture, copied into the case's scratch directory: the host's per-load copies go beside it,
// and a case owns nothing outside its own directory.
std::string fixture(const test::TempDir& tmp, const char* built) {
  const std::filesystem::path from(built);
  const std::string to = tmp.file("walk_poc" + from.extension().string());
  std::error_code ec;
  std::filesystem::copy_file(from, std::filesystem::path(to),
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_MESSAGE(!ec, "copying " << built << ": " << ec.message());
  return to;
}

// The committed erg's sand over a 128 m window of it (the walk cases' reason: a Debug engine-view
// takes minutes to build the whole erg's grid), or empty when this bundle has no erg.
std::string erg_window(const test::TempDir& tmp) {
  const std::string committed =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-erg/scene.json",
                      "content/test-scenes/desert-erg/scene.json");
  if (!test::path_exists(committed)) {
    MESSAGE("the erg is not in this bundle");
    return {};
  }
  std::string source;
  REQUIRE(io::read_file(committed, source) == io::Status::Ok);
  JsonValue erg;
  REQUIRE(parse_json(source, erg).ok);
  JsonValue terrain = *erg.find("terrain");
  terrain.set("size", static_cast<u64>(129));
  terrain.set("extent", JsonValue(128.0));
  erg.set("terrain", std::move(terrain));
  const std::string scene = tmp.file("erg.json");
  REQUIRE(io::write_file(scene, write_json(erg)) == io::Status::Ok);
  // The scene names its camera path beside it.
  std::string path_text;
  REQUIRE(io::read_file(committed.substr(0, committed.size() - std::string("scene.json").size()) +
                            "camera-path.json",
                        path_text) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("camera-path.json"), path_text) == io::Status::Ok);
  return scene;
}

// A walk to replay: walking from the start, W held from tick 10, E pressed at tick 100, `ticks`
// long, against the map's latest revision (the one with the game actions).
std::string walk_session(const test::TempDir& tmp, const std::string& scene, u64 ticks,
                         const char* name = "walk.jsonl") {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  const auto key = [&](u64 tick, u32 code, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0});
  };
  key(10, 26, true);  // W
  key(100, 8, true);  // E: interact, while walking
  key(101, 8, false);
  view::SessionHeader header;
  header.ticks = ticks;
  header.scene = scene;
  header.start.position = WorldPos{20.0, 40.0, 10.0};
  header.start.yaw = 0.5f;
  header.start.pitch = -0.2f;
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  const std::string path = tmp.file(name);
  REQUIRE(keys.save(path) == io::Status::Ok);
  return path;
}

const JsonValue* game_of(const JsonValue& summary) { return at(summary, {"interactive", "game"}); }

}  // namespace

TEST_CASE("engine-view: --game ticks the fixture every tick of a walk and reports it") {
  const test::TempDir tmp("engine_view_game");
  const std::string scene = erg_window(tmp);
  if (scene.empty()) return;
  const std::string session = walk_session(tmp, scene, 480);  // 120 frames at four ticks a frame
  const std::string report = tmp.file("report.json");
  const Run run = run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--game",
                            fixture(tmp, WALK_POC_PATH), "--report", report, "--width", "160",
                            "--height", "96", "--ddc", tmp.file("ddc")});
  if (run.exit_code == 3 || refused_without_dunes(run)) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* game = game_of(summary);
  REQUIRE_MESSAGE(game != nullptr, run.output);
  CHECK(number_at(*game, {"ticks"}) == 480.0);  // every tick, once
  CHECK(number_at(*game, {"last_tick"}) == 480.0);
  CHECK(text_at(*game, {"name"}) == "walk_poc");
  CHECK(text_at(*game, {"error"}).empty());
  // It walked, and the module saw it walk: its metres are the walker's.
  const f64 walked = number_at(*game, {"state", "walked_m"});
  const f64 walker = number_at(summary, {"interactive", "walk", "distance_m"});
  CHECK(walked > 1.0);
  CHECK(walked == doctest::Approx(walker).epsilon(1e-6));
  CHECK(number_at(*game, {"state", "thirst"}) < 100.0);
  CHECK(number_at(*game, {"state", "seconds"}) == doctest::Approx(2.0));
  CHECK(number_at(*game, {"state", "interactions"}) == 1.0);  // E, once, while walking
  const JsonValue* meters = at(*game, {"hud", "meters"});
  REQUIRE(meters != nullptr);
  CHECK(meters->size() == 1);
  // The report carries the game and the walk beside the machine.
  JsonValue r;
  REQUIRE(read_json_file(report, r));
  CHECK(text_at(r, {"exit", "reason"}) == "session");
  CHECK(number_at(r, {"game", "ticks"}) == 480.0);
  CHECK(number_at(r, {"walk", "distance_m"}) == doctest::Approx(walker).epsilon(1e-6));
  CHECK(number_at(r, {"walk", "seconds"}) > 1.0);
  MESSAGE("120 frames of walking: " << write_json(*at(*game, {"state"})) << " HUD "
                                    << write_json(*at(*game, {"hud"})));
}

TEST_CASE("engine-view: --game draws its HUD over the picture, top left, and nowhere else") {
  // The same replay captured with the game and without: the HUD's panel is the only difference,
  // and it is in the top-left quarter (renderer.md "The overlay"; game_hud.h).
  const test::TempDir tmp("engine_view_game_hud");
  const std::string scene = erg_window(tmp);
  if (scene.empty()) return;
  const std::string session = walk_session(tmp, scene, 120);
  const std::string with = tmp.file("with.png");
  const std::string without = tmp.file("without.png");
  const std::vector<std::string> common = {
      "--replay-input", session, "--scene", scene,          "--offscreen", "--width", "800",
      "--height",       "450",   "--ddc",   tmp.file("ddc")};
  std::vector<std::string> args = common;
  args.insert(args.end(), {"--game", fixture(tmp, WALK_POC_PATH), "--capture", with});
  const Run game = run_view(args);
  if (game.exit_code == 3 || refused_without_dunes(game)) return;
  REQUIRE_MESSAGE(game.exit_code == 0, game.output);
  args = common;
  args.insert(args.end(), {"--capture", without});
  const Run plain = run_view(args);
  REQUIRE_MESSAGE(plain.exit_code == 0, plain.output);
  image::Image a;
  image::Image b;
  REQUIRE(image::read_image(with, a) == io::Status::Ok);
  REQUIRE(image::read_image(without, b) == io::Status::Ok);
  REQUIRE(a.width == b.width);
  REQUIRE(a.height == b.height);
  u64 top_left = 0;
  u64 elsewhere = 0;
  for (u32 y = 0; y < a.height; ++y) {
    for (u32 x = 0; x < a.width; ++x) {
      const u32 i = (y * a.width + x) * 4;
      const bool differs = std::memcmp(&a.pixels[i], &b.pixels[i], 3) != 0;
      if (!differs) continue;
      if (x < a.width / 2 && y < a.height / 2) {
        ++top_left;
      } else {
        ++elsewhere;
      }
    }
  }
  MESSAGE("the HUD changed " << top_left << " pixels in the top-left quarter, " << elsewhere
                             << " elsewhere");
  CHECK(top_left > 2000);          // a panel with a meter and three lines of text
  CHECK(elsewhere == 0);           // the rest of the picture is the same picture, to the byte
  if (elsewhere != 0) tmp.keep();  // the two pictures, for a human to look at
}

TEST_CASE("engine-view: a --game run that drains the meter ends through end_run") {
  const test::TempDir tmp("engine_view_game_end");
  const std::string scene = erg_window(tmp);
  if (scene.empty()) return;
  const std::string session = walk_session(tmp, scene, 2400);  // ten seconds, 600 frames at most
  const std::string state = tmp.file("state.json");
  // A saved run with a quarter of a second's walking left in it.
  REQUIRE(io::write_file(state, R"({"format":"engine.game.state","version":1,"module":"walk_poc",
      "type":"engine.walk_poc.Survivor","state":{"thirst":0.0625,"seconds":30,"walked_m":40}})") ==
          io::Status::Ok);
  const std::string report = tmp.file("report.json");
  const Run run = run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--game",
                            fixture(tmp, WALK_POC_PATH), "--game-state", state, "--report", report,
                            "--width", "160", "--height", "96", "--ddc", tmp.file("ddc")});
  if (run.exit_code == 3 || refused_without_dunes(run)) return;
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* game = game_of(summary);
  REQUIRE(game != nullptr);
  CHECK(at(*game, {"ended"})->as_bool());
  CHECK(at(*game, {"restored"})->as_bool());
  CHECK(number_at(*game, {"result", "seconds"}) > 30.0);
  CHECK(number_at(*game, {"result", "distance"}) > 40.0);
  CHECK(number_at(*game, {"result", "days"}) == 0.0);
  // The session stopped at the frame the game ended it, long before the recording's end.
  CHECK(number_at(summary, {"frames"}) < 600.0);
  JsonValue r;
  REQUIRE(read_json_file(report, r));
  CHECK(text_at(r, {"exit", "reason"}) == "end_run");
  CHECK(number_at(r, {"game", "result", "seconds"}) > 30.0);
  // And the state file now holds the ended run.
  JsonValue saved;
  REQUIRE(read_json_file(state, saved));
  CHECK(at(saved, {"state", "over"})->as_bool());
  CHECK(number_at(saved, {"state", "seconds"}) ==
        doctest::Approx(number_at(*game, {"result", "seconds"})).epsilon(1e-4));
}

TEST_CASE("engine-view: --game-state leaves a run and resumes it") {
  const test::TempDir tmp("engine_view_game_state");
  const std::string scene = erg_window(tmp);
  if (scene.empty()) return;
  const std::string session = walk_session(tmp, scene, 240);
  const std::string state = tmp.file("state.json");
  const std::vector<std::string> args = {"--replay-input",
                                         session,
                                         "--scene",
                                         scene,
                                         "--offscreen",
                                         "--game",
                                         fixture(tmp, WALK_POC_PATH),
                                         "--game-state",
                                         state,
                                         "--width",
                                         "160",
                                         "--height",
                                         "96",
                                         "--ddc",
                                         tmp.file("ddc")};
  const Run first = run_view(args);
  if (first.exit_code == 3 || refused_without_dunes(first)) return;
  REQUIRE_MESSAGE(first.exit_code == 0, first.output);
  JsonValue one;
  REQUIRE(summary_of(first, one));
  REQUIRE(game_of(one) != nullptr);
  CHECK_FALSE(at(*game_of(one), {"restored"})->as_bool());
  CHECK(test::path_exists(state));
  const Run second = run_view(args);
  REQUIRE_MESSAGE(second.exit_code == 0, second.output);
  JsonValue two;
  REQUIRE(summary_of(second, two));
  REQUIRE(game_of(two) != nullptr);
  CHECK(at(*game_of(two), {"restored"})->as_bool());
  // The same second walked again, on top of the first.
  CHECK(number_at(*game_of(two), {"state", "seconds"}) ==
        doctest::Approx(2.0 * number_at(*game_of(one), {"state", "seconds"})));
  CHECK(number_at(*game_of(two), {"state", "walked_m"}) ==
        doctest::Approx(2.0 * number_at(*game_of(one), {"state", "walked_m"})));
}

TEST_CASE("engine-view: --game refuses a module the host refuses, before any device") {
  const test::TempDir tmp("engine_view_game_refused");
  const std::string scene = erg_window(tmp);
  if (scene.empty()) return;
  const std::string session = walk_session(tmp, scene, 40);
  const std::string report = tmp.file("report.json");
  const Run run = run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--game",
                            fixture(tmp, WALK_POC_BAD_ACTION_PATH), "--report", report});
  CHECK(run.exit_code == 1);
  CHECK(run.output.find("no game action 'dance'") != std::string::npos);
  JsonValue r;
  REQUIRE(read_json_file(report, r));
  CHECK(text_at(r, {"exit", "error"}).find("game: the module host refused") == 0);
  CHECK_FALSE(at(r, {"machine", "device"})->as_bool());  // refused before a device was made
  // And --game-state alone names what it is missing.
  const Run alone = run_view({"--game-state", tmp.file("s.json"), "--offscreen"});
  CHECK(alone.exit_code == 2);
}

TEST_CASE("engine-view: a module that fails from game_tick ends the session with its line") {
  const test::TempDir tmp("engine_view_game_failing");
  const std::string scene = erg_window(tmp);
  if (scene.empty()) return;
  const std::string session = walk_session(tmp, scene, 240);
  const std::string report = tmp.file("report.json");
  const Run run = run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--game",
                            fixture(tmp, WALK_POC_FAILING_PATH), "--report", report, "--width",
                            "160", "--height", "96", "--ddc", tmp.file("ddc")});
  if (run.exit_code == 3 || refused_without_dunes(run)) return;
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);  // the session ended cleanly; the game did not
  JsonValue summary;
  REQUIRE(summary_of(run, summary));
  const JsonValue* game = game_of(summary);
  REQUIRE(game != nullptr);
  CHECK(number_at(*game, {"ticks"}) == 30.0);
  CHECK(text_at(*game, {"error"}).find("game_tick failed at tick 30") == 0);
  CHECK(number_at(summary, {"frames"}) == 8.0);  // tick 30 is in the eighth frame of four ticks
  JsonValue r;
  REQUIRE(read_json_file(report, r));
  CHECK(text_at(r, {"exit", "reason"}) == "game_error");
  CHECK(text_at(r, {"exit", "error"}).find("game: game_tick failed at tick 30") == 0);
}

TEST_CASE("engine-view: a live --game walk replays to the same module state") {
  // A window on the test's desktop, walking from an injected log and recording itself; then the
  // recording replayed offscreen. The module's inputs are the ticks', so its state after the
  // replay is the live session's, to the bit.
  const test::TempDir tmp("engine_view_game_replay");
  const std::string scene = erg_window(tmp);
  if (scene.empty()) return;
  const std::string inject = walk_session(tmp, scene, 600, "inject.jsonl");
  const std::string recorded = tmp.file("recorded.jsonl");
  const std::string module = fixture(tmp, WALK_POC_PATH);
  const std::string ddc = tmp.file("ddc");
  const Run live = run_view({"--interactive",
                             "--walk",
                             "--start",
                             "20,40,10",
                             "28.6,-11.5",
                             "--inject-input",
                             inject,
                             "--record-input",
                             recorded,
                             "--scene",
                             scene,
                             "--game",
                             module,
                             "--width",
                             "160",
                             "--height",
                             "96",
                             "--no-vsync",
                             "--ddc",
                             ddc});
  if (live.exit_code == 3 || refused_without_dunes(live)) {
    MESSAGE("engine-view unavailable here: " << live.output);
    return;
  }
  REQUIRE_MESSAGE(live.exit_code == 0, live.output);
  JsonValue a;
  REQUIRE_MESSAGE(summary_of(live, a), live.output);
  const Run replay = run_view({"--replay-input", recorded, "--offscreen", "--game", module,
                               "--width", "160", "--height", "96", "--ddc", ddc});
  REQUIRE_MESSAGE(replay.exit_code == 0, replay.output);
  JsonValue b;
  REQUIRE_MESSAGE(summary_of(replay, b), replay.output);
  REQUIRE(game_of(a) != nullptr);
  REQUIRE(game_of(b) != nullptr);
  const std::string live_state = write_json(*at(*game_of(a), {"state"}));
  const std::string replay_state = write_json(*at(*game_of(b), {"state"}));
  CHECK(number_at(*game_of(a), {"ticks"}) == number_at(*game_of(b), {"ticks"}));
  CHECK(number_at(*game_of(a), {"state", "walked_m"}) > 1.0);
  CHECK(live_state == replay_state);
  REQUIRE(at(a, {"interactive", "trajectory", "hash"}) != nullptr);
  REQUIRE(at(b, {"interactive", "trajectory", "hash"}) != nullptr);
  CHECK(write_json(*at(a, {"interactive", "trajectory", "hash"})) ==
        write_json(*at(b, {"interactive", "trajectory", "hash"})));
  MESSAGE("live and replayed: " << live_state);
}

#endif  // ENGINE_VIEW_GAME && WALK_POC_PATH

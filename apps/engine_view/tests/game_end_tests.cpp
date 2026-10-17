// End to end: how a `--game` run ends and what clock its frames are drawn by
// (docs/subsystems/apps.md,
// "--game: running a game module", "Ending"; renderer.md "One clock"; roadmap P44). The drawn sun
// follows the runner's clock — a recording in which `]` was pressed replays offscreen to a sky the
// no-key replay does not draw, and the summary's clock is the sky's — and a live session whose
// module called `end_run` stays on its result panel until Enter or Escape, or starts a new run on
// R. Every run is offscreen or on the test's own desktop (view_process.h).

#include "../fly_camera.h"
#include "../game_runner.h"
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

#ifndef ENGINE_VIEW_TESTS_DUNES
#define ENGINE_VIEW_TESTS_DUNES 0
#endif
bool refused_without_dunes(const Run& run) {
  return ENGINE_VIEW_TESTS_DUNES == 0 && run.exit_code == 1 &&
         run.output.find("which this build does not have") != std::string::npos;
}

bool read_json_file(const std::string& path, JsonValue& out) {
  std::string text;
  return io::read_file(path, text) == io::Status::Ok && parse_json(text, out).ok;
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

std::string text_at(const JsonValue& v, std::initializer_list<const char*> path) {
  const JsonValue* p = at(v, path);
  return p != nullptr && p->is_string() ? std::string(p->as_string()) : std::string();
}

// The fixture, copied into the case's scratch directory (a case owns nothing outside it).
std::string fixture(const test::TempDir& tmp, const char* built) {
  const std::filesystem::path from(built);
  const std::string to = tmp.file("walk_poc" + from.extension().string());
  std::error_code ec;
  std::filesystem::copy_file(from, std::filesystem::path(to),
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_MESSAGE(!ec, "copying " << built << ": " << ec.message());
  return to;
}

// The committed erg's sand and sky over a 128 m window of it (game_view_tests.cpp's reason: a
// Debug engine-view takes minutes to build the whole erg's grid), or empty when this bundle has
// no erg.
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
  std::string path_text;
  REQUIRE(io::read_file(committed.substr(0, committed.size() - std::string("scene.json").size()) +
                            "camera-path.json",
                        path_text) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("camera-path.json"), path_text) == io::Status::Ok);
  return scene;
}

struct KeyPress {
  u64 tick;
  u32 code;  // an SDL scancode (docs/subsystems/input.md)
};
constexpr u32 k_w = 26;
constexpr u32 k_right_bracket = 48;  // `]`: sun_faster (fly_camera.h)

// A walk to replay: W held from tick 10, plus `presses`, each down on its tick and up on the next.
std::string walk_session(const test::TempDir& tmp, const std::string& scene, u64 ticks,
                         const char* name, std::initializer_list<KeyPress> presses) {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  const auto key = [&](u64 tick, u32 code, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0});
  };
  key(10, k_w, true);
  for (const KeyPress& p : presses) {
    key(p.tick, p.code, true);
    key(p.tick + 1, p.code, false);
  }
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

u64 differing_pixels(const std::string& a_path, const std::string& b_path) {
  image::Image a;
  image::Image b;
  REQUIRE(image::read_image(a_path, a) == io::Status::Ok);
  REQUIRE(image::read_image(b_path, b) == io::Status::Ok);
  REQUIRE(a.width == b.width);
  REQUIRE(a.height == b.height);
  u64 n = 0;
  for (u32 p = 0; p < a.width * a.height; ++p)
    n += std::memcmp(&a.pixels[p * 4], &b.pixels[p * 4], 3) != 0 ? 1u : 0u;
  return n;
}

}  // namespace

TEST_CASE("engine-view: a --game replay draws the sky where the runner's clock stands") {
  // Two recordings of the same second's walk, one with `]` pressed at tick 20. Replayed offscreen
  // with the fixture at a sun rate of 3,600, the press steps the runner's clock to 8,640 for the
  // remaining 220 ticks — 77 minutes of sun more than the other — and the frame is drawn from that
  // clock: the sky the two captures draw differs, and each summary's sky time (the renderer's,
  // `sun.sky.time_s`) is the runner's `clock.world_time_s` to the bit.
  const test::TempDir tmp("engine_view_game_clock");
  const std::string scene = erg_window(tmp);
  if (scene.empty()) return;
  const std::string pressed =
      walk_session(tmp, scene, 240, "pressed.jsonl", {KeyPress{20, k_right_bracket}});
  const std::string plain = walk_session(tmp, scene, 240, "plain.jsonl", {});
  const std::string module = fixture(tmp, WALK_POC_PATH);
  const auto replay = [&](const std::string& session, const std::string& capture) {
    return run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--game", module,
                     "--sun-rate", "3600", "--capture", capture, "--width", "160", "--height", "96",
                     "--ddc", tmp.file("ddc")});
  };
  const Run a = replay(pressed, tmp.file("pressed.png"));
  if (a.exit_code == 3 || refused_without_dunes(a)) {
    MESSAGE("engine-view unavailable here: " << a.output);
    return;
  }
  REQUIRE_MESSAGE(a.exit_code == 0, a.output);
  const Run b = replay(plain, tmp.file("plain.png"));
  REQUIRE_MESSAGE(b.exit_code == 0, b.output);
  JsonValue sa;
  JsonValue sb;
  REQUIRE_MESSAGE(summary_of(a, sa), a.output);
  REQUIRE_MESSAGE(summary_of(b, sb), b.output);
  // The press reached the runner's clock, at the tick it was pressed on.
  CHECK(number_at(sa, {"interactive", "game", "clock", "rate_changes"}) == 1.0);
  CHECK(number_at(sa, {"interactive", "game", "clock", "sun_rate"}) == 8640.0);
  CHECK(number_at(sb, {"interactive", "game", "clock", "rate_changes"}) == 0.0);
  CHECK(number_at(sb, {"interactive", "game", "clock", "sun_rate"}) == 3600.0);
  // The press is acted on at the tick it was pressed on, before that tick's step: 19 ticks at
  // 3,600 and 221 at 8,640, against 240 at 3,600 — 8,241 against 3,600 game seconds.
  const f64 day_a = number_at(sa, {"sun", "time_s"});
  const f64 day_b = number_at(sb, {"sun", "time_s"});
  CHECK(day_a == doctest::Approx(8241.0).epsilon(1e-9));
  CHECK(day_b == doctest::Approx(3600.0).epsilon(1e-9));
  // The frame was drawn from the runner's clock: the sky the renderer evaluated stood at the
  // ground's time plus the runner's sun time, which is the runner's world time.
  const f64 sky_a = number_at(sa, {"sun", "sky", "time_s"});
  const f64 sky_b = number_at(sb, {"sun", "sky", "time_s"});
  REQUIRE_MESSAGE(sky_a > 0.0, "the erg window has a sky: " << write_json(*at(sa, {"sun"})));
  CHECK(sky_a == number_at(sa, {"interactive", "game", "clock", "world_time_s"}));
  CHECK(sky_b == number_at(sb, {"interactive", "game", "clock", "world_time_s"}));
  CHECK(sky_a - sky_b == doctest::Approx(8241.0 - 3600.0).epsilon(1e-9));
  // And the two captures, drawn where each replay ended, show different skies.
  const u64 differ = differing_pixels(tmp.file("pressed.png"), tmp.file("plain.png"));
  MESSAGE("the press moved the sun by 77 minutes: " << differ << " of " << 160 * 96
                                                    << " pixels differ");
  CHECK(differ > 500);
  CHECK(text_at(sa, {"interactive", "game", "error"}).empty());
}

// ---- the run's end
// -------------------------------------------------------------------------------

namespace {

// A saved run with a quarter of a second's walking left in it: the fixture ends it 60 ticks after
// W goes down.
std::string nearly_over(const test::TempDir& tmp) {
  const std::string state = tmp.file("state.json");
  REQUIRE(io::write_file(state, R"({"format":"engine.game.state","version":1,"module":"walk_poc",
      "type":"engine.walk_poc.Survivor","state":{"thirst":0.0625,"seconds":30,"walked_m":40}})") ==
          io::Status::Ok);
  return state;
}

f64 state_number(const view::GameRunner& runner, const char* field) {
  const JsonValue state = runner.state_json();
  const JsonValue* v = state.find(field);
  f64 out = -1.0;
  return v != nullptr && v->get_f64(out) ? out : -1.0;
}

}  // namespace

TEST_CASE("game runner: a new run puts the state back at its defaults and keeps the result") {
  // No GPU and no window: the fixture loaded by the runner from a run nearly out of water, walked
  // to its end, then `new_run` (what the result panel's R does): the state is the descriptor's
  // defaults again in the same object, the ended run's result is kept in `results`, the run is
  // counted, and the next second of walking drains from 100.
  if (!test::bundle_root().empty()) {
    MESSAGE("game modules are not part of a test bundle; skipped");
    return;
  }
  const test::TempDir tmp("game_runner_new_run");
  const input::ActionMap map = view::default_fly_map();
  view::GameRunner::Config config;
  config.module = fixture(tmp, WALK_POC_PATH);
  config.shadow_directory = tmp.file("shadow");
  config.state_file = nearly_over(tmp);
  view::GameRunner runner;
  std::string error;
  REQUIRE_MESSAGE(runner.start(config, map, 240, &error), error);
  CHECK_FALSE(runner.new_run());  // nothing has ended
  u64 tick = 0;
  const auto walk = [&](u32 n) {
    for (u32 i = 0; i < n; ++i) {
      view::GameTickInput in;
      in.tick = ++tick;
      in.feet = WorldPos{0.0, 0.0, -static_cast<f64>(tick) * 1.5 / 240.0};
      in.eye = WorldPos{in.feet.x, 1.65, in.feet.z};
      in.walking = true;
      in.on_ground = true;
      in.moved_m = 1.5 / 240.0;
      runner.tick(in);
    }
  };
  walk(240);
  REQUIRE(runner.ended());
  CHECK_FALSE(runner.failed());
  CHECK(runner.runs() == 1);
  const u64 ended_at = runner.ticks();
  const f64 seconds = state_number(runner, "seconds");
  CHECK(seconds > 30.0);
  REQUIRE(runner.result().is_object());
  // The clock stepped on through the ticks after the end, the module did not.
  CHECK(runner.ticks() < 240);
  CHECK(runner.day_s() == doctest::Approx(0.0));  // no rate was set: still

  REQUIRE(runner.new_run());
  CHECK_FALSE(runner.ended());
  CHECK(runner.runs() == 2);
  CHECK(runner.hud().empty());
  CHECK(state_number(runner, "thirst") == 100.0);
  CHECK(state_number(runner, "seconds") == 0.0);
  CHECK(state_number(runner, "walked_m") == 0.0);
  JsonValue summary = runner.summary_json();
  REQUIRE(summary.find("results") != nullptr);
  CHECK(summary.find("results")->size() == 1);
  CHECK(number_at(summary, {"runs"}) == 2.0);
  CHECK(number_at((*summary.find("results"))[0u], {"seconds"}) == doctest::Approx(seconds));
  walk(240);
  CHECK(runner.ticks() == ended_at + 240);  // ticked again
  CHECK_FALSE(runner.ended());
  CHECK(state_number(runner, "thirst") == doctest::Approx(99.75).epsilon(1e-9));
  CHECK(state_number(runner, "seconds") == doctest::Approx(1.0).epsilon(1e-9));
}

namespace {

// A live window on the test's desktop, walking from an injected log with `--on-end panel`: W held
// from tick 10, `code` pressed at tick 200 (the run has ended by tick 70), the session's log
// `ticks` long.
Run injected_end(const test::TempDir& tmp, const std::string& scene, u32 code, u64 ticks,
                 const std::string& report) {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  const auto key = [&](u64 tick, u32 c, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, c, down ? 1.0f : 0.0f, 0});
  };
  key(10, k_w, true);
  key(200, code, true);
  key(201, code, false);
  view::SessionHeader header;
  header.ticks = ticks;
  keys.set_session(view::session_to_json(header));
  const std::string inject = tmp.file("inject.jsonl");
  REQUIRE(keys.save(inject) == io::Status::Ok);
  return run_view({"--interactive",
                   "--walk",
                   "--start",
                   "20,40,10",
                   "28.6,-11.5",
                   "--inject-input",
                   inject,
                   "--scene",
                   scene,
                   "--game",
                   fixture(tmp, WALK_POC_PATH),
                   "--game-state",
                   nearly_over(tmp),
                   "--on-end",
                   "panel",
                   "--report",
                   report,
                   "--width",
                   "160",
                   "--height",
                   "96",
                   "--no-vsync",
                   "--ddc",
                   tmp.file("ddc")});
}

constexpr u32 k_r = 21;
constexpr u32 k_enter = 40;

}  // namespace

TEST_CASE("engine-view: a live --game window stays on the result panel until Enter") {
  const test::TempDir tmp("engine_view_game_panel_enter");
  const std::string scene = erg_window(tmp);
  if (scene.empty()) return;
  const std::string report = tmp.file("report.json");
  // 480 ticks of log; the run ends about tick 70, Enter at tick 200 ends the session there.
  const Run run = injected_end(tmp, scene, k_enter, 480, report);
  if (run.exit_code == 3 || refused_without_dunes(run)) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* game = at(summary, {"interactive", "game"});
  REQUIRE(game != nullptr);
  CHECK(at(*game, {"ended"})->as_bool());
  CHECK(number_at(*game, {"runs"}) == 1.0);
  CHECK(number_at(*game, {"result", "seconds"}) > 30.0);
  // The session went on past the run's end — ticks ran, frames were drawn — and stopped at Enter,
  // well short of the log's end: the walker's seconds are the session's ticks at 240 Hz.
  JsonValue r;
  REQUIRE(read_json_file(report, r));
  const f64 walked_s = number_at(r, {"walk", "seconds"});
  MESSAGE("the run ended at module tick " << number_at(*game, {"last_tick"}) << "; the session at "
                                          << walked_s * 240.0 << " of 480 ticks, "
                                          << number_at(summary, {"frames"}) << " frames");
  CHECK(walked_s >= 200.0 / 240.0);
  CHECK(walked_s < 400.0 / 240.0);
  CHECK(number_at(*game, {"last_tick"}) < 100.0);
  CHECK(run.output.find("the run is over; Enter or Esc quits") != std::string::npos);
  CHECK(text_at(r, {"exit", "reason"}) == "end_run");
  CHECK(number_at(r, {"game", "runs"}) == 1.0);
}

TEST_CASE("engine-view: R on the result panel starts a new run from the defaults") {
  const test::TempDir tmp("engine_view_game_panel_r");
  const std::string scene = erg_window(tmp);
  if (scene.empty()) return;
  const std::string report = tmp.file("report.json");
  // The run ends about tick 70; R at tick 200 starts the second, which walks (W is still held) to
  // the log's end at 480: 280 ticks, a bit over a second, 0.29 of thirst.
  const Run run = injected_end(tmp, scene, k_r, 480, report);
  if (run.exit_code == 3 || refused_without_dunes(run)) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* game = at(summary, {"interactive", "game"});
  REQUIRE(game != nullptr);
  CHECK_FALSE(at(*game, {"ended"})->as_bool());
  CHECK(number_at(*game, {"runs"}) == 2.0);
  const JsonValue* results = at(*game, {"results"});
  REQUIRE(results != nullptr);
  REQUIRE(results->size() == 1);
  CHECK(number_at((*results)[0u], {"seconds"}) > 30.0);
  CHECK(at(*game, {"result"})->is_null());  // the second run has not ended
  const f64 thirst = number_at(*game, {"state", "thirst"});
  MESSAGE("the second run after " << number_at(*game, {"state", "seconds"}) << " s: thirst "
                                  << thirst << ", HUD " << write_json(*at(*game, {"hud"})));
  CHECK(thirst > 99.0);
  CHECK(thirst < 100.0);
  CHECK(number_at(*game, {"state", "seconds"}) > 1.0);
  CHECK(number_at(*game, {"state", "seconds"}) < 1.3);
  CHECK(run.output.find("run 2 started") != std::string::npos);
  JsonValue r;
  REQUIRE(read_json_file(report, r));
  CHECK(number_at(r, {"walk", "seconds"}) == doctest::Approx(2.0));  // to the log's end, 480 ticks
  CHECK(text_at(r, {"exit", "reason"}) == "session");
  CHECK(number_at(r, {"game", "runs"}) == 2.0);
}

#endif  // ENGINE_VIEW_GAME && WALK_POC_PATH

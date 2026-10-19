// The game runner with no GPU and no window (game_runner.h; docs/subsystems/apps.md "--game:
// running a game module"): the fixture module game/walk_poc loaded through the module host and
// ticked by hand, its HUD and its thirst, a rebuild of it — a state field added — swapped in under
// a running session with the meter carried, its refusals, its export failure, `end_run`, the state
// file's round trip and the water at a placement. The end-to-end cases (game_view_tests.cpp) run
// the same module from engine-view's session.

#include "../game_runner.h"

#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/input/input.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

using namespace engine;

#if defined(WALK_POC_PATH)

namespace {

bool in_bundle() {
  if (test::bundle_root().empty()) return false;
  MESSAGE("game runner: game modules and the compiler are not part of a test bundle; skipped");
  return true;
}

// A built module copied into the test's scratch directory (a test owns nothing outside it), its
// modification time moved on by `seconds` so a poll sees a change inside one tick of the clock
// (the same helper as foundation/hotload's tests).
std::string place(const test::TempDir& dir, const char* built, std::string_view name,
                  int seconds = 0) {
  const std::filesystem::path from(built);
  const std::string to = dir.file(std::string(name) + from.extension().string());
  std::error_code ec;
  std::filesystem::copy_file(from, std::filesystem::path(to),
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_MESSAGE(!ec, "copying " << built << ": " << ec.message());
  std::filesystem::last_write_time(
      std::filesystem::path(to),
      std::filesystem::file_time_type::clock::now() + std::chrono::seconds(seconds), ec);
  return to;
}

std::string read_text(const std::string& path) {
  std::string out;
  (void)io::read_file(path, out);
  return out;
}

void write_text(const std::string& path, std::string_view text) {
  std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

f64 state_number(const view::GameRunner& runner, const char* field) {
  const JsonValue state = runner.state_json();
  const JsonValue* v = state.find(field);
  f64 out = -1.0;
  return v != nullptr && v->get_f64(out) ? out : -1.0;
}

// `n` ticks of walking north at 1.5 m/s from `from`, the walker on the ground.
void walk(view::GameRunner& runner, u64& tick, u32 n, f64 step_m = 1.5 / 240.0,
          const input::InputState* input = nullptr, WorldPos from = WorldPos{0.0, 0.0, 0.0}) {
  for (u32 i = 0; i < n; ++i) {
    view::GameTickInput in;
    in.tick = ++tick;
    in.feet = WorldPos{from.x, from.y, from.z - static_cast<f64>(tick) * step_m};
    in.eye = WorldPos{in.feet.x, in.feet.y + 1.65, in.feet.z};
    in.walking = true;
    in.on_ground = true;
    in.moved_m = step_m;
    in.input = input;
    runner.tick(in);
  }
}

view::GameRunner::Config config_for(const std::string& module, const test::TempDir& tmp) {
  view::GameRunner::Config c;
  c.module = module;
  c.shadow_directory = tmp.file("shadow");
  return c;
}

}  // namespace

TEST_CASE("game runner: the fixture ticks, drains thirst while walking and says its HUD") {
  if (in_bundle()) return;
  const test::TempDir tmp("game_runner_tick");
  const input::ActionMap map = view::default_fly_map();
  view::GameRunner runner;
  std::string error;
  REQUIRE_MESSAGE(
      runner.start(config_for(place(tmp, WALK_POC_PATH, "walk_poc"), tmp), map, 240, &error),
      error);
  u64 tick = 0;
  walk(runner, tick, 240);  // one second of walking
  CHECK(runner.ticks() == 240);
  CHECK_FALSE(runner.ended());
  // 0.25 a second of walking, from 100.
  CHECK(state_number(runner, "thirst") == doctest::Approx(99.75).epsilon(1e-9));
  CHECK(state_number(runner, "seconds") == doctest::Approx(1.0).epsilon(1e-9));
  CHECK(state_number(runner, "walked_m") == doctest::Approx(1.5).epsilon(1e-9));
  const view::GameHud& hud = runner.hud();
  REQUIRE(hud.meter_count == 1);
  CHECK(std::string_view(hud.meters[0].name) == "thirst");
  CHECK(hud.meters[0].value == doctest::Approx(99.75));
  CHECK(hud.meters[0].max == 100.0);
  CHECK(hud.lines[0].set);
  CHECK(std::string_view(hud.lines[0].text).find("day 1") == 0);
  CHECK(hud.tick == 240);
  const JsonValue summary = runner.summary_json();
  REQUIRE(summary.find("actions") != nullptr);
  CHECK(summary.find("actions")->size() == 2);  // interact and use
  MESSAGE(
      "after a second: " << write_json(summary.find("hud") ? *summary.find("hud") : JsonValue()));
}

TEST_CASE("game runner: interact is a game action the module reads on the tick it was pressed") {
  if (in_bundle()) return;
  const test::TempDir tmp("game_runner_interact");
  const input::ActionMap map = view::default_fly_map();
  view::GameRunner runner;
  std::string error;
  REQUIRE_MESSAGE(
      runner.start(config_for(place(tmp, WALK_POC_PATH, "walk_poc"), tmp), map, 240, &error),
      error);
  input::InputState state(map);
  u64 tick = 0;
  // E down on tick 1 and held: pressed once, on that tick alone.
  state.begin_tick(SimTick{1});
  state.feed(input::RawEvent{SimTick{1}, input::Source::Key, 8, 1.0f, 0});
  state.end_tick();
  walk(runner, tick, 1, 0.0, &state);
  state.begin_tick(SimTick{2});
  state.end_tick();
  walk(runner, tick, 1, 0.0, &state);
  CHECK(state_number(runner, "interactions") == 1.0);
  CHECK(std::string_view(runner.hud().lines[2].text) == "E pressed 1 times");
}

TEST_CASE("game runner: a rebuild with a field added is swapped in with thirst carried") {
  if (in_bundle()) return;
  const test::TempDir tmp("game_runner_reload");
  const std::string watched = place(tmp, WALK_POC_PATH, "walk_poc");
  const input::ActionMap map = view::default_fly_map();
  view::GameRunner runner;
  std::string error;
  REQUIRE_MESSAGE(runner.start(config_for(watched, tmp), map, 240, &error), error);
  u64 tick = 0;
  walk(runner, tick, 480);  // two seconds: 99.5 left
  const f64 before = state_number(runner, "thirst");
  CHECK(before == doctest::Approx(99.5));
  CHECK(runner.poll() == 0);  // nothing changed

  // The second build: the same sources, its state with a field added (as a game author would).
  const std::string sources = tmp.file("v2");
  std::filesystem::create_directories(sources);
  std::filesystem::copy_file(std::string(WALK_POC_SOURCE_DIR) + "/walk_poc.cpp",
                             sources + "/walk_poc.cpp");
  std::string schema = read_text(std::string(WALK_POC_SOURCE_DIR) + "/walk_poc.schema");
  const usize close = schema.rfind('}');
  REQUIRE(close != std::string::npos);
  schema.insert(close, "  /// Added by the rebuild.\n  rebuilt: u32 = 7\n");
  write_text(sources + "/walk_poc.schema", schema);
  const std::string built =
      tmp.file("built/walk_poc" + std::filesystem::path(WALK_POC_PATH).extension().string());
  const std::string config = std::string("-DCONFIG=") + ENGINE_GAME_MODULE_BUILD_CONFIG;
  const std::string source_arg = "-DSOURCE_DIR=" + sources;
  const std::string out_arg = "-DOUTPUT=" + built;
  const std::string_view argv[] = {
      ENGINE_CMAKE_COMMAND, config, source_arg, out_arg, "-P", ENGINE_GAME_MODULE_BUILD_SCRIPT};
  platform::Process process;
  REQUIRE_MESSAGE(process.spawn(argv, &error, true), error);
  process.close_stdin();
  std::string log;
  process.read_all(log);
  REQUIRE_MESSAGE(process.wait() == 0, log);
  (void)place(tmp, built.c_str(), "walk_poc", 5);  // over the watched file, its time moved on

  CHECK(runner.poll() == 1);
  CHECK(state_number(runner, "thirst") == doctest::Approx(before));  // carried
  CHECK(state_number(runner, "rebuilt") == 7.0);                     // the new field, defaulted
  walk(runner, tick, 240);
  CHECK(state_number(runner, "thirst") == doctest::Approx(before - 0.25));
  const JsonValue summary = runner.summary_json();
  f64 reloads = 0.0;
  REQUIRE(summary.find("reloads") != nullptr);
  REQUIRE(summary.find("reloads")->get_f64(reloads));
  CHECK(reloads == 1.0);
}

TEST_CASE("game runner: an action no map has is refused when the module loads, by name") {
  if (in_bundle()) return;
  const test::TempDir tmp("game_runner_bad_action");
  const input::ActionMap map = view::default_fly_map();
  view::GameRunner runner;
  std::string error;
  CHECK_FALSE(runner.start(config_for(place(tmp, WALK_POC_BAD_ACTION_PATH, "walk_poc"), tmp), map,
                           240, &error));
  CHECK(error.find("LoadHookFailed") != std::string::npos);
  CHECK(error.find("no game action 'dance'") != std::string::npos);
  CHECK_FALSE(runner.started());
  // A module that is not a library at all is the host's refusal too.
  const std::string junk = tmp.file("junk.dll");
  write_text(junk, "not a library");
  view::GameRunner other;
  CHECK_FALSE(other.start(config_for(junk, tmp), map, 240, &error));
  CHECK(error.find("the module host refused") == 0);
}

TEST_CASE("game runner: a module that fails from game_tick ends the run with its line") {
  if (in_bundle()) return;
  const test::TempDir tmp("game_runner_failing");
  const input::ActionMap map = view::default_fly_map();
  view::GameRunner runner;
  std::string error;
  REQUIRE_MESSAGE(runner.start(config_for(place(tmp, WALK_POC_FAILING_PATH, "walk_poc"), tmp), map,
                               240, &error),
                  error);
  u64 tick = 0;
  walk(runner, tick, 40);
  CHECK(runner.ended());
  CHECK(runner.failed());
  CHECK(runner.ticks() == 30);  // nothing ticked after the failure
  CHECK(runner.error().find("game_tick failed at tick 30") == 0);
  CHECK(runner.error().find("on purpose") != std::string::npos);
}

TEST_CASE("game runner: thirst at zero ends the run, and the state file carries it over") {
  if (in_bundle()) return;
  const test::TempDir tmp("game_runner_end");
  const input::ActionMap map = view::default_fly_map();
  const std::string module = place(tmp, WALK_POC_PATH, "walk_poc");
  const std::string state_file = tmp.file("state.json");
  // A saved run nearly out of water: a tenth of a second of walking left... at 0.25 a second.
  write_text(state_file, R"({"format":"engine.game.state","version":1,"module":"walk_poc",
                            "type":"engine.walk_poc.Survivor",
                            "state":{"thirst":0.05,"seconds":100,"walked_m":150}})");
  view::GameRunner::Config c = config_for(module, tmp);
  c.state_file = state_file;
  view::GameRunner runner;
  std::string error;
  REQUIRE_MESSAGE(runner.start(c, map, 240, &error), error);
  u64 tick = 0;
  walk(runner, tick, 240);
  REQUIRE(runner.ended());
  CHECK_FALSE(runner.failed());
  CHECK(runner.ticks() >= 48);  // 0.05 / 0.25 s at 240 Hz, give or take the last tick
  CHECK(runner.ticks() <= 49);
  const JsonValue summary = runner.summary_json();
  const JsonValue* result = summary.find("result");
  REQUIRE(result != nullptr);
  f64 seconds = 0.0;
  f64 distance = 0.0;
  REQUIRE(result->find("seconds") != nullptr);
  REQUIRE(result->find("seconds")->get_f64(seconds));
  REQUIRE(result->find("distance")->get_f64(distance));
  CHECK(seconds == doctest::Approx(100.2).epsilon(0.001));
  CHECK(distance == doctest::Approx(150.3).epsilon(0.001));
  CHECK(summary.find("restored")->as_bool());
  REQUIRE(runner.finish(&error));

  // The file now holds the ended run; a second runner reads it back.
  // (Its own shadow directory: a per-load copy is named by the process and the build, and the
  // first runner still holds its copy open.)
  c.shadow_directory = tmp.file("shadow2");
  view::GameRunner again;
  REQUIRE_MESSAGE(again.start(c, map, 240, &error), error);
  CHECK(state_number(again, "seconds") == doctest::Approx(100.2).epsilon(0.001));
  REQUIRE(again.state_json().find("over") != nullptr);
  CHECK(again.state_json().find("over")->as_bool());

  // A state file of another type is refused, naming both.
  write_text(state_file, R"({"format":"engine.game.state","type":"engine.other.State",
                            "state":{}})");
  view::GameRunner wrong;
  c.shadow_directory = tmp.file("shadow3");
  CHECK_FALSE(wrong.start(c, map, 240, &error));
  CHECK(error.find("engine.other.State") != std::string::npos);
  CHECK(error.find("engine.walk_poc.Survivor") != std::string::npos);
}

TEST_CASE(
    "game runner: the state file keeps the walker's pose, its scales and the clock, and a "
    "replay restores from its header") {
  // Version 2 of the state file (roadmap P47): a second of walking at a sun rate of 60 saves where
  // the walker ended, how it looked, the sky's time and the ground's; a second runner reads it
  // back, takes the sky's time whatever its own offset, and puts the pose on a live header. A
  // version 1 file restores the state and no session; a session block of another version is
  // refused; a replay of a recording restores the header's block and ignores the file.
  if (in_bundle()) return;
  const test::TempDir tmp("game_runner_session");
  const input::ActionMap map = view::default_fly_map();
  const std::string module = place(tmp, WALK_POC_PATH, "walk_poc");
  const std::string state_file = tmp.file("state.json");
  view::GameRunner::Config c = config_for(module, tmp);
  c.state_file = state_file;
  std::string error;
  {
    view::GameRunner runner;
    REQUIRE_MESSAGE(runner.start(c, map, 240, &error), error);
    runner.set_clock(3600.0, 60.0, 1000.0);
    runner.set_time_rate(5.0);
    runner.note_surface_time(1012.5);
    CHECK(runner.restored_session() == nullptr);
    u64 tick = 0;
    walk(runner, tick, 240, 1.5 / 240.0, nullptr, WorldPos{10.0, 2.0, 30.0});
    REQUIRE(runner.finish(&error));
  }
  JsonValue saved;
  REQUIRE(parse_json(read_text(state_file), saved).ok);
  u64 version = 0;
  REQUIRE(saved.find("version")->get_u64(version));
  CHECK(version == 2);
  REQUIRE(saved.find("session") != nullptr);
  MESSAGE("saved session: " << write_json(*saved.find("session")));

  c.shadow_directory = tmp.file("shadow2");
  view::GameRunner again;
  REQUIRE_MESSAGE(again.start(c, map, 240, &error), error);
  const view::GameRunner::SavedSession* s = again.restored_session();
  REQUIRE(s != nullptr);
  // The last tick's eye: 1.5 m walked north of z = 30, 1.65 m over the feet.
  CHECK(s->eye.x == 10.0);
  CHECK(s->eye.y == doctest::Approx(3.65));
  CHECK(s->eye.z == doctest::Approx(28.5));
  CHECK(s->walking);
  CHECK(s->sun_time_s == doctest::Approx(3600.0 + 60.0));  // one second at 60
  CHECK(s->sun_rate == 60.0);
  CHECK(s->ground_time_s == 1000.0);
  CHECK(s->has_surface_time);
  CHECK(s->surface_time_s == 1012.5);
  CHECK(s->time_rate == 5.0);
  // Another offset: the sky stands where it stood.
  again.set_clock(0.0, 1.0, 0.0);
  CHECK(again.sun_time_s() == doctest::Approx(3660.0));
  CHECK(again.sun_rate() == 60.0);
  view::SessionHeader header;
  view::GameRunner::apply_pose(*s, header);
  CHECK(header.start.position.z == doctest::Approx(28.5));
  CHECK(header.walking);
  CHECK(again.restored_json().is_object());

  // A version 1 file: the state, and no session.
  write_text(state_file, R"({"format":"engine.game.state","version":1,"module":"walk_poc",
                            "type":"engine.walk_poc.Survivor","state":{"seconds":5}})");
  c.shadow_directory = tmp.file("shadow3");
  view::GameRunner v1;
  REQUIRE_MESSAGE(v1.start(c, map, 240, &error), error);
  CHECK(state_number(v1, "seconds") == 5.0);
  CHECK(v1.restored_session() == nullptr);
  // A session block of another version is refused, naming it.
  write_text(state_file, R"({"format":"engine.game.state","version":2,"module":"walk_poc",
                            "type":"engine.walk_poc.Survivor","state":{},"session":{"version":9}})");
  c.shadow_directory = tmp.file("shadow4");
  view::GameRunner later;
  CHECK_FALSE(later.start(c, map, 240, &error));
  CHECK(error.find("version 9") != std::string::npos);
  // A replay of a recording restores the header's block, and never the file (which holds the
  // refused version 9 here): fresh, with no block; the block, with one.
  view::GameRunner::Config replay = c;
  replay.restore_from_header = true;
  replay.restore = nullptr;
  replay.shadow_directory = tmp.file("shadow5");
  view::GameRunner fresh;
  REQUIRE_MESSAGE(fresh.start(replay, map, 240, &error), error);
  CHECK(fresh.restored_session() == nullptr);
  CHECK(state_number(fresh, "seconds") == 0.0);
  replay.restore = &saved;
  replay.shadow_directory = tmp.file("shadow6");
  view::GameRunner from_header;
  REQUIRE_MESSAGE(from_header.start(replay, map, 240, &error), error);
  REQUIRE(from_header.restored_session() != nullptr);
  CHECK(state_number(from_header, "seconds") == doctest::Approx(1.0));
}

TEST_CASE("game runner: water at the nearest placement the host names refills the meter") {
  if (in_bundle()) return;
  const test::TempDir tmp("game_runner_water");
  const input::ActionMap map = view::default_fly_map();
  view::GameRunner runner;
  std::string error;
  REQUIRE_MESSAGE(
      runner.start(config_for(place(tmp, WALK_POC_PATH, "walk_poc"), tmp), map, 240, &error),
      error);
  // A ruin 3 m east of wherever the walker stands: always within the fixture's 6 m.
  struct Query {
    static bool nearest(void*, std::string_view kind, const WorldPos& from, f64 radius,
                        WorldPos& out, f64& distance) {
      if (kind != "ruins" || radius < 3.0) return false;
      out = WorldPos{from.x + 3.0, from.y, from.z};
      distance = 3.0;
      return true;
    }
  };
  runner.set_placements(&Query::nearest, nullptr);
  u64 tick = 0;
  walk(runner, tick, 240);
  // Drinking all the while: 10 a second in, 0.25 out, capped at 100.
  CHECK(state_number(runner, "thirst") == 100.0);
  CHECK(state_number(runner, "drinking_ticks") == 240.0);
  CHECK(std::string_view(runner.hud().lines[1].text) == "at the water: drinking");
}

#endif  // WALK_POC_PATH

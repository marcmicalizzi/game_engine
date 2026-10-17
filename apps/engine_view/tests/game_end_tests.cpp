// End to end: how a `--game` run ends and what clock its frames are drawn by (docs/subsystems/apps.md,
// "--game: running a game module", "Ending"; renderer.md "One clock"; roadmap P44). The drawn sun
// follows the runner's clock — a recording in which `]` was pressed replays offscreen to a sky the
// no-key replay does not draw, and the summary's clock is the sky's — and a live session whose
// module called `end_run` stays on its result panel until Enter or Escape, or starts a new run on R.
// Every run is offscreen or on the test's own desktop (view_process.h).

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
                     "--sun-rate", "3600", "--capture", capture, "--width", "160", "--height",
                     "96", "--ddc", tmp.file("ddc")});
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

#endif  // ENGINE_VIEW_GAME && WALK_POC_PATH

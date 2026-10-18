// End to end: a walk into the committed oasis pool (docs/subsystems/water.md, "Wading";
// content/test-scenes/oasis-pool). The scene is copied into the test's scratch directory, and a
// live window fed an injected session starts a walker on the pool's east shore, facing its middle,
// and holds W for twelve seconds — eighteen metres at a walk, past the middle were nothing in the
// way. The water slows it by the declared curve and stops it where it is chest deep, short of the
// middle (2.27 m deep), and the summary says the depth it reached and the ticks it was stopped; the
// recording then replays offscreen to the same trajectory and the same walker. Compiled where the
// water, the dunes, the scatter and the sky are; needs a display and a device, and skips without.

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

std::string text(const JsonValue* v, const char* key) {
  const JsonValue* f = v != nullptr ? v->find(key) : nullptr;
  return f != nullptr && f->is_string() ? std::string(f->as_string()) : std::string();
}

f64 number(const JsonValue* v, const char* key) {
  const JsonValue* f = v != nullptr ? v->find(key) : nullptr;
  f64 out = -1.0;
  if (f != nullptr) (void)f->get_f64(out);
  return out;
}

f64 marker_x(const JsonValue* trajectory, u32 index) {
  const JsonValue* markers = trajectory != nullptr ? trajectory->find("markers") : nullptr;
  if (markers == nullptr || !markers->is_array() || markers->size() <= index) return 1.0e9;
  const JsonValue* position = (*markers)[index].find("position");
  f64 x = 1.0e9;
  if (position != nullptr && position->is_array() && position->size() == 3)
    (void)(*position)[0].get_f64(x);
  return x;
}

}  // namespace

TEST_CASE("engine-view: a walk into the oasis pool wades, and stops chest deep") {
  const test::TempDir tmp("engine_view_walk_water");
  const std::string committed =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/oasis-pool/scene.json",
                      "content/test-scenes/oasis-pool/scene.json");
  if (!test::path_exists(committed)) {
    MESSAGE("the oasis-pool scene is not in this bundle");
    return;
  }
  std::string source;
  REQUIRE(io::read_file(committed, source) == io::Status::Ok);
  const std::string scene = tmp.file("scene.json");
  REQUIRE(io::write_file(scene, source) == io::Status::Ok);

  constexpr u32 k_w = 26;
  constexpr u32 k_m = 16;
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  const auto key = [&](u64 tick, u32 code, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0});
  };
  key(10, k_w, true);
  key(2890, k_w, false);  // twelve seconds at a walk: eighteen metres, were nothing in the way
  key(2900, k_m, true);
  key(2901, k_m, false);
  view::SessionHeader header;
  header.ticks = 2920;
  keys.set_session(view::session_to_json(header));
  const std::string inject = tmp.file("walk.jsonl");
  REQUIRE(keys.save(inject) == io::Status::Ok);
  const std::string recorded = tmp.file("recorded.jsonl");
  const std::string ddc = tmp.file("ddc");
  // The east shore, three metres from the water, facing west (yaw 90: forward is -x).
  const Run live = run_view({"--interactive", "--walk", "--start", "13,20,0", "90,0",
                             "--inject-input", inject, "--record-input", recorded, "--scene", scene,
                             "--width", "160", "--height", "96", "--no-vsync", "--ddc", ddc});
  if (live.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << live.output);
    return;
  }
  REQUIRE_MESSAGE(live.exit_code == 0, live.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(live, summary), live.output);
  const JsonValue* interactive = summary.find("interactive");
  REQUIRE(interactive != nullptr);
  const JsonValue* walk = interactive->find("walk");
  REQUIRE(walk != nullptr);
  const JsonValue* wading = walk->find("wading");
  REQUIRE_MESSAGE(wading != nullptr, "the summary's walk block has no wading");
  const f64 deepest = number(wading, "max_depth_m");
  const f64 stopped_ticks = number(wading, "stopped_ticks");
  const f64 stopped_x = marker_x(interactive->find("trajectory"), 0);
  MESSAGE("walked into the pool (" << text(walk, "collision") << "): " << deepest
                                   << " m deep at the most, " << stopped_ticks
                                   << " ticks stopped, the eye at x = " << stopped_x);
  // Chest deep and no deeper, stopped for most of the walk's last seconds, short of the middle.
  CHECK(deepest >= 1.4);
  CHECK(deepest < 1.5);
  CHECK(stopped_ticks > 240.0);
  CHECK(stopped_x > 0.5);
  CHECK(stopped_x < 9.0);

  // The recorded walk replays offscreen to the same camera at every tick, and the same walker.
  const Run replay = run_view({"--replay-input", recorded, "--offscreen", "--width", "160",
                               "--height", "96", "--ddc", ddc});
  REQUIRE_MESSAGE(replay.exit_code == 0, replay.output);
  JsonValue replayed;
  REQUIRE_MESSAGE(summary_of(replay, replayed), replay.output);
  const JsonValue* replay_interactive = replayed.find("interactive");
  REQUIRE(replay_interactive != nullptr);
  REQUIRE(replay_interactive->find("trajectory") != nullptr);
  CHECK(write_json(*replay_interactive->find("trajectory")) ==
        write_json(*interactive->find("trajectory")));
  const JsonValue* replay_walk = replay_interactive->find("walk");
  CHECK(text(replay_walk, "hash") == text(walk, "hash"));
  CHECK(write_json(*replay_walk->find("wading")) == write_json(*wading));
}

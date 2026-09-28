// End to end: the walk mode over the committed erg (docs/subsystems/apps.md, "Walking";
// content/test-scenes/desert-erg). A live window fed an injected session through its own event
// queue, started walking (`--walk`) on the interdune floor five metres short of the mega-draa's
// slip face, 40 m east of the camera path's `toe`, sprints four seconds east up the face and four
// back down, and marks the three places; its recording replays offscreen to the live session's
// trajectory and the live walker's state chain, bit for bit. Needs a display and a device, and a
// build whose engine-view carries the terrain capability's dunes; skips without either.
//
// **Why a window of the erg.** The committed scene is 6 km a side on a 4,097 grid, and a Debug
// engine-view takes over ten minutes to build that grid's cluster DAG. The dunes are a function of
// position, seed and time and not of the grid (terrain.md), so the case draws the committed erg's
// own sand over a 512 m window of it at 2 m — `size` and `extent` replaced, every band and the
// time as committed — and walks on exactly the ground the whole scene has there.

#include "../fly_camera.h"

#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
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
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error,
               /*merge_stderr=*/true)) {
    FAIL("cannot spawn engine-view: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
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

f64 number(const JsonValue* v, const char* key) {
  const JsonValue* f = v != nullptr ? v->find(key) : nullptr;
  f64 out = -1.0;
  if (f == nullptr || !f->get_f64(out)) return -1.0;
  return out;
}

std::string text(const JsonValue* v, const char* key) {
  const JsonValue* f = v != nullptr ? v->find(key) : nullptr;
  return f != nullptr && f->is_string() ? std::string(f->as_string()) : std::string();
}

#ifndef ENGINE_VIEW_TESTS_DUNES
#define ENGINE_VIEW_TESTS_DUNES 0
#endif
bool refused_without_dunes(const Run& run) {
  return ENGINE_VIEW_TESTS_DUNES == 0 && run.exit_code == 1 &&
         run.output.find("which this build does not have") != std::string::npos;
}

// A marker's camera: its position's y and x.
bool marker_at(const JsonValue* trajectory, u32 index, Vec3& out) {
  const JsonValue* markers = trajectory != nullptr ? trajectory->find("markers") : nullptr;
  if (markers == nullptr || !markers->is_array() || markers->size() <= index) return false;
  const JsonValue* position = (*markers)[index].find("position");
  f64 c[3] = {};
  if (position == nullptr || !position->is_array() || position->size() != 3) return false;
  for (u32 k = 0; k < 3; ++k)
    if (!(*position)[k].get_f64(c[k])) return false;
  out = Vec3{static_cast<f32>(c[0]), static_cast<f32>(c[1]), static_cast<f32>(c[2])};
  return true;
}

}  // namespace

TEST_CASE("engine-view: a walk up an erg dune and back replays to its own trajectory") {
  const test::TempDir tmp("engine_view_walk_erg");
  const std::string committed =
      test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/desert-erg/scene.json",
                      "content/test-scenes/desert-erg/scene.json");
  if (!test::path_exists(committed)) {
    MESSAGE("the erg is not in this bundle");
    return;
  }
  std::string source;
  REQUIRE(io::read_file(committed, source) == io::Status::Ok);
  JsonValue erg;
  REQUIRE(parse_json(source, erg).ok);
  JsonValue terrain = *erg.find("terrain");
  terrain.set("size", static_cast<u64>(257));
  terrain.set("extent", JsonValue(256.0));
  erg.set("terrain", std::move(terrain));
  const std::string scene = tmp.file("erg.json");
  REQUIRE(io::write_file(scene, write_json(erg)) == io::Status::Ok);
  // The scene names its camera path beside it; `--start` says where this session starts instead.
  std::string path_text;
  REQUIRE(io::read_file(committed.substr(0, committed.size() - std::string("scene.json").size()) +
                            "camera-path.json",
                        path_text) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("camera-path.json"), path_text) == io::Status::Ok);

  // Four seconds' sprint east up the slip face (W and Shift), four back down (S and Shift), a
  // marker at the start, the top and the end.
  constexpr u32 k_w = 26;
  constexpr u32 k_s = 22;
  constexpr u32 k_m = 16;
  constexpr u32 k_shift = 225;
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  const auto key = [&](u64 tick, u32 code, bool down) {
    keys.record(input::RawEvent{SimTick{tick}, input::Source::Key, code, down ? 1.0f : 0.0f, 0});
  };
  key(5, k_m, true);
  key(6, k_m, false);
  key(10, k_shift, true);
  key(10, k_w, true);
  key(970, k_w, false);
  key(980, k_m, true);
  key(981, k_m, false);
  key(990, k_s, true);
  key(1950, k_s, false);
  key(1950, k_shift, false);
  key(1960, k_m, true);
  key(1961, k_m, false);
  view::SessionHeader header;
  header.ticks = 1980;
  keys.set_session(view::session_to_json(header));
  const std::string inject = tmp.file("walk.jsonl");
  REQUIRE(keys.save(inject) == io::Status::Ok);

  const std::string recorded = tmp.file("recorded.jsonl");
  const std::string ddc = tmp.file("ddc");
  // At x = 140, looking east (yaw -90) and a little down. Along z = 0 at the committed time the
  // floor is 12–14 m high and the face rises from x = 145 to 51 m at x = 220, at 27–31° (sampled
  // from the ground provider); the path's `toe` marker, x = 100, is 45 m out on the floor.
  const Run live = run_view({"--interactive", "--walk", "--start", "140,300,0", "-90,-10",
                             "--inject-input", inject, "--record-input", recorded, "--scene", scene,
                             "--width", "160", "--height", "96", "--no-vsync", "--ddc", ddc});
  if (live.exit_code == 3 || refused_without_dunes(live)) {
    MESSAGE("engine-view unavailable here: " << live.output);
    return;
  }
  REQUIRE_MESSAGE(live.exit_code == 0, live.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(live, summary), live.output);
  const JsonValue* interactive = summary.find("interactive");
  REQUIRE(interactive != nullptr);
  const JsonValue* walk = interactive->find("walk");
  REQUIRE_MESSAGE(walk != nullptr, live.output);
  const JsonValue* trajectory = interactive->find("trajectory");
  REQUIRE(trajectory != nullptr);
  CHECK(text(walk, "start") == "walk");
  CHECK(text(walk, "mode") == "walk");
  CHECK(number(walk, "mode_changes") == 0.0);
  const std::string collision = text(walk, "collision");
  CHECK((collision == "physics" || collision == "ground-follow"));
  Vec3 start{};
  Vec3 top{};
  Vec3 end{};
  REQUIRE(marker_at(trajectory, 0, start));
  REQUIRE(marker_at(trajectory, 1, top));
  REQUIRE(marker_at(trajectory, 2, end));
  MESSAGE("walked the erg (" << collision << "): from (" << start.x << ", " << start.y
                             << ") up to (" << top.x << ", " << top.y << ") and back to (" << end.x
                             << ", " << end.y << "); " << number(walk, "distance_m")
                             << " m walked, " << number(walk, "climb_m")
                             << " m climbed, the ground at most "
                             << number(walk, "max_ground_error_m") << " m off the drawn one");
  CHECK(top.x > start.x + 10.0f);  // it went east
  CHECK(top.y > start.y + 4.0f);   // up the face
  // And back down: the same four seconds cover more ground downhill, where nothing holds the
  // stride back, so it comes off the face onto the floor it started on and past its start.
  CHECK(end.x < start.x + 2.0f);
  CHECK(end.y < top.y - 4.0f);
  CHECK(std::fabs(end.y - start.y) < 1.5f);
  CHECK(number(walk, "distance_m") > 25.0);
  CHECK(number(walk, "max_ground_error_m") < 0.5);
  if (collision == "physics") {
    const JsonValue* bodies = walk->find("bodies");
    CHECK(number(bodies, "live") > 0.0);
    CHECK(number(bodies, "tiles") > 0.0);
  }

  // **The recorded walk replays offscreen to the live walk, bit for bit**: the camera at every
  // tick, and the walker's own state chain.
  const Run replay = run_view({"--replay-input", recorded, "--offscreen", "--width", "160",
                               "--height", "96", "--ddc", ddc});
  REQUIRE_MESSAGE(replay.exit_code == 0, replay.output);
  JsonValue replayed;
  REQUIRE_MESSAGE(summary_of(replay, replayed), replay.output);
  const JsonValue* replay_interactive = replayed.find("interactive");
  REQUIRE(replay_interactive != nullptr);
  const JsonValue* replay_trajectory = replay_interactive->find("trajectory");
  REQUIRE(replay_trajectory != nullptr);
  CHECK(write_json(*replay_trajectory) == write_json(*trajectory));
  CHECK(text(replay_interactive->find("walk"), "hash") == text(walk, "hash"));
}

// End to end: `engine-view --interactive`, `--record-input`, `--replay-input` and
// `--inject-input` (docs/subsystems/apps.md, "`--interactive`: a camera somebody flies").
//
// What is asserted, and where it can run:
//
// - The flags that cannot work together, and the replays that must be refused — a device
//   recording with no session, a log under another action map, a log from another camera
//   integration version — exit before a device or a window is asked for, so these run on every
//   machine, GPU or not.
// - **The committed synthetic session, replayed twice offscreen with `--marker-captures`, draws
//   byte-identical markers, and flies the committed trajectory.** The captures need a device and
//   skip without one; the trajectory in the summary is checked whenever the run happened, and the
//   GPU-free half of the same promise is fly_tests.cpp's.
// - **A live window fed the fixture through its own event queue (`--inject-input`) records a log
//   whose replay flies the live session's trajectory to the last bit** — the live path and the
//   replay path agree, which is the guarantee. Needs a display and a device; skips without.
// - **A live session starts where it is told**: at `--start`, and otherwise at the first frame of
//   the scene file's own camera path rather than at the orbit. Read back from the recording's
//   header; needs a display and a device, and skips without.
#include "../fly_camera.h"

#include <core/json/json.h>
#include <core/platform/process.h>
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

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

const std::string& view_exe() {
  static const std::string path = test::app_path(ENGINE_APP_PATH);
  return path;
}

Run run_view(const std::vector<std::string>& args) {
  std::vector<std::string_view> argv;
  argv.push_back(view_exe());
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

std::string content_path(const char* relative) {
  return test::data_path(std::string(ENGINE_SOURCE_DIR "/") + relative, relative);
}

std::string fixture() { return content_path("content/input-logs/sessions/fly-synthetic.jsonl"); }

std::string read_text(const std::string& path) {
  std::string text;
  if (io::read_file(path, text) != io::Status::Ok) return {};
  return text;
}

// The summary line: the last line of output that is a JSON object (stderr is merged, and an
// offscreen run says what it did on stderr after the summary).
bool summary_of(const Run& run, JsonValue& out) {
  usize end = run.output.size();
  while (end > 0) {
    usize begin = run.output.find_last_of('\n', end - 1);
    begin = begin == std::string::npos ? 0 : begin + 1;
    const std::string line = run.output.substr(begin, end - begin);
    if (!line.empty() && line[0] == '{' && parse_json(line, out).ok && out.is_object()) {
      return true;
    }
    if (begin == 0) break;
    end = begin - 1;
  }
  return false;
}

const JsonValue* trajectory_of(const JsonValue& summary) {
  const JsonValue* interactive = summary.find("interactive");
  return interactive != nullptr && interactive->is_object() ? interactive->find("trajectory")
                                                            : nullptr;
}

std::string text_of(const JsonValue* value, const char* key) {
  const JsonValue* found = value != nullptr ? value->find(key) : nullptr;
  return found != nullptr ? std::string(found->as_string()) : std::string();
}

f64 number_of(const JsonValue* value, const char* key) {
  const JsonValue* found = value != nullptr ? value->find(key) : nullptr;
  f64 out = -1.0;
  if (found == nullptr || !found->get_f64(out)) return -1.0;
  return out;
}

// A small dune terrain the time-lapse and the rings can move in a debug build: 33 samples over
// 64 m of the terrain capability's generator, three years into the world.
void write_dune_scene(const std::string& path) {
  REQUIRE(io::write_file(path, R"({"format":"engine.scene.v1","name":"dunes",
      "terrain":{"size":33,"extent":32,"seed":5,"dune_height":2,"dune_wavelength":20,
                 "generator":"Dunes","time":94608000}})") == io::Status::Ok);
}

// Whether this build's engine-view carries the terrain capability's dunes: the CMake file defines
// it where the capability is linked. A build without it (msvc-minimal) refuses the dune scene with
// the scene-generator registry's sentence (docs/subsystems/apps.md), which is not what the cases
// over that scene test, so they skip there — and only there: with the capability, a refusal fails
// them.
#ifndef ENGINE_VIEW_TESTS_DUNES
#define ENGINE_VIEW_TESTS_DUNES 0
#endif
bool refused_without_dunes(const Run& run) {
  return ENGINE_VIEW_TESTS_DUNES == 0 && run.exit_code == 1 &&
         run.output.find("which this build does not have") != std::string::npos;
}

std::string slashes(const std::filesystem::path& path) {
  std::string out = path.string();
  for (char& c : out) {
    if (c == '\\') c = '/';
  }
  return out;
}

}  // namespace

TEST_CASE("engine-view: interactive flags that cannot work together exit 2") {
  const std::string log = fixture();
  CHECK(run_view({"--record-input", "x.jsonl"}).exit_code == 2);  // not a live session
  CHECK(run_view({"--inject-input", log}).exit_code == 2);
  CHECK(run_view({"--interactive", "--offscreen"}).exit_code == 2);  // nobody can fly offscreen
  CHECK(run_view({"--interactive", "--marker-captures", "dir"}).exit_code == 2);
  CHECK(run_view({"--replay-input", log, "--camera-path", "p.json"}).exit_code == 2);
  CHECK(run_view({"--replay-input", log, "--record-input", "x.jsonl"}).exit_code == 2);
  CHECK(run_view({"--input-map", "map.json"}).exit_code == 2);
  CHECK(run_view({"--interactive", "--animate"}).exit_code == 2);
  CHECK(run_view({"--interactive", "--fly", "10", "2", "5"}).exit_code == 2);
  CHECK(run_view({"--replay-input"}).exit_code == 2);
  CHECK(run_view({"--tunable", "view.fly.tick_hz=1"}).exit_code == 2);  // outside its range
  CHECK(run_view({"--tunable", "view.fly.nothing=1"}).exit_code == 2);
  // `--start`: two values, three numbers and two, a pitch short of the poles, a live session only,
  // and not together with a path that also says where to start.
  CHECK(run_view({"--interactive", "--start", "0,1,2"}).exit_code == 2);
  CHECK(run_view({"--interactive", "--start", "0,1", "0,0"}).exit_code == 2);
  CHECK(run_view({"--interactive", "--start", "0,1,2", "0,95"}).exit_code == 2);
  CHECK(run_view({"--interactive", "--start", "0,1,2", "zero,0"}).exit_code == 2);
  // A position is doubles, anywhere a world cell can name it: 1.37e11 m each way, and no further.
  const Run outside = run_view({"--interactive", "--start", "0,1,2e11", "0,0"});
  CHECK(outside.exit_code == 2);
  CHECK_MESSAGE(outside.output.find("outside the world") != std::string::npos, outside.output);
  CHECK(run_view({"--replay-input", log, "--start", "0,1,2", "0,0"}).exit_code == 2);
  CHECK(
      run_view({"--interactive", "--start", "0,1,2", "0,0", "--camera-path", "p.json"}).exit_code ==
      2);
  // `--walk` starts a live session walking; a replay starts as its recording did.
  CHECK(run_view({"--walk"}).exit_code == 2);
  CHECK(run_view({"--replay-input", log, "--walk"}).exit_code == 2);
  // `--capture-channels`: offscreen only, known names, and something to capture.
  CHECK(run_view({"--capture-channels", "ids", "--capture", "x.png"}).exit_code == 2);
  CHECK(run_view({"--offscreen", "--capture-channels", "albedo", "--capture", "x.png"}).exit_code ==
        2);
  CHECK(run_view({"--offscreen", "--capture-channels", "ids"}).exit_code == 2);
  // Presentation (apps.md, "Pacing"): known names and ranges, the window's flags refused where
  // there is no window, and `--windowed` for a replay only.
  CHECK(run_view({"--present", "vsync"}).exit_code == 2);
  CHECK(run_view({"--swapchain-images", "1"}).exit_code == 2);
  CHECK(run_view({"--swapchain-images", "9"}).exit_code == 2);
  CHECK(run_view({"--frames-in-flight", "0"}).exit_code == 2);
  CHECK(run_view({"--frames-in-flight", "4"}).exit_code == 2);
  CHECK(run_view({"--pace", "sometimes"}).exit_code == 2);
  CHECK(run_view({"--offscreen", "--pace", "display"}).exit_code == 2);
  CHECK(run_view({"--offscreen", "--present", "mailbox"}).exit_code == 2);
  CHECK(run_view({"--offscreen", "--borderless"}).exit_code == 2);
  CHECK(run_view({"--offscreen", "--no-present-timing"}).exit_code == 2);
  CHECK(run_view({"--offscreen", "--pace", "off"}).exit_code == 2);
  CHECK(
      run_view({"--replay-input", log, "--benchmark", "x.jsonl", "--pace", "display"}).exit_code ==
      2);  // a replay's benchmark is offscreen unless --windowed
  CHECK(run_view({"--windowed"}).exit_code == 2);
  // The sun's day: game seconds per real second, 0 to 1e7.
  CHECK(run_view({"--sun-rate", "-1"}).exit_code == 2);
  CHECK(run_view({"--sun-rate", "fast"}).exit_code == 2);
  CHECK(run_view({"--sun-rate", "2e7"}).exit_code == 2);
  CHECK(run_view({"--sun-rate"}).exit_code == 2);
  CHECK(run_view({"--interactive", "--windowed"}).exit_code == 2);
  CHECK(run_view({"--replay-input", log, "--windowed", "--offscreen"}).exit_code == 2);
  CHECK(run_view({"--replay-input", log, "--windowed", "--marker-captures", "dir"}).exit_code == 2);
}

TEST_CASE("engine-view: a live session starts at --start, else at the scene's own camera path") {
  // The first owner session started at the orbit, which frames a 5 km scene's whole bounds from
  // 9 km out (docs/experiments/first-interactive-session-2026-09-24.md). A scene file that names
  // its camera path starts a session at that path's first frame, and `--start` puts it anywhere.
  const test::TempDir tmp("engine_view_start");
  const std::filesystem::path dir(tmp.native());
  const std::string scene = slashes(dir / "scene.json");
  REQUIRE(io::write_file(scene, R"({"format":"engine.scene.v1","name":"start",
      "camera_path":"path.json","terrain":{"size":17,"extent":20,"seed":1,"dune_height":0.5}})") ==
          io::Status::Ok);
  REQUIRE(io::write_file(slashes(dir / "path.json"),
                         R"({"keys":[{"time":0,"position":[3,2,15],"ground":true,
                                      "target":[0,0,0]}]})") == io::Status::Ok);
  // A tenth of a second of session with nothing pressed: the recording's header is the point.
  input::InputLog quiet;
  quiet.set_map(view::default_fly_map());
  view::SessionHeader quiet_header;
  quiet_header.ticks = 24;
  quiet.set_session(view::session_to_json(quiet_header));
  const std::string inject = tmp.file("quiet.jsonl");
  REQUIRE(quiet.save(inject) == io::Status::Ok);
  const std::string ddc = tmp.file("ddc");

  auto start_of = [&](const std::vector<std::string>& extra, view::FlyState& start) -> int {
    const std::string recorded = tmp.file("recorded.jsonl");
    std::vector<std::string> args = {"--interactive", "--inject-input",
                                     inject,          "--record-input",
                                     recorded,        "--scene",
                                     scene,           "--width",
                                     "256",           "--height",
                                     "160",           "--no-vsync",
                                     "--ddc",         ddc};
    args.insert(args.end(), extra.begin(), extra.end());
    const Run live = run_view(args);
    if (live.exit_code != 0) {
      if (live.exit_code != 3) FAIL_CHECK(live.output);
      return live.exit_code;
    }
    input::InputLog recording;
    std::string error;
    REQUIRE_MESSAGE(recording.load(recorded, &error) == io::Status::Ok, error);
    view::SessionHeader header;
    REQUIRE_MESSAGE(view::session_from_json(recording.session(), header, &error), error);
    start = header.start;
    return 0;
  };

  view::FlyState from_scene;
  const int code = start_of({}, from_scene);
  if (code == 3) {
    MESSAGE("engine-view unavailable here");
    return;
  }
  REQUIRE(code == 0);
  // The path's key, over the ground at (3, 15), looking at the origin: not the orbit.
  CHECK(from_scene.position.x == 3.0);
  CHECK(from_scene.position.z == 15.0);
  CHECK(from_scene.position.y > 1.0);
  CHECK(from_scene.position.y < 4.0);
  CHECK(std::fabs(from_scene.yaw - std::atan2(3.0f, 15.0f)) < 1e-4f);

  view::FlyState given;
  REQUIRE(start_of({"--start", "1,-2,3.5", "90,-10"}, given) == 0);
  CHECK(given.position == WorldPos{1.0, -2.0, 3.5});
  CHECK(std::fabs(given.yaw - 1.5707963f) < 1e-6f);
  CHECK(std::fabs(given.pitch + 0.17453293f) < 1e-6f);
  // Angles past a half turn come back into [-180, 180] before they are radians.
  view::FlyState wrapped;
  REQUIRE(start_of({"--start", "0,0,0", "270,0"}, wrapped) == 0);
  CHECK(std::fabs(wrapped.yaw + 1.5707963f) < 1e-6f);
}

TEST_CASE("engine-view: a replay refuses what it cannot fly, before it asks for a device") {
  const test::TempDir tmp("engine_view_refuse");
  const std::string log = fixture();
  if (!test::path_exists(log)) {
    MESSAGE("not in this bundle: " << log);
    return;
  }
  // A file that is not there, and a device recording from engine-input, which has no session.
  CHECK(run_view({"--replay-input", tmp.file("missing.jsonl")}).exit_code == 1);
  const std::string device_log = content_path("content/input-logs/f710.jsonl");
  if (test::path_exists(device_log)) {
    const Run device = run_view({"--replay-input", device_log});
    CHECK(device.exit_code == 1);
    CHECK_MESSAGE(device.output.find("no session block") != std::string::npos, device.output);
  }

  // **Another action map**: the player moved forward off W after recording.
  input::ActionMap rebound = view::default_fly_map();
  const input::ActionId move = rebound.find_action("move");
  rebound.clear_bindings(move);
  rebound.bind(move, input::Binding{input::Source::Key, 82, 1.0f}, 1);
  const std::string map_path = tmp.file("rebound.json");
  REQUIRE(io::write_file(map_path, write_json(rebound.to_json())) == io::Status::Ok);
  const Run rebind = run_view({"--replay-input", log, "--input-map", map_path, "--offscreen"});
  CHECK(rebind.exit_code == 1);
  CHECK_MESSAGE(rebind.output.find("rebinding invalidates a replay") != std::string::npos,
                rebind.output);

  // **Another integration version**: the same log with its session's version moved on.
  input::InputLog future;
  std::string error;
  REQUIRE_MESSAGE(future.load(log, &error) == io::Status::Ok, error);
  JsonValue session = future.session();
  session.set("version", static_cast<u64>(view::k_fly_version + 1));
  future.set_session(session);
  const std::string future_path = tmp.file("future.jsonl");
  REQUIRE(future.save(future_path) == io::Status::Ok);
  const Run version = run_view({"--replay-input", future_path, "--offscreen"});
  CHECK(version.exit_code == 1);
  CHECK_MESSAGE(version.output.find("refused rather than misread") != std::string::npos,
                version.output);
}

TEST_CASE("engine-view: a live session's frame records 420 km out carry its f64 camera") {
  // The frame log's pose (FrameRecord `pose_position`) was a float32 until 2026-10-05, and so was
  // the camera: 420 km out both stood on a 3.1 cm grid, and a camera flying along -z at 1 m/s, 4 mm
  // a tick, did not move. Started at an x no float holds (419,072.37 is 419,072.375 as one) and
  // flown forward for half a second, the session's records carry the x typed, to the bit, and a z
  // that moves by the flight's 0.5 m and stands off the float's grid. Needs a display and a device.
  // **How many records there are is the machine's**: the window runs whatever ticks are due when a
  // frame starts, so a frame slow enough runs sixty, and beside a merge gate's builds the first
  // frame alone did — a quarter of a metre down the line before the first record — and the 120
  // ticks came in as few as five records instead of about 58 (2026-10-07, docs/experiments/
  // load-sensitive-tests-2026-10-07.md). So the motion is checked against each record's own
  // simulated time, which holds at any frame rate: every pose lies on the line at 1 m/s, and the
  // last at the session's half second.
  const test::TempDir tmp("engine_view_far_pose");
  input::InputLog forward;
  forward.set_map(view::default_fly_map());
  forward.record(input::RawEvent{SimTick{1}, input::Source::Key, 26, 1.0f, 0});  // W
  view::SessionHeader header;
  header.ticks = 120;
  forward.set_session(view::session_to_json(header));
  const std::string inject = tmp.file("forward.jsonl");
  REQUIRE(forward.save(inject) == io::Status::Ok);
  const std::string jsonl = tmp.file("far.jsonl");
  const Run live = run_view({"--interactive",
                             "--inject-input",
                             inject,
                             "--benchmark",
                             jsonl,
                             "--start",
                             "419072.37,10,-419072.61",
                             "0,0",
                             "--procedural",
                             "heightfield",
                             "--grid",
                             "65",
                             "--tunable",
                             "view.fly.speed=1",
                             "--width",
                             "256",
                             "--height",
                             "160",
                             "--no-vsync",
                             "--ddc",
                             tmp.file("ddc")});
  if (live.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << live.output);
    return;
  }
  REQUIRE_MESSAGE(live.exit_code == 0, live.output);
  const std::string text = read_text(jsonl);
  u32 records = 0;
  u32 x_exact = 0;
  u32 z_off_grid = 0;
  u32 on_line = 0;
  f64 first_z = 0.0;
  f64 last_z = 0.0;
  f64 first_t = 0.0;
  f64 last_t = 0.0;
  constexpr f64 k_start_z = -419072.61;
  for (usize begin = 0; begin < text.size();) {
    const usize end = text.find('\n', begin);
    const usize stop = end == std::string::npos ? text.size() : end;
    JsonValue line;
    if (stop > begin && parse_json(text.substr(begin, stop - begin), line).ok &&
        line.find("repeat") != nullptr) {
      const JsonValue* pose = line.find("pose_position");
      REQUIRE(pose != nullptr);
      REQUIRE(pose->size() == 3);
      f64 x = 0.0;
      f64 z = 0.0;
      REQUIRE((*pose)[0].get_f64(x));
      REQUIRE((*pose)[2].get_f64(z));
      f64 t = -1.0;
      REQUIRE(line.find("pose_time") != nullptr);
      REQUIRE(line.find("pose_time")->get_f64(t));
      x_exact += x == 419072.37 ? 1u : 0u;
      z_off_grid += std::fmod(std::fabs(z), 0.03125) != 0.0 ? 1u : 0u;
      // A tenth of a millimetre: the ticks' float32 steps summed in f64, and the interpolation.
      on_line += std::fabs((k_start_z - z) - t) < 1.0e-4 ? 1u : 0u;
      if (records == 0) {
        first_z = z;
        first_t = t;
      }
      last_z = z;
      last_t = t;
      ++records;
    }
    begin = stop + 1;
  }
  MESSAGE(records << " records: x as typed in " << x_exact << ", z off a float's 3.1 cm grid in "
                  << z_off_grid << ", on the line at 1 m/s in " << on_line << "; z moved "
                  << first_z - last_z << " m along -z between " << first_t << " and " << last_t
                  << " s");
  CHECK(records > 1);
  CHECK(x_exact == records);
  CHECK(z_off_grid * 2 > records);  // a grid point is a 1-in-8,000 chance at a 4 um step
  CHECK(on_line == records);
  // The last frame draws the session's 120th tick, or a fraction of it if the clock stood exactly
  // there: half a second, and half a metre, to within a tick.
  CHECK(last_t >= 119.0 / 240.0);
  CHECK(last_t <= 0.5);
}

TEST_CASE("engine-view: a session recorded with float32 positions replays, and says so") {
  // Every session recorded before 2026-10-05 is version 1 of the fly integration (fly_camera.h):
  // it replays under version 2's f64 arithmetic, and the summary's trajectory says its hash cannot
  // be compared with the recording's, rather than the replay being refused.
  const test::TempDir tmp("engine_view_v1");
  const std::string log = content_path("content/input-logs/sessions/fly-synthetic-v1.jsonl");
  if (!test::path_exists(log)) {
    MESSAGE("not in this bundle: " << log);
    return;
  }
  const Run run = run_view({"--replay-input", log, "--offscreen", "--width", "256", "--height",
                            "160", "--ddc", tmp.file("ddc")});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  CHECK_MESSAGE(run.output.find("is not the recording's") != std::string::npos, run.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* trajectory = trajectory_of(summary);
  REQUIRE(trajectory != nullptr);
  const JsonValue* comparable = trajectory->find("comparable");
  REQUIRE(comparable != nullptr);
  bool value = true;
  CHECK(comparable->get_bool(value));
  CHECK_FALSE(value);
  CHECK(number_of(trajectory, "recorded_version") == 1.0);
  CHECK(number_of(trajectory, "ticks") == 480.0);
}

TEST_CASE("engine-view: a replay draws the same markers twice and flies the committed path") {
  const test::TempDir tmp("engine_view_replay");
  const std::string log = fixture();
  const std::string committed =
      read_text(content_path("content/input-logs/sessions/fly-synthetic.trajectory.json"));
  if (!test::path_exists(log) || committed.empty()) {
    MESSAGE("the session fixture is not in this bundle");
    return;
  }
  JsonValue expected;
  REQUIRE(parse_json(committed, expected).ok);
  const std::string ddc = tmp.file("ddc");
  const std::string first_dir = tmp.file("first");
  const std::string second_dir = tmp.file("second");
  const std::string jsonl = tmp.file("replay.jsonl");
  const std::vector<std::string> common = {"--replay-input", log,   "--width", "256",
                                           "--height",       "160", "--ddc",   ddc};
  std::vector<std::string> first_args = common;
  for (const char* a : {"--marker-captures", first_dir.c_str(), "--benchmark", jsonl.c_str()})
    first_args.push_back(a);
  const Run first = run_view(first_args);
  if (first.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << first.output);
    return;
  }
  REQUIRE_MESSAGE(first.exit_code == 0, first.output);
  std::vector<std::string> second_args = common;
  for (const char* a : {"--marker-captures", second_dir.c_str()})
    second_args.push_back(a);
  const Run second = run_view(second_args);
  REQUIRE_MESSAGE(second.exit_code == 0, second.output);

  // The camera: both runs flew the committed trajectory, hash and markers alike.
  for (const Run* run : {&first, &second}) {
    JsonValue summary;
    REQUIRE_MESSAGE(summary_of(*run, summary), run->output);
    const JsonValue* trajectory = trajectory_of(summary);
    REQUIRE_MESSAGE(trajectory != nullptr, run->output);
    CHECK(write_json(*trajectory) == write_json(expected));
    CHECK(text_of(summary.find("interactive"), "mode") == "replay");
  }

  // The pictures: one per marker, and the same bytes from both runs.
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::directory_iterator(first_dir))
    names.push_back(entry.path().filename().string());
  const JsonValue* markers = expected.find("markers");
  REQUIRE(markers != nullptr);
  CHECK(names.size() == markers->size());
  for (const std::string& name : names) {
    const std::string a = read_text(slashes(std::filesystem::path(first_dir) / name));
    const std::string b = read_text(slashes(std::filesystem::path(second_dir) / name));
    CHECK_MESSAGE(!a.empty(), name);
    if (a != b) tmp.keep();
    CHECK_MESSAGE(a == b,
                  name << " differs between two replays of one session; kept in " << tmp.path());
  }
  CHECK(test::path_exists(slashes(std::filesystem::path(first_dir) / "tick-0000060.png")));

  // The benchmark JSONL: a record per frame, four ticks each, and the flythrough's summary line
  // carrying the same trajectory.
  const std::string text = read_text(jsonl);
  std::vector<std::string> lines;
  for (usize begin = 0; begin < text.size();) {
    const usize end = text.find('\n', begin);
    const usize stop = end == std::string::npos ? text.size() : end;
    if (stop > begin) lines.push_back(text.substr(begin, stop - begin));
    begin = stop + 1;
  }
  REQUIRE(lines.size() == 121);  // 480 ticks at four a frame, and the summary
  u64 ticks = 0;
  for (usize i = 0; i + 1 < lines.size(); ++i) {
    JsonValue record;
    REQUIRE(parse_json(lines[i], record).ok);
    u64 n = 0;
    REQUIRE(record.find("ticks") != nullptr);
    REQUIRE(record.find("ticks")->get_u64(n));
    CHECK(n == 4);
    ticks += n;
    CHECK(record.find("cpu_ms") != nullptr);
    CHECK(record.find("frame_ms") != nullptr);
  }
  CHECK(ticks == 480);
  JsonValue summary;
  REQUIRE(parse_json(lines.back(), summary).ok);
  u64 total = 0;
  REQUIRE(summary.find("ticks")->get_u64(total));
  CHECK(total == 480);
  REQUIRE(trajectory_of(summary) != nullptr);
  CHECK(write_json(*trajectory_of(summary)) == write_json(expected));
}

TEST_CASE("engine-view: a live session fed through its window replays to its own trajectory") {
  const test::TempDir tmp("engine_view_live");
  const std::string log = fixture();
  if (!test::path_exists(log)) {
    MESSAGE("the session fixture is not in this bundle");
    return;
  }
  const std::string ddc = tmp.file("ddc");
  const std::string recorded = tmp.file("recorded.jsonl");
  const std::string jsonl = tmp.file("live.jsonl");
  // The fixture's keys and pointer motion go onto the window's own queue at their ticks and come
  // back out through the conversion, the fixed tick and the recording — every step a person at
  // the window goes through but the OS's. The real pointer is never taken.
  const Run live = run_view({"--interactive", "--inject-input", log, "--record-input", recorded,
                             "--benchmark", jsonl, "--procedural", "heightfield", "--grid", "65",
                             "--width", "256", "--height", "160", "--no-vsync", "--ddc", ddc});
  if (live.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << live.output);
    return;
  }
  REQUIRE_MESSAGE(live.exit_code == 0, live.output);
  JsonValue live_summary;
  REQUIRE_MESSAGE(summary_of(live, live_summary), live.output);
  const JsonValue* live_trajectory = trajectory_of(live_summary);
  REQUIRE_MESSAGE(live_trajectory != nullptr, live.output);
  CHECK(text_of(live_summary.find("interactive"), "mode") == "live");

  // The recording: the default map, a session header, and the injected session's length.
  input::InputLog recording;
  std::string error;
  REQUIRE_MESSAGE(recording.load(recorded, &error) == io::Status::Ok, error);
  CHECK(recording.map_hash() == view::default_fly_map().hash());
  view::SessionHeader header;
  REQUIRE_MESSAGE(view::session_from_json(recording.session(), header, &error), error);
  CHECK(header.ticks == 480);
  CHECK(header.grid == 65);
  CHECK(header.recorded_by == "engine-view");
  CHECK_FALSE(recording.empty());

  // **The replay flies the live session's trajectory, bit for bit.** The live ticks landed on
  // whatever frames the window happened to draw; the replay lands them four to a frame, offscreen.
  const Run replay = run_view({"--replay-input", recorded, "--offscreen", "--width", "256",
                               "--height", "160", "--ddc", ddc});
  REQUIRE_MESSAGE(replay.exit_code == 0, replay.output);
  JsonValue replay_summary;
  REQUIRE_MESSAGE(summary_of(replay, replay_summary), replay.output);
  const JsonValue* replay_trajectory = trajectory_of(replay_summary);
  REQUIRE(replay_trajectory != nullptr);
  CHECK(write_json(*replay_trajectory) == write_json(*live_trajectory));
  CHECK(text_of(live_trajectory, "hash") == text_of(replay_trajectory, "hash"));
  const JsonValue* markers = live_trajectory->find("markers");
  REQUIRE(markers != nullptr);
  CHECK(markers->size() == 5);  // the fixture's five
  // **And each at the tick the fixture pressed it** (apps.md, "Driving a window with nobody at
  // it"): the window lands every injected event on its own tick, however the frames fell. Until
  // 2026-10-07 a slow frame ran past the events inside it, and beside a merge gate's builds the
  // last markers, pressed at ticks 450 and 475 of 480, were recorded late or not at all (three or
  // four markers of five: docs/experiments/load-sensitive-tests-2026-10-07.md).
  input::InputLog pressed_log;
  REQUIRE_MESSAGE(pressed_log.load(log, &error) == io::Status::Ok, error);
  const input::ActionMap map = view::default_fly_map();
  const input::ActionId marker = map.find_action("marker");
  REQUIRE(marker != input::k_invalid_action);
  std::vector<u64> pressed;
  for (const input::RawEvent& e : pressed_log.events()) {
    if (e.value != 1.0f) continue;
    for (const input::ActionBinding& b : map.bindings(marker)) {
      if (b.binding.source == e.source && b.binding.code == e.code) pressed.push_back(e.tick.value);
    }
  }
  REQUIRE(pressed.size() == 5);
  for (u32 m = 0; m < markers->size() && m < pressed.size(); ++m) {
    CAPTURE(m);
    CHECK(number_of(&(*markers)[m], "tick") == static_cast<f64>(pressed[m]));
  }

  // The live --benchmark: the flythrough's JSONL, a record per presented frame whose ticks add up
  // to the session's.
  const std::string text = read_text(jsonl);
  u64 ticks = 0;
  u64 records = 0;
  for (usize begin = 0; begin < text.size();) {
    const usize end = text.find('\n', begin);
    const usize stop = end == std::string::npos ? text.size() : end;
    JsonValue line;
    if (stop > begin && parse_json(text.substr(begin, stop - begin), line).ok &&
        line.find("repeat") != nullptr) {
      u64 n = 0;
      if (line.find("ticks") != nullptr && line.find("ticks")->get_u64(n)) ticks += n;
      ++records;
    }
    begin = stop + 1;
  }
  CHECK(records > 0);
  CHECK(ticks == 480);
}

TEST_CASE("engine-view: a replay flown in the window measures its presentation") {
  // `--windowed` keeps a replay's `--benchmark` in the window, at the recording's pace, which is
  // how a presentation change is compared on the same input (apps.md, "Pacing"). The frame log
  // then has a record per presented frame with the frame's waits, and its summary a
  // `presentation` block saying what the swapchain was and how the frames were paced. The
  // camera is the recording's either way. Needs a display and a device; skips without.
  const test::TempDir tmp("engine_view_windowed");
  const std::string log = fixture();
  const std::string committed =
      read_text(content_path("content/input-logs/sessions/fly-synthetic.trajectory.json"));
  if (!test::path_exists(log) || committed.empty()) {
    MESSAGE("the session fixture is not in this bundle");
    return;
  }
  JsonValue expected;
  REQUIRE(parse_json(committed, expected).ok);
  const std::string jsonl = tmp.file("windowed.jsonl");
  const Run run =
      run_view({"--replay-input", log, "--windowed", "--benchmark", jsonl, "--pace", "display",
                "--width", "256", "--height", "160", "--ddc", tmp.file("ddc")});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* trajectory = trajectory_of(summary);
  REQUIRE(trajectory != nullptr);
  CHECK(text_of(trajectory, "hash") == text_of(&expected, "hash"));

  const std::string text = read_text(jsonl);
  u64 ticks = 0;
  u64 records = 0;
  u64 waits_named = 0;
  JsonValue last;
  for (usize begin = 0; begin < text.size();) {
    const usize end = text.find('\n', begin);
    const usize stop = end == std::string::npos ? text.size() : end;
    JsonValue line;
    if (stop > begin && parse_json(text.substr(begin, stop - begin), line).ok) {
      if (line.find("repeat") != nullptr) {
        u64 n = 0;
        if (line.find("ticks") != nullptr && line.find("ticks")->get_u64(n)) ticks += n;
        if (line.find("wait_ms") != nullptr && line.find("acquire_ms") != nullptr &&
            line.find("present_ms") != nullptr && line.find("pace_ms") != nullptr &&
            line.find("submit_ms") != nullptr && line.find("shown_ms") != nullptr &&
            line.find("pose_time") != nullptr && line.find("pose_position") != nullptr) {
          ++waits_named;
        }
        ++records;
      } else {
        last = std::move(line);
      }
    }
    begin = stop + 1;
  }
  CHECK(records > 0);
  CHECK(waits_named == records);
  CHECK(ticks == 480);
  const JsonValue* presentation = last.find("presentation");
  REQUIRE_MESSAGE(presentation != nullptr, text);
  REQUIRE(presentation->is_object());
  // FIFO, the default; the images the driver gave for the three asked; the default depth. The
  // pacer is "display" where the surface can wait on a present and "off" (with a warning) where
  // it cannot, so either is an answer; what is checked is that it says which.
  CHECK(text_of(presentation, "present_mode") == "fifo");
  CHECK(text_of(presentation, "requested_mode") == "auto");
  u64 images = 0;
  u64 in_flight = 0;
  REQUIRE(presentation->find("image_count") != nullptr);
  REQUIRE(presentation->find("image_count")->get_u64(images));
  REQUIRE(presentation->find("frames_in_flight") != nullptr);
  REQUIRE(presentation->find("frames_in_flight")->get_u64(in_flight));
  CHECK(images >= 2);
  CHECK(in_flight == 2);
  const std::string pacing = text_of(presentation, "pacing");
  CHECK((pacing == "display" || pacing == "off"));
}

TEST_CASE("engine-view: an offscreen time-lapse and sun's day carry both summary blocks") {
  // The brief's smoke, on a scene a debug build draws in a second: `--time-rate` and `--sun-rate`
  // offscreen, where the keys do nothing and a frame's place in the day is its frame index at
  // sixty a second. Needs a device; skips without.
  const test::TempDir tmp("engine_view_sun_day");
  const std::string scene = tmp.file("dunes.json");
  write_dune_scene(scene);
  const std::string ddc = tmp.file("ddc");
  const std::vector<std::string> common = {"--offscreen", "--scene", scene, "--frames",
                                           "12",          "--width", "96",  "--height",
                                           "64",          "--ddc",   ddc};
  std::vector<std::string> args = common;
  for (const char* a : {"--time-rate", "86400", "--sun-rate", "3600", "--capture"})
    args.push_back(a);
  args.push_back(tmp.file("a.png"));
  const Run run = run_view(args);
  if (run.exit_code == 3 || refused_without_dunes(run)) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* lapse = summary.find("time_lapse");
  REQUIRE_MESSAGE(lapse != nullptr, run.output);
  REQUIRE(lapse->is_object());
  CHECK(number_of(lapse, "rate") == 86'400.0);
  CHECK(number_of(lapse, "start_rate") == 86'400.0);
  CHECK(number_of(lapse, "rate_changes") == 0.0);
  const JsonValue* sun = summary.find("sun");
  REQUIRE_MESSAGE(sun != nullptr, run.output);
  REQUIRE(sun->is_object());
  CHECK(number_of(sun, "rate") == 3'600.0);
  CHECK(number_of(sun, "start_rate") == 3'600.0);
  CHECK(number_of(sun, "rate_changes") == 0.0);
  CHECK(number_of(sun, "time_s") == doctest::Approx(3'600.0 * 11.0 / 60.0));  // the last frame's
  CHECK(number_of(sun, "tilt_deg") == 40.0);
  CHECK(number_of(sun, "intensity") == 1.0);  // eleven game minutes into an April morning
  CHECK(number_of(sun, "elevation_deg") > number_of(sun, "start_elevation_deg"));  // climbing

  // A still day is the sun it always was, and the tunable is the default rate.
  args = common;
  for (const char* a : {"--tunable", "renderer.sun.rate=0"})
    args.push_back(a);
  const Run still = run_view(args);
  REQUIRE_MESSAGE(still.exit_code == 0, still.output);
  JsonValue still_summary;
  REQUIRE(summary_of(still, still_summary));
  const JsonValue* still_sun = still_summary.find("sun");
  REQUIRE(still_sun != nullptr);
  CHECK(number_of(still_sun, "time_s") == 0.0);
  CHECK(number_of(still_sun, "elevation_deg") ==
        doctest::Approx(number_of(still_sun, "start_elevation_deg")).epsilon(1e-5));
  // Without --time-rate, offscreen, nothing moves the sand and there is no time-lapse.
  REQUIRE(still_summary.find("time_lapse") != nullptr);
  CHECK(still_summary.find("time_lapse")->is_null());
}

TEST_CASE("engine-view: a live session's keys: the two rates, the click, and Esc twice") {
  // An injected session over a dune terrain at --time-rate 0 — drawn as terrain levels because an
  // interactive window may set the sand going — whose log presses `.` once and `]` twice, gives
  // the pointer back with Escape, takes it again with a click, gives it back again and presses
  // Escape once more, which ends the session long before the log does. The summary says where the
  // rates ended and how often they changed; the recording is written as a closed window's is.
  // Needs a display and a device; skips without.
  const test::TempDir tmp("engine_view_keys");
  const std::string scene = tmp.file("dunes.json");
  write_dune_scene(scene);
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  const auto press = [&](u64 tick, input::Source source, u32 code) {
    keys.record(input::RawEvent{SimTick{tick}, source, code, 1.0f, 0});
    keys.record(input::RawEvent{SimTick{tick + 1}, source, code, 0.0f, 0});
  };
  // Eighty ticks apart: a frame runs a quarter of a second of ticks at most (the fixed-step clock's
  // cap), so no two presses can land on one tick even on a machine that stalls, and a press is
  // never two presses swallowed into one.
  constexpr u32 k_escape = 41;
  press(3, input::Source::Key, k_escape);     // captured at the start: gives the pointer back
  press(83, input::Source::Key, 55);          // .  dunes 0 -> 60
  press(163, input::Source::Key, 48);         // ]  sun 0 -> 60
  press(243, input::Source::Key, 48);         // ]  sun 60 -> 600
  press(323, input::Source::MouseButton, 1);  // a click takes the pointer
  press(403, input::Source::Key, k_escape);   // gives it back
  press(483, input::Source::Key, k_escape);   // and with it given back, ends the session
  view::SessionHeader header;
  header.ticks = 800;
  keys.set_session(view::session_to_json(header));
  const std::string inject = tmp.file("keys.jsonl");
  REQUIRE(keys.save(inject) == io::Status::Ok);
  const std::string recorded = tmp.file("recorded.jsonl");
  const Run live =
      run_view({"--interactive", "--inject-input", inject, "--record-input", recorded, "--scene",
                scene, "--width", "160", "--height", "96", "--no-vsync", "--ddc", tmp.file("ddc")});
  if (live.exit_code == 3 || refused_without_dunes(live)) {
    MESSAGE("engine-view unavailable here: " << live.output);
    return;
  }
  REQUIRE_MESSAGE(live.exit_code == 0, live.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(live, summary), live.output);
  const JsonValue* lapse = summary.find("time_lapse");
  REQUIRE_MESSAGE(lapse != nullptr, live.output);
  REQUIRE_MESSAGE(lapse->is_object(), live.output);
  CHECK(number_of(lapse, "start_rate") == 0.0);
  CHECK(number_of(lapse, "rate") == 60.0);
  CHECK(number_of(lapse, "rate_changes") == 1.0);
  const JsonValue* sun = summary.find("sun");
  REQUIRE(sun != nullptr);
  CHECK(number_of(sun, "start_rate") == 0.0);
  CHECK(number_of(sun, "rate") == 600.0);
  CHECK(number_of(sun, "rate_changes") == 2.0);
  CHECK(number_of(sun, "time_s") > 0.0);
  // The console said so, a line a change.
  CHECK(live.output.find("dunes x60 game s a real s (was x0)") != std::string::npos);
  CHECK(live.output.find("sun x600 game s a real s (was x60)") != std::string::npos);
  // Esc with the pointer free ended it: the recording stops at the last Escape, not at 800.
  input::InputLog recording;
  std::string error;
  REQUIRE_MESSAGE(recording.load(recorded, &error) == io::Status::Ok, error);
  view::SessionHeader written;
  REQUIRE_MESSAGE(view::session_from_json(recording.session(), written, &error), error);
  CHECK(written.ticks >= 483u);
  CHECK(written.ticks < 800u);
  CHECK(recording.map_hash() == view::default_fly_map().hash());
}

TEST_CASE("engine-view: --present-max-hz takes a number, and nothing else") {
  CHECK(run_view({"--present-max-hz"}).exit_code == 2);
  CHECK(run_view({"--present-max-hz", "fast"}).exit_code == 2);
  CHECK(run_view({"--present-max-hz", "-1"}).exit_code == 2);
  CHECK(run_view({"--present-max-hz", "100001"}).exit_code == 2);
}

TEST_CASE("engine-view: a window its display does not pace presents under a ceiling") {
  // A window of 256×160 on mailbox or immediate draws a frame in microseconds and, left alone,
  // presents thousands of times a second; every one of those goes through the desktop's
  // compositor, which on 2026-09-28 stopped the owner's whole desktop repainting for as long as
  // such a window lived (apps.md, "Pacing"). So the loop holds a frame back until a period has
  // passed since the one before it. The session is its ticks' and replays to the same trajectory
  // under any ceiling. Needs a display and a device; skips without.
  const test::TempDir tmp("engine_view_ceiling");
  const std::string log = fixture();
  const std::string committed =
      read_text(content_path("content/input-logs/sessions/fly-synthetic.trajectory.json"));
  if (!test::path_exists(log) || committed.empty()) {
    MESSAGE("the session fixture is not in this bundle");
    return;
  }
  JsonValue expected;
  REQUIRE(parse_json(committed, expected).ok);

  struct Case {
    const char* flag_value;  // "" takes the tunable's default
    u64 ceiling;
  };
  for (const Case c : {Case{"", 120}, Case{"60", 60}}) {
    CAPTURE(c.ceiling);
    const std::string jsonl =
        tmp.file(std::string("ceiling-") + std::to_string(c.ceiling) + ".jsonl");
    std::vector<std::string> args = {
        "--replay-input", log,   "--windowed", "--benchmark", jsonl,   "--no-vsync",
        "--width",        "256", "--height",   "160",         "--ddc", tmp.file("ddc")};
    if (c.flag_value[0] != '\0') {
      args.push_back("--present-max-hz");
      args.push_back(c.flag_value);
    }
    const Run run = run_view(args);
    if (run.exit_code == 3) {
      MESSAGE("engine-view unavailable here: " << run.output);
      return;
    }
    REQUIRE_MESSAGE(run.exit_code == 0, run.output);
    JsonValue summary;
    REQUIRE_MESSAGE(summary_of(run, summary), run.output);
    const JsonValue* trajectory = trajectory_of(summary);
    REQUIRE(trajectory != nullptr);
    CHECK(text_of(trajectory, "hash") == text_of(&expected, "hash"));

    const std::string text = read_text(jsonl);
    u64 records = 0;
    JsonValue last;
    for (usize begin = 0; begin < text.size();) {
      const usize end = text.find('\n', begin);
      const usize stop = end == std::string::npos ? text.size() : end;
      JsonValue line;
      if (stop > begin && parse_json(text.substr(begin, stop - begin), line).ok) {
        if (line.find("repeat") != nullptr) {
          ++records;
        } else {
          last = std::move(line);
        }
      }
      begin = stop + 1;
    }
    const JsonValue* presentation = last.find("presentation");
    REQUIRE_MESSAGE(presentation != nullptr, text);
    const std::string mode = text_of(presentation, "present_mode");
    if (mode != "mailbox" && mode != "immediate") {
      MESSAGE("this surface has neither mailbox nor immediate (" << mode << "): nothing to cap");
      return;
    }
    CHECK(text_of(presentation, "pacing") == "off");
    CHECK(number_of(presentation, "max_hz") == doctest::Approx(static_cast<f64>(c.ceiling)));
    CHECK(number_of(presentation, "ceiling_waits") > 0.0);
    // The fixture is 480 ticks at 240 a second: two seconds of session, and the loading before
    // its first tick draws frames too. Four times the ceiling is room for that; without a ceiling
    // the count is in the thousands.
    CHECK(records > 0);
    CHECK(records <= c.ceiling * 4);
  }
}

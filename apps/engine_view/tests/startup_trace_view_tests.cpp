// `--startup-trace` end to end (apps.md, "--startup-trace: from launch to ready"): a short walk on
// the world's tiles replayed offscreen with the trace on, and once with it off. The block's
// milestones are all reached and monotone, the trace file is Chrome trace JSON whose dependency
// edges point backwards, the frame loop allocated nothing after its first frame with the trace
// recording, and without the flag the block is null.

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

#ifndef ENGINE_VIEW_TESTS_DUNES
#define ENGINE_VIEW_TESTS_DUNES 0
#endif

}  // namespace

TEST_CASE(
    "engine-view: --startup-trace records launch to nearby_stable, and allocates nothing in "
    "the frame loop") {
  const test::TempDir tmp("engine_view_startup_trace");
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
  terrain.set("size", static_cast<u64>(129));
  terrain.set("extent", JsonValue(128.0));
  erg.set("terrain", std::move(terrain));
  JsonValue world;
  REQUIRE(parse_json(R"({"ground":true,"rings":[{"radius":1.5,"ground_cells":32},
                                                {"radius":3,"ground_cells":16}]})",
                     world)
              .ok);
  erg.set("world", std::move(world));
  const std::string scene = tmp.file("erg-tiles.json");
  REQUIRE(io::write_file(scene, write_json(erg)) == io::Status::Ok);

  // Stand for a moment, then walk forward: the first step is the "controllable" milestone.
  constexpr u32 k_w = 26;
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  keys.record(input::RawEvent{SimTick{10}, input::Source::Key, k_w, 1.0f, 0});
  keys.record(input::RawEvent{SimTick{470}, input::Source::Key, k_w, 0.0f, 0});
  view::SessionHeader header;
  header.ticks = 480;
  header.scene = scene;
  header.start.position = WorldPos{10.0, 300.0, 0.0};
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  const std::string session = tmp.file("walk.jsonl");
  REQUIRE(keys.save(session) == io::Status::Ok);

  const std::string trace = tmp.file("trace.json");
  const std::string frames = tmp.file("frames.jsonl");
  const std::string ddc = tmp.file("ddc");
  const Run on = run_view({"--replay-input", session, "--scene", scene, "--width", "96", "--height",
                           "64", "--ddc", ddc, "--benchmark", frames, "--startup-trace", trace});
  if (on.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << on.output);
    return;
  }
  if (ENGINE_VIEW_TESTS_DUNES == 0 && on.exit_code == 1 &&
      on.output.find("which this build does not have") != std::string::npos) {
    MESSAGE("this build has no dunes");
    return;
  }
  REQUIRE_MESSAGE(on.exit_code == 0, on.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(on, summary), on.output);
  const JsonValue* startup = summary.find("startup");
  REQUIRE(startup != nullptr);
  REQUIRE(startup->is_object());
  bool reached = false;
  REQUIRE(startup->find("until_reached")->get_bool(reached));
  CHECK_MESSAGE(reached, on.output);
  bool acyclic = false;
  REQUIRE(startup->find("acyclic")->get_bool(acyclic));
  CHECK(acyclic);
  const JsonValue* milestones = startup->find("milestones");
  REQUIRE(milestones != nullptr);
  CHECK(milestones->size() == 8);
  f64 last = -1.0;
  for (const JsonValue& m : milestones->as_array()) {
    f64 ms = 0.0;
    REQUIRE_MESSAGE(m.find("ms")->get_f64(ms), write_json(m));
    CHECK(ms >= last);
    last = ms;
  }
  // The trace: Chrome trace JSON, every stage's edges pointing at an earlier stage.
  std::string trace_text;
  REQUIRE(io::read_file(trace, trace_text) == io::Status::Ok);
  JsonValue doc;
  REQUIRE(parse_json(trace_text, doc).ok);
  const JsonValue* events = doc.find("traceEvents");
  REQUIRE(events != nullptr);
  u32 stages = 0;
  for (const JsonValue& e : events->as_array()) {
    std::string_view ph;
    if (!e.find("ph")->get_string(ph) || ph != "X") continue;
    ++stages;
    u64 id = 0;
    REQUIRE(e.find("args")->find("id")->get_u64(id));
    for (const JsonValue& d : e.find("args")->find("depends")->as_array()) {
      u64 on_id = 0;
      REQUIRE(d.get_u64(on_id));
      CHECK(on_id < id);
    }
  }
  CHECK(stages > 10);
  // The frame loop allocated nothing after its first frame, the trace recording all along.
  std::string records;
  REQUIRE(io::read_file(frames, records) == io::Status::Ok);
  usize at = 0;
  u32 lines = 0;
  while (at < records.size()) {
    usize end = records.find('\n', at);
    if (end == std::string::npos) end = records.size();
    JsonValue record;
    // A frame record has both; the file's other lines (its summary) are not frames.
    if (end > at && parse_json(std::string_view(records).substr(at, end - at), record).ok &&
        record.find("frame") != nullptr && record.find("allocations") != nullptr) {
      u64 frame = 0, allocations = 0;
      REQUIRE(record.find("frame")->get_u64(frame));
      REQUIRE(record.find("allocations")->get_u64(allocations));
      if (frame > 0) CHECK_MESSAGE(allocations == 0, "frame " << frame);
      ++lines;
    }
    at = end + 1;
  }
  CHECK(lines > 30);

  // Off: the same run with no flag has no block.
  const Run off = run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--width",
                            "96", "--height", "64", "--ddc", ddc});
  REQUIRE_MESSAGE(off.exit_code == 0, off.output);
  JsonValue quiet;
  REQUIRE(summary_of(off, quiet));
  const JsonValue* none = quiet.find("startup");
  CHECK((none == nullptr || none->is_null()));
}

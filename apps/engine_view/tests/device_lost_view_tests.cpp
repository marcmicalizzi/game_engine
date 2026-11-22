// A lost device, end to end (roadmap T212; apps.md, "When the device is lost"): the committed
// synthetic fly session replayed offscreen with the test seam (`ENGINE_GFX_TEST_LOSE_DEVICE`,
// domain/gfx/device_lost.h) making the frame wait for timeline value 40 answer "lost". The run
// ends with `platform::k_exit_device_lost`, and leaves its frame records up to the last that
// folded, their last line the loss, a report whose exit reason is `device_lost` with the same
// block, and its startup trace — counted, not timed. No card faults: the seam answers before the
// device is asked.

#include "view_process.h"

#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_environment.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <string>
#include <vector>

using namespace engine;

namespace {

std::string content_path(const char* relative) {
  return test::data_path(std::string(ENGINE_SOURCE_DIR "/") + relative, relative);
}

i32 run_view(const std::vector<std::string>& args, std::string& output) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  return view_test::run_program(std::span<const std::string_view>(argv.data(), argv.size()),
                                output);
}

}  // namespace

TEST_CASE("engine-view: a lost device leaves its records, its report and its trace, and exits 76") {
  const std::string log = content_path("content/input-logs/sessions/fly-synthetic.jsonl");
  if (!test::path_exists(log)) {
    MESSAGE("the session fixture is not in this bundle");
    return;
  }
  const test::TempDir tmp("engine_view_device_lost");
  const std::string frames = tmp.file("frames.jsonl");
  const std::string report = tmp.file("report.json");
  const std::string trace = tmp.file("trace.json");
  constexpr u64 k_lose_at = 40;  // the timeline value whose wait answers "lost"
  std::string output;
  i32 code = -1;
  {
    const test::ScopedEnv lose("ENGINE_GFX_TEST_LOSE_DEVICE", std::to_string(k_lose_at));
    code = run_view({"--replay-input", log, "--offscreen", "--width", "96", "--height", "64",
                     "--benchmark", frames, "--report", report, "--startup-trace", trace},
                    output);
  }
  if (code == 3 && output.find("no Vulkan device") != std::string::npos) {
    MESSAGE("engine-view unavailable here: " << output.substr(0, 300));
    return;
  }
  REQUIRE_MESSAGE(code == platform::k_exit_device_lost, output);
  CHECK(output.find("the device was lost (frame_wait)") != std::string::npos);

  // The records: every line a frame but the last, which is the loss.
  std::string text;
  REQUIRE(io::read_file(frames, text) == io::Status::Ok);
  u64 records = 0;
  u64 last_frame = 0;
  JsonValue lost_line;
  usize at = 0;
  while (at < text.size()) {
    usize end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    JsonValue line;
    if (end > at && parse_json(std::string_view(text).substr(at, end - at), line).ok) {
      if (line.find("device_lost") != nullptr) {
        lost_line = *line.find("device_lost");
      } else if (line.find("frame") != nullptr) {
        REQUIRE(line.find("frame")->get_u64(last_frame));
        ++records;
      }
    }
    at = end + 1;
  }
  // Frames 0 to 37 folded before the wait for value 40 (submission 39) was asked: two in flight,
  // and the renderer folds a frame when its slot comes round. The count is the loop's, so allow
  // the frames in flight either way rather than pin a slot count.
  CHECK(records >= k_lose_at - 4);
  CHECK(records < k_lose_at);
  REQUIRE(lost_line.is_object());
  std::string_view where;
  REQUIRE(lost_line.find("at")->get_string(where));
  CHECK(where == "frame_wait");
  bool simulated = false;
  REQUIRE(lost_line.find("simulated")->get_bool(simulated));
  CHECK(simulated);
  u64 value = 0, recorded = 0, last_recorded = 0, submitted = 0;
  REQUIRE(lost_line.find("frame_value")->get_u64(value));
  CHECK(value == k_lose_at);
  REQUIRE(lost_line.find("last_submitted")->get_u64(submitted));
  CHECK(submitted >= value);
  REQUIRE(lost_line.find("frames_recorded")->get_u64(recorded));
  CHECK(recorded == records);
  REQUIRE(lost_line.find("last_recorded_frame")->get_u64(last_recorded));
  CHECK(last_recorded == last_frame);
  bool read = true;
  REQUIRE(lost_line.find("fault")->find("read")->get_bool(read));
  CHECK_FALSE(read);  // a simulated loss reads no fault

  // The report: the exit, the same block, and the frames the records hold.
  std::string report_text;
  REQUIRE(io::read_file(report, report_text) == io::Status::Ok);
  JsonValue r;
  REQUIRE(parse_json(report_text, r).ok);
  u64 version = 0;
  REQUIRE(r.find("version")->get_u64(version));
  CHECK(version >= 2);
  i64 exit_code = 0;
  REQUIRE(r.find("exit")->find("code")->get_i64(exit_code));
  CHECK(exit_code == platform::k_exit_device_lost);
  std::string_view reason;
  REQUIRE(r.find("exit")->find("reason")->get_string(reason));
  CHECK(reason == "device_lost");
  const JsonValue* block = r.find("device_lost");
  REQUIRE(block != nullptr);
  CHECK(write_json(*block) == write_json(lost_line));
  u64 counted = 0;
  REQUIRE(r.find("frames")->find("count")->get_u64(counted));
  CHECK(counted == records);

  // The startup trace, as far as the run got.
  std::string trace_text;
  REQUIRE(io::read_file(trace, trace_text) == io::Status::Ok);
  JsonValue doc;
  REQUIRE(parse_json(trace_text, doc).ok);
  CHECK(doc.find("traceEvents") != nullptr);
}

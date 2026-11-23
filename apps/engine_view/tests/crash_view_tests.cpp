// A fault in the process, end to end (roadmap T216; apps.md, "When the process faults"): the
// committed synthetic fly session replayed offscreen with the test seam (`ENGINE_VIEW_TEST_FAULT`,
// session_records.h) making the frame loop write through a bad pointer at frame 30 — a real access
// violation, in the child, on the CPU; the card is never asked to fault. The run ends with
// `platform::k_exit_crashed` (77) after one fatal line, and leaves its frame records up to the last
// that folded with a last `crashed` line, a report whose exit reason is `crashed` with the same
// block, the crash file the platform wrote itself, and its startup trace — counted, not timed.
//
// Under a sanitizer (`ENGINE_FAULT_UNDER_SANITIZER`) nothing of the engine's is installed: the
// sanitizer's runtime reports the fault and ends the process with its own status. The case then
// checks that half — the run still dies, and no crash file, `crashed` block or 77 appears — and
// says so; it is not skipped.

#include "view_process.h"

#include <core/json/json.h>
#include <core/platform/crash.h>
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

// [[maybe_unused]]: a sanitizer build reads no line (Clang's -Wunused-function, linux-clang-asan).
[[maybe_unused]] usize count_of(const std::string& text, std::string_view what) {
  usize n = 0;
  for (usize at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
    ++n;
  return n;
}

}  // namespace

TEST_CASE(
    "engine-view: a fault leaves its records, its report, a crash file and its trace, and "
    "exits 77") {
  const std::string log = content_path("content/input-logs/sessions/fly-synthetic.jsonl");
  if (!test::path_exists(log)) {
    MESSAGE("the session fixture is not in this bundle");
    return;
  }
  const test::TempDir tmp("engine_view_crash");
  const std::string frames = tmp.file("frames.jsonl");
  const std::string report = tmp.file("report.json");
  const std::string trace = tmp.file("trace.json");
  const std::string crash = report + ".crash.json";
  constexpr u64 k_fault_at = 30;  // the frame whose start writes through a bad pointer
  std::string output;
  i32 code = -1;
  {
    const test::ScopedEnv fault("ENGINE_VIEW_TEST_FAULT", std::to_string(k_fault_at));
    code = run_view({"--replay-input", log, "--offscreen", "--width", "96", "--height", "64",
                     "--benchmark", frames, "--report", report, "--startup-trace", trace},
                    output);
  }
  if (code == 3 && output.find("no Vulkan device") != std::string::npos) {
    MESSAGE("engine-view unavailable here: " << output.substr(0, 300));
    return;
  }
  INFO("engine-view printed: " << output);
  REQUIRE(output.find("the test seam faults at frame 30") != std::string::npos);
#if ENGINE_FAULT_UNDER_SANITIZER
  MESSAGE(
      "a sanitizer build: its runtime has the fault, and engine-view's turn is not installed; "
      "checking that the run still dies and leaves no crash file, block or 77");
  CHECK(code != 0);
  CHECK(code != platform::k_exit_crashed);
  CHECK_FALSE(test::path_exists(crash));
  CHECK(output.find("engine: fatal: ") == std::string::npos);
#else
  REQUIRE(code == platform::k_exit_crashed);
  // One fatal line, naming the write and the faulting instruction in engine-view itself, and the
  // host's line after it.
  CHECK(count_of(output, "engine: fatal: ") == 1);
  const bool names_the_write =
      output.find("access violation writing 0x10 at engine-view") != std::string::npos ||
      output.find("SIGSEGV, address not mapped writing 0x10 at engine-view") != std::string::npos;
  CHECK(names_the_write);
  CHECK(output.find("exit code 77 after the host's turn") != std::string::npos);
  CHECK(output.find("engine-view: the process faulted (") != std::string::npos);

  // The records: every line a frame but the last, which is the fault.
  std::string text;
  REQUIRE(io::read_file(frames, text) == io::Status::Ok);
  u64 records = 0;
  u64 last_frame = 0;
  JsonValue crashed_line;
  usize at = 0;
  while (at < text.size()) {
    usize end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    JsonValue line;
    if (end > at && parse_json(std::string_view(text).substr(at, end - at), line).ok) {
      if (line.find("crashed") != nullptr) {
        crashed_line = *line.find("crashed");
      } else if (line.find("frame") != nullptr) {
        REQUIRE(line.find("frame")->get_u64(last_frame));
        ++records;
      }
    }
    at = end + 1;
  }
  // Frames 0 to 29 started before the fault; the records are the ones that folded, a slot or two
  // behind. The count is the loop's, so allow the frames in flight rather than pin a slot count.
  CHECK(records >= k_fault_at - 4);
  CHECK(records < k_fault_at);
  REQUIRE(crashed_line.is_object());
  u64 frame = 0, recorded = 0, last_recorded = 0;
  REQUIRE(crashed_line.find("frame")->get_u64(frame));
  CHECK(frame == k_fault_at);
  std::string_view doing, access, address, at_text;
  REQUIRE(crashed_line.find("doing")->get_string(doing));
  CHECK(doing == "frame");
  REQUIRE(crashed_line.find("access")->get_string(access));
  CHECK(access == "writing");
  REQUIRE(crashed_line.find("address")->get_string(address));
  CHECK(address == "0x0000000000000010");
  REQUIRE(crashed_line.find("at")->get_string(at_text));
  CHECK(at_text.find("engine-view") == 0);
  CHECK(crashed_line.find("stack")->size() > 0);
  REQUIRE(crashed_line.find("frames_recorded")->get_u64(recorded));
  CHECK(recorded == records);
  REQUIRE(crashed_line.find("last_recorded_frame")->get_u64(last_recorded));
  CHECK(last_recorded == last_frame);
  std::string_view crash_named;
  REQUIRE(crashed_line.find("crash_file")->get_string(crash_named));
  CHECK(crash_named == crash);
  u64 tick = 0;
  REQUIRE(crashed_line.find("tick")->get_u64(tick));
  CHECK(tick > 0);

  // The report: the exit, the same block, the frames the records hold.
  std::string report_text;
  REQUIRE(io::read_file(report, report_text) == io::Status::Ok);
  JsonValue r;
  REQUIRE(parse_json(report_text, r).ok);
  u64 version = 0;
  REQUIRE(r.find("version")->get_u64(version));
  CHECK(version >= 4);
  i64 exit_code = 0;
  REQUIRE(r.find("exit")->find("code")->get_i64(exit_code));
  CHECK(exit_code == platform::k_exit_crashed);
  std::string_view reason;
  REQUIRE(r.find("exit")->find("reason")->get_string(reason));
  CHECK(reason == "crashed");
  const JsonValue* block = r.find("crashed");
  REQUIRE(block != nullptr);
  CHECK(write_json(*block) == write_json(crashed_line));
  u64 counted = 0;
  REQUIRE(r.find("frames")->find("count")->get_u64(counted));
  CHECK(counted == records);

  // The crash file the platform wrote before engine-view's turn: the same fault.
  std::string crash_text;
  REQUIRE(io::read_file(crash, crash_text) == io::Status::Ok);
  JsonValue c;
  REQUIRE(parse_json(crash_text, c).ok);
  std::string_view format;
  REQUIRE(c.find("format")->get_string(format));
  CHECK(format == "engine.crash");
  u64 crash_exit = 0;
  REQUIRE(c.find("exit_code")->get_u64(crash_exit));
  CHECK(crash_exit == static_cast<u64>(platform::k_exit_crashed));
  std::string_view crash_at;
  REQUIRE(c.find("at")->get_string(crash_at));
  CHECK(crash_at == at_text);

  // The startup trace, as far as the run got.
  std::string trace_text;
  REQUIRE(io::read_file(trace, trace_text) == io::Status::Ok);
  JsonValue doc;
  REQUIRE(parse_json(trace_text, doc).ok);
  CHECK(doc.find("traceEvents") != nullptr);
#endif
}

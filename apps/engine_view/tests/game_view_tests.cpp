// End to end: what a playable proof of concept needs from engine-view (docs/subsystems/apps.md,
// "--report: what a tester sends back" and "--game: running a game module"; roadmap P21). The
// report a run leaves behind whichever way it ended, and — where this build carries the hotload
// capability — a game module run from the walking session: ticked every tick, its HUD drawn, its
// result in the summary and the report, its state saved and restored, its refusals clean, and a
// recorded session replaying to the same module state. Every run is offscreen or on the test's own
// desktop (view_process.h).

#include "view_process.h"

#include <core/json/json.h>
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

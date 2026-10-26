// The window's load end to end (apps.md, "The loading presentation"; roadmap W79): on the test's
// own desktop, a window whose load has a deliberately slow stage on its load thread pumps its
// messages all the way through it and presents loading frames meanwhile, where the same load on
// the main thread (`--load-on-main`, the load before W79) leaves one gap as long as the stage; the
// default overlay is drawn or not as `--no-loading-overlay` says and the picture after the ready
// milestone is the same bytes either way; the previous run's stage times weight the next run's.

#include "view_process.h"

#include <core/json/json.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <filesystem>
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

f64 number(const JsonValue& object, const char* key) {
  const JsonValue* v = object.find(key);
  f64 out = -1.0;
  if (v == nullptr || !v->get_f64(out)) return -1.0;
  return out;
}

std::string text(const JsonValue& object, const char* key) {
  const JsonValue* v = object.find(key);
  std::string_view out;
  if (v == nullptr || !v->get_string(out)) return {};
  return std::string(out);
}

// A small window over the procedural heightfield, six frames and closed; `extra` after.
Run window_run(const std::string& ddc, std::vector<std::string> extra) {
  std::vector<std::string> args = {"--width", "320",    "--height", "200",     "--frames",
                                    "6",       "--grid", "33",       "--ddc",    ddc,
                                    "--no-vsync"};
  args.insert(args.end(), extra.begin(), extra.end());
  return run_view(args);
}

}  // namespace

TEST_CASE("engine-view: a window's slow load keeps pumping, and the next run is weighted by it") {
  const test::TempDir tmp("engine_view_loading_pump");
  const std::string ddc = tmp.file("ddc");
  const Run run = window_run(ddc, {"--test-slow-load", "1500"});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* loading = summary.find("loading");
  REQUIRE(loading != nullptr);
  REQUIRE(loading->is_object());
  MESSAGE("loading: " << write_json(*loading, JsonWriteOptions{.pretty = false}));
  CHECK(text(*loading, "overlay") == "default");
  CHECK(text(*loading, "plan") == "no previous run");
  // The 1.5 s stage on the load thread: the window's queue looked at all the way through it — at
  // least every 100 ms by the pumps' own count, and no gap between two of them as long as that.
  CHECK(number(*loading, "job_ms") >= 1500.0);
  CHECK(number(*loading, "job_pumps") >= 15.0);
  CHECK(number(*loading, "longest_job_gap_ms") < 100.0);
  CHECK(number(*loading, "longest_job_gap_ms") > 0.0);
  // And loading frames presented meanwhile, at about 30 Hz.
  CHECK(number(*loading, "frames") >= 20.0);
  CHECK(number(*loading, "overlay_ms_per_frame") > 0.0);
  CHECK(number(*loading, "ready_from_main_ms") >= number(*loading, "loading_from_main_ms"));
  CHECK(text(*loading, "ready_milestone") == "player_controllable");

  // The run kept its stages' times for the next one, under the derived-data root.
  std::error_code ec;
  u32 plans = 0;
  for (const auto& entry : std::filesystem::directory_iterator(ddc + "/startup", ec)) {
    (void)entry;
    ++plans;
  }
  CHECK(plans == 1);
  const Run again = window_run(ddc, {});
  REQUIRE_MESSAGE(again.exit_code == 0, again.output);
  JsonValue next;
  REQUIRE_MESSAGE(summary_of(again, next), again.output);
  REQUIRE(next.find("loading") != nullptr);
  CHECK(text(*next.find("loading"), "plan") == "the previous run");
}

TEST_CASE("engine-view: --load-on-main is the load as it was: one gap as long as the stage") {
  const test::TempDir tmp("engine_view_loading_main");
  const Run run = window_run(tmp.file("ddc"), {"--test-slow-load", "1200", "--load-on-main"});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue summary;
  REQUIRE_MESSAGE(summary_of(run, summary), run.output);
  const JsonValue* loading = summary.find("loading");
  REQUIRE(loading != nullptr);
  MESSAGE("loading: " << write_json(*loading, JsonWriteOptions{.pretty = false}));
  CHECK(number(*loading, "longest_gap_ms") >= 1200.0);
  CHECK(number(*loading, "job_pumps") == 0.0);
}

TEST_CASE("engine-view: --no-loading-overlay draws no line and bar, and the picture after ready "
          "is the same bytes") {
  const test::TempDir tmp("engine_view_loading_overlay");
  const std::string with = tmp.file("with.png");
  const std::string without = tmp.file("without.png");
  const Run a = window_run(tmp.file("ddc"), {"--capture", with});
  if (a.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << a.output);
    return;
  }
  REQUIRE_MESSAGE(a.exit_code == 0, a.output);
  const Run b = window_run(tmp.file("ddc"), {"--capture", without, "--no-loading-overlay"});
  REQUIRE_MESSAGE(b.exit_code == 0, b.output);
  JsonValue sa;
  JsonValue sb;
  REQUIRE(summary_of(a, sa));
  REQUIRE(summary_of(b, sb));
  REQUIRE(sa.find("loading") != nullptr);
  REQUIRE(sb.find("loading") != nullptr);
  CHECK(text(*sa.find("loading"), "overlay") == "default");
  CHECK(text(*sb.find("loading"), "overlay") == "none");
  std::string pa;
  std::string pb;
  REQUIRE(io::read_file(with, pa) == io::Status::Ok);
  REQUIRE(io::read_file(without, pb) == io::Status::Ok);
  CHECK(!pa.empty());
  CHECK(pa == pb);
}

// End to end: `profile.*` (roadmap A10; docs/subsystems/protocol.md, "profile.*: the frame capture
// over the protocol") through one engine-host fed a series of requests over --stdio, since the
// capture lives in the host. No GPU: the frames a GPU draws are render_tests.cpp's case; here the
// capture sees the host's own zones (the dispatch of every request), the refusals, the bounds, and
// the trace written into the test's scratch directory and read back.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace engine;

namespace {

constexpr i32 k_not_found = 1003;
constexpr i32 k_invalid_argument = 1004;

const std::string& host_exe() {
  static const std::string path = test::app_path(ENGINE_HOST_PATH);
  return path;
}

struct Host {
  platform::Process process;
  u32 next_id = 1;
  bool ok = false;

  Host() {
    const std::string roots = test::scratch_root_spec();
    const std::string_view argv[4] = {host_exe(), "--stdio", "--root", roots};
    std::string error;
    if (!process.spawn(std::span<const std::string_view>(argv, 4), &error)) {
      FAIL("cannot spawn engine-host: " << error);
      return;
    }
    ok = true;
  }
  ~Host() {
    if (!ok) return;
    process.close_stdin();
    (void)process.wait();
  }

  JsonValue call(std::string_view method, std::string_view params) {
    JsonValue response;
    const std::string line = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(next_id++) +
                             ",\"method\":\"" + std::string(method) +
                             "\",\"params\":" + std::string(params) + "}\n";
    if (!process.write(line)) {
      FAIL("cannot write to engine-host");
      return response;
    }
    std::string text;
    if (!process.read_line(text)) {
      FAIL("engine-host closed the connection");
      return response;
    }
    REQUIRE_MESSAGE(parse_json(text, response).ok, text);
    return response;
  }
};

i32 error_code(const JsonValue& response) {
  const JsonValue* error = response.find("error");
  if (error == nullptr) return 0;
  const JsonValue* code = error->find("code");
  i64 value = -1;
  return code != nullptr && code->get_i64(value) ? static_cast<i32>(value) : -1;
}

const JsonValue& result_of(const JsonValue& response) {
  const JsonValue* error = response.find("error");
  REQUIRE_MESSAGE(error == nullptr,
                  (error != nullptr ? write_json(*error, JsonWriteOptions{}) : std::string()));
  const JsonValue* result = response.find("result");
  REQUIRE(result != nullptr);
  return *result;
}

u64 number(const JsonValue& o, std::string_view key) {
  u64 out = 0;
  const JsonValue* v = o.find(key);
  return v != nullptr && v->get_u64(out) ? out : ~u64{0};
}

}  // namespace

TEST_CASE(
    "profile: start, report and stop over stdio, refused when out of order or out of bounds") {
  Host host;
  REQUIRE(host.ok);
  // Nothing to report before a capture, and nothing to stop.
  CHECK(error_code(host.call("profile.report", "{}")) == k_invalid_argument);
  CHECK(error_code(host.call("profile.stop", "{}")) == k_invalid_argument);
  CHECK(error_code(host.call("profile.start", R"({"capacity":100000000})")) == k_invalid_argument);

  const JsonValue start_response = host.call("profile.start", R"({"capacity":4096})");
  const JsonValue& started = result_of(start_response);
  CHECK(number(started, "capacity") == 4096);
  CHECK(number(started, "capture") == 1);
  CHECK(error_code(host.call("profile.start", "{}")) == k_invalid_argument);  // one a process
  for (int i = 0; i < 5; ++i)
    (void)host.call("engine.ping", "{}");

  // A report while it runs: no frames drawn, so the dispatch zones are outside every frame.
  const JsonValue running_response = host.call("profile.report", R"({"max_series":4})");
  const JsonValue& running = result_of(running_response);
  CHECK(running.find("format")->as_string() == "engine.profile.v1");
  bool flag = false;
  REQUIRE(running.find("running")->get_bool(flag));
  CHECK(flag);
  CHECK(number(running, "frames") == 0);
  const JsonValue* zones = running.find("zones");
  REQUIRE(zones != nullptr);
  bool dispatch = false;
  for (const JsonValue& z : zones->as_array()) {
    if (z.find("name")->as_string() != "rpc.dispatch") continue;
    dispatch = true;
    CHECK(number(z, "outside_count") >= 5);
    CHECK(number(z, "frames_seen") == 0);
  }
  CHECK(dispatch);
  CHECK(zones->as_array().size() <= 4);

  // Refused bounds leave the capture running.
  CHECK(error_code(host.call("profile.stop", R"({"max_series":65})")) == k_invalid_argument);
  CHECK(error_code(host.call("profile.stop", R"({"max_series":0})")) == k_invalid_argument);
  CHECK(error_code(host.call("profile.stop", R"({"out_dir":".","name":"../x"})")) ==
        k_invalid_argument);
  CHECK(error_code(host.call("profile.stop", R"({"out_dir":".","name":"CON"})")) ==
        k_invalid_argument);
  // The trace is written only under a writable root: before, `out_dir` was made and written
  // wherever it said, for every role, since the profile methods are reads (protocol.md, "The
  // host's file roots"). Refused as a missing place is, and the capture still running.
  {
    const JsonValue escaped =
        host.call("profile.stop", R"({"out_dir":"tests://../escape","name":"run"})");
    CHECK(error_code(escaped) == k_not_found);
    CHECK_FALSE(std::filesystem::exists(test::scratch_root() + "/../escape"));
  }

  test::TempDir dir("profile-stop");
  const std::string out = dir.path();
  std::string params = R"({"out_dir":)";
  params += write_json(JsonValue(out), JsonWriteOptions{});
  params += R"(,"name":"run"})";
  const JsonValue stop_response = host.call("profile.stop", params);
  const JsonValue& stopped = result_of(stop_response);
  REQUIRE(stopped.find("running")->get_bool(flag));
  CHECK_FALSE(flag);
  CHECK(number(stopped, "dropped_events") == 0);
  REQUIRE(stopped.find("truncated")->get_bool(flag));
  CHECK_FALSE(flag);  // a buffer that did not fill (profiler.md, "A capture that filled")
  CHECK(number(stopped, "invalid_values") == 0);
  // Answered as the root and the relative path, never the host's own path.
  const std::string trace(stopped.find("trace")->as_string());
  CHECK(trace.starts_with("tests://"));
  CHECK(trace.ends_with("/run.trace.json"));
  std::ifstream file(dir.file("run.trace.json"), std::ios::binary);
  REQUIRE(file.good());
  std::stringstream text;
  text << file.rdbuf();
  JsonValue json;
  REQUIRE(parse_json(text.str(), json).ok);
  REQUIRE(json.find("traceEvents") != nullptr);
  CHECK(json.find("otherData")->find("format")->as_string() == "engine.frame_capture.v1");

  // The stopped capture can be read again, and it is not running any more.
  const JsonValue again_response = host.call("profile.report", "{}");
  const JsonValue& again = result_of(again_response);
  CHECK(number(again, "events") == number(stopped, "events"));
  CHECK(number(again, "capture") == 1);
  CHECK(error_code(host.call("profile.stop", "{}")) == k_invalid_argument);

  // Two clients and one capture (protocol.md): a second start replaces the first's events, and a
  // call that names the first is told so, by number, and changes nothing.
  CHECK(number(result_of(host.call("profile.report", R"({"capture":1})")), "capture") == 1);
  CHECK(number(result_of(host.call("profile.start", "{}")), "capture") == 2);
  const JsonValue replaced = host.call("profile.report", R"({"capture":1})");
  CHECK(error_code(replaced) == k_not_found);
  CHECK(write_json(replaced, JsonWriteOptions{}).find("replaced by capture 2") !=
        std::string::npos);
  CHECK(error_code(host.call("profile.stop", R"({"capture":1})")) == k_not_found);
  CHECK(error_code(host.call("profile.report", R"({"capture":3})")) == k_not_found);
  const JsonValue second_response = host.call("profile.stop", R"({"capture":2})");
  const JsonValue& second = result_of(second_response);
  CHECK(number(second, "capture") == 2);
  REQUIRE(second.find("running")->get_bool(flag));
  CHECK_FALSE(flag);

  // A small capture after a default one drops where it said it would, not at the larger buffer's
  // end (batch 68's review of the capture found it kept the larger buffer): every ping is a
  // dispatch zone, so 100 of them fill 64 events.
  const JsonValue small_response = host.call("profile.start", R"({"capacity":64})");
  CHECK(number(result_of(small_response), "capacity") == 64);
  for (int i = 0; i < 100; ++i)
    (void)host.call("engine.ping", "{}");
  const JsonValue small_stop = host.call("profile.stop", "{}");
  const JsonValue& small = result_of(small_stop);
  CHECK(number(small, "events") == 64);
  CHECK(number(small, "dropped_events") > 0);
  REQUIRE(small.find("truncated")->get_bool(flag));
  CHECK(flag);
}

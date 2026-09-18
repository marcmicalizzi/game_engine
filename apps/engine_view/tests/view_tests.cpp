// End to end: engine-view renders a few frames into a real window and writes a capture. On a
// machine without a display, a Vulkan device, or mesh shaders the app exits 3 and the test
// records the skip; usage errors are checked everywhere.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run view(std::vector<std::string> args) {
  std::vector<std::string_view> argv;
  argv.push_back(ENGINE_APP_PATH);
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

}  // namespace

TEST_CASE("engine-view: usage errors exit 2") {
  CHECK(view({"--bogus"}).exit_code == 2);
  CHECK(view({"--width"}).exit_code == 2);
  CHECK(view({"--width", "abc"}).exit_code == 2);
  CHECK(view({"--grid", "1"}).exit_code == 2);
  CHECK(view({"--deform"}).exit_code == 2);
  CHECK(view({"--deform", "bogus"}).exit_code == 2);
  CHECK(view({"--deform-amplitude", "abc"}).exit_code == 2);
  const Run help = view({"--help"});
  CHECK(help.exit_code == 0);
  CHECK(help.output.find("usage: engine-view") != std::string::npos);
}

TEST_CASE("engine-view: renders frames and captures the last one") {
  const test::TempDir tmp("engine_view");
  const std::string capture = tmp.file("view.png");
  const Run run = view({"--width", "320", "--height", "200", "--frames", "6", "--grid", "33",
                        "--no-vsync", "--capture", capture});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  // The last line of output is the JSON summary; warnings may precede it on stderr.
  const usize line_start = run.output.find_last_of('\n', run.output.size() - 2);
  const std::string last = run.output.substr(line_start == std::string::npos ? 0 : line_start + 1);
  JsonValue summary;
  REQUIRE_MESSAGE(parse_json(last, summary).ok, last);
  auto number = [&](const char* key) {
    u64 v = 0;
    const JsonValue* value = summary.find(key);
    return value != nullptr && value->get_u64(v) ? v : ~u64{0};
  };
  CHECK(number("frames") == 6);
  CHECK(number("triangles") == 32 * 32 * 2);
  REQUIRE(summary.find("captured") != nullptr);
  CHECK(summary.find("captured")->as_bool());

  REQUIRE(std::filesystem::exists(capture));
  unsigned char head[24] = {};
  {
    std::ifstream f(capture, std::ios::binary);
    REQUIRE(f.is_open());
    f.read(reinterpret_cast<char*>(head), sizeof(head));
    REQUIRE(f.gcount() == static_cast<std::streamsize>(sizeof(head)));
  }
  CHECK(head[0] == 0x89);
  CHECK(head[1] == 'P');
  CHECK(head[2] == 'N');
  CHECK(head[3] == 'G');
  const u32 width = (u32{head[16]} << 24) | (u32{head[17]} << 16) | (u32{head[18]} << 8) | head[19];
  const u32 height =
      (u32{head[20]} << 24) | (u32{head[21]} << 16) | (u32{head[22]} << 8) | head[23];
  CHECK(width >= 320);  // high-DPI scaling can only enlarge the pixel size
  CHECK(height >= 200);
}

// `--deform identity` fills the per-frame vertex pool with the rest pose, so the picture and the
// cut must be exactly what the rigid run produced — the flag is wired up when nothing changes
// except the pool appearing and the pass running (experiment E25, docs/subsystems/gfx.md).
TEST_CASE("engine-view: --deform fills the vertex pool without changing the cut") {
  // No occlusion culling: its Hi-Z comes from the rasterized depth, and a deformed instance's
  // depth differs from a rigid one's in the last bit (the pool stores what the rasterizer would
  // have recomputed), which could flip a marginal cluster and make this comparison flaky.
  auto run_mode = [](const char* mode) {
    return view({"--width", "320", "--height", "200", "--frames", "8", "--grid", "65", "--orbit",
                 "22", "--no-vsync", "--no-occlusion", "--shadows", "off", "--deform", mode});
  };
  const Run rigid = run_mode("none");
  if (rigid.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << rigid.output);
    return;
  }
  const Run identity = run_mode("identity");
  REQUIRE_MESSAGE(rigid.exit_code == 0, rigid.output);
  REQUIRE_MESSAGE(identity.exit_code == 0, identity.output);
  auto summary_of = [](const Run& run) {
    const usize line_start = run.output.find_last_of('\n', run.output.size() - 2);
    const std::string last =
        run.output.substr(line_start == std::string::npos ? 0 : line_start + 1);
    JsonValue value;
    REQUIRE_MESSAGE(parse_json(last, value).ok, last);
    return value;
  };
  const JsonValue rigid_summary = summary_of(rigid);
  const JsonValue identity_summary = summary_of(identity);
  auto number = [](const JsonValue& summary, const char* key) {
    u64 v = 0;
    const JsonValue* value = summary.find(key);
    return value != nullptr && value->get_u64(v) ? v : ~u64{0};
  };
  REQUIRE(rigid_summary.find("deform") != nullptr);
  CHECK(rigid_summary.find("deform")->as_string() == "none");
  CHECK(identity_summary.find("deform")->as_string() == "identity");
  CHECK(number(rigid_summary, "deform_pool_bytes") == 0);
  CHECK(number(identity_summary, "deform_pool_bytes") > 0);
  // The same camera and the same threshold select the same clusters whether they deform or not.
  CHECK(number(identity_summary, "visible_pairs_last") ==
        number(rigid_summary, "visible_pairs_last"));
  CHECK(number(identity_summary, "visible_pairs_last") > 0);
}

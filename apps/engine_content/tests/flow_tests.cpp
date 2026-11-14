// `engine-content flow` end to end (docs/subsystems/channels.md, "The cook", "The fixture"; roadmap
// W120): the fixture written as a scene, then its three channels solved again by `flow solve` as
// an author's lines — points and a discharge, on the ground the scene draws through the tiles
// provider — and held to the fixture's own stations, which come from the closed forms.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>
#include <vector>

using namespace engine;

#if ENGINE_CONTENT_CHANNELS && ENGINE_CONTENT_TILESET
namespace {
struct Output {
  i32 code = -1;
  std::string text;
};
Output run(std::vector<std::string> args) {
  const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv{exe};
  for (const auto& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Output result;
  if (!p.spawn(argv, &error, /*merge_stderr=*/true)) {
    FAIL(error);
    return result;
  }
  p.close_stdin();
  p.read_all(result.text);
  result.code = p.wait();
  return result;
}
f64 number(const JsonValue& v) {
  f64 out = 0;
  REQUIRE(v.get_f64(out));
  return out;
}
}  // namespace
#endif

TEST_CASE("engine-content flow: the fixture, solved again as an author's lines, is the fixture") {
#if !ENGINE_CONTENT_CHANNELS || !ENGINE_CONTENT_TILESET
  MESSAGE("no channels capability or tiles ground provider in this configuration");
#else
  test::TempDir tmp("flow_fixture_cmd");
  REQUIRE(tmp.ok());
  const std::string dir = tmp.file("fixture");
  const auto made = run({"flow", "fixture", dir});
  REQUIRE_MESSAGE(made.code == 0, made.text);
  CHECK(made.text.find("\"stations\":[6,12,11]") != std::string::npos);
  std::string text;
  JsonValue scene;
  REQUIRE(io::read_file(dir + "/scene.json", text) == io::Status::Ok);
  REQUIRE(parse_json(text, scene).ok);
  const JsonValue& fixture = *scene.find("water_channels");
  REQUIRE(fixture.size() == 3);

  // n = 0.04 held, as the fixture is derived; the lattice is the heightfield's own quarter metre.
  const auto solved = run({"flow", "solve", dir + "/scene.json", tmp.file("flow.json"), "--ddc",
                           tmp.file("ddc"), "--resistance", "manning", "--reach", "6"});
  REQUIRE_MESSAGE(solved.code == 0, solved.text);
  CHECK(solved.text.find("\"authored_lines\":3") != std::string::npos);
  CHECK(solved.text.find("\"lattice_m\":0.25") != std::string::npos);
  CHECK(solved.text.find("\"falls\":1") != std::string::npos);
  CHECK(solved.text.find("\"jumps\":1") != std::string::npos);
  CHECK(solved.text.find("\"other\":0") != std::string::npos);
  CHECK(solved.text.find("\"head\":0") != std::string::npos);
  JsonValue out;
  REQUIRE(io::read_file(tmp.file("flow.json"), text) == io::Status::Ok);
  REQUIRE(parse_json(text, out).ok);
  const JsonValue& blocks = *out.find("water_channels");
  REQUIRE(blocks.size() == 3);
  for (u32 k = 0; k < 3; ++k) {
    const JsonValue& mine = blocks[k];
    const JsonValue& theirs = fixture[k];
    CAPTURE(theirs.find("name")->as_string());
    CHECK(mine.find("name")->as_string() == theirs.find("name")->as_string());
    const JsonValue& points = *mine.find("points");
    const JsonValue& flags = *mine.find("flags");
    const JsonValue& want_points = *theirs.find("points");
    const JsonValue& want_flags = *theirs.find("flags");
    // The same discontinuities and controls, in order.
    std::vector<u32> got, want;
    for (u32 i = 0; i < flags.size(); ++i)
      if (number(flags[i]) != 0) got.push_back(static_cast<u32>(number(flags[i])));
    for (u32 i = 0; i < want_flags.size(); ++i)
      if (number(want_flags[i]) != 0) want.push_back(static_cast<u32>(number(want_flags[i])));
    CHECK(got == want);
    // The level at each of the fixture's stations that is a place the solve also keeps: the
    // ends, and every flagged station but the jump's two (which stand where each solve found
    // the jump, 5 mm apart).
    const u32 n = static_cast<u32>(points.size()), m = static_cast<u32>(want_points.size());
    CHECK(number(points[0][1]) == doctest::Approx(number(want_points[0][1])).epsilon(2e-4));
    CHECK(number(points[n - 1][1]) == number(want_points[m - 1][1]));
    for (u32 i = 0; i < m; ++i) {
      const u32 f = static_cast<u32>(number(want_flags[i]));
      if (f == 0 || (f & 24u) != 0) continue;
      bool found = false;
      for (u32 j = 0; j < n && !found; ++j)
        if (static_cast<u32>(number(flags[j])) == f &&
            std::fabs(number(points[j][0]) - number(want_points[i][0])) < 1e-6) {
          found = true;
          CHECK(std::fabs(number(points[j][1]) - number(want_points[i][1])) <= 0.002);
        }
      CHECK(found);
    }
  }
#endif
}

TEST_CASE("engine-content flow: what a block declares decides what the cook does with it") {
#if !ENGINE_CONTENT_CHANNELS || !ENGINE_CONTENT_TILESET
  MESSAGE("no channels capability or tiles ground provider in this configuration");
#else
  test::TempDir tmp("flow_declares_cmd");
  REQUIRE(tmp.ok());
  const std::string dir = tmp.file("fixture");
  const auto made = run({"flow", "fixture", dir});
  REQUIRE_MESSAGE(made.code == 0, made.text);
  std::string text;
  JsonValue scene;
  REQUIRE(io::read_file(dir + "/scene.json", text) == io::Status::Ok);
  REQUIRE(parse_json(text, scene).ok);
  // A channels entry called "region", so the planner's links would be "region link <n>".
  JsonValue params = JsonValue::object();
  params.set("name", JsonValue("region"));
  params.set("seed", JsonValue(u64{3}));
  JsonValue lo = JsonValue::array(), hi = JsonValue::array();
  lo.push_back(JsonValue(4.0));
  lo.push_back(JsonValue(-24.0));
  hi.push_back(JsonValue(92.0));
  hi.push_back(JsonValue(24.0));
  params.set("region_min", std::move(lo));
  params.set("region_max", std::move(hi));
  JsonValue entry = JsonValue::object();
  entry.set("generator", JsonValue("channels"));
  entry.set("params", std::move(params));
  JsonValue placements = JsonValue::array();
  placements.push_back(std::move(entry));
  scene.set("placements", std::move(placements));
  JsonValue& channels = *scene.find("water_channels");
  REQUIRE(channels.size() == 3);
  // The author's own line, called what a planner's link is called: solved, whatever its name.
  channels[0].set("name", JsonValue("region link 0"));
  // Another writer's block: kept as it is.
  channels[1].set("derived_from", JsonValue("bridges culvert"));
  JsonWriteOptions flat;
  flat.pretty = false;
  const std::string theirs = write_json(channels[1], flat);
  // A description laid before W120 (a width, a depth and a slope a point, `fit`, no discharge
  // and nothing solved): read through the migration, and dropped.
  for (const char* gone :
       {"discharge_m3s", "speed_mps", "froude", "wet_left_m", "wet_right_m", "flags"})
    channels[2].as_object().erase(std::string(gone));
  JsonValue fit = JsonValue::array();
  for (u32 i = 0; i < channels[2].find("points")->size(); ++i)
    fit.push_back(JsonValue(u64{0}));
  channels[2].set("fit", std::move(fit));
  REQUIRE(io::write_file_atomic(dir + "/scene-mixed.json", write_json(scene, flat)) ==
          io::Status::Ok);

  const auto solved = run({"flow", "solve", dir + "/scene-mixed.json", tmp.file("flow.json"),
                           "--ddc", tmp.file("ddc"), "--resistance", "manning", "--reach", "6"});
  REQUIRE_MESSAGE(solved.code == 0, solved.text);
  CHECK(solved.text.find("\"authored_lines\":1") != std::string::npos);
  CHECK(solved.text.find("\"blocks_kept\":1") != std::string::npos);
  CHECK(solved.text.find("\"blocks_laid_dropped\":1") != std::string::npos);
  CHECK(solved.text.find("\"blocks_replaced\":0") != std::string::npos);
  JsonValue out;
  REQUIRE(io::read_file(tmp.file("flow.json"), text) == io::Status::Ok);
  REQUIRE(parse_json(text, out).ok);
  const JsonValue& blocks = *out.find("water_channels");
  bool author = false, other = false;
  for (u32 k = 0; k < blocks.size(); ++k) {
    const std::string name(blocks[k].find("name")->as_string());
    CHECK(name != "mouth");
    if (name == "step") {
      other = true;
      CHECK(write_json(blocks[k], flat) == theirs);
    }
    const JsonValue* from = blocks[k].find("derived_from");
    const bool declared = from != nullptr && from->is_string() && !from->as_string().empty();
    if (name == "region link 0" && !declared) {
      // (A link of the entry's own, were the planner to find one here, declares the entry.)
      author = true;
      CHECK(blocks[k].find("flags")->size() == blocks[k].find("points")->size());
    }
  }
  CHECK(author);
  CHECK(other);

  // Cooked again from its own output: the entry's blocks are replaced, never solved twice.
  JsonValue again = scene;
  again.set("water_channels", blocks);
  REQUIRE(io::write_file_atomic(dir + "/scene-again.json", write_json(again, flat)) ==
          io::Status::Ok);
  const auto twice = run({"flow", "solve", dir + "/scene-again.json", tmp.file("flow-2.json"),
                          "--ddc", tmp.file("ddc"), "--resistance", "manning", "--reach", "6"});
  REQUIRE_MESSAGE(twice.code == 0, twice.text);
  CHECK(twice.text.find("\"authored_lines\":1") != std::string::npos);
  CHECK(twice.text.find("\"blocks_kept\":1") != std::string::npos);
  JsonValue out2;
  REQUIRE(io::read_file(tmp.file("flow-2.json"), text) == io::Status::Ok);
  REQUIRE(parse_json(text, out2).ok);
  CHECK(out2.find("water_channels")->size() == blocks.size());
#endif
}

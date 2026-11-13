// `engine-content falls` end to end (docs/subsystems/apps.md, "engine-content falls";
// docs/subsystems/channels.md, "Where the water leaves its bed (S104)"): a valley with cliffs of
// known heights across it, handed through the tiles provider as a scene names it. The count finds
// the cliffs, the falls are cooked beside the plan once and read the second time, and the two
// runs write the same bytes.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>
#include <vector>

#if ENGINE_CONTENT_CHANNELS && ENGINE_CONTENT_TILESET
#include <domain/tileset/tileset.h>
#endif

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
  if (!p.spawn(argv, &error, /*merge_stderr=*/false)) {
    FAIL(error);
    return result;
  }
  p.close_stdin();
  p.read_all(result.text);
  result.code = p.wait();
  return result;
}
u64 count_of(const JsonValue& o, const char* key) {
  const JsonValue* v = o.find(key);
  u64 out = 0;
  return v != nullptr && v->get_u64(out) ? out : ~0ull;
}
}  // namespace
#endif

TEST_CASE("engine-content falls counts a valley's cliffs and cooks them beside the plan once") {
#if !ENGINE_CONTENT_CHANNELS || !ENGINE_CONTENT_TILESET
  MESSAGE("no channels capability or tiles ground provider in this configuration");
#else
  test::TempDir tmp("falls_cmd");
  REQUIRE(tmp.ok());
  // A valley falling 3 % toward +x, its floor at z = 128 m and its sides at 0.3, with cliffs of 7,
  // 10 and 14 m across it at x = 90, 140 and 190 m, each over one half-metre spacing.
  tileset::Heightfield field;
  field.width = 513;
  field.height = 513;
  field.spacing_mm = 500;
  const f64 xs[3] = {90, 140, 190}, hs[3] = {7, 10, 14};
  for (u32 z = 0; z < field.height; ++z)
    for (u32 x = 0; x < field.width; ++x) {
      const f64 xx = .5 * x, zz = .5 * z;
      f64 down = 0;
      for (u32 k = 0; k < 3; ++k)
        down += xx > xs[k] ? hs[k] : 0.0;
      field.data.push_back(static_cast<f32>(80 - .03 * xx - down + .3 * std::abs(zz - 128)));
    }
  std::string error;
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("valley.json"), field, error), error);
  const std::string scene =
      R"({"format":"engine.scene.v1","name":"valley","terrain":{"provider":"tiles","seed":5,)"
      R"("tiles":{"heightfield":"valley.json","ddc":"ddc","cells":32,"far_levels":0}},)"
      R"("placements":[{"generator":"channels","params":{"name":"valley","seed":1,)"
      R"("region_min":[0,0],"region_max":[256,256],"spacing_m":1,"head_threshold_m2":60,)"
      R"("width_at_km2_m":4}}]})";
  REQUIRE(io::write_file_atomic(tmp.file("scene.json"), scene) == io::Status::Ok);
  const std::string ddc = tmp.file("root");
  const auto out = run({"falls", tmp.file("scene.json"), tmp.file("a.json"), "--ddc", ddc,
                        "--min-area-m2", "0", "--paths"});
  REQUIRE_MESSAGE(out.code == 0, out.text);
  MESSAGE(out.text);
  JsonValue line;
  REQUIRE_MESSAGE(parse_json(out.text, line).ok, out.text);
  CHECK(count_of(line, "cooked") == 1);
  CHECK(count_of(line, "links") >= 1);
  CHECK(count_of(line, "falls") >= 3);
  CHECK(count_of(line, "drawn_falls") == count_of(line, "falls"));
  CHECK(count_of(line, "runs") >= count_of(line, "falls"));
  // The second run reads the falls it cooked, and writes the same file.
  const auto again = run({"falls", tmp.file("scene.json"), tmp.file("b.json"), "--ddc", ddc,
                          "--min-area-m2", "0", "--paths"});
  REQUIRE_MESSAGE(again.code == 0, again.text);
  JsonValue second;
  REQUIRE(parse_json(again.text, second).ok);
  CHECK(count_of(second, "cooked") == 0);
  CHECK(count_of(second, "planned") == 0);
  CHECK(count_of(second, "falls") == count_of(line, "falls"));
  std::string a, b;
  REQUIRE(io::read_file(tmp.file("a.json"), a) == io::Status::Ok);
  REQUIRE(io::read_file(tmp.file("b.json"), b) == io::Status::Ok);
  CHECK(a == b);
  JsonValue written;
  REQUIRE(parse_json(a, written).ok);
  const JsonValue* falls = written.find("falls");
  REQUIRE(falls != nullptr);
  CHECK(falls->as_array().size() >= count_of(line, "falls"));
  // A usage error says so.
  CHECK(run({"falls"}).code == 2);
#endif
}

// `engine-content channels --cut` end to end over a mountainside (docs/subsystems/channels.md,
// "Reaches", "Invariants"; roadmap W91): a 40 % valley with a 3 m scarp across it, the ground
// handed through the tiles provider as a scene names it. No step on any reach that is not a
// step-pool, the water's falls at the scarp alone, every block's points fitted to the ground with
// no ribbon edge over it, the summary's counts the file's, and the same bytes twice.
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
#include <core/hash/hash.h>
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

TEST_CASE(
    "engine-content channels lays a mountainside's water: no staircase, none over the ground") {
#if !ENGINE_CONTENT_CHANNELS || !ENGINE_CONTENT_TILESET
  MESSAGE("no channels capability or tiles ground provider in this configuration");
#else
  test::TempDir tmp("channel_reach_cmd");
  REQUIRE(tmp.ok());
  // A valley falling 40 % toward z = 0, its floor at x = 128 m and its sides at 100 %, a 3 m scarp
  // across it at z = 100 m: bedrock and colluvial by Montgomery & Buffington's bounds.
  tileset::Heightfield field;
  field.width = 129;
  field.height = 129;
  field.spacing_mm = 2000;
  for (u32 z = 0; z < field.height; ++z)
    for (u32 x = 0; x < field.width; ++x) {
      const f64 xx = 2.0 * x, zz = 2.0 * z;
      const f64 cliff = zz >= 102 ? 3.0 : (zz <= 100 ? 0.0 : 1.5 * (zz - 100));
      field.data.push_back(static_cast<f32>(.4 * zz + 1.0 * std::abs(xx - 128) + cliff));
    }
  std::string error;
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("face.json"), field, error), error);
  const std::string scene =
      R"({"format":"engine.scene.v1","name":"face","terrain":{"provider":"tiles","seed":5,)"
      R"("tiles":{"heightfield":"face.json","ddc":"ddc","cells":32,"far_levels":0}},)"
      R"("placements":[{"generator":"channels","params":{"name":"face","seed":2,)"
      R"("region_min":[0,0],"region_max":[256,256],"head_threshold_m2":300}}]})";
  REQUIRE(io::write_file_atomic(tmp.file("scene.json"), scene) == io::Status::Ok);
  const std::vector<std::string> args{
      "channels", tmp.file("scene.json"), tmp.file("a.json"), "--min-area-m2", "0", "--cut"};
  const auto out = run(args);
  REQUIRE_MESSAGE(out.code == 0, out.text);
  JsonValue line;
  REQUIRE_MESSAGE(parse_json(out.text, line).ok, out.text);
  MESSAGE(out.text);
  // No step on a bedrock, colluvial or cascade reach; the falls at the scarp, few; no ribbon edge
  // more than five centimetres over the ground.
  const JsonValue* steps = line.find("steps_by_reach");
  REQUIRE(steps != nullptr);
  CHECK(count_of(*steps, "bedrock") == 0);
  CHECK(count_of(*steps, "colluvial") == 0);
  CHECK(count_of(*steps, "cascade") == 0);
  CHECK(count_of(line, "knickpoints") >= 1);
  CHECK(count_of(line, "falls") >= 1);
  CHECK(count_of(line, "falls") <= count_of(line, "water_channels"));
  CHECK(count_of(line, "edges_above_005") == 0);
  CHECK(count_of(line, "ribbon_edges") > 0);
  std::string first;
  REQUIRE(io::read_file(tmp.file("a.json"), first) == io::Status::Ok);
  JsonValue written;
  REQUIRE(parse_json(first, written).ok);
  const JsonValue* blocks = written.find("water_channels");
  REQUIRE(blocks != nullptr);
  REQUIRE_FALSE(blocks->as_array().empty());
  CHECK(blocks->as_array().size() == count_of(line, "water_channels"));
  for (const JsonValue& b : blocks->as_array()) {
    const JsonValue* fit = b.find("fit");
    REQUIRE(fit != nullptr);
    CHECK(fit->as_array().size() == b.find("points")->as_array().size());
  }
  // The same bytes twice.
  std::vector<std::string> again = args;
  again[2] = tmp.file("b.json");
  REQUIRE(run(again).code == 0);
  std::string second;
  REQUIRE(io::read_file(tmp.file("b.json"), second) == io::Status::Ok);
  CHECK(hash_bytes(first.data(), first.size()) == hash_bytes(second.data(), second.size()));
#endif
}

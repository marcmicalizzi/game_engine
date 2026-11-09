// `engine-content channels` end to end (docs/subsystems/channels.md, "The water's description"):
// a scene over an authored valley, its `channels` placement run by the subcommand, and the blocks
// it writes held to the network the placement generator plans in this process — the same function
// over the same ground, so the same text.
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
#include <core/schema/json_reflect.h>
#include <domain/channels/channels.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>

#include <schemas/scene.h>
#endif

using namespace engine;
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
}  // namespace

TEST_CASE("engine-content channels writes the network's WaterChannel blocks") {
#if !ENGINE_CONTENT_CHANNELS
  const auto result = run({"channels", "scene.json", "out.json"});
  CHECK(result.code != 0);
  CHECK(result.text.find("channels capability") != std::string::npos);
#elif !ENGINE_CONTENT_TILESET
  MESSAGE("no tiles ground provider in this configuration");
#else
  test::TempDir tmp("channels_cmd");
  REQUIRE(tmp.ok());
  // A valley falling 1 % toward z = 0, its floor at x = 128 m, sides at 8 %.
  tileset::Heightfield field;
  field.width = 129;
  field.height = 257;
  field.spacing_mm = 2000;
  for (u32 z = 0; z < field.height; ++z)
    for (u32 x = 0; x < field.width; ++x) {
      const f64 xx = 2.0 * x, zz = 2.0 * z;
      field.data.push_back(static_cast<f32>(.01 * zz + .08 * std::abs(xx - 128) +
                                            .3 * std::sin(xx / 11.0) * std::cos(zz / 23.0)));
    }
  std::string error;
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("valley.json"), field, error), error);
  const std::string scene =
      R"({"format":"engine.scene.v1","name":"valley","terrain":{"provider":"tiles","seed":5,)"
      R"("tiles":{"heightfield":"valley.json","ddc":"ddc","cells":32,"far_levels":0}},)"
      R"("placements":[{"generator":"channels","params":{"name":"valley","seed":2,)"
      R"("region_min":[0,0],"region_max":[256,512],"head_threshold_m2":300}}]})";
  REQUIRE(io::write_file_atomic(tmp.file("scene.json"), scene) == io::Status::Ok);
  const auto out = run({"channels", tmp.file("scene.json"), tmp.file("out.json"), "--min-area-m2",
                        "0", "--stage", "0.5"});
  REQUIRE_MESSAGE(out.code == 0, out.text);
  CHECK(out.text.find("\"heads\"") != std::string::npos);
  std::string text;
  REQUIRE(io::read_file(tmp.file("out.json"), text) == io::Status::Ok);
  JsonValue written;
  REQUIRE(parse_json(text, written).ok);
  const JsonValue* blocks = written.find("water_channels");
  REQUIRE(blocks != nullptr);
  REQUIRE(blocks->is_array());
  REQUIRE_FALSE(blocks->as_array().empty());
  // The same network in this process, over the same provider.
  JsonValue scene_json;
  REQUIRE(parse_json(scene, scene_json).ok);
  scene::Terrain terrain;
  schema::ReadContext reader;
  REQUIRE(schema::from_json(terrain, *scene_json.find("terrain"), reader));
  const auto* provider = scene_gen::GeneratorRegistry::global().find_ground("tiles");
  REQUIRE(provider != nullptr);
  scene_gen::Context context;
  const std::string dir = tmp.path();
  context.dir = dir;
  context.world_seed = terrain.seed;
  scene_gen::GroundProvider ground;
  REQUIRE_MESSAGE(provider->make(terrain, context, ground, &error), error);
  channels::ChannelLayer layer;
  schema::ReadContext layer_reader;
  REQUIRE(
      schema::from_json(layer, *(*scene_json.find("placements"))[0].find("params"), layer_reader));
  channels::ChannelNetwork network;
  REQUIRE_MESSAGE(
      channels::plan_channels(layer, ground.view(), terrain.seed, nullptr, network, error), error);
  // W91: the subcommand fits the ribbon to the ground it planned over.
  const scene_gen::Ground view = ground.view();
  CHECK(write_json(channels::water_channels_json(layer, network, 0, .5f, false, &view)) ==
        write_json(*blocks));
  // Each block has the contract's six fields, one value per point.
  for (const JsonValue& b : blocks->as_array()) {
    const auto n = b.find("points")->as_array().size();
    CHECK(b.find("width_m")->as_array().size() == n);
    CHECK(b.find("depth_m")->as_array().size() == n);
    CHECK(b.find("slope")->as_array().size() == n);
    CHECK(b.find("name")->is_string());
    CHECK(b.find("seed") != nullptr);
  }
  // A scene with no channels placement, and a bad flag, are refused.
  REQUIRE(io::write_file_atomic(tmp.file("bare.json"),
                                R"({"format":"engine.scene.v1","name":"bare","terrain":)"
                                R"({"provider":"tiles","tiles":{"heightfield":"valley.json",)"
                                R"("ddc":"ddc","cells":32,"far_levels":0}}})") == io::Status::Ok);
  CHECK(run({"channels", tmp.file("bare.json"), tmp.file("bare-out.json")}).code == 1);
  CHECK(run({"channels", tmp.file("scene.json"), tmp.file("x.json"), "--stage", "2"}).code == 2);
#endif
}

// `engine-content channels` with a lake (docs/subsystems/channels.md, "Mouths"; roadmap W90): the
// scene's `water` bodies end the links at their shorelines. The valley of the `--cut` test, a lake
// at 1.2 m over its low end: no point of the water, the cuts or the line below the level, a mouth
// counted on the printed line, the network the in-process one with the same bodies, and the same
// text twice.
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
#include <core/schema/json_reflect.h>
#include <domain/channels/channels.h>
#include <domain/scene_gen/exclusion.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>

#include <schemas/scene.h>
#endif

using namespace engine;
#if ENGINE_CONTENT_CHANNELS && ENGINE_CONTENT_TILESET
namespace {
struct Output {
  i32 code = -1;
  std::string text;
};
Output run_mouth(std::vector<std::string> args) {
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
#endif

TEST_CASE("engine-content channels ends the links at a lake's shoreline") {
#if !ENGINE_CONTENT_CHANNELS || !ENGINE_CONTENT_TILESET
  MESSAGE("no channels capability or tiles ground provider in this configuration");
#else
  test::TempDir tmp("channel_mouth_cmd");
  REQUIRE(tmp.ok());
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
  constexpr f64 k_level = 1.2;
  const std::string scene =
      R"({"format":"engine.scene.v1","name":"valley","terrain":{"provider":"tiles","seed":5,)"
      R"("tiles":{"heightfield":"valley.json","ddc":"ddc","cells":32,"far_levels":0}},)"
      R"("water":[{"name":"lake","level_m":1.2,"shape":{"polygon":)"
      R"([[-10,0,-10],[300,0,-10],[300,0,200],[-10,0,200]]}}],)"
      R"("placements":[{"generator":"channels","params":{"name":"valley","seed":2,)"
      R"("region_min":[0,0],"region_max":[256,512],"head_threshold_m2":300}}]})";
  REQUIRE(io::write_file_atomic(tmp.file("scene.json"), scene) == io::Status::Ok);
  const std::vector<std::string> args{
      "channels", tmp.file("scene.json"), tmp.file("a.json"), "--min-area-m2", "0", "--cut"};
  const auto out = run_mouth(args);
  REQUIRE_MESSAGE(out.code == 0, out.text);
  MESSAGE(out.text);
  JsonValue line;
  REQUIRE(parse_json(out.text, line).ok);
  u64 mouths = 0, bodies = 0;
  REQUIRE(line.find("mouths") != nullptr);
  REQUIRE(line.find("mouths")->get_u64(mouths));
  REQUIRE(line.find("bodies")->get_u64(bodies));
  CHECK(bodies == 1);
  CHECK(mouths >= 1);
  std::string first, second;
  REQUIRE(io::read_file(tmp.file("a.json"), first) == io::Status::Ok);
  std::vector<std::string> again = args;
  again[2] = tmp.file("b.json");
  REQUIRE(run_mouth(again).code == 0);
  REQUIRE(io::read_file(tmp.file("b.json"), second) == io::Status::Ok);
  CHECK(hash_bytes(first.data(), first.size()) == hash_bytes(second.data(), second.size()));

  JsonValue written;
  REQUIRE(parse_json(first, written).ok);
  // Nothing of any link under the lake: every cut's top and every water point at or above the
  // level wherever the lake's outline holds it.
  u32 checked = 0;
  for (const char* key : {"cuts", "water_channels"})
    for (const JsonValue& b : written.find(key)->as_array())
      for (const JsonValue& at : b.find("points")->as_array()) {
        f64 y = 0, z = 0;
        REQUIRE(at[1].get_f64(y));
        REQUIRE(at[2].get_f64(z));
        if (z < 200) {
          CHECK(y >= k_level - 2e-3);
          ++checked;
        }
      }
  CHECK(checked > 0);
  // The in-process network over the same ground and bodies writes the same text.
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
  scene::WaterBody body;
  schema::ReadContext body_reader;
  REQUIRE(schema::from_json(body, (*scene_json.find("water"))[0], body_reader));
  scene_gen::Exclusion water;
  REQUIRE(water.build({}, {&body, 1}, {}, &error));
  channels::ChannelLayer layer;
  schema::ReadContext layer_reader;
  REQUIRE(
      schema::from_json(layer, *(*scene_json.find("placements"))[0].find("params"), layer_reader));
  channels::ChannelNetwork network;
  REQUIRE_MESSAGE(
      channels::plan_channels(layer, ground.view(), terrain.seed, nullptr, network, error, &water),
      error);
  CHECK(network.water.mouths == mouths);
  CHECK(write_json(channels::terrain_cuts_json(layer, network)) ==
        write_json(*written.find("cuts")));
  // W91: the subcommand fits the ribbon to the ground it planned over, the body's points left.
  const scene_gen::Ground view = ground.view();
  CHECK(write_json(channels::water_channels_json(layer, network, 0, .5f, true, &view, &water)) ==
        write_json(*written.find("water_channels")));
#endif
}

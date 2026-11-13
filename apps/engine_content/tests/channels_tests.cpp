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
#include <filesystem>
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

// **The commands write under `--ddc` and nowhere else** (T119; scene_gen.md, "The derived-data
// root"; the forest's engine-needs 46): `channels`, `routes` and `bridges`, each run from a working
// directory that is a checkout of its own (it holds an `AGENTS.md`, as the forest's does) over a
// scene in a third directory whose `tiles` block names a `ddc` of its own, with `--ddc` naming a
// fourth. Before T119 `routes` built the ground's set under the scene's `tiles.ddc` and `channels`
// did too, and a command given a bare scene path rooted its plans at the working directory's.
TEST_CASE("engine-content channels, routes and bridges write under --ddc, never the working dir") {
#if !ENGINE_CONTENT_CHANNELS || !ENGINE_CONTENT_TILESET
  MESSAGE("no channels or no tiles ground provider in this configuration");
#else
  test::TempDir tmp("content_roots");
  REQUIRE(tmp.ok());
  const std::string working = tmp.file("working"), scenes = tmp.file("scenes"),
                    host = tmp.file("host/ddc");
  std::filesystem::create_directories(working);
  std::filesystem::create_directories(scenes);
  REQUIRE(io::write_file_atomic(working + "/AGENTS.md", "x") == io::Status::Ok);
  tileset::Heightfield field;
  field.width = 65;
  field.height = 129;
  field.spacing_mm = 2000;
  for (u32 z = 0; z < field.height; ++z)
    for (u32 x = 0; x < field.width; ++x) {
      const f64 xx = 2.0 * x, zz = 2.0 * z;
      field.data.push_back(static_cast<f32>(.01 * zz + .08 * std::abs(xx - 64)));
    }
  std::string error;
  REQUIRE_MESSAGE(tileset::write_heightfield(scenes + "/valley.json", field, error), error);
  const std::string terrain =
      R"("terrain":{"provider":"tiles","seed":5,)"
      R"("tiles":{"heightfield":"valley.json","ddc":"ddc","cells":32,"far_levels":0}})";
  const std::string scene =
      R"({"format":"engine.scene.v1","name":"valley",)" + terrain +
      R"(,"placements":[{"generator":"channels","params":{"name":"valley","seed":2,)"
      R"("region_min":[0,0],"region_max":[128,256],"head_threshold_m2":300}},)"
      R"({"generator":"routes","params":{"name":"path","region_min":[0,0],"region_max":[128,256],)"
      R"("routes":[{"name":"a","waypoints":[[20,0,30],[110,0,220]]}]}}]})";
  REQUIRE(io::write_file_atomic(scenes + "/scene.json", scene) == io::Status::Ok);
  const std::filesystem::path before = std::filesystem::current_path();
  std::filesystem::current_path(working);
  struct Back {
    std::filesystem::path to;
    ~Back() {
      std::error_code ec;
      std::filesystem::current_path(to, ec);
    }
  } back{before};
  const auto nothing_beside = [&](const char* what) {
    INFO(what);
    CHECK_FALSE(std::filesystem::exists(working + "/ddc"));
    CHECK_FALSE(std::filesystem::exists(scenes + "/ddc"));
  };

  const auto channels = run({"channels", scenes + "/scene.json", tmp.file("channels.json"),
                             "--min-area-m2", "0", "--ddc", host});
  REQUIRE_MESSAGE(channels.code == 0, channels.text);
  CHECK(std::filesystem::exists(host + "/channels"));
  nothing_beside("channels");

  const auto routes =
      run({"routes", scenes + "/scene.json", tmp.file("routes.json"), "--ddc", host});
  if (routes.code != 0 && routes.text.find("capability") != std::string::npos) {
    MESSAGE("no routes capability in this configuration");
    return;
  }
  REQUIRE_MESSAGE(routes.code == 0, routes.text);
  CHECK(std::filesystem::exists(host + "/routes"));
  nothing_beside("routes");

  // The bridges over the scene with the channels' water and the route's plan named.
  std::string text;
  JsonValue water, planned;
  REQUIRE(io::read_file(tmp.file("channels.json"), text) == io::Status::Ok);
  REQUIRE(parse_json(text, water).ok);
  REQUIRE(io::read_file(tmp.file("routes.json"), text) == io::Status::Ok);
  REQUIRE(parse_json(text, planned).ok);
  const JsonValue* blocks = water.find("water_channels");
  const JsonValue* plans = planned.find("plans");
  REQUIRE(blocks != nullptr);
  REQUIRE(plans != nullptr);
  REQUIRE(plans->size() == 1);
  const std::string key((*plans)[0].find("plan")->as_string());
  const std::string bridged =
      R"({"format":"engine.scene.v1","name":"valley",)" + terrain + R"(,"water_channels":)" +
      write_json(*blocks) +
      R"(,"placements":[{"generator":"routes","params":{"name":"path","region_min":[0,0],)"
      R"("region_max":[128,256],"plan":")" +
      key + R"(","routes":[{"name":"a","waypoints":[[20,0,30],[110,0,220]]}]}}]})";
  REQUIRE(io::write_file_atomic(scenes + "/bridged.json", bridged) == io::Status::Ok);
  const auto bridges =
      run({"bridges", scenes + "/bridged.json", tmp.file("bridges.json"), "--ddc", host});
  if (bridges.code != 0 && bridges.text.find("capability") != std::string::npos) {
    MESSAGE("no bridges capability in this configuration");
    return;
  }
  REQUIRE_MESSAGE(bridges.code == 0, bridges.text);
  nothing_beside("bridges");
#endif
}

// `engine-content channels --cut` end to end (docs/subsystems/channels.md, "The cut"; roadmap W89):
// the terrain's cuts written beside the WaterChannel blocks, held to the network planned in this
// process, the same text twice, and the same again from a scene that already carries them (the
// network is planned over the ground before its cuts), with the water's surface inside the cut.
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
#endif

TEST_CASE("engine-content channels --cut writes the terrain's cuts beside the water") {
#if !ENGINE_CONTENT_CHANNELS || !ENGINE_CONTENT_TILESET
  MESSAGE("no channels capability or tiles ground provider in this configuration");
#else
  test::TempDir tmp("channel_cut_cmd");
  REQUIRE(tmp.ok());
  // The valley of the channels command's own test: falling 1 % toward z = 0, its floor at x = 128
  // m.
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
  const std::string head =
      R"({"format":"engine.scene.v1","name":"valley","terrain":{"provider":"tiles","seed":5,)"
      R"("tiles":{"heightfield":"valley.json","ddc":"ddc","cells":32,"far_levels":0)";
  const std::string tail =
      R"(}},"placements":[{"generator":"channels","params":{"name":"valley","seed":2,)"
      R"("region_min":[0,0],"region_max":[256,512],"head_threshold_m2":300}}]})";
  const std::string scene = head + tail;
  REQUIRE(io::write_file_atomic(tmp.file("scene.json"), scene) == io::Status::Ok);
  const std::vector<std::string> args{
      "channels", tmp.file("scene.json"), tmp.file("a.json"), "--min-area-m2", "0", "--cut"};
  const auto out = run(args);
  REQUIRE_MESSAGE(out.code == 0, out.text);
  CHECK(out.text.find("\"cuts\"") != std::string::npos);
  std::string first;
  REQUIRE(io::read_file(tmp.file("a.json"), first) == io::Status::Ok);
  // The same hash twice.
  std::vector<std::string> again = args;
  again[2] = tmp.file("b.json");
  REQUIRE(run(again).code == 0);
  std::string second;
  REQUIRE(io::read_file(tmp.file("b.json"), second) == io::Status::Ok);
  CHECK(hash_bytes(first.data(), first.size()) == hash_bytes(second.data(), second.size()));

  JsonValue written;
  REQUIRE(parse_json(first, written).ok);
  const JsonValue* cuts = written.find("cuts");
  const JsonValue* blocks = written.find("water_channels");
  REQUIRE(cuts != nullptr);
  REQUIRE(blocks != nullptr);
  REQUIRE_FALSE(cuts->as_array().empty());
  // The same network in this process: the cuts and the water in the cut are its.
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
  CHECK(write_json(channels::terrain_cuts_json(layer, network)) == write_json(*cuts));
  // W91: the subcommand fits the ribbon to the ground it planned over.
  const scene_gen::Ground view = ground.view();
  CHECK(write_json(channels::water_channels_json(layer, network, 0, .5f, true, &view)) ==
        write_json(*blocks));

  // A scene that carries the cuts: its ground is cut, and the command writes the same file, since
  // it plans over the ground before them.
  const std::string cut_scene = head + R"(,"cuts":)" + write_json(*cuts) + tail;
  REQUIRE(io::write_file_atomic(tmp.file("cut.json"), cut_scene) == io::Status::Ok);
  std::vector<std::string> from_cut = args;
  from_cut[1] = tmp.file("cut.json");
  from_cut[2] = tmp.file("c.json");
  const auto again_out = run(from_cut);
  REQUIRE_MESSAGE(again_out.code == 0, again_out.text);
  std::string third;
  REQUIRE(io::read_file(tmp.file("c.json"), third) == io::Status::Ok);
  CHECK(third == first);
  // And the cut scene's ground is lower than the plain one's at a cut's line, by its depth there.
  JsonValue cut_json;
  REQUIRE(parse_json(cut_scene, cut_json).ok);
  scene::Terrain cut_terrain;
  schema::ReadContext cut_reader;
  REQUIRE(schema::from_json(cut_terrain, *cut_json.find("terrain"), cut_reader));
  REQUIRE(cut_reader.ok());
  REQUIRE(cut_terrain.tiles.has_value());
  REQUIRE(cut_terrain.tiles->cuts.size() == cuts->as_array().size());
  scene_gen::GroundProvider cut_ground;
  REQUIRE_MESSAGE(provider->make(cut_terrain, context, cut_ground, &error), error);
  const scene::TerrainCut& c = cut_terrain.tiles->cuts.back();
  const u32 mid = c.points.size() / 2;
  const i64 x = scene_gen::nearest_mm(c.points[mid].x), z = scene_gen::nearest_mm(c.points[mid].z);
  const f64 plain = ground.view().surface_mm(x, z), cut = cut_ground.view().surface_mm(x, z);
  MESSAGE("at the middle of the last cut, the ground " << plain << " m, the cut ground " << cut
                                                       << " m, the cut's depth " << c.depth_m[mid]
                                                       << " m");
  // The valley's points are 2 m apart: a channel wider than that is cut into them, one narrower
  // closes on them (the cell rule; tileset.md, "Channels cut into the ground").
  if (c.width_m[mid] > 2.0f) {
    CHECK(cut < plain);
    CHECK(cut <= c.points[mid].y - static_cast<f64>(c.depth_m[mid]) + 0.001);  // metres
  } else {
    CHECK(cut == plain);
  }
  // Somewhere the scene's ground is cut or every channel is narrower than the points' spacing.
  bool any_wide = false;
  for (const scene::TerrainCut& k : cut_terrain.tiles->cuts)
    for (const f32 w : k.width_m)
      any_wide = any_wide || w > 2.0f;
  MESSAGE("a channel wider than the valley's 2 m points: " << std::string(any_wide ? "yes" : "no"));
#endif
}

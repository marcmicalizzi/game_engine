// `engine-content flow` (docs/subsystems/channels.md, "The water's description", "The fixture";
// roadmap W120): a stream's water from its discharge.
//
//   flow fixture <dir>
//
// writes the flow fixture as a scene a host loads: `<dir>/ground.json` and its grid (an
// `engine.terrain.heightfield/1`: three trenches under a flat cap), and `<dir>/scene.json`, whose
// terrain is that heightfield through the `tiles` provider and whose `water` and `water_channels`
// are the fixture's two bodies and three solved channels — every number from the closed forms
// (`channels::make_flow_fixture`), none from the solver. Prints the derivation's numbers as one
// JSON line. Exit 0, 1 when a file cannot be written, 2 for usage.
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_CHANNELS && ENGINE_CONTENT_TILESET
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/channels/flow_fixture.h>
#include <domain/tileset/tileset.h>
#include <foundation/io/vfs.h>

#include <schemas/scene.h>
#include <string>
#include <string_view>
#endif

namespace engine::content {

#if ENGINE_CONTENT_CHANNELS && ENGINE_CONTENT_TILESET
namespace {

const char* k_flow_usage =
    "usage: engine-content flow fixture <dir>\n"
    "  fixture: writes <dir>/ground.json (+ its grid) and <dir>/scene.json, the flow fixture: a\n"
    "           uniform reach, a step with a fall and a jump into a pool, a reach into a lake,\n"
    "           each channel's stations from the closed forms; prints the derivation as JSON\n";

int flow_fail(const std::string& why) {
  std::fprintf(stderr, "flow: %s\n", why.c_str());
  return 1;
}

int flow_fixture(const std::string& dir) {
  channels::FlowFixture fixture;
  std::string error;
  if (!channels::make_flow_fixture(fixture, error)) return flow_fail(error);
  if (io::make_directories(dir) != io::Status::Ok) return flow_fail("cannot make " + dir);
  tileset::Heightfield field;
  field.width = fixture.width;
  field.height = fixture.height;
  field.spacing_mm = fixture.spacing_mm;
  field.origin_x_mm = fixture.origin_x_mm;
  field.origin_z_mm = fixture.origin_z_mm;
  field.data = fixture.heights;
  if (!tileset::write_heightfield(dir + "/ground.json", field, error)) return flow_fail(error);

  JsonValue scene = JsonValue::object();
  scene["format"] = JsonValue("engine.scene.v1");
  scene["name"] = JsonValue("flow-fixture");
  JsonValue tiles = JsonValue::object();
  tiles["heightfield"] = JsonValue("ground.json");
  tiles["cells"] = JsonValue(u64{64});
  tiles["far_levels"] = JsonValue(u64{0});
  JsonValue terrain = JsonValue::object();
  terrain["provider"] = JsonValue("tiles");
  terrain["seed"] = JsonValue(u64{1});
  terrain["tiles"] = std::move(tiles);
  scene["terrain"] = std::move(terrain);
  JsonValue bodies = JsonValue::array();
  for (const scene::WaterBody& body : fixture.bodies)
    bodies.push_back(schema::to_json(body));
  scene["water"] = std::move(bodies);
  JsonValue blocks = JsonValue::array();
  for (const scene::WaterChannel& channel : fixture.channels)
    blocks.push_back(schema::to_json(channel));
  scene["water_channels"] = std::move(blocks);
  JsonWriteOptions pretty;
  pretty.pretty = true;
  if (io::write_file_atomic(dir + "/scene.json", write_json(scene, pretty) + "\n") !=
      io::Status::Ok)
    return flow_fail("cannot write " + dir + "/scene.json");

  const channels::FlowFixtureNumbers& n = fixture.numbers;
  JsonValue line = JsonValue::object();
  line["scene"] = JsonValue(dir + "/scene.json");
  line["discharge_m3s"] = JsonValue(n.discharge_m3s);
  line["manning_n"] = JsonValue(n.manning_n);
  line["slope"] = JsonValue(n.slope);
  line["normal_m"] = JsonValue(n.normal_m);
  line["critical_m"] = JsonValue(n.critical_m);
  line["normal_froude"] = JsonValue(n.normal_froude);
  JsonValue step = JsonValue::object();
  step["crest_x_m"] = JsonValue(n.crest_x_m);
  step["drop_m"] = JsonValue(n.drop_m);
  step["approach_head_m"] = JsonValue(n.approach_head_m);
  step["foot_x_m"] = JsonValue(n.foot_x_m);
  step["foot_m"] = JsonValue(n.foot_m);
  step["foot_froude"] = JsonValue(n.foot_froude);
  step["jump_head_x_m"] = JsonValue(n.jump_head_x_m);
  step["jump_head_m"] = JsonValue(n.jump_head_m);
  step["jump_head_froude"] = JsonValue(n.jump_head_froude);
  step["jump_foot_x_m"] = JsonValue(n.jump_foot_x_m);
  step["jump_foot_m"] = JsonValue(n.jump_foot_m);
  step["jump_length_m"] = JsonValue(n.jump_length_m);
  step["pool_mouth_m"] = JsonValue(n.pool_mouth_m);
  step["pool_level_m"] = JsonValue(n.pool_level_m);
  line["step"] = std::move(step);
  JsonValue mouth = JsonValue::object();
  mouth["lake_level_m"] = JsonValue(n.lake_level_m);
  mouth["mouth_m"] = JsonValue(n.lake_mouth_m);
  mouth["head_m"] = JsonValue(n.lake_head_m);
  line["mouth"] = std::move(mouth);
  JsonValue stations = JsonValue::array(), nodes = JsonValue::array();
  for (u32 k = 0; k < 3; ++k) {
    stations.push_back(JsonValue(u64{n.stations[k]}));
    nodes.push_back(JsonValue(u64{n.nodes[k]}));
  }
  line["stations"] = std::move(stations);
  line["nodes"] = std::move(nodes);
  JsonWriteOptions flat;
  flat.pretty = false;
  std::fprintf(stdout, "%s\n", write_json(line, flat).c_str());
  return 0;
}

}  // namespace
#endif

int flow_command(int argc, char** argv) {
#if !ENGINE_CONTENT_CHANNELS || !ENGINE_CONTENT_TILESET
  (void)argc;
  (void)argv;
  std::fprintf(stderr,
               "flow requires the channels capability and the tiles ground provider "
               "(ENGINE_WITH_CHANNELS, ENGINE_WITH_TILESET)\n");
  return 1;
#else
  // argv[1] is "flow", argv[2] the subcommand.
  if (argc == 4 && std::string_view(argv[2]) == "fixture") return flow_fixture(argv[3]);
  std::fputs(k_flow_usage, stderr);
  return 2;
#endif
}

}  // namespace engine::content

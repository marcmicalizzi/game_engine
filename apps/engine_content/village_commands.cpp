// `engine-content village` (docs/subsystems/apps.md, "engine-content village"; village.md, "In a
// scene"): the plan a scene's `village` placements make over its terrain, for a person to read and
// for the three things a scene's author pastes or keeps beside the scene — the wear (each lane's
// verge, worn margin and tread, each house's door yard) as terrain mask regions naming the palette
// layers `verge`, `worn` and `lane` (a planned mask region is not handed across), the plots'
// boundaries as the lines of a `walls` entry, and a camera path that goes in at a house's door,
// through its rooms, up its stair and to a window. It calls the functions the
// placement generator calls, over the ground the scene's terrain names before any edit, found
// through the same registry the renderer finds it through.
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_VILLAGE
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/village/flight.h>
#include <domain/village/village.h>
#include <foundation/io/vfs.h>

#include <charconv>
#include <schemas/scene.h>
#include <string>
#endif

namespace engine::content {

int village_command(int argc, char** argv) {
#if !ENGINE_CONTENT_VILLAGE
  (void)argc;
  (void)argv;
  std::fprintf(stderr, "village requires the village capability (ENGINE_WITH_VILLAGE)\n");
  return 1;
#else
  const char* usage =
      "usage: engine-content village <scene.json> <out.json> [--ddc <dir>]\n"
      "                              [--house <index>] [--flight <camera-path.json>]\n";
  if (argc < 4) {
    std::fputs(usage, stderr);
    return 2;
  }
  const std::string scene_path = argv[2], out_path = argv[3];
  std::string ddc, flight_path;
  i64 house = 0;
  for (int i = 4; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (i + 1 >= argc) {
      std::fputs(usage, stderr);
      return 2;
    }
    const std::string_view value = argv[++i];
    bool ok = true;
    if (flag == "--ddc") {
      ddc = std::string(value);
    } else if (flag == "--flight") {
      flight_path = std::string(value);
    } else if (flag == "--house") {
      const auto r = std::from_chars(value.data(), value.data() + value.size(), house);
      ok = r.ec == std::errc{} && r.ptr == value.data() + value.size() && house >= 0;
    } else {
      ok = false;
    }
    if (!ok) {
      std::fprintf(stderr, "village: invalid %.*s\n%s", static_cast<int>(flag.size()), flag.data(),
                   usage);
      return 2;
    }
  }
  const auto fail = [](const std::string& e) {
    std::fprintf(stderr, "village: %s\n", e.c_str());
    return 1;
  };
  std::string text;
  if (io::read_file(scene_path, text) != io::Status::Ok) return fail("cannot read " + scene_path);
  JsonValue json;
  const auto parsed = parse_json(text, json);
  if (!parsed.ok)
    return fail(scene_path + ":" + std::to_string(parsed.line) + ": " + parsed.message);
  const JsonValue* terrain_json = json.find("terrain");
  if (terrain_json == nullptr) return fail(scene_path + " has no terrain to stand a village on");
  scene::Terrain terrain;
  schema::ReadContext reader;
  if (!schema::from_json(terrain, *terrain_json, reader) || !reader.ok()) {
    std::string message = scene_path + ": terrain";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return fail(message);
  }
  terrain.masks.reset();
  // **The ground before any edit**: the plan stands on the heights its pads are benched into.
  if (terrain.tiles.has_value()) terrain.tiles->edits.clear();
  const std::string provider =
      !terrain.provider.empty()
          ? terrain.provider
          : (terrain.generator == scene::TerrainGenerator::Dunes ? "dunes" : "waves");
  const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
  const scene_gen::GroundProviderDesc* found = registry.find_ground(provider);
  if (found == nullptr) return fail("the terrain " + registry.unknown_ground(provider));
  const std::string dir(io::parent_path(scene_path));
  scene_gen::Context context;
  context.world_seed = terrain.seed;
  context.dir = dir;
  context.ddc = ddc;
  scene_gen::GroundProvider ground;
  std::string error;
  if (!found->make(terrain, context, ground, &error) || !ground.valid())
    return fail(error.empty() ? "the ground provider \"" + provider + "\" refused the terrain"
                              : error);
  context.ground = ground.view();
  Vector<scene::WaterBody> bodies;
  Vector<scene::WaterChannel> channels;
  if (const JsonValue* water = json.find("water"); water != nullptr && water->is_array())
    for (const JsonValue& b : water->as_array()) {
      scene::WaterBody body;
      schema::ReadContext body_reader;
      if (schema::from_json(body, b, body_reader) && body_reader.ok())
        bodies.push_back(std::move(body));
    }
  if (const JsonValue* list = json.find("water_channels"); list != nullptr && list->is_array())
    for (const JsonValue& c : list->as_array()) {
      scene::WaterChannel channel;
      schema::ReadContext channel_reader;
      if (schema::from_json(channel, c, channel_reader) && channel_reader.ok())
        channels.push_back(std::move(channel));
    }
  scene_gen::Exclusion water;
  if (!water.build({}, {bodies.data(), bodies.size()}, {channels.data(), channels.size()}, &error))
    return fail(error);
  if (!water.empty()) context.exclusion = &water;

  JsonValue villages = JsonValue::array(), regions = JsonValue::array();
  JsonValue boundaries = JsonValue::array(), wall_lines = JsonValue::array();
  u32 entries = 0, houses = 0, plots = 0, edits = 0, shapes = 0;
  bool flown = false;
  const JsonValue* placements = json.find("placements");
  if (placements != nullptr && placements->is_array()) {
    for (u32 k = 0; k < placements->as_array().size(); ++k) {
      const JsonValue& entry = (*placements)[k];
      const JsonValue* generator = entry.find("generator");
      if (generator == nullptr || !generator->is_string() || generator->as_string() != "village")
        continue;
      const JsonValue* params = entry.find("params");
      village::VillageLayer layer;
      schema::ReadContext layer_reader;
      if (params == nullptr || !schema::from_json(layer, *params, layer_reader) ||
          !layer_reader.ok()) {
        std::string message =
            scene_path + ": placements " + std::to_string(k) + ": engine.village.VillageLayer";
        for (const auto& d : layer_reader.diagnostics)
          message += "; " + d.path + ": " + d.message;
        return fail(message);
      }
      village::VillagePlan plan;
      if (!village::plan_scene_village(layer, context, plan, error))
        return fail(scene_path + ": placements " + std::to_string(k) + ": " + error);
      JsonValue summary = village::plan_json(layer, plan);
      summary["entry"] = JsonValue(k);
      villages.push_back(std::move(summary));
      const JsonValue outlines = village::ground_regions_json(layer, plan);
      for (const JsonValue& r : outlines.as_array())
        regions.push_back(r);
      // The plots' boundaries: the lines themselves, and the same as the lines of a `walls` entry
      // (engine.walls.Layer: centimetres along a line for a gateway) for an author to paste.
      Vector<village::BoundaryLine> bounds;
      village::plot_boundaries(layer, plan, bounds);
      for (const village::BoundaryLine& b : bounds) {
        JsonValue points = JsonValue::array(), gates_mm = JsonValue::array(),
                  gates_cm = JsonValue::array();
        for (const i64 v : b.points_mm)
          points.push_back(JsonValue(v));
        for (const i64 g : b.gateways_mm) {
          gates_mm.push_back(JsonValue(g));
          gates_cm.push_back(JsonValue((g + 5) / 10));
        }
        JsonValue line = JsonValue::object();
        line["plot"] = JsonValue(b.plot);
        line["points_mm"] = points;
        line["closed"] = JsonValue(b.closed);
        line["gateways_mm"] = std::move(gates_mm);
        boundaries.push_back(std::move(line));
        JsonValue wall = JsonValue::object();
        wall["points_mm"] = std::move(points);
        wall["closed"] = JsonValue(b.closed);
        wall["gateways_cm"] = std::move(gates_cm);
        wall_lines.push_back(std::move(wall));
      }
      Vector<scene::TerrainEdit> planned;
      village::ground_edits(layer, plan, planned);
      scene_gen::FootprintList occupied;
      village::footprints(layer, plan, occupied);
      ++entries;
      houses += plan.built;
      plots += plan.plots.size();
      edits += planned.size();
      shapes += occupied.size();
      // The flight: through the first village entry's house `--house` names.
      if (!flight_path.empty() && !flown) {
        JsonValue path;
        if (!village::flight_json(layer, plan, static_cast<u32>(house), path, error))
          return fail(scene_path + ": placements " + std::to_string(k) + ": " + error);
        if (io::write_file_atomic(flight_path, write_json(path)) != io::Status::Ok)
          return fail("cannot write " + flight_path);
        flown = true;
      }
    }
  }
  if (entries == 0) return fail(scene_path + " has no \"village\" placement to plan");
  JsonValue out = JsonValue::object();
  const u32 region_count = regions.as_array().size();
  out["villages"] = std::move(villages);
  out["mask_regions"] = std::move(regions);
  const u32 boundary_count = boundaries.as_array().size();
  out["boundaries"] = std::move(boundaries);
  // A toft's wall: breast high, a gateway a person and a barrow pass (the grounding's "The wear
  // and the boundaries"), and a base narrowed with the height so the batter stays in the wallers'
  // 1 in 6 to 1 in 10 (walls.md: 11 cm a side over the 75.5 cm under the cover is 1 in 6.9);
  // every other number is the walls capability's default.
  JsonValue wall_params = JsonValue::object();
  wall_params["seed"] = JsonValue(static_cast<u64>(1));
  wall_params["height_cm"] = JsonValue(static_cast<u64>(110));
  wall_params["base_width_cm"] = JsonValue(static_cast<u64>(62));
  wall_params["gateway_cm"] = JsonValue(static_cast<u64>(150));
  wall_params["lines"] = std::move(wall_lines);
  JsonValue walls = JsonValue::object();
  walls["generator"] = JsonValue("walls");
  walls["params"] = std::move(wall_params);
  out["walls"] = std::move(walls);
  const std::string written = write_json(out);
  if (io::write_file_atomic(out_path, written) != io::Status::Ok)
    return fail("cannot write " + out_path);
  JsonValue line = JsonValue::object();
  line["command"] = JsonValue("village");
  line["scene"] = JsonValue(scene_path);
  line["out"] = JsonValue(out_path);
  line["entries"] = JsonValue(entries);
  line["houses"] = JsonValue(houses);
  line["plots"] = JsonValue(plots);
  line["edits"] = JsonValue(edits);
  line["shapes"] = JsonValue(shapes);
  line["mask_regions"] = JsonValue(region_count);
  line["boundaries"] = JsonValue(boundary_count);
  if (flown) line["flight"] = JsonValue(flight_path);
  JsonWriteOptions compact;
  compact.pretty = false;
  std::printf("%s\n", write_json(line, compact).c_str());
  return 0;
#endif
}

}  // namespace engine::content

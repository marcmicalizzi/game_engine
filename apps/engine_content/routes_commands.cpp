// `engine-content routes` (docs/subsystems/apps.md, "engine-content"; docs/subsystems/routes.md,
// "In a scene"): the footpaths a scene's `routes` placements plan over its terrain, written as the
// treads the tile set benches into its heights — `engine.scene.TerrainEdit` lines of mode `Bench`
// for the `tiles` block's `edits` — with the treads' material as terrain mask regions and each
// entry's plan key, for the scene's author to paste. It calls the one function the placement
// generator calls (`routes::ensure_plan`), over the ground the scene's terrain names before any
// edit, found through the same registry the renderer finds it through.
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_ROUTES
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/routes/routes.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <schemas/scene.h>
#include <string>
#endif

namespace engine::content {

int routes_command(int argc, char** argv) {
#if !ENGINE_CONTENT_ROUTES
  (void)argc;
  (void)argv;
  std::fprintf(stderr, "routes requires the routes capability (ENGINE_WITH_ROUTES)\n");
  return 1;
#else
  const char* usage =
      "usage: engine-content routes <scene.json> <out.json> [--ddc <dir>] [--entry <index>]\n";
  if (argc < 4) {
    std::fputs(usage, stderr);
    return 2;
  }
  const std::string scene_path = argv[2], out_path = argv[3];
  std::string ddc;
  i64 only = -1;
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
    } else if (flag == "--entry") {
      const auto r = std::from_chars(value.data(), value.data() + value.size(), only);
      ok = r.ec == std::errc{} && r.ptr == value.data() + value.size() && only >= 0;
    } else {
      ok = false;
    }
    if (!ok) {
      std::fprintf(stderr, "routes: invalid %.*s\n%s", static_cast<int>(flag.size()), flag.data(),
                   usage);
      return 2;
    }
  }
  const auto fail = [](const std::string& e) {
    std::fprintf(stderr, "routes: %s\n", e.c_str());
    return 1;
  };
  std::string text;
  if (io::read_file(scene_path, text) != io::Status::Ok) return fail("cannot read " + scene_path);
  JsonValue json;
  const auto parsed = parse_json(text, json);
  if (!parsed.ok)
    return fail(scene_path + ":" + std::to_string(parsed.line) + ": " + parsed.message);
  const JsonValue* terrain_json = json.find("terrain");
  if (terrain_json == nullptr) return fail(scene_path + " has no terrain to walk");
  scene::Terrain terrain;
  schema::ReadContext reader;
  if (!schema::from_json(terrain, *terrain_json, reader) || !reader.ok()) {
    std::string message = scene_path + ": terrain";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return fail(message);
  }
  terrain.masks.reset();
  // **The ground before any edit**: a route is planned over the heights its treads are benched
  // into, never over a ground they (or a channel's cut) already changed, so a second run over a
  // scene that carries them writes the same treads again.
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
  scene_gen::GroundProvider ground;
  std::string error;
  if (!found->make(terrain, context, ground, &error) || !ground.valid())
    return fail(error.empty() ? "the ground provider \"" + provider + "\" refused the terrain"
                              : error);
  context.ground = ground.view();
  // **Water** (routes.md, "Water"): the scene's water bodies and channels, read as the placements'
  // exclusion reads them, so a route never enters a body and fords a stream short. A block that
  // does not parse is the water capability's to refuse and is left out here.
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
  context.exclusion = &water;
  std::string plan_dir;
  if (!routes::derived_dir(ddc, dir, plan_dir, error)) return fail(error);

  JsonValue edits = JsonValue::array(), regions = JsonValue::array(), plans = JsonValue::array();
  JsonValue keys = JsonValue::array(), crossings = JsonValue::array();
  u32 entries = 0, planned = 0, lines = 0, legs = 0, steep_legs = 0, unreached = 0;
  u32 switchbacks = 0, bands = 0;
  f64 metres = 0, over_cap = 0, held = 0, plan_s = 0, ford_m = 0;
  const JsonValue* placements = json.find("placements");
  if (placements != nullptr && placements->is_array()) {
    for (u32 k = 0; k < placements->as_array().size(); ++k) {
      const JsonValue& entry = (*placements)[k];
      const JsonValue* generator = entry.find("generator");
      if (generator == nullptr || !generator->is_string() || generator->as_string() != "routes")
        continue;
      if (only >= 0 && static_cast<i64>(k) != only) continue;
      const JsonValue* params = entry.find("params");
      routes::RouteLayer layer;
      schema::ReadContext layer_reader;
      if (params == nullptr || !schema::from_json(layer, *params, layer_reader) ||
          !layer_reader.ok()) {
        std::string message =
            scene_path + ": placements " + std::to_string(k) + ": engine.routes.RouteLayer";
        for (const auto& d : layer_reader.diagnostics)
          message += "; " + d.path + ": " + d.message;
        return fail(message);
      }
      // A pasted entry names last time's plan; this run keys it again from the ground before its
      // treads, as it clears the edits above.
      layer.plan.clear();
      routes::RoutePlan plan;
      const auto began = std::chrono::steady_clock::now();
      if (!routes::ensure_plan(layer, context, plan_dir, plan, error))
        return fail(scene_path + ": placements " + std::to_string(k) + ": " + error);
      plan_s += std::chrono::duration<f64>(std::chrono::steady_clock::now() - began).count();
      planned += plan.planned ? 1u : 0u;
      const routes::RouteNetwork& n = plan.network;
      // Held by name: a range over a temporary's member would outlive the temporary.
      const JsonValue these = routes::tread_edits_json(layer, n);
      for (const JsonValue& e : these.as_array())
        edits.push_back(e);
      const JsonValue outlines = routes::tread_regions_json(layer, n);
      for (const JsonValue& r : outlines.as_array())
        regions.push_back(r);
      const JsonValue fords = routes::crossings_json(n);
      for (const JsonValue& f : fords.as_array()) {
        JsonValue c = f;
        c["entry"] = JsonValue(k);
        crossings.push_back(std::move(c));
      }
      ford_m += n.ford_m;
      Vector<scene_gen::Footprint> footprints;
      routes::tread_footprints(layer, n, footprints);
      bands += footprints.size();
      JsonValue named = JsonValue::object();
      named["entry"] = JsonValue(k);
      named["name"] = JsonValue(layer.name);
      named["plan"] = JsonValue(routes::plan_key_text(plan.key));
      plans.push_back(std::move(named));
      keys.push_back(JsonValue(routes::plan_key_text(plan.key)));
      ++entries;
      lines += n.lines.size();
      legs += n.legs;
      steep_legs += n.steep_legs;
      unreached += n.unreached;
      switchbacks += n.switchbacks;
      metres += n.length_m;
      over_cap += n.over_cap_m;
      for (const routes::RouteLine& l : n.lines)
        held = std::max(held, l.max_hold_m);
    }
  }
  if (entries == 0) return fail(scene_path + " has no \"routes\" placement to run");
  JsonValue out = JsonValue::object();
  const u32 edit_count = edits.as_array().size(), region_count = regions.as_array().size();
  out["edits"] = std::move(edits);
  out["plans"] = std::move(plans);
  if (region_count > 0) out["mask_regions"] = std::move(regions);
  // Where a route fords a stream: a bridge's or stepping stones' site (row G147's to read).
  const u32 crossing_count = crossings.as_array().size();
  out["crossings"] = std::move(crossings);
  const std::string written = write_json(out);
  if (io::write_file_atomic(out_path, written) != io::Status::Ok)
    return fail("cannot write " + out_path);
  JsonValue line = JsonValue::object();
  line["command"] = JsonValue("routes");
  line["scene"] = JsonValue(scene_path);
  line["out"] = JsonValue(out_path);
  line["entries"] = JsonValue(entries);
  line["plans"] = std::move(keys);
  line["planned"] = JsonValue(planned);
  line["plan_s"] = JsonValue(std::round(plan_s * 1000) / 1000);
  line["legs"] = JsonValue(legs);
  line["steep_legs"] = JsonValue(steep_legs);
  line["unreached"] = JsonValue(unreached);
  line["lines"] = JsonValue(lines);
  line["route_m"] = JsonValue(std::round(metres * 10) / 10);
  line["over_cap_m"] = JsonValue(std::round(over_cap * 10) / 10);
  line["switchbacks"] = JsonValue(switchbacks);
  line["max_hold_m"] = JsonValue(std::round(held * 1000) / 1000);
  line["edits"] = JsonValue(edit_count);
  line["mask_regions"] = JsonValue(region_count);
  line["bands"] = JsonValue(bands);
  line["bodies"] = JsonValue(static_cast<u32>(bodies.size()));
  line["channels"] = JsonValue(static_cast<u32>(channels.size()));
  line["crossings"] = JsonValue(crossing_count);
  line["ford_m"] = JsonValue(std::round(ford_m * 10) / 10);
  line["out_bytes"] = JsonValue(static_cast<u64>(written.size()));
  JsonWriteOptions compact;
  compact.pretty = false;
  std::printf("%s\n", write_json(line, compact).c_str());
  return 0;
#endif
}

}  // namespace engine::content

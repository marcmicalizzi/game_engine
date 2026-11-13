// `engine-content bridges` (docs/subsystems/apps.md, "engine-content"; docs/subsystems/bridges.md,
// "In a scene"): the footbridges a scene's routes need where they ford its streams. For every
// crossing a `routes` entry's plan lists (routes.md, "Water"), it reads the stream's section there
// from the scene's `water_channels` (and, where the scene's `channels` entry names its plan, the
// bed's D84), samples the ground along the path through it, and writes a `bridges` placement entry
// of those crossings for the scene's author to paste. Beside it, each bridge's approaches as
// `Fill` ground edits and each bridge's footprint as a polygon, to read: the entry hands both to
// the host itself (`plan_ground` and its tiles' shapes, G186), so neither is pasted.
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_BRIDGES
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/bridges/bridges.h>
#include <domain/routes/routes.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>
#if ENGINE_CONTENT_CHANNELS
#include <domain/channels/channels.h>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <schemas/scene.h>
#include <string>
#endif

namespace engine::content {

#if ENGINE_CONTENT_BRIDGES
namespace {

// The stream under a point: the nearest segment of any channel's line, and where on it.
struct Under {
  bool found = false;
  f64 x = 0, z = 0;    // the nearest point of the line, metres
  f64 tx = 0, tz = 0;  // the line's direction there (unit)
  f64 width = 0, depth = 0, surface = 0;
};

Under stream_under(const Vector<scene::WaterChannel>& list, f64 px, f64 pz) {
  Under best;
  f64 best_d = 1e300;
  for (const scene::WaterChannel& ch : list) {
    const u32 n = ch.points.size();
    if (n < 2 || ch.width_m.size() != n || ch.depth_m.size() != n) continue;
    for (u32 i = 0; i + 1 < n; ++i) {
      const WorldPos a = ch.points[i], b = ch.points[i + 1];
      const f64 dx = b.x - a.x, dz = b.z - a.z, len2 = dx * dx + dz * dz;
      if (!(len2 > 0)) continue;
      const f64 t = std::clamp(((px - a.x) * dx + (pz - a.z) * dz) / len2, 0.0, 1.0);
      const f64 qx = a.x + t * dx, qz = a.z + t * dz;
      const f64 d = (px - qx) * (px - qx) + (pz - qz) * (pz - qz);
      if (d >= best_d) continue;
      best_d = d;
      const f64 len = std::sqrt(len2);
      best.found = true;
      best.x = qx;
      best.z = qz;
      best.tx = dx / len;
      best.tz = dz / len;
      best.width =
          static_cast<f64>(ch.width_m[i]) * (1 - t) + static_cast<f64>(ch.width_m[i + 1]) * t;
      best.depth =
          static_cast<f64>(ch.depth_m[i]) * (1 - t) + static_cast<f64>(ch.depth_m[i + 1]) * t;
      best.surface = a.y * (1 - t) + b.y * t;
    }
  }
  return best;
}

}  // namespace
#endif

int bridges_command(int argc, char** argv) {
#if !ENGINE_CONTENT_BRIDGES
  (void)argc;
  (void)argv;
  std::fprintf(stderr,
               "bridges requires the bridges and routes capabilities (ENGINE_WITH_BRIDGES, "
               "ENGINE_WITH_ROUTES)\n");
  return 1;
#else
  const char* usage =
      "usage: engine-content bridges <scene.json> <out.json> [--ddc <dir>] [--grade <permille>]\n";
  if (argc < 4) {
    std::fputs(usage, stderr);
    return 2;
  }
  const std::string scene_path = argv[2], out_path = argv[3];
  std::string ddc;
  i64 grade_override = -1;
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
    } else if (flag == "--grade") {
      const auto r = std::from_chars(value.data(), value.data() + value.size(), grade_override);
      ok = r.ec == std::errc{} && r.ptr == value.data() + value.size() && grade_override > 0 &&
           grade_override <= 1000;
    } else {
      ok = false;
    }
    if (!ok) {
      std::fprintf(stderr, "bridges: invalid %.*s\n%s", static_cast<int>(flag.size()), flag.data(),
                   usage);
      return 2;
    }
  }
  const auto fail = [](const std::string& e) {
    std::fprintf(stderr, "bridges: %s\n", e.c_str());
    return 1;
  };
  std::string text;
  if (io::read_file(scene_path, text) != io::Status::Ok) return fail("cannot read " + scene_path);
  JsonValue json;
  const auto parsed = parse_json(text, json);
  if (!parsed.ok)
    return fail(scene_path + ":" + std::to_string(parsed.line) + ": " + parsed.message);
  const JsonValue* terrain_json = json.find("terrain");
  if (terrain_json == nullptr) return fail(scene_path + " has no terrain");
  scene::Terrain terrain;
  schema::ReadContext reader;
  if (!schema::from_json(terrain, *terrain_json, reader) || !reader.ok()) {
    std::string message = scene_path + ": terrain";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return fail(message);
  }
  terrain.masks.reset();
  // **The ground with the channels' cuts and the routes' treads in it and no bridge's approach**:
  // a second run over a scene that carries this command's edits writes the same ones again.
  if (terrain.tiles.has_value()) {
    Vector<scene::TerrainEdit> kept;
    for (scene::TerrainEdit& e : terrain.tiles->edits) {
      const std::string_view name = e.name;
      const bool ours = name.size() > 9 && name.substr(name.size() - 9) == " approach" &&
                        name.substr(0, 8) == "bridges ";
      if (!ours) kept.push_back(std::move(e));
    }
    terrain.tiles->edits = std::move(kept);
  }
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
  std::string error;
  const JsonValue* placements = json.find("placements");
  if (placements == nullptr || !placements->is_array())
    return fail(scene_path + " has no placements");
  // **What the other entries plan for the ground** (scene_gen.md, "What an entry plans for the
  // ground"): a `routes` entry's treads, a mouth's apron, handed through their hook. The scene's
  // reader adds them to the tile set's edits before it makes the ground, so this command does too,
  // each entry opened on the ground before any edit as the reader opens it; a `bridges` entry's
  // own approaches are left out, as the pasted ones are above.
  if (terrain.tiles.has_value()) {
    scene::Terrain unedited = terrain;
    unedited.tiles->edits.clear();
    scene_gen::GroundProvider before;
    if (!found->make(unedited, context, before, &error) || !before.valid())
      return fail(error.empty() ? "the ground before its edits could not be made" : error);
    scene_gen::Context planning = context;
    planning.ground = before.view();
    for (u32 k = 0; k < placements->as_array().size(); ++k) {
      const JsonValue& entry = (*placements)[k];
      const JsonValue* generator = entry.find("generator");
      const JsonValue* params = entry.find("params");
      if (generator == nullptr || !generator->is_string() || params == nullptr ||
          generator->as_string() == "bridges")
        continue;
      const scene_gen::PlacementGeneratorDesc* desc =
          registry.find_placement(generator->as_string());
      if (desc == nullptr || desc->plan_ground == nullptr) continue;
      const std::string where = scene_path + ": placements " + std::to_string(k);
      planning.where = where;
      void* state = nullptr;
      if (!desc->open(*params, planning, &state, &error)) return fail(where + ": " + error);
      scene_gen::GroundPlan plan;
      const bool planned = desc->plan_ground(state, planning, plan, &error);
      desc->close(state);
      if (!planned) return fail(where + ": " + error);
      for (scene::TerrainEdit& edit : plan.edits)
        terrain.tiles->edits.push_back(std::move(edit));
    }
  }
  scene_gen::GroundProvider ground;
  if (!found->make(terrain, context, ground, &error) || !ground.valid())
    return fail(error.empty() ? "the ground provider \"" + provider + "\" refused the terrain"
                              : error);
  context.ground = ground.view();
  Vector<scene::WaterChannel> streams;
  if (const JsonValue* list = json.find("water_channels"); list != nullptr && list->is_array())
    for (const JsonValue& c : list->as_array()) {
      scene::WaterChannel channel;
      schema::ReadContext channel_reader;
      if (schema::from_json(channel, c, channel_reader) && channel_reader.ok())
        streams.push_back(std::move(channel));
    }
  if (streams.empty()) return fail(scene_path + " has no water_channels to bridge");

#if ENGINE_CONTENT_CHANNELS
  // The bed's sediment, from the channels entry's named plan: read, never planned here (the
  // ground this command reads already has the cuts in it).
  Vector<channels::ChannelPlan> beds;
  for (const JsonValue& entry : placements->as_array()) {
    const JsonValue* generator = entry.find("generator");
    const JsonValue* params = entry.find("params");
    if (generator == nullptr || !generator->is_string() || generator->as_string() != "channels" ||
        params == nullptr)
      continue;
    channels::ChannelLayer layer;
    schema::ReadContext layer_reader;
    if (!schema::from_json(layer, *params, layer_reader) || !layer_reader.ok() ||
        layer.plan.empty())
      continue;
    std::string channel_dir;
    channels::ChannelPlan plan;
    if (channels::derived_dir(ddc, dir, channel_dir, error) &&
        channels::ensure_plan(layer, context, channel_dir, plan, error))
      beds.push_back(std::move(plan));
  }
#endif
  std::string route_dir;
  if (!routes::derived_dir(ddc, dir, route_dir, error)) return fail(error);

  bridges::Layer out_layer;
  out_layer.seed = terrain.seed != 0 ? terrain.seed : 1;
  u32 grade = 0;
  const auto began = std::chrono::steady_clock::now();
  for (u32 k = 0; k < placements->as_array().size(); ++k) {
    const JsonValue& entry = (*placements)[k];
    const JsonValue* generator = entry.find("generator");
    if (generator == nullptr || !generator->is_string() || generator->as_string() != "routes")
      continue;
    const JsonValue* params = entry.find("params");
    routes::RouteLayer layer;
    schema::ReadContext layer_reader;
    if (params == nullptr || !schema::from_json(layer, *params, layer_reader) || !layer_reader.ok())
      return fail(scene_path + ": placements " + std::to_string(k) + ": engine.routes.RouteLayer");
    if (layer.plan.empty())
      return fail(scene_path + ": placements " + std::to_string(k) +
                  ": the routes entry names no plan; run `engine-content routes` and paste it");
    routes::RoutePlan plan;
    if (!routes::ensure_plan(layer, context, route_dir, plan, error))
      return fail(scene_path + ": placements " + std::to_string(k) + ": " + error);
    // The approaches keep the path's own grade cap: they are its tread.
    grade =
        std::max(grade, static_cast<u32>(std::lround(static_cast<f64>(layer.grade_cap) * 1000.0)));
    const JsonValue fords = routes::crossings_json(plan.network);
    for (const JsonValue& f : fords.as_array()) {
      const u32 line = static_cast<u32>(f.find("line")->as_uint());
      const u32 from = static_cast<u32>(f.find("from")->as_uint());
      const u32 to = static_cast<u32>(f.find("to")->as_uint());
      const Vector<routes::Station>& st = plan.network.lines[line].stations;
      const u32 mid = (from + to) / 2;
      const u32 a = from > 0 ? from - 1 : from, b = to + 1 < st.size() ? to + 1 : to;
      const f64 px = st[mid].at.x, pz = st[mid].at.z;
      const Under u = stream_under(streams, px, pz);
      if (!u.found) continue;
      // Square to the stream, the way the path goes.
      f64 nx = -u.tz, nz = u.tx;
      if (nx * (st[b].at.x - st[a].at.x) + nz * (st[b].at.z - st[a].at.z) < 0) {
        nx = -nx;
        nz = -nz;
      }
      bridges::Crossing c;
      c.route = k;
      c.line = line;
      c.station = mid;
      c.x_mm = scene_gen::nearest_mm(u.x);
      c.z_mm = scene_gen::nearest_mm(u.z);
      c.dir_x = static_cast<i32>(std::lround(nx * 10000.0));
      c.dir_z = static_cast<i32>(std::lround(nz * 10000.0));
      c.width_mm = static_cast<u32>(std::clamp<i64>(scene_gen::nearest_mm(u.width), 300, 20000));
      c.water_mm = scene_gen::nearest_mm(u.surface);
      c.bed_mm = scene_gen::nearest_mm(u.surface - u.depth);
      c.tread_mm = static_cast<u32>(
          std::clamp<i64>(scene_gen::nearest_mm(static_cast<f64>(layer.tread_width_m)), 300, 3000));
#if ENGINE_CONTENT_CHANNELS
      f64 nearest = 1e300;
      for (const channels::ChannelPlan& bed : beds)
        for (const channels::ChannelLink& link : bed.network.links)
          for (u32 i = 0; i < link.line.size() && i < link.sediment.size(); ++i) {
            const f64 dx = link.line[i].at.x - u.x, dz = link.line[i].at.z - u.z;
            if (dx * dx + dz * dz < nearest) {
              nearest = dx * dx + dz * dz;
              c.d84_mm =
                  static_cast<u32>(scene_gen::nearest_mm(static_cast<f64>(link.sediment[i].d84_m)));
            }
          }
#endif
      c.profile_step_mm = 250;
      const i64 reach = static_cast<i64>(c.width_mm) / 2 + 6000;
      const i64 n = reach / 250;
      for (i64 i = -n; i <= n; ++i) {
        i64 wx = 0, wz = 0;
        bridges::world_mm(c, i * 250, 0, wx, wz);
        c.profile_mm.push_back(scene_gen::nearest_mm(context.ground.surface_mm(wx, wz)));
      }
      out_layer.crossings.push_back(std::move(c));
    }
  }
  if (grade_override > 0) grade = static_cast<u32>(grade_override);
  if (grade == 0) grade = 250;
  out_layer.approach_grade_permille = grade;
  // What each crossing became, its triangles, and its footprint.
  u32 by_form[5] = {};
  u32 triangles = 0, steps = 0, stairs = 0;
  JsonValue footprints = JsonValue::array();
  for (const bridges::Crossing& c : out_layer.crossings) {
    bridges::Bridge b;
    b.crossing = c;
    b.seed = bridges::crossing_seed(out_layer.seed, c);
    bridges::MeshPart parts[bridges::k_materials];
    bridges::BridgePlan plan;
    bridges::BridgeReport report;
    if (!bridges::solve_bridge(b, parts, plan, report, error)) return fail(error);
    ++by_form[static_cast<u32>(plan.form)];
    triangles += report.triangles;
    const bridges::Approach approach = bridges::approach_of(c, plan, grade);
    steps = std::max<u32>(steps, static_cast<u32>(approach.step_mm));
    stairs += approach.steps[0] + approach.steps[1];
    scene_gen::FootprintPoint corners[4];
    if (bridges::bridge_footprint(c, plan, corners).point_count == 0) continue;
    JsonValue polygon = JsonValue::array();
    for (const scene_gen::FootprintPoint& p : corners) {
      JsonValue at = JsonValue::array();
      at.push_back(JsonValue(static_cast<f64>(p.x_mm) / 1000.0));
      at.push_back(JsonValue(static_cast<f64>(p.z_mm) / 1000.0));
      polygon.push_back(std::move(at));
    }
    footprints.push_back(std::move(polygon));
  }
  const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - began).count();
  JsonValue edits = bridges::approach_edits_json(out_layer, grade);
  const u32 edit_count = edits.as_array().size();
  JsonValue placement = JsonValue::object();
  placement["generator"] = JsonValue("bridges");
  placement["params"] = schema::to_json(out_layer);
  JsonValue out = JsonValue::object();
  out["placement"] = std::move(placement);
  out["edits"] = std::move(edits);
  out["footprints"] = std::move(footprints);
  const std::string written = write_json(out);
  if (io::write_file_atomic(out_path, written) != io::Status::Ok)
    return fail("cannot write " + out_path);
  JsonValue line = JsonValue::object();
  line["command"] = JsonValue("bridges");
  line["scene"] = JsonValue(scene_path);
  line["out"] = JsonValue(out_path);
  line["crossings"] = JsonValue(static_cast<u32>(out_layer.crossings.size()));
  line["stones"] = JsonValue(by_form[0]);
  line["clapper"] = JsonValue(by_form[1]);
  line["log"] = JsonValue(by_form[2]);
  line["stringers"] = JsonValue(by_form[3]);
  line["fords"] = JsonValue(by_form[4]);
  line["triangles"] = JsonValue(triangles);
  line["approaches"] = JsonValue(edit_count);
  line["grade_permille"] = JsonValue(grade);
  line["largest_step_mm"] = JsonValue(steps);
  line["approach_steps"] = JsonValue(stairs);
  line["solve_s"] = JsonValue(std::round(seconds * 1000) / 1000);
  line["out_bytes"] = JsonValue(static_cast<u64>(written.size()));
  JsonWriteOptions compact;
  compact.pretty = false;
  std::printf("%s\n", write_json(line, compact).c_str());
  return 0;
#endif
}

}  // namespace engine::content

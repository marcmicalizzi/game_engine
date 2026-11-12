// `engine-content channels` (docs/subsystems/apps.md, "engine-content";
// docs/subsystems/channels.md, "The water's description"): the network a scene's `channels`
// placements make over its terrain, written as the water's channel description —
// `engine.scene.WaterChannel` blocks under `water_channels` — for the scene's author to paste into
// the scene the water draws. It calls the one function the placement generator calls
// (`channels::plan_channels`), over the ground provider the scene's terrain names, found through
// the same registry the renderer finds it through. Beside them it writes the terrain's `cuts`
// (`engine.scene.TerrainCut`, the `tiles` block's; W89): the channels cut into the ground, which
// the water's surface then lies inside, with `--cut` (without it the surface stands on the ground
// as W86 laid it, and no cuts are written).
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_CHANNELS
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/channels/channels.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <charconv>
#include <chrono>
#include <cmath>
#include <schemas/scene.h>
#include <string>
#endif

namespace engine::content {

int channels_command(int argc, char** argv) {
#if !ENGINE_CONTENT_CHANNELS
  (void)argc;
  (void)argv;
  std::fprintf(stderr, "channels requires the channels capability (ENGINE_WITH_CHANNELS)\n");
  return 1;
#else
  const char* usage =
      "usage: engine-content channels <scene.json> <out.json> [--ddc <dir>] [--min-area-m2 <m2>]\n"
      "                               [--stage <fraction>] [--entry <placement index>] [--cut]\n"
      "                               [--measure <water.json>]\n";
  if (argc < 4) {
    std::fputs(usage, stderr);
    return 2;
  }
  const std::string scene_path = argv[2], out_path = argv[3];
  std::string ddc, measure_path;
  f64 min_area = 50000.0;
  f64 stage = .5;
  i64 only = -1;
  bool cut = false;
  for (int i = 4; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag == "--cut") {
      cut = true;
      continue;
    }
    if (i + 1 >= argc) {
      std::fputs(usage, stderr);
      return 2;
    }
    const std::string_view value = argv[++i];
    auto number = [&](f64& out) {
      const auto r = std::from_chars(value.data(), value.data() + value.size(), out);
      return r.ec == std::errc{} && r.ptr == value.data() + value.size() && std::isfinite(out);
    };
    bool ok = true;
    if (flag == "--ddc") {
      ddc = std::string(value);
    } else if (flag == "--measure") {
      measure_path = std::string(value);
    } else if (flag == "--min-area-m2") {
      ok = number(min_area) && min_area >= 0;
    } else if (flag == "--stage") {
      ok = number(stage) && stage > 0 && stage <= 1;
    } else if (flag == "--entry") {
      f64 v = 0;
      ok = number(v) && v >= 0 && v == std::floor(v);
      only = static_cast<i64>(v);
    } else {
      ok = false;
    }
    if (!ok) {
      std::fprintf(stderr, "channels: invalid %.*s\n%s", static_cast<int>(flag.size()), flag.data(),
                   usage);
      return 2;
    }
  }
  const auto fail = [](const std::string& e) {
    std::fprintf(stderr, "channels: %s\n", e.c_str());
    return 1;
  };
  std::string text;
  if (io::read_file(scene_path, text) != io::Status::Ok) return fail("cannot read " + scene_path);
  JsonValue json;
  const auto parsed = parse_json(text, json);
  if (!parsed.ok)
    return fail(scene_path + ":" + std::to_string(parsed.line) + ": " + parsed.message);
  const JsonValue* terrain_json = json.find("terrain");
  if (terrain_json == nullptr) return fail(scene_path + " has no terrain to drain");
  scene::Terrain terrain;
  schema::ReadContext reader;
  if (!schema::from_json(terrain, *terrain_json, reader) || !reader.ok()) {
    std::string message = scene_path + ": terrain";
    for (const auto& d : reader.diagnostics)
      message += "; " + d.path + ": " + d.message;
    return fail(message);
  }
  // The network asks the heights alone: the masks (which a host resolves against its water bodies
  // before it builds them) are left out, so the provider never builds them here.
  terrain.masks.reset();
  // **The ground as it was before any cut** (W89): the network is planned over the heights the cuts
  // are made from, never over a ground they already lowered, so writing the cuts of a scene that
  // carries them gives the same cuts again.
  if (terrain.tiles.has_value()) terrain.tiles->edits.clear();
  // The provider the renderer would draw this terrain with (renderer::terrain_provider's rule).
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
  // **One root, the host's** (T90, T112; channels.md, "In a scene"): `--ddc` is where
  // the uncut ground's set and the plan go, over the scene's own `tiles.ddc`, as a host given the
  // same `--ddc` reads them; without it, the scene's own (its `tiles.ddc` for the set, the
  // repository's `ddc/` above the scene or `ddc/` beside it for the plan), as a host given none.
  context.ddc = ddc;
  scene_gen::GroundProvider ground;
  std::string error;
  if (!found->make(terrain, context, ground, &error) || !ground.valid())
    return fail(error.empty() ? "the ground provider \"" + provider + "\" refused the terrain"
                              : error);
  // **One plan** (W93; channels.md, "One plan"): the network is a derived file under the
  // derived-data root's `channels/`, keyed by what it is made from, so a second run over the same
  // ground and layer plans nothing, and the placement generator reads the same file at a load.
  std::string plan_dir;
  if (!channels::derived_dir(ddc, dir, plan_dir, error)) return fail(error);
  // **The water bodies** (W90; channels.md, "Mouths"): the scene's `water` blocks, read as the
  // placement generators' exclusion reads them, so a link ends at the shoreline it meets. A block
  // that does not parse is the water capability's to refuse and is left out here.
  Vector<scene::WaterBody> bodies;
  if (const JsonValue* water = json.find("water"); water != nullptr && water->is_array()) {
    for (const JsonValue& b : water->as_array()) {
      scene::WaterBody body;
      schema::ReadContext body_reader;
      if (schema::from_json(body, b, body_reader) && body_reader.ok())
        bodies.push_back(std::move(body));
    }
  }
  const scene_gen::Ground ground_view = ground.view();
  scene_gen::Exclusion shores;
  if (!shores.build({}, {bodies.data(), bodies.size()}, {}, &error)) return fail(error);
  context.ground = ground_view;
  context.exclusion = &shores;
  JsonValue plans = JsonValue::array();
  u32 planned = 0;
  f64 plan_seconds = 0;
  const JsonValue* placements = json.find("placements");
  JsonValue all = JsonValue::array();
  JsonValue cuts = JsonValue::array();
  JsonValue regions = JsonValue::array();
  u32 entries = 0, heads = 0, links = 0;
  channels::ChannelWaterCounts shore_counts;
  channels::RibbonTrim trim;  // the ribbons' ends a body covers (W91 continuation)
  u32 reaches[7] = {};
  u64 steps[7] = {}, boulders = 0, clasts = 0, clast_tiles = 0, clusters = 0, knickpoints = 0,
      steep_left = 0, pulled = 0;
  u64 bank_moved = 0;
  f64 lip_convexity = 0;
  f64 region[4] = {1e300, 1e300, -1e300, -1e300}, border = 0;
  f64 metres = 0, area = 0;
  // W93: the section's summary (below).
  struct {
    u64 pools = 0;
    f64 flood_m = 0, flood_m2 = 0;
    f64 line_m = 0, half_bank_m = 0, no_bank_m = 0, no_bank_fall = 0;
    f64 by_angle_m[5] = {}, half_bank_by_angle_m[5] = {}, no_bank_by_angle_m[5] = {};
  } section;
  if (placements != nullptr && placements->is_array()) {
    for (u32 k = 0; k < placements->as_array().size(); ++k) {
      const JsonValue& entry = (*placements)[k];
      const JsonValue* generator = entry.find("generator");
      if (generator == nullptr || !generator->is_string() || generator->as_string() != "channels")
        continue;
      if (only >= 0 && static_cast<i64>(k) != only) continue;
      const JsonValue* params = entry.find("params");
      channels::ChannelLayer layer;
      schema::ReadContext layer_reader;
      if (params == nullptr || !schema::from_json(layer, *params, layer_reader) ||
          !layer_reader.ok()) {
        std::string message =
            scene_path + ": placements " + std::to_string(k) + ": engine.channels.ChannelLayer";
        for (const auto& d : layer_reader.diagnostics)
          message += "; " + d.path + ": " + d.message;
        return fail(message);
      }
      // The key a pasted entry carries names the plan made last time; this run keys it again from
      // the ground before its cuts, as it clears the cuts above.
      layer.plan.clear();
      channels::ChannelPlan plan;
      const auto began = std::chrono::steady_clock::now();
      if (!channels::ensure_plan(layer, context, plan_dir, plan, error))
        return fail(scene_path + ": placements " + std::to_string(k) + ": " + error);
      plan_seconds += std::chrono::duration<f64>(std::chrono::steady_clock::now() - began).count();
      planned += plan.planned ? 1u : 0u;
      const channels::ChannelNetwork& network = plan.network;
      {
        JsonValue named = JsonValue::object();
        named["entry"] = JsonValue(k);
        named["name"] = JsonValue(layer.name);
        named["plan"] = JsonValue(channels::plan_key_text(plan.key));
        plans.push_back(std::move(named));
      }
      // W91: the ribbon fitted to the ground the network was planned over, and the bodies.
      const JsonValue blocks = channels::water_channels_json(
          layer, network, min_area, static_cast<f32>(stage), cut, &ground_view, &shores, &trim);
      region[0] = std::min(region[0], layer.region_min[0]);
      region[1] = std::min(region[1], layer.region_min[1]);
      region[2] = std::max(region[2], layer.region_max[0]);
      region[3] = std::max(region[3], layer.region_max[1]);
      border = std::max(border, 2.0 * static_cast<f64>(layer.spacing_m));
      for (const JsonValue& b : blocks.as_array())
        all.push_back(b);
      if (cut) {
        const JsonValue c = channels::terrain_cuts_json(layer, network);
        for (const JsonValue& b : c.as_array())
          cuts.push_back(b);
        // The beds' outlines for the terrain's masks, where the layer names their material.
        const JsonValue r = channels::bed_regions_json(layer, network);
        for (const JsonValue& b : r.as_array())
          regions.push_back(b);
      }
      ++entries;
      shore_counts.mouths += network.water.mouths;
      shore_counts.dropped += network.water.dropped;
      shore_counts.outlets += network.water.outlets;
      shore_counts.confluences += network.water.confluences;
      shore_counts.points_removed += network.water.points_removed;
      for (const channels::ChannelLink& l : network.links) {
        for (u8 r : l.reach)
          ++reaches[r < 7 ? r : 6];
        for (const u32 i : l.step_at)
          ++steps[i < l.reach.size() && l.reach[i] < 7 ? l.reach[i] : 6];
        clusters += l.cluster_at.size();
        knickpoints += l.knickpoints;
        lip_convexity += l.lip_convexity;
        steep_left += l.steep_left;
        pulled += l.trough_pulled;
        bank_moved += l.bank_moved;
        // **W93** (channels.md, "The flood bed", "A line across a slope"): the metres of line
        // with a flood bed and the bed's area; the pools (a stretch on one side whose asymmetry
        // reaches half a bend's); and, stretch by stretch, how the line stands on its ground —
        // the ground at the two bank tops (half the width across the line each way) against the
        // cut's bed, and the line's heading against the ground's fall line over a width.
        const u32 kept = std::min<u32>(l.keep, l.line.size());
        bool in_pool = false;
        for (u32 i = 0; i + 1 < kept; ++i) {
          const channels::ChannelPoint& a = l.line[i];
          const channels::ChannelPoint& b = l.line[i + 1];
          const f64 dx = b.at.x - a.at.x, dz = b.at.z - a.at.z;
          const f64 run = std::sqrt(dx * dx + dz * dz);
          if (!(run > 0)) continue;
          const f64 asym = i < l.asym.size() ? static_cast<f64>(l.asym[i]) : 0.0;
          if (std::fabs(asym) >= .5 && !in_pool) ++section.pools;
          in_pool = std::fabs(asym) >= .5 || (in_pool && asym != 0);
          if (i < l.flood_left.size() && l.flood_left[i] > 0) {
            section.flood_m += run;
            section.flood_m2 +=
                run * (static_cast<f64>(l.flood_left[i]) + static_cast<f64>(l.flood_right[i]));
          }
          const f64 w = static_cast<f64>(a.width_m);
          if (w < 1.0 || i >= l.bed.size()) continue;
          const f64 tx = dx / run, tz = dz / run;
          const auto at = [&](f64 x, f64 z) {
            return ground_view.surface_mm(scene_gen::nearest_mm(x), scene_gen::nearest_mm(z));
          };
          const f64 depth = a.at.y - l.bed[i];
          const f64 lower = std::min(at(a.at.x - tz * .5 * w, a.at.z + tx * .5 * w),
                                     at(a.at.x + tz * .5 * w, a.at.z - tx * .5 * w));
          // The ground's gradient over a width, and the cosine of the line's angle to its fall.
          const f64 gx = (at(a.at.x + w, a.at.z) - at(a.at.x - w, a.at.z)) / (2 * w);
          const f64 gz = (at(a.at.x, a.at.z + w) - at(a.at.x, a.at.z - w)) / (2 * w);
          const f64 g = std::sqrt(gx * gx + gz * gz);
          const f64 cosine = g > 0 ? std::fabs(tx * gx + tz * gz) / g : 1.0;
          // 0-15, 15-30, 30-45, 45-60 and 60-90 degrees from the fall line.
          const u32 bin = cosine > .9659   ? 0
                          : cosine > .866  ? 1
                          : cosine > .7071 ? 2
                          : cosine > .5    ? 3
                                           : 4;
          section.line_m += run;
          section.by_angle_m[bin] += run;
          if (lower < a.at.y - .5 * depth) {
            section.half_bank_m += run;
            section.half_bank_by_angle_m[bin] += run;
          }
          if (lower < a.at.y - depth) {
            section.no_bank_m += run;
            section.no_bank_by_angle_m[bin] += run;
            section.no_bank_fall += run * g;
          }
        }
      }
      // W91: the boulders the placement generator lays over this network; W97: every clast, the
      // steps' and cascades' boulders (zones 4 and 5) among them, merged one piece a 64 m tile.
      Vector<channels::PlacedPiece> pieces;
      channels::ClastLog laid;
      if (!channels::place_pieces(layer, network, ground_view, 64.0f, pieces, error, &laid))
        return fail(scene_path + ": placements " + std::to_string(k) + ": " + error);
      for (const channels::Clast& c : laid.clasts)
        boulders += c.zone >= 4 ? 1u : 0u;
      clasts += laid.clasts.size();
      clast_tiles += pieces.size();
      heads += network.heads.size();
      links += network.links.size();
      metres += network.length_m;
      area += network.area_m2();
    }
  }
  if (entries == 0) return fail(scene_path + " has no \"channels\" placement to run");
  // W91: what the water will draw, measured against the ground (channels.md, "Against the proof of
  // concept").
  // `--measure` (W91): the water another file holds (its `water_channels`, as a scene or this
  // command's output holds them) is measured in place of what this run wrote, so a shipped scene's
  // water is measured by the same rule over the same ground.
  JsonValue measured = all;
  if (!measure_path.empty()) {
    std::string other;
    JsonValue other_json;
    if (io::read_file(measure_path, other) != io::Status::Ok || !parse_json(other, other_json).ok)
      return fail("cannot read " + measure_path);
    const JsonValue* list = other_json.find("water_channels");
    if (list == nullptr || !list->is_array()) return fail(measure_path + " has no water_channels");
    measured = *list;
  }
  // The region's border (two lattice spacings) counted apart: its links run along the outlet.
  const channels::WaterStats stats =
      channels::measure_water(measured, ground_view, &shores, region, border);
  JsonValue out = JsonValue::object();
  const u32 written = all.as_array().size();
  const u32 cut_count = cuts.as_array().size();
  out["water_channels"] = std::move(all);
  // The terrain's cuts, for the `tiles` block (`terrain.tiles.edits`, W98).
  if (cut) out["edits"] = std::move(cuts);
  // Each entry's plan, for its `params.plan`: what the generator reads on the cut ground.
  JsonValue plan_keys = JsonValue::array();
  for (const JsonValue& p : plans.as_array())
    plan_keys.push_back(*p.find("plan"));
  out["plans"] = std::move(plans);
  // The beds' outlines, for the terrain's `masks.regions` (W93): only where a layer names its
  // bed's material, so an output without one is the text it was.
  const u32 region_count = regions.as_array().size();
  if (region_count > 0) out["mask_regions"] = std::move(regions);
  if (io::write_file_atomic(out_path, write_json(out)) != io::Status::Ok)
    return fail("cannot write " + out_path);
  JsonValue line = JsonValue::object();
  line["command"] = JsonValue("channels");
  line["scene"] = JsonValue(scene_path);
  line["out"] = JsonValue(out_path);
  line["entries"] = JsonValue(entries);
  line["heads"] = JsonValue(heads);
  line["links"] = JsonValue(links);
  line["channel_m"] = JsonValue(std::round(metres));
  line["region_km2"] = JsonValue(std::round(area / 1e3) / 1e3);
  line["km_per_km2"] = JsonValue(area > 0 ? std::round(metres / area * 1e3 * 100) / 100 : 0.0);
  line["water_channels"] = JsonValue(written);
  line["cuts"] = JsonValue(cut_count);
  // W93: the plans' keys, how many of them this run made (the rest were read from the cache), and
  // the seconds the plans took, made or read.
  line["plans"] = std::move(plan_keys);
  line["planned"] = JsonValue(planned);
  line["mask_regions"] = JsonValue(region_count);
  line["plan_s"] = JsonValue(std::round(plan_seconds * 1000) / 1000);
  // W90: what the water bodies and the junctions did, and the line's points by reach.
  line["bodies"] = JsonValue(static_cast<u32>(bodies.size()));
  line["mouths"] = JsonValue(shore_counts.mouths);
  line["dropped"] = JsonValue(shore_counts.dropped);
  line["outlets"] = JsonValue(shore_counts.outlets);
  line["confluences"] = JsonValue(shore_counts.confluences);
  line["points_under_water"] = JsonValue(shore_counts.points_removed);
  JsonValue by_reach = JsonValue::object();
  const char* names[7] = {"dune_ripple", "pool_riffle", "plane_bed", "step_pool",
                          "cascade",     "bedrock",     "colluvial"};
  for (u32 r = 0; r < 7; ++r)
    by_reach[names[r]] = JsonValue(reaches[r]);
  line["reach_points"] = std::move(by_reach);
  // W91: the falls the water draws (stretches at 45 degrees or steeper, and their runs), by reach;
  // the steps laid, by reach; the boulders; the ribbon edges standing over the ground beside them.
  JsonValue fall_by_reach = JsonValue::object(), stretch_by_reach = JsonValue::object(),
            step_by_reach = JsonValue::object();
  for (u32 r = 0; r < 7; ++r) {
    fall_by_reach[names[r]] = JsonValue(stats.fall_stretches_by_reach[r]);
    stretch_by_reach[names[r]] = JsonValue(stats.stretches_by_reach[r]);
    step_by_reach[names[r]] = JsonValue(steps[r]);
  }
  line["water_stretches"] = JsonValue(stats.stretches);
  line["stretches_by_reach"] = std::move(stretch_by_reach);
  line["fall_stretches"] = JsonValue(stats.fall_stretches);
  line["fall_stretches_by_reach"] = std::move(fall_by_reach);
  line["falls"] = JsonValue(stats.falls);
  line["highest_fall_m"] = JsonValue(std::round(stats.highest_fall_m * 1000) / 1000);
  line["steps_by_reach"] = std::move(step_by_reach);
  line["boulders"] = JsonValue(boulders);
  line["clasts"] = JsonValue(clasts);
  line["clast_tiles"] = JsonValue(clast_tiles);
  line["cascade_clusters"] = JsonValue(clusters);
  line["knickpoints"] = JsonValue(knickpoints);
  line["lip_convexity_mean"] = JsonValue(
      knickpoints > 0 ? std::round(lip_convexity / static_cast<f64>(knickpoints) * 1000) / 1000
                      : 0.0);
  line["steep_left"] = JsonValue(steep_left);
  line["trough_pulled_points"] = JsonValue(pulled);
  // W93: pools, the flood bed, and the line against its ground's fall (metres of line, by the
  // line's angle to the fall line: 0-15, 15-30, 30-45, 45-60, 60-90 degrees).
  {
    const auto metres_of = [](const f64* bins) {
      JsonValue list = JsonValue::array();
      for (u32 b = 0; b < 5; ++b)
        list.push_back(JsonValue(std::round(bins[b])));
      return list;
    };
    line["pools"] = JsonValue(section.pools);
    line["bank_moved_points"] = JsonValue(bank_moved);
    line["flood_bed_m"] = JsonValue(std::round(section.flood_m));
    line["flood_bed_m2"] = JsonValue(std::round(section.flood_m2));
    line["line_measured_m"] = JsonValue(std::round(section.line_m));
    line["line_by_fall_angle_m"] = metres_of(section.by_angle_m);
    line["half_bank_m"] = JsonValue(std::round(section.half_bank_m));
    line["half_bank_by_fall_angle_m"] = metres_of(section.half_bank_by_angle_m);
    line["no_bank_m"] = JsonValue(std::round(section.no_bank_m));
    line["no_bank_by_fall_angle_m"] = metres_of(section.no_bank_by_angle_m);
    line["no_bank_mean_slope"] = JsonValue(
        section.no_bank_m > 0 ? std::round(section.no_bank_fall / section.no_bank_m * 1000) / 1000
                              : 0.0);
  }
  line["ribbon_edges"] = JsonValue(stats.edges);
  line["edges_above_005"] = JsonValue(stats.edges_above_005);
  line["edges_above_03"] = JsonValue(stats.edges_above_03);
  line["edge_max_m"] = JsonValue(std::round(stats.edge_max_m * 1000) / 1000);
  JsonValue where = JsonValue::array();
  where.push_back(JsonValue(std::round(stats.edge_max_at.x * 1000) / 1000));
  where.push_back(JsonValue(std::round(stats.edge_max_at.z * 1000) / 1000));
  line["edge_max_at"] = std::move(where);
  line["edge_max_block"] = JsonValue(stats.edge_max_block);
  line["border_edges_above_005"] = JsonValue(stats.border_edges_above_005);
  line["border_edge_max_m"] = JsonValue(std::round(stats.border_edge_max_m * 1000) / 1000);
  JsonValue border_blocks = JsonValue::array();
  for (const std::string& b : stats.border_blocks)
    border_blocks.push_back(JsonValue(b));
  line["border_blocks"] = std::move(border_blocks);
  line["depths_not_positive"] = JsonValue(stats.depths_not_positive);
  if (stats.depths_not_positive > 0) line["depth_block"] = JsonValue(stats.depth_block);
  // W91 continuation: the ribbons' ends a body's water covers, trimmed, named by the first body
  // (in the scene's order) that covers each point.
  JsonValue covered = JsonValue::object();
  covered["points"] = JsonValue(trim.points);
  covered["ribbons"] = JsonValue(trim.blocks);
  covered["dropped"] = JsonValue(trim.dropped);
  covered["mouth_fans"] = JsonValue(trim.mouth_fans);
  JsonValue by_body = JsonValue::object();
  if (!trim.at.empty()) {
    Vector<u64> counts(bodies.size(), 0);
    Vector<scene_gen::Exclusion> each(bodies.size());
    Vector<u8> built(bodies.size(), 0);
    for (u32 b = 0; b < bodies.size(); ++b)
      built[b] = each[b].build({}, {&bodies[b], 1}, {}, nullptr) ? 1 : 0;
    u64 unnamed = 0;
    for (const WorldPos& at : trim.at) {
      const f64 height = at.y + 1e-3;
      scene_gen::Ground flat;
      flat.context = &height;
      flat.surface_fn = [](const void* c, i64, i64) noexcept {
        return *static_cast<const f64*>(c);
      };
      bool named = false;
      for (u32 b = 0; b < bodies.size() && !named; ++b) {
        if (built[b] == 0) continue;
        if (each[b].excluded_mm(flat, scene_gen::nearest_mm(at.x), scene_gen::nearest_mm(at.z),
                                scene_gen::k_exclude_water)) {
          ++counts[b];
          named = true;
        }
      }
      unnamed += named ? 0 : 1;
    }
    for (u32 b = 0; b < bodies.size(); ++b)
      if (counts[b] > 0) by_body[bodies[b].name] = JsonValue(counts[b]);
    if (unnamed > 0) by_body["(none)"] = JsonValue(unnamed);
  }
  covered["by_body"] = std::move(by_body);
  line["covered_trimmed"] = std::move(covered);
  JsonWriteOptions compact;
  compact.pretty = false;
  std::printf("%s\n", write_json(line, compact).c_str());
  return 0;
#endif
}

}  // namespace engine::content

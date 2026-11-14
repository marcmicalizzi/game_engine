// `engine-content flow solve` (docs/subsystems/channels.md, "The cook"; roadmap W120): a scene's
// streams solved in their own beds.
//
//   flow solve <scene.json> <out.json> [--ddc <dir>] [--entry <i>] [--lattice <m>]
//              [--resistance jarrett|manning] [--manning-n <n>] [--reach <m>] [--jobs <n>]
//              [--scene-out <scene.json>]
//
// Opens **the composed ground the scene draws** (`renderer::SceneGround`: the terrain with the
// scene's edits and every edit its entries plan, the channels' cuts among them) and solves on it:
//
//   * every link of every `channels` placement (or the one at `--entry`) that carries water — the
//     discharge down the network from the entry's `flow` (the runoff over each point's catchment
//     and the point sources it names), a link wet from where that is a sheet's — downstream links
//     first, so a tributary meets the level its main stem was solved to;
//   * every `water_channels` block of the scene that is not one of those links' (an author's
//     line: its `points` and a `discharge_m3s` of one entry or one a point), by the same solver.
//
// Writes `{"water_channels": [...], "dry": [...]}` — the blocks for the scene's `water_channels`,
// and every link that carries no water with its reason — and prints one JSON line: links wet and
// dry, stations and stations a kilometre, falls, jumps, stations over a bank (the count, the
// worst, where), rises of the level by kind, and the cook's seconds a kilometre. `--scene-out`
// also writes the scene with its `water_channels` replaced.
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_CHANNELS && ENGINE_CONTENT_TILESET
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/channels/channels.h>
#include <domain/channels/discharge.h>
#include <domain/channels/profile.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/water_channel.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/ground_sample.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <memory>
#include <schemas/scene.h>
#include <string>
#include <string_view>
#endif

namespace engine::content {

#if ENGINE_CONTENT_CHANNELS && ENGINE_CONTENT_TILESET
namespace {

const char* k_solve_usage =
    "usage: engine-content flow solve <scene.json> <out.json> [--ddc <dir>] [--entry <i>]\n"
    "                                  [--lattice <m>] [--resistance jarrett|manning]\n"
    "                                  [--manning-n <n>] [--reach <m>] [--jobs <n>]\n"
    "                                  [--scene-out <scene.json>]\n"
    "  solves the scene's streams on the ground it draws: every wet link of its channels\n"
    "  placements and every water_channels block an author wrote (points and a discharge)\n";

int solve_fail(const std::string& why) {
  std::fprintf(stderr, "flow solve: %s\n", why.c_str());
  return 1;
}

JsonValue place(const WorldPos& at) {
  JsonValue out = JsonValue::array();
  out.push_back(JsonValue(at.x));
  out.push_back(JsonValue(at.y));
  out.push_back(JsonValue(at.z));
  return out;
}

// The sums of every line's stats, the worst kept with where.
struct Totals {
  u32 lines = 0;
  u64 nodes = 0, stations = 0;
  f64 metres = 0;
  u32 crests = 0, falls = 0, jumps = 0, drowned = 0, hung = 0;
  u32 over_bank = 0, over_bank_lines = 0;
  f64 over_bank_worst = 0;
  WorldPos over_bank_at{};
  std::string over_bank_line;
  u32 rises_jump = 0, rises_shooting = 0, rises_over_bank = 0, rises_other = 0;
  f64 rise_jump_worst = 0, rise_shooting_worst = 0, rise_other_worst = 0;
  WorldPos rise_shooting_at{}, rise_other_at{};
  u32 head_rises = 0;
  f64 head_rise_worst = 0;
  f64 highest_fall = 0, froude_max = 0;
  void add(const std::string& name, const channels::Profile& p) {
    const channels::ProfileStats& s = p.stats;
    ++lines;
    nodes += s.nodes;
    stations += s.stations;
    metres += s.length_m;
    crests += s.crests;
    falls += s.falls;
    jumps += s.jumps;
    drowned += s.drowned_feet;
    hung += s.tail_hung ? 1u : 0u;
    over_bank += s.over_bank;
    over_bank_lines += s.over_bank > 0 ? 1u : 0u;
    if (s.over_bank_worst_m > over_bank_worst) {
      over_bank_worst = s.over_bank_worst_m;
      over_bank_at = s.over_bank_at;
      over_bank_line = name;
    }
    rises_jump += s.rises_jump;
    rises_shooting += s.rises_shooting;
    rises_over_bank += s.rises_over_bank;
    rises_other += s.rises_other;
    rise_jump_worst = std::max(rise_jump_worst, s.rise_jump_worst_m);
    if (s.rise_shooting_worst_m > rise_shooting_worst) {
      rise_shooting_worst = s.rise_shooting_worst_m;
      rise_shooting_at = s.rise_shooting_at;
    }
    if (s.rise_other_worst_m > rise_other_worst) {
      rise_other_worst = s.rise_other_worst_m;
      rise_other_at = s.rise_other_at;
    }
    head_rises += s.head_rises;
    head_rise_worst = std::max(head_rise_worst, s.head_rise_worst_m);
    highest_fall = std::max(highest_fall, s.highest_fall_m);
    froude_max = std::max(froude_max, s.froude_max);
  }
};

}  // namespace
#endif

int flow_solve_command(int argc, char** argv) {
#if !ENGINE_CONTENT_CHANNELS || !ENGINE_CONTENT_TILESET
  (void)argc;
  (void)argv;
  std::fprintf(stderr,
               "flow requires the channels capability and the tiles ground provider "
               "(ENGINE_WITH_CHANNELS, ENGINE_WITH_TILESET)\n");
  return 1;
#else
  // argv[1] "flow", argv[2] "solve", argv[3] the scene, argv[4] the output.
  if (argc < 5) {
    std::fputs(k_solve_usage, stderr);
    return 2;
  }
  const std::string scene_path = argv[3];
  const std::string out_path = argv[4];
  std::string ddc, scene_out;
  i64 only = -1;
  u32 job_count = 1;
  f64 lattice = 0.0;
  channels::ProfileOptions options;
  for (int i = 5; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (i + 1 >= argc) {
      std::fputs(k_solve_usage, stderr);
      return 2;
    }
    const std::string_view value = argv[++i];
    f64 v = 0;
    const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
    const bool number =
        r.ec == std::errc{} && r.ptr == value.data() + value.size() && std::isfinite(v);
    bool ok = true;
    if (flag == "--ddc") {
      ddc = std::string(value);
    } else if (flag == "--scene-out") {
      scene_out = std::string(value);
    } else if (flag == "--entry") {
      ok = number && v >= 0 && v == std::floor(v);
      only = static_cast<i64>(v);
    } else if (flag == "--lattice") {
      ok = number && v >= 0.01 && v <= 100;
      lattice = v;
    } else if (flag == "--resistance") {
      ok = value == "jarrett" || value == "manning";
      options.resistance =
          value == "manning" ? channels::Resistance::Manning : channels::Resistance::Jarrett;
    } else if (flag == "--manning-n") {
      ok = number && v >= 0.005 && v <= 1;
      options.manning_n = v;
    } else if (flag == "--reach") {
      ok = number && v > 0 && v <= 1000;
      options.reach_m = v;
    } else if (flag == "--jobs") {
      ok = number && v >= 1 && v <= 256 && v == std::floor(v);
      job_count = static_cast<u32>(v);
    } else {
      ok = false;
    }
    if (!ok) {
      std::fprintf(stderr, "flow solve: invalid %.*s\n%s", static_cast<int>(flag.size()),
                   flag.data(), k_solve_usage);
      return 2;
    }
  }
  std::string text;
  if (io::read_file(scene_path, text) != io::Status::Ok)
    return solve_fail("cannot read " + scene_path);
  JsonValue json;
  const auto parsed = parse_json(text, json);
  if (!parsed.ok)
    return solve_fail(scene_path + ":" + std::to_string(parsed.line) + ": " + parsed.message);
  const JsonValue* terrain_json = json.find("terrain");
  if (terrain_json == nullptr) return solve_fail(scene_path + " has no terrain");
  scene::Terrain terrain;
  {
    schema::ReadContext reader;
    if (!schema::from_json(terrain, *terrain_json, reader) || !reader.ok()) {
      std::string message = scene_path + ": terrain";
      for (const auto& d : reader.diagnostics)
        message += "; " + d.path + ": " + d.message;
      return solve_fail(message);
    }
  }
  const std::string dir(io::parent_path(scene_path));
  std::string error;

  // **The lattice**: the ground's own spacing — a tile set's heightfield's, else the scene grid's.
  if (!(lattice > 0.0)) {
    if (terrain.tiles.has_value() && !terrain.tiles->heightfield.empty()) {
      const std::string header_path = io::is_absolute_path(terrain.tiles->heightfield)
                                          ? terrain.tiles->heightfield
                                          : io::join_path(dir, terrain.tiles->heightfield);
      std::string header_text;
      JsonValue header;
      f64 spacing_mm = 0;
      if (io::read_file(header_path, header_text) == io::Status::Ok &&
          parse_json(header_text, header).ok && header.find("spacing_mm") != nullptr &&
          header.find("spacing_mm")->get_f64(spacing_mm) && spacing_mm > 0)
        lattice = spacing_mm / 1000.0;
    }
    if (!(lattice > 0.0) && terrain.size > 1)
      lattice = 2.0 * static_cast<f64>(terrain.extent) / static_cast<f64>(terrain.size - 1);
    if (!(lattice > 0.0))
      return solve_fail("the ground's spacing is not known from the scene: name it with --lattice");
    lattice = std::clamp(lattice, 0.01, 100.0);
  }
  options.lattice_m = lattice;

  // The ground the scene draws.
  renderer::SceneGround composed;
  if (!composed.open(scene_path, ddc, false, false, error)) return solve_fail(error);
  const scene_gen::Ground ground = composed.ground();
  if (!ground.present()) return solve_fail(scene_path + ": the scene's ground gives no heights");
  for (const std::string& name : composed.not_carried())
    std::fprintf(stderr,
                 "flow solve: this build does not carry the placement generator \"%s\"; whatever "
                 "it plans for the ground is not in the ground solved on\n",
                 name.c_str());

  std::unique_ptr<jobs::JobSystem> job_system;
  if (job_count > 1) {
    jobs::JobSystemConfig config;
    config.performance_workers = job_count;
    config.efficiency_workers = 0;
    config.pin_threads = false;
    job_system = std::make_unique<jobs::JobSystem>(config);
  }

  JsonValue blocks = JsonValue::array();
  JsonValue dry = JsonValue::array();
  Totals totals;
  u32 entries = 0, links = 0, wet = 0, dry_sheet = 0, dry_short = 0, sources = 0, authored = 0;
  f64 wet_m = 0, dry_m = 0, largest = 0, smallest_wet = 0;
  f64 flow_seconds = 0, solve_seconds = 0;
  Vector<std::string> prefixes;  // "<layer> link ": the planner's blocks' names
  const auto seconds_since = [](std::chrono::steady_clock::time_point from) {
    return std::chrono::duration<f64>(std::chrono::steady_clock::now() - from).count();
  };

  // ---- The planner's links --------------------------------------------------------------------
  const JsonValue* placements = json.find("placements");
  if (placements != nullptr && placements->is_array()) {
    // What a plan that is not named is keyed from: the ground before every edit, and the bodies.
    std::unique_ptr<renderer::SceneGround> uncut;
    Vector<scene::WaterBody> bodies;
    if (const JsonValue* water = json.find("water"); water != nullptr && water->is_array()) {
      for (const JsonValue& b : water->as_array()) {
        scene::WaterBody body;
        schema::ReadContext body_reader;
        if (schema::from_json(body, b, body_reader) && body_reader.ok())
          bodies.push_back(std::move(body));
      }
    }
    scene_gen::Exclusion shores;
    if (!shores.build({}, {bodies.data(), bodies.size()}, {}, &error)) return solve_fail(error);
    std::string plan_dir;
    if (!channels::derived_dir(ddc, dir, plan_dir, error)) return solve_fail(error);
    for (u32 k = 0; k < placements->as_array().size(); ++k) {
      const JsonValue& entry = (*placements)[k];
      const JsonValue* generator = entry.find("generator");
      if (generator == nullptr || !generator->is_string() || generator->as_string() != "channels")
        continue;
      const JsonValue* params = entry.find("params");
      channels::ChannelLayer layer;
      schema::ReadContext layer_reader;
      if (params == nullptr || !schema::from_json(layer, *params, layer_reader) ||
          !layer_reader.ok()) {
        std::string message =
            scene_path + ": placements " + std::to_string(k) + ": engine.channels.ChannelLayer";
        for (const auto& d : layer_reader.diagnostics)
          message += "; " + d.path + ": " + d.message;
        return solve_fail(message);
      }
      prefixes.push_back(layer.name + " link ");
      if (only >= 0 && static_cast<i64>(k) != only) continue;
      const std::string where = scene_path + ": placements " + std::to_string(k);
      scene_gen::Context context;
      context.world_seed = terrain.seed;
      context.dir = dir;
      context.ddc = ddc;
      context.where = where;
      context.exclusion = &shores;
      if (layer.plan.empty()) {
        // Planned as `engine-content channels` plans it: over the ground before its cuts.
        if (uncut == nullptr) {
          uncut = std::make_unique<renderer::SceneGround>();
          if (!uncut->open(scene_path, ddc, true, false, error)) return solve_fail(error);
        }
        context.ground = uncut->ground();
      } else {
        context.ground = ground;
      }
      channels::ChannelPlan plan;
      if (!channels::ensure_plan(layer, context, plan_dir, plan, error))
        return solve_fail(where + ": " + error);
      ++entries;
      links += plan.network.links.size();
      auto began = std::chrono::steady_clock::now();
      channels::NetworkFlow flow;
      if (!channels::network_flow(layer, plan.network, flow, error))
        return solve_fail(where + ": " + error);
      flow_seconds += seconds_since(began);
      began = std::chrono::steady_clock::now();
      Vector<channels::SolvedLink> solved;
      if (!channels::solve_network(layer, plan.network, flow, ground, options, job_system.get(),
                                   solved, error))
        return solve_fail(where + ": " + error);
      solve_seconds += seconds_since(began);
      wet += flow.wet;
      dry_sheet += flow.dry_under_sheet;
      dry_short += flow.dry_too_short;
      sources += flow.sources;
      wet_m += flow.wet_m;
      dry_m += flow.dry_m;
      largest = std::max(largest, flow.largest_m3s);
      if (flow.wet > 0)
        smallest_wet = smallest_wet > 0 ? std::min(smallest_wet, flow.smallest_wet_m3s)
                                        : flow.smallest_wet_m3s;
      for (u32 l = 0; l < solved.size(); ++l) {
        const channels::LinkFlow& lf = flow.links[l];
        const std::string name = layer.name + " link " + std::to_string(l);
        if (solved[l].wet) {
          totals.add(name, solved[l].profile);
          blocks.push_back(schema::to_json(solved[l].block));
          continue;
        }
        JsonValue line = JsonValue::object();
        line["name"] = JsonValue(name);
        line["reason"] = JsonValue(lf.reason == channels::DryReason::UnderSheet
                                       ? "under a sheet: an ephemeral gully"
                                       : "too short: fewer than two points kept and wet");
        line["tail_l_s"] = JsonValue(std::round(lf.tail_m3s * 1e5) / 100.0);
        line["sheet_mm"] = JsonValue(std::round(lf.tail_sheet_m * 1e4) / 10.0);
        const channels::ChannelLink& link = plan.network.links[l];
        if (!link.line.empty()) {
          line["at"] = place(link.line[0].at);
          line["area_m2"] = JsonValue(std::round(static_cast<f64>(link.line.back().area_m2)));
        }
        dry.push_back(std::move(line));
      }
    }
  }

  // ---- An author's lines ----------------------------------------------------------------------
  if (const JsonValue* list = json.find("water_channels"); list != nullptr && list->is_array()) {
    for (u32 k = 0; k < list->as_array().size(); ++k) {
      scene::WaterChannel block;
      schema::ReadContext reader;
      if (!schema::from_json(block, (*list)[k], reader) || !reader.ok()) {
        std::string message = scene_path + ": water_channels " + std::to_string(k);
        for (const auto& d : reader.diagnostics)
          message += "; " + d.path + ": " + d.message;
        return solve_fail(message);
      }
      bool planners = false;
      for (const std::string& prefix : prefixes)
        planners = planners || block.name.compare(0, prefix.size(), prefix) == 0;
      if (planners) continue;  // solved above, from its plan
      const std::string where =
          scene_path + ": water_channels " + std::to_string(k) + " \"" + block.name + "\"";
      if (block.discharge_m3s.empty())
        return solve_fail(where +
                          " names no discharge_m3s: a line is solved from its points and "
                          "a discharge (one entry for the line, or one a point)");
      channels::FlowLine line;
      line.name = block.name;
      for (const WorldPos& p : block.points)
        line.points.push_back(p);
      for (const f32 q : block.discharge_m3s)
        line.discharge.push_back(static_cast<f64>(q));
      channels::ProfileOptions own = options;
      if (own.resistance == channels::Resistance::Manning)
        own.manning_n = static_cast<f64>(block.manning_n);
      const auto began = std::chrono::steady_clock::now();
      channels::Profile profile;
      if (!channels::solve_profile(line, ground, own, profile, error))
        return solve_fail(where + ": " + error);
      solve_seconds += seconds_since(began);
      // The block as its author wrote it — its optics, its seed, its banks — with the solve's
      // stations in place of its points.
      scene::WaterChannel solved = channels::describe_profile(block.name, profile, block.seed);
      block.points = solved.points;
      block.width_m = solved.width_m;
      block.depth_m = solved.depth_m;
      block.slope = solved.slope;
      block.discharge_m3s = solved.discharge_m3s;
      block.speed_mps = solved.speed_mps;
      block.froude = solved.froude;
      block.wet_left_m = solved.wet_left_m;
      block.wet_right_m = solved.wet_right_m;
      block.flags = solved.flags;
      block.manning_n = solved.manning_n;
      block.reach.clear();
      block.flood_left_m.clear();
      block.flood_right_m.clear();
      totals.add(block.name, profile);
      blocks.push_back(schema::to_json(block));
      ++authored;
    }
  }

  JsonValue out = JsonValue::object();
  out["dry"] = std::move(dry);
  JsonWriteOptions pretty;
  pretty.pretty = true;
  if (!scene_out.empty()) {
    JsonValue scene = json;
    scene.set("water_channels", blocks);
    if (io::write_file_atomic(scene_out, write_json(scene, pretty) + "\n") != io::Status::Ok)
      return solve_fail("cannot write " + scene_out);
  }
  const u32 written = static_cast<u32>(blocks.as_array().size());
  out["water_channels"] = std::move(blocks);
  if (io::write_file_atomic(out_path, write_json(out, pretty) + "\n") != io::Status::Ok)
    return solve_fail("cannot write " + out_path);

  const f64 km = totals.metres / 1000.0;
  const auto round3 = [](f64 v) { return std::round(v * 1e3) / 1e3; };
  JsonValue line = JsonValue::object();
  line["scene"] = JsonValue(scene_path);
  line["out"] = JsonValue(out_path);
  line["lattice_m"] = JsonValue(lattice);
  line["resistance"] =
      JsonValue(options.resistance == channels::Resistance::Manning ? "manning" : "jarrett");
  line["entries"] = JsonValue(entries);
  line["links"] = JsonValue(links);
  line["links_wet"] = JsonValue(wet);
  line["links_dry_under_sheet"] = JsonValue(dry_sheet);
  line["links_dry_too_short"] = JsonValue(dry_short);
  line["wet_km"] = JsonValue(round3(wet_m / 1000.0));
  line["dry_km"] = JsonValue(round3(dry_m / 1000.0));
  line["sources"] = JsonValue(sources);
  line["discharge_largest_m3s"] = JsonValue(round3(largest));
  line["discharge_smallest_wet_l_s"] = JsonValue(round3(smallest_wet * 1e3));
  line["authored_lines"] = JsonValue(authored);
  line["water_channels"] = JsonValue(written);
  line["solved_km"] = JsonValue(round3(km));
  line["nodes"] = JsonValue(totals.nodes);
  line["stations"] = JsonValue(totals.stations);
  line["stations_per_km"] =
      JsonValue(km > 0 ? round3(static_cast<f64>(totals.stations) / km) : 0.0);
  line["crests"] = JsonValue(totals.crests);
  line["falls"] = JsonValue(totals.falls);
  line["highest_fall_m"] = JsonValue(round3(totals.highest_fall));
  line["jumps"] = JsonValue(totals.jumps);
  line["drowned_feet"] = JsonValue(totals.drowned);
  line["tails_hung"] = JsonValue(totals.hung);
  line["froude_max"] = JsonValue(round3(totals.froude_max));
  JsonValue over = JsonValue::object();
  over["stations"] = JsonValue(totals.over_bank);
  over["lines"] = JsonValue(totals.over_bank_lines);
  over["worst_m"] = JsonValue(round3(totals.over_bank_worst));
  if (totals.over_bank > 0) {
    over["at"] = place(totals.over_bank_at);
    over["line"] = JsonValue(totals.over_bank_line);
  }
  line["over_bank"] = std::move(over);
  JsonValue rises = JsonValue::object();
  rises["jump"] = JsonValue(totals.rises_jump);
  rises["jump_worst_m"] = JsonValue(round3(totals.rise_jump_worst));
  rises["shooting"] = JsonValue(totals.rises_shooting);
  rises["shooting_worst_m"] = JsonValue(round3(totals.rise_shooting_worst));
  if (totals.rises_shooting > 0) rises["shooting_at"] = place(totals.rise_shooting_at);
  rises["beside_over_bank"] = JsonValue(totals.rises_over_bank);
  rises["other"] = JsonValue(totals.rises_other);
  rises["other_worst_m"] = JsonValue(round3(totals.rise_other_worst));
  if (totals.rises_other > 0) rises["other_at"] = place(totals.rise_other_at);
  rises["head"] = JsonValue(totals.head_rises);
  rises["head_worst_m"] = JsonValue(round3(totals.head_rise_worst));
  line["rises"] = std::move(rises);
  line["flow_seconds"] = JsonValue(round3(flow_seconds));
  line["solve_seconds"] = JsonValue(round3(solve_seconds));
  line["solve_seconds_per_km"] = JsonValue(km > 0 ? round3(solve_seconds / km) : 0.0);
  line["jobs"] = JsonValue(job_count);
  JsonWriteOptions flat;
  flat.pretty = false;
  std::fprintf(stdout, "%s\n", write_json(line, flat).c_str());
  return 0;
#endif
}

}  // namespace engine::content

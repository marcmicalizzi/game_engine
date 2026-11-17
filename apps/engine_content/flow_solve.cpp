// `engine-content flow solve` (docs/subsystems/channels.md, "The cook"; roadmap W120): a scene's
// streams solved in their own beds.
//
//   flow solve <scene.json> <out.json> [--ddc <dir>] [--entry <i>] [--lattice <m>]
//              [--resistance jarrett|manning] [--manning-n <n>] [--reach <m>] [--jobs <n>]
//              [--scene-out <scene.json>] [--under-water <list.json>]
//
// Opens **the composed ground the scene draws** (`renderer::SceneGround`: the terrain with the
// scene's edits and every edit its entries plan, the channels' cuts among them) and solves on it:
//
//   * every link of every `channels` placement (or the one at `--entry`) that carries water — the
//     discharge down the network from the entry's `flow` (the runoff over each point's catchment
//     and the point sources it names), a link wet from where that is a sheet's — downstream links
//     first, so a tributary meets the level its main stem was solved to;
//   * every `water_channels` block of the scene that declares no writer (`derived_from` empty:
//     an author's line, its `points` and a `discharge_m3s` of one entry or one a point), by the
//     same solver. A block that declares an entry solved here is replaced; one that declares
//     another writer is kept; one laid before W120 is dropped (channels.md, "The cook").
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
#include <domain/channels/runs.h>
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
    "                                  [--scene-out <scene.json>] [--under-water <list.json>]\n"
    "  solves the scene's streams on the ground it draws: every wet link of its channels\n"
    "  placements and every water_channels block an author wrote (points and a discharge);\n"
    "  --under-water lists the scene's instances that stand in a water body's water\n";

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

// The scene's water bodies that read (one that does not is the water capability's to refuse).
Vector<scene::WaterBody> read_bodies(const JsonValue& scene) {
  Vector<scene::WaterBody> bodies;
  if (const JsonValue* water = scene.find("water"); water != nullptr && water->is_array())
    for (const JsonValue& b : water->as_array()) {
      scene::WaterBody body;
      schema::ReadContext reader;
      if (schema::from_json(body, b, reader) && reader.ok()) bodies.push_back(std::move(body));
    }
  return bodies;
}

// **Does a body that fills hold its level on the ground the scene draws?** (W122; channels.md,
// "Hollows", "The level against the ground between the lattice's points"). The channels' plan
// stands a body a notch under a spill it found on its own lattice; the water capability floods
// the level over the drawn ground on a finer lattice of its own, four neighbours a node, and
// refuses a body whose flood reaches its extent, when the scene is loaded. This is that flood by
// the water's own rule (water.md, "A body that fills": a spacing of `shore_m / 3` held between
// 0.25 and 1 m, grown by a quarter while the lattice's side passes 4,097 nodes or the region's
// nodes and edge crossings pass 65,536; a node is wet where the ground stands under the level),
// run here, where the cuts are already in the ground, so that a cook names the body a load
// would refuse. It changes nothing in the scene. The rule is the water's and is mirrored, not
// called (this command does not link the capability): the two move together.
struct FillCheck {
  u64 nodes = 0;    // the nodes the last flood wetted
  f64 spacing = 0;  // that flood's lattice
  bool holds = false;
  bool seed_dry = false;  // the seed stands on ground at or over the level: nothing fills
  // The last flood's lattice, for whoever asks what stands in the water: `side` nodes a side
  // about the seed at node (`half`, `half`); a node is 0 not asked, 1 wet, 2 asked and dry.
  Vector<u8> state;
  u32 side = 0, half = 0;
  // Whether (x, z) stands in the flood: one of the four nodes about it is wet. (The ground at the
  // point itself against the level is the caller's to ask.)
  bool wets(const scene::WaterFill& f, f64 x, f64 z) const {
    if (!holds || side == 0) return false;
    const f64 fi = std::floor((x - f.seed.x) / spacing) + static_cast<f64>(half);
    const f64 fj = std::floor((z - f.seed.z) / spacing) + static_cast<f64>(half);
    if (!(fi >= 0.0) || !(fj >= 0.0) || fi + 1.0 >= static_cast<f64>(side) ||
        fj + 1.0 >= static_cast<f64>(side))
      return false;
    const u32 i = static_cast<u32>(fi), j = static_cast<u32>(fj);
    return state[j * side + i] == 1 || state[j * side + i + 1] == 1 ||
           state[(j + 1) * side + i] == 1 || state[(j + 1) * side + i + 1] == 1;
  }
};
FillCheck check_fill(const scene::WaterBody& body, const scene_gen::Ground& ground) {
  constexpr u32 k_max_side = 4097;
  constexpr u64 k_max_vertices = 65536;
  FillCheck out;
  const scene::WaterFill& f = *body.shape.fill;
  const f64 level = static_cast<f64>(body.level_m);
  const f64 extent = static_cast<f64>(f.max_extent_m);
  f32 spacing = std::clamp(body.shore_m / 3.0f, 0.25f, 1.0f);
  while (2.0 * std::ceil(extent / static_cast<f64>(spacing)) + 1.0 > static_cast<f64>(k_max_side))
    spacing *= 1.25f;
  Vector<u8>& state = out.state;
  Vector<u32> stack;
  for (;;) {
    const u32 half = static_cast<u32>(std::ceil(extent / static_cast<f64>(spacing)));
    const u32 side = 2 * half + 1;
    out.half = half;
    out.side = side;
    state.assign(side * side, u8{0});
    stack.clear();
    const auto wet = [&](u32 node) {
      const f64 x = static_cast<f64>(static_cast<i64>(node % side) - static_cast<i64>(half)) *
                    static_cast<f64>(spacing);
      const f64 z = static_cast<f64>(static_cast<i64>(node / side) - static_cast<i64>(half)) *
                    static_cast<f64>(spacing);
      return level - ground.surface(WorldPos{f.seed.x + x, level, f.seed.z + z}) > 0.0;
    };
    out.spacing = static_cast<f64>(spacing);
    out.nodes = 0;
    const u32 seed = half * side + half;
    if (!wet(seed)) {
      out.seed_dry = true;
      return out;
    }
    state[seed] = 1;
    stack.push_back(seed);
    u32 i0 = half, i1 = half, j0 = half, j1 = half;
    bool spilled = false;
    while (!stack.empty()) {
      const u32 node = stack.back();
      stack.pop_back();
      ++out.nodes;
      const u32 i = node % side, j = node / side;
      i0 = std::min(i0, i);
      i1 = std::max(i1, i);
      j0 = std::min(j0, j);
      j1 = std::max(j1, j);
      if (i == 0 || j == 0 || i + 1 == side || j + 1 == side) {
        spilled = true;
        break;
      }
      const u32 next[4] = {node - 1, node + 1, node - side, node + side};
      for (const u32 n : next) {
        if (state[n] != 0) continue;
        const bool under = wet(n);
        state[n] = under ? 1 : 2;
        if (under) stack.push_back(n);
      }
    }
    if (spilled) return out;
    // The lattice edges between a wet node and one that is not: the mesh's edge vertices.
    u64 crossings = 0;
    for (u32 j = j0 - 1; j <= j1 + 1; ++j) {
      for (u32 i = i0 - 1; i <= i1 + 1; ++i) {
        const bool in = state[j * side + i] == 1;
        if (i + 1 <= i1 + 1 && in != (state[j * side + i + 1] == 1)) ++crossings;
        if (j + 1 <= j1 + 1 && in != (state[(j + 1) * side + i] == 1)) ++crossings;
      }
    }
    if (out.nodes + crossings <= k_max_vertices) {
      out.holds = true;
      return out;
    }
    spacing *= 1.25f;
  }
}

// Whether (x, z) stands inside a body's circle or polygon (a flood's region is not known here).
bool inside_body(const scene::WaterBody& body, f64 x, f64 z) {
  if (body.shape.circle.has_value()) {
    const scene::WaterCircle& c = *body.shape.circle;
    const f64 dx = x - c.center.x, dz = z - c.center.z, r = static_cast<f64>(c.radius_m);
    return dx * dx + dz * dz <= r * r;
  }
  bool in = false;
  const u32 n = body.shape.polygon.size();
  for (u32 i = 0, j = n > 0 ? n - 1 : 0; i < n; j = i++) {
    const WorldPos& a = body.shape.polygon[i];
    const WorldPos& b = body.shape.polygon[j];
    if ((a.z > z) != (b.z > z) && x < (b.x - a.x) * (z - a.z) / (b.z - a.z) + a.x) in = !in;
  }
  return in;
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
  u32 pools = 0, pools_open = 0;  // W130.2: stations standing as a pool in a pit of their line
  u32 pools_spilled = 0, pools_reached = 0;  // W138
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
    pools += s.pools;
    pools_open += s.pools_open;
    pools_spilled += s.pools_spilled;
    pools_reached += s.pools_reached;
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
  std::string ddc, scene_out, under_path;
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
    } else if (flag == "--under-water") {
      under_path = std::string(value);
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
  {
    // A channel block of versions 3 to 5 reads through `WaterChannel`'s migrations, as it does
    // in every host: a scene cooked before W120 is cooked again by this command.
    schema::ReadContext migrated;
    if (!scene_gen::migrate_scene_water_channels(json, migrated) || !migrated.ok()) {
      std::string message = scene_path + ": water_channels";
      for (const auto& d : migrated.diagnostics)
        message += "; " + d.path + ": " + d.message;
      return solve_fail(message);
    }
  }
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
  if (!composed.open(scene_path, ddc, false, false, error, true)) return solve_fail(error);
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
  channels::RunsBySlope by_slope;  // W130: how whole each line is drawn, by its slope
  u32 entries = 0, links = 0, wet = 0, dry_sheet = 0, dry_short = 0, sources = 0, authored = 0;
  u32 replaced = 0, kept = 0, laid = 0;
  u32 unsolved = 0;  // W129: wet links whose profile was refused, left dry
  f64 wet_m = 0, dry_m = 0, largest = 0, smallest_wet = 0;
  u32 lake_outlets = 0;  // W122: outlets of the plan's lakes that lose water, and what they lose
  f64 lake_loss = 0;
  f64 flow_seconds = 0, solve_seconds = 0;
  Vector<std::string> writers;  // what the blocks this cook writes declare (`derived_from`)
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
      if (only >= 0 && static_cast<i64>(k) != only) continue;
      writers.push_back(channels::flow_writer(layer));
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
          if (!uncut->open(scene_path, ddc, true, false, error, true)) return solve_fail(error);
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
      lake_outlets += flow.lake_outlets;
      lake_loss += flow.lake_loss_m3s;
      if (flow.wet > 0)
        smallest_wet = smallest_wet > 0 ? std::min(smallest_wet, flow.smallest_wet_m3s)
                                        : flow.smallest_wet_m3s;
      for (u32 l = 0; l < solved.size(); ++l) {
        const channels::LinkFlow& lf = flow.links[l];
        const std::string name = layer.name + " link " + std::to_string(l);
        if (solved[l].wet) {
          totals.add(name, solved[l].profile);
          by_slope.add(channels::measure_runs(solved[l].block));
          blocks.push_back(schema::to_json(solved[l].block));
          continue;
        }
        JsonValue line = JsonValue::object();
        line["name"] = JsonValue(name);
        if (!solved[l].refused.empty()) {
          // W129: wet, but its profile could not be solved; every other line was.
          ++unsolved;
          std::fprintf(stderr, "flow solve: %s: left dry: %s\n", where.c_str(),
                       solved[l].refused.c_str());
          line["reason"] = JsonValue("unsolved: " + solved[l].refused);
        } else {
          line["reason"] = JsonValue(lf.reason == channels::DryReason::UnderSheet
                                         ? "under a sheet: an ephemeral gully"
                                         : "too short: fewer than two points kept and wet");
        }
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

  // ---- The scene's own blocks -----------------------------------------------------------------
  // **What a block declares and holds says what the cook does with it**, never what it is called:
  //   * `derived_from` names an entry this cook solved: the block is that entry's last cook, and
  //     the blocks above replace it;
  //   * `derived_from` names anything else: another writer's, kept as it is;
  //   * no `derived_from` and a discharge: an author's line, solved;
  //   * no `derived_from`, no discharge, and a depth and a slope a point: a description laid
  //     before W120, which nothing can be solved from — dropped and counted;
  //   * no `derived_from` and none of those: an author's line that names no discharge, refused.
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
      if (!block.derived_from.empty()) {
        bool mine = false;
        for (const std::string& writer : writers)
          mine = mine || writer == block.derived_from;
        if (mine) {
          ++replaced;
        } else {
          blocks.push_back((*list)[k]);
          ++kept;
        }
        continue;
      }
      const std::string where =
          scene_path + ": water_channels " + std::to_string(k) + " \"" + block.name + "\"";
      if (block.discharge_m3s.empty()) {
        if (!block.depth_m.empty() && !block.slope.empty()) {
          ++laid;
          continue;
        }
        return solve_fail(where +
                          " names no discharge_m3s: a line is solved from its points and "
                          "a discharge (one entry for the line, or one a point)");
      }
      channels::FlowLine line;
      line.name = block.name;
      for (const WorldPos& p : block.points)
        line.points.push_back(p);
      for (const f32 q : block.discharge_m3s)
        line.discharge.push_back(static_cast<f64>(q));
      // **A line that runs into a body ends at the body's level**: a lattice past its last
      // point, along its last stretch, stands inside a body's circle or polygon.
      if (line.points.size() >= 2) {
        const WorldPos& end = line.points[line.points.size() - 1];
        const WorldPos& before = line.points[line.points.size() - 2];
        const f64 dx = end.x - before.x, dz = end.z - before.z;
        const f64 length = std::sqrt(dx * dx + dz * dz);
        if (length > 0.0)
          for (const scene::WaterBody& body : read_bodies(json))
            if (inside_body(body, end.x + lattice * dx / length, end.z + lattice * dz / length)) {
              line.tail_level = true;
              line.level = std::round(body.level_m * 1e3) / 1e3;
              line.tail = scene_gen::ChannelControl::Mouth;
              break;
            }
      }
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
      by_slope.add(channels::measure_runs(block));
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

  // ---- The bodies that fill: does each hold its level on this ground? (`check_fill`) ----------
  JsonValue fills = JsonValue::object();
  const Vector<scene::WaterBody> filling_bodies = read_bodies(json);
  Vector<FillCheck> floods(filling_bodies.size());  // a body's flood, where it fills and holds
  {
    const auto fills_began = std::chrono::steady_clock::now();
    u32 filling = 0, spilling = 0, seed_dry = 0;
    JsonValue fill_list = JsonValue::array();
    for (u32 b = 0; b < filling_bodies.size(); ++b) {
      const scene::WaterBody& body = filling_bodies[b];
      if (!body.shape.fill.has_value() || body.shape.circle.has_value() ||
          !body.shape.polygon.empty())
        continue;
      const scene::WaterFill& shape = *body.shape.fill;
      if (!std::isfinite(static_cast<f64>(body.level_m)) || !std::isfinite(shape.seed.x) ||
          !std::isfinite(shape.seed.z) || !(shape.max_extent_m > 0.0f))
        continue;  // the water capability's to refuse, by its own sentence
      ++filling;
      floods[b] = check_fill(body, ground);
      const FillCheck& check = floods[b];
      JsonValue one = JsonValue::object();
      one["name"] = JsonValue(body.name);
      one["level_m"] = JsonValue(round3(static_cast<f64>(body.level_m)));
      one["spacing_m"] = JsonValue(round3(check.spacing));
      one["nodes"] = JsonValue(check.nodes);
      one["holds"] = JsonValue(check.holds);
      if (check.seed_dry) {
        ++seed_dry;
        one["seed_dry"] = JsonValue(true);
        std::fprintf(stderr,
                     "flow solve: water body \"%s\": its fill's seed stands on ground at or above "
                     "its level %.3f m on the ground the scene draws: a load will refuse it\n",
                     body.name.c_str(), static_cast<f64>(body.level_m));
      } else if (!check.holds) {
        ++spilling;
        std::fprintf(stderr,
                     "flow solve: water body \"%s\": at its level %.3f m its fill reaches its "
                     "max_extent_m (%g m) on the ground the scene draws (a lattice of %.3f m): the "
                     "level spills out of the hollow, and a load will refuse it\n",
                     body.name.c_str(), static_cast<f64>(body.level_m),
                     static_cast<f64>(shape.max_extent_m), check.spacing);
      }
      fill_list.push_back(std::move(one));
    }
    fills["bodies"] = JsonValue(filling);
    fills["spilling"] = JsonValue(spilling);
    fills["seed_dry"] = JsonValue(seed_dry);
    fills["seconds"] = JsonValue(round3(seconds_since(fills_began)));
    fills["list"] = std::move(fill_list);
  }

  // ---- The scene's own instances that stand in a body's water (W122; channels.md, "Hollows",
  // "What stands in the water") ------------------------------------------------------------------
  // A body the plan writes stands where the scene's author placed things before it existed, and
  // nothing removes an author's instance: the cook counts them, a body at a time, and with
  // `--under-water` lists each (its place in the scene's `instances`, its mesh, where it stands
  // and how deep the ground under it is), for whoever places them to read. An instance is under a
  // body where the ground at its own x and z stands under the body's level **in the body's
  // water**: a filling body's own flood (above), a circle's or a polygon's inside, an open body's
  // anywhere. It is counted under the first body of the scene's list that covers it. Authored
  // bodies are counted as the plan's are; a scene with no body or no instance pays the look.
  JsonValue under = JsonValue::object();
  {
    const auto under_began = std::chrono::steady_clock::now();
    const JsonValue* instances = json.find("instances");
    const u32 instance_count =
        instances != nullptr && instances->is_array() ? static_cast<u32>(instances->size()) : 0u;
    Vector<u64> counts(filling_bodies.size(), u64{0});
    Vector<f64> deepest(filling_bodies.size(), 0.0);
    Vector<JsonValue> lists(filling_bodies.size());
    const bool listing = !under_path.empty();
    for (JsonValue& l : lists)
      l = JsonValue::array();
    u64 total = 0;
    for (u32 k = 0; k < instance_count && !filling_bodies.empty(); ++k) {
      const JsonValue& instance = (*instances)[k];
      const JsonValue* t = instance.find("translation");
      f64 x = 0, y = 0, z = 0;
      if (t == nullptr || !t->is_array() || t->size() != 3 || !(*t)[0].get_f64(x) ||
          !(*t)[1].get_f64(y) || !(*t)[2].get_f64(z))
        continue;
      for (u32 b = 0; b < filling_bodies.size(); ++b) {
        const scene::WaterBody& body = filling_bodies[b];
        const bool in = body.shape.fill.has_value() ? floods[b].wets(*body.shape.fill, x, z)
                                                    : (body.open || inside_body(body, x, z));
        if (!in) continue;
        const f64 level = static_cast<f64>(body.level_m);
        const f64 depth = level - ground.surface(WorldPos{x, level, z});
        if (!(depth > 0.0)) continue;
        ++counts[b];
        ++total;
        deepest[b] = std::max(deepest[b], depth);
        if (listing) {
          JsonValue one = JsonValue::object();
          one["index"] = JsonValue(k);
          f64 mesh = 0;
          if (const JsonValue* m = instance.find("mesh"); m != nullptr && m->get_f64(mesh))
            one["mesh"] = JsonValue(static_cast<u32>(mesh));
          if (const JsonValue* name = instance.find("name");
              name != nullptr && name->is_string() && !name->as_string().empty())
            one["name"] = JsonValue(std::string(name->as_string()));
          JsonValue at = JsonValue::array();
          at.push_back(JsonValue(round3(x)));
          at.push_back(JsonValue(round3(y)));
          at.push_back(JsonValue(round3(z)));
          one["at"] = std::move(at);
          one["depth_m"] = JsonValue(round3(depth));
          lists[b].push_back(std::move(one));
        }
        break;
      }
    }
    JsonValue by_body = JsonValue::object();
    JsonValue file_bodies = JsonValue::array();
    u32 most = 0;
    for (u32 b = 0; b < filling_bodies.size(); ++b) {
      if (counts[b] == 0) continue;
      if (counts[b] > counts[most]) most = b;
      by_body[filling_bodies[b].name] = JsonValue(counts[b]);
      if (listing) {
        JsonValue one = JsonValue::object();
        one["name"] = JsonValue(filling_bodies[b].name);
        one["level_m"] = JsonValue(round3(static_cast<f64>(filling_bodies[b].level_m)));
        one["count"] = JsonValue(counts[b]);
        one["deepest_m"] = JsonValue(round3(deepest[b]));
        one["instances"] = std::move(lists[b]);
        file_bodies.push_back(std::move(one));
      }
    }
    under["instances"] = JsonValue(instance_count);
    under["under"] = JsonValue(total);
    under["by_body"] = std::move(by_body);
    if (listing) {
      JsonValue file = JsonValue::object();
      file["scene"] = JsonValue(scene_path);
      file["instances"] = JsonValue(instance_count);
      file["under"] = JsonValue(total);
      file["bodies"] = std::move(file_bodies);
      JsonWriteOptions compact;
      compact.pretty = false;
      if (io::write_file_atomic(under_path, write_json(file, compact) + "\n") != io::Status::Ok)
        return solve_fail("cannot write " + under_path);
      under["file"] = JsonValue(under_path);
    }
    under["seconds"] = JsonValue(round3(seconds_since(under_began)));
    if (total > 0)
      std::fprintf(stderr,
                   "flow solve: %llu of the scene's %u instances stand under a water body's level "
                   "(the most under \"%s\": %llu)%s\n",
                   static_cast<unsigned long long>(total), instance_count,
                   filling_bodies[most].name.c_str(), static_cast<unsigned long long>(counts[most]),
                   listing ? "" : "; --under-water <file> lists them");
  }

  JsonValue line = JsonValue::object();
  line["fills"] = std::move(fills);
  line["under_water"] = std::move(under);
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
  line["links_unsolved"] = JsonValue(unsolved);
  line["wet_km"] = JsonValue(round3(wet_m / 1000.0));
  line["dry_km"] = JsonValue(round3(dry_m / 1000.0));
  line["sources"] = JsonValue(sources);
  line["discharge_largest_m3s"] = JsonValue(round3(largest));
  line["discharge_smallest_wet_l_s"] = JsonValue(round3(smallest_wet * 1e3));
  line["lake_outlets_losing"] = JsonValue(lake_outlets);
  line["lake_loss_l_s"] = JsonValue(round3(lake_loss * 1e3));
  line["authored_lines"] = JsonValue(authored);
  line["blocks_replaced"] = JsonValue(replaced);
  line["blocks_kept"] = JsonValue(kept);
  line["blocks_laid_dropped"] = JsonValue(laid);
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
  JsonValue pools = JsonValue::object();
  pools["stations"] = JsonValue(totals.pools);
  pools["open"] = JsonValue(totals.pools_open);
  pools["spilled"] = JsonValue(totals.pools_spilled);
  pools["reached_back"] = JsonValue(totals.pools_reached);
  line["pools"] = std::move(pools);
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
  line["runs_by_slope"] = channels::runs_json(by_slope);
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

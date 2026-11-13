// `engine-content falls` (docs/subsystems/apps.md, "engine-content"; docs/subsystems/channels.md,
// "Where the water leaves its bed (S104)"): where the water of a scene's `channels` placements
// leaves its bed. For each entry it takes the entry's plan — the one it names (no ground is
// opened), else the plan keyed from the ground before its cuts, as `engine-content channels` makes
// it — tests every run of the bed's own steps on every link (whether or not the water's description
// draws the link), cooks the falls beside the plan (`channels::ensure_falls`) and prints the count
// as one JSON line. With `<out.json>` it also writes the falls as JSON (`--paths`: every column's
// path).
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_CHANNELS
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/channels/channels.h>
#include <domain/channels/falls.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <memory>
#include <schemas/scene.h>
#include <string>
#endif

namespace engine::content {

int falls_command(int argc, char** argv) {
#if !ENGINE_CONTENT_CHANNELS
  (void)argc;
  (void)argv;
  std::fprintf(stderr, "falls requires the channels capability (ENGINE_WITH_CHANNELS)\n");
  return 1;
#else
  const char* usage =
      "usage: engine-content falls <scene.json> [<out.json>] [--ddc <dir>] [--entry <i>]\n"
      "                            [--stage <fraction>] [--min-area-m2 <m2>] [--jobs <n>] "
      "[--paths]\n";
  if (argc < 3) {
    std::fputs(usage, stderr);
    return 2;
  }
  const std::string scene_path = argv[2];
  std::string out_path, ddc;
  i64 only = -1;
  u32 job_count = 0;
  bool paths = false;
  channels::FallOptions options;
  for (int i = 3; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag == "--paths") {
      paths = true;
      continue;
    }
    if (flag.substr(0, 2) != "--") {
      if (!out_path.empty()) {
        std::fputs(usage, stderr);
        return 2;
      }
      out_path = std::string(flag);
      continue;
    }
    if (i + 1 >= argc) {
      std::fputs(usage, stderr);
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
    } else if (flag == "--entry") {
      ok = number && v >= 0 && v == std::floor(v);
      only = static_cast<i64>(v);
    } else if (flag == "--stage") {
      ok = number && v > 0 && v <= 1;
      options.stage = static_cast<f32>(v);
    } else if (flag == "--min-area-m2") {
      ok = number && v >= 0;
      options.min_area_m2 = v;
    } else if (flag == "--jobs") {
      ok = number && v >= 0 && v <= 256 && v == std::floor(v);
      job_count = static_cast<u32>(v);
    } else {
      ok = false;
    }
    if (!ok) {
      std::fprintf(stderr, "falls: invalid %.*s\n%s", static_cast<int>(flag.size()), flag.data(),
                   usage);
      return 2;
    }
  }
  const auto fail = [](const std::string& e) {
    std::fprintf(stderr, "falls: %s\n", e.c_str());
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
  const std::string dir(io::parent_path(scene_path));
  std::string plan_dir, error;
  // One root, the host's (channels.md, "In a scene"): the plan and its falls under `--ddc`, else
  // the scene's own.
  if (!channels::derived_dir(ddc, dir, plan_dir, error)) return fail(error);
  const JsonValue* placements = json.find("placements");
  if (placements == nullptr || !placements->is_array())
    return fail(scene_path + " has no \"channels\" placement to run");
  // The entries, and whether any names no plan: only then is the ground before its cuts opened.
  struct Entry {
    u32 index = 0;
    channels::ChannelLayer layer;
  };
  Vector<Entry> entries;
  bool needs_ground = false;
  for (u32 k = 0; k < placements->as_array().size(); ++k) {
    const JsonValue& entry = (*placements)[k];
    const JsonValue* generator = entry.find("generator");
    if (generator == nullptr || !generator->is_string() || generator->as_string() != "channels")
      continue;
    if (only >= 0 && static_cast<i64>(k) != only) continue;
    const JsonValue* params = entry.find("params");
    Entry e;
    e.index = k;
    schema::ReadContext layer_reader;
    if (params == nullptr || !schema::from_json(e.layer, *params, layer_reader) ||
        !layer_reader.ok()) {
      std::string message =
          scene_path + ": placements " + std::to_string(k) + ": engine.channels.ChannelLayer";
      for (const auto& d : layer_reader.diagnostics)
        message += "; " + d.path + ": " + d.message;
      return fail(message);
    }
    needs_ground = needs_ground || e.layer.plan.empty();
    entries.push_back(std::move(e));
  }
  if (entries.empty()) return fail(scene_path + " has no \"channels\" placement to run");
  scene_gen::Context context;
  context.world_seed = terrain.seed;
  context.dir = dir;
  context.ddc = ddc;
  scene_gen::GroundProvider ground;
  Vector<scene::WaterBody> bodies;
  scene_gen::Exclusion shores;
  if (needs_ground) {
    // As `engine-content channels` does: the heights alone, before any cut, through the provider
    // the renderer would draw the terrain with, and the bodies that end a link at a shoreline.
    terrain.masks.reset();
    if (terrain.tiles.has_value()) terrain.tiles->edits.clear();
    const std::string provider =
        !terrain.provider.empty()
            ? terrain.provider
            : (terrain.generator == scene::TerrainGenerator::Dunes ? "dunes" : "waves");
    const scene_gen::GeneratorRegistry& registry = scene_gen::GeneratorRegistry::global();
    const scene_gen::GroundProviderDesc* found = registry.find_ground(provider);
    if (found == nullptr) return fail("the terrain " + registry.unknown_ground(provider));
    if (!found->make(terrain, context, ground, &error) || !ground.valid())
      return fail(error.empty() ? "the ground provider \"" + provider + "\" refused the terrain"
                                : error);
    if (const JsonValue* water = json.find("water"); water != nullptr && water->is_array()) {
      for (const JsonValue& b : water->as_array()) {
        scene::WaterBody body;
        schema::ReadContext body_reader;
        if (schema::from_json(body, b, body_reader) && body_reader.ok())
          bodies.push_back(std::move(body));
      }
    }
    if (!shores.build({}, {bodies.data(), bodies.size()}, {}, &error)) return fail(error);
    context.ground = ground.view();
    context.exclusion = &shores;
  }
  std::unique_ptr<jobs::JobSystem> pool;
  if (job_count > 0) {
    jobs::JobSystemConfig config;
    config.performance_workers = job_count;
    config.efficiency_workers = 0;
    config.pin_threads = false;
    pool = std::make_unique<jobs::JobSystem>(config);
  }
  channels::FallStats total;
  u32 cooked = 0, planned = 0;
  u64 bytes = 0, falls_written = 0;
  f64 plan_seconds = 0, cook_seconds = 0;
  JsonValue keys = JsonValue::array();
  JsonValue all = JsonValue::array();
  for (const Entry& e : entries) {
    channels::ChannelPlan plan;
    const auto began = std::chrono::steady_clock::now();
    if (!channels::ensure_plan(e.layer, context, plan_dir, plan, error))
      return fail(scene_path + ": placements " + std::to_string(e.index) + ": " + error);
    const auto planned_at = std::chrono::steady_clock::now();
    plan_seconds += std::chrono::duration<f64>(planned_at - began).count();
    planned += plan.planned ? 1u : 0u;
    channels::FallSet set;
    bool made = false;
    if (!channels::ensure_falls(e.layer, plan, options, pool.get(), plan_dir, set, made, error))
      return fail(scene_path + ": placements " + std::to_string(e.index) + ": " + error);
    cook_seconds +=
        std::chrono::duration<f64>(std::chrono::steady_clock::now() - planned_at).count();
    cooked += made ? 1u : 0u;
    bytes += channels::falls_bytes(set).size();
    falls_written += set.falls.size();
    {
      JsonValue named = JsonValue::object();
      named["entry"] = JsonValue(e.index);
      named["plan"] = JsonValue(channels::plan_key_text(plan.key));
      named["falls"] =
          JsonValue(channels::falls_path(plan_dir, plan.key, channels::fall_options_hash(options)));
      keys.push_back(std::move(named));
    }
    const channels::FallStats& s = set.stats;
    total.links += s.links;
    total.runs += s.runs;
    total.held += s.held;
    total.drowned += s.drowned;
    total.trickles += s.trickles;
    total.falls += s.falls;
    total.plunges += s.plunges;
    total.horsetails += s.horsetails;
    total.tiered += s.tiered;
    total.drawn_falls += s.drawn_falls;
    total.mouth_falls += s.mouth_falls;
    total.knick_falls += s.knick_falls;
    total.columns += s.columns;
    total.vertices += s.vertices;
    total.highest_m = std::max(total.highest_m, s.highest_m);
    if (!out_path.empty()) {
      const JsonValue list = channels::falls_json(e.layer, set, paths);
      for (const JsonValue& f : list.as_array())
        all.push_back(f);
    }
  }
  if (!out_path.empty()) {
    JsonValue out = JsonValue::object();
    out["falls"] = std::move(all);
    if (io::write_file_atomic(out_path, write_json(out)) != io::Status::Ok)
      return fail("cannot write " + out_path);
  }
  JsonValue line = JsonValue::object();
  line["command"] = JsonValue("falls");
  line["scene"] = JsonValue(scene_path);
  line["falls_version"] = JsonValue(channels::k_falls_version);
  line["entries"] = JsonValue(static_cast<u32>(entries.size()));
  line["entries_with_plan"] = std::move(keys);
  line["planned"] = JsonValue(planned);
  line["cooked"] = JsonValue(cooked);
  // The count (stage 0): every link tested, every run of the bed's steps, and what each came to.
  line["links"] = JsonValue(total.links);
  line["runs"] = JsonValue(total.runs);
  line["held"] = JsonValue(total.held);
  line["drowned"] = JsonValue(total.drowned);
  line["trickles"] = JsonValue(total.trickles);
  line["falls"] = JsonValue(total.falls);
  line["plunges"] = JsonValue(total.plunges);
  line["horsetails"] = JsonValue(total.horsetails);
  line["tiered"] = JsonValue(total.tiered);
  line["drawn_falls"] = JsonValue(total.drawn_falls);
  line["mouth_falls"] = JsonValue(total.mouth_falls);
  line["knick_falls"] = JsonValue(total.knick_falls);
  line["highest_fall_m"] = JsonValue(std::round(total.highest_m * 1000) / 1000);
  line["columns"] = JsonValue(total.columns);
  line["vertices"] = JsonValue(total.vertices);
  line["bytes"] = JsonValue(bytes);
  line["bytes_per_run"] = JsonValue(
      falls_written > 0 ? std::round(static_cast<f64>(bytes) / static_cast<f64>(falls_written))
                        : 0.0);
  line["plan_s"] = JsonValue(std::round(plan_seconds * 1000) / 1000);
  line["cook_s"] = JsonValue(std::round(cook_seconds * 1000) / 1000);
  line["jobs"] = JsonValue(job_count);
  JsonWriteOptions compact;
  compact.pretty = false;
  std::printf("%s\n", write_json(line, compact).c_str());
  return 0;
#endif
}

}  // namespace engine::content

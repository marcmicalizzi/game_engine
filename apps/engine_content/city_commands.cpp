// engine-content city: Island City's plan and building grammar on the command line
// (docs/subsystems/city.md, docs/subsystems/apps.md). Compiled against domain/city when this build
// has the capability (ENGINE_CONTENT_CITY); without it the command is refused with a sentence, as
// `ruins` is, and the minimal preset is the proof that the rest of the app builds.
//
//   city params <params.json>                      the default island's parameters, every table
//                                                  spelled out, for an owner to edit
//   city plan <params.json> --out <dir> | --cache  the whole-island plan: <dir>/plan.json, or the
//                                                  derived-data cache's ddc/city/<key>/plan.json
//   city building --plan <dir> --lot <id>          one lot's building and what the validators say
//   city fragment --plan <dir> --tile x,z | --district <id> --out <scene.json>
//                                                  the proxy scene fragment the renderer draws
//   city yield --plan <dir> [--buildings 200]      E18's measurement
//
// Each prints one JSON line on stdout and returns 0, 1 when a file could not be read, written or
// made, and 2 for a usage error.
#include "content_commands.h"

#include <core/base/types.h>
#include <core/json/json.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_CITY
#include <core/containers/vector.h>
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/city/city.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <charconv>
#include <filesystem>
#endif

namespace engine::content {

namespace {

[[maybe_unused]] constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;

const char* k_city_usage =
    "usage: engine-content city <command> ...   Island City's plan and building grammar\n"
    "  params <params.json>                 write the default island's parameters, every table\n"
    "                                       spelled out (engine.city.IslandParams)\n"
    "  plan <params.json> --out <dir>       make the whole-island plan and write <dir>/plan.json\n"
    "      --cache                          write it to the derived-data cache instead:\n"
    "                                       <ddc>/city/<key>/plan.json, kept when it is there\n"
    "      --ddc <dir>                      the cache's root (default: <repo>/ddc)\n"
    "      --no-write                       make and report without writing\n"
    "      --jobs <n>                       performance-pool workers (default: one per CPU)\n"
    "  building --plan <dir|plan.json> --lot <id>   one lot's building and its validator report\n"
    "      --stage massing|floors|rooms     how far the grammar runs (default rooms)\n"
    "      --out <building.json>            write the description (engine.city.BuildingFile)\n"
    "  fragment --plan <dir|plan.json> (--tile x,z | --district <id>) --out <scene.json>\n"
    "                                       the proxy scene fragment (engine.scene.Scene)\n"
    "      --detail massing|floors|rooms    how far the buildings go (default: rooms for a tile,\n"
    "                                       floors for a district)\n"
    "      --meshes <dir>                   where the twelve proxy meshes go (default: proxy/\n"
    "                                       beside the fragment)\n"
    "      --jobs <n>                       as above\n"
    "  yield --plan <dir|plan.json> [--buildings <n>] [--jobs <n>]\n"
    "                                       E18: n buildings (default 200) across the archetypes,\n"
    "                                       each generated and validated; the pass rate by\n"
    "                                       archetype and by validator\n";

[[maybe_unused]] int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content city: %s\n", message);
  std::fputs(k_city_usage, stderr);
  return k_exit_usage;
}

[[maybe_unused]] int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content city: %s\n", message.c_str());
  return k_exit_error;
}

[[maybe_unused]] void print_json(const JsonValue& value) {
  std::string text = write_json(value, JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
}

#if ENGINE_CONTENT_CITY

using namespace engine::city;

std::string hex16(u64 v) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
  return text;
}

bool parse_u64(std::string_view text, u64& out) {
  const char* end = text.data() + text.size();
  const auto r = std::from_chars(text.data(), end, out);
  return r.ec == std::errc() && r.ptr == end && !text.empty();
}

bool parse_pair(std::string_view text, i32& a, i32& b) {
  const usize comma = text.find(',');
  if (comma == std::string_view::npos) return false;
  const std::string_view x = text.substr(0, comma);
  const std::string_view z = text.substr(comma + 1);
  const auto ra = std::from_chars(x.data(), x.data() + x.size(), a);
  const auto rb = std::from_chars(z.data(), z.data() + z.size(), b);
  return !x.empty() && !z.empty() && ra.ec == std::errc() && ra.ptr == x.data() + x.size() &&
         rb.ec == std::errc() && rb.ptr == z.data() + z.size();
}

f64 km2(i64 cm2) { return static_cast<f64>(cm2) * 1.0e-10; }

// A plan given as its directory or its file.
std::string plan_path(const std::string& given) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (fs::is_directory(fs::path(given), ec)) return io::join_path(given, "plan.json");
  return given;
}

JsonValue report_json(const Report& report) {
  JsonValue by_rule = JsonValue::object();
  for (u32 k = 0; k < k_rules; ++k) {
    if (report.by_rule[k] != 0)
      by_rule.set(rule_name(static_cast<Rule>(k)), JsonValue(report.by_rule[k]));
  }
  return by_rule;
}

jobs::JobSystemConfig pool_config(u64 wanted) {
  jobs::JobSystemConfig config;
  if (wanted > 0) config.performance_workers = static_cast<u32>(wanted - 1);
  config.efficiency_workers = 1;
  return config;
}

int params_command(int argc, char** argv) {
  if (argc != 4) return usage("params takes one path");
  std::string error;
  if (!write_params_file(argv[3], default_island_params(), &error)) return failed(error);
  Params p;
  if (!read_params_file(argv[3], p, error)) return failed(error);
  JsonValue line = JsonValue::object();
  line.set("params", JsonValue(std::string(argv[3])));
  line.set("key", JsonValue(hex16(plan_key(p.source))));
  print_json(line);
  return k_exit_ok;
}

int plan_command(int argc, char** argv) {
  std::string params_path, out_dir, ddc;
  bool cache = false, write = true;
  u64 jobs_wanted = 0;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& v) {
      if (i + 1 >= argc) return false;
      v = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--out") {
      if (!value(out_dir)) return usage("--out needs a directory");
    } else if (a == "--cache") {
      cache = true;
    } else if (a == "--ddc") {
      if (!value(ddc)) return usage("--ddc needs a directory");
    } else if (a == "--no-write") {
      write = false;
    } else if (a == "--jobs") {
      if (!value(v) || !parse_u64(v, jobs_wanted) || jobs_wanted == 0 || jobs_wanted > 4096)
        return usage("--jobs is 1..4096");
    } else if (!a.empty() && a[0] == '-') {
      return usage("unknown option for plan");
    } else if (params_path.empty()) {
      params_path = std::string(a);
    } else {
      return usage("plan takes one parameters file");
    }
  }
  if (params_path.empty()) return usage("plan takes a parameters file");
  if (write && out_dir.empty() && !cache)
    return usage("plan needs --out <dir>, --cache or --no-write");
  Params params;
  std::string error;
  if (!read_params_file(params_path, params, error)) return failed(error);
  const u64 key = plan_key(params.source);
  if (cache) {
    if (ddc.empty())
      ddc = geometry::find_ddc_root(io::parent_path(io::normalize_path(params_path)));
    if (ddc.empty()) ddc = geometry::find_ddc_root(".");
    if (ddc.empty()) return failed("no derived-data root: give --ddc <dir>");
    out_dir = plan_cache_dir(ddc, key);
  }
  const std::string path = out_dir.empty() ? std::string() : io::join_path(out_dir, "plan.json");
  Plan plan;
  bool hit = false;
  const i64 start = time::monotonic_ns();
  if (cache && io::exists(path)) {
    std::string read_error;
    hit = read_plan_file(path, plan, read_error) && plan.key == key;
  }
  if (!hit) {
    jobs::JobSystem pool(pool_config(jobs_wanted));
    if (!generate_plan(params, plan, error, &pool)) return failed(error);
  }
  const i64 elapsed = time::monotonic_ns() - start;
  if (write && !hit && !write_plan_file(path, plan, &error)) return failed(error);
  PlanStats st;
  plan_stats(plan, st);
  Report report;
  validate_plan(plan, report);
  JsonValue line = JsonValue::object();
  line.set("name", JsonValue(plan.params.name));
  line.set("seed", JsonValue(plan.params.seed));
  line.set("key", JsonValue(hex16(plan.key)));
  line.set("hash", JsonValue(hex16(hash_plan(plan))));
  line.set("cached", JsonValue(hit));
  line.set("ms", JsonValue(static_cast<f64>(elapsed) / 1.0e6));
  line.set("city_km2", JsonValue(km2(st.city_area_cm2)));
  JsonValue districts = JsonValue::object();
  for (u32 k = 0; k < k_district_kinds; ++k) {
    JsonValue d = JsonValue::object();
    d.set("count", JsonValue(st.districts_by_kind[k]));
    d.set("km2", JsonValue(km2(st.district_area_cm2[k])));
    districts.set(district_kind_name(static_cast<DistrictKind>(k)), std::move(d));
  }
  line.set("districts", std::move(districts));
  JsonValue streets = JsonValue::object();
  for (u32 k = 0; k < k_street_classes; ++k) {
    JsonValue s = JsonValue::object();
    s.set("count", JsonValue(st.streets_by_class[k]));
    s.set("km", JsonValue(static_cast<f64>(st.street_length_cm[k]) * 1.0e-5));
    streets.set(street_class_name(static_cast<StreetClass>(k)), std::move(s));
  }
  line.set("streets", std::move(streets));
  line.set("nodes", JsonValue(st.nodes));
  line.set("segments", JsonValue(st.segments));
  line.set("blocks", JsonValue(st.blocks));
  line.set("lots", JsonValue(st.lots));
  JsonValue archetypes = JsonValue::object();
  for (u32 k = 0; k < k_archetypes; ++k)
    archetypes.set(archetype_name(static_cast<Archetype>(k)), JsonValue(st.lots_by_archetype[k]));
  line.set("archetypes", std::move(archetypes));
  JsonValue parks = JsonValue::object();
  for (u32 k = 1; k < 7; ++k)
    parks.set(park_kind_name(static_cast<ParkKind>(k)), JsonValue(st.parks_by_kind[k]));
  parks.set("km2", JsonValue(km2(st.park_area_cm2)));
  line.set("parks", std::move(parks));
  JsonValue civic = JsonValue::object();
  for (u32 k = 1; k < 5; ++k)
    civic.set(civic_kind_name(static_cast<CivicKind>(k)), JsonValue(st.civic_by_kind[k]));
  line.set("civic", std::move(civic));
  line.set("validators", report_json(report));
  if (!path.empty() && write) line.set("output", JsonValue(path));
  print_json(line);
  return k_exit_ok;
}

int building_command(int argc, char** argv) {
  std::string plan_arg, out;
  u64 lot_id = ~u64{0};
  Stage stage = Stage::Rooms;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& v) {
      if (i + 1 >= argc) return false;
      v = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--plan") {
      if (!value(plan_arg)) return usage("--plan needs a directory or a file");
    } else if (a == "--lot") {
      if (!value(v) || !parse_u64(v, lot_id) || lot_id > 0xffffffffull)
        return usage("--lot is a lot's id");
    } else if (a == "--stage") {
      if (!value(v) || !parse_stage(v, stage)) return usage("--stage is massing, floors or rooms");
    } else if (a == "--out") {
      if (!value(out)) return usage("--out needs a path");
    } else {
      return usage("unknown option for building");
    }
  }
  if (plan_arg.empty() || lot_id == ~u64{0}) return usage("building needs --plan and --lot");
  Plan plan;
  std::string error;
  if (!read_plan_file(plan_path(plan_arg), plan, error)) return failed(error);
  const u32 lot = plan.find_lot(static_cast<u32>(lot_id));
  if (lot == k_no_id) return failed("the plan has no lot " + std::to_string(lot_id));
  Grammar grammar(plan);
  Building b;
  const i64 start = time::monotonic_ns();
  if (!grammar.generate(lot, stage, b, &error)) return failed(error);
  const i64 generated = time::monotonic_ns();
  Report report;
  validate_building(plan, b, report);
  const i64 validated = time::monotonic_ns();
  if (!out.empty()) {
    BuildingFile file;
    building_to_schema(plan, b, &report, file);
    const std::string text = write_json(schema::to_json(file)) + "\n";
    const std::string_view dir = io::parent_path(out);
    if (!dir.empty() && io::make_directories(dir) != io::Status::Ok)
      return failed("cannot create " + std::string(dir));
    if (io::write_file(out, text) != io::Status::Ok) return failed("cannot write " + out);
  }
  const Lot& l = plan.lots[lot];
  JsonValue line = JsonValue::object();
  line.set("lot", JsonValue(l.id));
  line.set("archetype", JsonValue(archetype_name(b.archetype)));
  line.set("district", JsonValue(district_kind_name(plan.districts[l.district].kind)));
  if (b.civic != CivicKind::None) line.set("civic", JsonValue(civic_kind_name(b.civic)));
  line.set("stage", JsonValue(stage_name(stage)));
  line.set("seed", JsonValue(hex16(b.seed)));
  JsonValue fp = JsonValue::array();
  const Rect f = footprint_on_plan(b);
  for (const i32 v : {f.x0, f.z0, f.x1, f.z1})
    fp.push_back(JsonValue(static_cast<i64>(v)));
  line.set("footprint_cm", std::move(fp));
  line.set("floors", JsonValue(b.floors.size()));
  line.set("height_m", JsonValue(static_cast<f64>(b.height_cm()) / 100.0));
  line.set("cores", JsonValue(b.cores.size()));
  line.set("units", JsonValue(b.units.size()));
  line.set("spaces", JsonValue(b.spaces.size()));
  line.set("walls", JsonValue(b.walls.size()));
  line.set("openings", JsonValue(b.openings.size()));
  line.set("zones", JsonValue(b.zones.size()));
  const OccupancySummary& o = b.occupancy;
  JsonValue occupancy = JsonValue::object();
  occupancy.set("dwellings", JsonValue(o.dwellings));
  occupancy.set("bedrooms", JsonValue(o.bedrooms));
  occupancy.set("residents", JsonValue(o.residents));
  JsonValue work = JsonValue::object();
  for (u32 k = 0; k < k_work_kinds; ++k) {
    if (o.workplaces[k] != 0)
      work.set(work_kind_name(static_cast<WorkKind>(k)), JsonValue(o.workplaces[k]));
  }
  occupancy.set("workplaces", std::move(work));
  line.set("occupancy", std::move(occupancy));
  line.set("passed", JsonValue(report.passed()));
  line.set("failures", report_json(report));
  line.set("hash", JsonValue(hex16(hash_building(b))));
  line.set("generate_ms", JsonValue(static_cast<f64>(generated - start) / 1.0e6));
  line.set("validate_ms", JsonValue(static_cast<f64>(validated - generated) / 1.0e6));
  if (!out.empty()) line.set("output", JsonValue(out));
  print_json(line);
  return k_exit_ok;
}

int fragment_command(int argc, char** argv) {
  std::string plan_arg, out, meshes, v;
  bool have_tile = false, have_district = false, have_detail = false;
  i32 tx = 0, tz = 0;
  u64 district = 0, jobs_wanted = 0;
  Stage detail = Stage::Rooms;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& s) {
      if (i + 1 >= argc) return false;
      s = argv[++i];
      return true;
    };
    if (a == "--plan") {
      if (!value(plan_arg)) return usage("--plan needs a directory or a file");
    } else if (a == "--tile") {
      if (!value(v) || !parse_pair(v, tx, tz)) return usage("--tile is x,z");
      have_tile = true;
    } else if (a == "--district") {
      if (!value(v) || !parse_u64(v, district)) return usage("--district is a district's id");
      have_district = true;
    } else if (a == "--detail") {
      if (!value(v) || !parse_stage(v, detail))
        return usage("--detail is massing, floors or rooms");
      have_detail = true;
    } else if (a == "--out") {
      if (!value(out)) return usage("--out needs a path");
    } else if (a == "--meshes") {
      if (!value(meshes)) return usage("--meshes needs a directory");
    } else if (a == "--jobs") {
      if (!value(v) || !parse_u64(v, jobs_wanted) || jobs_wanted == 0 || jobs_wanted > 4096)
        return usage("--jobs is 1..4096");
    } else {
      return usage("unknown option for fragment");
    }
  }
  if (plan_arg.empty() || out.empty() || have_tile == have_district)
    return usage("fragment needs --plan, --out, and one of --tile or --district");
  if (!have_detail && have_district) detail = Stage::Floors;
  Plan plan;
  std::string error;
  if (!read_plan_file(plan_path(plan_arg), plan, error)) return failed(error);
  Vector<Proxy> proxies;
  u32 buildings = 0;
  const i64 start = time::monotonic_ns();
  if (have_tile) {
    Vector<u32> lots;
    lots_in_tile(plan, TileCoord{tx, tz}, lots);
    for (const u32 l : lots)
      buildings += plan.lots[l].use != LotUse::Park ? 1u : 0u;
    if (!tile_proxies(plan, TileCoord{tx, tz}, detail, proxies, &error)) return failed(error);
  } else {
    if (district >= plan.districts.size())
      return failed("the plan has no district " + std::to_string(district));
    Vector<u32> lots;
    for (u32 l = 0; l < plan.lots.size(); ++l) {
      if (plan.lots[l].district == district && plan.lots[l].use != LotUse::Park) lots.push_back(l);
    }
    buildings = lots.size();
    jobs::JobSystem pool(pool_config(jobs_wanted));
    // In runs of a few hundred, so a district of towers at rooms detail never holds every building
    // at once; the proxies are appended in lot order whatever the thread count.
    Vector<Building> batch;
    for (u32 at = 0; at < lots.size(); at += 256) {
      const u32 n = std::min<u32>(256, lots.size() - at);
      if (!generate_buildings(plan, std::span<const u32>(lots.data() + at, n), detail, &pool, batch,
                              &error))
        return failed(error);
      for (const Building& b : batch)
        append_building_proxies(plan, b, detail, proxies);
    }
    append_district_ground_proxies(plan, static_cast<u32>(district), proxies);
  }
  const i64 made = time::monotonic_ns();
  if (meshes.empty()) {
    const std::string_view dir = io::parent_path(out);
    meshes = io::join_path(dir.empty() ? std::string_view(".") : dir, "proxy");
  }
  if (!write_proxy_meshes(meshes, &error)) return failed(error);
  const std::string name =
      plan.params.name + (have_tile ? ", tile " + std::to_string(tx) + "," + std::to_string(tz)
                                    : ", district " + std::to_string(district));
  if (!write_fragment(out, proxies, meshes, name, &error)) return failed(error);
  u32 by_mesh[k_proxy_meshes] = {};
  for (const Proxy& p : proxies)
    ++by_mesh[static_cast<u32>(p.mesh)];
  JsonValue line = JsonValue::object();
  line.set("buildings", JsonValue(buildings));
  line.set("instances", JsonValue(proxies.size()));
  JsonValue counts = JsonValue::object();
  for (u32 m = 0; m < k_proxy_meshes; ++m)
    counts.set(proxy_mesh_name(static_cast<ProxyMesh>(m)), JsonValue(by_mesh[m]));
  line.set("meshes", std::move(counts));
  line.set("detail", JsonValue(stage_name(detail)));
  Vector<Proxy> sorted = proxies;
  sort_proxies(sorted);
  line.set("hash", JsonValue(hex16(hash_proxies(sorted))));
  line.set("ms", JsonValue(static_cast<f64>(made - start) / 1.0e6));
  line.set("output", JsonValue(out));
  print_json(line);
  return k_exit_ok;
}

int yield_command(int argc, char** argv) {
  std::string plan_arg, v;
  u64 count = 200, jobs_wanted = 0;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&](std::string& s) {
      if (i + 1 >= argc) return false;
      s = argv[++i];
      return true;
    };
    if (a == "--plan") {
      if (!value(plan_arg)) return usage("--plan needs a directory or a file");
    } else if (a == "--buildings") {
      if (!value(v) || !parse_u64(v, count) || count == 0 || count > 10000000)
        return usage("--buildings is a positive count");
    } else if (a == "--jobs") {
      if (!value(v) || !parse_u64(v, jobs_wanted) || jobs_wanted == 0 || jobs_wanted > 4096)
        return usage("--jobs is 1..4096");
    } else {
      return usage("unknown option for yield");
    }
  }
  if (plan_arg.empty()) return usage("yield needs --plan");
  Plan plan;
  std::string error;
  if (!read_plan_file(plan_path(plan_arg), plan, error)) return failed(error);
  Vector<u32> lots;
  choose_yield_lots(plan, static_cast<u32>(count), lots);
  jobs::JobSystem pool(pool_config(jobs_wanted));
  YieldTable table;
  const i64 start = time::monotonic_ns();
  if (!measure_yield(plan, std::span<const u32>(lots.data(), lots.size()), &pool, table, &error))
    return failed(error);
  const i64 elapsed = time::monotonic_ns() - start;
  auto row_json = [](const YieldRow& row) {
    JsonValue r = JsonValue::object();
    r.set("buildings", JsonValue(row.buildings));
    r.set("passed", JsonValue(row.passed));
    r.set("yield", JsonValue(row.buildings == 0
                                 ? 0.0
                                 : static_cast<f64>(row.passed) / static_cast<f64>(row.buildings)));
    r.set("units", JsonValue(row.units));
    r.set("units_passed", JsonValue(row.units_passed));
    JsonValue validators = JsonValue::object();
    for (u32 v2 = k_first_building_validator; v2 < k_validators; ++v2)
      validators.set(validator_name(static_cast<Validator>(v2)),
                     JsonValue(row.passed_by_validator[v2]));
    r.set("passed_by_validator", std::move(validators));
    JsonValue failures = JsonValue::object();
    for (u32 k = 0; k < k_rules; ++k) {
      if (row.failures_by_rule[k] != 0)
        failures.set(rule_name(static_cast<Rule>(k)), JsonValue(row.failures_by_rule[k]));
    }
    r.set("failures", std::move(failures));
    return r;
  };
  JsonValue line = JsonValue::object();
  line.set("plan", JsonValue(hex16(plan.key)));
  line.set("all", row_json(table.all));
  JsonValue by = JsonValue::object();
  for (u32 a = 0; a < k_archetypes; ++a) {
    if (table.by_archetype[a].buildings != 0)
      by.set(archetype_name(static_cast<Archetype>(a)), row_json(table.by_archetype[a]));
  }
  line.set("archetypes", std::move(by));
  // The table as one number, for the determinism test: every count of every row, in order.
  u64 h = hash_combine(k_hash_seed, table.all.buildings);
  auto mix = [&](const YieldRow& row) {
    h = hash_combine(h, u64{row.buildings} << 32 | row.passed);
    h = hash_combine(h, u64{row.units} << 32 | row.units_passed);
    for (const u32 n : row.passed_by_validator)
      h = hash_combine(h, n);
    for (const u32 n : row.failures_by_rule)
      h = hash_combine(h, n);
  };
  mix(table.all);
  for (const YieldRow& row : table.by_archetype)
    mix(row);
  line.set("hash", JsonValue(hex16(h)));
  line.set("ms", JsonValue(static_cast<f64>(elapsed) / 1.0e6));
  line.set("threads", JsonValue(pool.worker_count(jobs::Pool::Performance) + 1));
  print_json(line);
  return k_exit_ok;
}

#endif  // ENGINE_CONTENT_CITY

}  // namespace

int city_command(int argc, char** argv) {
#if ENGINE_CONTENT_CITY
  if (argc < 3) return usage(nullptr);
  const std::string_view sub = argv[2];
  if (sub == "params") return params_command(argc, argv);
  if (sub == "plan") return plan_command(argc, argv);
  if (sub == "building") return building_command(argc, argv);
  if (sub == "fragment") return fragment_command(argc, argv);
  if (sub == "yield") return yield_command(argc, argv);
  if (sub == "--help" || sub == "help") {
    std::fputs(k_city_usage, stdout);
    return k_exit_ok;
  }
  return usage("unknown city command");
#else
  (void)argc;
  (void)argv;
  std::fprintf(stderr,
               "engine-content city: this build has no city capability (ENGINE_WITH_CITY=OFF or "
               "a minimal preset); configure with it to make Island City's plan and buildings\n");
  return k_exit_error;
#endif
}

}  // namespace engine::content

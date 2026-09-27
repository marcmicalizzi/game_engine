// engine-content terrain: one tile of a scene's dune field at one game time, as one JSON line for
// tests and agents (docs/subsystems/terrain.md, docs/subsystems/apps.md). Compiled against
// domain/terrain when this build has the capability (ENGINE_CONTENT_TERRAIN); without it the
// command is refused with a sentence, as `ruins` is.
//
//   terrain <scene.json> --tile <x,z> [--time <s>] [--cells <n>] [--detail full|dunes|coarse|floor]
//                        [--tile-size <m>] [--crests]
//
// The scene's `terrain` is read for the generator's description — seed, dune height and
// wavelength, ridges, basins, sand flux — whether or not it names the generator, so an agent can
// look at what the generator would make of any terrain; `--time` overrides the scene's `time`. The
// line is `engine.terrain.TileReport`: the tile's range and mean, its content hash (the number the
// golden tests pin, `tile_hash`) and the field's (`field_hash`), the day's wind and sand flux, how
// far each band has moved since time zero, and — with `--crests` — every crest line.
#include "content_commands.h"

#include <core/base/types.h>
#include <core/json/json.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_TERRAIN
#include <core/jobs/job_system.h>
#include <core/json/json_value.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/terrain/stats.h>
#include <domain/terrain/terrain.h>
#include <foundation/io/vfs.h>

#include <charconv>
#include <cmath>
#include <optional>
#include <schemas/scene.h>
#include <schemas/terrain.h>
#endif

namespace engine::content {

namespace {

// Without the capability only `failed` is reached; the rest stay for the build that has it.
[[maybe_unused]] constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
[[maybe_unused]] constexpr int k_exit_usage = 2;

[[maybe_unused]] const char* k_terrain_usage =
    "usage: engine-content terrain <scene.json> --tile <x,z> [options]\n"
    "  one tile of the scene's dune field at one game time, as one JSON line\n"
    "  (engine.terrain.TileReport): the terrain's fields are read for the dune generator\n"
    "  whether or not the terrain names it\n"
    "      --time <s>          game seconds since the world's epoch (default: the terrain's time)\n"
    "      --cells <n>         quads a side (default 128: the overlay's 25 cm grid at 32 m)\n"
    "      --detail <d>        full, dunes (default), coarse or floor\n"
    "      --tile-size <m>     a tile's edge in metres (default 32)\n"
    "      --crests            list every crest line crossing the tile\n"
    "      --stats             the statistics of a square round the tile (`region`)\n"
    "      --stats-side <m>    its side (default 4096)\n"
    "      --stats-spacing <m> its grid (default 8)\n"
    "      --jobs <n>          performance-pool workers for --stats (default: one per CPU)\n";

[[maybe_unused]] int usage(const char* message) {
  if (message != nullptr) std::fprintf(stderr, "engine-content terrain: %s\n", message);
  std::fputs(k_terrain_usage, stderr);
  return k_exit_usage;
}

[[maybe_unused]] int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content terrain: %s\n", message.c_str());
  return k_exit_error;
}

#if ENGINE_CONTENT_TERRAIN

bool parse_f64(std::string_view text, f64& out) {
  const char* end = text.data() + text.size();
  const auto r = std::from_chars(text.data(), end, out);
  return r.ec == std::errc() && r.ptr == end && !text.empty();
}

bool parse_u32(std::string_view text, u32& out) {
  const char* end = text.data() + text.size();
  const auto r = std::from_chars(text.data(), end, out);
  return r.ec == std::errc() && r.ptr == end && !text.empty();
}

bool parse_tile(std::string_view text, i32& x, i32& z) {
  const usize comma = text.find(',');
  if (comma == std::string_view::npos) return false;
  const std::string_view a = text.substr(0, comma);
  const std::string_view b = text.substr(comma + 1);
  const auto ra = std::from_chars(a.data(), a.data() + a.size(), x);
  const auto rb = std::from_chars(b.data(), b.data() + b.size(), z);
  return !a.empty() && !b.empty() && ra.ec == std::errc() && rb.ec == std::errc() &&
         ra.ptr == a.data() + a.size() && rb.ptr == b.data() + b.size();
}

bool read_scene_terrain(const std::string& path, scene::Terrain& out, std::string& error) {
  std::string text;
  const io::Status status = io::read_file(path, text);
  if (status != io::Status::Ok) {
    error = "cannot read " + path + ": " + io::status_name(status);
    return false;
  }
  JsonValue root;
  const JsonParseResult parsed = parse_json(text, root);
  if (!parsed.ok) {
    error = path + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  scene::Scene file;
  schema::ReadContext ctx;
  if (!schema::from_json(file, root, ctx) || !ctx.ok()) {
    std::string messages;
    for (const schema::Diagnostic& d : ctx.diagnostics) {
      if (!messages.empty()) messages += "; ";
      messages += d.path.empty() ? d.message : d.path + ": " + d.message;
    }
    error = path + ": " + (messages.empty() ? std::string("not a scene") : messages);
    return false;
  }
  if (!file.terrain.has_value()) {
    error = path + ": the scene has no terrain";
    return false;
  }
  out = *file.terrain;
  return true;
}

// The generator's description from a scene's terrain, the conversion the renderer makes
// (systems/renderer/src/terrain.cpp, `field_desc`): metres to millimetres rounded, m^2 a year to
// cm^2 a day.
terrain::FieldDesc field_desc(const scene::Terrain& t) {
  terrain::FieldDesc f;
  f.seed = t.seed;
  f.wind.seed = t.seed;
  f.dune_height = terrain::to_mm(t.dune_height);
  f.wavelength = terrain::to_mm(t.dune_wavelength);
  f.wind.flux_cm2_per_day =
      static_cast<i32>(std::floor(static_cast<f64>(t.sand_flux) * 10'000.0 / 365.0 + 0.5));
  f.wind.storms_per_year = static_cast<i32>(t.storms_per_year);
  f.wind.storm_speed_q16 =
      static_cast<i32>(std::floor(static_cast<f64>(t.storm_strength) * 65'536.0 + 0.5));
  for (const scene::Ridge& r : t.ridges) {
    f.ridges.push_back(terrain::RidgeFeature{terrain::to_mm(r.from.x), terrain::to_mm(r.from.y),
                                             terrain::to_mm(r.to.x), terrain::to_mm(r.to.y),
                                             terrain::to_mm(r.width)});
  }
  for (const scene::Basin& b : t.basins) {
    f.basins.push_back(terrain::BasinFeature{terrain::to_mm(b.center.x), terrain::to_mm(b.center.y),
                                             terrain::to_mm(b.radius)});
  }
  if (t.bands.has_value()) {
    for (const scene::TerrainBand& b : *t.bands) {
      terrain::BandMetres m;
      m.name = b.name;
      m.kind = b.kind == scene::DuneKind::Barchan ? terrain::PrimitiveKind::barchan
                                                  : terrain::PrimitiveKind::transverse;
      m.height_min = b.height_min;
      m.height_max = b.height_max;
      m.cell = b.cell;
      m.share = b.share;
      m.length_min = b.length_min;
      m.length_max = b.length_max;
      m.stoss = b.stoss;
      m.bend = b.bend;
      m.sinuosity = b.sinuosity;
      m.spread_deg = b.spread_deg;
      m.sharpness = b.sharpness;
      m.side_days = b.side_days;
      m.sharp_days = b.sharp_days;
      m.couple = static_cast<terrain::BandCouple>(b.couple);
      m.couple_width = b.couple_width;
      m.far = b.far;
      m.celerity_scale = b.celerity_scale;
      f.bands.push_back(terrain::band_from_metres(m));
    }
  }
  return f;
}

std::string hex(u64 v) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
  return text;
}

terrain::StatsReport stats_report(const terrain::DuneField& field, const terrain::FieldStats& st) {
  terrain::StatsReport r;
  r.corner = Vec2{terrain::height_m(st.x0 * 1000), terrain::height_m(st.z0 * 1000)};
  r.spacing_m = terrain::height_m(st.spacing_mm * 1000);
  r.side_m = terrain::height_m(static_cast<i64>(st.nx - 1) * st.spacing_mm * 1000);
  r.vertices = st.vertices;
  for (const i64 v : st.above_floor_um)
    r.above_floor_m.push_back(terrain::height_m(v));
  for (const u32 e : terrain::k_slope_edges_deg)
    r.slope_edges_deg.push_back(e);
  const f64 sand = st.sand_vertices > 0 ? static_cast<f64>(st.sand_vertices) : 1.0;
  for (const u64 c : st.slope)
    r.slope_share.push_back(static_cast<f32>(static_cast<f64>(c) / sand));
  r.over_repose_share = static_cast<f32>(static_cast<f64>(st.over_repose()) / sand);
  r.over_36_share = static_cast<f32>(static_cast<f64>(st.over_36()) / sand);
  r.over_36_vertices = st.over_36();
  r.flat_share = static_cast<f32>(static_cast<f64>(st.flat) /
                                  static_cast<f64>(st.vertices > 0 ? st.vertices : 1));
  const f64 side_km = static_cast<f64>(static_cast<i64>(st.nx - 1) * st.spacing_mm) * 1e-6;
  const f64 km2 = side_km * side_km > 0.0 ? side_km * side_km : 1.0;
  for (u32 b = 0; b < st.bands; ++b) {
    terrain::BandStatsReport br;
    br.name = field.band_name(b);
    br.primitives = st.band[b].primitives;
    br.crest_m_per_km2 = static_cast<f32>(static_cast<f64>(st.band[b].crest_mm) * 1e-3 / km2);
    br.tallest_m = terrain::height_m(st.band[b].tallest_um);
    r.bands.push_back(std::move(br));
  }
  r.tallest_m = terrain::height_m(st.tallest_um);
  r.tallest_band = st.tallest_um > 0 ? field.band_name(st.tallest_band) : "";
  r.tallest_spacing_m = terrain::height_m(st.tallest_spacing_mm * 1000);
  r.hash = hex(st.hash());
  return r;
}

#endif

}  // namespace

int terrain_command(int argc, char** argv) {
#if !ENGINE_CONTENT_TERRAIN
  (void)argc;
  (void)argv;
  return failed(
      "this build has no terrain capability (ENGINE_WITH_TERRAIN is off): the dune generator is "
      "not "
      "here");
#else
  // argv[0] is the program, argv[1] "terrain".
  if (argc < 3) return usage("a scene file is required");
  const std::string scene_path = argv[2];
  if (scene_path == "--help" || scene_path == "-h") return usage(nullptr);
  bool have_tile = false;
  i32 tile_x = 0;
  i32 tile_z = 0;
  bool have_time = false;
  f64 time_s = 0.0;
  u32 cells = 128;
  f64 tile_m = 32.0;
  bool crests = false;
  bool stats = false;
  f64 stats_side_m = 4096.0;
  f64 stats_spacing_m = 8.0;
  u32 jobs_count = 0;
  terrain::Detail detail = terrain::Detail::dunes;
  for (int i = 3; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        usage((std::string(name) + " needs a value").c_str());
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--tile") {
      const char* v = value("--tile");
      if (v == nullptr) return k_exit_usage;
      if (!parse_tile(v, tile_x, tile_z)) return usage("--tile is two integers, x,z");
      have_tile = true;
    } else if (arg == "--time") {
      const char* v = value("--time");
      if (v == nullptr) return k_exit_usage;
      if (!parse_f64(v, time_s) || !(time_s >= 0.0) || !(time_s < 3.0e11))
        return usage("--time is game seconds within [0, 3e11)");
      have_time = true;
    } else if (arg == "--cells") {
      const char* v = value("--cells");
      if (v == nullptr) return k_exit_usage;
      if (!parse_u32(v, cells) || cells < 1 || cells > 1024)
        return usage("--cells is a whole number within 1..1024");
    } else if (arg == "--tile-size") {
      const char* v = value("--tile-size");
      if (v == nullptr) return k_exit_usage;
      if (!parse_f64(v, tile_m) || !(tile_m >= 1.0) || !(tile_m <= 4096.0))
        return usage("--tile-size is metres within 1..4096");
    } else if (arg == "--detail") {
      const char* v = value("--detail");
      if (v == nullptr) return k_exit_usage;
      const std::string_view d = v;
      if (d == "full")
        detail = terrain::Detail::full;
      else if (d == "dunes")
        detail = terrain::Detail::dunes;
      else if (d == "coarse")
        detail = terrain::Detail::coarse;
      else if (d == "floor")
        detail = terrain::Detail::floor;
      else
        return usage("--detail is full, dunes, coarse or floor");
    } else if (arg == "--crests") {
      crests = true;
    } else if (arg == "--stats") {
      stats = true;
    } else if (arg == "--stats-side") {
      const char* v = value("--stats-side");
      if (v == nullptr) return k_exit_usage;
      if (!parse_f64(v, stats_side_m) || !(stats_side_m >= 1.0) || !(stats_side_m <= 100'000.0))
        return usage("--stats-side is metres within 1..100000");
    } else if (arg == "--stats-spacing") {
      const char* v = value("--stats-spacing");
      if (v == nullptr) return k_exit_usage;
      if (!parse_f64(v, stats_spacing_m) || !(stats_spacing_m >= 0.01) ||
          !(stats_spacing_m <= 1000.0))
        return usage("--stats-spacing is metres within 0.01..1000");
    } else if (arg == "--jobs") {
      const char* v = value("--jobs");
      if (v == nullptr) return k_exit_usage;
      if (!parse_u32(v, jobs_count) || jobs_count > 256) return usage("--jobs is 0..256");
    } else {
      return usage(("unknown option " + std::string(arg)).c_str());
    }
  }
  if (!have_tile) return usage("--tile is required");

  scene::Terrain source;
  std::string error;
  if (!read_scene_terrain(scene_path, source, error)) return failed(error);
  const i64 tile_mm = static_cast<i64>(std::floor(tile_m * 1000.0 + 0.5));
  if (tile_mm % cells != 0) {
    return failed("the tile (" + std::to_string(tile_mm) + " mm) is not a whole number of " +
                  std::to_string(cells) + " cells");
  }
  if (!have_time) time_s = source.time;
  const i64 time_us = static_cast<i64>(std::floor(time_s * 1'000'000.0 + 0.5));

  const terrain::FieldDesc desc = field_desc(source);
  if (source.bands.has_value() &&
      !terrain::validate_bands(
          std::span<const terrain::BandDesc>(desc.bands.data(), desc.bands.size()), &error)) {
    return failed(scene_path + ": terrain.bands: " + error);
  }
  if (source.storms_per_year > static_cast<u32>(terrain::k_max_storms_per_year) ||
      !(source.storm_strength > 0.0f) || !(source.storm_strength <= 3.0f)) {
    return failed(scene_path +
                  ": terrain: storms_per_year must be within 0..31 and "
                  "storm_strength within (0, 3]");
  }
  const terrain::DuneField field(desc);
  terrain::TileOptions options;
  options.tile_mm = tile_mm;
  options.cells = cells;
  options.detail = detail;
  terrain::TileOutput tile;
  const i64 start = time::monotonic_ns();
  terrain::evaluate_tile(field, terrain::TileCoord{tile_x, tile_z}, time_us, options, nullptr,
                         nullptr, tile);
  const i64 elapsed = time::monotonic_ns() - start;

  terrain::TileReport report;
  report.format = "engine.terrain.tile";
  report.tile_x = tile_x;
  report.tile_z = tile_z;
  report.time_s = static_cast<f64>(time_us) / 1'000'000.0;
  report.tile_m = static_cast<f32>(tile_m);
  report.cells = cells;
  report.detail = terrain::detail_name(detail);
  report.field_hash = hex(field.hash());
  report.tile_hash = hex(tile.hash());
  report.min_m = terrain::height_m(tile.min_um);
  report.max_m = terrain::height_m(tile.max_um);
  report.mean_m = terrain::height_m(tile.mean_um);
  report.primitives = tile.primitives;
  report.sand_share =
      static_cast<f32>(tile.sand_vertices) / static_cast<f32>((cells + 1) * (cells + 1));
  report.wind_direction = tile.wind.direction;
  report.wind_speed_mps = tile.wind.speed_mps;
  report.flux_m2_per_day = tile.wind.flux_m2_per_day;
  report.saltation_m2_per_day = tile.wind.saltation_m2_per_day;
  report.storm = tile.wind.storm;
  for (u32 b = 0; b < field.band_count(); ++b) {
    i64 dx = 0;
    i64 dz = 0;
    field.displacement(b, time_us, dx, dz);
    report.displacement_m.push_back(
        Vec2{terrain::height_m(dx * 1000), terrain::height_m(dz * 1000)});
  }
  if (crests) {
    for (const terrain::CrestLine& c : tile.crests) {
      terrain::CrestReport r;
      r.band = field.band_name(c.band);
      r.kind =
          c.kind == static_cast<u8>(terrain::PrimitiveKind::barchan) ? "barchan" : "transverse";
      for (const Vec3& p : c.points)
        r.points.push_back(p);
      r.lee = c.lee;
      r.height_m = c.height_m;
      r.sharpness = c.sharpness;
      r.celerity_m_per_day = c.celerity_m_per_day;
      report.crests.push_back(std::move(r));
    }
  }
  report.eval_ms = static_cast<f64>(elapsed) / 1e6;

  // The tile's own statistics over its grid, and with --stats a square round it.
  const i64 spacing = tile_mm / static_cast<i64>(cells);
  terrain::FieldStats tile_stats;
  terrain::field_stats(field, static_cast<i64>(tile_x) * tile_mm,
                       static_cast<i64>(tile_z) * tile_mm, cells + 1, cells + 1, spacing, time_us,
                       detail, nullptr, nullptr, tile_stats);
  report.tile_stats = stats_report(field, tile_stats);
  if (stats) {
    const i64 step = static_cast<i64>(std::floor(stats_spacing_m * 1000.0 + 0.5));
    const i64 side = static_cast<i64>(std::floor(stats_side_m * 1000.0 + 0.5));
    const u32 n = static_cast<u32>(side / step) + 1;
    const i64 cx = static_cast<i64>(tile_x) * tile_mm + tile_mm / 2;
    const i64 cz = static_cast<i64>(tile_z) * tile_mm + tile_mm / 2;
    std::optional<jobs::JobSystem> pool;
    jobs::JobSystemConfig config;
    config.performance_workers = jobs_count;
    config.pin_threads = false;
    if (jobs_count != 1) pool.emplace(config);
    terrain::FieldStats region;
    terrain::field_stats(field, cx - side / 2, cz - side / 2, n, n, step, time_us, detail, nullptr,
                         pool.has_value() ? &*pool : nullptr, region);
    report.region = stats_report(field, region);
  }
  std::string text = write_json(schema::to_json(report), JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
  return k_exit_ok;
#endif
}

}  // namespace engine::content

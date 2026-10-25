// engine-content terrain-synth and terrain-tiles: an authored heightfield and the tile set built
// from it (docs/subsystems/tileset.md, docs/subsystems/apps.md). Compiled against domain/tileset
// when this build has the capability (ENGINE_CONTENT_TILESET); without it both are refused with a
// sentence, as `terrain` is.
//
//   terrain-synth <out.json> --kind <ridge|basin|steps|flat|mountain> --size <n> --spacing-mm <s>
//                 [--seed <n>] [--origin-mm <x,z>]
//   terrain-tiles <heightfield.json> [<out dir>] [--ddc <root>] [--cells <n>]
//                 [--ring-spacing-mm <s>] [--far-ratio <r>] [--far-levels <n>]
//                 [--vertical-scale <v>] [--offset-mm <x,y,z>] [--force]
//
//   terrain-masks <scene.json> [<out dir>] [--ddc <root>] [--force] [--write-priors <dir>]
//
// `terrain-masks` builds the mask level of a scene terrain's `masks` block (ground materials by
// place; tileset.md, "The mask level") over the tile set its `tiles` block names, as the `tiles`
// provider builds it on a miss: the same key, the same bytes. `--write-priors` also writes the
// generated priors as one greyscale PNG per role, for an author to edit and paint back.
//
// `terrain-tiles` builds into `<out dir>`, or with none into the derived-data cache
// (`<ddc>/tilesets/<key>/`, `--ddc` or the repository's), and skips the build when the manifest
// there records this build's key: a second build is a hit. It prints one JSON line,
// `engine.tileset.TileSetReport`. It is not a `build-all` step: `content_build::DerivedStep::build`
// is handed a source's bytes and no path, and a heightfield's header names its grid beside it.
//
//   terrain-read <manifest.json | dir> [--pattern level|windows|points|files] [--level <n>]
//                [--depth <n>] [--run-kib <n>] [--window <n>] [--cache-mib <n>] [--unbuffered]
//                [--masks]
//
// `terrain-read` is a measurement, not a build: it reads a set's level the way a load does — whole
// in file order, as windows that are each one batch, or point by point — or a directory of the old
// format's tile files one at a time, and prints the seconds, the bytes and the machine's state
// (docs/experiments/tile-archives-2026-10-24.md).
#include "content_commands.h"

#include <core/base/types.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_TILESET
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/masks.h>
#include <domain/tileset/tileset.h>
#include <foundation/bench/machine_state.h>
#include <foundation/io/vfs.h>

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <schemas/scene.h>
#endif

namespace engine::content {

namespace {

[[maybe_unused]] constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
[[maybe_unused]] constexpr int k_exit_usage = 2;

[[maybe_unused]] const char* k_synth_usage =
    "usage: engine-content terrain-synth <out.json> --kind <ridge|basin|steps|flat|mountain> "
    "--size <n>\n"
    "                                    --spacing-mm <s> [--seed <n>] [--origin-mm <x,z>]\n"
    "  writes an engine.terrain.heightfield/1 header and its float32 grid (<stem>.f32) beside\n"
    "  it: an analytic shape, n points a side, s mm apart, for tests and for a terrain without a\n"
    "  DEM\n";

[[maybe_unused]] const char* k_tiles_usage =
    "usage: engine-content terrain-tiles <heightfield.json> [<out dir>] [options]\n"
    "  builds the tile set of an authored heightfield: its finest tiles with a point of apron,\n"
    "  and its far mip chain, each level box-filtered over each point's cell; into <out dir>, or\n"
    "  the derived-data cache's tilesets/<key>/; prints one JSON line\n"
    "      --ddc <root>             the derived-data root (default: the repository's ddc/)\n"
    "      --cells <n>              points a tile side less one (default 64)\n"
    "      --ring-spacing-mm <s>    the outermost ring's spacing (default 2000)\n"
    "      --far-ratio <r>          the first far level's ratio to it (default 4)\n"
    "      --far-levels <n>         doublings the chain holds (default 6)\n"
    "      --vertical-scale <v>     every height times this (default 1)\n"
    "      --offset-mm <x,y,z>      moves the grid (x, z: whole spacings) and lifts it (y)\n"
    "      --force                  build even when the cache holds the key\n";

[[maybe_unused]] const char* k_masks_usage =
    "usage: engine-content terrain-masks <scene.json> [<out dir>] [options]\n"
    "  builds the mask level of the scene terrain's masks block over its tile set: per tile, the\n"
    "  palette layers it names and their weights; into <out dir>, or the derived-data cache's\n"
    "  tilemasks/<key>/; prints one JSON line\n"
    "      --ddc <root>             the derived-data root (default: the tiles block's, or the\n"
    "                               repository's round the scene)\n"
    "      --force                  build even when the cache holds the key\n"
    "      --write-priors <dir>     also write the priors as greyscale PNGs, one per role\n";

[[maybe_unused]] const char* k_read_usage =
    "usage: engine-content terrain-read <manifest.json | dir> [options]\n"
    "  reads a built tile set (or mask level) the way a load does and prints one JSON line of\n"
    "  what it cost: seconds, bytes, MB/s, the reader's batches, runs and queue depth, and the\n"
    "  machine's state at both ends (a measurement: tileset.md, \"Reading a level\")\n"
    "      --pattern <p>            level: the level whole, in file order, one batch (default);\n"
    "                               windows: square windows of --window tiles a side over the\n"
    "                               level in a scattered order, each window one batch; points: a\n"
    "                               point query a tile in a scattered order, each miss its own\n"
    "                               read (the scene load's standing); files: every file of <dir>\n"
    "                               read whole, one at a time (a level before its archive)\n"
    "      --level <n>              the manifest's level, the finest 0 (default 0)\n"
    "      --depth <n>              range reads in flight in one batch (default 32)\n"
    "      --run-kib <n>            adjacent tiles read as one run up to this (default 256)\n"
    "      --window <n>             tiles a side of a window (default 8)\n"
    "      --cache-mib <n>          the reader's tile budget (default 1024)\n"
    "      --unbuffered             read past the OS's file cache (cold reads, no rights needed)\n"
    "      --masks                  the manifest is a mask level's: every tile in order, twice\n";

[[maybe_unused]] int usage(const char* text, const std::string& message) {
  if (!message.empty()) std::fprintf(stderr, "engine-content: %s\n", message.c_str());
  std::fputs(text, stderr);
  return k_exit_usage;
}

int failed(std::string_view command, const std::string& message) {
  std::fprintf(stderr, "engine-content %.*s: %s\n", static_cast<int>(command.size()),
               command.data(), message.c_str());
  return k_exit_error;
}

#if ENGINE_CONTENT_TILESET

template <typename T>
bool parse_int(std::string_view text, T& out) {
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
  return ec == std::errc{} && end == text.data() + text.size();
}

bool parse_f64(std::string_view text, f64& out) {
  const std::string copy(text);
  char* end = nullptr;
  out = std::strtod(copy.c_str(), &end);
  return end == copy.c_str() + copy.size() && !copy.empty();
}

// "a,b" or "a,b,c" of integers.
bool parse_ints(std::string_view text, i64* out, u32 n) {
  for (u32 k = 0; k < n; ++k) {
    const usize comma = k + 1 < n ? text.find(',') : std::string_view::npos;
    if (k + 1 < n && comma == std::string_view::npos) return false;
    if (!parse_int(text.substr(0, comma), out[k])) return false;
    text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
  }
  return true;
}

int synth_command(int argc, char** argv) {
  if (argc < 3) return usage(k_synth_usage, "");
  const std::string out = argv[2];
  tileset::SynthKind kind = tileset::SynthKind::flat;
  bool have_kind = false;
  u32 size = 0;
  i64 spacing = 0;
  u64 seed = 0;
  i64 origin[2] = {0, 0};
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    const bool has_value = i + 1 < argc;
    if (a == "--kind" && has_value) {
      if (!tileset::parse_synth_kind(argv[++i], kind))
        return usage(k_synth_usage, "--kind is ridge, basin, steps, flat or mountain");
      have_kind = true;
    } else if (a == "--size" && has_value) {
      if (!parse_int(std::string_view(argv[++i]), size) || size < 2 ||
          size > tileset::k_max_heightfield_side)
        return usage(k_synth_usage, "--size is 2 to 65,536 points");
    } else if (a == "--spacing-mm" && has_value) {
      if (!parse_int(std::string_view(argv[++i]), spacing) || spacing < 1 ||
          spacing > tileset::k_max_spacing_mm)
        return usage(k_synth_usage, "--spacing-mm is 1 to 1,000,000");
    } else if (a == "--seed" && has_value) {
      if (!parse_int(std::string_view(argv[++i]), seed))
        return usage(k_synth_usage, "--seed is a whole number");
    } else if (a == "--origin-mm" && has_value) {
      if (!parse_ints(argv[++i], origin, 2)) return usage(k_synth_usage, "--origin-mm is x,z");
    } else {
      return usage(k_synth_usage, "unknown argument '" + std::string(a) + "'");
    }
  }
  if (!have_kind || size == 0 || spacing == 0)
    return usage(k_synth_usage, "--kind, --size and --spacing-mm are required");
  const tileset::Heightfield field =
      tileset::synth_heightfield(kind, size, spacing, seed, origin[0], origin[1]);
  std::string error;
  if (!tileset::write_heightfield(out, field, error)) return failed("terrain-synth", error);
  std::printf("{\"heightfield\":\"%s\",\"width\":%u,\"height\":%u,\"spacing_mm\":%lld}\n",
              out.c_str(), field.width, field.height, static_cast<long long>(spacing));
  return k_exit_ok;
}

int tiles_command(int argc, char** argv) {
  if (argc < 3) return usage(k_tiles_usage, "");
  const std::string source = argv[2];
  std::string out_dir;
  std::string ddc;
  bool force = false;
  tileset::BuildOptions o;
  int i = 3;
  if (i < argc && argv[i][0] != '-') out_dir = argv[i++];
  for (; i < argc; ++i) {
    const std::string_view a = argv[i];
    const bool has_value = i + 1 < argc;
    if (a == "--ddc" && has_value) {
      ddc = argv[++i];
    } else if (a == "--cells" && has_value) {
      if (!parse_int(std::string_view(argv[++i]), o.cells) || o.cells < 1 || o.cells > 4096)
        return usage(k_tiles_usage, "--cells is 1 to 4096");
    } else if (a == "--ring-spacing-mm" && has_value) {
      if (!parse_int(std::string_view(argv[++i]), o.ring_spacing_mm) || o.ring_spacing_mm < 1)
        return usage(k_tiles_usage, "--ring-spacing-mm is a positive whole number");
    } else if (a == "--far-ratio" && has_value) {
      if (!parse_int(std::string_view(argv[++i]), o.far_ratio) || o.far_ratio < 1)
        return usage(k_tiles_usage, "--far-ratio is a positive whole number");
    } else if (a == "--far-levels" && has_value) {
      if (!parse_int(std::string_view(argv[++i]), o.far_levels) || o.far_levels > 16)
        return usage(k_tiles_usage, "--far-levels is 0 to 16");
    } else if (a == "--vertical-scale" && has_value) {
      if (!parse_f64(argv[++i], o.vertical_scale) || !(o.vertical_scale > 0.0) ||
          !(o.vertical_scale <= 1000.0))
        return usage(k_tiles_usage, "--vertical-scale is in (0, 1000]");
    } else if (a == "--offset-mm" && has_value) {
      i64 v[3] = {};
      if (!parse_ints(argv[++i], v, 3)) return usage(k_tiles_usage, "--offset-mm is x,y,z");
      o.offset_x_mm = v[0];
      o.offset_y_mm = v[1];
      o.offset_z_mm = v[2];
    } else if (a == "--force") {
      force = true;
    } else {
      return usage(k_tiles_usage, "unknown argument '" + std::string(a) + "'");
    }
  }
  if (out_dir.empty() && ddc.empty()) {
    std::error_code ec;
    ddc = geometry::find_ddc_root(std::filesystem::current_path(ec).generic_string());
    if (ddc.empty())
      return failed("terrain-tiles",
                    "no output directory, no --ddc, and no repository round the working directory");
  }
  const i64 start = time::monotonic_ns();
  tileset::BuildResult built;
  std::string error;
  if (!tileset::ensure_tileset(source, o, out_dir, ddc, force, built, error))
    return failed("terrain-tiles", error);
  tileset::TileSetReport report;
  report.manifest = built.manifest;
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(built.key));
  report.key = hex;
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(built.hash));
  report.hash = hex;
  report.hit = built.hit;
  report.levels = built.levels;
  report.tiles = built.tiles;
  report.bytes = built.bytes;
  report.ms = static_cast<f64>(time::monotonic_ns() - start) / 1e6;
  std::string text = write_json(schema::to_json(report), JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
  return k_exit_ok;
}

std::string resolved(const std::string& dir, const std::string& path) {
  return path.empty() || io::is_absolute_path(path) ? path : io::join_path(dir, path);
}

int masks_command(int argc, char** argv) {
  if (argc < 3) return usage(k_masks_usage, "");
  const std::string scene_path = argv[2];
  std::string out_dir;
  std::string ddc;
  std::string priors_dir;
  bool force = false;
  int i = 3;
  if (i < argc && argv[i][0] != '-') out_dir = argv[i++];
  for (; i < argc; ++i) {
    const std::string_view a = argv[i];
    const bool has_value = i + 1 < argc;
    if (a == "--ddc" && has_value) {
      ddc = argv[++i];
    } else if (a == "--write-priors" && has_value) {
      priors_dir = argv[++i];
    } else if (a == "--force") {
      force = true;
    } else {
      return usage(k_masks_usage, "unknown argument '" + std::string(a) + "'");
    }
  }
  std::string bytes;
  if (io::read_file(scene_path, bytes) != io::Status::Ok)
    return failed("terrain-masks", "'" + scene_path + "' could not be read");
  JsonValue json;
  if (!parse_json(bytes, json).ok)
    return failed("terrain-masks", "'" + scene_path + "' is not JSON");
  scene::Scene scene;
  schema::ReadContext ctx;
  if (!schema::from_json(scene, json, ctx) || !ctx.ok())
    return failed("terrain-masks", "'" + scene_path + "' is not a scene");
  if (!scene.terrain.has_value() || !scene.terrain->masks.has_value() ||
      !scene.terrain->tiles.has_value())
    return failed("terrain-masks",
                  "the scene's terrain names no masks over a tile set (\"tiles\" and \"masks\")");
  scene::Terrain entry = *scene.terrain;
  const std::string dir(io::parent_path(scene_path));
  std::string error;
  if (!scene_gen::resolve_mask_water({scene.water.data(), scene.water.size()}, *entry.masks,
                                     &error))
    return failed("terrain-masks", error);
  // The set, as the provider opens it: the manifest named, or the heightfield built on a miss.
  scene::TerrainTileSet& t = *entry.tiles;
  if (ddc.empty() && !t.ddc.empty()) ddc = resolved(dir, t.ddc);
  std::string manifest;
  if (!t.manifest.empty()) {
    manifest = resolved(dir, t.manifest);
    if (ddc.empty()) ddc = geometry::find_ddc_root(io::parent_path(manifest));
  } else {
    const std::string source = resolved(dir, t.heightfield);
    if (ddc.empty()) ddc = geometry::find_ddc_root(io::parent_path(source));
    tileset::BuildOptions o;
    o.cells = t.cells;
    o.ring_spacing_mm = t.ring_spacing_mm;
    o.far_ratio = t.far_ratio;
    o.far_levels = t.far_levels;
    o.vertical_scale = static_cast<f64>(t.vertical_scale);
    o.offset_x_mm = scene_gen::nearest_mm(t.origin.x);
    o.offset_y_mm = scene_gen::nearest_mm(t.origin.y);
    o.offset_z_mm = scene_gen::nearest_mm(t.origin.z);
    tileset::BuildResult set_built;
    if (!tileset::ensure_tileset(source, o, std::string(), ddc, false, set_built, error))
      return failed("terrain-masks", error);
    manifest = set_built.manifest;
  }
  if (out_dir.empty() && ddc.empty())
    return failed("terrain-masks",
                  "no output directory, no --ddc, and no repository round the scene");
  tileset::TileSet set;
  if (!set.open(manifest, error)) return failed("terrain-masks", error);
  tileset::MaskInputs inputs;
  if (!tileset::mask_inputs(entry, dir, inputs, error)) return failed("terrain-masks", error);
  const i64 start = time::monotonic_ns();
  tileset::MaskBuildResult built;
  if (!tileset::ensure_mask_level(set, inputs, out_dir, ddc, force, built, error))
    return failed("terrain-masks", error);
  if (!priors_dir.empty()) {
    Vector<std::string> written;
    if (!tileset::write_mask_priors(set, inputs, priors_dir, written, error))
      return failed("terrain-masks", error);
  }
  tileset::MaskLevelReport report;
  report.manifest = built.manifest;
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(built.key));
  report.key = hex;
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(built.hash));
  report.hash = hex;
  report.hit = built.hit;
  report.tiles = built.tiles;
  report.bytes = built.bytes;
  report.ms = static_cast<f64>(time::monotonic_ns() - start) / 1e6;
  std::string text = write_json(schema::to_json(report), JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
  return k_exit_ok;
}

// A permutation of [0, n): a scattered order that is the same on every run, as a load's placements
// are scattered over a region and the same on every launch.
Vector<u32> scattered(u32 n) {
  Vector<u32> order(n);
  for (u32 i = 0; i < n; ++i)
    order[i] = i;
  for (u32 i = n; i > 1; --i) {
    const u32 j = static_cast<u32>(mix64(hash_combine(0x7465727261696eull, i)) % i);
    std::swap(order[i - 1], order[j]);
  }
  return order;
}

int read_command(int argc, char** argv) {
  if (argc < 3) return usage(k_read_usage, "");
  const std::string target = argv[2];
  std::string pattern = "level";
  u32 level_index = 0;
  u32 window = 8;
  u64 cache_mib = 1024;
  bool masks = false;
  tileset::TileSet::ReadOptions read;
  for (int i = 3; i < argc; ++i) {
    const std::string_view a = argv[i];
    const bool has = i + 1 < argc;
    u64 n = 0;
    if (a == "--pattern" && has) {
      pattern = argv[++i];
    } else if (a == "--level" && has && parse_int(argv[i + 1], level_index)) {
      ++i;
    } else if (a == "--depth" && has && parse_int(argv[i + 1], read.depth) && read.depth >= 1) {
      ++i;
    } else if (a == "--run-kib" && has && parse_int(argv[i + 1], n)) {
      read.run_bytes = n * 1024;
      ++i;
    } else if (a == "--window" && has && parse_int(argv[i + 1], window) && window >= 1) {
      ++i;
    } else if (a == "--cache-mib" && has && parse_int(argv[i + 1], cache_mib) && cache_mib >= 1) {
      ++i;
    } else if (a == "--unbuffered") {
      read.unbuffered = true;
    } else if (a == "--masks") {
      masks = true;
    } else {
      return usage(k_read_usage, "unknown or incomplete option '" + std::string(a) + "'");
    }
  }
  if (pattern != "level" && pattern != "windows" && pattern != "points" && pattern != "files")
    return usage(k_read_usage, "--pattern is level, windows, points or files");

  const bench::MachineState machine_start = bench::sample_machine_state(bench::k_sample_window_ms);
  JsonValue report = JsonValue::object();
  report.set("pattern", masks ? std::string("masks") : pattern);
  report.set("depth", static_cast<u64>(read.depth));
  report.set("run_kib", read.run_bytes / 1024);
  report.set("unbuffered", read.unbuffered);
  u64 tiles = 0;
  u64 bytes = 0;
  i64 begin = 0;
  i64 end = 0;
  std::string error;
  if (pattern == "files") {
    // A level before its archive: every file of the directory, whole, one at a time.
    Vector<io::DirEntry> entries;
    if (io::list_directory(target, entries) != io::Status::Ok)
      return failed("terrain-read", "'" + target + "' is not a directory it can list");
    std::string data;
    begin = time::monotonic_ns();
    for (const io::DirEntry& e : entries) {
      if (e.is_directory) continue;
      if (io::read_file(io::join_path(target, e.name), data) != io::Status::Ok)
        return failed("terrain-read", "'" + e.name + "' could not be read");
      ++tiles;
      bytes += data.size();
    }
    end = time::monotonic_ns();
  } else if (masks) {
    tileset::MaskLevel level;
    if (!level.open(target, error, read.unbuffered)) return failed("terrain-read", error);
    const scene_gen::GroundMaskInfo& info = level.info();
    Vector<u32> words(scene_gen::mask_tile_words(info.cells));
    begin = time::monotonic_ns();
    // The renderer's two passes: one to count, one to copy (terrain_materials.cpp).
    for (u32 pass = 0; pass < 2; ++pass)
      for (i32 tz = info.tile_min_z; tz <= info.tile_max_z; ++tz)
        for (i32 tx = info.tile_min_x; tx <= info.tile_max_x; ++tx)
          if (level.tile(tx, tz, {words.data(), words.size()}) && pass == 0) ++tiles;
    end = time::monotonic_ns();
    const tileset::MaskLevel::ReadStats stats = level.read_stats();
    bytes = stats.bytes;
    report.set("windows", stats.windows);
  } else {
    tileset::TileSet set(cache_mib * 1024 * 1024, read);
    if (!set.open(target, error)) return failed("terrain-read", error);
    const tileset::TileSetManifest& m = set.manifest();
    if (level_index >= m.levels.size())
      return failed("terrain-read", "the set has " + std::to_string(m.levels.size()) + " levels");
    const tileset::TileSetLevel& level = m.levels[level_index];
    const i32 tx0 = level.tile_min[0], tz0 = level.tile_min[1];
    const u32 ntx = static_cast<u32>(level.tile_max[0] - tx0 + 1);
    const u32 ntz = static_cast<u32>(level.tile_max[1] - tz0 + 1);
    const i64 cells = m.cells;
    begin = time::monotonic_ns();
    if (pattern == "level") {
      tiles = set.prefetch_level(level_index);
    } else if (pattern == "windows") {
      const u32 wx = (ntx + window - 1) / window;
      const u32 wz = (ntz + window - 1) / window;
      for (const u32 w : scattered(wx * wz)) {
        const i32 ax = tx0 + static_cast<i32>((w % wx) * window);
        const i32 az = tz0 + static_cast<i32>((w / wx) * window);
        tiles += set.prefetch(level_index, ax, az, ax + static_cast<i32>(window) - 1,
                              az + static_cast<i32>(window) - 1);
      }
    } else {
      // A point at each tile's centre, the tiles in a scattered order: each a miss read alone.
      volatile f64 sink = 0;
      for (const u32 t : scattered(ntx * ntz)) {
        const i64 i = (tx0 + static_cast<i64>(t % ntx)) * cells + cells / 2;
        const i64 j = (tz0 + static_cast<i64>(t / ntx)) * cells + cells / 2;
        sink = sink + set.point_mm(i * level.spacing_mm, j * level.spacing_mm);
      }
      tiles = set.reads();
    }
    end = time::monotonic_ns();
    const tileset::TileSet::ReaderStats stats = set.reader_stats();
    bytes = stats.io_bytes;
    report.set("level_mm", level.spacing_mm);
    report.set("batches", stats.batches);
    report.set("runs", stats.runs);
    report.set("most_in_flight", stats.most_in_flight);
    report.set("failures", static_cast<u64>(set.failures()));
  }
  const bench::MachineState machine_end = bench::sample_machine_state(bench::k_sample_window_ms);
  const f64 seconds = static_cast<f64>(end - begin) / 1.0e9;
  report.set("tiles", tiles);
  report.set("bytes", bytes);
  report.set("seconds", seconds);
  report.set("mb_per_s", seconds > 0 ? static_cast<f64>(bytes) / 1.0e6 / seconds : 0.0);
  JsonValue machine = JsonValue::object();
  machine.set("start", bench::machine_state_json(machine_start));
  machine.set("end", bench::machine_state_json(machine_end));
  report.set("machine_state", std::move(machine));
  (void)bench::warn_if_busy(bench::worst_of(machine_start, machine_end), bench::QuietThresholds{},
                            stderr);
  std::string text = write_json(report, JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
  return k_exit_ok;
}

#endif

}  // namespace

int tileset_command(int argc, char** argv) {
  const std::string_view command = argc > 1 ? std::string_view(argv[1]) : std::string_view();
#if ENGINE_CONTENT_TILESET
  if (command == "terrain-synth") return synth_command(argc, argv);
  if (command == "terrain-masks") return masks_command(argc, argv);
  if (command == "terrain-read") return read_command(argc, argv);
  return tiles_command(argc, argv);
#else
  (void)argc;
  return failed(command, "this build has no tile-set capability (ENGINE_WITH_TILESET is off)");
#endif
}

}  // namespace engine::content

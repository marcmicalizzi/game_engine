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
#include "content_commands.h"

#include <core/base/types.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_TILESET
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <core/time/time.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/masks.h>
#include <domain/tileset/tileset.h>
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

#endif

}  // namespace

int tileset_command(int argc, char** argv) {
  const std::string_view command = argc > 1 ? std::string_view(argv[1]) : std::string_view();
#if ENGINE_CONTENT_TILESET
  if (command == "terrain-synth") return synth_command(argc, argv);
  if (command == "terrain-masks") return masks_command(argc, argv);
  return tiles_command(argc, argv);
#else
  (void)argc;
  return failed(command, "this build has no tile-set capability (ENGINE_WITH_TILESET is off)");
#endif
}

}  // namespace engine::content

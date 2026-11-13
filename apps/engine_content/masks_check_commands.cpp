// engine-content masks check: the masks' palette budget, checked before a cook (roadmap T72;
// docs/subsystems/apps.md, tileset.md "The mask level").
//
//   masks check <scene.json> [--ddc <root>]
//
// Runs the composition a load runs (`tileset::check_mask_budget`, the same field `terrain-masks`
// builds from) over the scene terrain's tile set and prints one JSON line per tile whose points
// name more layers than `materials.layer_budget` — the tile, its place in metres, the layers it
// names with their mean and peak weight (heaviest first), the largest value the format would store
// (`stored`, 255ths) and the points at or over one stored step (`points`, of `tile_points`; W113),
// and the weakest, the one a blend-out would drop — then a summary line. Exit 0 when no tile is
// over, 1 when any is (or the scene cannot be composed), 2 for usage. No mask archive is written.
// The tile set is opened as `terrain-masks` opens it, built into the derived-data cache on a miss,
// because the priors read its heights.
#include "content_commands.h"

#include <core/base/types.h>

#include <cstdio>
#include <string>
#include <string_view>

#if ENGINE_CONTENT_TILESET
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/masks.h>
#include <domain/tileset/tileset.h>
#include <foundation/io/vfs.h>

#include <schemas/scene.h>
#endif

namespace engine::content {

namespace {

[[maybe_unused]] constexpr int k_exit_ok = 0;
constexpr int k_exit_over = 1;
constexpr int k_exit_usage = 2;

const char* k_usage =
    "usage: engine-content masks check <scene.json> [--ddc <root>]\n"
    "  composes the scene terrain's masks as a load does and prints one JSON line per tile whose\n"
    "  points name more layers than materials.layer_budget (its place, its layers with mean and\n"
    "  peak weight, the largest stored value and the points at or over one step, the weakest),\n"
    "  then a summary line; writes no mask archive\n"
    "  exit 0: no tile over budget; 1: some tile over (or the scene cannot be composed); 2: "
    "usage\n";

int failed(const std::string& message) {
  std::fprintf(stderr, "engine-content masks check: %s\n", message.c_str());
  return k_exit_over;
}

#if ENGINE_CONTENT_TILESET

std::string quoted(const std::string& s) {
  std::string out = "\"";
  for (const char ch : s) {
    if (ch == '"' || ch == '\\') out.push_back('\\');
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

std::string number(f64 v) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.4f", v);
  return text;
}

std::string metres(i64 mm) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.3f", static_cast<f64>(mm) / 1000.0);
  return text;
}

std::string tile_line(const tileset::OverBudgetTile& t) {
  std::string s = "{\"tile\":[" + std::to_string(t.tx) + "," + std::to_string(t.tz) +
                  "],\"x_m\":[" + metres(t.x0_mm) + "," + metres(t.x1_mm) + "],\"z_m\":[" +
                  metres(t.z0_mm) + "," + metres(t.z1_mm) +
                  "],\"count\":" + std::to_string(t.layers.size()) + ",\"layers\":[";
  for (u32 i = 0; i < t.layers.size(); ++i) {
    const tileset::MaskTileLayer& l = t.layers[i];
    if (i > 0) s += ",";
    s += "{\"name\":" + quoted(l.name) + ",\"mean\":" + number(l.mean) +
         ",\"peak\":" + number(l.peak) + ",\"stored\":" + std::to_string(l.stored) +
         ",\"points\":" + std::to_string(l.points) + "}";
  }
  s += "],\"weakest\":" + quoted(t.layers.empty() ? std::string() : t.layers[t.weakest].name) +
       ",\"tile_points\":" + std::to_string(t.points) + "}";
  return s;
}

int check_scene(const std::string& scene_path, std::string ddc) {
  std::string bytes;
  if (io::read_file(scene_path, bytes) != io::Status::Ok)
    return failed("'" + scene_path + "' could not be read");
  JsonValue json;
  if (!parse_json(bytes, json).ok) return failed("'" + scene_path + "' is not JSON");
  scene::Scene scene;
  schema::ReadContext ctx;
  if (!schema::from_json(scene, json, ctx) || !ctx.ok())
    return failed("'" + scene_path + "' is not a scene");
  if (!scene.terrain.has_value() || !scene.terrain->masks.has_value() ||
      !scene.terrain->tiles.has_value())
    return failed("the scene's terrain names no masks over a tile set (\"tiles\" and \"masks\")");
  scene::Terrain entry = *scene.terrain;
  const std::string dir(io::parent_path(scene_path));
  std::string error;
  if (!scene_gen::resolve_mask_water({scene.water.data(), scene.water.size()}, *entry.masks,
                                     &error))
    return failed(error);
  // The set a load composes, from the same function (T74): the heightfield's cuts included, so the
  // check and the load cannot disagree about the ground the masks read.
  std::string manifest, ddc_used;
  if (!tileset::resolve_tile_set(entry, dir, ddc, ddc_used, manifest, error)) return failed(error);
  tileset::TileSet set;
  if (!set.open(manifest, error)) return failed(error);
  tileset::MaskInputs inputs;
  if (!tileset::mask_inputs(entry, dir, inputs, error)) return failed(error);
  if (!tileset::validate_mask_inputs(inputs, error)) return failed(error);
  tileset::MaskBudgetReport report;
  if (!tileset::check_mask_budget(set, inputs, report, error)) return failed(error);
  std::string out;
  for (const tileset::OverBudgetTile& tile : report.over)
    out += tile_line(tile) + "\n";
  // A layer under one quantization step is dropped from its tile, not counted (masks.h,
  // `k_mask_step`): one line each, the tile, the layer and its peak weight, as a load warns.
  for (const tileset::DroppedLayer& d : report.dropped) {
    out += "{\"dropped\":true,\"tile\":[" + std::to_string(d.tx) + "," + std::to_string(d.tz) +
           "],\"layer\":" + quoted(d.name) + ",\"peak\":" + number(d.peak) + "}\n";
  }
  out += "{\"summary\":true,\"tiles\":" + std::to_string(report.tiles) +
         ",\"over\":" + std::to_string(report.over.size()) +
         ",\"dropped\":" + std::to_string(report.dropped.size()) +
         ",\"largest\":" + std::to_string(report.largest) +
         ",\"budget\":" + std::to_string(report.budget) + "}\n";
  std::fwrite(out.data(), 1, out.size(), stdout);
  return report.over.empty() ? k_exit_ok : k_exit_over;
}

#endif

}  // namespace

int masks_check_command(int argc, char** argv) {
  if (argc < 4 || std::string_view(argv[2]) != "check") {
    std::fputs(k_usage, stderr);
    return k_exit_usage;
  }
  const std::string scene_path = argv[3];
  std::string ddc;
  for (int i = 4; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--ddc" && i + 1 < argc) {
      ddc = argv[++i];
    } else {
      std::fprintf(stderr, "engine-content: unknown argument '%s'\n", argv[i]);
      std::fputs(k_usage, stderr);
      return k_exit_usage;
    }
  }
#if ENGINE_CONTENT_TILESET
  return check_scene(scene_path, ddc);
#else
  (void)scene_path;
  (void)ddc;
  return failed("this build has no tile-set capability (ENGINE_WITH_TILESET is off)");
#endif
}

}  // namespace engine::content

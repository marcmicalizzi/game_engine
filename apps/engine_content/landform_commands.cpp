// `engine-content landform` (docs/subsystems/apps.md, "engine-content";
// docs/subsystems/landform.md, "The command"): a region sketch solved into an authored
// heightfield the tile set builds as it is, the fields the later solvers read, and a shaded relief
// for a person to look at. It calls the one function a test calls (`landform::solve`), reads the
// heightfield back through the tile set's own reader when this build has the tile set (the proof
// that the two agree on the format), and prints one JSON line.
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_LANDFORM
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/time/time.h>
#include <domain/landform/landform.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#if ENGINE_CONTENT_TILESET
#include <domain/tileset/tileset.h>
#endif

#include <charconv>
#include <span>
#include <string>
#include <string_view>
#endif

namespace engine::content {

int landform_command(int argc, char** argv) {
#if !ENGINE_CONTENT_LANDFORM
  (void)argc;
  (void)argv;
  std::fprintf(stderr, "landform requires the landform capability (ENGINE_WITH_LANDFORM)\n");
  return 1;
#else
  const char* usage =
      "usage: engine-content landform <sketch.json> <out dir> [--name <stem>] [--jobs <n>]\n"
      "                                [--no-relief]\n"
      "  writes <stem>.json and <stem>.f32 (engine.terrain.heightfield/1, whole millimetres),\n"
      "  <stem>.fields.json and a grid a field (engine.landform.fields/1), <stem>.relief.png\n"
      "  and <stem>.drainage.png; the stem is the sketch's file name without .json\n";
  if (argc < 4) {
    std::fputs(usage, stderr);
    return 2;
  }
  const std::string sketch_path = argv[2], out_dir = argv[3];
  std::string name;
  u64 workers = 0;
  bool relief = true;
  for (int i = 4; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag == "--no-relief") {
      relief = false;
      continue;
    }
    if (i + 1 >= argc) {
      std::fputs(usage, stderr);
      return 2;
    }
    const std::string_view value = argv[++i];
    bool ok = true;
    if (flag == "--name") {
      name = std::string(value);
      ok = !name.empty();
    } else if (flag == "--jobs") {
      const auto r = std::from_chars(value.data(), value.data() + value.size(), workers);
      ok = r.ec == std::errc{} && r.ptr == value.data() + value.size() && workers >= 1 &&
           workers <= 256;
    } else {
      ok = false;
    }
    if (!ok) {
      std::fprintf(stderr, "landform: invalid %.*s\n%s", static_cast<int>(flag.size()), flag.data(),
                   usage);
      return 2;
    }
  }
  const auto fail = [](const std::string& e) {
    std::fprintf(stderr, "landform: %s\n", e.c_str());
    return 1;
  };
  if (name.empty()) {
    name = std::string(io::file_name(sketch_path));
    const usize dot = name.rfind('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
  }

  std::string error, start_path;
  landform::Sketch sketch;
  if (!landform::read_sketch(sketch_path, sketch, start_path, error)) return fail(error);
  landform::StartField start;
  if (!start_path.empty() && !landform::read_start_field(start_path, start, error))
    return fail(error);

  jobs::JobSystemConfig config;
  if (workers > 0) config.performance_workers = static_cast<u32>(workers - 1);
  config.efficiency_workers = 1;
  jobs::JobSystem pool(config);
  landform::SolveOptions options;
  options.jobs = &pool;
  landform::Landform land;
  const i64 began = time::monotonic_ns();
  if (!landform::solve(sketch, start_path.empty() ? nullptr : &start, options, land, error))
    return fail(error);
  const f64 seconds = static_cast<f64>(time::monotonic_ns() - began) * 1.0e-9;

  landform::WrittenFiles files;
  if (!landform::write_landform(land, sketch, out_dir, name, files, error)) return fail(error);
#if ENGINE_CONTENT_TILESET
  // The tile set's own reader takes what was written: the header's shape and the grid's hash.
  {
    tileset::Heightfield check;
    u64 source = 0;
    if (!tileset::read_heightfield(files.heightfield, check, source, error))
      return fail("the tile set refuses the heightfield written: " + error);
  }
#endif
  std::string relief_path, drainage_path;
  if (relief) {
    Vector<u8> rgba;
    relief_path = io::join_path(out_dir, name + ".relief.png");
    drainage_path = io::join_path(out_dir, name + ".drainage.png");
    landform::shaded_relief(land, false, rgba);
    if (image::write_png(relief_path, land.width, land.height, 4,
                         std::span<const u8>(rgba.data(), rgba.size())) != io::Status::Ok)
      return fail("'" + relief_path + "' could not be written");
    landform::shaded_relief(land, true, rgba);
    if (image::write_png(drainage_path, land.width, land.height, 4,
                         std::span<const u8>(rgba.data(), rgba.size())) != io::Status::Ok)
      return fail("'" + drainage_path + "' could not be written");
  }

  // The summary: what a person checks before building tiles from it.
  const u32 n = land.width * land.height;
  i32 lowest = land.height_mm[0], highest = land.height_mm[0];
  u32 held = 0, flooded = 0, under_sea = 0, largest = 0;
  f64 slope_sum = 0.0;
  u32 slope_count = 0;
  for (u32 p = 0; p < n; ++p) {
    const i32 mm = land.height_mm[p];
    if (mm < lowest) lowest = mm;
    if (mm > highest) highest = mm;
    if (land.receiver[p] == landform::k_no_receiver) ++held;
    if (land.fill[p] > 0) ++flooded;
    if (land.drainage_area[p] > largest) largest = land.drainage_area[p];
    if (land.has_sea && mm <= land.sea_level_mm) {
      ++under_sea;
    } else if (land.slope[p] > 0) {
      slope_sum += static_cast<f64>(land.slope[p]) * 1.0e-6;
      ++slope_count;
    }
  }
  JsonValue summary = JsonValue::object();
  summary.set("sketch", JsonValue(sketch_path));
  summary.set("heightfield", JsonValue(files.heightfield));
  summary.set("fields", JsonValue(files.fields));
  if (relief) {
    summary.set("relief", JsonValue(relief_path));
    summary.set("drainage", JsonValue(drainage_path));
  }
  summary.set("width", JsonValue(land.width));
  summary.set("height", JsonValue(land.height));
  summary.set("spacing_mm", JsonValue(land.spacing_mm));
  summary.set("steps", JsonValue(land.steps));
  summary.set("river_sweeps", JsonValue(land.river_sweeps));
  summary.set("step_yr", JsonValue(land.step_yr));
  summary.set("lowest_m", JsonValue(static_cast<f64>(lowest) / 1000.0));
  summary.set("highest_m", JsonValue(static_cast<f64>(highest) / 1000.0));
  summary.set("mean_slope",
              JsonValue(slope_count > 0 ? slope_sum / static_cast<f64>(slope_count) : 0.0));
  summary.set("held_points", JsonValue(held));
  summary.set("flooded_points", JsonValue(flooded));
  summary.set("sea_points", JsonValue(under_sea));
  summary.set("largest_area_cells", JsonValue(largest));
  // Where the material went (landform.md, "Deposition"): eroded = deposited + left.
  u32 covered = 0, thickest = 0;
  for (u32 p = 0; p < n; ++p) {
    if (land.sediment[p] >= 1000) ++covered;
    if (land.sediment[p] > thickest) thickest = land.sediment[p];
  }
  summary.set("sediment_points", JsonValue(covered));
  summary.set("thickest_sediment_m", JsonValue(static_cast<f64>(thickest) / 1000.0));
  JsonValue budget = JsonValue::object();
  budget.set("uplifted_m3", JsonValue(land.budget.uplifted_m3));
  budget.set("eroded_m3", JsonValue(land.budget.eroded_m3));
  budget.set("deposited_m3", JsonValue(land.budget.deposited_m3));
  budget.set("left_m3", JsonValue(land.budget.left_m3));
  budget.set("crept_out_m3", JsonValue(land.budget.crept_out_m3));
  budget.set("settled_m3", JsonValue(land.budget.settled_m3));
  budget.set("change_m3", JsonValue(land.budget.change_m3));
  summary.set("budget", std::move(budget));
  summary.set("solve_seconds", JsonValue(seconds));
  summary.set("workers", JsonValue(pool.worker_count(jobs::Pool::Performance) + 1));
  std::printf("%s\n", write_json(summary).c_str());
  return 0;
#endif
}

}  // namespace engine::content

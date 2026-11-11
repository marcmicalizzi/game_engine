// `engine-content cave-plan` (docs/subsystems/apps.md, "engine-content"; docs/subsystems/caves.md,
// "The command"): a cave network's plan against a hillside given by a few numbers — a plan view
// and a long section a person lays beside a published survey, the holes its mouths ask of the
// terrain, and, with `--cells`, every cell solved (and with `--build` built into its container)
// with its triangles and milliseconds. It calls the functions the tests and the placement
// generator call, and prints one JSON line.
#include "content_commands.h"

#include <cstdio>
#if ENGINE_CONTENT_CAVES
#include <core/json/json.h>
#include <core/time/time.h>
#include <domain/caves/network.h>
#include <foundation/io/vfs.h>

#include <cstdlib>
#include <string>
#include <string_view>
#endif

namespace engine::content {

#if ENGINE_CONTENT_CAVES
namespace {

// y = max(valley, slope_x * x - max(0, z)^2 / flank), less a doline: a hill rising toward +x, its
// +z side cut down by a side valley (flank 0: not cut).
struct Hill {
  f64 valley = 0.0, slope = 0.0, flank = 0.0;
  f64 doline_x = 0.0, doline_z = 0.0, radius = 0.0, depth = 0.0;
  bool present = false;
};
f64 hill_height(const void* c, i64 x_mm, i64 z_mm) noexcept {
  const Hill& h = *static_cast<const Hill*>(c);
  const f64 x = static_cast<f64>(x_mm) * 0.001, z = static_cast<f64>(z_mm) * 0.001;
  const f64 side = z > 0.0 ? z : 0.0;
  f64 y = h.slope * x - (h.flank > 0.0 ? side * side / h.flank : 0.0);
  if (y < h.valley) y = h.valley;
  if (h.radius > 0.0) {
    const f64 dx = (x - h.doline_x) / h.radius, dz = (z - h.doline_z) / h.radius;
    const f64 r2 = dx * dx + dz * dz;
    if (r2 < 1.0) y -= h.depth * (1.0 - r2) * (1.0 - r2);
  }
  return y;
}

// "a,b,c": up to `most` numbers; false for anything else.
bool numbers(std::string_view text, f64* out, u32 most, u32& count) {
  count = 0;
  while (!text.empty()) {
    if (count >= most) return false;
    const usize comma = text.find(',');
    const std::string one(text.substr(0, comma));
    char* end = nullptr;
    out[count] = std::strtod(one.c_str(), &end);
    if (one.empty() || end != one.c_str() + one.size()) return false;
    ++count;
    if (comma == std::string_view::npos) break;
    text.remove_prefix(comma + 1);
  }
  return count > 0;
}

}  // namespace
#endif

int caves_command(int argc, char** argv) {
#if !ENGINE_CONTENT_CAVES
  (void)argc;
  (void)argv;
  std::fprintf(stderr, "cave-plan requires the caves capability (ENGINE_WITH_CAVES)\n");
  return 1;
#else
  const char* usage =
      "usage: engine-content cave-plan <network.json> <out dir> [--name <stem>]\n"
      "                                 [--ground <valley>,<slope_x>[,<flank_m>]]\n"
      "                                 [--doline <x>,<z>,<radius>,<depth>] [--tile <m>]\n"
      "                                 [--cells] [--build]\n"
      "  writes <stem>.plan.svg, <stem>.section.svg, <stem>.holes.json (the holes its mouths\n"
      "  ask of the terrain) and <stem>.survey.json (where things are); the ground is\n"
      "  y = max(valley, slope_x * x - max(0, z)^2 / flank), level at 0 with no --ground\n";
  if (argc < 4) {
    std::fputs(usage, stderr);
    return 2;
  }
  const std::string network_path = argv[2], out_dir = argv[3];
  std::string name;
  Hill hill;
  f64 tile_m = 32.0;
  bool cells = false, build = false;
  for (int i = 4; i < argc; ++i) {
    const std::string_view flag = argv[i];
    if (flag == "--cells") {
      cells = true;
      continue;
    }
    if (flag == "--build") {
      cells = build = true;
      continue;
    }
    if (i + 1 >= argc) {
      std::fputs(usage, stderr);
      return 2;
    }
    const std::string_view value = argv[++i];
    bool ok = true;
    f64 v[4] = {};
    u32 n = 0;
    if (flag == "--name") {
      name = std::string(value);
      ok = !name.empty();
    } else if (flag == "--ground") {
      ok = numbers(value, v, 3, n) && n >= 2;
      hill.valley = v[0];
      hill.slope = v[1];
      hill.flank = n > 2 ? v[2] : 0.0;
      hill.present = true;
    } else if (flag == "--doline") {
      ok = numbers(value, v, 4, n) && n == 4 && v[2] > 0.0;
      hill.doline_x = v[0];
      hill.doline_z = v[1];
      hill.radius = v[2];
      hill.depth = v[3];
      hill.present = true;
    } else if (flag == "--tile") {
      ok = numbers(value, v, 1, n) && v[0] >= 8.0 && v[0] <= 4096.0;
      tile_m = v[0];
    } else {
      ok = false;
    }
    if (!ok) {
      std::fprintf(stderr, "cave-plan: invalid %.*s\n%s", static_cast<int>(flag.size()),
                   flag.data(), usage);
      return 2;
    }
  }
  const auto fail = [](const std::string& e) {
    std::fprintf(stderr, "cave-plan: %s\n", e.c_str());
    return 1;
  };
  if (name.empty()) {
    name = std::string(io::file_name(network_path));
    const usize dot = name.find('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
  }
  std::string text, error;
  if (io::read_file(network_path, text) != io::Status::Ok)
    return fail("'" + network_path + "' could not be read");
  caves::Network network;
  if (!caves::read_network(text, network, error)) return fail(error);
  scene_gen::Ground ground;
  if (hill.present) {
    ground.surface_fn = hill_height;
    ground.context = &hill;
  }
  caves::Plan plan;
  const i64 began = time::monotonic_ns();
  if (!caves::plan_network(network, ground, scene_gen::nearest_mm(tile_m), plan, error))
    return fail(error);
  const f64 plan_ms = static_cast<f64>(time::monotonic_ns() - began) * 1.0e-6;
  if (io::make_directories(out_dir) != io::Status::Ok) return fail("cannot create " + out_dir);
  const std::string plan_path = io::join_path(out_dir, name + ".plan.svg");
  const std::string section_path = io::join_path(out_dir, name + ".section.svg");
  const std::string holes_path = io::join_path(out_dir, name + ".holes.json");
  const std::string survey_path = io::join_path(out_dir, name + ".survey.json");
  if (io::write_file_atomic(plan_path, caves::plan_svg(plan)) != io::Status::Ok ||
      io::write_file_atomic(section_path, caves::section_svg(plan, ground)) != io::Status::Ok ||
      io::write_file_atomic(holes_path, caves::holes_json(plan)) != io::Status::Ok ||
      io::write_file_atomic(survey_path, caves::survey_json(plan)) != io::Status::Ok)
    return fail("the plan's files could not be written under " + out_dir);

  const caves::PlanReport& r = plan.report;
  JsonValue summary = JsonValue::object();
  summary.set("network", JsonValue(network_path));
  summary.set("plan", JsonValue(plan_path));
  summary.set("section", JsonValue(section_path));
  summary.set("holes", JsonValue(holes_path));
  summary.set("survey", JsonValue(survey_path));
  summary.set("plan_hash", JsonValue(std::to_string(caves::plan_hash(plan))));
  summary.set("plan_ms", JsonValue(plan_ms));
  summary.set("levels", JsonValue(r.levels));
  summary.set("strands", JsonValue(r.strands));
  summary.set("edges", JsonValue(plan.edges.size()));
  summary.set("nodes", JsonValue(plan.nodes.size()));
  summary.set("stations", JsonValue(plan.stations.size()));
  summary.set("passage_m", JsonValue(static_cast<f64>(r.passage_m)));
  summary.set("walkable_m", JsonValue(static_cast<f64>(r.walkable_m)));
  summary.set("reachable_m", JsonValue(static_cast<f64>(r.reachable_m)));
  summary.set("loops", JsonValue(r.loops));
  summary.set("dead_ends", JsonValue(r.dead_ends));
  summary.set("mouths", JsonValue(plan.mouths.size()));
  summary.set("mouths_found", JsonValue(r.mouths_found));
  summary.set("mouths_led", JsonValue(r.mouths_led));
  summary.set("fissures", JsonValue(r.fissures));
  summary.set("halls", JsonValue(plan.halls.size()));
  summary.set("cells", JsonValue(plan.cells.size()));
  summary.set("portals", JsonValue(plan.portals.size()));
  summary.set("base_level_m", JsonValue(r.base_level_m));
  summary.set("least_margin_m", JsonValue(r.least_margin_m));
  summary.set("dip_offset_m", JsonValue(r.dip_offset_m));
  summary.set("notes", JsonValue(r.notes));
  u64 plan_bytes = plan.stations.size() * sizeof(caves::PlanStation) +
                   plan.edges.size() * sizeof(caves::PlanEdge) +
                   plan.cells.size() * sizeof(caves::PlanCell);
  for (const caves::PlanMouth& m : plan.mouths)
    plan_bytes += m.patch.size() * sizeof(i32);
  summary.set("plan_bytes", JsonValue(static_cast<f64>(plan_bytes)));

  if (cells) {
    u64 triangles = 0, class_triangles[5] = {}, dress_triangles = 0, drips = 0, lattice = 0;
    u64 clusters = 0, roots = 0, recipe_bytes = 0;
    f64 solve_ms = 0.0, worst_ms = 0.0, build_ms = 0.0;
    for (u32 cell = 0; cell < plan.cells.size(); ++cell) {
      if (plan.cells[cell].flooded) continue;
      for (u32 part = 0; part < (network.speleothems.has_value() ? 2u : 1u); ++part) {
        caves::CellRecipe recipe;
        caves::cell_recipe(plan, cell, part, recipe);
        recipe_bytes += caves::cell_recipe_json(recipe).size();
        assets::MeshData mesh;
        caves::CellReport report;
        const i64 t0 = time::monotonic_ns();
        if (!caves::solve_cell(recipe, mesh, &report, error)) return fail(error);
        const f64 ms = static_cast<f64>(time::monotonic_ns() - t0) * 1.0e-6;
        solve_ms += ms;
        if (ms > worst_ms) worst_ms = ms;
        if (part == 0) {
          triangles += report.triangles;
          lattice += report.lattice_points;
          for (u32 k = 0; k < 5; ++k)
            class_triangles[k] += report.class_triangles[k];
        } else {
          dress_triangles += report.triangles;
          drips += report.drips;
        }
        if (build && report.triangles > 0) {
          geometry::ClusterFileData data;
          const i64 t1 = time::monotonic_ns();
          if (!caves::build_cell_container(recipe, assets::MeshGeneratorInputs{},
                                           geometry::ClusterLodOptions{}, 0, data, nullptr, error))
            return fail(error);
          build_ms += static_cast<f64>(time::monotonic_ns() - t1) * 1.0e-6;
          clusters += data.mesh.mesh.clusters.size();
          for (const geometry::ClusterLodDesc& d : data.mesh.lod)
            if (d.parent_error >= geometry::k_lod_terminal_error) ++roots;
        }
      }
    }
    const f64 metres = static_cast<f64>(r.passage_m) > 0.0 ? static_cast<f64>(r.passage_m) : 1.0;
    summary.set("triangles", JsonValue(static_cast<f64>(triangles)));
    summary.set("triangles_per_m", JsonValue(static_cast<f64>(triangles) / metres));
    summary.set("rock_triangles", JsonValue(static_cast<f64>(class_triangles[0])));
    summary.set("sediment_triangles", JsonValue(static_cast<f64>(class_triangles[1])));
    summary.set("breakdown_triangles", JsonValue(static_cast<f64>(class_triangles[2])));
    summary.set("water_triangles", JsonValue(static_cast<f64>(class_triangles[3])));
    summary.set("apron_triangles", JsonValue(static_cast<f64>(class_triangles[4])));
    summary.set("dressing_triangles", JsonValue(static_cast<f64>(dress_triangles)));
    summary.set("drips", JsonValue(static_cast<f64>(drips)));
    summary.set("lattice_points", JsonValue(static_cast<f64>(lattice)));
    summary.set("recipe_bytes", JsonValue(static_cast<f64>(recipe_bytes)));
    summary.set("solve_ms", JsonValue(solve_ms));
    summary.set("worst_cell_ms", JsonValue(worst_ms));
    if (build) {
      summary.set("build_ms", JsonValue(build_ms));
      summary.set("clusters", JsonValue(static_cast<f64>(clusters)));
      summary.set("root_clusters", JsonValue(static_cast<f64>(roots)));
    }
  }
  std::printf("%s\n", write_json(summary).c_str());
  return 0;
#endif
}

}  // namespace engine::content

// The walls capability's costs (docs/subsystems/walls.md, "Budgets"): solving one module (what a
// derived-data miss costs, once per kit entry), and standing a tile's modules on the ground (what
// a tile's activation costs).
#include <domain/walls/walls.h>
#include <foundation/bench/bench.h>

using namespace engine;

namespace {
f64 slope(const void*, i64 x_mm, i64 z_mm) noexcept {
  return 0.15 * static_cast<f64>(x_mm) * 0.001 + 0.05 * static_cast<f64>(z_mm) * 0.001;
}
}  // namespace

// arg: the module's grade, thousandths.
ENGINE_BENCH_ARGS(walls_solve_module, "walls.solve_module", 0, 250, 500) {
  walls::Module m;
  m.grade_permille = static_cast<i32>(state.arg());
  assets::MeshData mesh;
  walls::ModuleReport report;
  std::string error;
  while (state.keep_running()) {
    walls::solve_module(m, mesh, report, error);
    bench::keep(mesh.positions.size());
  }
  state.set_items(1);
}

// arg: metres of wall in one tile; the modules standing on it, each asked of the ground.
ENGINE_BENCH_ARGS(walls_stand_tile, "walls.stand_tile", 32, 100, 400) {
  walls::Layer layer;
  walls::WallLine line;
  line.points_mm = {0, 1000, static_cast<i64>(state.arg()) * 1000, 1000};
  layer.lines.push_back(line);
  walls::Plan plan;
  std::string why;
  walls::make_plan(layer, 1000000, plan, why);
  scene_gen::Ground ground;
  ground.surface_fn = slope;
  while (state.keep_running()) {
    f64 sum = 0;
    for (const walls::Slot& s : plan.slots)
      sum += walls::stand_slot(plan, layer, s, ground).placement.position.y;
    bench::keep(sum);
  }
  state.set_items(plan.slots.size());
}

// The caves capability's costs (docs/subsystems/caves.md, "Budgets"): planning a network (once a
// scene read, in `open`), and solving one cell's rock (what a derived-data miss costs, once a
// cell) at each lattice spacing.
#include <domain/caves/network.h>
#include <foundation/bench/bench.h>

using namespace engine;

namespace {

// A hill rising toward +x over a valley floor, its +z side cut down by a side valley.
f64 hill(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) * 0.001, z = static_cast<f64>(z_mm) * 0.001;
  const f64 side = z > 0.0 ? z : 0.0;
  const f64 y = 0.36 * x - side * side / 400.0;
  return y < 0.0 ? 0.0 : y;
}

caves::Network network(u32 voxel_mm) {
  caves::Network n;
  n.format = std::string(caves::k_network_format);
  n.seed = 11;
  n.origin = WorldPos{30.0, 1.0, 0.0};
  n.member_thickness_m = 24.0f;
  n.levels = 1;
  n.voxel_m = static_cast<f32>(voxel_mm) / 1000.0f;
  return n;
}

}  // namespace

// arg: the trunks' reach along strike, metres.
ENGINE_BENCH_ARGS(caves_plan, "caves.plan_network", 80, 160, 640) {
  caves::Network n = network(250);
  n.reach_strike_m = static_cast<f32>(state.arg());
  scene_gen::Ground ground;
  ground.surface_fn = hill;
  caves::Plan plan;
  std::string error;
  while (state.keep_running()) {
    caves::plan_network(n, ground, 32000, plan, error);
    bench::keep(plan.stations.size());
  }
  state.set_items(1);
}

// arg: the lattice's spacing, millimetres.
ENGINE_BENCH_ARGS(caves_solve_cell, "caves.solve_cell", 500, 250) {
  const caves::Network n = network(static_cast<u32>(state.arg()));
  scene_gen::Ground ground;
  ground.surface_fn = hill;
  caves::Plan plan;
  std::string error;
  caves::plan_network(n, ground, 32000, plan, error);
  caves::CellRecipe recipe;
  if (!plan.cells.empty()) caves::cell_recipe(plan, 0, 0, recipe);
  assets::MeshData mesh;
  while (state.keep_running()) {
    caves::solve_cell(recipe, mesh, nullptr, error);
    bench::keep(mesh.positions.size());
  }
  state.set_items(1);
}

// The bridges capability's costs (docs/subsystems/bridges.md, "Budgets"): planning one crossing and
// solving its mesh (what a tile's activation asks of the plan, and what a derived-data miss costs
// before the cluster build), and the whole container (the miss, once per bridge).
#include <domain/bridges/bridges.h>
#include <foundation/bench/bench.h>

using namespace engine;

namespace {

// arg: the form, 0 stepping stones (2 m brook), 1 a clapper (6 m, boulder bed), 2 a foot log
// (3 m), 3 log stringers (7 m).
bridges::Crossing crossing(i64 form) {
  const i32 widths[4] = {2000, 6000, 3000, 7000};
  const i32 depths[4] = {300, 800, 800, 900};
  const i32 width = widths[form], depth = depths[form];
  bridges::Crossing c;
  c.width_mm = static_cast<u32>(width);
  c.bed_mm = 0;
  c.water_mm = depth / 2;
  c.d84_mm = form == 1 ? 400u : 0u;
  const i32 n = (width / 2 + 6000) / 250 * 2 + 1;
  for (i32 i = 0; i < n; ++i) {
    const i64 x = static_cast<i64>(i - n / 2) * 250, ax = x < 0 ? -x : x;
    c.profile_mm.push_back(2 * ax < width ? static_cast<i64>(depth) * 2 * ax / width : depth);
  }
  return c;
}

}  // namespace

ENGINE_BENCH_ARGS(bridges_plan, "bridges.plan", 0, 1, 2, 3) {
  const bridges::Crossing c = crossing(state.arg());
  bridges::BridgePlan plan;
  std::string why;
  while (state.keep_running()) {
    bridges::plan_bridge(c, 7, plan, why);
    bench::keep(plan.members.size());
  }
  state.set_items(1);
}

ENGINE_BENCH_ARGS(bridges_solve, "bridges.solve", 0, 1, 2, 3) {
  bridges::Bridge b;
  b.crossing = crossing(state.arg());
  bridges::MeshPart parts[bridges::k_materials];
  bridges::BridgePlan plan;
  bridges::BridgeReport report;
  std::string error;
  while (state.keep_running()) {
    bridges::solve_bridge(b, parts, plan, report, error);
    bench::keep(report.triangles);
  }
  state.set_items(1);
}

ENGINE_BENCH_ARGS(bridges_container, "bridges.container", 0, 1, 2, 3) {
  bridges::Bridge b;
  b.crossing = crossing(state.arg());
  geometry::ClusterFileData data;
  bridges::BridgeReport report;
  std::string error;
  while (state.keep_running()) {
    bridges::build_container(b, {}, {}, 0, data, report, error);
    bench::keep(data.mesh.mesh.clusters.size());
  }
  state.set_items(1);
}

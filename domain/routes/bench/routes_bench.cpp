// Micro-benchmarks for the routes capability (docs/plan/11-performance-principles.md §11.8;
// routes.md, "Budgets"): the planner over a square region of a hillside with one route across it
// (the argument is the region's side in lattice points at 2 m), and the wear query a placement
// makes per candidate point.
#include <domain/routes/routes.h>
#include <foundation/bench/bench.h>

using namespace engine;

namespace {
// A hillside: 12 % across x and a rounded hill, products and one division.
f64 hillside(const void*, i64 x, i64 z) noexcept {
  const f64 xx = static_cast<f64>(x) / 1000.0, zz = static_cast<f64>(z) / 1000.0;
  const f64 r2 = (xx - 60) * (xx - 60) + (zz - 40) * (zz - 40);
  return .12 * xx + 25.0 / (1.0 + r2 / 900.0);
}
routes::RouteLayer layer(u32 side) {
  routes::RouteLayer p;
  p.name = "bench";
  const f64 extent = 2.0 * static_cast<f64>(side - 1);
  p.region_min = {0.0, 0.0};
  p.region_max = {extent, extent};
  routes::Route r;
  r.name = "across";
  r.waypoints.push_back(WorldPos{1.0, 0.0, 1.0});
  r.waypoints.push_back(WorldPos{extent - 1.0, 0.0, extent - 1.0});
  p.routes.push_back(r);
  return p;
}
}  // namespace

ENGINE_BENCH_ARGS(routes_plan, "routes.plan", 64, 128, 256) {
  const u32 side = static_cast<u32>(state.arg());
  const routes::RouteLayer p = layer(side);
  scene_gen::Ground g;
  g.surface_fn = &hillside;
  std::string error;
  while (state.keep_running()) {
    routes::RouteNetwork n;
    const bool ok = routes::plan_routes(p, g, nullptr, n, error);
    bench::keep(ok);
    bench::keep(n.length_m);
  }
  state.set_items(side * side);
}

ENGINE_BENCH_ARGS(routes_wear, "routes.wear", 1024, 16384) {
  const u32 queries = static_cast<u32>(state.arg());
  const routes::RouteLayer p = layer(128);
  scene_gen::Ground g;
  g.surface_fn = &hillside;
  std::string error;
  routes::RouteNetwork n;
  routes::plan_routes(p, g, nullptr, n, error);
  routes::RouteWear wear;
  wear.build(p, n);
  while (state.keep_running()) {
    f64 sum = 0;
    for (u32 i = 0; i < queries; ++i)
      sum +=
          wear.cover_mm(static_cast<i64>(i % 254) * 1000, static_cast<i64>(i / 254 * 997) % 254000);
    bench::keep(sum);
  }
  state.set_items(queries);
}

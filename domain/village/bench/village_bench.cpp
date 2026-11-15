// Micro-benchmarks for the village capability (docs/subsystems/village.md, "Budgets"): the plan of
// a street of a stated number of houses, which an entry pays at each open.
#include <domain/village/village.h>
#include <foundation/bench/bench.h>

using namespace engine;

namespace {
f64 gentle(const void*, i64 x, i64 z) noexcept {
  return 0.04 * static_cast<f64>(z) / 1000.0 + 0.01 * static_cast<f64>(x) / 1000.0;
}
}  // namespace

ENGINE_BENCH_ARGS(village_plan, "village.plan", 8, 64, 256) {
  const u32 houses = static_cast<u32>(state.arg());
  village::VillageLayer layer;
  layer.seed = 5;
  layer.houses = houses;
  village::Lane lane;
  lane.waypoints.push_back(WorldPos{0.0, 0.0, 0.0});
  lane.waypoints.push_back(WorldPos{static_cast<f64>(houses) * 12.0, 0.0, 40.0});
  layer.lanes.push_back(lane);
  scene_gen::Context context;
  context.ground.surface_fn = &gentle;
  while (state.keep_running()) {
    village::VillagePlan plan;
    std::string error;
    const bool ok = village::plan_village(layer, context.ground, context, &village::box_house,
                                          nullptr, plan, error);
    bench::keep(ok);
    bench::keep(plan.built);
  }
  state.set_items(houses);
}

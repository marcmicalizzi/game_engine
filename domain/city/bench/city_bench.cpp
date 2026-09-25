// The city's costs (docs/subsystems/city.md, "Performance notes"; plan 11 §11.8): the whole-island
// plan, which is made once per parameter set and cached; one building, which is what materializing
// a lot costs at each LOD stage; the validators over one building, which is what the E18 loop pays
// per building on top; and a district's proxy fragment, which is what the owner's fly-through
// reads. The island is the default one (a 2 km radius) at the golden seed, in memory; the buildings
// are E18's lots, across the archetypes.
#include <core/jobs/job_system.h>
#include <domain/city/city.h>
#include <foundation/bench/bench.h>

#include <string>

using namespace engine;
using namespace engine::city;

namespace {

const Params& bench_params() {
  static const Params p = [] {
    IslandParams source = default_island_params();
    source.seed = 2026;
    Params out;
    std::string error;
    (void)params_from_schema(source, out, error);
    return out;
  }();
  return p;
}

const Plan& bench_plan() {
  static const Plan plan = [] {
    Plan out;
    std::string error;
    (void)generate_plan(bench_params(), out, error);
    return out;
  }();
  return plan;
}

const Vector<u32>& bench_lots() {
  static const Vector<u32> lots = [] {
    Vector<u32> out;
    choose_yield_lots(bench_plan(), 200, out);
    return out;
  }();
  return lots;
}

}  // namespace

// The whole plan, on one thread (0) and with the lots cut on a pool of 4 workers and the caller.
ENGINE_BENCH_ARGS(city_plan, "city.plan", 0, 4) {
  const u32 workers = static_cast<u32>(state.arg());
  jobs::JobSystem js(jobs::JobSystemConfig{
      .performance_workers = workers, .efficiency_workers = 1, .pin_threads = false});
  Plan plan;
  std::string error;
  u64 lots = 0;
  while (state.keep_running()) {
    (void)generate_plan(bench_params(), plan, error, workers > 0 ? &js : nullptr);
    lots += plan.lots.size();
    bench::keep(plan.lots.data());
  }
  bench::keep(lots);
  state.set_items(1);
}

// One building at a stage (0 massing, 1 floors, 2 rooms), cycling through E18's 200 lots, the
// building's arrays reused: the steady state of materializing lots one after another.
ENGINE_BENCH_ARGS(city_building, "city.building", 0, 1, 2) {
  const Stage stage = static_cast<Stage>(state.arg());
  const Plan& plan = bench_plan();
  const Vector<u32>& lots = bench_lots();
  Grammar grammar(plan);
  Building b;
  std::string error;
  u32 next = 0;
  u64 spaces = 0;
  while (state.keep_running()) {
    (void)grammar.generate(lots[next], stage, b, &error);
    next = next + 1 == lots.size() ? 0 : next + 1;
    spaces += b.spaces.size();
    bench::keep(b.spaces.data());
  }
  bench::keep(spaces);
  state.set_items(1);
}

// The validators over one building at the rooms stage, the buildings made beforehand.
ENGINE_BENCH(city_validate, "city.validate") {
  const Plan& plan = bench_plan();
  static const Vector<Building> buildings = [&] {
    Vector<Building> out;
    std::string error;
    const Vector<u32>& lots = bench_lots();
    (void)generate_buildings(plan, std::span<const u32>(lots.data(), lots.size()), Stage::Rooms,
                             nullptr, out, &error);
    return out;
  }();
  Report report;
  u32 next = 0;
  u64 findings = 0;
  while (state.keep_running()) {
    report.clear();
    validate_building(plan, buildings[next], report);
    next = next + 1 == buildings.size() ? 0 : next + 1;
    findings += report.findings.size();
  }
  bench::keep(findings);
  state.set_items(1);
}

// A district's proxies at the floors stage, one thread: the buildings, their proxies and the
// ground's. The district is the default island's largest.
ENGINE_BENCH(city_fragment, "city.fragment.district") {
  const Plan& plan = bench_plan();
  u32 district = 0;
  for (const District& d : plan.districts) {
    if (d.area_cm2 > plan.districts[district].area_cm2) district = d.id;
  }
  Vector<u32> lots;
  for (u32 l = 0; l < plan.lots.size(); ++l) {
    if (plan.lots[l].district == district && plan.lots[l].use != LotUse::Park) lots.push_back(l);
  }
  Grammar grammar(plan);
  Building b;
  Vector<Proxy> proxies;
  std::string error;
  while (state.keep_running()) {
    proxies.clear();
    for (const u32 l : lots) {
      (void)grammar.generate(l, Stage::Floors, b, &error);
      append_building_proxies(plan, b, Stage::Floors, proxies);
    }
    append_district_ground_proxies(plan, district, proxies);
    bench::keep(proxies.data());
  }
  state.set_items(lots.size());
}

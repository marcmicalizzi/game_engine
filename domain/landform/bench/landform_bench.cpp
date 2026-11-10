// Benchmarks for the landform capability (docs/subsystems/landform.md, "Performance notes").
//
//   landform.solve        a square sketch of the argument's side, 16 m points, one ridge, solved
//                         for 100,000 years in twenty steps on four workers: the items are
//                         point-steps, so the rate is what a step costs a point
//   landform.route        the flood, the receivers and the drainage area of one solved ground:
//                         the serial part of a step
//   landform.area_power   A^m over a million areas, as the solver does it (one square root, the
//                         argument 0) and as a general exponent would (det::exp(m * det::log(A)),
//                         the argument 1): what m = 1/2 saves (experiments/landform.md)
//
// A smoke run (CTest) solves a 64-point sketch for two steps, so it stays well under a second.
#include <core/jobs/job_system.h>
#include <core/math/deterministic.h>
#include <domain/landform/landform.h>
#include <foundation/bench/bench.h>

#include <cmath>
#include <string>

using namespace engine;

namespace {

landform::Sketch bench_sketch(u32 side, u32 steps) {
  landform::Sketch s;
  s.width = side;
  s.height = side;
  s.spacing_mm = 16000;
  s.origin_mm = {0, 0};
  const f64 extent = 16.0 * static_cast<f64>(side - 1);
  landform::Ridge ridge;
  ridge.points.push_back(landform::Point{0.2 * extent, 0.3 * extent});
  ridge.points.push_back(landform::Point{0.5 * extent, 0.55 * extent});
  ridge.points.push_back(landform::Point{0.8 * extent, 0.45 * extent});
  ridge.rate_mm_per_yr = 2.0;
  ridge.reach_m = 0.35 * extent;
  s.uplift.ridges.push_back(ridge);
  s.uplift.regional_mm_per_yr = 0.2;
  s.seed = 59;
  s.step_yr = 5000;
  s.duration_yr = 5000 * static_cast<i64>(steps);
  return s;
}

jobs::JobSystemConfig four_workers() {
  jobs::JobSystemConfig config;
  config.performance_workers = 4;
  config.efficiency_workers = 1;
  config.pin_threads = false;
  return config;
}

}  // namespace

ENGINE_BENCH_ARGS(landform_solve, "landform.solve", 256, 1024, 2048) {
  const u32 side = bench::smoke_mode() ? 64u : static_cast<u32>(state.arg());
  const u32 steps = bench::smoke_mode() ? 2u : 20u;
  const landform::Sketch sketch = bench_sketch(side, steps);
  jobs::JobSystem jobs(four_workers());
  landform::SolveOptions options;
  options.jobs = &jobs;
  landform::Landform land;
  std::string error;
  while (state.keep_running()) {
    const bool ok = landform::solve(sketch, nullptr, options, land, error);
    bench::keep(ok);
  }
  state.set_items(u64{side} * side * steps);
}

ENGINE_BENCH_ARGS(landform_route, "landform.route", 256, 1024, 2048) {
  const u32 side = bench::smoke_mode() ? 64u : static_cast<u32>(state.arg());
  const landform::Sketch sketch = bench_sketch(side, 2);
  jobs::JobSystem jobs(four_workers());
  landform::SolveOptions options;
  options.jobs = &jobs;
  landform::Landform land;
  std::string error;
  if (!landform::solve(sketch, nullptr, options, land, error)) return;
  Vector<u8> held(side * side, u8{0});
  for (u32 i = 0; i < side * side; ++i)
    held[i] = land.receiver[i] == landform::k_no_receiver ? u8{1} : u8{0};
  Vector<i64> filled;
  Vector<u32> order;
  Vector<u8> receiver;
  Vector<u32> area;
  while (state.keep_running()) {
    const bool ok = landform::route(side, side, land.height_mm, held, land.route_seed, &jobs,
                                    filled, order, receiver, area);
    bench::keep(ok);
  }
  state.set_items(u64{side} * side);
}

ENGINE_BENCH_ARGS(landform_area_power, "landform.area_power", 0, 1) {
  const u32 count = bench::smoke_mode() ? 1024u : 1000000u;
  const bool general = state.arg() != 0;
  f64 sum = 0.0;
  while (state.keep_running()) {
    sum = 0.0;
    if (general) {
      for (u32 i = 1; i <= count; ++i)
        sum += det::exp(0.45 * det::log(static_cast<f64>(i)));
    } else {
      for (u32 i = 1; i <= count; ++i)
        sum += std::sqrt(static_cast<f64>(i));
    }
    bench::keep(sum);
  }
  state.set_items(count);
}

// Micro-benchmarks for the water_state reference (docs/plan/11-performance-principles.md §11.8):
// one step of a square grid of side N, a lake with dry islands and a dam's break across it, on one
// thread. Items are cells, so the harness's rate is cells a second.
#include <domain/water_state/water_state.h>
#include <foundation/bench/bench.h>

#include <vector>

using namespace engine;

ENGINE_BENCH_ARGS(water_state_step, "water_state.step", 64, 256, 512) {
  const u32 n = static_cast<u32>(state.arg());
  water_state::Config config;
  config.nx = n;
  config.ny = n;
  config.cell_m = 0.5;
  water_state::Solver solver(config);
  std::vector<f64> bed(solver.cell_count());
  std::vector<f64> manning(solver.cell_count(), 0.03);
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const u32 i = solver.index(x, y);
      const bool island = (x / 16 + y / 16) % 4 == 0;
      bed[i] = island ? 1.5 : 0.01 * static_cast<f64>((x * 7 + y * 3) % 11);
      const f64 surface = x < n / 2 ? 1.0 : 0.5;
      solver.depth()[i] = surface > bed[i] ? surface - bed[i] : 0.0;
    }
  }
  water_state::Inputs in;
  in.bed_m = bed;
  in.manning = manning;
  while (state.keep_running()) {
    bench::keep(solver.step(in, 1.0));
  }
  state.set_items(solver.cell_count());
}

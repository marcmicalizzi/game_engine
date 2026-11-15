// Micro-benchmarks for the structures capability (docs/plan/11-performance-principles.md §11.8):
// the mesher over the example cot, and the walk records alone, which is what a collision build
// or a validator pays a building.
#include <domain/structures/structures.h>
#include <foundation/bench/bench.h>

using namespace engine;

ENGINE_BENCH_ARGS(structures_solve, "structures.solve", 1, 8) {
  const u32 houses = static_cast<u32>(state.arg());
  const structure::Description cot = structures::example_cot();
  structures::MeshPart parts[structures::k_surfaces];
  structures::Report report;
  std::string error;
  while (state.keep_running()) {
    for (u32 i = 0; i < houses; ++i)
      structures::solve(cot, structure::MeshScope::whole, parts, report, error);
    bench::keep(report.triangles);
  }
  state.set_items(houses);
}

ENGINE_BENCH_ARGS(structures_walk, "structures.walk_records", 1, 8) {
  const u32 houses = static_cast<u32>(state.arg());
  const structure::Description cot = structures::example_cot();
  structures::Walk walk;
  std::string error;
  while (state.keep_running()) {
    for (u32 i = 0; i < houses; ++i)
      structures::walk_records(cot, walk, error);
    bench::keep(walk.walls.size());
  }
  state.set_items(houses);
}

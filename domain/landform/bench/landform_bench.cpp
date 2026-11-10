// Micro-benchmarks for the landform capability (docs/plan/11-performance-principles.md §11.8).
// TODO(landform): measure the capability's hot path. The before-and-after numbers in a change
// description come from here, and a capability with a hot path and no bench is not finished.
#include <domain/landform/landform.h>
#include <foundation/bench/bench.h>

using namespace engine;

ENGINE_BENCH_ARGS(landform_tick, "landform.tick", 64, 1024, 16384) {
  const u32 instances = static_cast<u32>(state.arg());
  landform::LandformSystem system;
  system.begin_tick(0);
  while (state.keep_running()) {
    for (u32 i = 0; i < instances; ++i)
      system.tick(1.0f / 60.0f);
    bench::keep(system.elapsed());
  }
  state.set_items(instances);
}

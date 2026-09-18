// Micro-benchmarks for the animation capability (docs/plan/11-performance-principles.md §11.8).
// TODO(animation): measure the capability's hot path. The before-and-after numbers in a change
// description come from here, and a capability with a hot path and no bench is not finished.
#include <foundation/bench/bench.h>

#include <systems/animation/animation.h>

using namespace engine;

ENGINE_BENCH_ARGS(animation_tick, "animation.tick", 64, 1024, 16384) {
  const u32 instances = static_cast<u32>(state.arg());
  animation::AnimationSystem system;
  system.begin_tick(0);
  while (state.keep_running()) {
    for (u32 i = 0; i < instances; ++i)
      system.tick(1.0f / 60.0f);
    bench::keep(system.elapsed());
  }
  state.set_items(instances);
}

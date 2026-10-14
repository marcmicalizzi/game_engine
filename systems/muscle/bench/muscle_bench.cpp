// Micro-benchmarks for the muscle line tier (docs/plan/11-performance-principles.md §11.8).
// One S probe at the `step` case's static start: three RK4 activations and three implicit stages.
#include <foundation/bench/bench.h>
#include <systems/muscle/muscle.h>

using namespace engine;
using namespace engine::muscle;

ENGINE_BENCH(muscle_probe, "muscle.probe") {
  const LawRow law = make_law_row(FiberParameters{}, SeriesParameters{});
  const AnalyticPath path{AnalyticPath::Kind::Constant, 0.14800665778412417, 0.0, 0.0, 0.0};
  const State start{0.0, 0.0, 0.09988618207614752};
  Counters n{};
  Probe out{};
  Failure f{};
  while (state.keep_running()) {
    const bool ok = probe_s(law, path, 0.0, 0.8, start, 1.0 / 480.0, out, n, f);
    bench::keep(ok);
    bench::keep(out.stage[2].l);
  }
  state.set_items(1);
}

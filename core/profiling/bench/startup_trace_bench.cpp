// The startup trace's readers on a streamed session's shape (T210; docs/subsystems/profiling.md,
// "The readers are linear in the events"): the summary engine-view writes at exit, the Chrome
// trace and the acyclic check, by the trace's size, on the two shapes that matter — a session
// ready at frame 100 and walked on (its summary's top-level stages fixed, its events growing),
// and one never ready (every frame a top-level stage of the summary's). `.reference` is the
// summary as it was before T210, quadratic in the events, at sizes that finish in seconds; its
// growth law, extrapolated to the trace's 2^18 events, is the twenty minutes at exit the owner
// met after a five-minute walk. A smoke run takes the smallest size of each.

#include "../tests/startup_summary_reference.h"
#include "../tests/synthetic_session.h"

#include <core/json/json.h>
#include <foundation/bench/bench.h>

using namespace engine;
using namespace engine::profiling;

namespace {

u32 events_for(i64 arg) { return bench::smoke_mode() ? 2'000u : static_cast<u32>(arg); }

std::span<const TraceEvent> span_of(const Vector<TraceEvent>& events) {
  return {events.data(), events.size()};
}

}  // namespace

ENGINE_BENCH_ARGS(startup_summary_ready, "profiling.startup_summary.ready", 10'000, 100'000,
                  262'144, 1'000'000) {
  const Vector<TraceEvent> events = synthetic::streamed_session({events_for(state.arg()), 100});
  while (state.keep_running()) {
    bench::keep(startup_summary_json(span_of(events), synthetic::k_milestones, "nearby_stable"));
  }
  state.set_items(events.size());
}

ENGINE_BENCH_ARGS(startup_summary_never, "profiling.startup_summary.never", 10'000, 100'000,
                  262'144) {
  const Vector<TraceEvent> events =
      synthetic::streamed_session({events_for(state.arg()), synthetic::k_never});
  while (state.keep_running()) {
    bench::keep(startup_summary_json(span_of(events), synthetic::k_milestones, "nearby_stable"));
  }
  state.set_items(events.size());
}

ENGINE_BENCH_ARGS(startup_chrome_trace, "profiling.startup_trace.chrome", 10'000, 100'000,
                  262'144) {
  const Vector<TraceEvent> events = synthetic::streamed_session({events_for(state.arg()), 100});
  while (state.keep_running()) {
    bench::keep(chrome_trace_json(span_of(events), 0, 0).size());
  }
  state.set_items(events.size());
}

ENGINE_BENCH_ARGS(startup_trace_acyclic, "profiling.startup_trace.acyclic", 10'000, 100'000,
                  262'144) {
  const Vector<TraceEvent> events = synthetic::streamed_session({events_for(state.arg()), 100});
  while (state.keep_running()) {
    bench::keep(trace_acyclic(span_of(events)));
  }
  state.set_items(events.size());
}

// The summary before T210, for its growth law: ready at frame 100 it is linear in the events
// with a constant of the stages before ready; never ready it is quadratic.
ENGINE_BENCH_ARGS(startup_summary_reference_ready, "profiling.startup_summary.reference.ready",
                  10'000, 100'000, 262'144) {
  const Vector<TraceEvent> events = synthetic::streamed_session({events_for(state.arg()), 100});
  while (state.keep_running()) {
    bench::keep(reference::startup_summary_json(span_of(events), synthetic::k_milestones,
                                                "nearby_stable", 0));
  }
  state.set_items(events.size());
}

ENGINE_BENCH_ARGS(startup_summary_reference_never, "profiling.startup_summary.reference.never",
                  4'000, 8'000, 16'000, 32'000) {
  const Vector<TraceEvent> events =
      synthetic::streamed_session({events_for(state.arg()), synthetic::k_never});
  while (state.keep_running()) {
    bench::keep(reference::startup_summary_json(span_of(events), synthetic::k_milestones,
                                                "nearby_stable", 0));
  }
  state.set_items(events.size());
}

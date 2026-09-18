// The simulation core's hot paths (docs/subsystems/sim.md, "Performance notes"):
//
//   sim.wheel.*      10^6 timers through insert, cancel and advance — plan 05 §5.6's LOD3
//                    population is 10^5-10^6 records with a few events a game day each, so this
//                    is the shape of the real load and not a stress test.
//   sim.tiers.assign 10^5 entities scored against three observers at 1, 4 and 8 workers.
//   sim.scheduler.*  a tick of sixteen non-conflicting systems, four batches each.
//
// Debug builds only smoke-run benchmarks (docs/subsystems/bench.md), so the debug sizes are
// small enough for CTest; every number that reaches the docs page comes from msvc-release.

#include <core/base/macros.h>
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <domain/sim/scheduler.h>
#include <domain/sim/tiers.h>
#include <domain/sim/timing_wheel.h>
#include <foundation/bench/bench.h>

#include <memory>

using namespace engine;
using namespace engine::sim;

namespace {

#if ENGINE_DEBUG
constexpr u32 k_timers = 20'000;
constexpr u32 k_entities = 4'000;
#else
constexpr u32 k_timers = 1'000'000;
constexpr u32 k_entities = 100'000;
#endif

constexpr i64 k_spread_us = 30ll * 24 * 60 * 60 * 1'000'000;  // thirty game days

u64 next_random(u64& state) noexcept {
  state = state * 6364136223846793005ull + 1442695040888963407ull;
  return state >> 11;
}

// The same due times on every run and every machine, so two measurements compare.
Vector<i64> due_times(u32 count) {
  Vector<i64> times;
  times.reserve(count);
  u64 state = 0x9E3779B97F4A7C15ull;
  for (u32 i = 0; i < count; ++i) {
    times.push_back(static_cast<i64>(next_random(state) % static_cast<u64>(k_spread_us)));
  }
  return times;
}

TimingWheelConfig wheel_config(u32 capacity) {
  TimingWheelConfig config;
  config.initial_capacity = capacity;
  return config;
}

void fill(TimingWheel& wheel, const Vector<i64>& times, Vector<TimerHandle>& handles) {
  handles.clear();
  handles.reserve(times.size());
  TimerPayload payload;
  for (u32 i = 0; i < times.size(); ++i) {
    payload.subject = i;
    handles.push_back(wheel.schedule(GameTime{times[i]}, payload));
  }
}

void ignore_event(void*, const TimerEvent& event) { bench::keep(event.at.us); }

EventSink counting_sink(u64& counter) {
  EventSink sink;
  sink.fn = [](void* context, const TimerEvent& event) {
    ++(*static_cast<u64*>(context));
    bench::keep(event.at.us);
  };
  sink.context = &counter;
  return sink;
}

}  // namespace

ENGINE_BENCH(wheel_insert, "sim.wheel.insert") {
  const Vector<i64> times = due_times(k_timers);
  TimingWheel wheel(wheel_config(k_timers));
  Vector<TimerHandle> handles;
  handles.reserve(k_timers);
  while (state.keep_running()) {
    state.pause_timing();
    wheel.reset(GameTime{0});
    state.resume_timing();
    fill(wheel, times, handles);
    bench::keep(wheel.live_count());
  }
  state.set_items(k_timers);
}

ENGINE_BENCH(wheel_cancel, "sim.wheel.cancel") {
  const Vector<i64> times = due_times(k_timers);
  TimingWheel wheel(wheel_config(k_timers));
  Vector<TimerHandle> handles;
  while (state.keep_running()) {
    state.pause_timing();
    wheel.reset(GameTime{0});
    fill(wheel, times, handles);
    state.resume_timing();
    // Half of them, spread over the whole wheel rather than a prefix: cancelling is what the
    // simulation does most of, because a rescheduled timer is a cancel and an insert.
    for (u32 i = 0; i < handles.size(); i += 2)
      wheel.cancel(handles[i]);
    bench::keep(wheel.live_count());
  }
  state.set_items(k_timers / 2);
}

ENGINE_BENCH(wheel_advance, "sim.wheel.advance") {
  const Vector<i64> times = due_times(k_timers);
  TimingWheel wheel(wheel_config(k_timers));
  Vector<TimerHandle> handles;
  u64 delivered = 0;
  const EventSink sink = counting_sink(delivered);
  while (state.keep_running()) {
    state.pause_timing();
    wheel.reset(GameTime{0});
    fill(wheel, times, handles);
    for (u32 i = 0; i < handles.size(); i += 2)
      wheel.cancel(handles[i]);
    delivered = 0;
    state.resume_timing();
    wheel.advance(GameTime{k_spread_us}, sink);
    bench::keep(delivered);
  }
  state.set_items(k_timers / 2);
}

// The per-tick shape: a wheel holding the whole population, advanced one fixed step at a time.
ENGINE_BENCH(wheel_tick, "sim.wheel.tick") {
  const Vector<i64> times = due_times(k_timers);
  TimingWheel wheel(wheel_config(k_timers));
  Vector<TimerHandle> handles;
  fill(wheel, times, handles);
  EventSink sink;
  sink.fn = &ignore_event;
  GameTime now = wheel.now();
  while (state.keep_running()) {
    now += wheel.step();
    wheel.advance(now, sink);
    bench::keep(wheel.live_count());
  }
}

ENGINE_BENCH_ARGS(tiers_assign, "sim.tiers.assign", 0, 1, 4, 8) {
  const u32 workers = static_cast<u32>(state.arg());

  Vector<Vec3> positions;
  Vector<f32> importance;
  Vector<u8> tiers;
  positions.reserve(k_entities);
  importance.reserve(k_entities);
  tiers.reserve(k_entities);
  u64 random = 0x243F6A8885A308D3ull;
  for (u32 i = 0; i < k_entities; ++i) {
    const f32 x = static_cast<f32>(next_random(random) % 4096u);
    const f32 z = static_cast<f32>(next_random(random) % 4096u);
    positions.push_back(Vec3{x, 0.0f, z});
    importance.push_back(1.0f + static_cast<f32>(i % 4u));
    tiers.push_back(3);
  }

  ObserverSet observers;
  observers.add(Vec3{512.0f, 0.0f, 512.0f}, 1.0f);
  observers.add(Vec3{3000.0f, 0.0f, 900.0f}, 2.0f);
  observers.add(Vec3{100.0f, 0.0f, 3500.0f}, 0.5f);

  TierParams params;
  params.max_promotions = k_entities;
  params.max_demotions = k_entities;

  jobs::JobSystemConfig config;
  config.performance_workers = workers;
  config.efficiency_workers = 1;
  // Argument 0 means "no job system at all", which is the baseline every worker count is
  // measured against and the path a headless fast-forward takes.
  std::unique_ptr<jobs::JobSystem> owned;
  if (workers != 0) owned = std::make_unique<jobs::JobSystem>(config);
  jobs::JobSystem* system = owned.get();

  TierAssignment assignment(system);
  assignment.set_grain(4096);
  TierInput input;
  input.positions = std::span<const Vec3>(positions.data(), positions.size());
  input.importance = std::span<const f32>(importance.data(), importance.size());
  input.tiers = std::span<u8>(tiers.data(), tiers.size());

  Vector<TierChange> changes;
  changes.reserve(k_entities);
  while (state.keep_running()) {
    changes.clear();
    const TierStats stats = assignment.assign_tiers(input, observers, params, changes);
    bench::keep(stats.promoted);
    bench::keep(changes.size());
  }
  state.set_items(k_entities);
}

namespace {

struct BenchSystem {
  u64 accumulator = 0;

  static void tick(SystemContext& context, Batch batch) {
    auto* self = static_cast<BenchSystem*>(context.context);
    self->accumulator += context.tick.value + batch.begin;
    bench::keep(self->accumulator);
  }
};

}  // namespace

ENGINE_BENCH_ARGS(scheduler_tick, "sim.scheduler.tick", 0, 1, 4, 8) {
  const u32 workers = static_cast<u32>(state.arg());
  jobs::JobSystemConfig config;
  config.performance_workers = workers;
  config.efficiency_workers = 1;
  std::unique_ptr<jobs::JobSystem> owned;
  if (workers != 0) owned = std::make_unique<jobs::JobSystem>(config);

  SimSchedulerConfig scheduler_config;
  scheduler_config.job_system = owned.get();
  SimScheduler scheduler(scheduler_config);

  constexpr u32 k_systems = 16;
  BenchSystem bodies[k_systems];
  for (u32 i = 0; i < k_systems; ++i) {
    SystemDesc desc;
    desc.name = "bench_system";
    desc.phase = TickPhase::Systems;
    desc.batches = 4;
    desc.context = &bodies[i];
    desc.tick = &BenchSystem::tick;
    scheduler.add_system(desc);
  }

  while (state.keep_running()) {
    scheduler.step();
    bench::keep(scheduler.tick().value);
  }
  state.set_items(k_systems * 4);
}

// The line tier's cost (docs/plan/11-performance-principles.md §11.8,
// docs/experiments/muscle-line-tier-cost.md): nanoseconds per actuator per tick for 64 and 256
// actuators replicating two corpus cases on the runtime's path, a cubic Hermite per tick.
//
//   muscle.tick.step     the `step` case: excitation 0 -> 0.8 at 0.05 s -> 0 at 0.25 s, a
//                        constant taut path, no crossing
//   muscle.tick.passive  `corner_04_passive`: no excitation, a sine path that goes slack and taut
//                        again (six located crossings in 36 ticks), the stiffest tendon (e1 0.02)
//
// One iteration restores every actuator to its state after the startup tick (untimed) and runs
// the 60 ticks after it, ticks 2 to 61; an item is one actuator's tick. Before measuring, each
// benchmark runs those ticks once untimed and prints what a tick spends — probes, implicit stages,
// residual evaluations and root iterations per stage, activation rates, equilibrium solves,
// crossings — which is what the time is made of.
//
//   muscle.tick.rigid    tier 1 (the supervisor's rigid tendon, D20's 0.3 us target): the `step`
//                        case's actuators taken into tier 1 by the supervisor itself (eight frozen
//                        ticks, the projection at the ninth), then ticks 10 to 59 through
//                        `supervise` — fifty, so that no promotion falls inside — each the
//                        activation over eight parents, the projection and the rigid forces
#include <core/containers/vector.h>
#include <foundation/bench/bench.h>
#include <systems/muscle/muscle.h>
#include <systems/muscle/store.h>
#include <systems/muscle/supervisor.h>

#include <cstdio>
#include <span>

using namespace engine;
using namespace engine::muscle;

namespace {

constexpr u32 k_first = 2;   // the first tick after the startup tick
constexpr u32 k_ticks = 60;  // ticks 2..61

struct Scenario {
  const char* name;
  SeriesParameters series;
  AnalyticPath path;
  double (*excitation)(double t);
};

double step_control(double t) { return t < 0.05 ? 0.0 : (t < 0.25 ? 0.8 : 0.0); }
double passive_control(double) { return 0.0; }

Scenario step_case() {
  return Scenario{"step", SeriesParameters{},
                  AnalyticPath{AnalyticPath::Kind::Constant, 0.14800665778412417, 0.0, 0.0, 0.0},
                  step_control};
}

Scenario passive_case() {
  SeriesParameters s{};
  s.tendon_slack_length = 0.05;
  s.tendon_strain_at_unit_force = 0.02;
  s.pennation_at_optimal = 0.0;
  s.normalized_damping = 0.2;
  s.minimum_cosine = 0.2;
  return Scenario{"corner_04_passive", s,
                  AnalyticPath{AnalyticPath::Kind::Sine, 0.15000000000000002, 0.008,
                               31.41592653589793, 0.25132741228718347},
                  passive_control};
}

HermitePath hermite_for(const AnalyticPath& p, u64 tick) {
  const double t0 = static_cast<double>(tick - 1) / 60.0;
  const double t1 = static_cast<double>(tick) / 60.0;
  HermitePath h{};
  p.sample(t0, t0, h.l0, h.v0);
  p.sample(t1, t0, h.l1, h.v1);
  return h;
}

struct Fixture {
  LineStore store;
  Vector<LineState> after_startup;
  Vector<HermitePath> paths;  // [tick - k_first][actuator]
  Vector<double> excitation;  // [tick - k_first][actuator]
  u32 n = 0;
};

void build(Fixture& fx, const Scenario& sc, u32 n) {
  fx.n = n;
  fx.store.reserve(n);
  Vector<HermitePath> first(n, hermite_for(sc.path, 1));
  Vector<double> first_u(n, sc.excitation(0.0));
  for (u32 i = 0; i < n; ++i) {
    ActuatorSpec spec{};
    spec.series = sc.series;
    u32 index = 0;
    fx.store.add(spec, index);
    double length = 0.0;
    double rate = 0.0;
    sc.path.sample(0.0, 0.0, length, rate);
    Failure f{};
    initialize_static(fx.store, index, 0, 0.0, length, f);
  }
  tick(fx.store, 1, std::span<const double>(first_u.data(), n),
       std::span<const HermitePath>(first.data(), n));
  fx.after_startup.resize(n);
  for (u32 i = 0; i < n; ++i)
    save(fx.store, i, fx.after_startup[i]);
  fx.paths.resize(n * k_ticks);
  fx.excitation.resize(n * k_ticks);
  for (u32 k = 0; k < k_ticks; ++k) {
    const u64 t = k_first + k;
    for (u32 i = 0; i < n; ++i) {
      fx.paths[k * n + i] = hermite_for(sc.path, t);
      fx.excitation[k * n + i] = sc.excitation(static_cast<double>(t - 1) / 60.0);
    }
  }
}

void restore_all(Fixture& fx) {
  for (u32 i = 0; i < fx.n; ++i)
    restore(fx.store, i, fx.after_startup[i]);
}

template <StageRoot R>
u32 run_ticks(Fixture& fx, Counters* total) {
  u32 failures = 0;
  for (u32 k = 0; k < k_ticks; ++k) {
    failures += tick<HermitePath, R>(
        fx.store, k_first + k, std::span<const double>(fx.excitation.data() + k * fx.n, fx.n),
        std::span<const HermitePath>(fx.paths.data() + k * fx.n, fx.n));
    if (total != nullptr) {
      for (u32 i = 0; i < fx.n; ++i) {
        const Counters& c = fx.store.counters[i];
        total->probes += c.probes;
        total->stages += c.stages;
        total->residuals += c.residuals;
        total->root_iterations += c.root_iterations;
        total->rate_evaluations += c.rate_evaluations;
        total->events += c.events;
        total->bisection_probes += c.bisection_probes;
        total->zero_boundary_queries += c.zero_boundary_queries;
        total->equilibrium_solves += c.equilibrium_solves;
      }
    }
  }
  return failures;
}

template <StageRoot R>
void breakdown(Fixture& fx, const Scenario& sc) {
  restore_all(fx);
  Counters c{};
  const u32 failures = run_ticks<R>(fx, &c);
  const double per = 1.0 / (static_cast<double>(fx.n) * k_ticks);
  std::printf(
      "muscle.tick.%s (%s) n=%u: per actuator per tick: %.2f probes, %.2f stages, %.2f residuals "
      "(%.2f a stage), %.2f root iterations (%.2f a stage), %.1f activation rates, %.2f "
      "equilibrium "
      "solves, %.3f crossings (%.2f bisection probes); %u failed ticks. Transcendentals: a "
      "residual "
      "is 6 (3 exp in the active curve, 1 exp in the passive curve, 1 expm1 in a taut tendon, "
      "1 log1p or log in asinh) plus 2 sqrt; an activation rate 1 (tanh -> expm1)\n",
      sc.name, R == StageRoot::Newton ? "Newton" : "Brent", fx.n, c.probes * per, c.stages * per,
      c.residuals * per, static_cast<double>(c.residuals) / c.stages, c.root_iterations * per,
      static_cast<double>(c.root_iterations) / c.stages, c.rate_evaluations * per,
      c.equilibrium_solves * per, c.events * per, c.bisection_probes * per, failures);
}

template <StageRoot R>
void measure(bench::State& state, const Scenario& sc, bool& printed) {
  const u32 n = static_cast<u32>(state.arg());
  Fixture fx;
  build(fx, sc, n);
  // Once per benchmark: the harness calls a body once per repeat, and the counts are the same per
  // actuator at any n.
  if (!printed) breakdown<R>(fx, sc);
  printed = true;
  while (state.keep_running()) {
    state.pause_timing();
    restore_all(fx);
    state.resume_timing();
    bench::keep(run_ticks<R>(fx, nullptr));
    bench::keep(fx.store.fiber_length[0]);
  }
  state.set_items(static_cast<u64>(n) * k_ticks);
}

}  // namespace

ENGINE_BENCH_ARGS(muscle_tick_step, "muscle.tick.step", 64, 256) {
  static bool printed = false;
  measure<StageRoot::Brent>(state, step_case(), printed);
}

ENGINE_BENCH_ARGS(muscle_tick_passive, "muscle.tick.passive", 64, 256) {
  static bool printed = false;
  measure<StageRoot::Brent>(state, passive_case(), printed);
}

// The same with the stage root solved by the safeguarded Newton (newton.h, D23 (5)).
ENGINE_BENCH_ARGS(muscle_tick_step_newton, "muscle.tick.step.newton", 64, 256) {
  static bool printed = false;
  measure<StageRoot::Newton>(state, step_case(), printed);
}

ENGINE_BENCH_ARGS(muscle_tick_passive_newton, "muscle.tick.passive.newton", 64, 256) {
  static bool printed = false;
  measure<StageRoot::Newton>(state, passive_case(), printed);
}

// Tier 1: see the header. The setup's frozen ticks and the projection are untimed; an iteration
// restores every actuator to its first rigid tick's record and runs the fifty after it.
ENGINE_BENCH_ARGS(muscle_tick_rigid, "muscle.tick.rigid", 64, 256) {
  constexpr u64 k_rigid_first = 10;
  constexpr u32 k_rigid_ticks = 50;
  const u32 n = static_cast<u32>(state.arg());
  const Scenario sc = step_case();
  LineStore store;
  store.reserve(n);
  double length = 0.0;
  double rate = 0.0;
  sc.path.sample(0.0, 0.0, length, rate);
  for (u32 i = 0; i < n; ++i) {
    ActuatorSpec spec{};
    spec.series = sc.series;
    u32 index = 0;
    store.add(spec, index);
    Failure f{};
    initialize_static(store, index, 0, 0.0, length, f);
  }
  Vector<HermitePath> paths(n, hermite_for(sc.path, 1));
  Vector<double> u(n, 2.0);  // outside [0, 1]: eight frozen ticks
  for (u64 k = 1; k <= 9; ++k) {
    for (u32 i = 0; i < n; ++i) {
      paths[i] = hermite_for(sc.path, k);
      u[i] = k < 9 ? 2.0 : sc.excitation(static_cast<double>(k - 1) / 60.0);
    }
    supervise(store, k, std::span<const double>(u.data(), n),
              std::span<const HermitePath>(paths.data(), n));
  }
  if (store.supervisory[0].tier != LineTier::Rigid)
    std::printf("muscle.tick.rigid: the setup did not reach tier 1\n");
  Vector<LineState> rigid(n);
  for (u32 i = 0; i < n; ++i)
    save(store, i, rigid[i]);
  Vector<HermitePath> tick_paths(static_cast<usize>(n) * k_rigid_ticks);
  Vector<double> tick_u(static_cast<usize>(n) * k_rigid_ticks);
  for (u32 k = 0; k < k_rigid_ticks; ++k) {
    const u64 t = k_rigid_first + k;
    for (u32 i = 0; i < n; ++i) {
      tick_paths[static_cast<usize>(k) * n + i] = hermite_for(sc.path, t);
      tick_u[static_cast<usize>(k) * n + i] = sc.excitation(static_cast<double>(t - 1) / 60.0);
    }
  }
  while (state.keep_running()) {
    state.pause_timing();
    for (u32 i = 0; i < n; ++i)
      restore(store, i, rigid[i]);
    state.resume_timing();
    u32 frozen = 0;
    for (u32 k = 0; k < k_rigid_ticks; ++k) {
      frozen +=
          supervise(store, k_rigid_first + k,
                    std::span<const double>(tick_u.data() + static_cast<usize>(k) * n, n),
                    std::span<const HermitePath>(tick_paths.data() + static_cast<usize>(k) * n, n));
    }
    bench::keep(frozen);
    bench::keep(store.activation[0]);
  }
  state.set_items(static_cast<u64>(n) * k_rigid_ticks);
}

// One S probe at the `step` case's static start: three RK4 activations and three implicit stages.
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

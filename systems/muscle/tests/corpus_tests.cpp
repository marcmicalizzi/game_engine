// The qualification corpus replayed (content/muscle/, docs/subsystems/muscle.md, "The corpus"):
// every case from its archived static start through its trajectory's ticks against the
// reference's held oracle, the three D20 gates per case against expectations.json, a batch of the
// whole corpus against each case alone, and a pinned hash of every case's final state and work that
// every toolchain must reproduce. The case count, each case's tick count and its path kind come
// from the data: an analytic path (constant, linear, sine) for the synthetic rows, or a tabulated
// one (length and rate at every tick boundary, through the runtime's HermitePath) for a routed
// trajectory.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <systems/muscle/muscle.h>
#include <systems/muscle/store.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using namespace engine;
using namespace engine::muscle;

namespace {

std::string read_text(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

double num(const JsonValue& v, std::string_view key) {
  const JsonValue* x = v.find(key);
  double out = 0.0;
  REQUIRE(x != nullptr);
  REQUIRE(x->get_f64(out));
  return out;
}

std::string str(const JsonValue& v, std::string_view key) {
  const JsonValue* x = v.find(key);
  REQUIRE(x != nullptr);
  return std::string(x->as_string());
}

// The tick rate the scheme is qualified at (HermitePath::k_duration is its period).
constexpr double k_tick_hz = 60.0;

// The reference's control_value: the first segment whose end is after t, else the last.
struct Control {
  struct Segment {
    double start, end;
    int kind;  // 0 constant, 1 linear, 2 sine
    double value, start_value, end_value, offset, amplitude, omega;
  };
  Segment segments[8] = {};
  u32 count = 0;

  double at(double t) const {
    const Segment* s = &segments[count - 1];
    for (u32 i = 0; i < count; ++i) {
      if (t < segments[i].end) {
        s = &segments[i];
        break;
      }
    }
    if (s->kind == 0) return s->value;
    if (s->kind == 1) {
      const double r = (t - s->start) / (s->end - s->start);
      const double lower = r > 0.0 ? r : 0.0;      // Python's max(0., r)
      const double x = lower < 1.0 ? lower : 1.0;  // and min(1., ...)
      return s->start_value + (s->end_value - s->start_value) * x;
    }
    return s->offset + s->amplitude * det::sin(s->omega * t);
  }
};

// A corpus case's path for one tick: the analytic path, sampled in absolute time, or the tick's
// cubic Hermite between two tabulated boundaries, which is the runtime's own input type.
struct CorpusPath {
  bool tabulated = false;
  AnalyticPath analytic{};
  HermitePath hermite{};

  void sample(double t, double tick_start, double& length, double& rate) const noexcept {
    if (tabulated)
      hermite.sample(t, tick_start, length, rate);
    else
      analytic.sample(t, tick_start, length, rate);
  }
};

struct Case {
  std::string name;
  FiberParameters fiber{};
  SeriesParameters series{};
  Sha256 parameter_sha256{};
  bool tabulated = false;
  AnalyticPath analytic{};
  // Tabulated: length (m) and rate (m/s) at every tick boundary t = k / 60, k = 0 .. ticks.
  std::vector<double> boundary_l, boundary_v;
  Control control{};
  u32 ticks = 0;
  double a0 = 0.0;
  double l0 = 0.0;
  Cache cache0{};
  double force_limit = 0.0, fiber_limit = 0.0, work_limit = 0.0;
  // The oracle at t = 0 and every parent's end: l and F_t.
  std::vector<double> oracle_l, oracle_f;
  std::string expected_class;
  double tolerance_force = 0.0, tolerance_work = 0.0;  // a labelled case's base numbers

  // Tick k's path (k >= 1). Past a tabulated trajectory's end the last boundary is held, so a
  // batch can run a shorter case beside longer ones; nothing past a case's end is read.
  CorpusPath path_at(u64 k) const {
    CorpusPath p{};
    if (!tabulated) {
      p.analytic = analytic;
      return p;
    }
    p.tabulated = true;
    if (k > ticks) {
      p.hermite = HermitePath{boundary_l[ticks], 0.0, boundary_l[ticks], 0.0};
      return p;
    }
    p.hermite = HermitePath{boundary_l[k - 1], boundary_v[k - 1], boundary_l[k], boundary_v[k]};
    return p;
  }

  void start(double& length, double& rate) const { path_at(1).sample(0.0, 0.0, length, rate); }
};

// A path object: {"kind": "constant" | "linear" | "sine", ...} or
// {"kind": "tabulated", "tick_hz": 60, "samples": [[length_m, rate_m_s], ...]}. Sets the path and
// the tick count; `end_s` is the analytic kinds' duration and, when given, checks a table's.
void load_path(const JsonValue& path, double end_s, Case& c) {
  const std::string kind = str(path, "kind");
  c.tabulated = false;
  if (kind == "tabulated") {
    REQUIRE(num(path, "tick_hz") == k_tick_hz);
    const JsonValue::Array& samples = path.find("samples")->as_array();
    REQUIRE(samples.size() >= 2u);
    c.tabulated = true;
    c.boundary_l.assign(samples.size(), 0.0);
    c.boundary_v.assign(samples.size(), 0.0);
    for (u32 k = 0; k < samples.size(); ++k) {
      REQUIRE(samples[k].size() == 2u);
      REQUIRE(samples[k][0].get_f64(c.boundary_l[k]));
      REQUIRE(samples[k][1].get_f64(c.boundary_v[k]));
    }
    c.ticks = static_cast<u32>(samples.size() - 1);
    if (end_s > 0.0) CHECK(std::fabs(end_s * k_tick_hz - c.ticks) <= 1e-9);
    return;
  }
  if (kind == "constant") {
    c.analytic = AnalyticPath{AnalyticPath::Kind::Constant, num(path, "length_m"), 0.0, 0.0, 0.0};
  } else if (kind == "linear") {
    c.analytic = AnalyticPath{AnalyticPath::Kind::Linear, num(path, "origin_m"),
                              num(path, "rate_m_s"), 0.0, 0.0};
  } else {
    REQUIRE(kind == "sine");
    c.analytic =
        AnalyticPath{AnalyticPath::Kind::Sine, num(path, "center_m"), num(path, "amplitude_m"),
                     num(path, "omega_rad_s"), num(path, "rate_amplitude_m_s")};
  }
  const double ticks = std::round(end_s * k_tick_hz);
  REQUIRE(ticks >= 1.0);
  REQUIRE(std::fabs(end_s * k_tick_hz - ticks) <= 1e-9);
  c.ticks = static_cast<u32>(ticks);
}

void load_case(const std::string& root, const JsonValue& index_row, const JsonValue& expectation,
               Case& c) {
  c.name = str(index_row, "case");
  JsonValue doc;
  const std::string text = read_text(root + "/cases/" + c.name + ".json");
  REQUIRE(parse_json(text, doc).ok);
  const JsonValue& inputs = *doc.find("inputs");
  const JsonValue& fiber = *inputs.find("parameters")->find("fiber");
  const JsonValue& series = *inputs.find("parameters")->find("series");
  c.fiber.force_scale = num(fiber, "force_scale_N");
  c.fiber.optimal_fiber_length = num(fiber, "optimal_fiber_length_m");
  c.fiber.rigid_tendon_length = num(fiber, "rigid_tendon_length_m");
  c.fiber.vmax_lopt_per_s = num(fiber, "vmax_lopt_per_s");
  c.fiber.tau_activation = num(fiber, "tau_activation_s");
  c.fiber.tau_deactivation = num(fiber, "tau_deactivation_s");
  c.fiber.activation_smoothing = num(fiber, "activation_smoothing");
  c.fiber.passive_strain_at_unit_force = num(fiber, "passive_strain_at_unit_force");
  c.fiber.passive_shape = num(fiber, "passive_shape");
  c.fiber.passive_zero_normalized_length = num(fiber, "passive_zero_normalized_length");
  c.fiber.min_normalized_length = num(fiber, "min_normalized_length");
  c.fiber.max_normalized_length = num(fiber, "max_normalized_length");
  c.fiber.reference_area_m2 = num(fiber, "reference_area_m2");
  c.fiber.reference_fiber_length = num(fiber, "reference_fiber_length_m");
  c.series.tendon_slack_length = num(series, "tendon_slack_length_m");
  c.series.tendon_strain_at_unit_force = num(series, "tendon_strain_at_unit_force");
  c.series.pennation_at_optimal = num(series, "pennation_at_optimal_rad");
  c.series.normalized_damping = num(series, "normalized_damping");
  c.series.minimum_cosine = num(series, "minimum_cosine");
  REQUIRE(parse_sha256(str(inputs, "parameter_sha256"), c.parameter_sha256));

  load_path(*inputs.find("path"), num(inputs, "end_s"), c);
  const JsonValue::Array& segments = inputs.find("control_segments")->as_array();
  REQUIRE(segments.size() <= 8u);
  for (const JsonValue& s : segments) {
    Control::Segment& out = c.control.segments[c.control.count++];
    out.start = num(s, "start_s");
    out.end = num(s, "end_s");
    const std::string k = str(s, "kind");
    out.kind = k == "constant" ? 0 : k == "linear" ? 1 : 2;
    if (out.kind == 0) out.value = num(s, "value");
    if (out.kind == 1) {
      out.start_value = num(s, "start_value");
      out.end_value = num(s, "end_value");
    }
    if (out.kind == 2) {
      out.offset = num(s, "offset");
      out.amplitude = num(s, "amplitude");
      out.omega = num(s, "omega_rad_s");
    }
  }
  c.a0 = num(inputs, "initial_activation");
  const JsonValue& initial = *doc.find("initial_state");
  CHECK(num(initial, "activation") == c.a0);
  c.l0 = num(initial, "fiber_length_m");
  const JsonValue& out = *initial.find("output");
  c.cache0 = Cache{num(out, "tendon_force_N"),         num(out, "active_fiber_force_N"),
                   num(out, "passive_fiber_force_N"),  num(out, "damping_fiber_force_N"),
                   num(out, "passive_fiber_energy_J"), num(out, "tendon_energy_J")};

  // The oracle's nominal samples are t = 0 and every parent's end, matched within 1e-13 s (r010
  // METHOD.json); offset_pulse also has two samples at its off-tick control changes, which the
  // nominal comparison skips. There must be one for every parent end of the case's ticks.
  const u32 nominal_count = 1 + c.ticks * k_parents;
  c.oracle_l.assign(nominal_count, 0.0);
  c.oracle_f.assign(nominal_count, 0.0);
  const JsonValue::Array& rows = doc.find("samples")->as_array();
  const double parent_hz = k_tick_hz * static_cast<double>(k_parents);  // 480, exactly
  u32 nominal = 0;
  for (u32 i = 0; i < rows.size(); ++i) {
    double t = 0.0;
    REQUIRE(rows[i][0].get_f64(t));
    const double slot = std::round(t * parent_hz);
    if (std::fabs(t - slot / parent_hz) > 1e-13) continue;
    const u32 index = static_cast<u32>(slot);
    REQUIRE(index == nominal);
    REQUIRE(index < nominal_count);
    REQUIRE(rows[i][2].get_f64(c.oracle_l[index]));
    REQUIRE(rows[i][3].get_f64(c.oracle_f[index]));
    ++nominal;
  }
  REQUIRE(nominal == nominal_count);
  c.force_limit = num(index_row, "force_N");
  c.fiber_limit = num(index_row, "fiber_m");
  c.work_limit = num(index_row, "work_J");
  CHECK(str(expectation, "case") == c.name);
  c.expected_class = str(expectation, "class");
  if (c.expected_class != "qualified") {
    // A labelled case's tolerance is measured from the result its labels name.
    const std::string base = str(expectation, "tolerance_result");
    c.tolerance_force = num(expectation, base + "_force_error_N");
    c.tolerance_work = num(expectation, base + "_work_over_limit");
  }
}

// One actuator's replay: the maxima, the residual, the events, the final bytes, and its state at
// every parent's end.
struct Replay {
  bool complete = true;
  u32 failed_tick = 0;
  ValidationTest failed_test = ValidationTest::None;
  double force = 0.0, fiber = 0.0;
  double force_first = 0.0;  // the error at t = 1/480 s, the first parent's end
  double work[k_work_channels] = {};
  double residual = 0.0;
  u32 events = 0;
  u32 probes = 0;
  double a = 0.0, l = 0.0;
  Cache cache{};
  std::vector<double> sample_f, sample_l;  // [k_parents * (tick - 1) + j + 1], [0] the start
};

void arm(LineStore& store, u32 i, const Case& c) {
  LineState s{};
  s.parameter_sha256 = c.parameter_sha256;
  s.model_sha256 = k_model_sha256;
  s.last_commit_tick = 0;
  s.activation = c.a0;
  s.fiber_length = c.l0;
  s.committed_forces =
      ForceCache{c.cache0.tendon_force,  c.cache0.active_force,   c.cache0.passive_force,
                 c.cache0.damping_force, c.cache0.passive_energy, c.cache0.tendon_energy};
  s.initialized = true;
  s.startup_pending = true;  // the archived start is a static initialisation
  REQUIRE(restore(store, i, s) == RestoreStatus::Restored);
}

// Runs `count` cases in one store, each for its own ticks (the store for the longest); one replay
// each.
template <StageRoot R>
void replay(const Case* cases, u32 count, Replay* out) {
  LineStore store;
  store.reserve(count);
  u32 ticks = 0;
  for (u32 i = 0; i < count; ++i) {
    ActuatorSpec spec{};
    spec.parameter_sha256 = cases[i].parameter_sha256;
    spec.fiber = cases[i].fiber;
    spec.series = cases[i].series;
    u32 index = 0;
    REQUIRE(store.add(spec, index));
    arm(store, index, cases[i]);
    out[i] = Replay{};
    out[i].sample_f.assign(cases[i].oracle_f.size(), 0.0);
    out[i].sample_l.assign(cases[i].oracle_l.size(), 0.0);
    out[i].sample_f[0] = cases[i].cache0.tendon_force;
    out[i].sample_l[0] = cases[i].l0;
    ticks = std::max(ticks, cases[i].ticks);
  }
  std::vector<double> excitation(count, 0.0);
  std::vector<CorpusPath> paths(count);
  std::vector<ParentSample> samples(static_cast<usize>(count) * k_parents);
  for (u64 k = 1; k <= ticks; ++k) {
    // The excitation is sampled at the tick's start and held (study.held_segments).
    for (u32 i = 0; i < count; ++i) {
      excitation[i] = cases[i].control.at(static_cast<double>(k - 1) / k_tick_hz);
      paths[i] = cases[i].path_at(k);
    }
    tick<CorpusPath, R>(store, k, std::span<const double>(excitation),
                        std::span<const CorpusPath>(paths), std::span<ParentSample>(samples));
    for (u32 i = 0; i < count; ++i) {
      Replay& r = out[i];
      const TickOutcome& o = store.outcome[i];
      if (!r.complete || k > cases[i].ticks) continue;  // past its trajectory's end
      if (!o.committed) {
        r.complete = false;
        r.failed_tick = static_cast<u32>(k);
        r.failed_test = o.failed_test;
        continue;
      }
      for (u32 ch = 0; ch < k_work_channels; ++ch)
        r.work[ch] += o.work[ch];
      r.events += store.counters[i].events;
      r.probes += store.counters[i].probes;
      for (u32 j = 0; j < k_parents; ++j) {
        const ParentSample& s = samples[static_cast<usize>(i) * k_parents + j];
        const u32 index = static_cast<u32>((k - 1) * k_parents + j + 1);
        const double fe = std::fabs(s.cache.tendon_force - cases[i].oracle_f[index]);
        const double le = std::fabs(s.l - cases[i].oracle_l[index]);
        r.sample_f[index] = s.cache.tendon_force;
        r.sample_l[index] = s.l;
        if (index == 1) r.force_first = fe;
        if (fe > r.force) r.force = fe;
        if (le > r.fiber) r.fiber = le;
      }
      if (k == cases[i].ticks) {
        r.a = store.activation[i];
        r.l = store.fiber_length[i];
        r.cache = store.cache[i];
      }
    }
  }
  for (u32 i = 0; i < count; ++i) {
    Replay& r = out[i];
    // The t = 0 sample is the archived start itself: its errors are zero by construction.
    r.force = std::fmax(r.force, std::fabs(cases[i].cache0.tendon_force - cases[i].oracle_f[0]));
    r.residual = work_residual(r.work, elastic_energy(cases[i].cache0), elastic_energy(r.cache));
  }
}

bool same_bits(double x, double y) { return std::bit_cast<u64>(x) == std::bit_cast<u64>(y); }

// The corpus's cases with their expectations, in the index's order. False (and a message) when the
// corpus is not in this bundle.
bool load_corpus(std::vector<Case>& cases) {
  const std::string root = test::data_path(ENGINE_SOURCE_DIR "/content/muscle", "content/muscle");
  if (!test::path_exists(root + "/corpus.json")) {
    MESSAGE("not in this bundle: " << root);
    return false;
  }
  JsonValue index;
  JsonValue expectations;
  REQUIRE(parse_json(read_text(root + "/corpus.json"), index).ok);
  REQUIRE(parse_json(read_text(root + "/expectations.json"), expectations).ok);
  const JsonValue::Array& rows = index.find("cases")->as_array();
  const JsonValue::Array& expected = expectations.find("cases")->as_array();
  REQUIRE(!rows.empty());
  REQUIRE(expected.size() == rows.size());
  cases.assign(rows.size(), Case{});
  for (u32 i = 0; i < rows.size(); ++i)
    load_case(root, rows[i], expected[i], cases[i]);
  return true;
}

struct Gates {
  bool complete, force, fiber, work;
  double work_ratio;
  bool all() const { return complete && force && fiber && work; }
};

Gates gates_of(const Case& c, const Replay& r) {
  const double work_ratio = std::fabs(r.residual) / c.work_limit;
  return Gates{r.complete, r.force <= c.force_limit, r.fiber <= c.fiber_limit, work_ratio <= 1.0,
               work_ratio};
}

// One stage root's replay of the whole corpus: the gates per case, the batch against each alone,
// and the hash of every case's final (a, l) and work channels.
template <StageRoot R>
u64 check_corpus(const char* label, const std::vector<Case>& cases) {
  const u32 n = static_cast<u32>(cases.size());
  // Each case alone.
  std::vector<Replay> alone(n);
  for (u32 i = 0; i < n; ++i)
    replay<R>(&cases[i], 1, &alone[i]);

  std::printf(
      "muscle corpus (r010 schedule, %s stage root): case, max |F - oracle| / limit N, max |l - "
      "oracle| / limit mm, "
      "|R| / limit, events, class, result\n",
      label);
  u32 qualified = 0;
  for (u32 i = 0; i < n; ++i) {
    const Case& c = cases[i];
    const Replay& r = alone[i];
    INFO(c.name);
    const Gates g = gates_of(c, r);
    if (g.all()) ++qualified;
    const char* result = "FAIL";
    if (c.expected_class == "qualified") {
      result = g.all() ? "pass" : "FAIL";
      CHECK(g.complete);
      CHECK(g.force);
      CHECK(g.fiber);
      CHECK(g.work);
    } else if (c.expected_class == "known_limit_work") {
      const bool within = g.work_ratio <= 1.5 * c.tolerance_work;
      result = g.all()  ? "pass (labelled, passes all)"
               : within ? "known limit, within 1.5x"
                        : "FAIL";
      CHECK(g.complete);
      CHECK(g.force);
      CHECK(g.fiber);
      CHECK(within);
    } else {
      REQUIRE(c.expected_class == "known_limit_force");
      const bool within = r.force <= 1.5 * c.tolerance_force;
      result = g.all()  ? "pass (labelled, passes all)"
               : within ? "known limit, within 1.5x"
                        : "FAIL";
      CHECK(g.complete);
      CHECK(g.fiber);
      CHECK(g.work);
      CHECK(within);
    }
    std::printf("  %-18s %8.4f / %7.4f  %8.5f / %6.3f  %7.4f  %3u  %-17s %s%s\n", c.name.c_str(),
                r.force, c.force_limit, r.fiber * 1e3, c.fiber_limit * 1e3, g.work_ratio, r.events,
                c.expected_class.c_str(), result, r.complete ? "" : " (incomplete)");
    if (!r.complete)
      std::printf("    failed at tick %u: test %u\n", r.failed_tick,
                  static_cast<unsigned>(r.failed_test));
  }
  std::printf("muscle corpus (%s): %u of %u pass all three gates\n", label, qualified, n);
  for (const char* name : {"corner_04_passive", "corner_06_passive"}) {
    for (u32 i = 0; i < n; ++i) {
      if (cases[i].name == name)
        std::printf("muscle corpus: %s at t = 1/480 s: |F - oracle| = %.4f N\n", name,
                    alone[i].force_first);
    }
  }

  // The whole corpus in one store: per-actuator bytes identical to each alone.
  std::vector<Replay> batch(n);
  replay<R>(cases.data(), n, batch.data());
  for (u32 i = 0; i < n; ++i) {
    INFO(cases[i].name);
    CHECK(batch[i].complete == alone[i].complete);
    CHECK(same_bits(batch[i].a, alone[i].a));
    CHECK(same_bits(batch[i].l, alone[i].l));
    CHECK(same_bits(batch[i].cache.tendon_force, alone[i].cache.tendon_force));
    for (u32 ch = 0; ch < k_work_channels; ++ch)
      CHECK(same_bits(batch[i].work[ch], alone[i].work[ch]));
  }

  u64 h = k_hash_seed;
  for (u32 i = 0; i < n; ++i) {
    h = hash_combine(h, std::bit_cast<u64>(alone[i].a));
    h = hash_combine(h, std::bit_cast<u64>(alone[i].l));
    for (const double w : alone[i].work)
      h = hash_combine(h, std::bit_cast<u64>(w));
  }
  std::printf("muscle corpus (%s): final state and work hash 0x%016llx\n", label,
              static_cast<unsigned long long>(h));
  return h;
}

}  // namespace

TEST_CASE("muscle corpus: every case against the held oracle, three gates, known limits labelled") {
  std::vector<Case> cases;
  if (!load_corpus(cases)) return;
  CHECK(cases.size() >= 39u);

  // The law at t = 0 and the engine's own static start against the archived one, first.
  for (const Case& c : cases) {
    INFO(c.name);
    const LawRow p = make_law_row(c.fiber, c.series);
    Counters n{};
    Failure f{};
    double l = 0.0;
    double length = 0.0;
    double rate = 0.0;
    c.start(length, rate);
    REQUIRE(static_initial_length(p, c.a0, length, l, n, f));
    CHECK(std::fabs(l - c.l0) <= 1e-9);
    double v = 0.0;
    REQUIRE(equilibrium_velocity(p, c.a0, c.l0, length, v, n, f));
    LengthTerms terms{};
    REQUIRE(length_terms(p, c.a0, c.l0, length, terms, f));
    Forces forces{};
    REQUIRE(velocity_terms(p, terms, v, forces, f));
    double passive = 0.0;
    REQUIRE(passive_energy(p, c.l0, passive, f));
    const double unit = 1.0;  // a 1 N (or 1 J) floor under the relative tolerance
    CHECK(std::fabs(forces.tendon - c.cache0.tendon_force) <=
          1e-9 * std::fmax(std::fabs(c.cache0.tendon_force), unit));
    CHECK(std::fabs(forces.active - c.cache0.active_force) <=
          1e-9 * std::fmax(std::fabs(c.cache0.active_force), unit));
    CHECK(std::fabs(forces.passive - c.cache0.passive_force) <=
          1e-9 * std::fmax(std::fabs(c.cache0.passive_force), unit));
    CHECK(std::fabs(passive - c.cache0.passive_energy) <=
          1e-9 * std::fmax(std::fabs(c.cache0.passive_energy), unit));
    CHECK(std::fabs(terms.tendon.energy - c.cache0.tendon_energy) <=
          1e-9 * std::fmax(std::fabs(c.cache0.tendon_energy), unit));
  }

  const u64 brent = check_corpus<StageRoot::Brent>("Brent", cases);
  const u64 newton = check_corpus<StageRoot::Newton>("Newton", cases);
  // Taken on MSVC; GCC and Clang must reproduce them (core/math/deterministic.h, ADR-0035). A
  // difference on one toolchain is a determinism bug, not a value to replace; a change of the law,
  // the scheme, the schedule or the corpus's cases changes them on purpose, and says so.
  constexpr u64 k_pinned_brent = 0x19968103c9d00becull;
  constexpr u64 k_pinned_newton = 0x340a836aefb50dd1ull;
  CHECK(brent == k_pinned_brent);
  CHECK(newton == k_pinned_newton);
}

namespace {

// The tabulated kind proved on a synthetic case: tabulate the analytic path at the tick boundaries,
// write it as the corpus's JSON, read it back through load_path, and replay both.
template <StageRoot R>
void check_tabulated(const char* label, const Case& analytic) {
  REQUIRE(!analytic.tabulated);
  REQUIRE(analytic.analytic.kind == AnalyticPath::Kind::Sine);
  std::string json = "{\"kind\": \"tabulated\", \"tick_hz\": 60, \"samples\": [";
  for (u32 k = 0; k <= analytic.ticks; ++k) {
    // Boundary k is tick k + 1's start, t = k / 60, as the tick computes it.
    const double t = parent_start(k + 1, 0);
    double l = 0.0;
    double v = 0.0;
    analytic.analytic.sample(t, t, l, v);
    char buffer[64];
    json += k == 0 ? "[" : ", [";
    json.append(buffer, std::to_chars(buffer, buffer + sizeof(buffer), l).ptr);
    json += ", ";
    json.append(buffer, std::to_chars(buffer, buffer + sizeof(buffer), v).ptr);
    json += "]";
  }
  json += "]}";
  JsonValue path;
  REQUIRE(parse_json(json, path).ok);
  Case tabulated = analytic;
  load_path(path, analytic.ticks / k_tick_hz, tabulated);
  REQUIRE(tabulated.tabulated);
  REQUIRE(tabulated.ticks == analytic.ticks);

  Replay a{};
  Replay b{};
  replay<R>(&analytic, 1, &a);
  replay<R>(&tabulated, 1, &b);
  // The three gates still pass against the archived oracle.
  const Gates g = gates_of(tabulated, b);
  CHECK(g.complete);
  CHECK(g.force);
  CHECK(g.fiber);
  CHECK(g.work);

  // What to expect. A cubic Hermite through exact values and rates misses a function with a
  // bounded fourth derivative by at most h^4 / 384 * max |f''''| (at mid-tick), so a sine of
  // amplitude A and frequency w misses by e_L = A (w h)^4 / 384 in length, h = 1/60 s. The
  // tendon in series turns a path error into a force error of at most its stiffness times it:
  // dF/dL = (F + s) k / l_s for F = s (exp(k (L / l_s - 1)) - 1), taken at the run's largest force,
  // with the fiber held rigid against it (it is not: it yields, which only lowers the force error),
  // and the fiber can take up at most the whole path error along its line of action, e_L / cos.
  const double h = 1.0 / k_tick_hz;
  const double wh = analytic.analytic.omega * h;
  const double e_length = analytic.analytic.slope * wh * wh * wh * wh / 384.0;
  const LawRow p = make_law_row(analytic.fiber, analytic.series);
  double f_max = 0.0;
  for (const double f : a.sample_f)
    f_max = std::fmax(f_max, f);
  const double stiffness = (f_max + p.tendon_scale) * p.tendon_k / p.slack;
  const double e_rate =
      std::sqrt(3.0) / 216.0 * analytic.analytic.slope * wh * wh * wh * analytic.analytic.omega;
  // The path itself, at every parent end and mid-parent: the Hermite's own error within its bound.
  double path_error = 0.0;
  double rate_error = 0.0;
  for (u32 k = 1; k <= analytic.ticks; ++k) {
    const CorpusPath ap = analytic.path_at(k);
    const CorpusPath tp = tabulated.path_at(k);
    for (u32 j = 1; j <= 2 * k_parents; ++j) {
      const double t = parent_start(k, 0) + static_cast<double>(j) * h / (2.0 * k_parents);
      double la = 0.0, va = 0.0, lt = 0.0, vt = 0.0;
      ap.sample(t, parent_start(k, 0), la, va);
      tp.sample(t, parent_start(k, 0), lt, vt);
      path_error = std::fmax(path_error, std::fabs(lt - la));
      rate_error = std::fmax(rate_error, std::fabs(vt - va));
    }
  }
  double force_diff = 0.0;
  double fiber_diff = 0.0;
  for (usize i = 0; i < a.sample_f.size(); ++i) {
    force_diff = std::fmax(force_diff, std::fabs(b.sample_f[i] - a.sample_f[i]));
    fiber_diff = std::fmax(fiber_diff, std::fabs(b.sample_l[i] - a.sample_l[i]));
  }
  const double force_bound = stiffness * e_length;
  const double fiber_bound = e_length / analytic.series.minimum_cosine;
  std::printf(
      "muscle corpus (%s): %s tabulated through HermitePath: path |dL| %.3e m (bound %.3e), "
      "|dv| %.3e m/s (bound %.3e); replay |dF| %.3e N (bound %.3e = %.0f N/m x e_L), |dl| %.3e m "
      "(bound %.3e); |R| / limit %.4f against %.4f analytic; max |F - oracle| %.4f N against "
      "%.4f\n",
      label, analytic.name.c_str(), path_error, e_length, rate_error, e_rate, force_diff,
      force_bound, stiffness, fiber_diff, fiber_bound, g.work_ratio,
      std::fabs(a.residual) / analytic.work_limit, b.force, a.force);
  // The bounds with a margin of two: the path's for rounding in the Hermite's evaluation, the
  // force's and fiber's for the rate's own error (e_v = sqrt(3) / 216 A w^4 h^3), which the
  // stage equations read too.
  CHECK(path_error <= 2.0 * e_length);
  CHECK(rate_error <= 2.0 * e_rate);
  CHECK(force_diff <= 2.0 * force_bound);
  CHECK(fiber_diff <= 2.0 * fiber_bound);
}

}  // namespace

TEST_CASE("muscle corpus: a tabulated path replays a sine case within the Hermite's own error") {
  std::vector<Case> cases;
  if (!load_corpus(cases)) return;
  const Case* wave = nullptr;
  for (const Case& c : cases) {
    if (c.name == "active_wave") wave = &c;
  }
  REQUIRE(wave != nullptr);
  check_tabulated<StageRoot::Brent>("Brent", *wave);
  check_tabulated<StageRoot::Newton>("Newton", *wave);
}

// The corpus's numbers survive a parse-print-parse through core/json bit for bit: they are the
// reference's binary64 values, written in shortest round-trip form.
TEST_CASE("muscle corpus: every number round-trips through core/json") {
  const std::string root = test::data_path(ENGINE_SOURCE_DIR "/content/muscle", "content/muscle");
  if (!test::path_exists(root + "/corpus.json")) {
    MESSAGE("not in this bundle: " << root);
    return;
  }
  u64 numbers = 0;
  auto walk = [&](auto&& self, const JsonValue& a, const JsonValue& b) -> void {
    REQUIRE(a.kind() == b.kind());
    if (a.is_array()) {
      REQUIRE(a.size() == b.size());
      for (usize i = 0; i < a.size(); ++i)
        self(self, a[i], b[i]);
    } else if (a.is_object()) {
      for (const auto& [key, value] : a.as_object()) {
        const JsonValue* other = b.find(key);
        REQUIRE(other != nullptr);
        self(self, value, *other);
      }
    } else if (a.is_number()) {
      double x = 0.0;
      double y = 0.0;
      REQUIRE(a.get_f64(x));
      REQUIRE(b.get_f64(y));
      CHECK(std::bit_cast<u64>(x) == std::bit_cast<u64>(y));
      ++numbers;
    }
  };
  auto round_trip = [&](const std::string& path) {
    JsonValue first;
    REQUIRE(parse_json(read_text(path), first).ok);
    JsonValue second;
    REQUIRE(parse_json(write_json(first, JsonWriteOptions{false}), second).ok);
    walk(walk, first, second);
  };
  // The index too: it carries every qualified identity's nineteen values.
  round_trip(root + "/corpus.json");
  JsonValue index;
  REQUIRE(parse_json(read_text(root + "/corpus.json"), index).ok);
  for (const JsonValue& row : index.find("cases")->as_array())
    round_trip(root + "/cases/" + str(row, "case") + ".json");
  CHECK(numbers > 37u * 289u * 11u);
}

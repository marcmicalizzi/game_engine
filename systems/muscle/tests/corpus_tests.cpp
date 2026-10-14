// The qualification corpus replayed (content/muscle/, docs/subsystems/muscle.md, "The corpus"):
// every case from its archived static start through 36 ticks against the reference's held oracle,
// the three D20 gates per case against expectations.json, a batch of all 39 against each alone,
// and a pinned hash of every case's final state and work that every toolchain must reproduce.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <systems/muscle/muscle.h>
#include <systems/muscle/store.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

using namespace engine;
using namespace engine::muscle;

namespace {

// At most 36 ticks (0.6 s); shortening and lengthening run 24 (0.4 s).
constexpr u32 k_ticks = 36;
constexpr u32 k_samples = 1 + k_ticks * k_parents;  // t = 0 and every parent's end

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

struct Case {
  std::string name;
  FiberParameters fiber{};
  SeriesParameters series{};
  Sha256 parameter_sha256{};
  AnalyticPath path{};
  Control control{};
  u32 ticks = 0;
  double a0 = 0.0;
  double l0 = 0.0;
  Cache cache0{};
  double force_limit = 0.0, fiber_limit = 0.0, work_limit = 0.0;
  // The oracle: per sample, l, F_t, and the t = 0 energy.
  double oracle_l[k_samples] = {};
  double oracle_f[k_samples] = {};
  std::string expected_class;
  double r009_force = 0.0, r009_work = 0.0;
};

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

  const JsonValue& path = *inputs.find("path");
  const std::string kind = str(path, "kind");
  if (kind == "constant") {
    c.path = AnalyticPath{AnalyticPath::Kind::Constant, num(path, "length_m"), 0.0, 0.0, 0.0};
  } else if (kind == "linear") {
    c.path = AnalyticPath{AnalyticPath::Kind::Linear, num(path, "origin_m"), num(path, "rate_m_s"),
                          0.0, 0.0};
  } else {
    REQUIRE(kind == "sine");
    c.path = AnalyticPath{AnalyticPath::Kind::Sine, num(path, "center_m"), num(path, "amplitude_m"),
                          num(path, "omega_rad_s"), num(path, "rate_amplitude_m_s")};
  }
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
  // nominal comparison skips.
  const JsonValue::Array& rows = doc.find("samples")->as_array();
  u32 nominal = 0;
  for (u32 i = 0; i < rows.size(); ++i) {
    double t = 0.0;
    REQUIRE(rows[i][0].get_f64(t));
    const double slot = std::round(t * 480.0);
    if (std::fabs(t - slot / 480.0) > 1e-13) continue;
    const u32 index = static_cast<u32>(slot);
    REQUIRE(index == nominal);
    REQUIRE(index < k_samples);
    REQUIRE(rows[i][2].get_f64(c.oracle_l[index]));
    REQUIRE(rows[i][3].get_f64(c.oracle_f[index]));
    ++nominal;
  }
  REQUIRE((nominal - 1) % k_parents == 0);
  c.ticks = (nominal - 1) / k_parents;
  c.force_limit = num(index_row, "force_N");
  c.fiber_limit = num(index_row, "fiber_m");
  c.work_limit = num(index_row, "work_J");
  CHECK(str(expectation, "case") == c.name);
  c.expected_class = str(expectation, "class");
  c.r009_force = num(expectation, "r009_force_error_N");
  c.r009_work = num(expectation, "r009_work_over_limit");
}

// One actuator's replay: the maxima, the residual, the events, the final bytes.
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

// Runs the cases [first, first + count) in one store; one replay each.
template <StageRoot R>
void replay(const Case* cases, u32 count, Replay* out) {
  LineStore store;
  store.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    ActuatorSpec spec{};
    spec.parameter_sha256 = cases[i].parameter_sha256;
    spec.fiber = cases[i].fiber;
    spec.series = cases[i].series;
    u32 index = 0;
    REQUIRE(store.add(spec, index));
    arm(store, index, cases[i]);
    out[i] = Replay{};
  }
  double excitation[64] = {};
  AnalyticPath paths[64] = {};
  ParentSample samples[64 * k_parents] = {};
  REQUIRE(count <= 64u);
  for (u32 i = 0; i < count; ++i)
    paths[i] = cases[i].path;
  for (u64 k = 1; k <= k_ticks; ++k) {
    // The excitation is sampled at the tick's start and held (study.held_segments).
    for (u32 i = 0; i < count; ++i)
      excitation[i] = cases[i].control.at(static_cast<double>(k - 1) / 60.0);
    tick<AnalyticPath, R>(store, k, std::span<const double>(excitation, count),
                          std::span<const AnalyticPath>(paths, count),
                          std::span<ParentSample>(samples, static_cast<usize>(count) * k_parents));
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

}  // namespace

namespace {

// One stage root's replay of the whole corpus: the gates per case, the batch against each alone,
// and the hash of every case's final (a, l) and work channels.
template <StageRoot R>
u64 check_corpus(const char* label, Case* cases) {
  // Each case alone.
  static Replay alone[39];
  for (u32 i = 0; i < 39; ++i)
    replay<R>(&cases[i], 1, &alone[i]);

  std::printf(
      "muscle corpus (r010 schedule, %s stage root): case, max |F - oracle| / limit N, max |l - "
      "oracle| / limit mm, "
      "|R| / limit, events, class, result\n",
      label);
  u32 qualified = 0;
  for (u32 i = 0; i < 39; ++i) {
    const Case& c = cases[i];
    const Replay& r = alone[i];
    INFO(c.name);
    const bool force_ok = r.force <= c.force_limit;
    const bool fiber_ok = r.fiber <= c.fiber_limit;
    const double work_ratio = std::fabs(r.residual) / c.work_limit;
    const bool work_ok = work_ratio <= 1.0;
    const bool all = r.complete && force_ok && fiber_ok && work_ok;
    if (all) ++qualified;
    const char* result = "FAIL";
    if (c.expected_class == "qualified") {
      result = all ? "pass" : "FAIL";
      CHECK(r.complete);
      CHECK(force_ok);
      CHECK(fiber_ok);
      CHECK(work_ok);
    } else if (c.expected_class == "known_limit_work") {
      const bool within = work_ratio <= 1.5 * c.r009_work;
      result = all ? "pass (labelled, passes all)" : within ? "known limit, within 1.5x" : "FAIL";
      CHECK(r.complete);
      CHECK(force_ok);
      CHECK(fiber_ok);
      CHECK(within);
    } else {
      REQUIRE(c.expected_class == "known_limit_force");
      const bool within = r.force <= 1.5 * c.r009_force;
      result = all ? "pass (labelled, passes all)" : within ? "known limit, within 1.5x" : "FAIL";
      CHECK(r.complete);
      CHECK(fiber_ok);
      CHECK(work_ok);
      CHECK(within);
    }
    std::printf("  %-18s %8.4f / %7.4f  %8.5f / %6.3f  %7.4f  %3u  %-17s %s%s\n", c.name.c_str(),
                r.force, c.force_limit, r.fiber * 1e3, c.fiber_limit * 1e3, work_ratio, r.events,
                c.expected_class.c_str(), result, r.complete ? "" : " (incomplete)");
    if (!r.complete)
      std::printf("    failed at tick %u: test %u\n", r.failed_tick,
                  static_cast<unsigned>(r.failed_test));
  }
  std::printf("muscle corpus (%s): %u of 39 pass all three gates\n", label, qualified);
  for (const char* name : {"corner_04_passive", "corner_06_passive"}) {
    for (u32 i = 0; i < 39; ++i) {
      if (cases[i].name == name)
        std::printf(
            "muscle corpus: %s at t = 1/480 s: |F - oracle| = %.4f N (r009 S: %.4f N over the "
            "run)\n",
            name, alone[i].force_first, cases[i].r009_force);
    }
  }

  // All 39 in one store: per-actuator bytes identical to each alone.
  static Replay batch[39];
  replay<R>(cases, 39, batch);
  for (u32 i = 0; i < 39; ++i) {
    INFO(cases[i].name);
    CHECK(batch[i].complete == alone[i].complete);
    CHECK(same_bits(batch[i].a, alone[i].a));
    CHECK(same_bits(batch[i].l, alone[i].l));
    CHECK(same_bits(batch[i].cache.tendon_force, alone[i].cache.tendon_force));
    for (u32 ch = 0; ch < k_work_channels; ++ch)
      CHECK(same_bits(batch[i].work[ch], alone[i].work[ch]));
  }

  u64 h = k_hash_seed;
  for (u32 i = 0; i < 39; ++i) {
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

TEST_CASE("muscle corpus: 39 cases against the held oracle, three gates, known limits labelled") {
  const std::string root = test::data_path(ENGINE_SOURCE_DIR "/content/muscle", "content/muscle");
  if (!test::path_exists(root + "/corpus.json")) {
    MESSAGE("not in this bundle: " << root);
    return;
  }
  JsonValue index;
  JsonValue expectations;
  REQUIRE(parse_json(read_text(root + "/corpus.json"), index).ok);
  REQUIRE(parse_json(read_text(root + "/expectations.json"), expectations).ok);
  const JsonValue::Array& rows = index.find("cases")->as_array();
  const JsonValue::Array& expected = expectations.find("cases")->as_array();
  REQUIRE(rows.size() == 39u);
  REQUIRE(expected.size() == 39u);
  static Case cases[39];
  for (u32 i = 0; i < 39; ++i)
    load_case(root, rows[i], expected[i], cases[i]);

  // The law at t = 0 and the engine's own static start against the archived one, first.
  for (const Case& c : cases) {
    INFO(c.name);
    const LawRow p = make_law_row(c.fiber, c.series);
    Counters n{};
    Failure f{};
    double l = 0.0;
    double length = 0.0;
    double rate = 0.0;
    c.path.sample(0.0, 0.0, length, rate);
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
  // the scheme or the schedule changes them on purpose, and says so.
  constexpr u64 k_pinned_brent = 0x19968103c9d00becull;
  constexpr u64 k_pinned_newton = 0x340a836aefb50dd1ull;
  CHECK(brent == k_pinned_brent);
  CHECK(newton == k_pinned_newton);
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
  JsonValue index;
  REQUIRE(parse_json(read_text(root + "/corpus.json"), index).ok);
  for (const JsonValue& row : index.find("cases")->as_array()) {
    JsonValue first;
    REQUIRE(parse_json(read_text(root + "/cases/" + str(row, "case") + ".json"), first).ok);
    JsonValue second;
    REQUIRE(parse_json(write_json(first, JsonWriteOptions{false}), second).ok);
    walk(walk, first, second);
  }
  CHECK(numbers > 37u * 289u * 11u);
}

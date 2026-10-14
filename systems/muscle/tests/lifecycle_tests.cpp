// The store's lifecycle with the real law: static initialisation against the archived starts,
// whole-tick rollback, and the startup marker through save, JSON and restore (store.h).
#include <systems/muscle/muscle.h>
#include <systems/muscle/store.h>

#include <core/json/json.h>
#include <core/schema/json_reflect.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "rows.h"

using namespace engine;
using namespace engine::muscle;
using namespace engine::muscle::test_rows;

namespace {

// The `step` row's path: constant, taut, no crossing.
AnalyticPath constant_path(const Row& r) { return AnalyticPath{AnalyticPath::Kind::Constant, r.length, 0.0, 0.0, 0.0}; }

// A path that becomes invalid at `bad_after`: what a late failure in a tick looks like.
struct LatePath {
  AnalyticPath base;
  double bad_after;
  void sample(double t, double tick_start, double& length, double& rate) const noexcept {
    base.sample(t, tick_start, length, rate);
    if (t > bad_after) length = std::numeric_limits<double>::quiet_NaN();
  }
};

ActuatorSpec spec_of(const Row& r, u8 tag) {
  ActuatorSpec s{};
  s.actuator_id = Id128{1, tag};
  s.path_id = Id128{2, tag};
  s.parameter_sha256[0] = tag;
  s.series = series_of(r);
  return s;
}

bool same_bytes(const LineState& a, const LineState& b) {
  // The generated struct has padding; compare field by field, floats by their bits.
  auto bits = [](double x) {
    u64 u = 0;
    std::memcpy(&u, &x, sizeof u);
    return u;
  };
  return a.actuator_id == b.actuator_id && a.parameter_sha256 == b.parameter_sha256 &&
         a.path_id == b.path_id && a.model_sha256 == b.model_sha256 &&
         a.last_commit_tick == b.last_commit_tick && bits(a.activation) == bits(b.activation) &&
         bits(a.fiber_length) == bits(b.fiber_length) &&
         bits(a.committed_forces.tendon_force) == bits(b.committed_forces.tendon_force) &&
         bits(a.committed_forces.active_fiber_force) == bits(b.committed_forces.active_fiber_force) &&
         bits(a.committed_forces.passive_fiber_force) == bits(b.committed_forces.passive_fiber_force) &&
         bits(a.committed_forces.damping_fiber_force) == bits(b.committed_forces.damping_fiber_force) &&
         bits(a.committed_forces.passive_energy) == bits(b.committed_forces.passive_energy) &&
         bits(a.committed_forces.tendon_energy) == bits(b.committed_forces.tendon_energy) &&
         a.initialized == b.initialized && a.startup_pending == b.startup_pending;
}

}  // namespace

TEST_CASE("muscle lifecycle: static initialisation reproduces the archived static starts") {
  for (const Row& r : k_rows) {
    INFO(r.name);
    const LawRow p = law_of(r);
    Counters n{};
    Failure f{};
    double l = 0.0;
    REQUIRE(static_initial_length(p, r.a, r.length, l, n, f));
    CHECK(std::fabs(l - r.l) <= 1e-9);
  }
  // Outside the admissible interval: a path too short for any fiber length.
  const LawRow p = law_of(k_rows[0]);
  Counters n{};
  Failure f{};
  double l = 0.0;
  CHECK_FALSE(static_initial_length(p, 0.0, 0.01, l, n, f));
  CHECK(f.test == ValidationTest::StaticIntervalEmpty);
}

TEST_CASE("muscle lifecycle: a late failure rolls back the whole tick") {
  const Row& r = k_rows[0];
  LineStore store;
  store.reserve(1);
  u32 i = 0;
  REQUIRE(store.add(spec_of(r, 1), i));
  Failure f{};
  REQUIRE(initialize_static(store, i, 0, r.a, r.length, f));
  LineState before{};
  save(store, i, before);
  CHECK(before.startup_pending);
  // The path turns invalid in the eighth parent, after 28 of the startup tick's 32 probes.
  const double excitation[] = {0.8};
  const LatePath late[] = {LatePath{constant_path(r), 7.5 / 480.0}};
  CHECK(tick(store, 1, std::span<const double>(excitation), std::span<const LatePath>(late)) == 1);
  LineState after{};
  save(store, i, after);
  CHECK(same_bytes(before, after));  // committed bytes untouched, startup_pending kept
  const TickOutcome& o = store.outcome[i];
  CHECK_FALSE(o.committed);
  CHECK(o.failed_test == ValidationTest::PathSampleInvalid);
  CHECK(std::isnan(o.failed_value));
  for (const double w : o.work)
    CHECK(w == 0.0);
  CHECK(store.counters[i].probes > 24);  // it failed late, not at the start
  // The next tick retries from the retained state with its own inputs.
  const AnalyticPath good[] = {constant_path(r)};
  CHECK(tick(store, 2, std::span<const double>(excitation), std::span<const AnalyticPath>(good)) == 0);
  CHECK(store.outcome[i].committed);
  CHECK(store.last_commit_tick[i] == 2);
  CHECK(store.startup_pending[i] == 0);
  CHECK(store.activation[i] > 0.0);
  // A tick at or before the last committed one is refused by name.
  CHECK(tick(store, 2, std::span<const double>(excitation), std::span<const AnalyticPath>(good)) == 1);
  CHECK(store.outcome[i].failed_test == ValidationTest::TickOrder);
}

TEST_CASE("muscle lifecycle: startup_pending through save, JSON and restore") {
  const Row& r = k_rows[0];
  const double excitation[] = {0.3};
  const AnalyticPath path[] = {constant_path(r)};
  LineStore store;
  store.reserve(2);
  u32 i = 0;
  REQUIRE(store.add(spec_of(r, 7), i));
  Failure f{};
  REQUIRE(initialize_static(store, i, 0, r.a, r.length, f));

  // Save, through the schema's JSON, and restore into another store: the marker comes back.
  auto round_trip = [&](u32 from, LineStore& into, u32 to) {
    LineState saved{};
    save(store, from, saved);
    const std::string text = write_json(schema::to_json(saved));
    JsonValue parsed;
    REQUIRE(parse_json(text, parsed).ok);
    LineState loaded{};
    schema::ReadContext ctx;
    REQUIRE(schema::from_json(loaded, parsed, ctx));
    CHECK(same_bytes(saved, loaded));
    CHECK(restore(into, to, loaded) == RestoreStatus::Restored);
  };
  LineStore other;
  other.reserve(1);
  u32 j = 0;
  REQUIRE(other.add(spec_of(r, 7), j));
  round_trip(i, other, j);
  CHECK(other.startup_pending[j] == 1);
  CHECK(tick(other, 1, std::span<const double>(excitation), std::span<const AnalyticPath>(path)) == 0);
  CHECK(other.counters[j].probes == 32);  // the startup schedule
  CHECK(other.startup_pending[j] == 0);   // cleared by the first committed tick
  CHECK(tick(other, 2, std::span<const double>(excitation), std::span<const AnalyticPath>(path)) == 0);
  CHECK(other.counters[j].probes == 8);

  // The original store runs its first tick.
  CHECK(tick(store, 1, std::span<const double>(excitation), std::span<const AnalyticPath>(path)) == 0);
  // A restore after the first tick keeps the marker cleared: a restart is not a new lifetime.
  round_trip(i, other, j);
  CHECK(other.startup_pending[j] == 0);
  CHECK(tick(other, 2, std::span<const double>(excitation), std::span<const AnalyticPath>(path)) == 0);
  CHECK(other.counters[j].probes == 8);
  // Only a static reinitialisation re-arms it.
  REQUIRE(initialize_static(other, j, 2, 0.5, r.length, f));
  CHECK(other.startup_pending[j] == 1);
  CHECK(tick(other, 3, std::span<const double>(excitation), std::span<const AnalyticPath>(path)) == 0);
  CHECK(other.counters[j].probes == 32);

  // A record of another parameter set or another law is refused.
  LineState foreign{};
  save(store, i, foreign);
  foreign.parameter_sha256[0] = 99;
  CHECK(restore(other, j, foreign) == RestoreStatus::ParameterSetMismatch);
  save(store, i, foreign);
  foreign.model_sha256[0] ^= 1;
  CHECK(restore(other, j, foreign) == RestoreStatus::ModelUnrecognized);
}

TEST_CASE("muscle lifecycle: the store refuses past its capacity and an uninitialised actuator") {
  LineStore store;
  store.reserve(1);
  u32 i = 0;
  REQUIRE(store.add(spec_of(k_rows[0], 1), i));
  CHECK_FALSE(store.add(spec_of(k_rows[0], 2), i));
  const double excitation[] = {0.0};
  const AnalyticPath path[] = {constant_path(k_rows[0])};
  CHECK(tick(store, 1, std::span<const double>(excitation), std::span<const AnalyticPath>(path)) == 1);
  CHECK(store.outcome[0].failed_test == ValidationTest::NotInitialized);
}

// The supervisor (supervisor.h; D20, D21 (4)): the frozen streak and its degradation, the rigid
// projection, the rigid and inert tiers, the resume, promotion and its rejection, the freeze
// ledger, persistence and batching, against the authoring track's r008 Arm A witnesses read as data
// (r008_witnesses.h), and a pinned hash of a scripted sequence that every toolchain reproduces.
#include "r008_witnesses.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <systems/muscle/muscle.h>
#include <systems/muscle/store.h>
#include <systems/muscle/supervisor.h>

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <string_view>

using namespace engine;
using namespace engine::muscle;

namespace {

const JsonValue& witnesses() {
  static JsonValue root;
  static const bool parsed = parse_json(test_witnesses::k_r008, root).ok;
  REQUIRE(parsed);
  return root;
}

const JsonValue& at(const JsonValue& v, std::string_view key) {
  const JsonValue* x = v.find(key);
  REQUIRE(x != nullptr);
  return *x;
}

double num(const JsonValue& v, std::string_view key) {
  double out = 0.0;
  REQUIRE(at(v, key).get_f64(out));
  return out;
}

u64 whole(const JsonValue& v, std::string_view key) {
  u64 out = 0;
  REQUIRE(at(v, key).get_u64(out));
  return out;
}

std::string_view text(const JsonValue& v, std::string_view key) {
  std::string_view out;
  REQUIRE(at(v, key).get_string(out));
  return out;
}

PendingTransition pending_of(std::string_view name) {
  if (name == "ToRigid") return PendingTransition::ToRigid;
  if (name == "ToCompliant") return PendingTransition::ToCompliant;
  return PendingTransition::None;
}

// The r008 actuator's path, constant (PREFLIGHT-INPUTS.json, `initial_inputs.path`).
constexpr double k_l0 = 0.14800665778412417;
// Shorter than the tendon's slack (0.05 m): no rigid projection exists.
constexpr double k_short = 0.04;

// A path over one tick, linear from l0 at its start to l1 at its end. A script of these, one a
// tick, is how a test moves a path in and out of a tier's domain.
struct Ramp {
  double l0;
  double l1;
  void sample(double t, double tick_start, double& length, double& rate) const noexcept {
    const double s = (t - tick_start) / k_tick_duration;
    length = l0 + (l1 - l0) * s;
    rate = (l1 - l0) / k_tick_duration;
  }
};

constexpr Ramp k_still{k_l0, k_l0};

// A path that is not a number after the tick's first instant.
struct NanAfterStart {
  double l0;
  void sample(double t, double tick_start, double& length, double& rate) const noexcept {
    length = t == tick_start ? l0 : std::numeric_limits<double>::quiet_NaN();
    rate = 0.0;
  }
};

ActuatorSpec r008_spec(u8 tag) {
  ActuatorSpec s{};
  s.actuator_id = Id128{0x6d75, tag};
  s.path_id = Id128{0x7061, tag};
  REQUIRE(parse_sha256(text(witnesses(), "parameter_sha256"), s.parameter_sha256));
  return s;  // the reference law's default fiber and series parameters: the `step` row's
}

// r008's committed initial record, installed bit for bit rather than re-solved, so the ledger's
// numbers start from the witness's own inputs. `startup_pending` is armed, as a static start is.
void install_r008_initial(LineStore& store, u32 i) {
  const JsonValue& s0 = at(witnesses(), "initial_state");
  LineState st{};
  save(store, i, st);
  st.last_commit_tick = whole(s0, "last_commit_tick");
  st.activation = num(s0, "activation");
  st.fiber_length = num(s0, "fiber_length_m");
  st.committed_forces =
      ForceCache{num(s0, "tendon_force_N"),         num(s0, "active_fiber_force_N"),
                 num(s0, "passive_fiber_force_N"),  num(s0, "damping_fiber_force_N"),
                 num(s0, "passive_fiber_energy_J"), num(s0, "tendon_energy_J")};
  st.initialized = true;
  st.startup_pending = true;
  REQUIRE(restore(store, i, st) == RestoreStatus::Restored);
}

void r008_store(LineStore& store, u32 n = 1) {
  store.reserve(n);
  for (u32 j = 0; j < n; ++j) {
    u32 i = 0;
    REQUIRE(store.add(r008_spec(static_cast<u8>(j)), i));
    install_r008_initial(store, i);
  }
}

// One supervised tick of a one-actuator store; true when it committed.
template <class Path>
bool step(LineStore& store, u64 k, double u, const Path& path) {
  const double e[1] = {u};
  const Path p[1] = {path};
  const u32 frozen =
      supervise<Path>(store, k, std::span<const double>(e, 1), std::span<const Path>(p, 1));
  return frozen == 0 && store.outcome[0].committed;
}

u64 bits(double x) noexcept { return std::bit_cast<u64>(x); }

// Every field of actuator i's committed record, its tick's work and its tick's events, by value:
// two actuators with the same hash have the same bytes (the identities are left out, so two stores
// with different ids compare).
u64 hash_actuator(u64 h, const LineStore& store, u32 i) {
  LineState s{};
  save(store, i, s);
  for (const double d :
       {s.activation, s.fiber_length, s.committed_forces.tendon_force,
        s.committed_forces.active_fiber_force, s.committed_forces.passive_fiber_force,
        s.committed_forces.damping_fiber_force, s.committed_forces.passive_energy,
        s.committed_forces.tendon_energy, s.last_path_length})
    h = hash_combine(h, bits(d));
  for (const u64 v : {s.last_commit_tick, s.last_attempt_tick, u64{s.consecutive_frozen_ticks},
                      u64{s.consecutive_rigid_success_ticks}, static_cast<u64>(s.tier),
                      static_cast<u64>(s.pending_transition), u64{s.forces_stale},
                      u64{s.initialized}, u64{s.startup_pending}, u64{s.last_path_length_valid}})
    h = hash_combine(h, v);
  const TickWork& w = store.work[i];
  for (const double d :
       {w.external_input, w.active_output, w.dissipated, w.active_throughput, w.external_throughput,
        w.transition_elastic_jump, w.active_net_on, w.freeze_external_input,
        w.freeze_elastic_change, w.freeze_approximation_error, w.resume_initial_elastic,
        w.freeze_path_start, w.freeze_path_end})
    h = hash_combine(h, bits(d));
  for (const u64 v : {w.scheduler_tick, u64{w.mechanically_integrated},
                      static_cast<u64>(w.freeze_status), u64{w.freeze_external_input_valid}})
    h = hash_combine(h, v);
  for (const ValidationEvent& e : tick_events(store, i)) {
    for (const u64 v :
         {static_cast<u64>(e.kind), static_cast<u64>(e.failed_test), bits(e.failed_value),
          e.scheduler_tick, e.last_commit_tick, static_cast<u64>(e.tier_before),
          static_cast<u64>(e.tier_after), static_cast<u64>(e.requested_tier),
          static_cast<u64>(e.pending_transition), u64{e.consecutive_frozen_ticks},
          u64{e.consecutive_rigid_success_ticks}, u64{e.forces_stale}, u64{e.path_inputs_finite},
          bits(e.excitation), bits(e.path_length), bits(e.path_velocity),
          bits(e.transition_elastic_jump)})
      h = hash_combine(h, v);
  }
  return h;
}

u64 hash_state(const LineStore& store, u32 i) { return hash_actuator(k_hash_seed, store, i); }

// Through the schema's JSON and into another store, as a save and a restart would.
void restart(const LineStore& from, u32 i, LineStore& into, u32 j) {
  LineState saved{};
  save(from, i, saved);
  const std::string json = write_json(schema::to_json(saved));
  JsonValue parsed;
  REQUIRE(parse_json(json, parsed).ok);
  LineState loaded{};
  schema::ReadContext ctx;
  REQUIRE(schema::from_json(loaded, parsed, ctx));
  REQUIRE(restore(into, j, loaded) == RestoreStatus::Restored);
}

// Ticks 1..8 frozen by an excitation outside [0, 1] on the still path, tick 9 the projection and
// the first rigid tick.
void degrade(LineStore& store) {
  for (u64 k = 1; k <= 8; ++k)
    CHECK_FALSE(step(store, k, 1.2, k_still));
  REQUIRE(step(store, 9, 0.0, k_still));
  REQUIRE(store.supervisory[0].tier == LineTier::Rigid);
}

}  // namespace

TEST_CASE(
    "muscle supervisor: eight frozen ticks degrade, a restart keeps the decision, tick nine "
    "projects (r008 witness)") {
  const JsonValue& w = at(witnesses(), "failure_eight_restart_nine");
  LineStore store;
  r008_store(store);
  LineState initial{};
  save(store, 0, initial);
  for (u64 k = 1; k <= 8; ++k) {
    CAPTURE(k);
    CHECK_FALSE(step(store, k, 1.2, k_still));
    const auto events = tick_events(store, 0);
    REQUIRE(events.size() == (k == 8 ? 2u : 1u));
    CHECK(events[0].kind == ValidationKind::DomainFailure);
    CHECK(events[0].failed_test == ValidationTest::ExcitationOutOfRange);
    CHECK(events[0].failed_value == 1.2);
    CHECK(events[0].forces_stale);
    CHECK(events[0].consecutive_frozen_ticks == k);
    CHECK(events[0].tier_after == LineTier::Compliant);
    // D20: the stale cached force is the tick's output, with no catch-up.
    CHECK(has_force_output(store, 0));
    CHECK(output_force(store, 0) == initial.committed_forces.tendon_force);
    CHECK(store.supervisory[0].forces_stale);
    // A still path does no stale work and changes no elastic energy, and the ledger says so.
    CHECK(store.work[0].freeze_status == FreezeAccountingStatus::Valid);
    CHECK(store.work[0].freeze_external_input == 0.0);
    CHECK(store.work[0].freeze_elastic_change == 0.0);
    CHECK_FALSE(store.work[0].mechanically_integrated);
    CHECK(store.work[0].external_input == 0.0);
  }
  // The eighth failure's decision, the witness's: a request, the tier still compliant.
  const JsonValue& decision = at(w, "decisions")[0];
  const ValidationEvent& degraded = tick_events(store, 0)[1];
  CHECK(text(decision, "kind") == "TierDegraded");
  CHECK(degraded.kind == ValidationKind::TierDegraded);
  CHECK(degraded.scheduler_tick == whole(decision, "tick"));
  CHECK(static_cast<u64>(degraded.tier_before) == whole(decision, "tier_before"));
  CHECK(static_cast<u64>(degraded.tier_after) == whole(decision, "tier_after"));
  CHECK(static_cast<u64>(degraded.requested_tier) == whole(decision, "requested_tier"));
  CHECK(degraded.pending_transition == PendingTransition::ToRigid);

  // The persisted record: the mechanics retained bit for bit, the supervisor's fields the
  // witness's.
  LineState saved{};
  save(store, 0, saved);
  CHECK(saved.last_commit_tick == initial.last_commit_tick);
  CHECK(bits(saved.activation) == bits(initial.activation));
  CHECK(bits(saved.fiber_length) == bits(initial.fiber_length));
  CHECK(saved.committed_forces == initial.committed_forces);
  const JsonValue& sup = at(w, "supervisor");
  CHECK(saved.last_attempt_tick == whole(sup, "last_attempt_tick"));
  CHECK(saved.consecutive_frozen_ticks == whole(sup, "consecutive_frozen_ticks"));
  CHECK(static_cast<u64>(saved.tier) == whole(sup, "tier"));
  CHECK(saved.pending_transition == pending_of(text(sup, "pending_transition")));
  CHECK(saved.consecutive_rigid_success_ticks == whole(sup, "consecutive_rigid_success_ticks"));

  // A restart through the schema's JSON makes the same ninth tick.
  LineStore restarted;
  restarted.reserve(1);
  u32 j = 0;
  REQUIRE(restarted.add(r008_spec(0), j));
  restart(store, 0, restarted, j);
  CHECK(step(store, 9, 0.0, k_still));
  CHECK(step(restarted, 9, 0.0, k_still));
  CHECK(hash_state(store, 0) == hash_state(restarted, 0));

  // The projection at tick nine's start, the witness's geometry and jump, then the tick's rigid
  // integration from it: success one.
  const JsonValue& projection = at(w, "projection");
  const auto events = tick_events(store, 0);
  REQUIRE(events.size() == 1);
  CHECK(events[0].kind == ValidationKind::TierChanged);
  CHECK(events[0].tier_before == LineTier::Compliant);
  CHECK(events[0].tier_after == LineTier::Rigid);
  CHECK(events[0].scheduler_tick == whole(projection, "tick"));
  CHECK(std::fabs(events[0].transition_elastic_jump -
                  num(projection, "transition_elastic_jump_J")) <= 1e-15);
  CHECK(store.work[0].transition_elastic_jump == events[0].transition_elastic_jump);
  CHECK(static_cast<u64>(store.supervisory[0].tier) == whole(projection, "tier"));
  CHECK(store.supervisory[0].pending_transition ==
        pending_of(text(projection, "pending_transition")));
  double l = 0.0;
  Failure f{};
  REQUIRE(rigid_projection(store.law[0], k_l0, l, f));
  CHECK(std::fabs(l - num(projection, "fiber_length_m")) <= 1e-15);
  CHECK(store.fiber_length[0] == l);  // the still path's projection at the tick's end too
  CHECK(store.last_commit_tick[0] == 9);
  CHECK(store.supervisory[0].consecutive_rigid_success_ticks == 1);
  CHECK_FALSE(store.supervisory[0].forces_stale);
  CHECK(store.work[0].mechanically_integrated);
  // The degradation's streak stays with the record through the rigid tier (the witness's 8).
  CHECK(store.supervisory[0].consecutive_frozen_ticks == 8);
  std::printf("muscle supervisor: tick 9 projection l = %.17g m, jump = %.17g J (r008: %.17g)\n", l,
              events[0].transition_elastic_jump, num(projection, "transition_elastic_jump_J"));
}

TEST_CASE(
    "muscle supervisor: sixty rigid successes schedule the promotion, a rigid failure resets "
    "the streak (r008 witness), and the promotion is the next tick's start") {
  const JsonValue& recovery = at(witnesses(), "rigid_recovery");
  LineStore store;
  r008_store(store);
  degrade(store);
  // Success one was tick 9; sixty is tick 68, and the decision is that tick's and no earlier one's.
  for (u64 k = 10; k <= 68; ++k) {
    CAPTURE(k);
    REQUIRE(step(store, k, 0.0, k_still));
    const auto events = tick_events(store, 0);
    CHECK(events.size() == (k == 68 ? 1u : 0u));
    if (k == 68) CHECK(events[0].kind == ValidationKind::PromotionScheduled);
  }
  const JsonValue& scheduled = at(recovery, "scheduled");
  const Supervisory& s = store.supervisory[0];
  CHECK(static_cast<u64>(s.tier) == whole(scheduled, "tier"));
  CHECK(s.pending_transition == pending_of(text(scheduled, "pending_transition")));
  CHECK(s.consecutive_rigid_success_ticks == whole(scheduled, "consecutive_rigid_success_ticks"));
  CHECK(s.consecutive_frozen_ticks == whole(scheduled, "consecutive_frozen_ticks"));
  // The witness's `last_attempt_tick` is the synthetic double's, which never attempted a rigid
  // tick; the engine's is the tick it last ran.
  CHECK(s.last_attempt_tick == 68);

  // Tick 69's start: static_initial_length at the retained activation and the current path, the
  // tier-0 record initialised as a static start (startup_pending armed: the tick is 32 probes),
  // and the tick runs compliant.
  const Cache rigid = store.cache[0];
  REQUIRE(step(store, 69, 0.0, k_still));
  const auto events = tick_events(store, 0);
  REQUIRE(events.size() == 1);
  CHECK(events[0].kind == ValidationKind::TierPromoted);
  CHECK(events[0].tier_before == LineTier::Rigid);
  CHECK(events[0].tier_after == LineTier::Compliant);
  CHECK(store.supervisory[0].tier == LineTier::Compliant);
  CHECK(store.supervisory[0].pending_transition == PendingTransition::None);
  CHECK(store.supervisory[0].consecutive_rigid_success_ticks == 0);
  CHECK(store.supervisory[0].consecutive_frozen_ticks == 0);
  CHECK(store.counters[0].probes == k_parents * k_startup_pieces);
  CHECK(store.startup_pending[0] == 0);  // cleared by the tick that committed
  // The jump undoes the degradation's: back to the static start the record began at (activation
  // zero throughout, so the same equilibrium, to the static solve's tolerance).
  const double jump = events[0].transition_elastic_jump;
  CHECK(store.work[0].transition_elastic_jump == jump);
  const double down = num(at(at(witnesses(), "failure_eight_restart_nine"), "projection"),
                          "transition_elastic_jump_J");
  CHECK(std::fabs(jump + down) <= 1e-9);
  CHECK(jump == elastic_energy(store.cache[0]) - elastic_energy(rigid));

  // The reset witness: at 59 successes a failed rigid tick resets the streak to zero and schedules
  // nothing; the stale force is the output and the rigid ledger books the stale work.
  LineStore reset;
  r008_store(reset);
  degrade(reset);
  for (u64 k = 10; k <= 67; ++k)
    REQUIRE(step(reset, k, 0.0, k_still));
  CHECK(reset.supervisory[0].consecutive_rigid_success_ticks == 59);
  CHECK_FALSE(step(reset, 68, 1.2, k_still));
  const auto failed = tick_events(reset, 0);
  REQUIRE(failed.size() == 1);
  CHECK(failed[0].kind == ValidationKind::DomainFailure);
  CHECK(failed[0].failed_test == ValidationTest::ExcitationOutOfRange);
  CHECK(failed[0].tier_after == LineTier::Rigid);
  CHECK(reset.supervisory[0].consecutive_rigid_success_ticks ==
        whole(at(recovery, "reset"), "consecutive_rigid_success_ticks"));
  CHECK(reset.supervisory[0].pending_transition == PendingTransition::None);
  CHECK(reset.supervisory[0].forces_stale);
  CHECK(reset.work[0].freeze_status == FreezeAccountingStatus::Valid);
  CHECK(output_force(reset, 0) == reset.cache[0].tendon_force);
}

TEST_CASE("muscle supervisor: a rejected promotion resets the streak and the tick runs rigid") {
  LineStore store;
  r008_store(store);
  degrade(store);
  for (u64 k = 10; k <= 68; ++k)
    REQUIRE(step(store, k, 0.0, k_still));
  REQUIRE(store.supervisory[0].pending_transition == PendingTransition::ToCompliant);
  // A path that starts the tick at a nanometre has no admissible static interval; it ends the tick
  // where the rigid projection is valid, so the tick still integrates in tier 1.
  CHECK(step(store, 69, 0.0, Ramp{1e-9, k_l0}));
  const auto events = tick_events(store, 0);
  REQUIRE(events.size() == 1);
  CHECK(events[0].kind == ValidationKind::PromotionRejected);
  CHECK(events[0].failed_test == ValidationTest::StaticIntervalEmpty);
  CHECK(events[0].consecutive_rigid_success_ticks == 0);
  CHECK(store.supervisory[0].tier == LineTier::Rigid);
  CHECK(store.supervisory[0].pending_transition == PendingTransition::None);
  CHECK(store.supervisory[0].consecutive_rigid_success_ticks == 1);  // the tick's own success
  CHECK(store.last_commit_tick[0] == 69);
}

TEST_CASE(
    "muscle supervisor: a frozen tick on a moving path books the stale work against the "
    "elastic change at the held fiber; the resume starts at the kinematic energy (r008 "
    "witness)") {
  const JsonValue& witness = at(witnesses(), "moving_freeze_resume");
  const JsonValue& book = at(witness, "freeze");
  const double delta = num(witnesses(), "freeze_path_delta_m");
  const AnalyticPath moving{AnalyticPath::Kind::Linear, k_l0, delta * 60.0, 0.0, 0.0};
  LineStore store;
  r008_store(store);
  LineState initial{};
  save(store, 0, initial);
  CHECK_FALSE(step(store, 1, 1.2, moving));
  const TickWork& w = store.work[0];
  CHECK(text(book, "status") == "Valid");
  CHECK(w.freeze_status == FreezeAccountingStatus::Valid);
  CHECK(w.freeze_external_input_valid);
  // The two ends of a 10 um move differ from the witness's L0 + delta by an ulp of 0.148 m, which
  // is 3e-12 of the move: these agree to 2e-15 J of 2e-4.
  CHECK(std::fabs(w.freeze_external_input - num(book, "external_input_J")) <= 2e-15);
  CHECK(std::fabs(w.freeze_elastic_change - num(book, "elastic_change_J")) <= 2e-15);
  CHECK(std::fabs(w.freeze_approximation_error - num(book, "approximation_error_J")) <= 4e-15);
  CHECK(std::fabs(w.freeze_external_input - w.freeze_elastic_change -
                  w.freeze_approximation_error) <= 1e-14);
  CHECK(std::fabs(w.resume_initial_elastic - num(book, "resume_initial_elastic_J")) <= 1e-15);
  // Not the old cache's energy: the path moved the tendon, the held fiber did not move.
  CHECK(std::fabs(w.resume_initial_elastic - elastic_energy(store.cache[0])) > 1e-4);
  std::printf(
      "muscle supervisor: freeze ledger W = %.17g J, dE = %.17g J, W - dE = %.17g J, "
      "resume E = %.17g J\n",
      w.freeze_external_input, w.freeze_elastic_change, w.freeze_approximation_error,
      w.resume_initial_elastic);
  // The frozen tick retained every committed byte.
  LineState frozen{};
  save(store, 0, frozen);
  CHECK(bits(frozen.fiber_length) == bits(initial.fiber_length));
  CHECK(frozen.committed_forces == initial.committed_forces);
  CHECK(frozen.last_commit_tick == 0);
  const double resume_e = w.resume_initial_elastic;

  // The resume: one nominal tick [1/60, 2/60] from the retained state, no catch-up. r008's tick was
  // r008's scheme and this is r010's with its startup pieces, so the resumed state agrees to the
  // schemes' difference: measured 1.8e-13 m and 2.9e-8 N on MSVC.
  REQUIRE(step(store, 2, 0.0, moving));
  const JsonValue& final_state = at(witness, "final_state");
  CHECK(store.last_commit_tick[0] == whole(final_state, "last_commit_tick"));
  CHECK(store.activation[0] == num(final_state, "activation"));
  const double dl = store.fiber_length[0] - num(final_state, "fiber_length_m");
  const double dforce = store.cache[0].tendon_force - num(final_state, "tendon_force_N");
  CHECK(std::fabs(dl) <= 1e-11);
  CHECK(std::fabs(dforce) <= 1e-6);
  // The resume tick's ledger from the kinematic energy closes, as r008's did (-1.8e-6 J against a
  // 1e-2 J floor; measured -8.4e-8 J here).
  const double residual = store.work[0].external_input + store.work[0].active_output -
                          (elastic_energy(store.cache[0]) - resume_e) - store.work[0].dissipated;
  CHECK(std::fabs(residual) <= 1e-6);
  std::printf(
      "muscle supervisor: resumed tick 2 against r008: dl = %.3g m, dF = %.3g N, "
      "residual from the kinematic energy %.3g J (r008 %.3g J)\n",
      dl, dforce, residual, num(at(witness, "resume_work"), "signed_residual_J"));
}

TEST_CASE(
    "muscle supervisor: an undefined path is a DomainFailure frozen tick with an undefined "
    "ledger and its own event (r008 witness)") {
  const JsonValue& undefined = at(at(witnesses(), "undefined_and_inert"), "undefined");
  LineStore store;
  r008_store(store);
  CHECK_FALSE(step(store, 1, 0.0, NanAfterStart{k_l0}));
  const auto events = tick_events(store, 0);
  REQUIRE(events.size() == 2);
  CHECK(events[0].kind == ValidationKind::DomainFailure);
  CHECK(events[0].failed_test == ValidationTest::PathSampleInvalid);
  CHECK_FALSE(events[0].path_inputs_finite);
  CHECK(events[1].kind == ValidationKind::FreezeAccountingUndefined);
  CHECK(events[1].failed_test == ValidationTest::PathSampleInvalid);
  const TickWork& w = store.work[0];
  CHECK(text(undefined, "status") == "UndefinedPath");
  CHECK(w.freeze_status == FreezeAccountingStatus::UndefinedPath);
  bool valid = true;
  REQUIRE(at(undefined, "external_input_valid").get_bool(valid));
  CHECK(w.freeze_external_input_valid == valid);
  CHECK(at(undefined, "external_input_J").is_null());
  CHECK(w.freeze_external_input == 0.0);
  CHECK(at(undefined, "approximation_error_J").is_null());
  CHECK(store.supervisory[0].consecutive_frozen_ticks == 1);
  CHECK(store.supervisory[0].forces_stale);
  CHECK_FALSE(store.supervisory[0].last_path_length_valid);
}

TEST_CASE(
    "muscle supervisor: an invalid projection enters the inert tier, which holds everything "
    "and outputs nothing until a valid projection resumes it (D20, D21 (4), r008 witness)") {
  const JsonValue& inert = at(at(witnesses(), "undefined_and_inert"), "inert");
  LineStore store;
  r008_store(store);
  for (u64 k = 1; k <= 8; ++k)
    CHECK_FALSE(step(store, k, 1.2, k_still));
  LineState held{};
  save(store, 0, held);
  // Tick 9's projection: the path is shorter than the tendon's slack.
  CHECK_FALSE(step(store, 9, 0.5, Ramp{k_short, k_short}));
  auto events = tick_events(store, 0);
  REQUIRE(events.size() == 1);
  CHECK(events[0].kind == ValidationKind::TierInert);
  CHECK(events[0].failed_test == ValidationTest::RigidProjectionInvalid);
  CHECK(events[0].failed_value == doctest::Approx(k_short - 0.05));
  CHECK(events[0].tier_before == LineTier::Compliant);
  CHECK(events[0].tier_after == LineTier::Animation);
  for (u64 k = 9; k <= 11; ++k) {
    CAPTURE(k);
    if (k > 9) {
      CHECK_FALSE(step(store, k, 0.5, Ramp{k_short, k_short}));
      CHECK(tick_events(store, 0).empty());
    }
    CHECK(store.supervisory[0].tier == LineTier::Animation);
    CHECK_FALSE(has_force_output(store, 0));
    CHECK(output_force(store, 0) == 0.0);
    const TickWork& w = store.work[0];
    CHECK(text(inert, "status") == "Inert");
    CHECK(w.freeze_status == FreezeAccountingStatus::Inert);
    CHECK(w.freeze_external_input == num(inert, "external_input_J"));
    CHECK(w.freeze_external_input_valid);
    CHECK_FALSE(w.mechanically_integrated);
    // Activation and fiber length retained; nothing integrates.
    CHECK(bits(store.activation[0]) == bits(held.activation));
    CHECK(bits(store.fiber_length[0]) == bits(held.fiber_length));
    CHECK(store.last_commit_tick[0] == 0);
    CHECK(store.supervisory[0].last_attempt_tick == k);
  }
  // Tick 12: the path is back; the projection at its start resumes tier 1 with the jump from the
  // last committed elastic energy, and the tick integrates one nominal rigid tick.
  REQUIRE(step(store, 12, 0.5, k_still));
  events = tick_events(store, 0);
  REQUIRE(events.size() == 1);
  CHECK(events[0].kind == ValidationKind::TierResumed);
  CHECK(events[0].tier_before == LineTier::Animation);
  CHECK(events[0].tier_after == LineTier::Rigid);
  double l = 0.0;
  double passive = 0.0;
  Failure f{};
  REQUIRE(rigid_projection(store.law[0], k_l0, l, f));
  REQUIRE(passive_energy(store.law[0], l, passive, f));
  const double last = held.committed_forces.passive_energy + held.committed_forces.tendon_energy;
  CHECK(events[0].transition_elastic_jump == passive - last);
  CHECK(store.work[0].transition_elastic_jump == events[0].transition_elastic_jump);
  CHECK(store.supervisory[0].consecutive_rigid_success_ticks == 1);
  CHECK(store.last_commit_tick[0] == 12);
  CHECK(output_force(store, 0) > 0.0);
  // A rigid tick whose path leaves the rigid geometry by its end goes inert at once, with no force.
  CHECK_FALSE(step(store, 13, 0.5, Ramp{k_l0, k_short}));
  events = tick_events(store, 0);
  REQUIRE(events.size() == 1);
  CHECK(events[0].kind == ValidationKind::TierInert);
  CHECK(events[0].tier_before == LineTier::Rigid);
  CHECK(store.work[0].freeze_status == FreezeAccountingStatus::Inert);
  CHECK(output_force(store, 0) == 0.0);
  CHECK(store.last_commit_tick[0] == 12);
}

TEST_CASE(
    "muscle supervisor: the rigid tier's activation is tier 0's, its force is the fiber's "
    "along the line, and its failures are events") {
  // A tick past startup on a taut constant path has no crossing, so tier 0's activation over it is
  // eight RK4 steps of H: the rigid tier's, to the bit.
  LineStore store;
  store.reserve(1);
  u32 i = 0;
  REQUIRE(store.add(r008_spec(0), i));
  Failure f{};
  REQUIRE(initialize_static(store, i, 0, 0.0, k_l0, f));
  const double e[1] = {0.8};
  const AnalyticPath p[1] = {{AnalyticPath::Kind::Constant, k_l0, 0.0, 0.0, 0.0}};
  REQUIRE(tick(store, 1, std::span<const double>(e, 1), std::span<const AnalyticPath>(p, 1)) == 0);
  const double a1 = store.activation[i];
  REQUIRE(tick(store, 2, std::span<const double>(e, 1), std::span<const AnalyticPath>(p, 1)) == 0);
  REQUIRE(store.counters[i].events == 0);
  Counters n{};
  double a2 = 0.0;
  REQUIRE(activation_over_tick(store.law[i], a1, 0.8, a2, n, f));
  CHECK(bits(a2) == bits(store.activation[i]));

  // The force: (F_a + F_p + F_d) cos at the projected state, and the compliant tendon at the
  // projection is at its slack length.
  const LawRow& law = store.law[i];
  double l = 0.0;
  REQUIRE(rigid_projection(law, k_l0, l, f));
  Cache c{};
  REQUIRE(rigid_cache(law, 0.6, l, 0.2, c, f));
  const double cosine = std::sqrt(l * l - law.height * law.height) / l;
  CHECK(c.tendon_force ==
        doctest::Approx((c.active_force + c.passive_force + c.damping_force) * cosine)
            .epsilon(1e-15));
  CHECK(c.tendon_energy == 0.0);
  Tendon t{};
  REQUIRE(tendon(law, k_l0 - std::sqrt(l * l - law.height * law.height), t, f));
  CHECK(std::fabs(t.force) <= 1e-9);

  // A path too fast for the fiber's velocity domain fails the rigid tick with the test named.
  LineStore fast;
  r008_store(fast);
  degrade(fast);
  CHECK_FALSE(step(fast, 10, 0.0, Ramp{k_l0, k_l0 + 0.03}));
  const auto events = tick_events(fast, 0);
  REQUIRE(events.size() == 1);
  CHECK(events[0].kind == ValidationKind::DomainFailure);
  CHECK(events[0].failed_test == ValidationTest::NormalizedVelocityOutOfRange);
  CHECK(fast.supervisory[0].tier == LineTier::Rigid);
  CHECK(fast.supervisory[0].forces_stale);
  CHECK(fast.supervisory[0].consecutive_rigid_success_ticks == 0);
  CHECK(fast.work[0].freeze_status == FreezeAccountingStatus::Valid);
  CHECK(fast.work[0].freeze_elastic_change == 0.0);  // a rigid tendon stores nothing
}

namespace {

// Four scripts over 80 ticks that between them visit every tier and transition: 0 degrades,
// rides tier 1, goes inert, resumes, is promoted and runs compliant on a moving path; 1 runs
// compliant throughout under a step of excitation; 2 degrades into the inert tier and stays; 3
// freezes once on a moving path and resumes.
constexpr u64 k_script_ticks = 80;

void script(u32 which, u64 k, double& u, Ramp& path) {
  const double t0 = static_cast<double>(k - 1) / 60.0;
  const double t1 = static_cast<double>(k) / 60.0;
  u = 0.3;
  path = k_still;
  switch (which) {
    case 0:
      if (k <= 8)
        u = 1.2;
      else if (k <= 11)
        u = 0.5;
      else if (k <= 13)
        path = Ramp{k_short, k_short};
      else if (k >= 77)
        path = Ramp{k_l0 + 0.002 * (t0 - 76.0 / 60.0), k_l0 + 0.002 * (t1 - 76.0 / 60.0)};
      return;
    case 1: u = k < 20 ? 0.0 : 0.8; return;
    case 2:
      if (k <= 8)
        u = 1.2;
      else
        path = Ramp{k_short, k_short};
      return;
    default:
      path = Ramp{k_l0 + 0.0006 * t0, k_l0 + 0.0006 * t1};
      if (k == 3) u = 1.2;
      return;
  }
}

void run_script_tick(LineStore& store, std::span<const u32> scripts, u64 k) {
  double u[4] = {};
  Ramp paths[4] = {};
  for (usize j = 0; j < scripts.size(); ++j)
    script(scripts[j], k, u[j], paths[j]);
  supervise<Ramp>(store, k, std::span<const double>(u, scripts.size()),
                  std::span<const Ramp>(paths, scripts.size()));
}

}  // namespace

TEST_CASE("muscle supervisor: a restore mid-sequence reproduces every later tick's bytes") {
  for (const u64 at_tick : {u64{5}, u64{8}, u64{9}, u64{12}, u64{20}, u64{73}, u64{74}}) {
    CAPTURE(at_tick);
    LineStore store;
    r008_store(store);
    const u32 mine[1] = {0};
    for (u64 k = 1; k <= at_tick; ++k)
      run_script_tick(store, mine, k);
    LineStore restarted;
    restarted.reserve(1);
    u32 j = 0;
    REQUIRE(restarted.add(r008_spec(0), j));
    restart(store, 0, restarted, j);
    for (u64 k = at_tick + 1; k <= k_script_ticks; ++k) {
      CAPTURE(k);
      run_script_tick(store, mine, k);
      run_script_tick(restarted, mine, k);
      REQUIRE(hash_state(store, 0) == hash_state(restarted, 0));
    }
  }
}

TEST_CASE(
    "muscle supervisor: a batch in different tiers equals each alone, and the scripted "
    "sequence's hash is pinned") {
  const u32 all[4] = {0, 1, 2, 3};
  LineStore batch;
  r008_store(batch, 4);
  LineStore alone[4];
  for (u32 s = 0; s < 4; ++s)
    r008_store(alone[s]);
  u64 h = k_hash_seed;
  u32 tiers_seen = 0;
  u32 kinds_seen = 0;
  for (u64 k = 1; k <= k_script_ticks; ++k) {
    CAPTURE(k);
    run_script_tick(batch, all, k);
    for (u32 s = 0; s < 4; ++s) {
      const u32 mine[1] = {s};
      run_script_tick(alone[s], mine, k);
      REQUIRE(hash_state(batch, s) == hash_state(alone[s], 0));
      h = hash_actuator(h, batch, s);
      tiers_seen |= 1u << static_cast<u32>(batch.supervisory[s].tier);
      for (const ValidationEvent& e : tick_events(batch, s))
        kinds_seen |= 1u << static_cast<u32>(e.kind);
    }
  }
  // The script visits every tier and every transition the supervisor makes.
  CHECK(tiers_seen == 0b111u);
  for (const ValidationKind kind :
       {ValidationKind::DomainFailure, ValidationKind::TierDegraded, ValidationKind::TierChanged,
        ValidationKind::TierInert, ValidationKind::TierResumed, ValidationKind::PromotionScheduled,
        ValidationKind::TierPromoted})
    CHECK((kinds_seen & (1u << static_cast<u32>(kind))) != 0u);
  CHECK(batch.supervisory[0].tier == LineTier::Compliant);
  CHECK(batch.supervisory[2].tier == LineTier::Animation);
  std::printf("muscle supervisor: scripted sequence hash 0x%016llx\n",
              static_cast<unsigned long long>(h));
  // Taken on MSVC (msvc-debug); reproduced by GCC 13 (release) and Clang 18 (debug) in the Linux
  // containers, as the corpus's hash is.
  constexpr u64 k_pinned = 0xa5875ef1adbd7c50ull;
  CHECK(h == k_pinned);
}

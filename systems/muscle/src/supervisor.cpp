#include <systems/muscle/supervisor.h>

#include <cmath>

namespace engine::muscle {

bool rigid_projection(const LawRow& p, double path_length, double& l, Failure& f) noexcept {
  if (!std::isfinite(path_length) || path_length <= 0.0)
    return fail(f, ValidationTest::PathSampleInvalid, path_length);
  const double belly = path_length - p.slack;
  if (!(belly > 0.0)) return fail(f, ValidationTest::RigidProjectionInvalid, belly);
  const double length = std::sqrt(belly * belly + p.height * p.height);
  Geometry g{};
  if (!geometry(p, length, g, f)) return false;
  l = length;
  return true;
}

bool rigid_cache(const LawRow& p, double a, double l, double v, Cache& out, Failure& f) noexcept {
  // length_terms() with the tendon taken out: the fiber side in the law's order of domain tests,
  // then velocity_terms(), whose residual against a zero tendon force is (F_a + F_p + F_d) cos.
  LengthTerms t{};
  if (!geometry(p, l, t.geometry, f)) return false;
  if (!bounded(a, 0.0, 1.0)) return fail(f, ValidationTest::ActivationOutOfRange, a);
  double x = 0.0;
  if (!normalized_fiber_length(p, l, x, f)) return false;
  t.tendon = Tendon{0.0, 0.0};
  t.activation_length = p.force_scale * a * active_length(x);
  t.passive_force = p.force_scale * passive_length(p, x);
  Forces forces{};
  if (!velocity_terms(p, t, v, forces, f)) return false;
  double energy = 0.0;
  if (!passive_energy(p, l, energy, f)) return false;
  out.tendon_force = forces.residual;
  out.active_force = forces.active;
  out.passive_force = forces.passive;
  out.damping_force = forces.damping;
  out.passive_energy = energy;
  out.tendon_energy = 0.0;
  return true;
}

bool activation_over_tick(const LawRow& p, double a, double u, double& out, Counters& n,
                          Failure& f) noexcept {
  double state = a;
  for (u32 j = 0; j < k_parents; ++j) {
    double k1 = 0.0;
    if (!activation_rate(p, state, u, k1, f)) return false;
    double next = 0.0;
    if (!rk4_activation(p, state, k1, k_parent_duration, u, next, f)) return false;
    n.rate_evaluations += 4;
    state = next;
  }
  out = state;
  return true;
}

ValidationKind failure_kind(ValidationTest test) noexcept {
  switch (test) {
    case ValidationTest::StageBracketEmpty:
    case ValidationTest::StageRootNotBracketed:
    case ValidationTest::StageRootNotConverged:
    case ValidationTest::EquilibriumNotBracketed:
    case ValidationTest::EquilibriumNotConverged:
    case ValidationTest::StaticIntervalEmpty:
    case ValidationTest::StaticRootNotBracketed:
    case ValidationTest::StaticRootNotConverged:
    case ValidationTest::StaticSlopeNotRestoring:
    case ValidationTest::ResidualNotANumber:
    case ValidationTest::EventUnresolved:
    case ValidationTest::EventTangentUnresolved:
    case ValidationTest::EventLocationUnresolved:
    case ValidationTest::EventBudgetExceeded: return ValidationKind::RootFailure;
    default: return ValidationKind::DomainFailure;
  }
}

namespace {

// The next of actuator i's event slots, filled with the identities, the tick's inputs and the
// supervisory state as it stands after the change the event reports. A tick emits at most
// `k_event_slots` by construction; a fifth would overwrite the fourth rather than another
// actuator's slots.
ValidationEvent& emit(LineStore& store, u32 i, u64 tick, double u, const PathEnds& ends,
                      ValidationKind kind, LineTier before, LineTier requested) noexcept {
  u8& count = store.event_count[i];
  ENGINE_ASSERT(count < k_event_slots,
                "muscle: a tick emitted more events than an actuator's slots");
  const u32 at = count < k_event_slots ? count++ : k_event_slots - 1;
  ValidationEvent& e = store.events[static_cast<usize>(i) * k_event_slots + at];
  const Supervisory& s = store.supervisory[i];
  e = ValidationEvent{};
  e.actuator_id = store.actuator_id[i];
  e.path_id = store.path_id[i];
  e.parameter_sha256 = store.parameter_sha256[i];
  e.scheduler_tick = tick;
  e.last_commit_tick = store.last_commit_tick[i];
  e.kind = kind;
  e.excitation = u;
  e.path_length = ends.length0;
  e.path_velocity = ends.rate0;
  e.forces_stale = s.forces_stale;
  e.consecutive_frozen_ticks = s.consecutive_frozen_ticks;
  e.tier_before = before;
  e.tier_after = s.tier;
  e.requested_tier = requested;
  e.pending_transition = s.pending_transition;
  e.consecutive_rigid_success_ticks = s.consecutive_rigid_success_ticks;
  e.path_inputs_finite = ends.finite;
  return e;
}

void failed(ValidationEvent& e, const Failure& f) noexcept {
  e.failed_test = f.test;
  e.failed_value = f.value;
}

// Which of a path's ends is invalid, as the failure that names it.
bool path_failure(const PathEnds& ends, Failure& f) noexcept {
  if (!std::isfinite(ends.length0) || ends.length0 <= 0.0)
    return fail(f, ValidationTest::PathSampleInvalid, ends.length0);
  if (!std::isfinite(ends.rate0)) return fail(f, ValidationTest::PathSampleInvalid, ends.rate0);
  if (!std::isfinite(ends.length1) || ends.length1 <= 0.0)
    return fail(f, ValidationTest::PathSampleInvalid, ends.length1);
  return fail(f, ValidationTest::PathSampleInvalid, ends.rate1);
}

// The rigid projection at the tick's start, from tier 0 (the pending ToRigid of a degradation) or
// tier 2 (an inert actuator's resume). A valid projection installs the projected fiber length and
// the rigid cache at zero fiber velocity and books the jump E_after - E_before: the passive
// energy's change and the tendon energy the rigid tendon discards. An invalid one makes a degrading
// actuator inert and leaves an inert one as it is.
void enter_rigid(LineStore& store, u32 i, u64 tick, double u, const PathEnds& ends,
                 LineTier from) noexcept {
  Supervisory& s = store.supervisory[i];
  const LawRow& p = store.law[i];
  Failure f{};
  double l = 0.0;
  Cache c{};
  const bool ok =
      rigid_projection(p, ends.length0, l, f) && rigid_cache(p, store.activation[i], l, 0.0, c, f);
  if (!ok) {
    if (from == LineTier::Animation) return;
    s.tier = LineTier::Animation;
    s.pending_transition = PendingTransition::None;
    s.consecutive_rigid_success_ticks = 0;
    failed(emit(store, i, tick, u, ends, ValidationKind::TierInert, from, LineTier::Rigid), f);
    return;
  }
  const double before = elastic_energy(store.cache[i]);
  store.fiber_length[i] = l;
  store.cache[i] = c;
  const double jump = elastic_energy(c) - before;
  s.tier = LineTier::Rigid;
  s.pending_transition = PendingTransition::None;
  s.consecutive_rigid_success_ticks = 0;
  s.forces_stale = false;
  store.work[i].transition_elastic_jump += jump;
  const ValidationKind kind =
      from == LineTier::Compliant ? ValidationKind::TierChanged : ValidationKind::TierResumed;
  emit(store, i, tick, u, ends, kind, from, LineTier::Rigid).transition_elastic_jump = jump;
}

// The scheduled promotion at the tick's start: static_initial_length with the retained activation
// against the path's current length at zero fiber velocity, through `initialize_static`, which
// arms `startup_pending` (D24) and leaves the actuator unchanged when it fails.
void promote(LineStore& store, u32 i, u64 tick, double u, const PathEnds& ends) noexcept {
  Supervisory& s = store.supervisory[i];
  // A tick at or before the last commit fails its order test in the rigid tick; the promotion
  // waits for a tick that can commit rather than moving the committed tick backwards.
  if (tick <= store.last_commit_tick[i]) return;
  const double before = elastic_energy(store.cache[i]);
  Failure f{};
  if (!initialize_static(store, i, tick - 1, store.activation[i], ends.length0, f)) {
    s.pending_transition = PendingTransition::None;
    s.consecutive_rigid_success_ticks = 0;
    failed(emit(store, i, tick, u, ends, ValidationKind::PromotionRejected, LineTier::Rigid,
                LineTier::Compliant),
           f);
    return;
  }
  const double jump = elastic_energy(store.cache[i]) - before;
  s.tier = LineTier::Compliant;
  s.pending_transition = PendingTransition::None;
  s.consecutive_rigid_success_ticks = 0;
  s.consecutive_frozen_ticks = 0;
  s.forces_stale = false;
  store.work[i].transition_elastic_jump += jump;
  emit(store, i, tick, u, ends, ValidationKind::TierPromoted, LineTier::Rigid, LineTier::Compliant)
      .transition_elastic_jump = jump;
}

// A frozen tick's ledger (D20, D21 (4)): with the fiber held at the retained l_f while the path
// moves from L_start to L_end, the stale force's work F_stale (L_end - L_start) against the change
// of the elastic energy the path's motion implies, U_t(L_end - projected(l_f)) - U_t(L_start -
// projected(l_f)), and their signed difference. The rigid tier's tendon stores nothing, so there
// the change is zero and the whole work is the error. A path whose ends are not finite, or whose
// energy at the held fiber is outside the law's domain, is UndefinedPath with its own event: the
// stale work is kept only when both ends are finite, and no error is claimed.
void freeze_ledger(LineStore& store, u32 i, u64 tick, double u, const PathEnds& ends,
                   bool rigid) noexcept {
  TickWork& w = store.work[i];
  const LawRow& p = store.law[i];
  const double l = store.fiber_length[i];
  w.freeze_path_start = ends.length0;
  w.freeze_path_end = ends.length1;
  const bool both = std::isfinite(ends.length0) && std::isfinite(ends.length1);
  if (both) {
    w.freeze_external_input = store.cache[i].tendon_force * (ends.length1 - ends.length0);
    w.freeze_external_input_valid = true;
  }
  Failure f{};
  double start = 0.0;
  double end = 0.0;
  double passive = 0.0;
  bool ok = both || fail(f, ValidationTest::PathSampleInvalid,
                         std::isfinite(ends.length0) ? ends.length1 : ends.length0);
  if (ok && rigid) {
    ok = passive_energy(p, l, passive, f);
  } else if (ok) {
    Geometry g{};
    Tendon t0{};
    Tendon t1{};
    ok = geometry(p, l, g, f) && tendon(p, ends.length0 - g.projected, t0, f) &&
         tendon(p, ends.length1 - g.projected, t1, f) && passive_energy(p, l, passive, f);
    start = t0.energy;
    end = t1.energy;
  }
  if (!ok) {
    w.freeze_status = FreezeAccountingStatus::UndefinedPath;
    const LineTier tier = store.supervisory[i].tier;
    failed(emit(store, i, tick, u, ends, ValidationKind::FreezeAccountingUndefined, tier, tier), f);
    return;
  }
  w.freeze_status = FreezeAccountingStatus::Valid;
  w.freeze_elastic_change = end - start;
  w.freeze_approximation_error = w.freeze_external_input - w.freeze_elastic_change;
  w.resume_initial_elastic = passive + end;
}

void book_work(TickWork& w, const double (&work)[k_work_channels]) noexcept {
  w.external_input = work[k_work_external];
  w.active_output = work[k_work_active_output];
  w.dissipated = work[k_work_dissipation];
  w.active_throughput = work[k_work_active_throughput];
  w.external_throughput = work[k_work_external_throughput];
  w.active_net_on = -work[k_work_active_output];
  w.mechanically_integrated = true;
}

}  // namespace

LineTier begin_supervised_tick(LineStore& store, u32 i, u64 tick, double excitation,
                               const PathEnds& ends) noexcept {
  TickOutcome& o = store.outcome[i];
  o = TickOutcome{};
  o.tick = tick;
  o.excitation = excitation;
  store.counters[i] = Counters{};
  TickWork& w = store.work[i];
  w = TickWork{};
  w.actuator_id = store.actuator_id[i];
  w.scheduler_tick = tick;
  store.event_count[i] = 0;
  Supervisory& s = store.supervisory[i];
  s.last_attempt_tick = tick;
  if (s.tier == LineTier::Compliant && s.pending_transition == PendingTransition::ToRigid) {
    enter_rigid(store, i, tick, excitation, ends, LineTier::Compliant);
  } else if (s.tier == LineTier::Rigid && s.pending_transition == PendingTransition::ToCompliant) {
    promote(store, i, tick, excitation, ends);
  } else if (s.tier == LineTier::Animation) {
    enter_rigid(store, i, tick, excitation, ends, LineTier::Animation);
  }
  return s.tier;
}

void finish_compliant_tick(LineStore& store, u32 i, u64 tick, double excitation, bool committed,
                           const PathEnds& ends) noexcept {
  Supervisory& s = store.supervisory[i];
  TickWork& w = store.work[i];
  const TickOutcome& o = store.outcome[i];
  if (committed) {
    s.forces_stale = false;
    s.consecutive_frozen_ticks = 0;
    book_work(w, o.work);
    return;
  }
  const Failure f{o.failed_test, o.failed_value};
  // An actuator that was never initialised has no committed state to hold, no stale force and
  // nothing to degrade from: the failure is reported and nothing else changes.
  if (store.initialized[i] == 0) {
    failed(emit(store, i, tick, excitation, ends, ValidationKind::DomainFailure,
                LineTier::Compliant, LineTier::Compliant),
           f);
    return;
  }
  s.forces_stale = true;
  ++s.consecutive_frozen_ticks;
  failed(emit(store, i, tick, excitation, ends, failure_kind(f.test), LineTier::Compliant,
              LineTier::Compliant),
         f);
  freeze_ledger(store, i, tick, excitation, ends, false);
}

bool rigid_tick(LineStore& store, u32 i, u64 tick, double excitation,
                const PathEnds& ends) noexcept {
  Supervisory& s = store.supervisory[i];
  TickWork& w = store.work[i];
  TickOutcome& o = store.outcome[i];
  const LawRow& p = store.law[i];
  Failure f{};
  double l1 = 0.0;
  if (tick <= store.last_commit_tick[i]) {
    fail(f, ValidationTest::TickOrder, static_cast<double>(tick));
  } else if (!ends.start_ok || !ends.end_ok || !rigid_projection(p, ends.length1, l1, f)) {
    // The path has left the rigid tier's geometry: the actuator is inert from this tick, with its
    // activation and fiber length retained and no force output (D20).
    if (f.test == ValidationTest::None) path_failure(ends, f);
    s.tier = LineTier::Animation;
    s.consecutive_rigid_success_ticks = 0;
    s.forces_stale = true;
    failed(emit(store, i, tick, excitation, ends, ValidationKind::TierInert, LineTier::Rigid,
                LineTier::Rigid),
           f);
    inert_tick(store, i);
    return false;
  } else {
    // One step: the activation over the tick's eight parents, the fiber at the projection of the
    // path's end, its velocity the projection's change over the tick, the forces at that state.
    const double v = (l1 - store.fiber_length[i]) / k_tick_duration;
    double a1 = 0.0;
    Cache c{};
    if (activation_over_tick(p, store.activation[i], excitation, a1, store.counters[i], f) &&
        rigid_cache(p, a1, l1, v, c, f)) {
      store.activation[i] = a1;
      store.fiber_length[i] = l1;
      store.cache[i] = c;
      store.last_commit_tick[i] = tick;
      s.forces_stale = false;
      if (s.consecutive_rigid_success_ticks < k_promote_after) ++s.consecutive_rigid_success_ticks;
      // The ledger by the one-point rule at the tick's end state: first order in the step, and not
      // a qualified ledger (the corpus qualified tier 0 only).
      double work[k_work_channels] = {};
      work[k_work_external] = c.tendon_force * (ends.length1 - ends.length0);
      work[k_work_active_output] = -c.active_force * v * k_tick_duration;
      work[k_work_dissipation] = c.damping_force * v * k_tick_duration;
      work[k_work_active_throughput] = std::fabs(work[k_work_active_output]);
      work[k_work_external_throughput] = std::fabs(work[k_work_external]);
      for (u32 k = 0; k < k_work_channels; ++k)
        o.work[k] = work[k];
      o.committed = true;
      book_work(w, work);
      return true;
    }
  }
  o.failed_test = f.test;
  o.failed_value = f.value;
  s.forces_stale = true;
  s.consecutive_rigid_success_ticks = 0;
  failed(emit(store, i, tick, excitation, ends, failure_kind(f.test), LineTier::Rigid,
              LineTier::Rigid),
         f);
  freeze_ledger(store, i, tick, excitation, ends, true);
  return false;
}

void inert_tick(LineStore& store, u32 i) noexcept {
  TickWork& w = store.work[i];
  w.freeze_status = FreezeAccountingStatus::Inert;
  w.freeze_external_input = 0.0;
  w.freeze_external_input_valid = true;
  w.mechanically_integrated = false;
}

void end_supervised_tick(LineStore& store, u32 i, u64 tick, double excitation, bool committed,
                         const PathEnds& ends) noexcept {
  Supervisory& s = store.supervisory[i];
  if (s.tier == LineTier::Compliant && !committed && store.initialized[i] != 0 &&
      s.pending_transition == PendingTransition::None &&
      s.consecutive_frozen_ticks >= k_degrade_after) {
    // D20: a decision, while the stale tier-0 force is still this tick's output; the projection
    // is the next tick's start.
    s.pending_transition = PendingTransition::ToRigid;
    emit(store, i, tick, excitation, ends, ValidationKind::TierDegraded, LineTier::Compliant,
         LineTier::Rigid);
  } else if (s.tier == LineTier::Rigid && committed &&
             s.pending_transition == PendingTransition::None &&
             s.consecutive_rigid_success_ticks >= k_promote_after) {
    s.pending_transition = PendingTransition::ToCompliant;
    emit(store, i, tick, excitation, ends, ValidationKind::PromotionScheduled, LineTier::Rigid,
         LineTier::Compliant);
  }
  s.last_path_length = ends.length1;
  s.last_path_length_valid = ends.end_ok;
}

}  // namespace engine::muscle

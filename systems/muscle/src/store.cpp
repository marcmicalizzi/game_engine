#include <systems/muscle/muscle.h>
#include <systems/muscle/store.h>

#include <cmath>

namespace engine::muscle {

void LineStore::reserve(u32 capacity) {
  capacity_ = capacity;
  law.reserve(capacity);
  last_commit_tick.reserve(capacity);
  activation.reserve(capacity);
  fiber_length.reserve(capacity);
  startup_pending.reserve(capacity);
  initialized.reserve(capacity);
  cache.reserve(capacity);
  actuator_id.reserve(capacity);
  path_id.reserve(capacity);
  parameter_sha256.reserve(capacity);
  supervisory.reserve(capacity);
  outcome.reserve(capacity);
  counters.reserve(capacity);
  work.reserve(capacity);
  events.reserve(static_cast<usize>(capacity) * k_event_slots);
  event_count.reserve(capacity);
}

bool LineStore::add(const ActuatorSpec& spec, u32& index) {
  if (size() >= capacity_) return false;
  index = size();
  law.push_back(make_law_row(spec.fiber, spec.series));
  last_commit_tick.push_back(0);
  activation.push_back(0.0);
  fiber_length.push_back(0.0);
  startup_pending.push_back(0);
  initialized.push_back(0);
  cache.push_back(Cache{});
  actuator_id.push_back(spec.actuator_id);
  path_id.push_back(spec.path_id);
  parameter_sha256.push_back(spec.parameter_sha256);
  supervisory.push_back(Supervisory{});
  outcome.push_back(TickOutcome{});
  counters.push_back(Counters{});
  TickWork w{};
  w.actuator_id = spec.actuator_id;
  work.push_back(w);
  for (u32 e = 0; e < k_event_slots; ++e)
    events.push_back(ValidationEvent{});
  event_count.push_back(0);
  return true;
}

bool static_initial_length(const LawRow& p, double a, double total, double& l, Counters& n,
                           Failure& f) noexcept {
  double lo = p.lmin;
  const double above = total - 1e-8;
  const double reach = std::sqrt(above * above + p.height * p.height);
  double hi = reach < p.lmax ? reach : p.lmax;
  if (hi <= lo || total <= 1e-8) return fail(f, ValidationTest::StaticIntervalEmpty, total);
  // Avoid huge tendon forces while retaining the supported fiber interval.
  if (total > p.cap) {
    const double over = total - p.cap;
    const double floor = std::sqrt(over * over + p.height * p.height);
    if (floor > lo) lo = floor;
  }
  hi = std::nextafter(hi, lo);
  auto residual = [&](double x, double& r, Failure& fl) noexcept {
    ++n.residuals;
    Forces forces{};
    if (!components(p, a, x, 0.0, total, forces, fl)) return false;
    r = forces.residual;
    return residual_is_number(r, x, fl);
  };
  double r_lo = 0.0;
  double r_hi = 0.0;
  if (!residual(lo, r_lo, f)) return false;
  if (!residual(hi, r_hi, f)) return false;
  if (r_lo * r_hi > 0.0) return fail(f, ValidationTest::StaticRootNotBracketed, r_lo);
  double answer = 0.0;
  switch (brent(residual, lo, hi, r_lo, r_hi, 1e-14, 1e-14, 100, answer, n.root_iterations, f)) {
    case RootStatus::Converged: break;
    case RootStatus::NotConverged: return fail(f, ValidationTest::StaticRootNotConverged, answer);
    default: return false;
  }
  const double eps = 1e-7;
  double r_plus = 0.0;
  double r_minus = 0.0;
  if (!residual(answer + eps, r_plus, f)) return false;
  if (!residual(answer - eps, r_minus, f)) return false;
  const double slope = (r_plus - r_minus) / (2.0 * eps);
  if (slope <= 0.0) return fail(f, ValidationTest::StaticSlopeNotRestoring, slope);
  l = answer;
  return true;
}

namespace {

// study.mechanical_sample at a static state: the cache at the equilibrium velocity.
bool static_cache(const LawRow& p, double a, double l, double total, Cache& out, Counters& n,
                  Failure& f) noexcept {
  double v = 0.0;
  if (!equilibrium_velocity(p, a, l, total, v, n, f)) return false;
  LengthTerms terms{};
  if (!length_terms(p, a, l, total, terms, f)) return false;
  Forces forces{};
  if (!velocity_terms(p, terms, v, forces, f)) return false;
  out.tendon_force = forces.tendon;
  out.active_force = forces.active;
  out.passive_force = forces.passive;
  out.damping_force = forces.damping;
  out.tendon_energy = terms.tendon.energy;
  return passive_energy(p, l, out.passive_energy, f);
}

}  // namespace

bool initialize_static(LineStore& store, u32 i, u64 tick, double a, double length,
                       Failure& f) noexcept {
  Counters n{};
  const LawRow& p = store.law[i];
  if (!bounded(a, 0.0, 1.0)) return fail(f, ValidationTest::ActivationOutOfRange, a);
  if (!std::isfinite(length) || length <= 0.0)
    return fail(f, ValidationTest::PathSampleInvalid, length);
  double l = 0.0;
  if (!static_initial_length(p, a, length, l, n, f)) return false;
  Cache c{};
  if (!static_cache(p, a, l, length, c, n, f)) return false;
  store.activation[i] = a;
  store.fiber_length[i] = l;
  store.cache[i] = c;
  store.last_commit_tick[i] = tick;
  store.startup_pending[i] = 1;
  store.initialized[i] = 1;
  return true;
}

void save(const LineStore& store, u32 i, LineState& out) noexcept {
  out = LineState{};
  out.actuator_id = store.actuator_id[i];
  out.parameter_sha256 = store.parameter_sha256[i];
  out.path_id = store.path_id[i];
  out.model_sha256 = k_model_sha256;
  out.last_commit_tick = store.last_commit_tick[i];
  out.activation = store.activation[i];
  out.fiber_length = store.fiber_length[i];
  const Cache& c = store.cache[i];
  out.committed_forces = ForceCache{c.tendon_force,  c.active_force,   c.passive_force,
                                    c.damping_force, c.passive_energy, c.tendon_energy};
  out.initialized = store.initialized[i] != 0;
  out.startup_pending = store.startup_pending[i] != 0;
  const Supervisory& s = store.supervisory[i];
  out.last_attempt_tick = s.last_attempt_tick;
  out.consecutive_frozen_ticks = s.consecutive_frozen_ticks;
  out.tier = s.tier;
  out.forces_stale = s.forces_stale;
  out.pending_transition = s.pending_transition;
  out.consecutive_rigid_success_ticks = s.consecutive_rigid_success_ticks;
  out.last_path_length = s.last_path_length;
  out.last_path_length_valid = s.last_path_length_valid;
}

RestoreStatus restore(LineStore& store, u32 i, const LineState& in) noexcept {
  // The record's law and parameter set must be the actuator's: the law row was derived from the
  // parameters given to add(), and a state integrated under another law is not this law's data.
  if (in.model_sha256 != k_model_sha256) return RestoreStatus::ModelUnrecognized;
  if (in.parameter_sha256 != store.parameter_sha256[i]) return RestoreStatus::ParameterSetMismatch;
  store.actuator_id[i] = in.actuator_id;
  store.path_id[i] = in.path_id;
  store.last_commit_tick[i] = in.last_commit_tick;
  store.activation[i] = in.activation;
  store.fiber_length[i] = in.fiber_length;
  const ForceCache& c = in.committed_forces;
  store.cache[i] = Cache{c.tendon_force,        c.active_fiber_force, c.passive_fiber_force,
                         c.damping_fiber_force, c.passive_energy,     c.tendon_energy};
  store.initialized[i] = in.initialized ? 1 : 0;
  store.startup_pending[i] = in.startup_pending ? 1 : 0;
  Supervisory& s = store.supervisory[i];
  s.last_attempt_tick = in.last_attempt_tick;
  s.consecutive_frozen_ticks = in.consecutive_frozen_ticks;
  s.tier = in.tier;
  s.forces_stale = in.forces_stale;
  s.pending_transition = in.pending_transition;
  s.consecutive_rigid_success_ticks = in.consecutive_rigid_success_ticks;
  s.last_path_length = in.last_path_length;
  s.last_path_length_valid = in.last_path_length_valid;
  return RestoreStatus::Restored;
}

}  // namespace engine::muscle

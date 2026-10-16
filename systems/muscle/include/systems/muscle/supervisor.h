#pragma once

// The line tier's supervisor (D20, D21 (4); ADR-0055; docs/subsystems/muscle.md, "The supervisor"):
// which tier each actuator integrates in, and what a failed tick leaves behind.
//
//   tier 0, Compliant  the stage-1 kernel (store.h, `tick_actuator`). A failed tick retains the
//                      committed mechanics and cache, outputs the stale cached force, books the
//                      freeze ledger and counts on the frozen streak; the next tick retries.
//   tier 1, Rigid      the tendon is rigid: the activation integrates over the tick's eight
//                      parents and the fiber is the path's projection at the tick's end.
//   tier 2, Animation  inert: nothing integrates, no force is output, the projection is checked.
//
//   the 8th frozen tier-0 tick      TierDegraded, pending ToRigid; the stale force still output
//   the next tick's start           the rigid projection: TierChanged and a tier-1 tick, or
//                                   TierInert and an inert one
//   an inert tick's start           the projection again: TierResumed and a tier-1 tick, or inert
//   the 60th rigid success          PromotionScheduled, pending ToCompliant
//   the next tick's start           static_initial_length: TierPromoted and a tier-0 tick from a
//                                   static start (startup_pending armed), or PromotionRejected,
//                                   the streak reset and a tier-1 tick
//
// Every transition books its signed elastic jump E_after - E_before on its event and on the tick's
// `TickWork::transition_elastic_jump`. Events and work go into the store's fixed per-actuator slots
// (`LineStore::work`, `events`, `event_count`), which a caller drains after the tick: nothing here
// allocates. The per-actuator phases are functions in supervisor.cpp; only the tier-0 call and the
// path's two samples are templates, so the path stays an inlined call (muscle.h, "paths").

#include <core/base/types.h>
#include <systems/muscle/law.h>
#include <systems/muscle/schedule.h>
#include <systems/muscle/store.h>

#include <cmath>
#include <schemas/muscle.h>
#include <span>

namespace engine::muscle {

// D20: the eighth consecutive frozen tier-0 tick degrades.
inline constexpr u32 k_degrade_after = 8;
// D21 (4): sixty consecutive successful tier-1 ticks schedule a promotion. The streak saturates.
inline constexpr u32 k_promote_after = 60;
// A tick, 1/60 s: eight parents of H, the same binary64 value as 8 * k_parent_duration.
inline constexpr double k_tick_duration = 8.0 * k_parent_duration;

// The path at tick k's two ends, (k-1)/60 and k/60: what the projection, the rigid tier's work and
// the freeze ledger read, and what the next tick's caller can difference for a rate.
struct PathEnds {
  double length0 = 0.0;  // m, at the tick's start
  double rate0 = 0.0;    // m/s
  double length1 = 0.0;  // m, at the tick's end
  double rate1 = 0.0;
  bool start_ok = false;  // length0 finite and positive, rate0 finite
  bool end_ok = false;    // the same at the end
  bool finite = false;    // all four finite (ValidationEvent::path_inputs_finite)
};

template <class Path>
inline PathEnds path_ends(const Path& path, u64 tick) noexcept {
  PathEnds e{};
  const double t0 = parent_start(tick, 0);
  path.sample(t0, t0, e.length0, e.rate0);
  path.sample(parent_start(tick, k_parents), t0, e.length1, e.rate1);
  e.start_ok = std::isfinite(e.length0) && e.length0 > 0.0 && std::isfinite(e.rate0);
  e.end_ok = std::isfinite(e.length1) && e.length1 > 0.0 && std::isfinite(e.rate1);
  e.finite = std::isfinite(e.length0) && std::isfinite(e.rate0) && std::isfinite(e.length1) &&
             std::isfinite(e.rate1);
  return e;
}

// The rigid tier's fiber length for path length L (D20, #304): belly = L - Ls, l = sqrt(belly^2 +
// h^2), held to the law's geometry (the normalized-length domain, the pennation height, the cosine
// floor). Fails with PathSampleInvalid, RigidProjectionInvalid or the geometry's test.
bool rigid_projection(const LawRow& p, double path_length, double& l, Failure& f) noexcept;

// The rigid tier's forces at (a, l, v): the law's active, passive and damping forces, the tendon
// force their sum times the pennation cosine (no tendon compliance, no implicit root), the passive
// energy, and no tendon energy.
bool rigid_cache(const LawRow& p, double a, double l, double v, Cache& out, Failure& f) noexcept;

// The activation over one tick under held excitation u: one RK4 step of H per parent, eight in all,
// which is the activation tier 0 integrates in a parent with no crossing and no startup pieces.
bool activation_over_tick(const LawRow& p, double a, double u, double& out, Counters& n,
                          Failure& f) noexcept;

// The event kind a failed tick's test reports: a root, a locator or a budget test is a
// RootFailure, anything else (the law's domain, the path, the order of ticks) a DomainFailure.
ValidationKind failure_kind(ValidationTest test) noexcept;

// ---- the per-actuator phases (supervisor.cpp) ---------------------------------------------------

// Resets actuator i's slots for tick k and applies a transition the last tick decided or an inert
// actuator's resume, at the tick's start. Returns the tier this tick integrates in.
LineTier begin_supervised_tick(LineStore& store, u32 i, u64 tick, double excitation,
                               const PathEnds& ends) noexcept;
// After a tier-0 tick: the streak, the stale flag, the work or the freeze ledger, the events.
void finish_compliant_tick(LineStore& store, u32 i, u64 tick, double excitation, bool committed,
                           const PathEnds& ends) noexcept;
// A tier-1 tick, whole. False when it did not commit (a failure, or an invalid projection that made
// the actuator inert, which `supervisory[i].tier` then says).
bool rigid_tick(LineStore& store, u32 i, u64 tick, double excitation,
                const PathEnds& ends) noexcept;
// A tier-2 tick: nothing integrates, no force is output, the ledger is not applicable.
void inert_tick(LineStore& store, u32 i) noexcept;
// The tick's end: the degradation and the promotion decisions, and the path endpoint kept.
void end_supervised_tick(LineStore& store, u32 i, u64 tick, double excitation, bool committed,
                         const PathEnds& ends) noexcept;

// One supervised tick of actuator i. True when the tick committed an integration (tier 0 or 1).
template <class Path, StageRoot R = k_stage_root>
bool supervise_actuator(LineStore& store, u32 i, u64 tick, double excitation,
                        const Path& path) noexcept {
  const PathEnds ends = path_ends(path, tick);
  bool committed = false;
  switch (begin_supervised_tick(store, i, tick, excitation, ends)) {
    case LineTier::Compliant:
      committed = tick_actuator<Path, R>(store, i, tick, excitation, path);
      finish_compliant_tick(store, i, tick, excitation, committed, ends);
      break;
    case LineTier::Rigid: committed = rigid_tick(store, i, tick, excitation, ends); break;
    case LineTier::Animation: inert_tick(store, i); break;
  }
  end_supervised_tick(store, i, tick, excitation, committed, ends);
  return committed;
}

// One supervised tick over every actuator of the store: actuator i reads `excitation[i]` and
// `paths[i]`. Returns the number of frozen ticks (a tier-0 or tier-1 tick that failed); an inert
// tick is not a failure. The outputs are `output_force`, `supervisory[i]`, `work[i]` and
// `tick_events(store, i)`.
template <class Path, StageRoot R = k_stage_root>
u32 supervise(LineStore& store, u64 tick, std::span<const double> excitation,
              std::span<const Path> paths) noexcept {
  const u32 count = store.size();
  ENGINE_ASSERT(excitation.size() >= count && paths.size() >= count,
                "muscle: an input per actuator");
  u32 frozen = 0;
  for (u32 i = 0; i < count; ++i) {
    const bool committed = supervise_actuator<Path, R>(store, i, tick, excitation[i], paths[i]);
    if (!committed && store.supervisory[i].tier != LineTier::Animation) ++frozen;
  }
  return frozen;
}

// Whether actuator i outputs a force: it is initialised and not inert.
inline bool has_force_output(const LineStore& store, u32 i) noexcept {
  return store.initialized[i] != 0 && store.supervisory[i].tier != LineTier::Animation;
}
// The tendon force actuator i outputs this tick: the committed cache's, stale when
// `supervisory[i].forces_stale`, and zero for an inert or uninitialised actuator.
inline double output_force(const LineStore& store, u32 i) noexcept {
  return has_force_output(store, i) ? store.cache[i].tendon_force : 0.0;
}
// The events actuator i's last supervised tick emitted, in order.
inline std::span<const ValidationEvent> tick_events(const LineStore& store, u32 i) noexcept {
  return {store.events.data() + static_cast<usize>(i) * k_event_slots, store.event_count[i]};
}

}  // namespace engine::muscle

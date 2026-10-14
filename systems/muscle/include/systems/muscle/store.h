#pragma once

// The line tier's batch API: a structure-of-arrays store of actuators and the tick over it
// (ADR-0055, docs/subsystems/muscle.md, "The store and the tick").
//
// The store is sized once, `reserve(capacity)`, and never allocates again: `add` refuses past the
// capacity, and the tick keeps everything else it needs — the stage states, the probe records, the
// tick's work, the parent samples — on the stack or in the store's fixed per-actuator slots. Each
// actuator's tick is a trial on locals that is committed into the store only when the whole tick
// succeeded, so a failure leaves the committed bytes untouched by never having written them.

#include <systems/muscle/law.h>
#include <systems/muscle/schedule.h>
#include <systems/muscle/scheme.h>

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>

#include <schemas/muscle.h>

#include <array>
#include <span>

namespace engine::muscle {

using Sha256 = std::array<u8, 32>;

// What `add` takes: the identities and the parameters (the validator's to check, validate()).
struct ActuatorSpec {
  Id128 actuator_id{};
  Id128 path_id{};
  Sha256 parameter_sha256{};
  FiberParameters fiber{};
  SeriesParameters series{};
};

// The supervisory half of the committed record: stage 2's, carried through save and restore.
struct Supervisory {
  u64 last_attempt_tick = 0;
  u32 consecutive_frozen_ticks = 0;
  LineTier tier = LineTier::Compliant;
  bool forces_stale = false;
  PendingTransition pending_transition = PendingTransition::None;
  u32 consecutive_rigid_success_ticks = 0;
  double last_path_length = 0.0;
  bool last_path_length_valid = false;
};

// One actuator's last tick: the work it committed (zero on a failure) and, on a failure, which
// test failed, the value it read, and where in the schedule.
struct TickOutcome {
  u64 tick = 0;
  double work[k_work_channels] = {0.0, 0.0, 0.0, 0.0, 0.0};
  ValidationTest failed_test = ValidationTest::None;
  double failed_value = 0.0;
  double excitation = 0.0;
  bool committed = false;
};

struct LineStore {
  void reserve(u32 capacity);
  u32 size() const noexcept { return law.size(); }
  u32 capacity() const noexcept { return capacity_; }

  // Adds an actuator, uninitialised; false when the store is full. The parameters must have
  // passed validate() (validator.h); `add_validated` there does both.
  bool add(const ActuatorSpec& spec, u32& index);

  // Hot, read per tick.
  Vector<LawRow> law;
  Vector<u64> last_commit_tick;
  Vector<double> activation;
  Vector<double> fiber_length;
  Vector<u8> startup_pending;
  Vector<u8> initialized;
  Vector<Cache> cache;
  // Cold: identities and stage 2's fields.
  Vector<Id128> actuator_id;
  Vector<Id128> path_id;
  Vector<Sha256> parameter_sha256;
  Vector<Supervisory> supervisory;
  // Per tick, fixed slots.
  Vector<TickOutcome> outcome;
  Vector<Counters> counters;

 private:
  u32 capacity_ = 0;
};

// compliant.static_initial_length: the fiber length at zero fiber velocity that balances the
// tendon for activation a and path length `total`, with a positive restoring slope. The bracket,
// the cap rule, the `nextafter`, Brent with xtol = rtol = 1e-14 and 100 iterations, and the slope
// check at +-1e-7 m are the reference's.
bool static_initial_length(const LawRow& p, double a, double total, double& l, Counters& n,
                           Failure& f) noexcept;

// Initialises actuator i at the static equilibrium for activation `a` against path length
// `length`, as of `tick` (its last committed tick): the length, the cache at the equilibrium
// velocity, and `startup_pending` armed. The only way to arm it (r010 review, "Mapping 4"). On a
// failure the actuator is unchanged.
bool initialize_static(LineStore& store, u32 i, u64 tick, double a, double length, Failure& f) noexcept;

// The committed record of actuator i, and back. `restore` keeps `startup_pending` as saved and
// refuses a record of another law (model id) or another parameter set than the actuator's.
enum class RestoreStatus : u8 { Restored, ModelUnrecognized, ParameterSetMismatch };
void save(const LineStore& store, u32 i, LineState& out) noexcept;
RestoreStatus restore(LineStore& store, u32 i, const LineState& in) noexcept;

// One tick over every actuator: actuator i reads `excitation[i]` (held for the tick) and
// `paths[i]`. A success commits the actuator's activation, fiber length, cache, tick and clears
// `startup_pending`; a failure commits nothing. `outcome[i]` says which. When `samples` is not
// empty it holds 8 entries an actuator, written for actuators that committed: the state and cache
// at each parent's end, for the corpus replay. Returns the number of actuators that failed.
template <class Path>
u32 tick(LineStore& store, u64 tick_index, std::span<const double> excitation,
         std::span<const Path> paths, std::span<ParentSample> samples = {}) noexcept {
  const u32 count = store.size();
  ENGINE_ASSERT(excitation.size() >= count && paths.size() >= count, "muscle: an input per actuator");
  ENGINE_ASSERT(samples.empty() || samples.size() >= static_cast<usize>(count) * k_parents,
                "muscle: eight samples per actuator");
  const double tick_start = parent_start(tick_index, 0);
  u32 failures = 0;
  for (u32 i = 0; i < count; ++i) {
    TickOutcome& outcome = store.outcome[i];
    outcome = TickOutcome{};
    outcome.tick = tick_index;
    outcome.excitation = excitation[i];
    Counters& n = store.counters[i];
    n = Counters{};
    Failure f{};
    TickTrial trial{};
    ParentSample local[k_parents] = {};
    bool ok = false;
    if (store.initialized[i] == 0) {
      fail(f, ValidationTest::NotInitialized, 0.0);
    } else if (tick_index <= store.last_commit_tick[i]) {
      fail(f, ValidationTest::TickOrder, static_cast<double>(tick_index));
    } else {
      LineProber<Path> prober{store.law[i], paths[i], tick_start, excitation[i], n};
      ok = run_tick(prober, tick_index, store.startup_pending[i] != 0, store.activation[i],
                    store.fiber_length[i], trial, n, f,
                    [&](u32 j, const State& s, const Cache& c) noexcept {
                      local[j] = ParentSample{parent_start(tick_index, j + 1), s.a, s.l, c};
                    });
    }
    if (!ok) {
      outcome.failed_test = f.test;
      outcome.failed_value = f.value;
      ++failures;
      continue;
    }
    store.activation[i] = trial.end.a;
    store.fiber_length[i] = trial.end.l;
    store.cache[i] = trial.cache;
    store.last_commit_tick[i] = tick_index;
    store.startup_pending[i] = 0;
    for (u32 c = 0; c < k_work_channels; ++c)
      outcome.work[c] = trial.work[c];
    outcome.committed = true;
    if (!samples.empty()) {
      for (u32 j = 0; j < k_parents; ++j)
        samples[static_cast<usize>(i) * k_parents + j] = local[j];
    }
  }
  return failures;
}

// The elastic energy of a cache: passive fiber energy plus tendon energy (the ledger's E).
inline double elastic_energy(const Cache& c) noexcept { return c.passive_energy + c.tendon_energy; }

// The trajectory's work residual R = W_ext + W_active_output - (E_end - E_0) - W_diss.
inline double work_residual(const double (&work)[k_work_channels], double e0, double e_end) noexcept {
  return work[k_work_external] + work[k_work_active_output] - (e_end - e0) - work[k_work_dissipation];
}

}  // namespace engine::muscle

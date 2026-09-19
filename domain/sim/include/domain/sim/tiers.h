#pragma once

// Simulation LOD tier assignment (docs/plan/05-simulation.md §5.4, ADR-0010).
//
// A tier is not a property of an entity; it is a function of the **observer set**. The tier of
// an entity is the minimum over observers of f(distance, importance, observer weight), which is
// the one formulation that generalizes: two players, a security camera, a quest marker and a
// dedicated server with no camera at all are all observers, and nothing in the simulation has to
// know which is "the player" ([05
// §5.12](../../../docs/plan/05-simulation.md#512-multiplayer-readiness) calls the same set interest
// management). A radius around a singleton camera would have to be unpicked for every one of those
// cases.
//
// Two mechanisms keep the function usable rather than merely correct:
//
//   * **Hysteresis.** Promotion tests the band boundary; demotion tests the boundary widened by
//     `hysteresis`. A crowd walking along a boundary therefore crosses once instead of
//     oscillating, and the cost of materializing is paid once per crossing.
//   * **Rate limits.** At most `max_promotions` and `max_demotions` entities change tier per
//     call, so a camera cut that puts 10,000 entities inside the LOD0 radius spreads its
//     materialization over ticks instead of dropping a frame. Candidates are ordered by score,
//     so the nearest promote first and the farthest demote first; the rest are deferred, not
//     forgotten.
//
// Determinism: `hashed`. Scores are computed per entity from the inputs alone, so the optional
// job-system split changes nothing, and the emitted changes are sorted by entity index rather
// than by the order candidates were selected in.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/jobs/job_system.h>
#include <core/math/math.h>

#include <span>

namespace engine::sim {

// The plan fixes four tiers (LOD0 immediate, LOD1 nearby, LOD2 loaded, LOD3 distant) and says
// the number is configurable per game; eight is the cap, so a tier fits in three bits next to a
// per-entity flag byte and `LodMask` stays a `u8`.
inline constexpr u32 k_max_tiers = 8;

// Positions and weights of everything the simulation is being run for. SoA because the scoring
// loop reads all positions and then all weights, and because an observer set is rebuilt from
// scratch every tick rather than edited.
class ObserverSet {
 public:
  void clear() noexcept;
  void add(Vec3 observer_position, f32 observer_weight);
  u32 size() const noexcept { return positions_.size(); }
  bool empty() const noexcept { return positions_.empty(); }
  Vec3 position(u32 index) const noexcept { return positions_[index]; }
  f32 weight(u32 index) const noexcept { return weights_[index]; }
  std::span<const Vec3> positions() const noexcept {
    return {positions_.data(), positions_.size()};
  }
  std::span<const f32> weights() const noexcept { return {weights_.data(), weights_.size()}; }

 private:
  Vector<Vec3> positions_;
  Vector<f32> weights_;
};

struct TierParams {
  // Tiers in use, 2..k_max_tiers. Tier 0 is the most detailed.
  u32 tier_count = 4;
  // boundaries[t] is the score below which an entity belongs at tier t; only the first
  // tier_count - 1 entries are read. Units are metres divided by (weight * importance).
  f32 boundaries[k_max_tiers] = {32.0f, 128.0f, 1024.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  // Demotion uses boundaries[t] * (1 + hysteresis). 0 disables hysteresis and invites thrash.
  f32 hysteresis = 0.15f;
  // Importance divides the distance, so a floor keeps an unimportant entity from scoring
  // infinity and a zero from dividing by zero.
  f32 min_importance = 0.0625f;
  u32 max_promotions = 64;
  u32 max_demotions = 256;
};

// One entity's tier change. `index` indexes the caller's SoA arrays, which is also how the
// caller finds the entity id: the tier system never sees one.
struct TierChange {
  u32 index = 0;
  u8 from = 0;
  u8 to = 0;
  // Named and zeroed rather than left to the compiler, because this type's *bytes* are the
  // contract: the module's test memcmps the change lists a run with no job system, one worker
  // and eight produce, and two bytes of tail padding carry whatever the allocator last left in
  // that slot. It reads as a determinism failure and is a reading of uninitialized memory —
  // found by the GCC RelWithDebInfo build, where the freed blocks the vector grew into were no
  // longer zero; the Clang debug build had been handing out fresh zeroed pages and hiding it.
  // The size table's 8/4 entry is unchanged: this occupies the padding, it does not add to it.
  u16 pad = 0;
};

struct TierStats {
  u32 evaluated = 0;
  u32 promoted = 0;
  u32 demoted = 0;
  u32 deferred_promotions = 0;  // wanted to promote, lost to the rate limit
  u32 deferred_demotions = 0;
};

// The SoA the caller owns. `tiers` is in/out: assign_tiers writes the accepted changes into it
// so the next call sees the state it left, which is what makes hysteresis work at all.
struct TierInput {
  std::span<const Vec3> positions;
  std::span<const f32> importance;
  std::span<u8> tiers;
};

class TierAssignment {
 public:
  TierAssignment() noexcept = default;
  explicit TierAssignment(jobs::JobSystem* value) noexcept : jobs_(value) {}
  ENGINE_NON_COPYABLE(TierAssignment);

  void set_job_system(jobs::JobSystem* value) noexcept { jobs_ = value; }
  jobs::JobSystem* job_system() const noexcept { return jobs_; }
  // Entities per job. Only a hint: the result never depends on it.
  void set_grain(u32 value) noexcept { grain_ = value != 0 ? value : 1u; }

  // Scores every entity, applies hysteresis and the rate limits, updates `entities.tiers`, and
  // appends the accepted changes to `changes` in ascending index order. `changes` is not
  // cleared, so several populations can accumulate into one list.
  TierStats assign_tiers(const TierInput& entities, const ObserverSet& observers,
                         const TierParams& params, Vector<TierChange>& changes);

  // The score of one entity: min over observers of distance / (weight * importance). Exposed
  // because a capability's own LOD policy has to be able to agree with this one.
  static f32 score(Vec3 position, f32 importance, const ObserverSet& observers,
                   const TierParams& params) noexcept;
  // The tier a score lands in, ignoring hysteresis and rate limits.
  static u8 tier_of(f32 value, const TierParams& params) noexcept;

  std::span<const f32> last_scores() const noexcept { return {scores_.data(), scores_.size()}; }

 private:
  struct Candidate {
    f32 value = 0.0f;
    u32 index = 0;
    u8 from = 0;
    u8 to = 0;
  };

  jobs::JobSystem* jobs_ = nullptr;
  u32 grain_ = 2048;
  Vector<f32> scores_;
  Vector<u8> bands_;  // tight tier in the low nibble, hysteresis-widened tier in the high one
  Vector<Candidate> promotions_;
  Vector<Candidate> demotions_;
  Vector<TierChange> accepted_;
};

}  // namespace engine::sim

#include <core/base/assert.h>
#include <domain/sim/tiers.h>

#include <algorithm>
#include <limits>

namespace engine::sim {

void ObserverSet::clear() noexcept {
  positions_.clear();
  weights_.clear();
}

void ObserverSet::add(WorldPos observer_position, f32 observer_weight) {
  positions_.push_back(observer_position);
  weights_.push_back(observer_weight > 0.0f ? observer_weight : 1.0f);
}

f32 TierAssignment::score(WorldPos position, f32 importance, const ObserverSet& observers,
                          const TierParams& params) noexcept {
  const u32 count = observers.size();
  if (count == 0) return std::numeric_limits<f32>::max();
  f32 scale = importance > params.min_importance ? importance : params.min_importance;
  if (!(scale > 0.0f)) scale = 1.0f;
  f32 best = std::numeric_limits<f32>::max();
  for (u32 i = 0; i < count; ++i) {
    const f32 weight = observers.weight(i);
    // The displacement and its length in f64, then the length as a float32: the one narrowing on
    // this path (tiers.h), so the distance a score is made of is within half a float step of the
    // true one wherever the two are. Narrowing the displacement first and taking its length in
    // float32 would round each axis and then the sum, two to three times that. (Measured
    // 2026-10-06, docs/subsystems/sim.md "Positions are f64": this costs the 100,000-entity pass a
    // tenth over the float32 one, and taking one root per entity instead of one per observer saved
    // none of it.)
    const f32 metres = static_cast<f32>(length(position - observers.position(i)));
    const f32 value = metres / (weight * scale);
    if (value < best) best = value;
  }
  return best;
}

u8 TierAssignment::tier_of(f32 value, const TierParams& params) noexcept {
  const u32 count = params.tier_count < 2u ? 2u : params.tier_count;
  const u32 last = (count < k_max_tiers ? count : k_max_tiers) - 1u;
  for (u32 tier = 0; tier < last; ++tier) {
    if (value < params.boundaries[tier]) return static_cast<u8>(tier);
  }
  return static_cast<u8>(last);
}

namespace {

// Both bands for one entity, packed into a byte: the tight tier in the low nibble, the widened
// one in the high nibble. Computed in the scoring pass rather than the serial selection pass,
// because the serial pass is the scaling limit and this is the largest thing that can leave it.
// One walk of the boundary table serves both bands, which is why it is not two calls.
u8 bands_of(f32 value, const TierParams& params) noexcept {
  const u32 count = params.tier_count < 2u ? 2u : params.tier_count;
  const u32 last = (count < k_max_tiers ? count : k_max_tiers) - 1u;
  const f32 widen = 1.0f + (params.hysteresis > 0.0f ? params.hysteresis : 0.0f);
  u32 tight = last;
  u32 wide = last;
  for (u32 tier = 0; tier < last; ++tier) {
    const f32 boundary = params.boundaries[tier];
    if (wide == last && value < boundary * widen) wide = tier;
    if (value < boundary) {
      // The widened boundary is never below the tight one, so by the time the tight tier is
      // found the widened one already is: there is nothing left to look for.
      tight = tier;
      break;
    }
  }
  return static_cast<u8>(tight | (wide << 4));
}

}  // namespace

TierStats TierAssignment::assign_tiers(const TierInput& entities, const ObserverSet& observers,
                                       const TierParams& params, Vector<TierChange>& changes) {
  TierStats stats;
  const u32 count = static_cast<u32>(entities.positions.size());
  ENGINE_ASSERT(entities.importance.size() == entities.positions.size(),
                "assign_tiers: importance and positions must be the same length");
  ENGINE_ASSERT(entities.tiers.size() == entities.positions.size(),
                "assign_tiers: tiers and positions must be the same length");
  if (count == 0) return stats;
  stats.evaluated = count;

  scores_.resize(count);
  bands_.resize(count);
  f32* scores = scores_.data();
  u8* bands = bands_.data();
  const WorldPos* positions = entities.positions.data();
  const f32* importance = entities.importance.data();

  // Scoring is a pure function of the inputs, entity by entity, so splitting it over workers
  // cannot change a single bit of the result; only how long it takes.
  if (jobs_ != nullptr && count >= grain_ * 2u) {
    jobs_->parallel_for(
        jobs::Pool::Performance, count, grain_,
        [scores, bands, positions, importance, &observers, &params](u32 begin, u32 end) {
          for (u32 i = begin; i < end; ++i) {
            scores[i] = score(positions[i], importance[i], observers, params);
            bands[i] = bands_of(scores[i], params);
          }
        });
  } else {
    for (u32 i = 0; i < count; ++i) {
      scores[i] = score(positions[i], importance[i], observers, params);
      bands[i] = bands_of(scores[i], params);
    }
  }

  promotions_.clear();
  demotions_.clear();
  accepted_.clear();

  const u32 tier_cap = (params.tier_count < 2u
                            ? 2u
                            : (params.tier_count < k_max_tiers ? params.tier_count : k_max_tiers)) -
                       1u;
  for (u32 i = 0; i < count; ++i) {
    u8 current = entities.tiers[i];
    // A tier the params no longer have (a game that shrank `tier_count` between ticks, or an
    // uninitialized byte) is repaired in place rather than trusted. It is not a transition, so
    // no change is emitted: nothing was ever materialized at a tier that does not exist.
    if (static_cast<u32>(current) > tier_cap) {
      current = static_cast<u8>(tier_cap);
      entities.tiers[i] = current;
    }
    const u8 tight = static_cast<u8>(bands[i] & 0x0Fu);
    if (tight < current) {
      promotions_.push_back(Candidate{scores[i], i, current, tight});
      continue;
    }
    const u8 wide = static_cast<u8>(bands[i] >> 4);
    if (wide > current) demotions_.push_back(Candidate{scores[i], i, current, wide});
  }

  stats.deferred_promotions = promotions_.size();
  stats.deferred_demotions = demotions_.size();

  // Nearest promotes first, farthest demotes first: under a rate limit the work that buys the
  // most fidelity per tick is the work that happens. Index breaks ties so the order is total.
  std::sort(promotions_.begin(), promotions_.end(), [](const Candidate& a, const Candidate& b) {
    if (a.value != b.value) return a.value < b.value;
    return a.index < b.index;
  });
  std::sort(demotions_.begin(), demotions_.end(), [](const Candidate& a, const Candidate& b) {
    if (a.value != b.value) return a.value > b.value;
    return a.index < b.index;
  });

  const u32 promote_count =
      promotions_.size() < params.max_promotions ? promotions_.size() : params.max_promotions;
  const u32 demote_count =
      demotions_.size() < params.max_demotions ? demotions_.size() : params.max_demotions;

  for (u32 i = 0; i < promote_count; ++i) {
    const Candidate& candidate = promotions_[i];
    entities.tiers[candidate.index] = candidate.to;
    accepted_.push_back(TierChange{candidate.index, candidate.from, candidate.to});
  }
  for (u32 i = 0; i < demote_count; ++i) {
    const Candidate& candidate = demotions_[i];
    entities.tiers[candidate.index] = candidate.to;
    accepted_.push_back(TierChange{candidate.index, candidate.from, candidate.to});
  }
  stats.promoted = promote_count;
  stats.demoted = demote_count;
  stats.deferred_promotions -= promote_count;
  stats.deferred_demotions -= demote_count;

  // Emitted in entity order, not in the order the rate limit accepted them, so a consumer that
  // walks the changes touches its own arrays forwards and two runs produce identical bytes.
  std::sort(accepted_.begin(), accepted_.end(),
            [](const TierChange& a, const TierChange& b) { return a.index < b.index; });
  for (u32 i = 0; i < accepted_.size(); ++i)
    changes.push_back(accepted_[i]);
  return stats;
}

}  // namespace engine::sim

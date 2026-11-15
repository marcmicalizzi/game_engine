#include <foundation/tunables/tunables.h>
#include <systems/animation/visibility.h>

#include <algorithm>

namespace engine::animation {

namespace {

// ---- tunables (ADR-0011) ----------------------------------------------------------------------
//
// The ceilings on the finest tiers (docs/subsystems/animation.md, "The budget"). Derived from what
// a tier costs an instance on the capability's bench (a 23-joint skeleton, msvc-release, one
// worker): 256 actors at LOD0 and 768 more at LOD1 are about 1.5 ms of a 60 Hz step, a tenth of a
// frame, on top of which the rest of a crowd at LOD2 and LOD3 costs tenths of a microsecond each.
// A game with a heavier rig or a lighter frame moves them.
tunables::Int lod_full_max{"animation.lod.full_max", 256, 0, 1 << 20,
                           "Actors a step may keep at LOD0; the rest held at LOD1 (0: no ceiling)"};
tunables::Int lod_skinned_max{
    "animation.lod.skinned_max", 1024, 0, 1 << 20,
    "Actors a step may keep skinned (LOD0 and LOD1); the rest held at LOD2 (0: no ceiling)"};

// How far past the widened boundary a held-back actor's score is put: far enough that a float's
// rounding in the tier code cannot leave it on the boundary, near enough that the margin is not a
// second band.
constexpr f32 k_hold_margin = 1.0625f;

u32 limit(u32 budget, u32 wanted) noexcept {
  return budget == 0 || budget > wanted ? wanted : budget;
}

}  // namespace

TierBudget tier_budget() noexcept {
  TierBudget out;
  out.full = static_cast<u32>(lod_full_max.get());
  out.skinned = static_cast<u32>(lod_skinned_max.get());
  return out;
}

TierBudgetStats cap_tiers(std::span<const f32> scores, std::span<const u8> tiers,
                          const sim::TierParams& params, const TierBudget& budget,
                          std::span<f32> importance, Vector<u32>& order) {
  TierBudgetStats stats;
  const u32 rows = static_cast<u32>(std::min({scores.size(), tiers.size(), importance.size()}));
  const f32 widen = 1.0f + (params.hysteresis > 0.0f ? params.hysteresis : 0.0f);
  const f32 full_boundary = params.boundaries[0];
  const f32 skinned_boundary = params.tier_count > 2 ? params.boundaries[1] : params.boundaries[0];
  // The band the tier code keeps a row in: the tight boundary to come in, the widened one to stay.
  const auto wants = [&](u32 row, f32 boundary, u8 finest_kept) {
    return scores[row] < (tiers[row] <= finest_kept ? boundary * widen : boundary);
  };
  // The rank: the score, an incumbent's divided by the widening (the same hysteresis, so an actor
  // that has the tier keeps it against a newcomer within the band), ties by row.
  const auto ranked_by = [&](u8 finest_kept) {
    return [&, finest_kept](u32 a, u32 b) {
      const f32 ka = tiers[a] <= finest_kept ? scores[a] / widen : scores[a];
      const f32 kb = tiers[b] <= finest_kept ? scores[b] / widen : scores[b];
      return ka < kb || (ka == kb && a < b);
    };
  };
  // Lowers a row's importance so its score lands just past `boundary` widened, where the tier code
  // demotes it (or keeps it) a tier coarser. A score of zero (an actor on the eye) cannot be moved
  // by an importance and is never held back: it ranks first.
  const auto hold = [&](u32 row, f32 boundary) {
    const f32 target = boundary * widen * k_hold_margin;
    if (scores[row] > 0.0f && scores[row] < target) importance[row] *= scores[row] / target;
  };

  order.clear();
  for (u32 row = 0; row < rows; ++row) {
    if (wants(row, skinned_boundary, 1)) order.push_back(row);
  }
  stats.wanted_skinned = order.size();
  u32 full_wanting = 0;
  for (const u32 row : order)
    full_wanting += wants(row, full_boundary, 0) ? 1u : 0u;
  stats.wanted_full = full_wanting;
  const u32 keep_skinned = limit(budget.skinned, stats.wanted_skinned);
  if (keep_skinned < stats.wanted_skinned) {
    std::sort(order.begin(), order.end(), ranked_by(1));
    for (u32 k = keep_skinned; k < order.size(); ++k)
      hold(order[k], skinned_boundary);
    stats.fell_back_skinned = stats.wanted_skinned - keep_skinned;
    order.resize(keep_skinned);
  }
  // Of the rows that keep a skinned tier, those that want LOD0, ranked again by their own band.
  u32 kept = 0;
  for (u32 k = 0; k < order.size(); ++k) {
    if (wants(order[k], full_boundary, 0)) order[kept++] = order[k];
  }
  order.resize(kept);
  const u32 keep_full = limit(budget.full, kept);
  if (keep_full < kept) {
    std::sort(order.begin(), order.end(), ranked_by(0));
    for (u32 k = keep_full; k < order.size(); ++k)
      hold(order[k], full_boundary);
    stats.fell_back_full = kept - keep_full;
  }
  return stats;
}

}  // namespace engine::animation

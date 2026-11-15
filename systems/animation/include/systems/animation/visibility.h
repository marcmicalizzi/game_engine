#pragma once

// **An actor keeps animating as far as it can be seen to move** (docs/subsystems/animation.md,
// "The LOD policy: what the eye can see"; roadmap A44).
//
// The tier of an animated instance is `sim::TierAssignment`'s: a score, distance over importance,
// against three band boundaries. Until A44 the importance was the camera's frustum gate alone, so
// the bands were metres and nothing else — 12 / 40 / 120 m — and a deer that runs away across a
// clearing stopped moving at 40 m whatever the screen it ran across: the owner watched it happen at
// 3840 x 2160, and a raven's wings froze at 45 m against an open sky. A distance cannot be right
// for both a bird and a crowd of characters, nor for 1080p and a surround.
//
// **What makes a frozen pose visible is how far a limb travels on the screen.** A tier past the
// middle band draws the rest pose (animation.md, "What a coarsened instance draws"), so the error
// it makes is, per vertex, the vertex's displacement from rest — bounded by the clip's
// displacement bound, the same number the cull pass pads the instance's spheres by
// (`clip_displacement_bound`, `gfx::InstanceDesc::bounds_padding`). Through a lens of `p` pixels a
// radian, a travel of `b` metres at `d` metres subtends `b * p / d` pixels. So the rule is a
// threshold on that number, and it folds into the tier code's importance exactly:
//
//     importance = (b / k_reference_travel_m) * (p / k_reference_px_per_rad) * bias
//     score      = d / importance  <  boundary
//               <=>  b * p * bias / d  >  k_reference_travel_m * k_reference_px_per_rad / boundary
//
// **The bands stay metres, and they mean one thing**: the distance at which an actor whose limbs
// travel one metre (`k_reference_travel_m`), seen through a lens of 1,000 pixels a radian
// (`k_reference_px_per_rad`, a 1080-row view at 56.7 degrees), stops being worth a tier. A band of
// `D` metres is a threshold of `1000 / D` pixels of travel: the defaults, 250 / 1000 / 3000, are 4,
// 1 and 1/3 of a pixel (the page derives each). At the owner's target — 3840 x 2160 at the
// renderer's 55 degrees, 2,074.6 pixels a radian — an actor whose limbs travel half a metre (a
// deer, a person) keeps every tick to 259 m, a skinned pose to 1,037 m, and its playhead to 3,112
// m; at 1080p each of those halves. Nothing here knows a species or a size: the travel is measured
// from the asset's own skin and clip, so an imported glTF gets the same rule as a generated one.
//
// **A per-actor bias** multiplies the importance: a bird against the sky is conspicuous beyond what
// its travel says (bias above 1), a prop's idle is not worth its pixels (below 1, or 0 for the
// coarsest tier always). A scene authors it on an instance's `animation` block (`lod_bias`), a game
// module sets it at run time (`actor_lod_bias`, module_abi.h ABI 11).
//
// **The budget still rules** (`TierBudget`): when more actors want the finest tiers than the frame
// pays for, the ones the rule ranks first — the nearest and the largest on screen, which is what a
// low score is — keep them, and the rest are held a tier coarser and counted (`cap_tiers`).
//
// Nothing here includes flecs or a world: it is arithmetic over spans, so a host with no ECS (an
// app; ADR-0028 seam 5) computes it and the capability's tests drive it with no `SimWorld`.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/sim/tiers.h>

#include <span>

namespace engine::animation {

// The reference the band boundaries are stated for: a limb travel of one metre through a lens of
// 1,000 pixels a radian. A boundary of `D` metres is a threshold of `1000 / D` pixels of travel.
inline constexpr f32 k_reference_travel_m = 1.0f;
inline constexpr f32 k_reference_px_per_rad = 1000.0f;

// The bias a scene or a game may give an actor: 0 (the coarsest tier always) to 64 (a bird a
// kilometre up keeps its wingbeat). Outside it is refused, not clamped.
inline constexpr f32 k_lod_bias_max = 64.0f;

// The pixels a travel of `travel_m` metres subtends `distance_m` metres away through a lens of
// `px_per_rad` pixels a radian (`renderer::View::proj_scale`: cot(fov_y / 2) * height / 2).
inline f32 travel_px(f32 travel_m, f32 distance_m, f32 px_per_rad) noexcept {
  return distance_m > 0.0f ? travel_m * px_per_rad / distance_m : 0.0f;
}

// The rule's factor on `sim::TierInput::importance`: the travel and the lens against the
// reference's, times the bias. Zero travel (a skin standing at its bind pose, a clip that moves
// nothing) is zero importance, which the tier code's floor turns into the coarsest tier.
inline f32 motion_importance(f32 travel_m, f32 px_per_rad, f32 bias) noexcept {
  return (travel_m / k_reference_travel_m) * (px_per_rad / k_reference_px_per_rad) * bias;
}

// The pixel threshold a band boundary of `boundary_m` stands for (`1000 / D`), for a report.
inline f32 boundary_px(f32 boundary_m) noexcept {
  return boundary_m > 0.0f ? k_reference_travel_m * k_reference_px_per_rad / boundary_m : 0.0f;
}

// ---- the budget --------------------------------------------------------------------------------

// How many actors a step may keep at the finest tiers: `full` at LOD0 (every tick, blended,
// skinned), `skinned` at LOD0 and LOD1 together (every tick or every second, skinned). Zero is no
// ceiling. Backed by `animation.lod.full_max` and `animation.lod.skinned_max`.
struct TierBudget {
  u32 full = 0;
  u32 skinned = 0;
};
TierBudget tier_budget() noexcept;

// What the last `cap_tiers` held back: actors whose score wanted LOD0 and were held at LOD1
// (`full`), and actors whose score wanted a skinned tier and were held at LOD2 (`skinned`).
struct TierBudgetStats {
  u32 wanted_full = 0;
  u32 wanted_skinned = 0;
  u32 fell_back_full = 0;
  u32 fell_back_skinned = 0;
};

// **The ceiling, applied before `sim::TierAssignment::assign_tiers`** by lowering the importance of
// the actors it holds back, so the tier code's hysteresis and rate limits apply to a held-back
// actor exactly as they apply to one that walked away.
//
// `scores` are the rows' scores at their unmodified importance (`sim::TierAssignment::score`),
// `tiers` the tiers they are at now. An actor wants LOD0 when its score is under `boundaries[0]` —
// or, already at LOD0, under it widened by the hysteresis, the band the tier code itself keeps it
// in — and a skinned tier likewise against `boundaries[1]`. The wanting rows are ranked by score
// (an incumbent's divided by the same widening, so two actors at the margin do not trade places
// every step; ties by row), the first `budget.full` keep LOD0 and the first `budget.skinned` a
// skinned tier, and each one held back has its importance scaled so its score lands just past the
// widened boundary it may not cross. Deterministic, allocation-free once `order` has the rows'
// capacity (it is the caller's scratch), and O(n log n) in the rows that want a skinned tier.
TierBudgetStats cap_tiers(std::span<const f32> scores, std::span<const u8> tiers,
                          const sim::TierParams& params, const TierBudget& budget,
                          std::span<f32> importance, Vector<u32>& order);

}  // namespace engine::animation

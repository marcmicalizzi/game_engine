#pragma once
// The trunk's skirt's numbers (G201; docs/experiments/tree-base-on-slopes.md, "Seating"; flora.md,
// "The trunk's skirt"), shared by the branching species' `build_skirt` (kit.h) and the palm's
// trunk, which has its own emitter. Internal to the module.
//
// A tree stands on the ground at its origin and its stem's bottom ring is level, so on a slope of
// angle a the downhill rim of radius r stands r tan a above the ground. The skirt carries the stem
// on below that ring: the flare's curve for a quarter of the rim's radius (the radius growing by a
// tenth), then straight down to rho tan 45 + 0.15 m below the origin, rho the lowest ring's
// farthest point from the origin across the level plane, so it is under a plane of 45 degrees
// through the origin at every azimuth with 0.15 m to spare. For a stem on the axis that is the
// design's 1.1 r + 0.15 m; a clump's stem or a leaning foot off the axis goes deeper by its offset.

#include <core/base/types.h>

namespace engine::flora::detail {

inline constexpr f64 k_skirt_slope_tan = 1.0;  // tan 45 degrees: the steepest ground it closes
inline constexpr f64 k_skirt_reach = 1.1;      // the rim's radius and the tenth the flare adds
inline constexpr f64 k_skirt_margin_m = 0.15;  // ground that curves away under the rim
inline constexpr f64 k_skirt_flare = 0.25;  // the flare's ring: this share of the rim's radius down
// Two bands of `sides` quads under the ring, open at the bottom.
inline u32 skirt_triangles(u32 sides) { return 4 * sides; }

}  // namespace engine::flora::detail

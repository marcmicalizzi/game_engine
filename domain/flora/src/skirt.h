#pragma once
// The trunk's skirt's numbers (G201; docs/experiments/tree-base-on-slopes.md, "Seating"; flora.md,
// "The trunk's skirt"), shared by the branching species' `build_skirt` (kit.h) and the palm's
// trunk, which has its own emitter. Internal to the module.
//
// A tree stands on the ground at its origin and its stem's bottom ring is level, so on a slope of
// angle a the downhill rim of radius r stands r tan a above the ground. The skirt carries the stem
// straight down from that ring to rho tan 45 + 0.15 m below the origin, rho the ring's farthest
// point from the origin across the level plane, so it is under a plane of 45 degrees through the
// origin at every azimuth with 0.15 m to spare: r + 0.15 m for a stem on the axis, deeper by its
// offset for a clump's stem or a leaning foot. Straight, because the flare's profile meets its
// bottom ring with no slope (a smoothstep), so the curve carried on is vertical there; the design's
// tenth of growth made a crease at the collar ring that the traced shadows drew as a dark line.

#include <core/base/types.h>

namespace engine::flora::detail {

inline constexpr f64 k_skirt_slope_tan = 1.0;  // tan 45 degrees: the steepest ground it closes
inline constexpr f64 k_skirt_margin_m = 0.15;  // ground that curves away under the rim
// A foot sunk already (the ancient tree's metre) still runs at least this share of its rim's
// radius below its lowest point.
inline constexpr f64 k_skirt_least_run = 0.5;
// One band of `sides` quads under the ring, open at the bottom.
inline u32 skirt_triangles(u32 sides) { return 2 * sides; }

}  // namespace engine::flora::detail

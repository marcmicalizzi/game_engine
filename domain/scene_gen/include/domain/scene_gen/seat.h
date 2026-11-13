#pragma once

// **A placement seated by its footprint** (G201 stage 2; docs/experiments/tree-base-on-slopes.md,
// "Seating"; scene_gen.md, "Seating by the footprint"). A `ground: true` tree stands on the ground
// at its origin, and stage 1's skirt carries its stem down far enough for any plane of up to 45
// degrees through that point. What a skirt cannot reach is ground that falls faster than that under
// the rim — a tread's cut, a bank, a bench's edge, the lattice's edge — and there the host lowers
// the instance until the skirt's bottom is under the lowest ground the rim stands over.
//
// The ground is asked at the centre and at 16 points on the rim's circle, in fixed world
// directions (so the answer does not depend on the yaw), in whole millimetres, and every decision
// is integer millimetres, products and sums, and a 2 by 2 solve in f64: the same bits on every
// toolchain, and a function of the scene and the place alone, never of the order tiles activate or
// the thread count. A sample in a hole (the exclusion's `k_exclude_holes`) is not ground and is
// left out; a centre in one seats nothing.
//
// The plane fitted to the rim's samples is kept as two 16-bit words, **four bytes a placement**
// (`GroundSlope`): the gradient a conformed foot (stage 3) will shear the flare by.

#include <core/base/types.h>
#include <core/math/world.h>
#include <domain/geometry/cluster_file.h>

namespace engine::scene_gen {

struct Ground;
class Exclusion;

// The rim's samples, and the margin the skirt's bottom is kept under the lowest of them by.
inline constexpr u32 k_seat_rim_samples = 16;
inline constexpr i64 k_seat_margin_mm = 50;

// A plane's gradient (dy/dx, dy/dz) in two words of `1 / k_slope_unit`: to plus or minus 4
// (76 degrees) at 0.00012 a step. 4 bytes, 2-aligned (tests/size_table.cpp).
inline constexpr f64 k_slope_unit = 8192.0;
struct GroundSlope {
  i16 gx = 0;
  i16 gz = 0;
  f64 x() const noexcept { return static_cast<f64>(gx) / k_slope_unit; }
  f64 z() const noexcept { return static_cast<f64>(gz) / k_slope_unit; }
  constexpr bool operator==(const GroundSlope&) const = default;
};
// The gradient's words: each the nearest step, clamped to the range.
GroundSlope quantize_slope(f64 gx, f64 gz) noexcept;

// **What the drawn ground is over a disc** of `radius_mm` round (x_mm, z_mm): the centre's height,
// the lowest and highest of the centre and the rim's samples that are ground, the drop from the
// centre to the lowest, and the plane fitted to the rim by least squares (its gradient and its
// largest residual). Heights are whole millimetres of the ground's surface.
struct GroundDisc {
  bool centre_hole = false;  // the centre is in a hole: nothing below is meaningful
  u32 rim_samples = 0;       // the rim's samples that are ground, of `k_seat_rim_samples`
  i64 centre_mm = 0;
  i64 min_mm = 0;
  i64 max_mm = 0;
  i64 drop_mm = 0;  // centre less the lowest
  f64 gx = 0.0;     // the fitted plane's dy/dx and dy/dz; 0 with fewer than three rim samples
  f64 gz = 0.0;
  f64 residual_mm = 0.0;  // the largest |sample - plane| over the rim's samples
  GroundSlope slope() const noexcept { return quantize_slope(gx, gz); }
};

// The rim's sample k, its offset from the centre in whole millimetres: the circle at k 22.5
// degrees from +x towards +z, each offset rounded half away from zero, so the 16 are symmetric
// under every quarter turn and reflection of the axes.
void seat_rim_offset(i64 radius_mm, u32 k, i64& dx_mm, i64& dz_mm) noexcept;

// Samples `ground` at the centre and, with a positive radius, at the 16 rim points. `exclusion`
// may be null (no holes).
GroundDisc ground_disc(const Ground& ground, const Exclusion* exclusion, i64 x_mm, i64 z_mm,
                       i64 radius_mm) noexcept;

// **The seat of an instance whose origin is at height `y_mm`**: the highest its origin may stand so
// that a skirt reaching `skirt_mm` under the origin is `k_seat_margin_mm` under the lowest ground
// the rim stands over — min(y, lowest + skirt - margin), so it never raises an instance and an
// instance sunk already (a scatter kind's `sink`) keeps the deeper of the two. Seating twice is
// seating once.
i64 seated_y_mm(const GroundDisc& disc, i64 y_mm, i64 skirt_mm) noexcept;

// A mesh's seat at an instance's scale (the container's section 42, `geometry::ClusterFileSeat`):
// the rim's radius across the ground's plane and how far the skirt reaches under the origin, both
// metres and positive. A radius of zero is no seat.
struct MeshSeat {
  f32 radius_m = 0.0f;
  f32 skirt_m = 0.0f;
  bool valid() const noexcept { return radius_m > 0.0f; }
};
// The container's record as a seat: its radius, and its skirt's bottom as a depth under the origin.
inline MeshSeat mesh_seat(const geometry::ClusterFileSeat& record) noexcept {
  return record.radius_m > 0.0f ? MeshSeat{record.radius_m, -record.skirt_y_m} : MeshSeat{};
}

// What seating one instance did.
struct SeatResult {
  bool seated = false;  // the ground was asked (a seat, a ground, a centre not in a hole)
  f64 lowered_m = 0.0;  // how far the origin went down; 0 for nearly every tree
  GroundSlope slope{};
};

// **The one call every host makes** for a placement whose mesh has a seat and which stands on the
// ground: the disc at the seat's radius times the larger of the scale's x and z, the skirt times
// the scale's y, and `position.y` lowered to `seated_y_mm` when that is below it (in f64 whole
// millimetres). Nothing happens for an invalid seat or a ground that is not present.
SeatResult seat_on_ground(const Ground& ground, const Exclusion* exclusion, const MeshSeat& seat,
                          Vec3 scale, WorldPos& position) noexcept;

}  // namespace engine::scene_gen

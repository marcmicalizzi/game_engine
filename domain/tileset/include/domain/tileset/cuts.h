#pragma once

// **Channels cut into the ground** (docs/subsystems/tileset.md, "Channels cut into the ground";
// roadmap W89): a set's `cuts` (`engine.scene.TerrainCut`, the `tiles` block's), compiled once and
// written into the set's heights when it is built.
//
// **The section.** Across the line at a point whose top of bank is `top`, bankfull width `W`, bed
// depth `D` and bank slope `m` (rise over run), at a distance `d` from the line the cut's surface
// is `top - delta(d)`:
//
//   m' = max(m, 2 D / W)                       (a bed narrower than the banks allow is a V)
//   delta(d) = min(D, m' (W / 2 - |d|))        for |d| <= W / 2 + D / m'
//
// — a flat bed `D` under the top, the banks rising from it at `m'` to the top at `W / 2`, and the
// face going on up at the same slope for at most `D` more past it, where it meets higher ground
// (a channel cut into a trough's side). Past that reach the cut is nothing. The ground becomes
// `min(ground, surface)`: lowered where it stands above the section and nowhere else, so a
// floodplain past the top of the bank is untouched.
//
// **Along the line** each segment's numbers are its two points' interpolated by the projection's
// parameter, and the surface is the lowest over every segment of every cut, so a joint, a
// junction and an overlap are each the union of the sections and the result does not depend on the
// order cuts or segments are applied in.
//
// **The cell rule.** A level whose spacing is not finer than the width at a point keeps its heights
// there: the cut closes on a level that cannot draw it, as a hole does (scene_gen's
// `TerrainHoles`). In a set the finest level (the heightfield's spacing) holds every cut wider than
// a point, and the far chain, whose cells are kilometres to tens of metres, none of a channel's.
//
// **Integer in every decision.** Coordinates are millimetres in f64; the arithmetic is products,
// sums, one division and one square root (both correctly rounded), never libm, so a point is cut to
// the same millimetre on every toolchain; the result is rounded half up once.

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::tileset {

class TerrainCuts {
 public:
  // Compiles `cuts`. False, with a sentence naming the cut, for fewer than two points, arrays whose
  // length is not the points', a width or depth not positive and finite, a coordinate not finite,
  // or a bank slope outside [0.25, 8].
  bool build(std::span<const scene::TerrainCut> cuts, std::string* error = nullptr);
  bool empty() const noexcept { return segments_.empty(); }
  u32 cut_count() const noexcept { return cuts_; }
  u32 segment_count() const noexcept { return segments_.size(); }
  // `hash_combine` over every cut's numbers in order; 0 for none, so a set without cuts keys as it
  // always did.
  u64 hash() const noexcept { return hash_; }

  // The cut surface at the world point (millimetres) for a level of spacing `cell_mm`: the lowest
  // over every segment that reaches the point and whose width there is wider than the cell. False
  // when none does (the ground is the ground there).
  bool surface_mm(f64 x_mm, f64 z_mm, i64 cell_mm, f64& out) const noexcept;

  // Lowers a window of a lattice in place: `values` holds `nx * nz` whole millimetres, rows of x
  // in order of z, point (i, j) at lattice index (i0 + i, j0 + j) of spacing `spacing_mm`. Each
  // point becomes `min(value, round(surface))` where a cut applies at that spacing. Walks each
  // segment's reach, so a window no cut reaches costs one comparison a segment. Returns the points
  // lowered.
  u64 apply(i64 spacing_mm, i64 i0, i64 j0, i64 nx, i64 nz, std::span<i32> values) const;

 private:
  struct Segment {
    f64 ax = 0, az = 0, bx = 0, bz = 0;  // millimetres
    f64 top_a = 0, top_b = 0;            // millimetres
    f64 w_a = 0, w_b = 0;                // millimetres
    f64 d_a = 0, d_b = 0;                // millimetres
    f64 slope = 1;
    f64 x0 = 0, z0 = 0, x1 = 0, z1 = 0;  // the reach's bounds
  };
  // The segment's surface at the point for the cell, millimetres; false when it does not reach it
  // or is no wider than the cell there.
  static bool segment_surface(const Segment& s, f64 x, f64 z, f64 cell, f64& out) noexcept;
  Vector<Segment> segments_;
  u32 cuts_ = 0;
  u64 hash_ = 0;
};

// The section across a cut, at distance `d` from its line, for a top `top`, width `w`, depth `depth`
// and bank slope `slope` (one length unit throughout): the surface's height, or false past the
// reach. Exposed for the tests, which hold the build's heights to it.
bool cut_section(f64 top, f64 w, f64 depth, f64 slope, f64 d, f64& out) noexcept;

}  // namespace engine::tileset

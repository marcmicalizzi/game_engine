#pragma once

// **Channels cut into the ground** (docs/subsystems/tileset.md, "Channels cut into the ground";
// roadmap W89, W93): a set's `cuts` (`engine.scene.TerrainCut`, the `tiles` block's), compiled once
// and written into the set's heights when it is built.
//
// **A cut is a ground edit along a line**: a profile across the line at every point, interpolated
// along each segment, and the ground lowered to it. The profile (`CutProfile`) is the lower
// envelope of three planes below a top: a bed `depth` under it and a bank each side rising from the
// bed at that side's slope to the top, which the left bank reaches `width / 2 + offset` from the
// line and the right `width / 2 - offset`. With `s` the signed distance from the line, left
// positive (the side of (-dz, dx) for a segment running (dx, dz)):
//
//   delta(s) = min(depth, m_l (width / 2 + offset - s), m_r (width / 2 - offset + s))
//   surface  = top - delta(s)                  while delta(s) >= -rise
//
// — past the top each face goes on up at its slope for `rise` more, where it meets higher ground
// (a channel cut into a trough's side), and then runs out (below). Banks too gentle to reach the
// bed inside the width (`depth / m_l + depth / m_r > width`) are steepened together until they
// meet at it, a V. A cut that names no side slopes is this profile with both the cut's
// `bank_slope` and no offset; every cut's `rise` is its depth.
//
// **What W93 adds to a cut** (`TerrainCut` version 2; docs/subsystems/channels.md, "The section"):
// a slope per point for each bank (a cut bank on a bend's outside, a point bar's slope on its
// inside), a rounded top (`bank_round`, below), and a **flood bed**: a second profile on the same
// line, a flat bed `flood_depth_m` under the top reaching `flood_left_m` and `flood_right_m` from
// the line, bounded each side by a scarp at `scarp_slope` whose top stands `scarp_m` over the bed,
// its face going on as much again where it meets higher ground. A segment's surface is its
// channel's and its flood bed's joined by the rounded minimum.
//
// **A rounded top.** Where a cut's surface crosses the ground the lower of the two has a crease;
// with a rounding height `k` (`bank_round` times the depth at the point) the ground becomes
//
//   round(ground, surface, k) = min(g, s) - (max(k - |g - s|, 0) / k)^2 k / 4
//
// — the polynomial smooth minimum: the two meet tangent over the band where they are within `k` of
// each other, the crease lowered by `k / 4`. It needs the ground, so every segment is rounded
// against the ground as it was and the lowest of them kept (`TerrainCuts::lowered_mm`), never one
// segment against another's work: the result is the same in any order. `k` is at most the depth
// (`bank_round` at most 1), so a face's reach, which ends a depth over the top, ends where the
// rounding has: no step at the reach's edge over ground at the top or below it.
//
// **A face that runs out.** W89's face ended a depth over its top, and where the ground beside
// the channel stood higher than that (a channel in a steep-sided valley, a line off its trough's
// floor) the cut ended in a wall, which a lattice draws as a row of teeth along the lip (the forest
// proof of concept's item 7, reproduced on W93's scratch scene). Every cut's faces, and its
// scarps, run out instead: past the end the face goes on over one more run of its own, its height
// over the top `rise / (1 - v)` at the share `v` of that run — the same height and slope where it
// begins, steepening without bound — so it meets the ground wherever the ground is, the reach is
// two runs past the top and no more, and nothing has a wall at its end. **Every cut rounds and
// runs out**: `bank_round` is 0.1 to 1 (0.75 where a cut names none), and the crease and the wall
// a `bank_round` of 0 kept until W93's continuation are not a shape a cut can ask for.
//
// **Along the line** each segment's numbers are its two points' interpolated by the projection's
// parameter, and the surface is the lowest over every segment of every cut, so a joint, a
// junction and an overlap are each the union of the sections and the result does not depend on the
// order cuts or segments are applied in. Beyond a segment's end the profile is read at the radial
// distance from the end point, its two sides blended by the direction (the left's share
// `(1 + lateral / distance) / 2`), so a line's end is a round cap whichever its banks are, and the
// outside of a joint is the two caps' union: an offset curve, with no tooth.
//
// **The cell rule.** A level whose spacing is not finer than the width at a point keeps its heights
// there: the cut closes on a level that cannot draw it, as a hole does (scene_gen's
// `TerrainHoles`). The channel is asked by its width, the flood bed by its own (`flood_left_m +
// flood_right_m`), so a level too coarse for a stream may still hold its flood bed. In a set the
// finest level (the heightfield's spacing) holds every cut wider than a point, and the far chain,
// whose cells are kilometres to tens of metres, none of a channel's.
//
// **Integer in every decision.** Coordinates are millimetres in f64; the arithmetic is products,
// sums, divisions and one square root (all correctly rounded), never libm, so a point is cut to
// the same millimetre on every toolchain; the result is rounded half up once.
//
// **A second client** (a trail's tread, a terrace; not built): the profile and the union are a
// client's as they stand — a tread is a profile with a flat bed and two short banks. What it would
// add: its own scene block compiled into `Segment`s beside the cuts' (`TerrainCuts::build` reads
// `TerrainCut` alone), a **fill** rule (this edit only lowers; a bench across a slope raises its
// downhill side, so a side needs `max` where a cut has `min`, and the union's order rule extended
// to the two together), and a top per side (here one `top` serves both banks).

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::tileset {

// The profile across a line at one station (one length unit throughout).
struct CutProfile {
  f64 top = 0;         // the height the banks' tops stand at
  f64 width = 0;       // top of the left bank to top of the right
  f64 offset = 0;      // the section's centre off the line, toward the left
  f64 depth = 0;       // the bed below the top
  f64 slope_left = 1;  // rise over run
  f64 slope_right = 1;
  // How far above the top a face goes on at its slope. Past that it **runs out**: it goes on
  // over one more run of its own (`rise / slope`), steepening, `rise / (1 - v)` over the top at
  // the share `v` of that run, so it meets ground however high with no wall at its end.
  f64 rise = 0;
};

// The profile's surface at the signed distance `s` from the line (left positive), or false past
// the reach on that side. Exposed for the tests, which hold the build's heights to it.
bool cut_profile(const CutProfile& p, f64 s, f64& out) noexcept;

// The rounded minimum of the ground and a cut's surface for a rounding height `k` (above); `k` 0
// is the minimum.
f64 cut_round(f64 ground, f64 surface, f64 k) noexcept;

class TerrainCuts {
 public:
  // Compiles `cuts`. False, with a sentence naming the cut, for fewer than two points, arrays whose
  // length is not the points', a width or depth not positive and finite, a coordinate not finite,
  // a bank slope outside [0.25, 8]; and (W93) a per-point slope outside [0.02, 16], a rounding
  // outside [0.1, 1], a flood bed with a negative reach or depth, a scarp that is not positive
  // where the bed has a reach, or a scarp slope outside [0.25, 8].
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
  // What the cuts leave of a ground standing at `ground_mm` there: the lowest over every such
  // segment of `cut_round(ground, surface, k)` with the segment's own rounding height — each
  // segment against the ground as it was, so the answer is the same in any order and is
  // continuous wherever the surfaces are. False when no segment reaches the point. Not rounded
  // to the millimetre: `apply` rounds it once.
  bool lowered_mm(f64 x_mm, f64 z_mm, i64 cell_mm, f64 ground_mm, f64& out) const noexcept;

  // Lowers a window of a lattice in place: `values` holds `nx * nz` whole millimetres, rows of x
  // in order of z, point (i, j) at lattice index (i0 + i, j0 + j) of spacing `spacing_mm`. Each
  // point becomes `min(value, round(lowered_mm(point, value)))` where a cut applies at that
  // spacing. Walks the window in blocks and each block's segments, so a window no cut reaches
  // costs one comparison a segment, and a point is a function of the cuts and its own height:
  // the same from any window that holds it. Returns the points lowered, each once.
  u64 apply(i64 spacing_mm, i64 i0, i64 j0, i64 nx, i64 nz, std::span<i32> values) const;

 private:
  // One end of a segment's profile: what `CutProfile` holds, the rounding height and the width
  // the cell rule reads.
  struct End {
    f64 top = 0, width = 0, offset = 0, depth = 0, slope_left = 1, slope_right = 1, rise = 0;
    f64 round = 0;
    f64 cell_width = 0;
  };
  struct Segment {
    f64 ax = 0, az = 0, bx = 0, bz = 0;  // millimetres
    End a, b;                            // the channel
    End fa, fb;                          // the flood bed, where `flood`
    bool flood = false;
    f64 x0 = 0, z0 = 0, x1 = 0, z1 = 0;  // the reach's bounds
  };
  // What a segment says at a point: its channel's surface (joined to its flood bed's by the
  // rounded minimum where it has both) and its flood bed's, each with its rounding height.
  struct Reading {
    bool has_channel = false, has_bed = false;
    f64 channel = 0, channel_round = 0;
    f64 bed = 0, bed_round = 0;
    f64 surface() const noexcept {
      if (!has_bed) return channel;
      return has_channel ? (channel < bed ? channel : bed) : bed;
    }
    f64 lowered(f64 ground) const noexcept {
      f64 v = ground;
      if (has_channel) v = cut_round(ground, channel, channel_round);
      if (has_bed) {
        const f64 b = cut_round(ground, bed, bed_round);
        if (b < v) v = b;
      }
      return v;
    }
  };
  // The segment's reading at the point for the cell, millimetres; false when it does not reach
  // it or is no wider than the cell there.
  static bool segment_at(const Segment& s, f64 x, f64 z, f64 cell, Reading& out) noexcept;
  Vector<Segment> segments_;
  u32 cuts_ = 0;
  u64 hash_ = 0;
};

}  // namespace engine::tileset

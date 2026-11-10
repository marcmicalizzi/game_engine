#pragma once

// **Where nothing is placed** (roadmap G135; scene_gen.md, "Exclusion"): the places of a scene's
// ground a placement generator must leave empty, read once from the scene entry and asked by every
// generator through `Context::excluded_mm`. Three kinds:
//
//  * **holes** (`Terrain.holes`, W87) -- the ground has no surface there (a cave's mouth, a shaft),
//    so there is nothing to stand on;
//  * **channels** (`water_channels`, S96) -- the bed, the water and the dressed banks of a stream:
//    a point within `half the surface width + the bank's reach` of the centreline, the reach being
//    the channel's own `bank_width_widths * w + bank_width_m` (`WaterChannel`, which the channels
//    recipe writes; its defaults 0.25 w + 0.4 m are the recipe's; T78), whose outer edge is the
//    riparian edge the channels page names; and, where the channel's cut has a **flood bed**
//    (W93), out to the bed's edge on each side (`WaterChannel.flood_left_m`, `flood_right_m`, the
//    cut's own numbers), whichever of the two reaches further on that side;
//  * **water** (`water`) -- a point of a body's outline whose ground is below its level (what
//    `water::depth_at` calls wading), and the whole of an open body's sea.
//
// **Why here**, in the registry's module like the holes (terrain_holes.h): the generators are
// siblings that see the ground and nothing of each other (ADR-0046 decision 4), so the one thing
// they share is read from the scene entry by the host and handed over in the `Context`; a
// generator never names the water capability, which is a higher layer.
//
// **Beyond the hard edge**, a channel also gives a *riparian* answer, `riparian_factor_mm`: 1 far
// from any channel, falling to `k_riparian_floor` at the bank's outer edge, along a smoothstep over
// `k_riparian_reach_m`. A generator with a density multiplies it in (the sward does; scatter and
// litter only drop). It is a design choice, not a fit: the riparian zone is a gradient of
// disturbance and moisture across the bank (Naiman and Decamps 1997), and the sward's patches
// cannot be scaled (flora.md), so the gradient is carried by the density class a patch is drawn
// from.
//
// **Determinism.** Everything is integer millimetres in, f64 out, with products, sums and one
// square root (correctly rounded), so the answer is the same on every toolchain, in any order and
// on any number of threads; the object is read-only once built.

#include "domain/scene_gen/terrain_holes.h"

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::scene_gen {

struct Ground;

enum ExclusionKind : u32 {
  k_exclude_holes = 1u,
  k_exclude_channels = 2u,
  k_exclude_water = 4u,
  k_exclude_all = 7u,
};

// The fraction of a density left at the bank's outer edge, and the distance over which it recovers
// to 1 (metres).
inline constexpr f64 k_riparian_floor = 0.2;
inline constexpr f64 k_riparian_reach_m = 3.0;

class Exclusion {
 public:
  // Compiles the scene's three sources. False, with a sentence, for a hole the terrain would have
  // refused; a body or channel that does not parse is not this class's to refuse (the water
  // capability says why when it reads it) and is left out.
  bool build(std::span<const scene::TerrainHole> holes, std::span<const scene::WaterBody> water,
             std::span<const scene::WaterChannel> channels, std::string* error = nullptr);
  bool empty() const noexcept { return holes_.empty() && bodies_.empty() && segments_.empty(); }
  u32 hole_count() const noexcept { return holes_.count(); }
  u32 body_count() const noexcept { return bodies_.size(); }
  u32 segment_count() const noexcept { return segments_.size(); }

  // Whether the world point (whole millimetres) is excluded by any of the `kinds`. `ground` gives
  // the surface the water bodies are compared against (absent: flat at 0).
  bool excluded_mm(const Ground& ground, i64 x_mm, i64 z_mm,
                   u32 kinds = k_exclude_all) const noexcept;
  // Metres from the point to the nearest channel's bank edge (0 inside it), or a very large number
  // when none is within `k_riparian_reach_m` of it.
  f64 channel_distance_mm(i64 x_mm, i64 z_mm) const noexcept;
  // 1 beyond the riparian reach; `k_riparian_floor` at the bank's edge.
  f64 riparian_factor_mm(i64 x_mm, i64 z_mm) const noexcept;

 private:
  struct Body {
    bool open = false;
    bool fill = false;
    bool circle = false;
    f64 level = 0.0;
    f64 cx = 0.0, cz = 0.0, r2 = 0.0;  // circle, or a fill's seed
    f64 extent = 0.0;                  // a fill's box half-side
    Vector<f64> xs, zs;                // polygon
    f64 x0 = 0.0, z0 = 0.0, x1 = 0.0, z1 = 0.0;
  };
  struct Segment {
    f64 ax = 0.0, az = 0.0, bx = 0.0, bz = 0.0;  // metres
    f64 ra = 0.0, rb = 0.0;                      // bank edge at each end, from the line
    // The flood bed's edge at each end, from the line, left ((-dz, dx)) and right; 0: none (W93).
    f64 la = 0.0, lb = 0.0, ga = 0.0, gb = 0.0;
  };
  struct Cell {
    i64 key_z = 0;
    i64 key_x = 0;
    u32 segment = 0;
  };
  static bool inside(const Body& body, f64 x, f64 z) noexcept;
  // The distance from (x, z) to the segment less its bank edge there; < 0 inside.
  static f64 bank_gap(const Segment& s, f64 x, f64 z) noexcept;
  // The least `bank_gap` over the segments filed in the point's cell; a large number when none.
  f64 nearest_gap(f64 x, f64 z) const noexcept;

  TerrainHoles holes_;
  Vector<Body> bodies_;
  Vector<Segment> segments_;
  Vector<Cell> cells_;  // sorted by (z, x, segment)
};

}  // namespace engine::scene_gen

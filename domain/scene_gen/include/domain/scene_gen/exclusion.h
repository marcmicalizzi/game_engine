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

// ---- footprints -------------------------------------------------------------------------------

// A point of a polygon footprint, whole millimetres of the world.
struct FootprintPoint {
  i64 x_mm = 0;
  i64 z_mm = 0;
};

enum class FootprintShape : u8 { disc = 0, band = 1, polygon = 2 };

// **A placement's footprint** (scene_gen.md, "Earlier placements" and "Footprints as shapes"; W98):
// what of the ground a thing stands on, as the placements after it are handed it
// (`Context::earlier`) and as the exclusion holds it (`Exclusion::add_footprints`). Three shapes,
// each in whole millimetres:
//
//  * a **disc** (`Footprint{x, z, radius}`): a tree's, a boulder's, a building's bounds;
//  * a **band** (`Footprint::band`): a segment with a half-width, its ends round — a wall's foot, a
//    tread, a hedge;
//  * a **polygon** (`Footprint::polygon`): a simple polygon — a pad, a building's plan, a yard. Its
//    points are held by whoever made the footprint for as long as it is read (the exclusion copies
//    them).
//
// **Every shape keeps a disc that holds it whole** (`x_mm`, `z_mm`, `radius`): a disc's own; a
// band's or a polygon's bounding circle, a millimetre over. So a reader that knows only discs, as
// every reader before W98 does, keeps clear of the whole shape — never inside it, though further
// off than it needs (a band's disc reaches half the band's length to either side) — and a reader
// that honours the shape asks `edge_m` or `covers_mm`, or the exclusion, which every generator
// already asks. Integer millimetres in, f64 out: products, sums, divisions and square roots, each
// correctly rounded, so the answer is the same on every toolchain.
struct Footprint {
  i64 x_mm = 0;
  i64 z_mm = 0;
  f64 radius = 0.0;  // metres
  // A band's line, end to end, and its half-width.
  i64 ax_mm = 0, az_mm = 0, bx_mm = 0, bz_mm = 0;
  i64 half_mm = 0;
  // A polygon's points, in order (either turn), and their number.
  const FootprintPoint* points = nullptr;
  u32 point_count = 0;
  FootprintShape shape = FootprintShape::disc;

  constexpr Footprint() noexcept = default;
  constexpr Footprint(i64 x, i64 z, f64 r) noexcept : x_mm(x), z_mm(z), radius(r) {}
  static Footprint band(i64 ax, i64 az, i64 bx, i64 bz, i64 half) noexcept;
  // The polygon over `pts`, which must outlive the footprint; fewer than three points is a disc
  // of no radius, which covers nothing.
  static Footprint polygon(std::span<const FootprintPoint> pts) noexcept;
  // Metres from the point to the shape's edge: positive outside, 0 on it, negative inside (how
  // deep). A disc of no radius, a band of no half-width and a polygon of no points are nowhere: a
  // very large number.
  f64 edge_m(i64 x_mm_at, i64 z_mm_at) const noexcept;
  // Whether the point is on or inside the shape.
  bool covers_mm(i64 x_mm_at, i64 z_mm_at) const noexcept {
    return edge_m(x_mm_at, z_mm_at) <= 0.0;
  }
};

enum ExclusionKind : u32 {
  k_exclude_holes = 1u,
  k_exclude_channels = 2u,
  k_exclude_water = 4u,
  // The shaped footprints the host handed the exclusion (W98).
  k_exclude_footprints = 8u,
  k_exclude_all = 15u,
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
  bool empty() const noexcept {
    return holes_.empty() && bodies_.empty() && segments_.empty() && footprints_.empty();
  }
  u32 hole_count() const noexcept { return holes_.count(); }
  u32 body_count() const noexcept { return bodies_.size(); }
  u32 segment_count() const noexcept { return segments_.size(); }
  u32 footprint_count() const noexcept { return footprints_.size(); }

  // **Shaped footprints** (W98; scene_gen.md, "Footprints as shapes"): adds `footprints` to what
  // is excluded (`k_exclude_footprints`), copying a polygon's points, after `build` (which clears
  // them). Each is filed on the exclusion's 16 m grid over the disc that holds it, so a query reads
  // the footprints of its own cell and no others. False, with a sentence, for a disc whose radius
  // is negative or not finite, a band with a half-width under a millimetre, or a polygon of fewer
  // than three points or with no area; nothing is added then.
  bool add_footprints(std::span<const Footprint> footprints, std::string* error = nullptr);
  // Metres from the world point (whole millimetres) to the nearest footprint's edge, 0 inside one,
  // or a very large number (over 1e20) when no edge is within `reach_m`: what a reader that wants
  // a margin of its own past a shape asks (the sward, which grows to a wall's foot). Reads the
  // cells within the reach.
  f64 footprint_distance_mm(i64 x_mm, i64 z_mm, f64 reach_m) const noexcept;

  // Whether the world point (whole millimetres) is excluded by any of the `kinds`. `ground` gives
  // the surface the water bodies are compared against (absent: flat at 0).
  bool excluded_mm(const Ground& ground, i64 x_mm, i64 z_mm,
                   u32 kinds = k_exclude_all) const noexcept;
  // Metres from the point to the nearest channel's bank edge (0 inside it), or a very large number
  // when none is within `k_riparian_reach_m` of it.
  f64 channel_distance_mm(i64 x_mm, i64 z_mm) const noexcept;
  // 1 beyond the riparian reach; `k_riparian_floor` at the bank's edge.
  f64 riparian_factor_mm(i64 x_mm, i64 z_mm) const noexcept;
  // **The one definition of a channel's bank edge, asked out to any reach** (W93, for the site
  // query, G144): metres from the world point (whole millimetres) to the nearest channel's bank
  // edge — the edge `excluded_mm` leaves bare: the dressed bank's, or a flood bed's where it
  // reaches further on the point's side — 0 inside it, or a very large number (over 1e20) when no
  // edge is within `reach_m` metres. `channel_distance_mm` is this at `k_riparian_reach_m`, to
  // the bit. Const and without state of its own, so any number of threads may ask at once. It
  // reads the 16 m grid's cells nearest the point first, its own and then a row at a time
  // outward, and stops when no cell left can hold an edge nearer than the best found (T102), so
  // a point near a channel reads a few cells whatever the reach, and only a point with no edge
  // near it reads the cells within the reach (at 128 m, a disc 16 cells across).
  f64 channel_edge_distance_mm(i64 x_mm, i64 z_mm, f64 reach_m) const noexcept;
  // **The square scan the query above replaced** (T90's): every cell within the reach and the
  // widest edge of the point. Kept for the test that holds the two to the bit and for nothing
  // else; no generator calls it.
  f64 channel_edge_distance_reference_mm(i64 x_mm, i64 z_mm, f64 reach_m) const noexcept;

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

  // A footprint's edge distance, its polygon's points read from `points_` at `first`.
  f64 footprint_edge_m(u32 k, i64 x_mm, i64 z_mm) const noexcept;

  TerrainHoles holes_;
  Vector<Body> bodies_;
  Vector<Segment> segments_;
  Vector<Cell> cells_;            // sorted by (z, x, segment)
  f64 widest_edge_ = 0.0;         // the furthest any segment's edge stands from its line, metres
  Vector<Footprint> footprints_;  // a polygon's `points` null: its points are `points_` from
  Vector<u32> footprint_first_;   // this index
  Vector<FootprintPoint> points_;
  Vector<Cell> footprint_cells_;  // sorted by (z, x, footprint)
};

}  // namespace engine::scene_gen

#pragma once

// **The site a placement reads** (roadmap G144; scene_gen.md, "The site"; the grounding in
// docs/experiments/site-and-ecotope.md): what the ground is like at a point, as the terrain
// variables a species distribution model reads — elevation, slope, the downslope direction, a heat
// load, curvature, and the distances to the nearest channel bank and water body — so a placement's
// rules ("forest on the shaded aspect", "a treeline at 2,200 m, higher on the sunny side", "green
// where water reaches") are a few lines over one query, the same for every generator that asks.
//
// **The pattern is the exclusion's** (exclusion.h): built once a scene read by the host, from the
// scene's sky (its latitude) and its water, handed to every generator in the `Context`
// (`Context::site`, `Context::site_mm`), integer millimetres in and f64 out. It lives here, in the
// registry's module, for the exclusion's reason: the generators are siblings that see the ground
// and nothing of each other (ADR-0046 decision 4), so what they share is read from the scene entry
// and passed down, and none of them names the water capability.
//
// **The baseline.** Slope, direction and curvature are central differences `k_site_step_mm` either
// side of the point (5 m: a 10 m baseline, the finest of the 10–30 m terrain models the literature
// computes them from), so a boulder's face is not a slope and a gully 20 m wide is still a hollow.
// Elevation is the surface at the point.
//
// **The heat load is a dot product** (McCune and Keon 2002's fold, the grounding page): the
// ground's unit normal against a reference sun at the folded bearing (225°, south-west, north of
// the equator; 315° south of it) and at the equinox's noon elevation, 90° − |latitude|, over its
// value on level ground — 1 level, more facing the sun, less facing away, never below 0. The sun's
// direction is computed once, at `build`, with `core/math/deterministic.h`; per point the load is
// products and sums. **No arctangent is ever needed for aspect**: the downslope direction is a unit
// vector's two components, and `sunward` is the horizontal part of the unit normal along the sun's
// bearing, sin(slope) cos(aspect − bearing).
//
// **Determinism.** Integer millimetres in; the ground's heights, then `+ - * /` and correctly
// rounded square roots, with floating-point contraction off (ADR-0035), so a sample is the same
// bits on every toolchain, in any order, on any number of threads, and from any tile that asks for
// it (a sample is a function of the point and the scene, never of a window). Read-only once built.
//
// **Cost.** `k_site_form` is five ground queries (the point and four neighbours) and about forty
// floating-point operations; `k_site_elevation` one query; `k_site_water` no ground query, one
// binary search into the channels' cells and a walk of the segments filed there, and the bodies'
// outlines. Nothing is cached per tile: a placement samples the points it keeps after its cheap
// tests, which is fewer than a lattice would be for every kind sparser than the lattice
// (scatter.md, "Kinds by site", has the measured numbers).

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/scene.h>
#include <span>
#include <string>

namespace engine::scene_gen {

struct Ground;

// The step either side of the point the derivatives are taken over, millimetres.
inline constexpr i64 k_site_step_mm = 5000;
// The farthest a distance to water is measured, metres: a point farther than this from every
// channel bank and body outline answers this. A rule that reads a distance must keep its limits
// within it.
inline constexpr f64 k_site_water_reach_m = 128.0;
// The latitude a scene naming no sky is read at: `engine.scene.Sky.latitude_deg`'s default.
inline constexpr f64 k_site_default_latitude_deg = 30.0;
// The latitude is held within this of the equator: at a pole the reference sun is on the horizon
// and the load's level value is 0.
inline constexpr f64 k_site_max_latitude_deg = 85.0;

// What a sample computes; a caller asks for what its rules read.
enum SiteField : u32 {
  k_site_elevation = 1u,  // one ground query
  k_site_form = 2u,       // slope, direction, heat load, sunward, curvature: five ground queries
  k_site_water = 4u,      // the two distances: no ground query
  k_site_all = 7u,
};

// One point's site. The fields a sample did not compute keep these defaults: level ground at 0 with
// no water within reach.
struct SiteSample {
  f64 elevation = 0.0;  // metres, the surface
  f64 slope = 0.0;      // rise over run, |grad z|
  // The unit vector down the slope in world x and z (north is -z, east +x); (0, 0) where it is
  // exactly level.
  f64 down_x = 0.0;
  f64 down_z = 0.0;
  // The reference sun's incidence over level ground's: 1 level, > 1 facing it, < 1 away, >= 0.
  f64 heat_load = 1.0;
  // The unit normal's horizontal part along the sun's bearing, [-1, 1]: 0.5 on a 30° slope facing
  // it.
  f64 sunward = 0.0;
  // The negative Laplacian of the elevation, 1/m: positive on a convex crest, negative in a hollow.
  f64 curvature = 0.0;
  // Metres to the nearest channel's bank edge (0 inside it), and to the nearest water body's
  // outline (0 inside it); `k_site_water_reach_m` when none is within it.
  f64 channel_m = k_site_water_reach_m;
  f64 body_m = k_site_water_reach_m;
  f64 water_m() const noexcept { return channel_m < body_m ? channel_m : body_m; }
};

class Site {
 public:
  // The fallback: the default latitude, no water. What `Context::site_mm` answers with when the
  // host hands none.
  Site() noexcept;

  // Compiles the scene's latitude (degrees north, negative south; held within
  // `k_site_max_latitude_deg`), its water bodies and its channels. A body or channel that does not
  // parse is left out, as the exclusion leaves it; a latitude that is not a number is refused.
  bool build(f64 latitude_deg, std::span<const scene::WaterBody> water,
             std::span<const scene::WaterChannel> channels, std::string* error = nullptr);

  f64 latitude_deg() const noexcept { return latitude_deg_; }
  // The reference sun as a unit vector, y up.
  f64 sun_x() const noexcept { return sun_x_; }
  f64 sun_y() const noexcept { return sun_y_; }
  f64 sun_z() const noexcept { return sun_z_; }
  u32 segment_count() const noexcept { return segments_.size(); }
  u32 body_count() const noexcept { return bodies_.size(); }

  // The site at the world point (whole millimetres), standing on `ground` (absent: flat at 0).
  SiteSample sample_mm(const Ground& ground, i64 x_mm, i64 z_mm,
                       u32 fields = k_site_all) const noexcept;

  // A default-built site, shared: what a generator handed none reads.
  static const Site& fallback() noexcept;

 private:
  struct Segment {
    f64 ax = 0.0, az = 0.0, bx = 0.0, bz = 0.0;  // metres
    f64 ra = 0.0, rb = 0.0;                      // bank edge at each end, from the line
  };
  struct Cell {
    i64 key_z = 0;
    i64 key_x = 0;
    u32 segment = 0;
  };
  struct Body {
    bool circle = false;
    f64 cx = 0.0, cz = 0.0, r = 0.0;
    Vector<f64> xs, zs;
    f64 x0 = 0.0, z0 = 0.0, x1 = 0.0, z1 = 0.0;  // the outline's box
  };
  f64 channel_distance(f64 x, f64 z) const noexcept;
  f64 body_distance(f64 x, f64 z) const noexcept;

  f64 latitude_deg_ = k_site_default_latitude_deg;
  f64 sun_x_ = 0.0, sun_y_ = 1.0, sun_z_ = 0.0;
  f64 bearing_x_ = 0.0, bearing_z_ = 1.0;  // the sun's horizontal bearing, unit
  Vector<Segment> segments_;
  Vector<Cell> cells_;  // sorted by (z, x, segment)
  Vector<Body> bodies_;
};

}  // namespace engine::scene_gen

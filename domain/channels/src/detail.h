#pragma once

// What the channels' sources share: the range checks, the hydraulic-geometry power law and the
// arithmetic every decision is made in, with no C library call (core/math/deterministic.h).
#include <core/hash/hash.h>
#include <core/math/deterministic.h>
#include <domain/channels/channels.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace engine::channels::detail {

inline constexpr f64 k_pi = 3.141592653589793;

inline bool fail(std::string& error, std::string_view field) {
  error = "engine.channels: invalid " + std::string(field);
  return false;
}
inline bool in(f64 v, f64 lo, f64 hi) { return v >= lo && v <= hi; }  // a NaN fails
inline f64 wide(f32 v) { return static_cast<f64>(v); }
inline f64 powd(f64 x, f64 y) { return x <= 0 ? 0.0 : det::exp(y * det::log(x)); }
inline f64 smoothstep(f64 lo, f64 hi, f64 v) {
  const f64 t = hi > lo ? (v - lo) / (hi - lo) : (v >= hi ? 1.0 : 0.0);
  const f64 c = t < 0 ? 0.0 : (t > 1 ? 1.0 : t);
  return c * c * (3 - 2 * c);
}
// Downstream hydraulic geometry, `a (A / 1 km^2)^b` (Leopold & Maddock 1953).
inline f64 hydraulic(f64 at_km2, f64 exponent, f64 area_m2) {
  return at_km2 * powd(area_m2 / 1e6, exponent);
}
// **The cut's cross-section** (channels.md, "The cut"; W89): a trapezoid of the bankfull width w
// at the top whose area is w times the mean depth mean the hydraulic geometry gives (Leopold &
// Maddock's depth is a mean, area over width), its banks at slope (rise over run). The thalweg
// depth solves D (w - D / slope) = w mean; banks too gentle for that area (slope w < 4 mean)
// make a V of twice the mean depth, its banks steeper than asked. `bed` is the flat bed's width.
struct Section {
  f64 thalweg = 0;
  f64 bed = 0;
};
inline Section section_of(f64 w, f64 mean, f64 slope) {
  Section s;
  if (!(w > 0) || !(mean > 0)) return s;
  const f64 q = 4 * mean / (slope * w);
  if (q >= 1) {
    s.thalweg = 2 * mean;
    return s;
  }
  s.thalweg = .5 * slope * w * (1 - std::sqrt(1 - q));
  s.bed = std::max(0.0, w - 2 * s.thalweg / slope);
  return s;
}
// **The section at a bend** (channels.md, "The section"; W93): what the cut's cross-section is at a
// point whose asymmetry is `asym`, -1 to 1 (positive where the line bends left, so the outer bank
// is the right). The riffle's section (`asym` 0) is the trapezoid above; at a bend's apex (`asym`
// 1 or -1) it is a triangle of the same area, `w mean`: the outer bank at `cut_bank_slope`, the
// thalweg at its toe twice the mean depth down, the point bar rising from there to the inner
// bank's top. Between the two the thalweg and the outer bank go linearly with |asym| and the
// inner bank's run is what keeps the area: `2 W (1 - mean / D) - D / m_outer`. The width is the
// hydraulic geometry's times `1 + swell` (`ChannelLink::swell`: wider at a riffle and over a
// step's pool, narrower at a bend's pool and at a step's crest), the mean depth changed with it so
// the area stays.
struct BendSection {
  f64 width = 0;       // the cut's width, top of bank to top of bank
  f64 thalweg = 0;     // the riffle's thalweg at this width, below the top
  f64 pool = 0;        // how far the pool lowers the thalweg below that
  f64 slope_left = 1;  // rise over run
  f64 slope_right = 1;
  f64 bed = 0;     // the flat bed's width (none at a bend's apex)
  f64 centre = 0;  // the flat bed's centre off the line, toward the left
};
inline BendSection bend_section(const ChannelLayer& p, f64 w, f64 mean, f64 asym, f64 swell) {
  BendSection out;
  if (!(w > 0) || !(mean > 0)) return out;
  const f64 a = std::min(1.0, std::fabs(asym));
  const f64 m = wide(p.bank_slope);
  out.width = w * (1 + std::clamp(swell, -.45, .45));
  const f64 mean_here = mean * w / out.width;
  const Section riffle = section_of(out.width, mean_here, m);
  out.thalweg = riffle.thalweg;
  out.slope_left = out.slope_right = m;
  out.bed = riffle.bed;
  if (!(a > 0)) return out;
  out.pool = a * std::max(0.0, 2 * mean_here - riffle.thalweg);
  const f64 depth = riffle.thalweg + out.pool;
  const f64 outer = m + a * std::max(0.0, wide(p.cut_bank_slope) - m);
  const f64 outer_run = depth / outer;
  // The inner bank's run, never shorter than the outer's and never past the outer bank's toe.
  const f64 inner_run = std::clamp(2 * out.width * (1 - mean_here / depth) - outer_run, outer_run,
                                   std::max(outer_run, out.width - outer_run));
  const f64 inner = std::clamp(depth / inner_run, .02, 16.0);
  out.bed = std::max(0.0, out.width - outer_run - inner_run);
  // The flat bed lies between the two toes: toward the outer bank, the right where asym > 0.
  const f64 shift = .5 * (inner_run - outer_run);
  if (asym > 0) {
    out.slope_left = inner;
    out.slope_right = outer;
    out.centre = -shift;
  } else {
    out.slope_left = outer;
    out.slope_right = inner;
    out.centre = shift;
  }
  return out;
}
inline f64 round_to(f64 v, f64 scale) { return std::round(v * scale) / scale; }
inline i64 floor_div(i64 a, i64 b) {
  const i64 q = a / b;
  return q - (a % b < 0 ? 1 : 0);
}
inline u32 neighbour(u32 i, u32 w, u32 h, i32 dx, i32 dz) {
  const i32 x = static_cast<i32>(i % w) + dx;
  const i32 z = static_cast<i32>(i / w) + dz;
  return x < 0 || z < 0 || x >= static_cast<i32>(w) || z >= static_cast<i32>(h)
             ? ~0u
             : static_cast<u32>(z) * w + static_cast<u32>(x);
}

// splitmix64; a draw's float is its top 53 bits scaled by a power of two, exact on every target.
struct Rng {
  u64 state;
  explicit Rng(u64 seed) : state(seed) {}
  u64 next() {
    state += 0x9E3779B97F4A7C15ull;
    return mix64(state);
  }
  f64 unit() { return static_cast<f64>(next() >> 11) * (1.0 / 9007199254740992.0); }
};
// A hash of a world place to [-1, 1]: the bed's relief at a vertex, the same in every piece that
// holds the vertex.
inline f64 place_noise(u64 seed, i64 x_mm, i64 z_mm) {
  const u64 h = hash_combine(hash_combine(seed, static_cast<u64>(x_mm)), static_cast<u64>(z_mm));
  return static_cast<f64>(mix64(h) >> 11) * (2.0 / 9007199254740992.0) - 1.0;
}

// `plan_channels` in its two halves (network.cpp): the lattice, which a plan's key reads, and the
// planner over it.
bool sample_lattice(const ChannelLayer& p, const scene_gen::Ground& ground, jobs::JobSystem* jobs,
                    ChannelNetwork& n, std::string& error);
bool plan_from_lattice(const ChannelLayer& p, const scene_gen::Ground& ground, u64 seed,
                       ChannelNetwork& n, std::string& error, const scene_gen::Exclusion* water);

inline bool surface_in_range(const ChannelSurface& s, std::string& error) {
  if (s.tile_m.has_value() && !in(wide(*s.tile_m), .05, 20)) return fail(error, "surface tile_m");
  if (!in(wide(s.roughness), .02, 1)) return fail(error, "surface roughness");
  if (s.color.has_value())
    for (f32 c : *s.color)
      if (!in(wide(c), 0, 1)) return fail(error, "surface color");
  if (s.tint.has_value())
    for (f32 c : *s.tint)
      if (!in(wide(c), 0, 1)) return fail(error, "surface tint");
  return true;
}

}  // namespace engine::channels::detail

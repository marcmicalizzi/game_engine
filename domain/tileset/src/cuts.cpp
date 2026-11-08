// Channels cut into the ground (cuts.h; docs/subsystems/tileset.md, "Channels cut into the ground";
// roadmap W89).

#include <core/hash/hash.h>
#include <domain/tileset/cuts.h>

#include <algorithm>
#include <bit>
#include <cmath>

namespace engine::tileset {

namespace {

constexpr f64 k_min_slope = 0.25;
constexpr f64 k_max_slope = 8.0;

u64 bits(f64 v) noexcept { return std::bit_cast<u64>(v); }

i64 floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
i64 ceil_div(i64 a, i64 b) noexcept { return -floor_div(-a, b); }

}  // namespace

bool cut_section(f64 top, f64 w, f64 depth, f64 slope, f64 d, f64& out) noexcept {
  // A bed narrower than the banks allow is a V: the banks steepen until the bed is a line.
  const f64 m = std::max(slope, 2.0 * depth / w);
  const f64 half = 0.5 * w;
  const f64 a = std::fabs(d);
  if (a > half + depth / m) return false;
  out = top - std::min(depth, m * (half - a));
  return true;
}

bool TerrainCuts::build(std::span<const scene::TerrainCut> cuts, std::string* error) {
  segments_.clear();
  cuts_ = 0;
  hash_ = 0;
  const auto fail = [&](const scene::TerrainCut& c, const char* why) {
    if (error != nullptr) *error = "the terrain's cut \"" + c.name + "\" " + why;
    segments_.clear();
    cuts_ = 0;
    hash_ = 0;
    return false;
  };
  u64 h = k_hash_seed;
  for (const scene::TerrainCut& c : cuts) {
    const u32 n = c.points.size();
    if (n < 2) return fail(c, "has fewer than two points");
    if (c.width_m.size() != n || c.depth_m.size() != n)
      return fail(c, "has a width and a depth array whose length is not its points'");
    const f64 slope = static_cast<f64>(c.bank_slope);
    if (!(slope >= k_min_slope) || !(slope <= k_max_slope))
      return fail(c, "has a bank slope outside [0.25, 8]");
    h = hash_combine(h, n);
    h = hash_combine(h, bits(slope));
    for (u32 k = 0; k < n; ++k) {
      const f64 w = static_cast<f64>(c.width_m[k]);
      const f64 d = static_cast<f64>(c.depth_m[k]);
      if (!std::isfinite(w) || !std::isfinite(d) || !(w > 0.0) || !(d > 0.0))
        return fail(c, "has a width or a depth that is not positive and finite");
      const WorldPos& p = c.points[k];
      if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        return fail(c, "has a point that is not finite");
      h = hash_combine(hash_combine(hash_combine(h, bits(p.x)), bits(p.y)), bits(p.z));
      h = hash_combine(hash_combine(h, bits(w)), bits(d));
    }
    for (u32 k = 0; k + 1 < n; ++k) {
      Segment s;
      s.ax = c.points[k].x * 1000.0;
      s.az = c.points[k].z * 1000.0;
      s.bx = c.points[k + 1].x * 1000.0;
      s.bz = c.points[k + 1].z * 1000.0;
      s.top_a = c.points[k].y * 1000.0;
      s.top_b = c.points[k + 1].y * 1000.0;
      s.w_a = static_cast<f64>(c.width_m[k]) * 1000.0;
      s.w_b = static_cast<f64>(c.width_m[k + 1]) * 1000.0;
      s.d_a = static_cast<f64>(c.depth_m[k]) * 1000.0;
      s.d_b = static_cast<f64>(c.depth_m[k + 1]) * 1000.0;
      s.slope = slope;
      // The reach: half the width and the face's run past it, the widest of the two ends (the
      // numbers between are interpolated, so the larger end bounds them).
      const f64 reach_a = 0.5 * s.w_a + s.d_a / std::max(slope, 2.0 * s.d_a / s.w_a);
      const f64 reach_b = 0.5 * s.w_b + s.d_b / std::max(slope, 2.0 * s.d_b / s.w_b);
      const f64 r = std::max(reach_a, reach_b) + 1.0;
      s.x0 = std::min(s.ax, s.bx) - r;
      s.x1 = std::max(s.ax, s.bx) + r;
      s.z0 = std::min(s.az, s.bz) - r;
      s.z1 = std::max(s.az, s.bz) + r;
      segments_.push_back(s);
    }
    ++cuts_;
  }
  hash_ = cuts_ == 0 ? 0 : h;
  return true;
}

bool TerrainCuts::segment_surface(const Segment& s, f64 x, f64 z, f64 cell, f64& out) noexcept {
  if (x < s.x0 || x > s.x1 || z < s.z0 || z > s.z1) return false;
  const f64 ex = s.bx - s.ax;
  const f64 ez = s.bz - s.az;
  const f64 len2 = ex * ex + ez * ez;
  f64 t = 0.0;
  if (len2 > 0.0) t = std::clamp(((x - s.ax) * ex + (z - s.az) * ez) / len2, 0.0, 1.0);
  const f64 px = s.ax + t * ex - x;
  const f64 pz = s.az + t * ez - z;
  const f64 d = std::sqrt(px * px + pz * pz);
  const f64 w = s.w_a + t * (s.w_b - s.w_a);
  // The cell rule: a level whose spacing is not finer than the width here keeps its heights.
  if (!(cell < w)) return false;
  const f64 top = s.top_a + t * (s.top_b - s.top_a);
  const f64 depth = s.d_a + t * (s.d_b - s.d_a);
  return cut_section(top, w, depth, s.slope, d, out);
}

bool TerrainCuts::surface_mm(f64 x_mm, f64 z_mm, i64 cell_mm, f64& out) const noexcept {
  bool any = false;
  const f64 cell = static_cast<f64>(cell_mm);
  for (const Segment& s : segments_) {
    f64 v = 0.0;
    if (!segment_surface(s, x_mm, z_mm, cell, v)) continue;
    out = any ? std::min(out, v) : v;
    any = true;
  }
  return any;
}

u64 TerrainCuts::apply(i64 spacing_mm, i64 i0, i64 j0, i64 nx, i64 nz,
                       std::span<i32> values) const {
  if (segments_.empty() || nx <= 0 || nz <= 0) return 0;
  const f64 cell = static_cast<f64>(spacing_mm);
  u64 lowered = 0;
  for (const Segment& s : segments_) {
    // A segment no wider than the cell anywhere is nothing at this spacing.
    if (!(cell < std::max(s.w_a, s.w_b))) continue;
    // The window's points inside the segment's reach.
    const i64 ia = std::max(i0, ceil_div(static_cast<i64>(std::floor(s.x0)), spacing_mm));
    const i64 ib = std::min(i0 + nx - 1, floor_div(static_cast<i64>(std::ceil(s.x1)), spacing_mm));
    const i64 ja = std::max(j0, ceil_div(static_cast<i64>(std::floor(s.z0)), spacing_mm));
    const i64 jb = std::min(j0 + nz - 1, floor_div(static_cast<i64>(std::ceil(s.z1)), spacing_mm));
    for (i64 j = ja; j <= jb; ++j) {
      for (i64 i = ia; i <= ib; ++i) {
        f64 v = 0.0;
        if (!segment_surface(s, static_cast<f64>(i * spacing_mm), static_cast<f64>(j * spacing_mm),
                             cell, v))
          continue;
        const f64 mm = std::floor(v + 0.5);
        i32& at = values[static_cast<usize>((j - j0) * nx + (i - i0))];
        if (mm < static_cast<f64>(at)) {
          at = static_cast<i32>(std::max(mm, -2147483647.0));
          ++lowered;
        }
      }
    }
  }
  return lowered;
}

}  // namespace engine::tileset

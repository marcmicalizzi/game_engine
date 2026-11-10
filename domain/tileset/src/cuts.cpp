// Channels cut into the ground (cuts.h; docs/subsystems/tileset.md, "Channels cut into the ground";
// roadmap W89, W93).

#include <core/hash/hash.h>
#include <domain/tileset/cuts.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace engine::tileset {

namespace {

constexpr f64 k_min_slope = 0.25;
constexpr f64 k_max_slope = 8.0;
// A point bar's surface is far gentler than a bank, and a cut bank far steeper (W93).
constexpr f64 k_min_side_slope = 0.02;
constexpr f64 k_max_side_slope = 16.0;
constexpr f64 k_max_round = 1.0;
// The side of a block of the window `apply` walks, lattice points.
constexpr i64 k_block = 64;

u64 bits(f64 v) noexcept { return std::bit_cast<u64>(v); }

i64 floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
i64 ceil_div(i64 a, i64 b) noexcept { return -floor_div(-a, b); }

// The profile's depth below its top at the signed distance `s`, or false past that side's reach.
// The symmetric case is W89's arithmetic, operation for operation.
bool delta_at(const CutProfile& p, f64 s, f64& delta) noexcept {
  f64 ml = p.slope_left, mr = p.slope_right;
  if (ml == mr) {
    // A bed narrower than the banks allow is a V: the banks steepen until the bed is a line.
    ml = mr = std::max(ml, 2.0 * p.depth / p.width);
  } else {
    const f64 f = (p.depth / ml + p.depth / mr) / p.width;
    if (f > 1.0) {
      ml *= f;
      mr *= f;
    }
  }
  const f64 half = 0.5 * p.width;
  const f64 rl = half + p.offset, rr = half - p.offset;
  if (s >= 0.0) {
    if (s > rl + p.rise / ml) return false;
  } else if (-s > rr + p.rise / mr) {
    return false;
  }
  delta = std::min(p.depth, std::min(ml * (rl - s), mr * (rr + s)));
  return true;
}

}  // namespace

bool cut_profile(const CutProfile& p, f64 s, f64& out) noexcept {
  f64 delta = 0.0;
  if (!delta_at(p, s, delta)) return false;
  out = p.top - delta;
  return true;
}

f64 cut_round(f64 ground, f64 surface, f64 k) noexcept {
  const f64 low = std::min(ground, surface);
  const f64 d = std::fabs(ground - surface);
  if (!(d < k)) return low;
  const f64 h = (k - d) / k;
  return low - h * h * k * 0.25;
}

bool cut_section(f64 top, f64 w, f64 depth, f64 slope, f64 d, f64& out) noexcept {
  return cut_profile(CutProfile{top, w, 0.0, depth, slope, slope, depth}, std::fabs(d), out);
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
    // W93's fields: each absent (empty, or its default) or whole.
    const bool sides = !c.bank_slope_left.empty() || !c.bank_slope_right.empty();
    if (sides && (c.bank_slope_left.size() != n || c.bank_slope_right.size() != n))
      return fail(c, "has a left and a right bank slope array whose length is not its points'");
    const f64 round = static_cast<f64>(c.bank_round);
    if (!(round >= 0.0) || !(round <= k_max_round))
      return fail(c, "has a bank rounding outside [0, 1]");
    const bool flood = !c.flood_left_m.empty() || !c.flood_right_m.empty() ||
                       !c.flood_depth_m.empty() || !c.scarp_m.empty();
    if (flood && (c.flood_left_m.size() != n || c.flood_right_m.size() != n ||
                  c.flood_depth_m.size() != n || c.scarp_m.size() != n))
      return fail(c, "has flood bed arrays whose length is not its points'");
    const f64 scarp_slope = static_cast<f64>(c.scarp_slope);
    if (flood && (!(scarp_slope >= k_min_slope) || !(scarp_slope <= k_max_slope)))
      return fail(c, "has a scarp slope outside [0.25, 8]");
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
    // Hashed only where a cut names them, so a W89 cut hashes as it did.
    if (sides) {
      h = hash_combine(h, 0x5349444553ull);
      for (u32 k = 0; k < n; ++k) {
        const f64 l = static_cast<f64>(c.bank_slope_left[k]);
        const f64 r = static_cast<f64>(c.bank_slope_right[k]);
        if (!(l >= k_min_side_slope) || !(l <= k_max_side_slope) || !(r >= k_min_side_slope) ||
            !(r <= k_max_side_slope))
          return fail(c, "has a left or a right bank slope outside [0.02, 16]");
        h = hash_combine(hash_combine(h, bits(l)), bits(r));
      }
    }
    if (round != 0.0) h = hash_combine(hash_combine(h, 0x524f554e44ull), bits(round));
    if (flood) {
      h = hash_combine(hash_combine(h, 0x464c4f4f44ull), bits(scarp_slope));
      for (u32 k = 0; k < n; ++k) {
        const f64 l = static_cast<f64>(c.flood_left_m[k]);
        const f64 r = static_cast<f64>(c.flood_right_m[k]);
        const f64 d = static_cast<f64>(c.flood_depth_m[k]);
        const f64 s = static_cast<f64>(c.scarp_m[k]);
        if (!std::isfinite(l) || !std::isfinite(r) || !std::isfinite(d) || !std::isfinite(s) ||
            !(l >= 0.0) || !(r >= 0.0) || !(d >= 0.0))
          return fail(c, "has a flood bed whose reach or depth is negative or not finite");
        if (l + r > 0.0 && !(s > 0.0))
          return fail(c, "has a flood bed whose scarp is not positive");
        h = hash_combine(hash_combine(h, bits(l)), bits(r));
        h = hash_combine(hash_combine(h, bits(d)), bits(s));
      }
    }
    const auto channel_end = [&](u32 k) {
      End e;
      e.top = c.points[k].y * 1000.0;
      e.width = static_cast<f64>(c.width_m[k]) * 1000.0;
      e.depth = static_cast<f64>(c.depth_m[k]) * 1000.0;
      e.slope_left = sides ? static_cast<f64>(c.bank_slope_left[k]) : slope;
      e.slope_right = sides ? static_cast<f64>(c.bank_slope_right[k]) : slope;
      e.rise = e.depth;
      e.round = round * e.depth;
      e.cell_width = e.width;
      return e;
    };
    // The flood bed as a profile of its own: its top the scarp's, its bed the flood bed's
    // surface, its banks the scarps, each face going on a scarp's height past its top where it
    // meets higher ground, as a bank's does.
    const auto flood_end = [&](u32 k, const End& channel) {
      End e;
      const f64 l = static_cast<f64>(c.flood_left_m[k]) * 1000.0;
      const f64 r = static_cast<f64>(c.flood_right_m[k]) * 1000.0;
      const f64 scarp = std::max(static_cast<f64>(c.scarp_m[k]) * 1000.0, 1.0);
      e.top = channel.top - static_cast<f64>(c.flood_depth_m[k]) * 1000.0 + scarp;
      e.depth = scarp;
      e.width = l + r + 2.0 * scarp / scarp_slope;
      e.offset = 0.5 * (l - r);
      e.slope_left = e.slope_right = scarp_slope;
      e.rise = scarp;
      e.round = round * scarp;
      e.cell_width = l + r;
      return e;
    };
    // An end's reach from the line: the further bank's top and the face's run past it, with the
    // banks as given (steepened banks reach less).
    const auto reach_of = [](const End& e) {
      return 0.5 * e.width + std::fabs(e.offset) + e.rise / std::min(e.slope_left, e.slope_right);
    };
    for (u32 k = 0; k + 1 < n; ++k) {
      Segment s;
      s.ax = c.points[k].x * 1000.0;
      s.az = c.points[k].z * 1000.0;
      s.bx = c.points[k + 1].x * 1000.0;
      s.bz = c.points[k + 1].z * 1000.0;
      s.a = channel_end(k);
      s.b = channel_end(k + 1);
      // The reach: the widest of the two ends (the numbers between are interpolated, so the larger
      // end bounds them).
      f64 r = std::max(reach_of(s.a), reach_of(s.b));
      if (flood) {
        s.fa = flood_end(k, s.a);
        s.fb = flood_end(k + 1, s.b);
        s.flood = s.fa.cell_width > 0.0 || s.fb.cell_width > 0.0;
        if (s.flood) r = std::max(r, std::max(reach_of(s.fa), reach_of(s.fb)));
      }
      r += 1.0;
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

bool TerrainCuts::segment_at(const Segment& s, f64 x, f64 z, f64 cell, Reading& out) noexcept {
  if (x < s.x0 || x > s.x1 || z < s.z0 || z > s.z1) return false;
  const f64 ex = s.bx - s.ax;
  const f64 ez = s.bz - s.az;
  const f64 len2 = ex * ex + ez * ez;
  f64 t = 0.0;
  bool cap = true;
  if (len2 > 0.0) {
    const f64 raw = ((x - s.ax) * ex + (z - s.az) * ez) / len2;
    cap = raw < 0.0 || raw > 1.0;
    t = std::clamp(raw, 0.0, 1.0);
  }
  const f64 px = s.ax + t * ex - x;
  const f64 pz = s.az + t * ez - z;
  const f64 d = std::sqrt(px * px + pz * pz);
  // Which side of the line: left is the side of (-ez, ex).
  const f64 cross = ex * (z - s.az) - ez * (x - s.ax);
  // Beyond an end the two sides are blended by the direction (cuts.h); `share` is the left's.
  f64 share = cross >= 0.0 ? 1.0 : 0.0;
  if (cap && d > 0.0 && len2 > 0.0)
    share = std::clamp(0.5 + 0.5 * (cross / std::sqrt(len2)) / d, 0.0, 1.0);
  else if (cap)
    share = 0.5;
  const auto profile = [&](const End& a, const End& b, f64& surface, f64& k) -> bool {
    // The cell rule: a level whose spacing is not finer than the width here keeps its heights.
    if (!(cell < a.cell_width + t * (b.cell_width - a.cell_width))) return false;
    CutProfile p;
    p.top = a.top + t * (b.top - a.top);
    p.width = a.width + t * (b.width - a.width);
    p.offset = a.offset + t * (b.offset - a.offset);
    p.depth = a.depth + t * (b.depth - a.depth);
    p.slope_left = a.slope_left + t * (b.slope_left - a.slope_left);
    p.slope_right = a.slope_right + t * (b.slope_right - a.slope_right);
    p.rise = a.rise + t * (b.rise - a.rise);
    f64 delta = 0.0;
    if (!cap) {
      if (!delta_at(p, cross >= 0.0 ? d : -d, delta)) return false;
    } else {
      f64 left = 0.0, right = 0.0;
      const bool has_left = delta_at(p, d, left);
      const bool has_right = delta_at(p, -d, right);
      if (!has_left && !has_right) return false;
      // A side past its reach stands at the reach's edge, the face's top.
      if (!has_left) left = -p.rise;
      if (!has_right) right = -p.rise;
      delta = right + share * (left - right);
    }
    surface = p.top - delta;
    k = a.round + t * (b.round - a.round);
    return true;
  };
  out.has_channel = profile(s.a, s.b, out.channel, out.channel_round);
  out.has_bed = s.flood && profile(s.fa, s.fb, out.bed, out.bed_round);
  // The channel's banks meet the flood bed rounded, as they meet the ground.
  if (out.has_channel && out.has_bed)
    out.channel = cut_round(out.bed, out.channel, out.channel_round);
  return out.has_channel || out.has_bed;
}

bool TerrainCuts::surface_mm(f64 x_mm, f64 z_mm, i64 cell_mm, f64& out) const noexcept {
  bool any = false;
  const f64 cell = static_cast<f64>(cell_mm);
  for (const Segment& s : segments_) {
    Reading r;
    if (!segment_at(s, x_mm, z_mm, cell, r)) continue;
    const f64 v = r.surface();
    out = any ? std::min(out, v) : v;
    any = true;
  }
  return any;
}

bool TerrainCuts::lowered_mm(f64 x_mm, f64 z_mm, i64 cell_mm, f64 ground_mm,
                             f64& out) const noexcept {
  bool any = false;
  const f64 cell = static_cast<f64>(cell_mm);
  for (const Segment& s : segments_) {
    Reading r;
    if (!segment_at(s, x_mm, z_mm, cell, r)) continue;
    const f64 v = r.lowered(ground_mm);
    out = any ? std::min(out, v) : v;
    any = true;
  }
  return any;
}

u64 TerrainCuts::apply(i64 spacing_mm, i64 i0, i64 j0, i64 nx, i64 nz,
                       std::span<i32> values) const {
  if (segments_.empty() || nx <= 0 || nz <= 0) return 0;
  const f64 cell = static_cast<f64>(spacing_mm);
  // The window's points inside a segment's reach, as lattice indices; false for none.
  struct Range {
    i64 ia, ib, ja, jb;
  };
  const auto range_of = [&](const Segment& s, Range& r) {
    // A segment no wider than the cell anywhere is nothing at this spacing.
    const f64 widest = std::max(std::max(s.a.cell_width, s.b.cell_width),
                                s.flood ? std::max(s.fa.cell_width, s.fb.cell_width) : 0.0);
    if (!(cell < widest)) return false;
    r.ia = std::max(i0, ceil_div(static_cast<i64>(std::floor(s.x0)), spacing_mm));
    r.ib = std::min(i0 + nx - 1, floor_div(static_cast<i64>(std::ceil(s.x1)), spacing_mm));
    r.ja = std::max(j0, ceil_div(static_cast<i64>(std::floor(s.z0)), spacing_mm));
    r.jb = std::min(j0 + nz - 1, floor_div(static_cast<i64>(std::ceil(s.z1)), spacing_mm));
    return r.ia <= r.ib && r.ja <= r.jb;
  };
  // The segments of each block of the window, by a counting sort: a block's are in the cuts'
  // order, though the lowest surface does not depend on it.
  const i64 bw = (nx + k_block - 1) / k_block, bh = (nz + k_block - 1) / k_block;
  Vector<u32> first(static_cast<u32>(bw * bh + 1), 0u);
  for (const Segment& s : segments_) {
    Range r{};
    if (!range_of(s, r)) continue;
    for (i64 bj = (r.ja - j0) / k_block; bj <= (r.jb - j0) / k_block; ++bj)
      for (i64 bi = (r.ia - i0) / k_block; bi <= (r.ib - i0) / k_block; ++bi)
        ++first[static_cast<u32>(bj * bw + bi + 1)];
  }
  for (u32 b = 0; b + 1 < first.size(); ++b)
    first[b + 1] += first[b];
  if (first[first.size() - 1] == 0) return 0;
  Vector<u32> refs(first[first.size() - 1], 0u);
  {
    Vector<u32> fill(static_cast<u32>(bw * bh), 0u);
    for (u32 k = 0; k < segments_.size(); ++k) {
      Range r{};
      if (!range_of(segments_[k], r)) continue;
      for (i64 bj = (r.ja - j0) / k_block; bj <= (r.jb - j0) / k_block; ++bj)
        for (i64 bi = (r.ia - i0) / k_block; bi <= (r.ib - i0) / k_block; ++bi) {
          const u32 b = static_cast<u32>(bj * bw + bi);
          refs[first[b] + fill[b]++] = k;
        }
    }
  }
  constexpr f64 k_none = std::numeric_limits<f64>::infinity();
  // The lowest ground any segment leaves at each point of a block. The values themselves are
  // written only once a block's segments have all read them: each segment rounds against the
  // ground as it was, never against another segment's work.
  Vector<f64> lowest(static_cast<u32>(k_block * k_block), k_none);
  u64 lowered = 0;
  for (i64 bj = 0; bj < bh; ++bj) {
    for (i64 bi = 0; bi < bw; ++bi) {
      const u32 b = static_cast<u32>(bj * bw + bi);
      if (first[b] == first[b + 1]) continue;
      // The block's points, and the lowest surface over its segments at each.
      const i64 bi0 = i0 + bi * k_block, bj0 = j0 + bj * k_block;
      const i64 bi1 = std::min(bi0 + k_block, i0 + nx) - 1;
      const i64 bj1 = std::min(bj0 + k_block, j0 + nz) - 1;
      std::fill(lowest.begin(), lowest.end(), k_none);
      for (u32 q = first[b]; q < first[b + 1]; ++q) {
        const Segment& s = segments_[refs[q]];
        Range r{};
        if (!range_of(s, r)) continue;
        for (i64 j = std::max(r.ja, bj0); j <= std::min(r.jb, bj1); ++j) {
          for (i64 i = std::max(r.ia, bi0); i <= std::min(r.ib, bi1); ++i) {
            Reading reading;
            if (!segment_at(s, static_cast<f64>(i * spacing_mm), static_cast<f64>(j * spacing_mm),
                            cell, reading))
              continue;
            const u32 at = static_cast<u32>((j - bj0) * k_block + (i - bi0));
            const f64 ground =
                static_cast<f64>(values[static_cast<usize>((j - j0) * nx + (i - i0))]);
            lowest[at] = std::min(lowest[at], reading.lowered(ground));
          }
        }
      }
      for (i64 j = bj0; j <= bj1; ++j) {
        for (i64 i = bi0; i <= bi1; ++i) {
          const u32 at = static_cast<u32>((j - bj0) * k_block + (i - bi0));
          if (lowest[at] == k_none) continue;
          i32& value = values[static_cast<usize>((j - j0) * nx + (i - i0))];
          const f64 mm = std::floor(lowest[at] + 0.5);
          if (mm < static_cast<f64>(value)) {
            value = static_cast<i32>(std::max(mm, -2147483647.0));
            ++lowered;
          }
        }
      }
    }
  }
  return lowered;
}

}  // namespace engine::tileset

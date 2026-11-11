// Ground edits: channels cut into the ground, and edits that raise as well as lower (cuts.h;
// docs/subsystems/tileset.md, "Channels cut into the ground" and "Ground edits that raise as well
// as lower"; roadmap W89, W93, W98).

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
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
// A fill's batter: from a long ramp to a face of dumped rock (W98; ground-edits.md).
constexpr f64 k_min_fill_slope = 0.1;
constexpr f64 k_max_fill_slope = 8.0;
constexpr f64 k_min_round = 0.1;
constexpr f64 k_max_round = 1.0;
// A face's height at its slope before it runs out, metres (W98).
constexpr f64 k_max_face_m = 100.0;
// What a bench's and a patch's cut faces hold their slope for where the edit names none: they have
// no depth for it to be (W98).
constexpr f64 k_default_face_m = 2.0;
// A patch's grid, points a side.
constexpr u32 k_max_grid = 1024;
// The share of a face's second run at which a run-out ends: the face stands 20 rises over the top
// there, which no ground a channel is cut into does.
constexpr f64 k_run_out_end = 0.95;
// The side of a block of the window `apply` walks, lattice points.
constexpr i64 k_block = 64;
constexpr f64 k_none = std::numeric_limits<f64>::infinity();

i64 floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
i64 ceil_div(i64 a, i64 b) noexcept { return -floor_div(-a, b); }

bool finite(f64 v) noexcept { return std::isfinite(v); }

// The profile's depth below its top at the signed distance `s`, or false past that side's reach.
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
  // How far the point lies past the end of its side's face, along the ground, and that face's
  // slope.
  const f64 m = s >= 0.0 ? ml : mr;
  const f64 past = s >= 0.0 ? s - (rl + p.rise / ml) : -s - (rr + p.rise / mr);
  if (past > 0.0) {
    // The face runs out (cuts.h): it goes on, steepening, over one more run of its own,
    // `rise / (1 - v)` over the top at the share `v` of that run.
    if (!(p.rise > 0.0)) return false;
    const f64 v = past * m / p.rise;
    if (!(v < k_run_out_end)) return false;
    delta = -p.rise / (1.0 - v);
    return true;
  }
  delta = std::min(p.depth, std::min(ml * (rl - s), mr * (rr + s)));
  return true;
}

// The depth (negative: a height over the top) a face stands at where it ends.
f64 end_delta(const CutProfile& p) noexcept { return -p.rise / (1.0 - k_run_out_end); }

// The squared distance from (x, z) to the segment from a to b.
f64 segment_distance2(f64 x, f64 z, f64 ax, f64 az, f64 bx, f64 bz) noexcept {
  const f64 ex = bx - ax, ez = bz - az;
  const f64 len2 = ex * ex + ez * ez;
  f64 t = 0.0;
  if (len2 > 0.0) t = std::clamp(((x - ax) * ex + (z - az) * ez) / len2, 0.0, 1.0);
  const f64 px = ax + t * ex - x, pz = az + t * ez - z;
  return px * px + pz * pz;
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

f64 fill_round(f64 ground, f64 surface, f64 k) noexcept {
  const f64 high = std::max(ground, surface);
  const f64 d = std::fabs(ground - surface);
  if (!(d < k)) return high;
  const f64 h = (k - d) / k;
  return high + h * h * k * 0.25;
}

bool face_height(f64 d, f64 slope, f64 rise, f64& out) noexcept {
  const f64 run = rise / slope;
  if (d <= run) {
    out = slope * d;
    return true;
  }
  if (!(rise > 0.0)) return false;
  const f64 v = (d - run) * slope / rise;
  if (!(v < k_run_out_end)) return false;
  out = rise / (1.0 - v);
  return true;
}

bool TerrainEdits::build(std::span<const scene::TerrainEdit> edits, std::string* error) {
  segments_.clear();
  patches_.clear();
  edits_ = 0;
  raises_ = false;
  hash_ = 0;
  const auto fail = [&](const scene::TerrainEdit& c, const char* why) {
    if (error != nullptr) *error = "the terrain's edit \"" + c.name + "\" " + why;
    segments_.clear();
    patches_.clear();
    edits_ = 0;
    raises_ = false;
    hash_ = 0;
    return false;
  };
  u64 h = k_hash_seed;
  for (const scene::TerrainEdit& c : edits) {
    const bool line = !c.points.empty();
    const bool patch = !c.outline.empty();
    if (line && patch) return fail(c, "names both a line's points and a patch's outline");
    if (!line && !patch) return fail(c, "names neither a line's points nor a patch's outline");
    const bool cuts = c.mode != scene::TerrainEditMode::Fill;
    const bool fills = c.mode != scene::TerrainEditMode::Cut;
    const bool bench = c.mode == scene::TerrainEditMode::Bench;
    const f64 slope = static_cast<f64>(c.bank_slope);
    if (!(slope >= k_min_slope) || !(slope <= k_max_slope))
      return fail(c, "has a bank slope outside [0.25, 8]");
    const f64 fill_slope = static_cast<f64>(c.fill_slope);
    if (!(fill_slope >= k_min_fill_slope) || !(fill_slope <= k_max_fill_slope))
      return fail(c, "has a fill slope outside [0.1, 8]");
    const f64 round = static_cast<f64>(c.bank_round);
    if (!(round >= k_min_round) || !(round <= k_max_round))
      return fail(c, "has a bank rounding outside [0.1, 1]");
    const f64 face_m = static_cast<f64>(c.face_m);
    if (!(face_m >= 0.0) || !(face_m <= k_max_face_m))
      return fail(c, "has a face height outside [0, 100] m");
    const f64 fill_m = static_cast<f64>(c.fill_m);
    if (!(fill_m > 0.0) || !(fill_m <= k_max_face_m))
      return fail(c, "has a fill height outside (0, 100] m");
    const bool fill_sides = !c.fill_slope_left.empty() || !c.fill_slope_right.empty();
    const bool flood = !c.flood_left_m.empty() || !c.flood_right_m.empty() ||
                       !c.flood_depth_m.empty() || !c.scarp_m.empty();

    if (patch) {
      if (!c.width_m.empty() || !c.depth_m.empty() || !c.bank_slope_left.empty() ||
          !c.bank_slope_right.empty() || fill_sides || flood)
        return fail(c, "is a patch and names a line's arrays");
      const u32 n = c.outline.size();
      if (n < 3) return fail(c, "is a patch of fewer than three points");
      Patch p;
      p.cuts = cuts;
      p.fills = fills;
      p.bench = bench;
      for (const WorldPos& q : c.outline) {
        if (!finite(q.x) || !finite(q.y) || !finite(q.z))
          return fail(c, "has a point that is not finite");
        p.xs.push_back(q.x * 1000.0);
        p.zs.push_back(q.z * 1000.0);
      }
      // Twice the signed area, and the perimeter, in millimetres.
      f64 area2 = 0.0, perimeter = 0.0;
      for (u32 i = 0, j = n - 1; i < n; j = i++) {
        area2 += p.xs[j] * p.zs[i] - p.xs[i] * p.zs[j];
        const f64 ex = p.xs[i] - p.xs[j], ez = p.zs[i] - p.zs[j];
        perimeter += std::sqrt(ex * ex + ez * ez);
      }
      if (!(std::fabs(area2) > 0.0)) return fail(c, "is a patch with no area");
      p.width = 2.0 * std::fabs(area2) / perimeter;
      const bool grid =
          c.grid_nx != 0 || c.grid_nz != 0 || c.grid_spacing_mm != 0 || !c.grid_mm.empty();
      if (grid) {
        if (c.grid_nx < 2 || c.grid_nz < 2 || c.grid_nx > k_max_grid || c.grid_nz > k_max_grid)
          return fail(c, "has a grid of fewer than 2 or more than 1024 points a side");
        if (c.grid_spacing_mm <= 0) return fail(c, "has a grid whose spacing is not positive");
        if (c.grid_mm.size() != c.grid_nx * c.grid_nz)
          return fail(c, "has a grid whose heights are not its points'");
        if (!finite(c.grid_origin.x) || !finite(c.grid_origin.y) || !finite(c.grid_origin.z))
          return fail(c, "has a grid whose origin is not finite");
        p.grid = true;
        p.gx = c.grid_origin.x * 1000.0;
        p.gy = c.grid_origin.y * 1000.0;
        p.gz = c.grid_origin.z * 1000.0;
        p.gs = static_cast<f64>(c.grid_spacing_mm);
        p.gnx = c.grid_nx;
        p.gnz = c.grid_nz;
        for (const i32 v : c.grid_mm)
          p.heights.push_back(v);
      } else {
        for (const WorldPos& q : c.outline)
          if (q.y != c.outline[0].y)
            return fail(c, "is a patch with no grid whose points are not all at one height");
        p.level = c.outline[0].y * 1000.0;
      }
      p.cut_slope = slope;
      p.cut_rise = (face_m > 0.0 ? face_m : k_default_face_m) * 1000.0;
      p.fill_slope = fill_slope;
      p.fill_rise = fill_m * 1000.0;
      p.cut_k = round * p.cut_rise;
      p.fill_k = round * p.fill_rise;
      // The reach: the faces' two runs past the edge, whichever the mode has, and a millimetre.
      f64 reach = 0.0;
      if (cuts) reach = std::max(reach, 2.0 * p.cut_rise / p.cut_slope);
      if (fills) reach = std::max(reach, 2.0 * p.fill_rise / p.fill_slope);
      reach += 1.0;
      p.x0 = *std::min_element(p.xs.begin(), p.xs.end()) - reach;
      p.x1 = *std::max_element(p.xs.begin(), p.xs.end()) + reach;
      p.z0 = *std::min_element(p.zs.begin(), p.zs.end()) - reach;
      p.z1 = *std::max_element(p.zs.begin(), p.zs.end()) + reach;
      patches_.push_back(std::move(p));
    } else {
      const u32 n = c.points.size();
      if (n < 2) return fail(c, "has fewer than two points");
      if (c.width_m.size() != n)
        return fail(c, "has a width array whose length is not its points'");
      if (cuts && !bench && c.depth_m.size() != n)
        return fail(c, "has a depth array whose length is not its points'");
      if (!(cuts && !bench) && !c.depth_m.empty() && c.depth_m.size() != n)
        return fail(c, "has a depth array whose length is not its points'");
      if (flood && !(cuts && !bench)) return fail(c, "has a flood bed and is not a cut");
      // W93's fields: each absent (empty, or its default) or whole.
      const bool sides = !c.bank_slope_left.empty() || !c.bank_slope_right.empty();
      if (sides && (c.bank_slope_left.size() != n || c.bank_slope_right.size() != n))
        return fail(c, "has a left and a right bank slope array whose length is not its points'");
      if (fill_sides && (c.fill_slope_left.size() != n || c.fill_slope_right.size() != n))
        return fail(c, "has a left and a right fill slope array whose length is not its points'");
      if (flood && (c.flood_left_m.size() != n || c.flood_right_m.size() != n ||
                    c.flood_depth_m.size() != n || c.scarp_m.size() != n))
        return fail(c, "has flood bed arrays whose length is not its points'");
      const f64 scarp_slope = static_cast<f64>(c.scarp_slope);
      if (flood && (!(scarp_slope >= k_min_slope) || !(scarp_slope <= k_max_slope)))
        return fail(c, "has a scarp slope outside [0.25, 8]");
      for (u32 k = 0; k < n; ++k) {
        const f64 w = static_cast<f64>(c.width_m[k]);
        if (!finite(w) || !(w > 0.0)) return fail(c, "has a width that is not positive and finite");
        if (cuts && !bench) {
          const f64 d = static_cast<f64>(c.depth_m[k]);
          if (!finite(d) || !(d > 0.0))
            return fail(c, "has a width or a depth that is not positive and finite");
        } else if (!c.depth_m.empty() && c.depth_m[k] != 0.0f) {
          return fail(c, "names a depth: a fill's or a bench's section is flat across its width");
        }
        const WorldPos& q = c.points[k];
        if (!finite(q.x) || !finite(q.y) || !finite(q.z))
          return fail(c, "has a point that is not finite");
        if (sides) {
          const f64 l = static_cast<f64>(c.bank_slope_left[k]);
          const f64 r = static_cast<f64>(c.bank_slope_right[k]);
          if (!(l >= k_min_side_slope) || !(l <= k_max_side_slope) || !(r >= k_min_side_slope) ||
              !(r <= k_max_side_slope))
            return fail(c, "has a left or a right bank slope outside [0.02, 16]");
        }
        if (fill_sides) {
          const f64 l = static_cast<f64>(c.fill_slope_left[k]);
          const f64 r = static_cast<f64>(c.fill_slope_right[k]);
          if (!(l >= k_min_side_slope) || !(l <= k_max_side_slope) || !(r >= k_min_side_slope) ||
              !(r <= k_max_side_slope))
            return fail(c, "has a left or a right fill slope outside [0.02, 16]");
        }
        if (flood) {
          const f64 l = static_cast<f64>(c.flood_left_m[k]);
          const f64 r = static_cast<f64>(c.flood_right_m[k]);
          const f64 d = static_cast<f64>(c.flood_depth_m[k]);
          const f64 s = static_cast<f64>(c.scarp_m[k]);
          if (!finite(l) || !finite(r) || !finite(d) || !finite(s) || !(l >= 0.0) || !(r >= 0.0) ||
              !(d >= 0.0))
            return fail(c, "has a flood bed whose reach or depth is negative or not finite");
          if (l + r > 0.0 && !(s > 0.0))
            return fail(c, "has a flood bed whose scarp is not positive");
        }
      }
      // The upper envelope at a point: a cut's trapezoid under its top, or a bench's flat top with
      // its cut faces.
      const auto channel_end = [&](u32 k) {
        End e;
        e.top = c.points[k].y * 1000.0;
        e.width = static_cast<f64>(c.width_m[k]) * 1000.0;
        e.depth = bench ? 0.0 : static_cast<f64>(c.depth_m[k]) * 1000.0;
        e.slope_left = sides ? static_cast<f64>(c.bank_slope_left[k]) : slope;
        e.slope_right = sides ? static_cast<f64>(c.bank_slope_right[k]) : slope;
        e.rise = face_m > 0.0 ? face_m * 1000.0 : (bench ? k_default_face_m * 1000.0 : e.depth);
        e.round = round * e.rise;
        e.cell_width = e.width;
        return e;
      };
      // The lower envelope: a flat top across the width with its fill faces.
      const auto fill_end = [&](u32 k) {
        End e;
        e.top = c.points[k].y * 1000.0;
        e.width = static_cast<f64>(c.width_m[k]) * 1000.0;
        e.slope_left = fill_sides ? static_cast<f64>(c.fill_slope_left[k]) : fill_slope;
        e.slope_right = fill_sides ? static_cast<f64>(c.fill_slope_right[k]) : fill_slope;
        e.rise = fill_m * 1000.0;
        e.round = round * e.rise;
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
        return 0.5 * e.width + std::fabs(e.offset) +
               2.0 * e.rise / std::min(e.slope_left, e.slope_right);
      };
      for (u32 k = 0; k + 1 < n; ++k) {
        Segment s;
        s.ax = c.points[k].x * 1000.0;
        s.az = c.points[k].z * 1000.0;
        s.bx = c.points[k + 1].x * 1000.0;
        s.bz = c.points[k + 1].z * 1000.0;
        s.cuts = cuts;
        s.fills = fills;
        s.bench = bench;
        // The reach: the widest of the two ends (the numbers between are interpolated, so the
        // larger end bounds them).
        f64 r = 0.0;
        if (cuts) {
          s.a = channel_end(k);
          s.b = channel_end(k + 1);
          r = std::max(reach_of(s.a), reach_of(s.b));
          s.widest = std::max(s.a.cell_width, s.b.cell_width);
        }
        if (flood) {
          s.fa = flood_end(k, s.a);
          s.fb = flood_end(k + 1, s.b);
          s.flood = s.fa.cell_width > 0.0 || s.fb.cell_width > 0.0;
          if (s.flood) {
            r = std::max(r, std::max(reach_of(s.fa), reach_of(s.fb)));
            s.widest = std::max(s.widest, std::max(s.fa.cell_width, s.fb.cell_width));
          }
        }
        if (fills) {
          s.da = fill_end(k);
          s.db = fill_end(k + 1);
          r = std::max(r, std::max(reach_of(s.da), reach_of(s.db)));
          s.widest = std::max(s.widest, std::max(s.da.cell_width, s.db.cell_width));
        }
        r += 1.0;
        s.x0 = std::min(s.ax, s.bx) - r;
        s.x1 = std::max(s.ax, s.bx) + r;
        s.z0 = std::min(s.az, s.bz) - r;
        s.z1 = std::max(s.az, s.bz) + r;
        segments_.push_back(s);
      }
    }
    // Every field of the edit, by construction: its canonical text.
    const std::string bytes = write_json(schema::to_json(c));
    h = hash_combine(h, hash_bytes(bytes.data(), bytes.size()));
    raises_ = raises_ || fills;
    ++edits_;
  }
  hash_ = edits_ == 0 ? 0 : h;
  return true;
}

bool TerrainEdits::segment_at(const Segment& s, f64 x, f64 z, f64 cell, Reading& out) noexcept {
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
  // The profile's delta at the point, its top and its rounding height; false past its reach.
  const auto profile = [&](const End& a, const End& b, f64& delta, f64& top, f64& k) -> bool {
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
    if (!cap) {
      if (!delta_at(p, cross >= 0.0 ? d : -d, delta)) return false;
    } else {
      f64 left = 0.0, right = 0.0;
      const bool has_left = delta_at(p, d, left);
      const bool has_right = delta_at(p, -d, right);
      if (!has_left && !has_right) return false;
      // A side past its reach stands at the reach's edge, the face's top.
      if (!has_left) left = end_delta(p);
      if (!has_right) right = end_delta(p);
      delta = right + share * (left - right);
    }
    top = p.top;
    k = a.round + t * (b.round - a.round);
    return true;
  };
  f64 delta = 0.0, top = 0.0, k = 0.0;
  out.has_channel = s.cuts && profile(s.a, s.b, delta, top, k);
  if (out.has_channel) {
    out.channel = top - delta;
    // A bench's rounding stops at its tread: no more than the face's height over the top here.
    out.channel_round = s.bench ? std::min(k, std::max(-delta, 0.0)) : k;
  }
  out.has_bed = s.flood && profile(s.fa, s.fb, delta, top, k);
  if (out.has_bed) {
    out.bed = top - delta;
    out.bed_round = k;
  }
  // The channel's banks meet the flood bed rounded, as they meet the ground.
  if (out.has_channel && out.has_bed)
    out.channel = cut_round(out.bed, out.channel, out.channel_round);
  out.has_fill = s.fills && profile(s.da, s.db, delta, top, k);
  if (out.has_fill) {
    // The fill's faces fall where a cut's rise: its delta is never positive (no depth).
    out.fill = top + delta;
    // A fill's rounding stops at its top (cuts.h): no more than its drop under the top here.
    out.fill_k = std::min(k, std::max(-delta, 0.0));
  }
  return out.has_channel || out.has_bed || out.has_fill;
}

bool TerrainEdits::patch_at(const Patch& p, f64 x, f64 z, f64 cell, Reading& out) noexcept {
  if (x < p.x0 || x > p.x1 || z < p.z0 || z > p.z1) return false;
  if (!(cell < p.width)) return false;
  // Inside by the even-odd rule over the edges (a crossing of the ray towards +x), and the
  // squared distance to the nearest edge outside.
  const u32 n = p.xs.size();
  bool inside = false;
  f64 d2 = k_none;
  for (u32 i = 0, j = n - 1; i < n; j = i++) {
    const f64 zi = p.zs[i], zj = p.zs[j];
    if ((zi > z) != (zj > z)) {
      const f64 cross = p.xs[j] + (z - zj) * (p.xs[i] - p.xs[j]) / (zi - zj);
      if (x < cross) inside = !inside;
    }
    d2 = std::min(d2, segment_distance2(x, z, p.xs[j], p.zs[j], p.xs[i], p.zs[i]));
  }
  const f64 d = inside ? 0.0 : std::sqrt(d2);
  // The surface: flat, or the grid's bilinear surface held at its edge.
  f64 surface = p.level;
  if (p.grid) {
    const f64 fx = std::clamp((x - p.gx) / p.gs, 0.0, static_cast<f64>(p.gnx - 1));
    const f64 fz = std::clamp((z - p.gz) / p.gs, 0.0, static_cast<f64>(p.gnz - 1));
    const i64 i = std::min(static_cast<i64>(std::floor(fx)), p.gnx - 2);
    const i64 j = std::min(static_cast<i64>(std::floor(fz)), p.gnz - 2);
    const f64 u = fx - static_cast<f64>(i), v = fz - static_cast<f64>(j);
    const auto at = [&](i64 a, i64 b) {
      return static_cast<f64>(p.heights[static_cast<u32>(b * p.gnx + a)]);
    };
    const f64 near_row = at(i, j) + u * (at(i + 1, j) - at(i, j));
    const f64 far_row = at(i, j + 1) + u * (at(i + 1, j + 1) - at(i, j + 1));
    surface = p.gy + near_row + v * (far_row - near_row);
  }
  f64 rise = 0.0;
  if (p.cuts && face_height(d, p.cut_slope, p.cut_rise, rise)) {
    out.has_channel = true;
    out.channel = surface + rise;
    out.channel_round = std::min(p.cut_k, rise);
  }
  if (p.fills && face_height(d, p.fill_slope, p.fill_rise, rise)) {
    out.has_fill = true;
    out.fill = surface - rise;
    out.fill_k = std::min(p.fill_k, rise);
  }
  return out.has_channel || out.has_fill;
}

bool TerrainEdits::item_at(u32 k, f64 x, f64 z, f64 cell, Reading& out) const noexcept {
  const u32 lines = segments_.size();
  return k < lines ? segment_at(segments_[k], x, z, cell, out)
                   : patch_at(patches_[k - lines], x, z, cell, out);
}

bool TerrainEdits::surface_mm(f64 x_mm, f64 z_mm, i64 cell_mm, f64& out) const noexcept {
  bool any = false;
  const f64 cell = static_cast<f64>(cell_mm);
  const u32 items = segments_.size() + patches_.size();
  for (u32 k = 0; k < items; ++k) {
    Reading r;
    if (!item_at(k, x_mm, z_mm, cell, r) || !r.cuts()) continue;
    const f64 v = r.surface();
    out = any ? std::min(out, v) : v;
    any = true;
  }
  return any;
}

bool TerrainEdits::fill_mm(f64 x_mm, f64 z_mm, i64 cell_mm, f64& out) const noexcept {
  bool any = false;
  const f64 cell = static_cast<f64>(cell_mm);
  const u32 items = segments_.size() + patches_.size();
  for (u32 k = 0; k < items; ++k) {
    Reading r;
    if (!item_at(k, x_mm, z_mm, cell, r) || !r.has_fill) continue;
    out = any ? std::max(out, r.fill) : r.fill;
    any = true;
  }
  return any;
}

bool TerrainEdits::edited_mm(f64 x_mm, f64 z_mm, i64 cell_mm, f64 ground_mm,
                             f64& out) const noexcept {
  bool any = false;
  const f64 cell = static_cast<f64>(cell_mm);
  const u32 items = segments_.size() + patches_.size();
  // Fills first, each against the ground as it was, the highest kept.
  f64 raised = ground_mm;
  if (raises_) {
    for (u32 k = 0; k < items; ++k) {
      Reading r;
      if (!item_at(k, x_mm, z_mm, cell, r) || !r.has_fill) continue;
      raised = std::max(raised, r.raised(ground_mm));
      any = true;
    }
  }
  // Then cuts, each against that, the lowest kept.
  f64 lowered = raised;
  for (u32 k = 0; k < items; ++k) {
    Reading r;
    if (!item_at(k, x_mm, z_mm, cell, r) || !r.cuts()) continue;
    lowered = std::min(lowered, r.lowered(raised));
    any = true;
  }
  if (any) out = lowered;
  return any;
}

u64 TerrainEdits::apply(i64 spacing_mm, i64 i0, i64 j0, i64 nx, i64 nz,
                        std::span<i32> values) const {
  const u32 lines = segments_.size();
  const u32 items = lines + patches_.size();
  if (items == 0 || nx <= 0 || nz <= 0) return 0;
  const f64 cell = static_cast<f64>(spacing_mm);
  // The window's points inside an item's reach, as lattice indices; false for none.
  struct Range {
    i64 ia, ib, ja, jb;
  };
  const auto range_of = [&](u32 k, Range& r) {
    f64 x0 = 0, x1 = 0, z0 = 0, z1 = 0, widest = 0;
    if (k < lines) {
      const Segment& s = segments_[k];
      x0 = s.x0, x1 = s.x1, z0 = s.z0, z1 = s.z1, widest = s.widest;
    } else {
      const Patch& p = patches_[k - lines];
      x0 = p.x0, x1 = p.x1, z0 = p.z0, z1 = p.z1, widest = p.width;
    }
    // An item no wider than the cell anywhere is nothing at this spacing.
    if (!(cell < widest)) return false;
    r.ia = std::max(i0, ceil_div(static_cast<i64>(std::floor(x0)), spacing_mm));
    r.ib = std::min(i0 + nx - 1, floor_div(static_cast<i64>(std::ceil(x1)), spacing_mm));
    r.ja = std::max(j0, ceil_div(static_cast<i64>(std::floor(z0)), spacing_mm));
    r.jb = std::min(j0 + nz - 1, floor_div(static_cast<i64>(std::ceil(z1)), spacing_mm));
    return r.ia <= r.ib && r.ja <= r.jb;
  };
  const auto fills_of = [&](u32 k) {
    return k < lines ? segments_[k].fills : patches_[k - lines].fills;
  };
  const auto cuts_of = [&](u32 k) {
    return k < lines ? (segments_[k].cuts || segments_[k].flood) : patches_[k - lines].cuts;
  };
  // The items of each block of the window, by a counting sort: a block's are in the list's order,
  // though the result does not depend on it.
  const i64 bw = (nx + k_block - 1) / k_block, bh = (nz + k_block - 1) / k_block;
  Vector<u32> first(static_cast<u32>(bw * bh + 1), 0u);
  for (u32 k = 0; k < items; ++k) {
    Range r{};
    if (!range_of(k, r)) continue;
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
    for (u32 k = 0; k < items; ++k) {
      Range r{};
      if (!range_of(k, r)) continue;
      for (i64 bj = (r.ja - j0) / k_block; bj <= (r.jb - j0) / k_block; ++bj)
        for (i64 bi = (r.ia - i0) / k_block; bi <= (r.ib - i0) / k_block; ++bi) {
          const u32 b = static_cast<u32>(bj * bw + bi);
          refs[first[b] + fill[b]++] = k;
        }
    }
  }
  // The highest fill and the lowest cut at each point of a block. The values themselves are
  // written only once a block's items have all read them: each fill reads the ground as it was and
  // each cut the raised ground, never another item's work.
  Vector<f64> raised(static_cast<u32>(k_block * k_block), -k_none);
  Vector<f64> lowest(static_cast<u32>(k_block * k_block), k_none);
  u64 changed = 0;
  for (i64 bj = 0; bj < bh; ++bj) {
    for (i64 bi = 0; bi < bw; ++bi) {
      const u32 b = static_cast<u32>(bj * bw + bi);
      if (first[b] == first[b + 1]) continue;
      const i64 bi0 = i0 + bi * k_block, bj0 = j0 + bj * k_block;
      const i64 bi1 = std::min(bi0 + k_block, i0 + nx) - 1;
      const i64 bj1 = std::min(bj0 + k_block, j0 + nz) - 1;
      std::fill(raised.begin(), raised.end(), -k_none);
      std::fill(lowest.begin(), lowest.end(), k_none);
      const auto ground_at = [&](i64 i, i64 j) {
        return static_cast<f64>(values[static_cast<usize>((j - j0) * nx + (i - i0))]);
      };
      // Two passes over the block's items: the fills, then the cuts against what they raised.
      for (u32 pass = 0; pass < 2; ++pass) {
        if (pass == 0 && !raises_) continue;
        for (u32 q = first[b]; q < first[b + 1]; ++q) {
          const u32 k = refs[q];
          if (pass == 0 ? !fills_of(k) : !cuts_of(k)) continue;
          Range r{};
          if (!range_of(k, r)) continue;
          for (i64 j = std::max(r.ja, bj0); j <= std::min(r.jb, bj1); ++j) {
            for (i64 i = std::max(r.ia, bi0); i <= std::min(r.ib, bi1); ++i) {
              Reading reading;
              if (!item_at(k, static_cast<f64>(i * spacing_mm), static_cast<f64>(j * spacing_mm),
                           cell, reading))
                continue;
              const u32 at = static_cast<u32>((j - bj0) * k_block + (i - bi0));
              const f64 ground = ground_at(i, j);
              if (pass == 0) {
                if (reading.has_fill) raised[at] = std::max(raised[at], reading.raised(ground));
              } else if (reading.cuts()) {
                const f64 base = raised[at] == -k_none ? ground : std::max(raised[at], ground);
                lowest[at] = std::min(lowest[at], reading.lowered(base));
              }
            }
          }
        }
      }
      for (i64 j = bj0; j <= bj1; ++j) {
        for (i64 i = bi0; i <= bi1; ++i) {
          const u32 at = static_cast<u32>((j - bj0) * k_block + (i - bi0));
          const f64 v = lowest[at] != k_none ? lowest[at] : raised[at];
          if (v == -k_none) continue;
          i32& value = values[static_cast<usize>((j - j0) * nx + (i - i0))];
          const f64 mm = std::clamp(std::floor(v + 0.5), -2147483647.0, 2147483647.0);
          if (mm != static_cast<f64>(value)) {
            value = static_cast<i32>(mm);
            ++changed;
          }
        }
      }
    }
  }
  return changed;
}

}  // namespace engine::tileset

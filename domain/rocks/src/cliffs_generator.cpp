// The scene generator "cliffs" (cliffs.h; docs/subsystems/rocks.md, "The cliffs generator").
//
// Per block of a world-anchored grid: the ground's heights on a lattice of `spacing_m`, the slope
// from them by central differences, marching squares on "steeper than `min_slope_deg`", the
// boundary segments whose steep side is uphill kept — the feet — and joined into lines; each line
// resampled, smoothed, measured up the slope to where the steep ground ends (the face's height and
// its batter), stood off downhill so the carving stays proud of the ground, cut into faces no
// longer than `max_length_m`, and written as recipes. Everything is a function of the block's
// heights, so a face is the same whichever tile asks for it; the identity is a hash of the line in
// millimetres.
#include "detail.h"
#include "geology.h"

#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_file.h>
#include <domain/gfx/formation.h>
#include <domain/rocks/cliffs.h>
#include <domain/rocks/rocks.h>
#include <foundation/io/vfs.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>

namespace engine::rocks {

namespace {

using namespace detail;

ENGINE_LOG_CATEGORY_DEFINE(log_cliffs, "cliffs");

f64 min2(f64 a, f64 b) { return a < b ? a : b; }
f64 max2(f64 a, f64 b) { return a > b ? a : b; }

// Consecutive vertices of a written line stay at least this far apart: the recipe validator's 5 cm
// (cliff.cpp, "consecutive vertices at least 5 cm apart") with a hair of slack for the float32
// rounding of the face-local coordinates.
constexpr f64 k_min_gap = 0.0505;
// How far under the ground behind it a face's top is held, and under the ground at its foot a
// tapered end's (rocks.md, "Faces in the slope").
constexpr f64 k_under_m = 0.3;
// Seating a top (rocks.md, "A face in tiers"): the ground behind it is looked at this many times
// across the back, and must hold from this far behind where the top goes in.
constexpr u32 k_stations = 32;
constexpr f64 k_settle_m = 1.5;

// ---- the block's lattice -----------------------------------------------------------------------

struct Lattice {
  i64 x0_mm = 0;  // point (0, 0): the block's corner
  i64 z0_mm = 0;
  i64 spacing_mm = 1;
  f64 spacing = 1.0;  // metres
  i32 cells = 0;      // a side, in the block
  i32 apron = 2;      // points outside the block a side, for the gradients
  Vector<f64> h;      // heights, metres, (cells + 1 + 2 apron)² points
  Vector<f64> gx;     // the slope's components at the block's points, (cells + 1)²
  Vector<f64> gz;
  u32 side() const { return static_cast<u32>(cells + 1 + 2 * apron); }
  f64 height(i32 i, i32 j) const {
    return h[static_cast<u32>(j + apron) * side() + static_cast<u32>(i + apron)];
  }
  u32 at(i32 i, i32 j) const {
    return static_cast<u32>(j) * static_cast<u32>(cells + 1) + static_cast<u32>(i);
  }
  f64 x(f64 i) const {
    return (static_cast<f64>(x0_mm) + i * static_cast<f64>(spacing_mm)) / 1000.0;
  }
  f64 z(f64 j) const {
    return (static_cast<f64>(z0_mm) + j * static_cast<f64>(spacing_mm)) / 1000.0;
  }
};

f64 ground_at(const scene_gen::Ground& ground, f64 x, f64 z) {
  return ground.surface_mm(scene_gen::nearest_mm(x), scene_gen::nearest_mm(z));
}

// The slope's square at a point between lattice points: central differences of the ground there.
f64 slope2_at(const scene_gen::Ground& ground, f64 x, f64 z, f64 d) {
  const f64 sx = (ground_at(ground, x + d, z) - ground_at(ground, x - d, z)) / (2.0 * d);
  const f64 sz = (ground_at(ground, x, z + d) - ground_at(ground, x, z - d)) / (2.0 * d);
  return sx * sx + sz * sz;
}

// ---- marching squares --------------------------------------------------------------------------

struct Segment {
  u64 from_edge = 0;
  u64 to_edge = 0;
  D3 from;  // x, 0, z in metres
  D3 to;
};

// The crossing on the edge from lattice point a to b, where the field changes sign.
D3 crossing(const Lattice& l, i32 ai, i32 aj, i32 bi, i32 bj, f64 fa, f64 fb) {
  const f64 t = fa / (fa - fb);
  const f64 i = static_cast<f64>(ai) + (static_cast<f64>(bi) - static_cast<f64>(ai)) * t;
  const f64 j = static_cast<f64>(aj) + (static_cast<f64>(bj) - static_cast<f64>(aj)) * t;
  return D3{l.x(i), 0.0, l.z(j)};
}

// Bilinear in cell (ci, cj) of a field on the block's points at metres (x, z).
f64 bilinear(const Lattice& l, const Vector<f64>& field, i32 ci, i32 cj, f64 x, f64 z) {
  const f64 u = clamp01((x - l.x(static_cast<f64>(ci))) / l.spacing);
  const f64 v = clamp01((z - l.z(static_cast<f64>(cj))) / l.spacing);
  const f64 f00 = field[l.at(ci, cj)];
  const f64 f10 = field[l.at(ci + 1, cj)];
  const f64 f01 = field[l.at(ci, cj + 1)];
  const f64 f11 = field[l.at(ci + 1, cj + 1)];
  return lerp(lerp(f00, f10, u), lerp(f01, f11, u), v);
}

// ---- a line into faces -------------------------------------------------------------------------

struct Point2 {
  f64 x = 0.0;
  f64 z = 0.0;
};

// Resampled at `step` along its length, ends kept.
Vector<Point2> resample(const Vector<Point2>& in, f64 step) {
  Vector<f64> s(in.size());
  for (u32 i = 1; i < in.size(); ++i) {
    const f64 dx = in[i].x - in[i - 1].x;
    const f64 dz = in[i].z - in[i - 1].z;
    s[i] = s[i - 1] + std::sqrt(dx * dx + dz * dz);
  }
  const f64 length = s[s.size() - 1];
  const u32 n = static_cast<u32>(std::ceil(length / step - 1e-9)) > 1
                    ? static_cast<u32>(std::ceil(length / step - 1e-9))
                    : 1u;
  Vector<Point2> out;
  out.reserve(n + 1);
  u32 k = 0;
  for (u32 i = 0; i <= n; ++i) {
    const f64 at = i == n ? length : length * static_cast<f64>(i) / static_cast<f64>(n);
    while (k + 2 < in.size() && s[k + 1] < at)
      ++k;
    const f64 span = s[k + 1] - s[k];
    const f64 t = span > 0.0 ? clamp01((at - s[k]) / span) : 0.0;
    out.push_back(Point2{lerp(in[k].x, in[k + 1].x, t), lerp(in[k].z, in[k + 1].z, t)});
  }
  return out;
}

void smooth(Vector<Point2>& p, u32 passes) {
  for (u32 pass = 0; pass < passes; ++pass) {
    Vector<Point2> next = p;
    for (u32 i = 1; i + 1 < p.size(); ++i) {
      next[i].x = 0.25 * p[i - 1].x + 0.5 * p[i].x + 0.25 * p[i + 1].x;
      next[i].z = 0.25 * p[i - 1].z + 0.5 * p[i].z + 0.25 * p[i + 1].z;
    }
    p = next;
  }
}

void smooth_values(Vector<f64>& v, u32 half) {
  Vector<f64> next(v.size());
  for (u32 i = 0; i < v.size(); ++i) {
    f64 sum = 0.0;
    f64 count = 0.0;
    const u32 lo = i > half ? i - half : 0;
    const u32 hi = i + half < v.size() ? i + half : v.size() - 1;
    for (u32 k = lo; k <= hi; ++k) {
      sum += v[k];
      count += 1.0;
    }
    next[i] = sum / count;
  }
  v = next;
}

RockType type_for(const CliffLayer& layer, f64 foot_y) {
  for (const CliffBand& band : layer.bands) {
    if (foot_y < wide(band.below_m)) return band.type;
  }
  return layer.face.type;
}

f64 relief_of(const CliffLayer& layer, RockType type) {
  if (layer.face.relief.has_value()) return wide(*layer.face.relief);
  switch (type) {
    case RockType::Sandstone: return 3.5;
    case RockType::Basalt: return 2.0;
    case RockType::Limestone: return 3.0;
    case RockType::Dolostone: return 3.5;
    case RockType::Quartzite: return 4.0;
    case RockType::Shale: return 2.5;
    case RockType::Slate: return 2.0;
    case RockType::Schist: return 3.0;
    case RockType::Marble: return 3.5;
    case RockType::Obsidian: return 3.0;
    case RockType::Gneiss:
    case RockType::Granite:
    default: return 4.0;
  }
}

// ---- a face in tiers (rocks.md, "A face in tiers") ---------------------------------------------

// The tier coordinate and the layer's breaks. A tier is a band between two breaks; a break is a
// bedding plane of the formation where a resistant bed's top is near, else a level. The breaks are
// the world's (a function of the layer alone), so every block and every line cuts at the same ones.
struct Tiering {
  f64 tier = 12.0;
  bool bedded = false;  // the coordinate is the formation's, plumb metres; else the height
  gfx::FormationFrame frame{};
  gfx::FormationSequence beds{};
  f64 ny = 1.0;

  f64 at(f64 x, f64 y, f64 z) const {
    return bedded ? gfx::formation_coordinate(frame, x, y, z).coordinate / ny : y;
  }
  // Break k: `k tier`, moved onto the top of a resistant bed that a weaker one lies on when one is
  // within a third of a tier (the cliff-forming bed's top is where a face steps back).
  f64 break_at(i64 k) const {
    const f64 nominal = static_cast<f64>(k) * tier;
    if (!bedded || beds.count == 0) return nominal;
    const f64 period = static_cast<f64>(beds.period_mm) / 1000.0 / ny;
    f64 best = nominal;
    f64 best_d = tier / 3.0;
    for (u32 b = 0; b < beds.count; ++b) {
      const u32 above = (b + 1) % beds.count;
      if (!(beds.beds[b].resistance >= 0.5f) ||
          (beds.count > 1 && beds.beds[above].resistance >= 0.5f))
        continue;
      const f64 top = (wide(beds.beds[b].start_m) + wide(beds.beds[b].thickness_m)) / ny;
      const f64 at_top = top + std::floor((nominal - top) / period + 0.5) * period;
      const f64 d = std::fabs(at_top - nominal);
      if (d < best_d) {
        best_d = d;
        best = at_top;
      }
    }
    return best;
  }
};

Tiering tiering_of(const CliffLayer& layer) {
  Tiering t;
  t.tier = wide(layer.tier_m);
  // Beds that dip more steeply than 20 degrees do not run along a face: the tiers are levels.
  if (layer.formation.has_value() && std::fabs(layer.formation->dip_deg) <= 20.0f) {
    gfx::FormationParams field;
    if (gfx::pack_formation(*layer.formation, field, nullptr)) {
      t.bedded = true;
      t.frame = gfx::formation_frame(field);
      t.beds = field.beds;
      t.ny = t.frame.normal[1];
    }
  }
  return t;
}

// A tier's part of one vertex's profile: from (r_lo, y_lo) to (r_hi, y_hi), metres uphill of the
// traced foot and heights.
struct Part {
  i32 label = 0;
  f64 r_lo = 0.0;
  f64 y_lo = 0.0;
  f64 r_hi = 0.0;
  f64 y_hi = 0.0;
  bool lowest = false;
  bool topmost = false;
};
constexpr u32 k_max_parts = 24;

// One traced line into faces: measured up the slope, cut into tiers, stood off, seated, and written
// as recipes.
void faces_of_line(const CliffLayer& layer, const scene_gen::Ground& ground, const Lattice& l,
                   i32 bx, i32 bz, const Vector<Point2>& traced, Vector<CliffFace>& out,
                   u32* skipped) {
  const f64 step = l.spacing;
  Vector<Point2> line = resample(traced, step);
  smooth(line, 3);
  if (line.size() < 3) return;
  // **A line cut by the block's edge runs on past it** by `k_overlap`, so the face traced in the
  // next block overlaps this one's end instead of leaving the ground showing between two ends
  // and two tapering skirts. The renderer draws the union.
  constexpr f64 k_overlap = 12.0;
  const f64 bx0 = l.x(0.0);
  const f64 bz0 = l.z(0.0);
  const f64 bx1 = l.x(static_cast<f64>(l.cells));
  const f64 bz1 = l.z(static_cast<f64>(l.cells));
  const auto on_edge = [&](const Point2& p) {
    return std::fabs(p.x - bx0) < 1e-6 || std::fabs(p.x - bx1) < 1e-6 ||
           std::fabs(p.z - bz0) < 1e-6 || std::fabs(p.z - bz1) < 1e-6;
  };
  const auto extend = [&](const Point2& end, const Point2& inner) {
    const D3 d = normalized(D3{end.x - inner.x, 0.0, end.z - inner.z});
    return Point2{end.x + d.x * k_overlap, end.z + d.z * k_overlap};
  };
  if (on_edge(traced[0])) line[0] = extend(line[0], line[1]);
  if (on_edge(traced[traced.size() - 1]))
    line[line.size() - 1] = extend(line[line.size() - 1], line[line.size() - 2]);
  line = resample(line, step);
  const f64 tan_t = det::sin(wide(layer.min_slope_deg) * k_pi / 180.0) /
                    det::cos(wide(layer.min_slope_deg) * k_pi / 180.0);
  const f64 keep2 = (0.8 * tan_t) * (0.8 * tan_t);
  const u32 n = line.size();
  const Tiering tiers = tiering_of(layer);
  // A part thinner than this joins its neighbour: a break this close to the foot or the brow is
  // not cut.
  const f64 thin = tiers.tier / 3.0;

  // ---- per vertex: the normal, the slope's profile, and its parts ------------------------------
  // `prof_y` holds every vertex's profile, the traced foot first and then each step up the slope
  // while it stays steep (four fifths of the threshold, so a ledge part way up does not end the
  // face), at most 200 m up.
  Vector<f64> out_x(n), out_z(n), stand(n), whole_foot_y(n), whole_height(n);
  Vector<u32> prof_at(n + 1);
  Vector<f64> prof_y;
  Vector<Part> parts;
  Vector<u32> part_at(n + 1);
  i32 label_lo = std::numeric_limits<i32>::max();
  i32 label_hi = std::numeric_limits<i32>::min();
  for (u32 i = 0; i < n; ++i) {
    const Point2 a = line[i > 0 ? i - 1 : 0];
    const Point2 b = line[i + 1 < n ? i + 1 : n - 1];
    const D3 t = normalized(D3{b.x - a.x, 0.0, b.z - a.z});
    const D3 nrm{-t.z, 0.0, t.x};
    out_x[i] = nrm.x;
    out_z[i] = nrm.z;
    const f64 y0 = ground_at(ground, line[i].x, line[i].z);
    prof_at[i] = prof_y.size();
    prof_y.push_back(y0);
    u32 k = 0;
    f64 top = y0;
    while (k < 400) {
      const f64 qx = line[i].x - nrm.x * step * static_cast<f64>(k + 1);
      const f64 qz = line[i].z - nrm.z * step * static_cast<f64>(k + 1);
      if (slope2_at(ground, qx, qz, step) < keep2) break;
      ++k;
      top = ground_at(ground, qx, qz);
      prof_y.push_back(top);
      if (top - y0 > 200.0) break;
    }
    const f64 relief = relief_of(layer, type_for(layer, y0));
    const f64 rise = max2(1.0, top - y0);
    stand[i] = layer.standoff_m.has_value() ? wide(*layer.standoff_m) : 0.75 * relief + 0.03 * rise;
    whole_foot_y[i] = ground_at(ground, line[i].x + nrm.x * stand[i], line[i].z + nrm.z * stand[i]);
    whole_height[i] = min2(200.0, max2(1.0, top - 0.5 - whole_foot_y[i]));

    // The breaks this profile crosses, lowest first: where the tier coordinate first reaches each.
    const f64* py = &prof_y[prof_at[i]];
    Vector<f64> tau(k + 1);
    f64 tau_max = -1.0e30;
    for (u32 q = 0; q <= k; ++q) {
      const f64 r = step * static_cast<f64>(q);
      tau[q] = tiers.at(line[i].x - nrm.x * r, py[q], line[i].z - nrm.z * r);
      tau_max = max2(tau_max, tau[q]);
    }
    part_at[i] = parts.size();
    Part part;
    part.lowest = true;
    part.r_lo = 0.0;
    part.y_lo = y0;
    bool labelled = false;
    u32 made = 1;
    const i64 k_first = static_cast<i64>(std::floor(tau[0] / tiers.tier)) - 1;
    const i64 k_last = static_cast<i64>(std::floor(tau_max / tiers.tier)) + 2;
    for (i64 kb = k_first; kb <= k_last && made < k_max_parts; ++kb) {
      const f64 at_break = tiers.break_at(kb);
      if (!(at_break > tau[0])) continue;
      u32 q = 1;
      while (q <= k && tau[q] < at_break)
        ++q;
      if (q > k) break;
      const f64 f = (at_break - tau[q - 1]) / (tau[q] - tau[q - 1]);
      const f64 r = step * (static_cast<f64>(q - 1) + f);
      const f64 y = py[q - 1] + (py[q] - py[q - 1]) * f;
      if (y - part.y_lo < thin || top - y < thin || !(r > part.r_lo)) continue;
      if (!labelled) part.label = static_cast<i32>(kb - 1);
      labelled = true;
      part.r_hi = r;
      part.y_hi = y;
      parts.push_back(part);
      ++made;
      part = Part{};
      part.label = static_cast<i32>(kb);
      part.r_lo = r;
      part.y_lo = y;
    }
    if (!labelled) part.label = static_cast<i32>(std::floor(0.5 * (tau[0] + tau[k]) / tiers.tier));
    part.topmost = true;
    part.r_hi = step * static_cast<f64>(k);
    part.y_hi = top;
    parts.push_back(part);
    for (u32 q = part_at[i]; q < parts.size(); ++q) {
      label_lo = parts[q].label < label_lo ? parts[q].label : label_lo;
      label_hi = parts[q].label > label_hi ? parts[q].label : label_hi;
    }
  }
  prof_at[n] = prof_y.size();
  part_at[n] = parts.size();

  // The whole steep band decides whether the line is dressed at all: its length along the
  // stood-off foot and its mean height, as before the tiers.
  f64 whole_length = 0.0;
  f64 whole_mean = 0.0;
  for (u32 i = 0; i < n; ++i) {
    whole_mean += whole_height[i] / static_cast<f64>(n);
    if (i == 0) continue;
    const f64 dx =
        (line[i].x + out_x[i] * stand[i]) - (line[i - 1].x + out_x[i - 1] * stand[i - 1]);
    const f64 dz =
        (line[i].z + out_z[i] * stand[i]) - (line[i - 1].z + out_z[i - 1] * stand[i - 1]);
    whole_length += std::sqrt(dx * dx + dz * dz);
  }
  if (whole_length < wide(layer.min_length_m) || whole_mean < wide(layer.min_height_m)) return;
  const bool line_starts = !on_edge(traced[0]);  // the steep ground itself ends there
  const bool line_ends = !on_edge(traced[traced.size() - 1]);
  const f64 taper = wide(layer.taper_m);
  const f64 buried = wide(layer.face.buried);

  // What the tier below left at each vertex for the next one to stand on: the lowest its top can
  // be (the solver breaks a skyline back by up to 15 % of the height) and the next tier's own
  // `buried` under its foot, less half a metre, so the next tier's bottom edge is under that top.
  Vector<f64> lower_top(n, 1.0e30);
  Vector<f64> next_top(n);

  const auto part_of = [&](u32 i, i32 label) -> const Part* {
    for (u32 q = part_at[i]; q < part_at[i + 1]; ++q)
      if (parts[q].label == label) return &parts[q];
    return nullptr;
  };

  // One run of a tier, vertices [first, last] of the line: seated, tapered and written.
  const auto tier_run = [&](i32 label, u32 first, u32 last) {
    const u32 m = last - first + 1;
    Vector<f64> foot_x(m), foot_z(m), foot_y(m), height(m), batter(m), sink(m), base_r(m),
        base_y(m);
    u32 lowest_count = 0;
    f64 tallest = 1.0;
    for (u32 v = 0; v < m; ++v) {
      const u32 i = first + v;
      const Part& p = *part_of(i, label);
      const f64* py = &prof_y[prof_at[i]];
      const u32 steps = prof_at[i + 1] - prof_at[i] - 1;
      const f64 top_y = p.topmost ? p.y_hi - 0.5 : p.y_hi;
      // **The batter that keeps the tier in front of the ground**: the lean that passes at least
      // the standoff downhill of every point of the tier's own part of the profile — not the chord
      // of the part, which a slope that rounds over rises above.
      // It is measured from the part's own start on the profile (the traced foot, or the break),
      // never from the ground under the stood-off foot: a plane leaned from there is steeper than
      // the slope and stands metres off it at the top, where its top then cannot reach the ground.
      f64 lean = 1.5;
      if (p.lowest) ++lowest_count;
      base_r[v] = p.r_lo;
      base_y[v] = p.y_lo;
      for (u32 q = 1; q <= steps && step * static_cast<f64>(q) <= p.r_hi + 1e-9; ++q) {
        const f64 r = step * static_cast<f64>(q);
        const f64 up = py[q] - p.y_lo;
        if (r > p.r_lo && up > 1.0) lean = min2(lean, (r - p.r_lo) / up);
      }
      if (p.y_hi - base_y[v] > 1.0) lean = min2(lean, (p.r_hi - base_r[v]) / (p.y_hi - base_y[v]));
      height[v] = min2(200.0, max2(1.0, top_y - base_y[v]));
      batter[v] = max2(0.0, lean);
    }
    // The tier with the talus and the foot boulders: the lowest at most of its vertices.
    const bool lowest = 2 * lowest_count > m;
    smooth_values(height, 2);
    smooth_values(batter, 2);
    for (u32 v = 0; v < m; ++v) {
      const u32 i = first + v;
      // A tier above another stands on the lower one's top: its foot at the break, or lower where
      // the tier below ends lower, and its line moved out by what its lean then gains. The lowest
      // stands on the ground its own lean reaches, found in two looks.
      f64 under = lower_top[i];
      f64 out_m = stand[i] - base_r[v];
      if (part_of(i, label)->lowest) {
        under = whole_foot_y[i];
        for (u32 look = 0; look < 3; ++look) {
          // No further out than twice the standoff: under a line's end the ground can go on
          // falling, and a foot that chased it would swing out of the line.
          out_m = stand[i] + min2(stand[i], batter[v] * max2(0.0, base_y[v] - under));
          under = ground_at(ground, line[i].x + out_x[i] * out_m, line[i].z + out_z[i] * out_m);
        }
      }
      foot_y[v] = min2(base_y[v], under);
      const f64 extra = base_y[v] - foot_y[v];
      height[v] = min2(200.0, height[v] + extra);
      foot_x[v] = line[i].x + out_x[i] * out_m;
      foot_z[v] = line[i].z + out_z[i] * out_m;
      tallest = max2(tallest, height[v]);
    }
    // Where the tier below ends, this one's foot drops to the ground within a few vertices and its
    // line steps out with it: the line is smoothed once so the step is a bend, and the way the
    // face looks is the written line's own right, as the solver will take it.
    Vector<f64> look_x(m), look_z(m);
    {
      const Vector<f64> raw_x = foot_x, raw_z = foot_z;
      for (u32 v = 1; v + 1 < m; ++v) {
        foot_x[v] = 0.25 * raw_x[v - 1] + 0.5 * raw_x[v] + 0.25 * raw_x[v + 1];
        foot_z[v] = 0.25 * raw_z[v - 1] + 0.5 * raw_z[v] + 0.25 * raw_z[v + 1];
      }
      for (u32 v = 0; v < m; ++v) {
        const u32 a = v > 0 ? v - 1 : 0;
        const u32 b = v + 1 < m ? v + 1 : m - 1;
        const D3 t = normalized(D3{foot_x[b] - foot_x[a], 0.0, foot_z[b] - foot_z[a]});
        look_x[v] = -t.z;
        look_z[v] = t.x;
      }
    }
    Cliff recipe = layer.face;
    recipe.points.clear();
    recipe.type = type_for(layer, foot_y[0]);
    if (layer.formation.has_value()) recipe.formation = *layer.formation;
    const f64 back = cliff_back_m(recipe, tallest);
    // **Seated** (rocks.md, "A face in tiers"): no top proud of the ground behind it. A face's top
    // is flat from its crest to its back. Its first stretch is the rock's own top, in front of the
    // slope; then it goes into the ground, and from there to the back it must stay in: where the
    // ground behind the brow falls away again (a fin, a rounded crest narrower than the back) the
    // top would stand out of the far side as a wall's does. So a top that does not hold is lowered,
    // a quarter of a metre at a time, to the first height at which it enters the ground within
    // the back and the ground stays `k_under_m` over it from there on (lowering it moves its crest
    // out along the lean, so the answer is looked for, not stepped to). `sink` is how far a
    // vertex's whole section is then lowered under its foot: a face's height cannot go under a
    // metre.
    const auto seat = [&](u32 v) {
      // Whether a top at `top`, on a face `tall` high here, goes into the ground within the back
      // and stays `margin` under it from `k_settle_m` behind its entry (at the entry itself the
      // ground is the top's height by definition).
      const auto holds = [&](f64 top, f64 tall, f64 margin) {
        const f64 lean = batter[v] * tall;
        f64 entry = -1.0;
        for (u32 q = 0; q <= k_stations; ++q) {
          const f64 d = back * static_cast<f64>(q) / static_cast<f64>(k_stations);
          const f64 g = ground_at(ground, foot_x[v] - look_x[v] * (lean + d),
                                  foot_z[v] - look_z[v] * (lean + d));
          if (entry < 0.0 && g >= top) entry = d;
          if (entry >= 0.0 && d >= entry + k_settle_m && g < top + margin) return false;
        }
        return entry >= 0.0;
      };
      const f64 foot = foot_y[v] - sink[v];
      if (holds(foot + height[v], height[v], 0.0)) return;
      for (f64 tall = height[v] - 0.25; tall >= 1.0; tall -= 0.25) {
        if (!holds(foot + tall, tall, k_under_m)) continue;
        height[v] = tall;
        return;
      }
      height[v] = 1.0;
      for (f64 down = 0.5; sink[v] + down < 200.0; down += 0.5) {
        if (!holds(foot - down + 1.0, 1.0, k_under_m)) continue;
        sink[v] += down;
        return;
      }
      sink[v] = 200.0;
    };
    for (u32 v = 0; v < m; ++v)
      seat(v);
    Vector<f64> s(m);
    for (u32 v = 1; v < m; ++v) {
      const f64 dx = foot_x[v] - foot_x[v - 1];
      const f64 dz = foot_z[v] - foot_z[v - 1];
      s[v] = s[v - 1] + std::sqrt(dx * dx + dz * dz);
    }
    const f64 total = s[m - 1];
    // The ends taper. Where the steep ground itself ends (not where a block's edge cut the line,
    // which the next block's face runs on from) the tier comes down to its least height and goes
    // under its foot over `taper_m` of its line; where the tier itself ends inside the line (its
    // band has left the steep ground, and the tier beside takes over) over four spacings.
    const f64 taper_a = first == 0 ? (line_starts ? taper : 0.0) : 4.0 * step;
    const f64 taper_b = last + 1 == n ? (line_ends ? taper : 0.0) : 4.0 * step;
    for (u32 v = 0; v < m; ++v) {
      f64 w = 1.0;
      if (taper_a > 0.0) w = min2(w, s[v] / taper_a);
      if (taper_b > 0.0) w = min2(w, (total - s[v]) / taper_b);
      if (w < 1.0) {
        w = max2(0.0, w);
        w = w * w * (3.0 - 2.0 * w);
        height[v] = 1.0 + (height[v] - 1.0) * w;
        sink[v] = min2(200.0, sink[v] + (1.0 - w) * (1.0 + k_under_m));
        // A tapered section is a lower one, with its crest further out: seated again.
        seat(v);
      }
      next_top[first + v] = foot_y[v] - sink[v] + 0.85 * height[v] + max2(0.0, buried - 0.5);
    }
    if (total < 1.0) return;
    f64 run_mean = 0.0;
    for (u32 v = 0; v < m; ++v)
      run_mean += height[v] / static_cast<f64>(m);
    // The tier's share of the layer's budget: its height's share of the whole steep band's.
    const f64 share = min2(1.0, max2(0.1, run_mean / whole_mean));
    // Cut into faces no longer than `max_length_m`, of equal length.
    const u32 pieces = static_cast<u32>(std::ceil(total / wide(layer.max_length_m) - 1e-9));
    u32 from = 0;
    for (u32 piece = 0; piece < pieces; ++piece) {
      const f64 end = piece + 1 == pieces ? total : total * static_cast<f64>(piece + 1) / pieces;
      u32 to = from;
      while (to + 1 < m && s[to + 1] <= end + 1e-9)
        ++to;
      if (to <= from) break;
      const u32 begin = from;
      from = to;
      const f64 length = s[to] - s[begin];
      f64 mean = 0.0;
      for (u32 v = begin; v <= to; ++v)
        mean += height[v];
      mean /= static_cast<f64>(to - begin + 1);
      // **The line the recipe is written with is merged to the validator's bound.** The stood-off
      // foot line can fold where the ground bends sharper than the standoff (the inside of a
      // hairpin): consecutive vertices then come within `k_min_gap` of each other, which `Cliff`'s
      // validation refuses, and one such face refuses the whole scene. Keep the first, drop each
      // next one while it is within the gap of the last kept; a piece left under two vertices or a
      // metre of line is skipped and counted.
      Vector<u32> kept;
      for (u32 v = begin; v <= to; ++v) {
        if (!kept.empty()) {
          const f64 dx = foot_x[v] - foot_x[kept[kept.size() - 1]];
          const f64 dz = foot_z[v] - foot_z[kept[kept.size() - 1]];
          if (dx * dx + dz * dz < k_min_gap * k_min_gap) continue;
        }
        kept.push_back(v);
      }
      f64 kept_length = 0.0;
      for (u32 q = 1; q < kept.size(); ++q) {
        const f64 dx = foot_x[kept[q]] - foot_x[kept[q - 1]];
        const f64 dz = foot_z[kept[q]] - foot_z[kept[q - 1]];
        kept_length += std::sqrt(dx * dx + dz * dz);
      }
      if (kept.size() < 2 || kept_length < 1.0) {
        if (skipped != nullptr) ++*skipped;
        ENGINE_LOG_INFO(log_cliffs, "cliff face skipped: its line collapsed under the vertex gap",
                        log::field("block_x", static_cast<i64>(bx)),
                        log::field("block_z", static_cast<i64>(bz)),
                        log::field("vertices", static_cast<u64>(to - begin + 1)));
        continue;
      }
      CliffFace face;
      face.x_mm = scene_gen::nearest_mm(foot_x[begin]);
      face.z_mm = scene_gen::nearest_mm(foot_z[begin]);
      face.y = std::floor(foot_y[begin] * 1000.0 + 0.5) / 1000.0;
      face.anchor_x_mm = scene_gen::nearest_mm(foot_x[(begin + to) / 2]);
      face.anchor_z_mm = scene_gen::nearest_mm(foot_z[(begin + to) / 2]);
      face.block_x = bx;
      face.block_z = bz;
      face.length = length;
      face.mean_height = mean;
      face.tier = label;
      face.lowest = lowest;
      // The identity: the line in whole millimetres and its heights in centimetres.
      u64 id = mix64(0x434C4946ull ^ static_cast<u64>(kept.size()));
      for (const u32 v : kept) {
        id = hash_combine(id, static_cast<u64>(scene_gen::nearest_mm(foot_x[v])));
        id = hash_combine(id, static_cast<u64>(scene_gen::nearest_mm(foot_z[v])));
        id = hash_combine(id,
                          static_cast<u64>(static_cast<i64>(std::floor(height[v] * 100.0 + 0.5))));
        id = hash_combine(
            id,
            static_cast<u64>(static_cast<i64>(std::floor((foot_y[v] - sink[v]) * 100.0 + 0.5))));
      }
      face.identity = id;
      Cliff c = layer.face;
      c.points.clear();
      // A generated face stands in a slope: no back and no bottom (rocks.md, "Faces in the slope").
      c.open = true;
      c.type = type_for(layer, face.y);
      c.seed = mix64(layer.seed ^ id);
      c.height = static_cast<f32>(min2(200.0, max2(1.0, mean)));
      c.datum_y = static_cast<f32>(-face.y);
      c.triangle_budget = static_cast<u32>(
          max2(2000.0, std::floor(static_cast<f64>(layer.face.triangle_budget) * share + 0.5)));
      if (lowest) {
        // The skirt is the whole steep band's, standing against its lowest tier.
        c.talus =
            static_cast<f32>(min2(0.59, wide(layer.face.talus) * whole_mean / max2(1.0, mean)));
      } else {
        c.talus = 0.0f;
        c.talus_blocks = 0;
      }
      const f64 ox = static_cast<f64>(face.x_mm) / 1000.0;
      const f64 oz = static_cast<f64>(face.z_mm) / 1000.0;
      // The scene's formation, and where this recipe's origin stands in it: the face's beds are
      // then the terrain's bands (rocks.md, "Geology on the world formation (W85)").
      if (layer.formation.has_value()) {
        c.formation = *layer.formation;
        c.world_origin = WorldPos{ox, face.y, oz};
      }
      // **A tier's rock is its beds'** (rocks.md, "A face in tiers"): with a bedded formation the
      // face's colour is the type's (or the layer's tint) times the mean tint of the beds its
      // height cuts at its middle vertex, and its roughness plus their mean offset, so tiers cut
      // from different members read apart, as the ledges' units do.
      if (tiers.bedded && tiers.beds.count > 0) {
        const u32 mid = kept[kept.size() / 2];
        const f64 low = foot_y[mid] - sink[mid];
        f64 sum[4] = {0.0, 0.0, 0.0, 0.0};
        constexpr u32 k_samples = 16;
        for (u32 q = 0; q < k_samples; ++q) {
          const f64 y =
              low + height[mid] * (static_cast<f64>(q) + 0.5) / static_cast<f64>(k_samples);
          const f64 coordinate =
              gfx::formation_coordinate(tiers.frame, foot_x[mid], y, foot_z[mid]).coordinate;
          const Vec4 v = gfx::formation_bed(tiers.beds, coordinate).value;
          sum[0] += wide(v.x);
          sum[1] += wide(v.y);
          sum[2] += wide(v.z);
          sum[3] += wide(v.w);
        }
        assets::Material base = rock_material(c.type);
        material_factors(c.material, base.base_color, base.roughness);
        const f64 count = static_cast<f64>(k_samples);
        const f64 rgb[3] = {wide(base.base_color.x), wide(base.base_color.y),
                            wide(base.base_color.z)};
        std::array<f32, 3> tint{};
        for (u32 k = 0; k < 3; ++k)
          tint[k] = static_cast<f32>(std::clamp(rgb[k] * sum[k] / count, 0.0, 1.0));
        c.material.tint = tint;
        c.material.roughness_factor =
            static_cast<f32>(std::clamp(wide(base.roughness) + sum[3] / count, 0.02, 1.0));
      }
      // The back the top was seated to, so the solver's is the same one.
      c.depth = static_cast<f32>(back);
      for (const u32 v : kept) {
        CliffPoint p;
        p.x = static_cast<f32>(foot_x[v] - ox);
        p.y = static_cast<f32>(foot_y[v] - sink[v] - face.y);
        p.z = static_cast<f32>(foot_z[v] - oz);
        p.height = static_cast<f32>(height[v]);
        p.batter = static_cast<f32>(batter[v]);
        c.points.push_back(p);
      }
      face.cliff = c;
      out.push_back(face);
    }
  };

  // The tiers, lowest first, each in runs of consecutive vertices that have it.
  for (i32 label = label_lo; label <= label_hi; ++label) {
    for (u32 i = 0; i < n; ++i)
      next_top[i] = 1.0e30;
    u32 i = 0;
    while (i < n) {
      if (part_of(i, label) == nullptr) {
        ++i;
        continue;
      }
      u32 last = i;
      while (last + 1 < n && part_of(last + 1, label) != nullptr)
        ++last;
      if (last > i) tier_run(label, i, last);
      i = last + 1;
    }
    for (u32 q = 0; q < n; ++q)
      if (next_top[q] < 1.0e29) lower_top[q] = next_top[q];
  }
}

}  // namespace

bool cliff_layer_in_range(const CliffLayer& layer, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.rocks.CliffLayer.") + field + " is out of range (" + range + ")";
    return false;
  };
  const auto in = [](f64 v, f64 lo, f64 hi) { return v >= lo && v <= hi; };
  if (!(layer.region_min[0] < layer.region_max[0] && layer.region_min[1] < layer.region_max[1]))
    return bad("region_min", "below region_max on both axes");
  if (!in(wide(layer.spacing_m), 0.5, 32.0)) return bad("spacing_m", "0.5 to 32 m");
  if (!in(wide(layer.block_m), 16.0, 4096.0)) return bad("block_m", "16 to 4,096 m");
  const i64 spacing_mm = scene_gen::nearest_mm(wide(layer.spacing_m));
  const i64 block_mm = scene_gen::nearest_mm(wide(layer.block_m));
  if (block_mm % spacing_mm != 0) return bad("block_m", "a whole number of spacings");
  if (!in(wide(layer.min_slope_deg), 30.0, 85.0)) return bad("min_slope_deg", "30 to 85");
  if (!in(wide(layer.min_length_m), 4.0, 1000.0)) return bad("min_length_m", "4 to 1,000 m");
  if (!in(wide(layer.min_height_m), 1.0, 200.0)) return bad("min_height_m", "1 to 200 m");
  if (!in(wide(layer.max_length_m), 16.0, 1000.0)) return bad("max_length_m", "16 to 1,000 m");
  if (layer.standoff_m.has_value() && !in(wide(*layer.standoff_m), 0.0, 20.0))
    return bad("standoff_m", "0 to 20 m");
  if (!in(wide(layer.clear_m), 0.0, 100.0)) return bad("clear_m", "0 to 100 m");
  if (!in(wide(layer.foot_per_100m), 0.0, 100.0)) return bad("foot_per_100m", "0 to 100");
  if (!(wide(layer.foot_scale_min) > 0.0 && layer.foot_scale_min <= layer.foot_scale_max &&
        wide(layer.foot_scale_max) <= 10.0))
    return bad("foot_scale_min", "above 0, and no more than foot_scale_max, at most 10");
  if (!in(wide(layer.taper_m), 0.0, 100.0)) return bad("taper_m", "0 to 100 m");
  if (!in(wide(layer.tier_m), 3.0, 200.0)) return bad("tier_m", "3 to 200 m");
  if (layer.formation.has_value()) {
    std::string why;
    if (!gfx::validate_formation(*layer.formation, &why)) {
      error = "engine.rocks.CliffLayer." + why;
      return false;
    }
  }
  // The face's own fields, on a stand-in line: the ground gives the line and the heights.
  Cliff face = layer.face;
  face.points.clear();
  CliffPoint a;
  CliffPoint b;
  b.x = 10.0f;
  face.points.push_back(a);
  face.points.push_back(b);
  std::string why;
  if (!cliff_in_range(face, why)) {
    error = "engine.rocks.CliffLayer.face: " + why;
    return false;
  }
  return true;
}

bool fit_cliff_ground(Cliff& cliff, const scene_gen::Ground& ground, WorldPos origin, f32 spacing_m,
                      std::string* error) {
  if (!(spacing_m >= 0.25f && spacing_m <= 32.0f) || !std::isfinite(origin.x) ||
      !std::isfinite(origin.y) || !std::isfinite(origin.z)) {
    if (error) *error = "cliff ground fit needs finite origin and spacing 0.25..32 m";
    return false;
  }
  Cliff candidate = cliff;
  candidate.ground.reset();
  assets::MeshData mesh;
  CliffReport report;
  if (!solve_cliff(candidate, mesh, &report, error)) return false;
  if (report.skirt_triangles == 0 && report.block_triangles == 0) {
    cliff = std::move(candidate);
    return true;
  }
  f64 x0 = 1e9, z0 = 1e9, x1 = -1e9, z1 = -1e9;
  // Only the apron and its blocks need support; the face's buried back can be much wider.
  for (u32 i = report.face_triangles * 3; i < mesh.indices.size(); ++i) {
    const auto p = mesh.positions[mesh.indices[i]];
    x0 = min2(x0, wide(p.x));
    z0 = min2(z0, wide(p.z));
    x1 = max2(x1, wide(p.x));
    z1 = max2(z1, wide(p.z));
  }
  CliffGround patch;
  f64 spacing = wide(spacing_m);
  for (;;) {
    patch.min_x_m = static_cast<f32>(std::floor(x0 / spacing - 1.0) * spacing);
    patch.min_z_m = static_cast<f32>(std::floor(z0 / spacing - 1.0) * spacing);
    patch.width = static_cast<u32>(std::ceil((x1 - wide(patch.min_x_m)) / spacing)) + 2;
    patch.depth = static_cast<u32>(std::ceil((z1 - wide(patch.min_z_m)) / spacing)) + 2;
    if (patch.width <= 2049 && patch.depth <= 2049 &&
        static_cast<u64>(patch.width) * patch.depth <= 1048576)
      break;
    spacing *= 2.0;
    if (spacing > 32.0) {
      if (error) *error = "cliff ground footprint exceeds the sample budget at 32 m";
      return false;
    }
  }
  patch.spacing_m = static_cast<f32>(spacing);
  patch.heights.reserve(patch.width * patch.depth);
  for (u32 j = 0; j < patch.depth; ++j)
    for (u32 i = 0; i < patch.width; ++i) {
      const f64 h = ground_at(ground, origin.x + wide(patch.min_x_m) + i * spacing,
                              origin.z + wide(patch.min_z_m) + j * spacing) -
                    origin.y;
      if (!(h >= -1e5 && h <= 1e5)) {
        if (error) *error = "cliff ground returned a non-finite or out-of-range height";
        return false;
      }
      patch.heights.push_back(static_cast<f32>(h));
    }
  candidate.ground = std::move(patch);
  cliff = std::move(candidate);
  return true;
}

bool trace_cliff_block(const CliffLayer& layer, const scene_gen::Ground& ground, i32 bx, i32 bz,
                       Vector<CliffFace>& out, std::string* error, u32* faces_skipped) {
  std::string why;
  if (!cliff_layer_in_range(layer, why)) {
    if (error != nullptr) *error = why;
    return false;
  }
  Lattice l;
  l.spacing_mm = scene_gen::nearest_mm(wide(layer.spacing_m));
  l.spacing = static_cast<f64>(l.spacing_mm) / 1000.0;
  const i64 block_mm = scene_gen::nearest_mm(wide(layer.block_m));
  l.cells = static_cast<i32>(block_mm / l.spacing_mm);
  l.x0_mm = static_cast<i64>(bx) * block_mm;
  l.z0_mm = static_cast<i64>(bz) * block_mm;
  const u32 side = l.side();
  l.h.resize(side * side);
  for (u32 j = 0; j < side; ++j) {
    for (u32 i = 0; i < side; ++i) {
      const i64 x = l.x0_mm + (static_cast<i64>(i) - l.apron) * l.spacing_mm;
      const i64 z = l.z0_mm + (static_cast<i64>(j) - l.apron) * l.spacing_mm;
      l.h[j * side + i] = ground.surface_mm(x, z);
    }
  }
  // The slope at the block's points, and the field marching squares walks: steeper than the
  // threshold is positive.
  const f64 tan_t = det::sin(wide(layer.min_slope_deg) * k_pi / 180.0) /
                    det::cos(wide(layer.min_slope_deg) * k_pi / 180.0);
  const u32 points = static_cast<u32>(l.cells + 1) * static_cast<u32>(l.cells + 1);
  l.gx.resize(points);
  l.gz.resize(points);
  Vector<f64> f(points);
  for (i32 j = 0; j <= l.cells; ++j) {
    for (i32 i = 0; i <= l.cells; ++i) {
      const f64 gx = (l.height(i + 1, j) - l.height(i - 1, j)) / (2.0 * l.spacing);
      const f64 gz = (l.height(i, j + 1) - l.height(i, j - 1)) / (2.0 * l.spacing);
      l.gx[l.at(i, j)] = gx;
      l.gz[l.at(i, j)] = gz;
      f[l.at(i, j)] = gx * gx + gz * gz - tan_t * tan_t;
    }
  }
  // Marching squares over the block's cells: a segment where the field changes sign, oriented
  // with the steep side on its left; kept when that side is uphill — the foot of the steep ground,
  // not its brow or its ends.
  const auto edge_id = [&](i32 i, i32 j, bool vertical) {
    return (static_cast<u64>(static_cast<u32>(j)) * static_cast<u64>(l.cells + 1) +
            static_cast<u64>(static_cast<u32>(i))) *
               2u +
           (vertical ? 1u : 0u);
  };
  Vector<Segment> feet;
  for (i32 j = 0; j < l.cells; ++j) {
    for (i32 i = 0; i < l.cells; ++i) {
      const f64 v[4] = {f[l.at(i, j)], f[l.at(i + 1, j)], f[l.at(i + 1, j + 1)], f[l.at(i, j + 1)]};
      const u32 mask = (v[0] > 0.0 ? 1u : 0u) | (v[1] > 0.0 ? 2u : 0u) | (v[2] > 0.0 ? 4u : 0u) |
                       (v[3] > 0.0 ? 8u : 0u);
      if (mask == 0u || mask == 15u) continue;
      // The cell's four edges: bottom (0-1), right (1-2), top (3-2), left (0-3).
      struct Edge {
        bool crossed;
        u64 id;
        D3 at;
      };
      Edge e[4];
      const i32 ci[4] = {i, i + 1, i + 1, i};
      const i32 cj[4] = {j, j, j + 1, j + 1};
      const u32 ea[4] = {0, 1, 3, 0};
      const u32 eb[4] = {1, 2, 2, 3};
      for (u32 k = 0; k < 4; ++k) {
        const f64 fa = v[ea[k]];
        const f64 fb = v[eb[k]];
        e[k].crossed = (fa > 0.0) != (fb > 0.0);
        e[k].id = k == 0   ? edge_id(i, j, false)
                  : k == 1 ? edge_id(i + 1, j, true)
                  : k == 2 ? edge_id(i, j + 1, false)
                           : edge_id(i, j, true);
        if (e[k].crossed) e[k].at = crossing(l, ci[ea[k]], cj[ea[k]], ci[eb[k]], cj[eb[k]], fa, fb);
      }
      // Pairs of crossed edges: two, or four at a saddle, joined as the cell's centre says.
      u32 pairs[2][2] = {{0, 0}, {0, 0}};
      u32 count = 0;
      u32 crossed[4] = {0, 0, 0, 0};
      u32 crossings = 0;
      for (u32 k = 0; k < 4; ++k) {
        if (e[k].crossed) crossed[crossings++] = k;
      }
      if (crossings == 2) {
        pairs[0][0] = crossed[0];
        pairs[0][1] = crossed[1];
        count = 1;
      } else if (crossings == 4) {
        const bool centre = (v[0] + v[1] + v[2] + v[3]) * 0.25 > 0.0;
        // With the centre steep, the steep corners join across it: cut off the flat corners.
        const bool corner0_steep = v[0] > 0.0;
        if (centre == corner0_steep) {
          pairs[0][0] = 0;  // corner 1 cut off by edges 0 and 1, corner 3 by 2 and 3
          pairs[0][1] = 1;
          pairs[1][0] = 2;
          pairs[1][1] = 3;
        } else {
          pairs[0][0] = 3;  // corner 0 cut off by edges 3 and 0, corner 2 by 1 and 2
          pairs[0][1] = 0;
          pairs[1][0] = 1;
          pairs[1][1] = 2;
        }
        count = 2;
      }
      for (u32 p = 0; p < count; ++p) {
        Segment seg;
        seg.from_edge = e[pairs[p][0]].id;
        seg.to_edge = e[pairs[p][1]].id;
        seg.from = e[pairs[p][0]].at;
        seg.to = e[pairs[p][1]].at;
        const D3 d = seg.to - seg.from;
        const f64 len = std::sqrt(d.x * d.x + d.z * d.z);
        if (!(len > 1e-9)) continue;
        const D3 left{d.z / len, 0.0, -d.x / len};
        const D3 mid = (seg.from + seg.to) * 0.5;
        const f64 eps = 0.05 * l.spacing;
        const f64 ahead = bilinear(l, f, i, j, mid.x + left.x * eps, mid.z + left.z * eps) -
                          bilinear(l, f, i, j, mid.x - left.x * eps, mid.z - left.z * eps);
        D3 inside = left;
        if (ahead < 0.0) {
          const u64 te = seg.from_edge;
          seg.from_edge = seg.to_edge;
          seg.to_edge = te;
          const D3 tp = seg.from;
          seg.from = seg.to;
          seg.to = tp;
          inside = left * -1.0;
        }
        const f64 hx = bilinear(l, l.gx, i, j, mid.x, mid.z);
        const f64 hz = bilinear(l, l.gz, i, j, mid.x, mid.z);
        const f64 g = std::sqrt(hx * hx + hz * hz);
        if (g > 0.0 && inside.x * hx + inside.z * hz > 0.5 * g) feet.push_back(seg);
      }
    }
  }
  // Join the feet into lines by their shared edges, in the order they were found: a line begins
  // at a foot no other foot leads into, then the closed loops, each opened at its first foot.
  HashMap<u64, u32> starting;
  HashMap<u64, u32> ending;
  for (u32 k = 0; k < feet.size(); ++k) {
    starting[feet[k].from_edge] = k;
    ending[feet[k].to_edge] = k;
  }
  const u32 first_face = out.size();
  Vector<u8> used(feet.size(), 0);
  const auto walk = [&](u32 k0) {
    Vector<Point2> line;
    line.push_back(Point2{feet[k0].from.x, feet[k0].from.z});
    u32 k = k0;
    while (used[k] == 0) {
      used[k] = 1;
      line.push_back(Point2{feet[k].to.x, feet[k].to.z});
      const u32* next = starting.find_value(feet[k].to_edge);
      if (next == nullptr) break;
      k = *next;
    }
    faces_of_line(layer, ground, l, bx, bz, line, out, faces_skipped);
  };
  for (u32 k = 0; k < feet.size(); ++k) {
    if (used[k] == 0 && ending.find_value(feet[k].from_edge) == nullptr) walk(k);
  }
  for (u32 k = 0; k < feet.size(); ++k) {
    if (used[k] == 0) walk(k);
  }
  for (u32 i = first_face; i < out.size(); ++i) {
    auto& face = out[i];
    const WorldPos origin{static_cast<f64>(face.x_mm) / 1000.0, face.y,
                          static_cast<f64>(face.z_mm) / 1000.0};
    // Only a skirt and its blocks lie on the ground: a tier with neither needs no patch.
    if (!(face.cliff.talus > 0.0f) && face.cliff.talus_blocks == 0) continue;
    if (!fit_cliff_ground(face.cliff, ground, origin,
                          static_cast<f32>(min2(1.0, wide(layer.spacing_m) * 0.5)), error)) {
      out.resize(first_face);
      return false;
    }
  }
  return true;
}

namespace {

// ---- the placement generator (scene_gen.h) -----------------------------------------------------

struct Entry {
  std::string where;
  CliffLayer layer;
  Vector<CliffFace> faces;  // the region's, by block (rows of x in order of z), then as traced
  Vector<scene_gen::PlacementMesh> meshes;  // a face's recipe each, in `faces`' order, then the
                                            // foot boulder's when the layer names one
  i64 tile_mm = 32000;
};

std::string resolve(std::string_view dir, const std::string& path) {
  return io::is_absolute_path(path) || dir.empty() ? path : io::join_path(dir, path);
}

std::string schema_errors(const schema::ReadContext& ctx) {
  std::string text;
  for (const schema::Diagnostic& d : ctx.diagnostics) {
    if (!text.empty()) text += "; ";
    text += d.path.empty() ? d.message : d.path + ": " + d.message;
  }
  return text.empty() ? std::string("not an engine.rocks.CliffLayer") : text;
}

i64 floor_div(i64 a, i64 b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

bool open(const JsonValue& params, const scene_gen::Context& context, void** state,
          std::string* error) {
  const auto fail = [&](std::string sentence) {
    if (error != nullptr) *error = std::move(sentence);
    return false;
  };
  auto entry = std::make_unique<Entry>();
  entry->where = std::string(context.where);
  schema::ReadContext ctx;
  if (!schema::from_json(entry->layer, params, ctx) || !ctx.ok())
    return fail(entry->where + ": " + schema_errors(ctx));
  const CliffLayer& layer = entry->layer;
  std::string why;
  if (!cliff_layer_in_range(layer, why)) return fail(entry->where + ": " + why);
  entry->tile_mm = scene_gen::nearest_mm(static_cast<f64>(context.tile_size));
  if (entry->tile_mm <= 0) entry->tile_mm = 32000;
  // Where the recipes go: where the layer says; else the host's derived-data root; else the
  // repository's, found from the scene's directory; else `cliffs` beside the scene.
  std::string dir;
  if (!layer.recipes.empty()) {
    dir = resolve(context.dir, layer.recipes);
  } else {
    std::string ddc(context.ddc);
    if (ddc.empty()) {
      std::error_code ec;
      const std::filesystem::path from = std::filesystem::absolute(
          std::filesystem::path(context.dir.empty() ? "." : std::string(context.dir)), ec);
      if (!ec) ddc = geometry::find_ddc_root(from.generic_string());
    }
    dir = !ddc.empty() ? io::join_path(ddc, "cliffs") : resolve(context.dir, "cliffs");
  }
  // Every block the region touches, rows of x in order of z; a face is the region's when its
  // origin is inside it.
  const i64 block_mm = scene_gen::nearest_mm(wide(layer.block_m));
  const i64 x0 = scene_gen::nearest_mm(layer.region_min[0]);
  const i64 z0 = scene_gen::nearest_mm(layer.region_min[1]);
  const i64 x1 = scene_gen::nearest_mm(layer.region_max[0]);
  const i64 z1 = scene_gen::nearest_mm(layer.region_max[1]);
  const i64 bx0 = floor_div(x0, block_mm);
  const i64 bz0 = floor_div(z0, block_mm);
  const i64 bx1 = floor_div(x1 - 1, block_mm);
  const i64 bz1 = floor_div(z1 - 1, block_mm);
  if ((bx1 - bx0 + 1) * (bz1 - bz0 + 1) > 65536)
    return fail(entry->where + ": its region is more than 65,536 blocks of block_m");
  u32 skipped = 0;
  for (i64 bz = bz0; bz <= bz1; ++bz) {
    for (i64 bx = bx0; bx <= bx1; ++bx) {
      Vector<CliffFace> faces;
      if (!trace_cliff_block(layer, context.ground, static_cast<i32>(bx), static_cast<i32>(bz),
                             faces, &why, &skipped))
        return fail(entry->where + ": " + why);
      for (const CliffFace& face : faces) {
        if (face.anchor_x_mm >= x0 && face.anchor_x_mm < x1 && face.anchor_z_mm >= z0 &&
            face.anchor_z_mm < z1)
          entry->faces.push_back(face);
      }
    }
  }
  if (!entry->faces.empty() && io::make_directories(dir) != io::Status::Ok)
    return fail(entry->where + ": cannot create " + dir);
  for (const CliffFace& face : entry->faces) {
    RockFile file;
    file.shape = RockShape::Cliff;
    file.cliff = face.cliff;
    const std::string text = canonical_rock_json(file);
    char name[48];
    std::snprintf(name, sizeof(name), "cliff-%016llx.rock.json",
                  static_cast<unsigned long long>(rock_source_hash(file)));
    scene_gen::PlacementMesh mesh;
    mesh.path = io::join_path(dir, name);
    mesh.name = std::string("cliffs/") + name;
    if (!io::exists(mesh.path)) {
      const io::Status status = io::write_file_atomic(mesh.path, text);
      if (status != io::Status::Ok)
        return fail(entry->where + ": cannot write " + mesh.path + ": " + io::status_name(status));
    }
    entry->meshes.push_back(std::move(mesh));
  }
  if (!layer.foot_mesh.empty() && layer.foot_per_100m > 0.0f) {
    scene_gen::PlacementMesh foot;
    foot.path = resolve(context.dir, layer.foot_mesh);
    foot.name = "cliffs/" + std::string(io::file_name(foot.path));
    entry->meshes.push_back(std::move(foot));
  }
  ENGINE_LOG_INFO(log_cliffs, "cliffs traced", log::field("entry", entry->where),
                  log::field("layer", layer.name), log::field("faces", entry->faces.size()),
                  log::field("skipped", static_cast<u64>(skipped)),
                  log::field("blocks", static_cast<u64>((bx1 - bx0 + 1) * (bz1 - bz0 + 1))));
  *state = entry.release();
  return true;
}

void close(void* state) noexcept { delete static_cast<Entry*>(state); }

// Whether an earlier placement's footprint comes within `clear_m` of the face's line.
bool cleared(const Entry& entry, const CliffFace& face, const scene_gen::Context& context) {
  const f64 clear = wide(entry.layer.clear_m);
  for (const scene_gen::Footprint& f : context.earlier) {
    const f64 reach = f.radius + clear;
    for (const CliffPoint& p : face.cliff.points) {
      const f64 dx = static_cast<f64>(face.x_mm - f.x_mm) / 1000.0 + wide(p.x);
      const f64 dz = static_cast<f64>(face.z_mm - f.z_mm) / 1000.0 + wide(p.z);
      if (dx * dx + dz * dz < reach * reach) return false;
    }
  }
  return true;
}

// The face's instance at its origin, and its foot boulders: seeded from its identity, along its
// line and out from its foot over the skirt, on the ground.
void place_face(const Entry& entry, u32 index, u32 mesh, u32 foot_mesh, bool with_foot,
                const scene_gen::Context& context, scene_gen::Placements& out) {
  const CliffFace& face = entry.faces[index];
  scene_gen::Placement placement;
  placement.position =
      WorldPos{static_cast<f64>(face.x_mm) / 1000.0, face.y, static_cast<f64>(face.z_mm) / 1000.0};
  placement.mesh = mesh;
  out.instances.push_back(placement);
  ++out.things;
  if (!with_foot || !face.lowest) return;
  const CliffLayer& layer = entry.layer;
  const u32 count =
      static_cast<u32>(std::floor(face.length / 100.0 * wide(layer.foot_per_100m) + 0.5));
  Rng rng(mix64(face.identity ^ 0x464F4F54ull));  // "FOOT"
  const Vector<CliffPoint>& pts = face.cliff.points;
  const f64 skirt = wide(layer.face.talus) * face.mean_height / 0.7;
  for (u32 b = 0; b < count; ++b) {
    const u32 seg = static_cast<u32>(rng.unit() * static_cast<f64>(pts.size() - 1));
    const f64 t = rng.unit();
    const CliffPoint& a = pts[seg];
    const CliffPoint& c = pts[seg + 1 < pts.size() ? seg + 1 : seg];
    const D3 d = normalized(D3{wide(c.x) - wide(a.x), 0.0, wide(c.z) - wide(a.z)});
    const f64 out_m = rng.range(1.0, skirt + 6.0);
    const f64 x =
        static_cast<f64>(face.x_mm) / 1000.0 + lerp(wide(a.x), wide(c.x), t) - d.z * out_m;
    const f64 z =
        static_cast<f64>(face.z_mm) / 1000.0 + lerp(wide(a.z), wide(c.z), t) + d.x * out_m;
    const f64 yaw = rng.range(0.0, 2.0 * k_pi);
    const f64 scale = rng.range(wide(layer.foot_scale_min), wide(layer.foot_scale_max));
    scene_gen::Placement p;
    p.position = WorldPos{x, ground_at(context.ground, x, z), z};
    p.rotation = Quat{0.0f, static_cast<f32>(det::sin(yaw * 0.5)), 0.0f,
                      static_cast<f32>(det::cos(yaw * 0.5))};
    const f32 s = static_cast<f32>(scale);
    p.scale = Vec3{s, s, s};
    p.mesh = foot_mesh;
    p.tag = 1;
    out.instances.push_back(p);
  }
}

bool has_foot(const Entry& entry) {
  return !entry.layer.foot_mesh.empty() && entry.layer.foot_per_100m > 0.0f;
}

bool expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
            std::string*) {
  const Entry& entry = *static_cast<const Entry*>(state);
  const bool foot = has_foot(entry);
  u32 foot_mesh = 0;
  Vector<u32> placed;
  for (u32 i = 0; i < entry.faces.size(); ++i) {
    if (cleared(entry, entry.faces[i], context)) placed.push_back(i);
  }
  // The meshes the instances name, and nothing a footprint cleared: each face's, then the foot's.
  for (const u32 i : placed)
    out.meshes.push_back(entry.meshes[i]);
  if (foot) {
    foot_mesh = out.meshes.size();
    out.meshes.push_back(entry.meshes[entry.meshes.size() - 1]);
  }
  for (u32 k = 0; k < placed.size(); ++k)
    place_face(entry, placed[k], k, foot_mesh, foot, context, out);
  return true;
}

bool meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
            std::string*) {
  const Entry& entry = *static_cast<const Entry*>(state);
  for (const scene_gen::PlacementMesh& mesh : entry.meshes)
    out.push_back(mesh);
  return true;
}

scene_gen::TileCoord tile_of(const Entry& entry, const CliffFace& face) {
  return scene_gen::TileCoord{static_cast<i32>(floor_div(face.anchor_x_mm, entry.tile_mm)),
                              static_cast<i32>(floor_div(face.anchor_z_mm, entry.tile_mm))};
}

bool occupies(const void* state, scene_gen::TileCoord tile) noexcept {
  const Entry& entry = *static_cast<const Entry*>(state);
  for (const CliffFace& face : entry.faces) {
    if (tile_of(entry, face) == tile) return true;
  }
  return false;
}

// One tile: the faces whose origin is in it, and their foot boulders, indexing `meshes`' list.
bool tile(void* state, scene_gen::TileCoord at, const scene_gen::Context& context,
          scene_gen::Placements& out, std::string*) {
  const Entry& entry = *static_cast<const Entry*>(state);
  const bool foot = has_foot(entry);
  const u32 foot_mesh = foot ? entry.meshes.size() - 1 : 0u;
  for (u32 i = 0; i < entry.faces.size(); ++i) {
    if (!(tile_of(entry, entry.faces[i]) == at)) continue;
    if (!cleared(entry, entry.faces[i], context)) continue;
    place_face(entry, i, i, foot_mesh, foot, context, out);
  }
  return true;
}

constexpr scene_gen::PlacementGeneratorDesc k_cliffs{
    .name = k_cliffs_generator,
    .open = &open,
    .close = &close,
    .expand = &expand,
    .meshes = &meshes,
    .tile = &tile,
    .occupies = &occupies,
    .representation = nullptr,
};
const scene_gen::Registrar k_cliffs_registrar{k_cliffs};

}  // namespace

}  // namespace engine::rocks

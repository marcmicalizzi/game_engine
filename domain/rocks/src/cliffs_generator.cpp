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

#include <core/containers/hash_map.h>
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/math/deterministic.h>
#include <core/schema/json_reflect.h>
#include <domain/geometry/cluster_file.h>
#include <domain/rocks/cliffs.h>
#include <domain/rocks/rocks.h>
#include <foundation/io/vfs.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace engine::rocks {

namespace {

using namespace detail;

ENGINE_LOG_CATEGORY_DEFINE(log_cliffs, "cliffs");

f64 min2(f64 a, f64 b) { return a < b ? a : b; }
f64 max2(f64 a, f64 b) { return a > b ? a : b; }

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
    case RockType::Limestone: return 4.5;
    case RockType::Granite:
    default: return 4.0;
  }
}

// One traced line into faces: measured up the slope, stood off, cut, and written as recipes.
void faces_of_line(const CliffLayer& layer, const scene_gen::Ground& ground, const Lattice& l,
                   i32 bx, i32 bz, const Vector<Point2>& traced, Vector<CliffFace>& out) {
  const f64 step = l.spacing;
  Vector<Point2> line = resample(traced, step);
  smooth(line, 3);
  if (line.size() < 3) return;
  // **A line cut by the block's edge runs on past it** by `k_overlap`, so the face traced in the
  // next block overlaps this one's end instead of leaving the ground showing between two ends
  // and two tapering skirts. Both are closed; the renderer draws the union.
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
  // Per vertex: the normal (the line's right, downhill), the steep ground's height above the foot
  // and its run, and then the face's own foot, stood off downhill.
  Vector<f64> foot_x(n), foot_z(n), foot_y(n), height(n), batter(n);
  for (u32 i = 0; i < n; ++i) {
    const Point2 a = line[i > 0 ? i - 1 : 0];
    const Point2 b = line[i + 1 < n ? i + 1 : n - 1];
    const D3 t = normalized(D3{b.x - a.x, 0.0, b.z - a.z});
    const D3 nrm{-t.z, 0.0, t.x};
    const f64 y0 = ground_at(ground, line[i].x, line[i].z);
    // Up the slope while it stays steep (four fifths of the threshold, so a ledge part way up
    // does not end the face), at most 200 m up.
    u32 k = 0;
    f64 top = y0;
    Vector<f64> profile;  // the ground's height at each step up the slope
    while (k < 400) {
      const f64 qx = line[i].x - nrm.x * step * static_cast<f64>(k + 1);
      const f64 qz = line[i].z - nrm.z * step * static_cast<f64>(k + 1);
      if (slope2_at(ground, qx, qz, step) < keep2) break;
      ++k;
      top = ground_at(ground, qx, qz);
      profile.push_back(top);
      if (top - y0 > 200.0) break;
    }
    const f64 relief = relief_of(layer, type_for(layer, y0));
    const f64 rise = max2(1.0, top - y0);
    const f64 stand =
        layer.standoff_m.has_value() ? wide(*layer.standoff_m) : 0.75 * relief + 0.03 * rise;
    foot_x[i] = line[i].x + nrm.x * stand;
    foot_z[i] = line[i].z + nrm.z * stand;
    foot_y[i] = ground_at(ground, foot_x[i], foot_z[i]);
    // The top half a metre under the brow, so the ground's own edge stands over the crest.
    height[i] = min2(200.0, max2(1.0, top - 0.5 - foot_y[i]));
    // **The batter that keeps the face in front of the ground**: the lean from the stood-off foot
    // that passes at least the standoff downhill of every point of the slope's profile — not the
    // chord from the foot to the brow, which a slope that steepens and then rounds over (most do)
    // rises above in its upper half and buries the face in. A profile point `r` up the slope from
    // the traced foot is `stand + r` from the stood-off one, so a lean of `r / up` keeps it `stand`
    // behind the face's mean plane, which is what the carving retreats into.
    f64 lean = 1.5;
    for (u32 q = 0; q < profile.size(); ++q) {
      const f64 up = profile[q] - foot_y[i];
      if (up > 1.0) lean = min2(lean, step * static_cast<f64>(q + 1) / up);
    }
    batter[i] = max2(0.0, lean);
  }
  smooth_values(height, 2);
  smooth_values(batter, 2);
  // Cut into faces no longer than `max_length_m`, of equal length.
  Vector<f64> s(n);
  for (u32 i = 1; i < n; ++i) {
    const f64 dx = foot_x[i] - foot_x[i - 1];
    const f64 dz = foot_z[i] - foot_z[i - 1];
    s[i] = s[i - 1] + std::sqrt(dx * dx + dz * dz);
  }
  const f64 total = s[n - 1];
  if (total < wide(layer.min_length_m)) return;
  const u32 pieces = static_cast<u32>(std::ceil(total / wide(layer.max_length_m) - 1e-9));
  u32 first = 0;
  for (u32 piece = 0; piece < pieces; ++piece) {
    const f64 end = piece + 1 == pieces ? total : total * static_cast<f64>(piece + 1) / pieces;
    u32 last = first;
    while (last + 1 < n && s[last + 1] <= end + 1e-9)
      ++last;
    if (last <= first) break;
    const u32 begin = first;
    first = last;
    const f64 length = s[last] - s[begin];
    if (length < wide(layer.min_length_m)) continue;
    f64 mean = 0.0;
    for (u32 i = begin; i <= last; ++i)
      mean += height[i];
    mean /= static_cast<f64>(last - begin + 1);
    if (mean < wide(layer.min_height_m)) continue;
    CliffFace face;
    face.x_mm = scene_gen::nearest_mm(foot_x[begin]);
    face.z_mm = scene_gen::nearest_mm(foot_z[begin]);
    face.y = std::floor(foot_y[begin] * 1000.0 + 0.5) / 1000.0;
    face.anchor_x_mm = scene_gen::nearest_mm(foot_x[(begin + last) / 2]);
    face.anchor_z_mm = scene_gen::nearest_mm(foot_z[(begin + last) / 2]);
    face.block_x = bx;
    face.block_z = bz;
    face.length = length;
    face.mean_height = mean;
    // The identity: the line in whole millimetres and its heights in centimetres.
    u64 id = mix64(0x434C4946ull ^ static_cast<u64>(last - begin + 1));
    for (u32 i = begin; i <= last; ++i) {
      id = hash_combine(id, static_cast<u64>(scene_gen::nearest_mm(foot_x[i])));
      id = hash_combine(id, static_cast<u64>(scene_gen::nearest_mm(foot_z[i])));
      id =
          hash_combine(id, static_cast<u64>(static_cast<i64>(std::floor(height[i] * 100.0 + 0.5))));
    }
    face.identity = id;
    Cliff c = layer.face;
    c.points.clear();
    c.type = type_for(layer, face.y);
    c.seed = mix64(layer.seed ^ id);
    c.height = static_cast<f32>(min2(200.0, max2(1.0, mean)));
    c.datum_y = static_cast<f32>(-face.y);
    const f64 ox = static_cast<f64>(face.x_mm) / 1000.0;
    const f64 oz = static_cast<f64>(face.z_mm) / 1000.0;
    for (u32 i = begin; i <= last; ++i) {
      CliffPoint p;
      p.x = static_cast<f32>(foot_x[i] - ox);
      p.y = static_cast<f32>(foot_y[i] - face.y);
      p.z = static_cast<f32>(foot_z[i] - oz);
      p.height = static_cast<f32>(height[i]);
      p.batter = static_cast<f32>(batter[i]);
      c.points.push_back(p);
    }
    face.cliff = c;
    out.push_back(face);
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

bool trace_cliff_block(const CliffLayer& layer, const scene_gen::Ground& ground, i32 bx, i32 bz,
                       Vector<CliffFace>& out, std::string* error) {
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
    faces_of_line(layer, ground, l, bx, bz, line, out);
  };
  for (u32 k = 0; k < feet.size(); ++k) {
    if (used[k] == 0 && ending.find_value(feet[k].from_edge) == nullptr) walk(k);
  }
  for (u32 k = 0; k < feet.size(); ++k) {
    if (used[k] == 0) walk(k);
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
  for (i64 bz = bz0; bz <= bz1; ++bz) {
    for (i64 bx = bx0; bx <= bx1; ++bx) {
      Vector<CliffFace> faces;
      if (!trace_cliff_block(layer, context.ground, static_cast<i32>(bx), static_cast<i32>(bz),
                             faces, &why))
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
  if (!with_foot) return;
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

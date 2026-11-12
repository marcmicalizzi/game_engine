#include "domain/scene_gen/exclusion.h"

#include "domain/scene_gen/scene_gen.h"

#include <algorithm>
#include <cmath>

namespace engine::scene_gen {
namespace {

// The side of the grid the segments are filed on, metres. A query reads one cell's list.
constexpr f64 k_cell_m = 16.0;
constexpr f64 k_far = 1.0e30;

i64 cell_of(f64 metres) noexcept { return static_cast<i64>(std::floor(metres / k_cell_m)); }

bool finite(f64 v) noexcept { return std::isfinite(v); }

// A channel's bank reach as its description gives it (`WaterChannel.bank_width_widths`,
// `bank_width_m`, which the channels recipe writes; T78), or the field's default where the number
// is negative or not finite: the water does not read them, so nothing else refuses a bad one.
f64 bank_number(f64 value, f64 fallback) noexcept {
  return finite(value) && value >= 0.0 ? value : fallback;
}

i64 floor_half(i64 v) noexcept { return v >= 0 ? v / 2 : -((-v + 1) / 2); }

// The squared distance from (x, z) to the segment from a to b, millimetres.
f64 segment_distance2(f64 x, f64 z, f64 ax, f64 az, f64 bx, f64 bz) noexcept {
  const f64 ex = bx - ax, ez = bz - az;
  const f64 len2 = ex * ex + ez * ez;
  f64 t = 0.0;
  if (len2 > 0.0) t = std::clamp(((x - ax) * ex + (z - az) * ez) / len2, 0.0, 1.0);
  const f64 px = ax + t * ex - x, pz = az + t * ez - z;
  return px * px + pz * pz;
}

f64 mm(i64 v) noexcept { return static_cast<f64>(v); }

}  // namespace

// ---- footprints ---------------------------------------------------------------------------------

Footprint Footprint::band(i64 ax, i64 az, i64 bx, i64 bz, i64 half) noexcept {
  Footprint f;
  f.shape = FootprintShape::band;
  f.ax_mm = ax;
  f.az_mm = az;
  f.bx_mm = bx;
  f.bz_mm = bz;
  f.half_mm = half;
  f.x_mm = floor_half(ax + bx);
  f.z_mm = floor_half(az + bz);
  // The disc that holds it: half the line's length from its middle and the half-width, and a
  // millimetre for the middle's rounding.
  const f64 ex = mm(bx - ax), ez = mm(bz - az);
  f.radius = (0.5 * std::sqrt(ex * ex + ez * ez) + mm(half) + 1.0) * 0.001;
  return f;
}

Footprint Footprint::polygon(std::span<const FootprintPoint> pts) noexcept {
  Footprint f;
  f.shape = FootprintShape::polygon;
  if (pts.size() < 3) return f;
  f.points = pts.data();
  f.point_count = static_cast<u32>(pts.size());
  i64 x0 = pts[0].x_mm, x1 = x0, z0 = pts[0].z_mm, z1 = z0;
  for (const FootprintPoint& p : pts) {
    x0 = std::min(x0, p.x_mm);
    x1 = std::max(x1, p.x_mm);
    z0 = std::min(z0, p.z_mm);
    z1 = std::max(z1, p.z_mm);
  }
  f.x_mm = floor_half(x0 + x1);
  f.z_mm = floor_half(z0 + z1);
  f64 far2 = 0.0;
  for (const FootprintPoint& p : pts) {
    const f64 dx = mm(p.x_mm - f.x_mm), dz = mm(p.z_mm - f.z_mm);
    far2 = std::max(far2, dx * dx + dz * dz);
  }
  f.radius = (std::sqrt(far2) + 1.0) * 0.001;
  return f;
}

f64 Footprint::edge_m(i64 x_mm_at, i64 z_mm_at) const noexcept {
  const f64 x = mm(x_mm_at), z = mm(z_mm_at);
  switch (shape) {
    case FootprintShape::disc: {
      if (!(radius > 0.0)) return k_far;
      const f64 dx = x - mm(x_mm), dz = z - mm(z_mm);
      return std::sqrt(dx * dx + dz * dz) * 0.001 - radius;
    }
    case FootprintShape::band: {
      if (half_mm < 1) return k_far;
      const f64 d2 = segment_distance2(x, z, mm(ax_mm), mm(az_mm), mm(bx_mm), mm(bz_mm));
      return (std::sqrt(d2) - mm(half_mm)) * 0.001;
    }
    case FootprintShape::polygon: {
      if (points == nullptr || point_count < 3) return k_far;
      // Inside by the even-odd rule (a crossing of the ray towards +x), and the nearest edge.
      bool in = false;
      f64 d2 = k_far;
      for (u32 i = 0, j = point_count - 1; i < point_count; j = i++) {
        const f64 xi = mm(points[i].x_mm), zi = mm(points[i].z_mm);
        const f64 xj = mm(points[j].x_mm), zj = mm(points[j].z_mm);
        if ((zi > z) != (zj > z)) {
          const f64 cross = xj + (z - zj) * (xi - xj) / (zi - zj);
          if (x < cross) in = !in;
        }
        d2 = std::min(d2, segment_distance2(x, z, xj, zj, xi, zi));
      }
      const f64 d = std::sqrt(d2) * 0.001;
      return in ? -d : d;
    }
  }
  return k_far;
}

bool Exclusion::add_footprints(std::span<const Footprint> footprints, std::string* error) {
  const auto fail = [&](usize k, const char* why) {
    if (error != nullptr) *error = "footprint " + std::to_string(k) + " " + why;
    return false;
  };
  // Checked whole before anything is added.
  for (usize k = 0; k < footprints.size(); ++k) {
    const Footprint& f = footprints[k];
    if (!finite(f.radius) || f.radius < 0.0)
      return fail(k, "has a radius that is negative or not finite");
    if (f.shape == FootprintShape::band && f.half_mm < 1)
      return fail(k, "is a band with a half-width under a millimetre");
    if (f.shape == FootprintShape::polygon) {
      if (f.points == nullptr || f.point_count < 3)
        return fail(k, "is a polygon of fewer than three points");
      f64 area2 = 0.0;
      for (u32 i = 0, j = f.point_count - 1; i < f.point_count; j = i++)
        area2 += mm(f.points[j].x_mm) * mm(f.points[i].z_mm) -
                 mm(f.points[i].x_mm) * mm(f.points[j].z_mm);
      if (!(std::fabs(area2) > 0.0)) return fail(k, "is a polygon with no area");
    }
  }
  for (const Footprint& f : footprints) {
    if (!(f.radius > 0.0)) continue;  // a disc of no radius covers nothing
    const u32 id = footprints_.size();
    Footprint kept = f;
    footprint_first_.push_back(points_.size());
    if (f.shape == FootprintShape::polygon) {
      for (u32 i = 0; i < f.point_count; ++i)
        points_.push_back(f.points[i]);
      kept.points = nullptr;
    }
    footprints_.push_back(kept);
    const f64 cx = mm(f.x_mm) * 0.001, cz = mm(f.z_mm) * 0.001;
    for (i64 z = cell_of(cz - f.radius); z <= cell_of(cz + f.radius); ++z)
      for (i64 x = cell_of(cx - f.radius); x <= cell_of(cx + f.radius); ++x)
        footprint_cells_.push_back(Cell{z, x, id});
  }
  std::sort(footprint_cells_.begin(), footprint_cells_.end(), [](const Cell& a, const Cell& b) {
    if (a.key_z != b.key_z) return a.key_z < b.key_z;
    if (a.key_x != b.key_x) return a.key_x < b.key_x;
    return a.segment < b.segment;
  });
  return true;
}

f64 Exclusion::footprint_edge_m(u32 k, i64 x_mm, i64 z_mm) const noexcept {
  const Footprint& f = footprints_[k];
  if (f.shape != FootprintShape::polygon) return f.edge_m(x_mm, z_mm);
  Footprint p = f;
  p.points = points_.data() + footprint_first_[k];
  return p.edge_m(x_mm, z_mm);
}

f64 Exclusion::footprint_distance_mm(i64 x_mm, i64 z_mm, f64 reach_m) const noexcept {
  if (footprint_cells_.empty() || !(reach_m > 0.0)) return k_far;
  const f64 x = mm(x_mm) * 0.001, z = mm(z_mm) * 0.001;
  // A footprint whose edge is within the reach has its disc within the reach of the point, and it
  // is filed in every cell its disc's box touches.
  const i64 cx0 = cell_of(x - reach_m), cx1 = cell_of(x + reach_m);
  const i64 cz0 = cell_of(z - reach_m), cz1 = cell_of(z + reach_m);
  f64 best = k_far;
  for (i64 cz = cz0; cz <= cz1; ++cz) {
    auto it = std::lower_bound(footprint_cells_.begin(), footprint_cells_.end(), Cell{cz, cx0, 0},
                               [](const Cell& a, const Cell& b) {
                                 if (a.key_z != b.key_z) return a.key_z < b.key_z;
                                 return a.key_x < b.key_x;
                               });
    for (; it != footprint_cells_.end() && it->key_z == cz && it->key_x <= cx1; ++it)
      best = std::min(best, footprint_edge_m(it->segment, x_mm, z_mm));
  }
  if (best >= reach_m) return k_far;
  return best > 0.0 ? best : 0.0;
}

bool Exclusion::build(std::span<const scene::TerrainHole> holes,
                      std::span<const scene::WaterBody> water,
                      std::span<const scene::WaterChannel> channels, std::string* error) {
  holes_ = TerrainHoles{};
  bodies_.clear();
  segments_.clear();
  cells_.clear();
  widest_edge_ = 0.0;
  footprints_.clear();
  footprint_first_.clear();
  points_.clear();
  footprint_cells_.clear();
  if (!holes_.build(holes, error)) return false;

  for (const scene::WaterBody& block : water) {
    if (!finite(block.level_m)) continue;
    Body body;
    body.level = block.level_m;
    body.open = block.open;
    const bool circle = block.shape.circle.has_value();
    const bool polygon = !block.shape.polygon.empty();
    const bool fill = block.shape.fill.has_value();
    if (static_cast<int>(circle) + static_cast<int>(polygon) + static_cast<int>(fill) != 1)
      continue;
    if (circle) {
      const scene::WaterCircle& c = *block.shape.circle;
      const f64 r = static_cast<f64>(c.radius_m);
      if (!finite(c.center.x) || !finite(c.center.z) || !(r > 0.0)) continue;
      body.circle = true;
      body.cx = c.center.x;
      body.cz = c.center.z;
      body.r2 = r * r;
      body.x0 = body.cx - r;
      body.x1 = body.cx + r;
      body.z0 = body.cz - r;
      body.z1 = body.cz + r;
    } else if (fill) {
      // The flood runs when the scene's water is read, after the placements, so its region is not
      // known here: the box the flood may reach, where the ground is below the level (the same
      // box `water::depth_at` falls back to for a flood not yet run).
      const scene::WaterFill& f = *block.shape.fill;
      const f64 e = static_cast<f64>(f.max_extent_m);
      if (!finite(f.seed.x) || !finite(f.seed.z) || !(e > 0.0)) continue;
      body.fill = true;
      body.cx = f.seed.x;
      body.cz = f.seed.z;
      body.extent = e;
      body.x0 = body.cx - e;
      body.x1 = body.cx + e;
      body.z0 = body.cz - e;
      body.z1 = body.cz + e;
    } else {
      bool ok = block.shape.polygon.size() >= 3;
      for (const WorldPos& p : block.shape.polygon)
        ok = ok && finite(p.x) && finite(p.z);
      if (!ok) continue;
      body.x0 = body.x1 = block.shape.polygon[0].x;
      body.z0 = body.z1 = block.shape.polygon[0].z;
      for (const WorldPos& p : block.shape.polygon) {
        body.xs.push_back(p.x);
        body.zs.push_back(p.z);
        body.x0 = std::min(body.x0, p.x);
        body.x1 = std::max(body.x1, p.x);
        body.z0 = std::min(body.z0, p.z);
        body.z1 = std::max(body.z1, p.z);
      }
    }
    bodies_.push_back(std::move(body));
  }

  const scene::WaterChannel defaults;  // the schema's defaults, which are the recipe's
  for (const scene::WaterChannel& channel : channels) {
    const u32 n = channel.points.size();
    if (n < 2 || channel.width_m.size() < n) continue;
    bool ok = true;
    for (u32 i = 0; i < n; ++i)
      ok = ok && finite(channel.points[i].x) && finite(channel.points[i].z) &&
           finite(static_cast<f64>(channel.width_m[i])) && channel.width_m[i] > 0.0f;
    if (!ok) continue;
    const f64 bank_widths = bank_number(channel.bank_width_widths, defaults.bank_width_widths);
    const f64 bank_m = bank_number(channel.bank_width_m, defaults.bank_width_m);
    const auto edge = [&](u32 i) {
      const f64 w = static_cast<f64>(channel.width_m[i]);
      return 0.5 * w + bank_widths * w + bank_m;
    };
    // The flood bed's reach each side (W93), the cut's own numbers: read where both arrays are
    // the points' long; a negative or non-finite one is none, as a bank's number is its default.
    const bool flood = channel.flood_left_m.size() == n && channel.flood_right_m.size() == n;
    const auto flood_number = [](f32 v) {
      const f64 d = static_cast<f64>(v);
      return finite(d) && d > 0.0 ? d : 0.0;
    };
    for (u32 i = 0; i + 1 < n; ++i) {
      Segment s;
      s.ax = channel.points[i].x;
      s.az = channel.points[i].z;
      s.bx = channel.points[i + 1].x;
      s.bz = channel.points[i + 1].z;
      s.ra = edge(i);
      s.rb = edge(i + 1);
      if (flood) {
        s.la = flood_number(channel.flood_left_m[i]);
        s.lb = flood_number(channel.flood_left_m[i + 1]);
        s.ga = flood_number(channel.flood_right_m[i]);
        s.gb = flood_number(channel.flood_right_m[i + 1]);
      }
      const u32 id = segments_.size();
      const f64 edge_reach =
          std::max(std::max(std::max(s.ra, s.rb), std::max(s.la, s.lb)), std::max(s.ga, s.gb));
      widest_edge_ = std::max(widest_edge_, edge_reach);
      const f64 reach = edge_reach + k_riparian_reach_m;
      const i64 cx0 = cell_of(std::min(s.ax, s.bx) - reach);
      const i64 cx1 = cell_of(std::max(s.ax, s.bx) + reach);
      const i64 cz0 = cell_of(std::min(s.az, s.bz) - reach);
      const i64 cz1 = cell_of(std::max(s.az, s.bz) + reach);
      segments_.push_back(s);
      for (i64 cz = cz0; cz <= cz1; ++cz)
        for (i64 cx = cx0; cx <= cx1; ++cx)
          cells_.push_back(Cell{cz, cx, id});
    }
  }
  std::sort(cells_.begin(), cells_.end(), [](const Cell& a, const Cell& b) {
    if (a.key_z != b.key_z) return a.key_z < b.key_z;
    if (a.key_x != b.key_x) return a.key_x < b.key_x;
    return a.segment < b.segment;
  });
  return true;
}

bool Exclusion::inside(const Body& body, f64 x, f64 z) noexcept {
  if (x < body.x0 || x > body.x1 || z < body.z0 || z > body.z1) return false;
  if (body.circle) {
    const f64 dx = x - body.cx;
    const f64 dz = z - body.cz;
    return dx * dx + dz * dz < body.r2;
  }
  if (body.fill) return true;
  // Even-odd over the edges: a crossing of the ray towards +x.
  bool in = false;
  const u32 n = body.xs.size();
  for (u32 i = 0, j = n - 1; i < n; j = i++) {
    const f64 zi = body.zs[i];
    const f64 zj = body.zs[j];
    if ((zi > z) != (zj > z)) {
      const f64 cross = body.xs[j] + (z - zj) * (body.xs[i] - body.xs[j]) / (zi - zj);
      if (x < cross) in = !in;
    }
  }
  return in;
}

f64 Exclusion::bank_gap(const Segment& s, f64 x, f64 z) noexcept {
  const f64 dx = s.bx - s.ax;
  const f64 dz = s.bz - s.az;
  const f64 len2 = dx * dx + dz * dz;
  f64 t = 0.0;
  if (len2 > 0.0) t = std::clamp(((x - s.ax) * dx + (z - s.az) * dz) / len2, 0.0, 1.0);
  const f64 px = s.ax + t * dx - x;
  const f64 pz = s.az + t * dz - z;
  f64 reach = s.ra + t * (s.rb - s.ra);
  // The flood bed's edge on the point's side of the line, where it reaches further (W93). The
  // left is the side of (-dz, dx).
  const bool left = dx * (z - s.az) - dz * (x - s.ax) >= 0.0;
  const f64 bed = left ? s.la + t * (s.lb - s.la) : s.ga + t * (s.gb - s.ga);
  if (bed > reach) reach = bed;
  return std::sqrt(px * px + pz * pz) - reach;
}

f64 Exclusion::nearest_gap(f64 x, f64 z) const noexcept {
  if (cells_.empty()) return k_far;
  const i64 cz = cell_of(z);
  const i64 cx = cell_of(x);
  const auto first = std::lower_bound(cells_.begin(), cells_.end(), Cell{cz, cx, 0},
                                      [](const Cell& a, const Cell& b) {
                                        if (a.key_z != b.key_z) return a.key_z < b.key_z;
                                        return a.key_x < b.key_x;
                                      });
  f64 best = k_far;
  for (auto it = first; it != cells_.end() && it->key_z == cz && it->key_x == cx; ++it)
    best = std::min(best, bank_gap(segments_[it->segment], x, z));
  return best;
}

bool Exclusion::excluded_mm(const Ground& ground, i64 x_mm, i64 z_mm, u32 kinds) const noexcept {
  if ((kinds & k_exclude_holes) != 0 && !holes_.empty() &&
      holes_.contains_mm(static_cast<f64>(x_mm), static_cast<f64>(z_mm)))
    return true;
  const f64 x = static_cast<f64>(x_mm) * 0.001;
  const f64 z = static_cast<f64>(z_mm) * 0.001;
  if ((kinds & k_exclude_channels) != 0 && !cells_.empty() && nearest_gap(x, z) <= 0.0) return true;
  // The shaped footprints (W98): the point's own cell's.
  if ((kinds & k_exclude_footprints) != 0 && !footprint_cells_.empty()) {
    const i64 cz = cell_of(z), cx = cell_of(x);
    auto it = std::lower_bound(footprint_cells_.begin(), footprint_cells_.end(), Cell{cz, cx, 0},
                               [](const Cell& a, const Cell& b) {
                                 if (a.key_z != b.key_z) return a.key_z < b.key_z;
                                 return a.key_x < b.key_x;
                               });
    for (; it != footprint_cells_.end() && it->key_z == cz && it->key_x == cx; ++it)
      if (footprint_edge_m(it->segment, x_mm, z_mm) <= 0.0) return true;
  }
  if ((kinds & k_exclude_water) != 0) {
    for (const Body& body : bodies_) {
      if (!body.open && !inside(body, x, z)) continue;
      if (ground.surface_mm(x_mm, z_mm) < body.level) return true;
    }
  }
  return false;
}

f64 Exclusion::channel_distance_mm(i64 x_mm, i64 z_mm) const noexcept {
  if (cells_.empty()) return k_far;
  const f64 gap = nearest_gap(static_cast<f64>(x_mm) * 0.001, static_cast<f64>(z_mm) * 0.001);
  if (gap >= k_riparian_reach_m) return k_far;
  return std::max(gap, 0.0);
}

f64 Exclusion::channel_edge_distance_mm(i64 x_mm, i64 z_mm, f64 reach_m) const noexcept {
  if (cells_.empty() || !(reach_m > 0.0)) return k_far;
  const f64 x = static_cast<f64>(x_mm) * 0.001;
  const f64 z = static_cast<f64>(z_mm) * 0.001;
  const auto row_start = [&](i64 kz, i64 kx) {
    return std::lower_bound(cells_.begin(), cells_.end(), Cell{kz, kx, 0},
                            [](const Cell& a, const Cell& b) {
                              if (a.key_z != b.key_z) return a.key_z < b.key_z;
                              return a.key_x < b.key_x;
                            });
  };
  // **Nearest cells first** (T102). A segment is filed in every cell its line's box touches once
  // grown by its own widest edge and `k_riparian_reach_m` (`build`), so a segment whose edge is g
  // metres from the point is filed in a cell no further than g - k_riparian_reach_m from it: the
  // point itself is in that grown box when g is under the riparian reach, and otherwise the point
  // of the box nearest it, on the line from the point to the segment, is that close. So once every
  // cell within L - k_riparian_reach_m of the point has been read, where L is the lesser of the
  // best gap found and the reach, no cell left can hold a nearer edge, and the answer is the old
  // square scan's to the bit (the least of the same gaps; the test holds the two together). The
  // grid is read a row at a time outward from the point's own, and each row from the point's
  // column outward, the nearer side's next filed cell first, until the next cell is further than
  // the bound, which shrinks as the best gap does: one binary search a row, and a point beside a
  // stream reads its own cell and its neighbours. `k_slack_m` covers the rounding of the filing's
  // and this bound's arithmetic, a centimetre where a micrometre would do.
  constexpr f64 k_slack_m = 0.01;
  const i64 cz = cell_of(z), cx = cell_of(x);
  // Never under 0: the point's own cell is read whatever the reach, since a segment whose edge is
  // within the riparian reach is filed there.
  const auto bound = [&](f64 so_far) {
    return std::max(std::min(so_far, reach_m) - k_riparian_reach_m + k_slack_m, 0.0);
  };
  f64 best = k_far;
  // Rows outside the filed ones hold nothing, which also bounds the walk for any reach.
  const i64 z_lo = cells_.front().key_z, z_hi = cells_.back().key_z;
  const f64 cell_z0 = static_cast<f64>(cz) * k_cell_m;
  const f64 cell_x0 = static_cast<f64>(cx) * k_cell_m;
  for (i64 k = 0;; ++k) {
    bool any = false;
    for (int side = 0; side < (k == 0 ? 1 : 2); ++side) {
      const i64 row = side == 0 ? cz + k : cz - k;
      // How far the row's nearest edge is from the point, along z.
      const f64 dz = k == 0      ? 0.0
                     : side == 0 ? cell_z0 + static_cast<f64>(k) * k_cell_m - z
                                 : z - (cell_z0 - static_cast<f64>(k - 1) * k_cell_m);
      if (!(dz <= bound(best))) continue;
      any = true;
      if (row < z_lo || row > z_hi) continue;
      // `right` walks the row's cells from the point's column up, `left` the ones below it down.
      auto right = row_start(row, cx);
      auto left = right;
      for (;;) {
        f64 dr = k_far, dl = k_far;
        if (right != cells_.end() && right->key_z == row)
          dr = right->key_x == cx ? 0.0
                                  : static_cast<f64>(right->key_x - cx) * k_cell_m + cell_x0 - x;
        if (left != cells_.begin() && (left - 1)->key_z == row)
          dl = x - (cell_x0 - static_cast<f64>(cx - (left - 1)->key_x - 1) * k_cell_m);
        const bool take_right = dr <= dl;
        const f64 dx = std::max(take_right ? dr : dl, 0.0);
        const f64 limit = bound(best);
        if (dx >= k_far || !(dx <= limit && dz * dz + dx * dx <= limit * limit)) break;
        if (take_right) {
          const i64 kx = right->key_x;
          for (; right != cells_.end() && right->key_z == row && right->key_x == kx; ++right)
            best = std::min(best, bank_gap(segments_[right->segment], x, z));
        } else {
          const i64 kx = (left - 1)->key_x;
          for (; left != cells_.begin() && (left - 1)->key_z == row && (left - 1)->key_x == kx;
               --left)
            best = std::min(best, bank_gap(segments_[(left - 1)->segment], x, z));
        }
      }
    }
    if (!any || (cz + k >= z_hi && cz - k <= z_lo)) break;
  }
  if (best >= reach_m) return k_far;
  return best > 0.0 ? best : 0.0;
}

f64 Exclusion::channel_edge_distance_reference_mm(i64 x_mm, i64 z_mm, f64 reach_m) const noexcept {
  if (cells_.empty() || !(reach_m > 0.0)) return k_far;
  const f64 x = static_cast<f64>(x_mm) * 0.001;
  const f64 z = static_cast<f64>(z_mm) * 0.001;
  // A segment whose edge is within the reach has its line within the reach and its widest edge
  // of the point, and it is filed in every cell its line's box touches.
  const f64 box = reach_m + widest_edge_;
  const i64 cx0 = cell_of(x - box), cx1 = cell_of(x + box);
  const i64 cz0 = cell_of(z - box), cz1 = cell_of(z + box);
  f64 best = k_far;
  for (i64 cz = cz0; cz <= cz1; ++cz) {
    auto it = std::lower_bound(cells_.begin(), cells_.end(), Cell{cz, cx0, 0},
                               [](const Cell& a, const Cell& b) {
                                 if (a.key_z != b.key_z) return a.key_z < b.key_z;
                                 return a.key_x < b.key_x;
                               });
    for (; it != cells_.end() && it->key_z == cz && it->key_x <= cx1; ++it)
      best = std::min(best, bank_gap(segments_[it->segment], x, z));
  }
  if (best >= reach_m) return k_far;
  return best > 0.0 ? best : 0.0;
}

f64 Exclusion::riparian_factor_mm(i64 x_mm, i64 z_mm) const noexcept {
  const f64 d = channel_distance_mm(x_mm, z_mm);
  if (d >= k_riparian_reach_m) return 1.0;
  const f64 t = d / k_riparian_reach_m;
  const f64 smooth = t * t * (3.0 - 2.0 * t);
  return k_riparian_floor + (1.0 - k_riparian_floor) * smooth;
}

}  // namespace engine::scene_gen

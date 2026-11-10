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

}  // namespace

bool Exclusion::build(std::span<const scene::TerrainHole> holes,
                      std::span<const scene::WaterBody> water,
                      std::span<const scene::WaterChannel> channels, std::string* error) {
  holes_ = TerrainHoles{};
  bodies_.clear();
  segments_.clear();
  cells_.clear();
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
    for (u32 i = 0; i + 1 < n; ++i) {
      Segment s;
      s.ax = channel.points[i].x;
      s.az = channel.points[i].z;
      s.bx = channel.points[i + 1].x;
      s.bz = channel.points[i + 1].z;
      s.ra = edge(i);
      s.rb = edge(i + 1);
      const u32 id = segments_.size();
      const f64 reach = std::max(s.ra, s.rb) + k_riparian_reach_m;
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
  return std::sqrt(px * px + pz * pz) - (s.ra + t * (s.rb - s.ra));
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

f64 Exclusion::riparian_factor_mm(i64 x_mm, i64 z_mm) const noexcept {
  const f64 d = channel_distance_mm(x_mm, z_mm);
  if (d >= k_riparian_reach_m) return 1.0;
  const f64 t = d / k_riparian_reach_m;
  const f64 smooth = t * t * (3.0 - 2.0 * t);
  return k_riparian_floor + (1.0 - k_riparian_floor) * smooth;
}

}  // namespace engine::scene_gen

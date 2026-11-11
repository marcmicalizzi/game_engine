// The site a placement reads (site.h; docs/subsystems/scene_gen.md, "The site"; roadmap G144).
#include "domain/scene_gen/site.h"

#include "domain/scene_gen/scene_gen.h"

#include <core/math/deterministic.h>

#include <algorithm>
#include <cmath>

namespace engine::scene_gen {
namespace {

constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_sqrt_half = 0.70710678118654752440;
// The side of the grid the channels' segments are filed on, metres: a segment is filed in every
// cell within its bank edge and the reach of it, so a query reads one cell's list.
constexpr f64 k_cell_m = 64.0;

i64 cell_of(f64 metres) noexcept { return static_cast<i64>(std::floor(metres / k_cell_m)); }
bool finite(f64 v) noexcept { return std::isfinite(v); }

// A channel's bank reach as its description gives it, or the field's default where the number is
// negative or not a number: the exclusion's reading (exclusion.cpp, `bank_number`; T78), so the
// distance to a bank is measured from the edge the exclusion clears to.
f64 bank_number(f64 value, f64 fallback) noexcept {
  return finite(value) && value >= 0.0 ? value : fallback;
}

// From (x, z) to the segment a-b, metres.
f64 segment_distance(f64 ax, f64 az, f64 bx, f64 bz, f64 x, f64 z, f64* t_out) noexcept {
  const f64 dx = bx - ax;
  const f64 dz = bz - az;
  const f64 len2 = dx * dx + dz * dz;
  f64 t = 0.0;
  if (len2 > 0.0) t = std::clamp(((x - ax) * dx + (z - az) * dz) / len2, 0.0, 1.0);
  if (t_out != nullptr) *t_out = t;
  const f64 px = ax + t * dx - x;
  const f64 pz = az + t * dz - z;
  return std::sqrt(px * px + pz * pz);
}

}  // namespace

Site::Site() noexcept {
  std::string unused;
  build(k_site_default_latitude_deg, {}, {}, &unused);
}

bool Site::build(f64 latitude_deg, std::span<const scene::WaterBody> water,
                 std::span<const scene::WaterChannel> channels, std::string* error) {
  segments_.clear();
  cells_.clear();
  bodies_.clear();
  if (!finite(latitude_deg)) {
    if (error != nullptr) *error = "the site's latitude is not a number";
    return false;
  }
  latitude_deg_ = std::clamp(latitude_deg, -k_site_max_latitude_deg, k_site_max_latitude_deg);
  // The reference sun (site.h): at the folded bearing, 225° north of the equator and 315° south of
  // it (north is -z, east +x, so a bearing A is (sin A, -cos A)), and at the equinox's noon
  // elevation 90° - |latitude|, whose sine is cos(latitude) and cosine sin|latitude|.
  const f64 phi = (latitude_deg_ < 0.0 ? -latitude_deg_ : latitude_deg_) * (k_pi / 180.0);
  const f64 cos_e = det::sin(phi);
  const f64 sin_e = det::cos(phi);
  bearing_x_ = -k_sqrt_half;
  bearing_z_ = latitude_deg_ < 0.0 ? -k_sqrt_half : k_sqrt_half;
  sun_x_ = cos_e * bearing_x_;
  sun_y_ = sin_e;
  sun_z_ = cos_e * bearing_z_;

  // The bodies whose outline is geometry: a circle or a polygon. A fill's region is found by a
  // flood when the water is read, after the placements, and an open body's shore is where the
  // ground meets its level; neither is an outline to measure from here (scene_gen.md, "The site").
  for (const scene::WaterBody& block : water) {
    if (!finite(block.level_m) || block.open) continue;
    const bool circle = block.shape.circle.has_value();
    const bool polygon = !block.shape.polygon.empty();
    const bool fill = block.shape.fill.has_value();
    if (static_cast<int>(circle) + static_cast<int>(polygon) + static_cast<int>(fill) != 1 || fill)
      continue;
    Body body;
    if (circle) {
      const scene::WaterCircle& c = *block.shape.circle;
      const f64 r = static_cast<f64>(c.radius_m);
      if (!finite(c.center.x) || !finite(c.center.z) || !(r > 0.0)) continue;
      body.circle = true;
      body.cx = c.center.x;
      body.cz = c.center.z;
      body.r = r;
      body.x0 = body.cx - r;
      body.x1 = body.cx + r;
      body.z0 = body.cz - r;
      body.z1 = body.cz + r;
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
      const f64 reach = std::max(s.ra, s.rb) + k_site_water_reach_m;
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

f64 Site::channel_distance(f64 x, f64 z) const noexcept {
  if (cells_.empty()) return k_site_water_reach_m;
  const i64 cz = cell_of(z);
  const i64 cx = cell_of(x);
  const auto first = std::lower_bound(cells_.begin(), cells_.end(), Cell{cz, cx, 0},
                                      [](const Cell& a, const Cell& b) {
                                        if (a.key_z != b.key_z) return a.key_z < b.key_z;
                                        return a.key_x < b.key_x;
                                      });
  f64 best = k_site_water_reach_m;
  for (auto it = first; it != cells_.end() && it->key_z == cz && it->key_x == cx; ++it) {
    const Segment& s = segments_[it->segment];
    f64 t = 0.0;
    const f64 d = segment_distance(s.ax, s.az, s.bx, s.bz, x, z, &t);
    best = std::min(best, d - (s.ra + t * (s.rb - s.ra)));
  }
  return std::max(best, 0.0);
}

f64 Site::body_distance(f64 x, f64 z) const noexcept {
  f64 best = k_site_water_reach_m;
  for (const Body& body : bodies_) {
    if (x < body.x0 - best || x > body.x1 + best || z < body.z0 - best || z > body.z1 + best)
      continue;
    if (body.circle) {
      const f64 dx = x - body.cx;
      const f64 dz = z - body.cz;
      best = std::min(best, std::max(0.0, std::sqrt(dx * dx + dz * dz) - body.r));
      continue;
    }
    // Even-odd over the edges, as the exclusion reads a polygon; inside is 0.
    bool in = false;
    f64 edge = k_site_water_reach_m;
    const u32 n = body.xs.size();
    for (u32 i = 0, j = n - 1; i < n; j = i++) {
      const f64 zi = body.zs[i];
      const f64 zj = body.zs[j];
      if ((zi > z) != (zj > z)) {
        const f64 cross = body.xs[j] + (z - zj) * (body.xs[i] - body.xs[j]) / (zi - zj);
        if (x < cross) in = !in;
      }
      edge = std::min(edge, segment_distance(body.xs[j], zj, body.xs[i], zi, x, z, nullptr));
    }
    best = std::min(best, in ? 0.0 : edge);
  }
  return best;
}

SiteSample Site::sample_mm(const Ground& ground, i64 x_mm, i64 z_mm, u32 fields) const noexcept {
  SiteSample s;
  if ((fields & (k_site_elevation | k_site_form)) != 0) s.elevation = ground.surface_mm(x_mm, z_mm);
  if ((fields & k_site_form) != 0) {
    // Central differences over the 10 m baseline, the five-point Laplacian over the same points.
    const f64 e = ground.surface_mm(x_mm + k_site_step_mm, z_mm);
    const f64 w = ground.surface_mm(x_mm - k_site_step_mm, z_mm);
    const f64 n = ground.surface_mm(x_mm, z_mm - k_site_step_mm);
    const f64 so = ground.surface_mm(x_mm, z_mm + k_site_step_mm);
    const f64 h = static_cast<f64>(k_site_step_mm) / 1000.0;
    const f64 gx = (e - w) / (2.0 * h);
    const f64 gz = (so - n) / (2.0 * h);
    const f64 g2 = gx * gx + gz * gz;
    s.slope = std::sqrt(g2);
    if (s.slope > 0.0) {
      s.down_x = -gx / s.slope;
      s.down_z = -gz / s.slope;
    }
    s.curvature = -((e + w + n + so) - 4.0 * s.elevation) / (h * h);
    // The unit normal (-gx, 1, -gz) / |...| against the reference sun, over level ground's (sun_y).
    const f64 len = std::sqrt(1.0 + g2);
    const f64 incidence = (sun_y_ - gx * sun_x_ - gz * sun_z_) / len;
    s.heat_load = std::max(0.0, incidence / sun_y_);
    s.sunward = -(gx * bearing_x_ + gz * bearing_z_) / len;
  }
  if ((fields & k_site_water) != 0) {
    const f64 x = static_cast<f64>(x_mm) * 0.001;
    const f64 z = static_cast<f64>(z_mm) * 0.001;
    s.channel_m = channel_distance(x, z);
    s.body_m = body_distance(x, z);
  }
  return s;
}

const Site& Site::fallback() noexcept {
  static const Site site;
  return site;
}

}  // namespace engine::scene_gen

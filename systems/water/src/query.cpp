// The water at a point (query.h; docs/subsystems/water.md, "The water at a point"; roadmap A43).

#include <systems/water/query.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace engine::water {

namespace {

// The map's signed depth at body-local (x, z), bilinear over the four texels round it, the point
// clamped to the texels' centres. False past the map's square (a half texel beyond its outer
// centres).
bool map_depth(const DepthMap& map, f32 x, f32 z, f32& out) noexcept {
  if (map.size == 0 || !(map.texel_m > 0.0f)) return false;
  const f32 u = (x - map.origin.x) / map.texel_m;
  const f32 v = (z - map.origin.y) / map.texel_m;
  const f32 last = static_cast<f32>(map.size - 1);
  if (u < -0.5f || v < -0.5f || u > last + 0.5f || v > last + 0.5f) return false;
  const f32 cu = std::clamp(u, 0.0f, last);
  const f32 cv = std::clamp(v, 0.0f, last);
  const u32 i = std::min(static_cast<u32>(cu), map.size - 1);
  const u32 j = std::min(static_cast<u32>(cv), map.size - 1);
  const u32 i1 = std::min(i + 1, map.size - 1);
  const u32 j1 = std::min(j + 1, map.size - 1);
  const f32 fu = cu - static_cast<f32>(i);
  const f32 fv = cv - static_cast<f32>(j);
  const f32 a = depth_map_depth(map, i, j);
  const f32 b = depth_map_depth(map, i1, j);
  const f32 c = depth_map_depth(map, i, j1);
  const f32 d = depth_map_depth(map, i1, j1);
  const f32 top = a + (b - a) * fu;
  const f32 bottom = c + (d - c) * fu;
  out = top + (bottom - top) * fv;
  return true;
}

// Whether body-local (x, z) is in the body: `depth_at`'s rule (water.cpp).
bool holds(const Body& body, f32 x, f32 z) noexcept {
  if (body.fill) {
    if (!body.filled())
      return std::fabs(x) <= body.fill_extent_m && std::fabs(z) <= body.fill_extent_m;
    return fill_value(body, x, z) > 0.0f;
  }
  return body.open || inside_distance(body, x, z) >= 0.0f;
}

}  // namespace

bool body_water_at(const Body& body, const DepthMap& map, const scene_gen::Ground* ground,
                   WorldPos at, WaterPoint& out) noexcept {
  out = WaterPoint{};
  const f32 x = static_cast<f32>(at.x - body.center.x);
  const f32 z = static_cast<f32>(at.z - body.center.z);
  if (!holds(body, x, z)) return false;
  f32 depth = 0.0f;
  if (!map_depth(map, x, z, depth)) {
    // Past the map: an open body's sea goes on where the ground is below its level, and a body
    // with no map answers the ground as `depth_at` does.
    if (map.size != 0 && !body.open) return false;
    const f64 surface = ground != nullptr ? ground->surface(at) : 0.0;
    depth = static_cast<f32>(body.level_m - surface);
  }
  if (!(depth > 0.0f)) return false;
  out.wet = true;
  out.level = body.level_m;
  out.depth = depth;
  out.source = 1;
  return true;
}

bool channel_water_at(const Channel& channel, WorldPos at, WaterPoint& out) noexcept {
  out = WaterPoint{};
  const u32 n = channel.points.size();
  if (n < 2 || channel.width_m.size() != n || channel.depth_m.size() != n ||
      channel.slope.size() != n) {
    return false;
  }
  const f32 px = static_cast<f32>(at.x - channel.origin.x);
  const f32 pz = static_cast<f32>(at.z - channel.origin.z);
  // The stretch that holds the point nearest its line; the first such in the line's order on a tie.
  bool found = false;
  f32 best_share = 0.0f;  // |2a / w|: how far out across its width the point lies
  u32 best_i = 0;
  f32 best_t = 0.0f;
  for (u32 i = 0; i + 1 < n; ++i) {
    const Vec3 p = channel.points[i];
    const Vec3 q = channel.points[i + 1];
    const f32 dx = q.x - p.x;
    const f32 dz = q.z - p.z;
    const f32 run2 = dx * dx + dz * dz;
    if (!(run2 > 0.0f)) continue;  // a vertical drop holds no surface in plan
    f32 t = ((px - p.x) * dx + (pz - p.z) * dz) / run2;
    t = std::clamp(t, 0.0f, 1.0f);
    const f32 cx = p.x + dx * t;
    const f32 cz = p.z + dz * t;
    const f32 ox = px - cx;
    const f32 oz = pz - cz;
    const f32 a = std::sqrt(ox * ox + oz * oz);
    const f32 w = channel.width_m[i] + (channel.width_m[i + 1] - channel.width_m[i]) * t;
    if (!(w > 0.0f)) continue;
    const f32 share = 2.0f * a / w;
    if (share > 1.0f) continue;
    if (!found || share < best_share) {
      found = true;
      best_share = share;
      best_i = i;
      best_t = t;
    }
  }
  if (!found) return false;
  const u32 i = best_i;
  const f32 t = best_t;
  const Vec3 p = channel.points[i];
  const Vec3 q = channel.points[i + 1];
  const f32 centre_depth = channel.depth_m[i] + (channel.depth_m[i + 1] - channel.depth_m[i]) * t;
  const f32 slope = channel.slope[i] + (channel.slope[i + 1] - channel.slope[i]) * t;
  const f32 r = 1.0f - best_share * best_share;  // the parabola's share of the centre's depth
  const f32 depth = centre_depth * r;
  if (!(depth > 0.0f)) return false;
  // Manning's mean speed for the centre's depth, and the 1/6-power profile across the width:
  // r^(1/6) = cbrt(sqrt(r)), both correctly rounded or this module's own, never `std::pow`.
  const f64 mean = static_cast<f64>(manning_speed(channel.manning_n, centre_depth, slope));
  const f64 share = k_channel_profile_mean * cube_root(std::sqrt(static_cast<f64>(r)));
  const f32 dx = q.x - p.x;
  const f32 dz = q.z - p.z;
  const f32 run = std::sqrt(dx * dx + dz * dz);
  const f32 speed = static_cast<f32>(mean * share);
  out.wet = true;
  out.level = channel.origin.y + static_cast<f64>(p.y + (q.y - p.y) * t);
  out.depth = depth;
  out.velocity = Vec2{dx / run * speed, dz / run * speed};
  out.source = 2;
  return true;
}

void WaterQuery::add_body(Body body, DepthMap map) {
  bodies_.push_back(HeldBody{std::move(body), std::move(map)});
}

void WaterQuery::add_channel(Channel channel) { channels_.push_back(std::move(channel)); }

void WaterQuery::sample(WorldPos at, WaterPoint& out) const noexcept {
  out = WaterPoint{};
  WaterPoint here;
  for (u32 b = 0; b < bodies_.size(); ++b) {
    if (body_water_at(bodies_[b].body, bodies_[b].map, ground_, at, here) &&
        (!out.wet || here.depth > out.depth)) {
      out = here;
      out.index = b;
    }
  }
  for (u32 c = 0; c < channels_.size(); ++c) {
    if (channel_water_at(channels_[c], at, here) && (!out.wet || here.depth > out.depth)) {
      out = here;
      out.index = c;
    }
  }
}

}  // namespace engine::water

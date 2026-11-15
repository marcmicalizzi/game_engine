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

bool channel_water_at(const Channel& channel, const ChannelLine& line,
                      const scene_gen::Ground& ground, WorldPos at, WaterPoint& out, u32 first,
                      u32 last) noexcept {
  out = WaterPoint{};
  ChannelSample s;
  if (!channel_sample(channel, line, ground, at, s, first, last)) return false;
  out.wet = true;
  out.level = s.level_m;
  out.depth = s.depth_m;
  out.velocity = Vec2{s.velocity.x, s.velocity.z};
  out.source = 2;
  return true;
}

void WaterQuery::add_body(Body body, DepthMap map) {
  bodies_.push_back(HeldBody{std::move(body), std::move(map)});
}

void WaterQuery::add_channel(Channel channel) {
  lines_.push_back(channel_line(channel));
  channels_.push_back(std::move(channel));
  cell_start_.clear();  // the index is built again by `prepare`
}

namespace {

// One stretch's claim on a cell of the index, before the claims are folded into spans.
struct Claim {
  u32 cell;
  u32 channel;
  u32 stretch;
};

// How far from its line any of a channel's stretches can hold a wet point, metres: the widest wet
// reach either side at any station, and the margin under the bank, as `stretch_reach` (channel.cpp)
// takes it. One number a channel, so every stretch that ties for nearest with the one holding the
// point is in the point's cell too (query.h, "The channels' index").
f32 channel_reach(const Channel& c) noexcept {
  f32 reach = 0.0f;
  for (u32 i = 0; i < c.points.size(); ++i)
    reach = std::max(reach, std::max(c.wet_left_m[i], c.wet_right_m[i]));
  return reach + k_channel_under_bank_m;
}

}  // namespace

void WaterQuery::prepare(const scene_gen::Ground* ground) {
  ground_ = ground;
  cell_start_.clear();
  spans_.clear();
  // The bounds of every stretch, grown by its channel's reach, in world metres.
  f64 x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
  for (u32 c = 0; c < channels_.size(); ++c) {
    const Channel& ch = channels_[c];
    if (ch.points.size() < 2) continue;
    const f64 r = static_cast<f64>(channel_reach(ch));
    for (const Vec3& p : ch.points) {
      x0 = std::min(x0, ch.origin.x + static_cast<f64>(p.x) - r);
      x1 = std::max(x1, ch.origin.x + static_cast<f64>(p.x) + r);
      z0 = std::min(z0, ch.origin.z + static_cast<f64>(p.z) - r);
      z1 = std::max(z1, ch.origin.z + static_cast<f64>(p.z) + r);
    }
  }
  if (!(x1 >= x0)) return;  // no channel with a stretch: nothing to index
  cell_m_ = k_channel_cell_m;
  while ((x1 - x0) / cell_m_ >= static_cast<f64>(k_channel_cells_max) ||
         (z1 - z0) / cell_m_ >= static_cast<f64>(k_channel_cells_max)) {
    cell_m_ *= 2.0;
  }
  grid_x_ = x0;
  grid_z_ = z0;
  cells_x_ = static_cast<u32>((x1 - x0) / cell_m_) + 1;
  cells_z_ = static_cast<u32>((z1 - z0) / cell_m_) + 1;
  // Every cell each stretch's grown bounds touch, then sorted by cell, channel and stretch.
  Vector<Claim> claims;  // the load's, freed on return; a question allocates nothing
  for (u32 c = 0; c < channels_.size(); ++c) {
    const Channel& ch = channels_[c];
    if (ch.points.size() < 2) continue;
    const f64 r = static_cast<f64>(channel_reach(ch));
    for (u32 i = 0; i + 1 < ch.points.size(); ++i) {
      const Vec3 p = ch.points[i];
      const Vec3 q = ch.points[i + 1];
      const f64 ax = ch.origin.x + static_cast<f64>(std::min(p.x, q.x)) - r;
      const f64 bx = ch.origin.x + static_cast<f64>(std::max(p.x, q.x)) + r;
      const f64 az = ch.origin.z + static_cast<f64>(std::min(p.z, q.z)) - r;
      const f64 bz = ch.origin.z + static_cast<f64>(std::max(p.z, q.z)) + r;
      const u32 i0 = cell_of(ax, cells_x_, grid_x_);
      const u32 i1 = cell_of(bx, cells_x_, grid_x_);
      const u32 j0 = cell_of(az, cells_z_, grid_z_);
      const u32 j1 = cell_of(bz, cells_z_, grid_z_);
      for (u32 j = j0; j <= j1; ++j)
        for (u32 k = i0; k <= i1; ++k) claims.push_back(Claim{j * cells_x_ + k, c, i});
    }
  }
  std::sort(claims.begin(), claims.end(), [](const Claim& a, const Claim& b) {
    if (a.cell != b.cell) return a.cell < b.cell;
    if (a.channel != b.channel) return a.channel < b.channel;
    return a.stretch < b.stretch;
  });
  // One span a channel a cell: its stretches from the first that touches the cell to the last.
  cell_start_.resize(cells_x_ * cells_z_ + 1, 0u);
  for (u32 n = 0; n < claims.size();) {
    const Claim& head = claims[n];
    u32 m = n + 1;
    while (m < claims.size() && claims[m].cell == head.cell && claims[m].channel == head.channel)
      ++m;
    spans_.push_back(ChannelSpan{head.channel, head.stretch, claims[m - 1].stretch});
    cell_start_[head.cell + 1] += 1;
    n = m;
  }
  for (u32 k = 0; k < cells_x_ * cells_z_; ++k) cell_start_[k + 1] += cell_start_[k];
}

u32 WaterQuery::cell_of(f64 v, u32 cells, f64 origin) const noexcept {
  const f64 f = std::floor((v - origin) / cell_m_);
  if (!(f > 0.0)) return 0;
  return f >= static_cast<f64>(cells - 1) ? cells - 1 : static_cast<u32>(f);
}

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
  if (channels_.empty()) return;
  static const scene_gen::Ground k_no_ground{};
  const scene_gen::Ground& ground = ground_ != nullptr ? *ground_ : k_no_ground;
  if (cell_start_.empty()) {
    // Not prepared: every channel, every stretch (what the index answers, slower).
    for (u32 c = 0; c < channels_.size(); ++c) {
      if (channel_water_at(channels_[c], lines_[c], ground, at, here) &&
          (!out.wet || here.depth > out.depth)) {
        out = here;
        out.index = c;
      }
    }
    return;
  }
  const f64 fx = std::floor((at.x - grid_x_) / cell_m_);
  const f64 fz = std::floor((at.z - grid_z_) / cell_m_);
  if (!(fx >= 0.0 && fz >= 0.0 && fx < static_cast<f64>(cells_x_) &&
        fz < static_cast<f64>(cells_z_))) {
    return;  // past every channel's reach
  }
  const u32 cell = static_cast<u32>(fz) * cells_x_ + static_cast<u32>(fx);
  for (u32 s = cell_start_[cell]; s < cell_start_[cell + 1]; ++s) {
    const ChannelSpan& span = spans_[s];
    if (channel_water_at(channels_[span.channel], lines_[span.channel], ground, at, here,
                         span.first, span.last) &&
        (!out.wet || here.depth > out.depth)) {
      out = here;
      out.index = span.channel;
    }
  }
}

}  // namespace engine::water

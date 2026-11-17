// What stands in a line's water (in_water.h; channels.md, "What stands in the water"; W138).
#include <domain/channels/in_water.h>
#include <domain/scene_gen/water_channel.h>

#include <algorithm>
#include <cmath>

namespace engine::channels {

namespace {

u64 cell_key(i64 ix, i64 iz) noexcept {
  return (static_cast<u64>(static_cast<u32>(static_cast<i32>(ix))) << 32) |
         static_cast<u64>(static_cast<u32>(static_cast<i32>(iz)));
}

i64 cell_of(f64 v) noexcept { return static_cast<i64>(std::floor(v / k_in_water_cell_m)); }

bool drawn_water(const scene::WaterChannel& b, u32 i) noexcept {
  const u16 fa = b.flags[i], fb = b.flags[i + 1];
  if ((fa & scene_gen::k_channel_fall_lip) != 0 && (fb & scene_gen::k_channel_fall_foot) != 0)
    return false;
  return scene_gen::channel_stretch_drawn(fa, fb, b.depth_m[i], b.depth_m[i + 1], b.points[i].y,
                                          b.points[i + 1].y);
}

bool readable(const scene::WaterChannel& b) noexcept {
  const u32 n = b.points.size();
  return n >= 2 && b.flags.size() == n && b.depth_m.size() == n && b.wet_left_m.size() == n &&
         b.wet_right_m.size() == n;
}

}  // namespace

ChannelWater::ChannelWater(std::span<const scene::WaterChannel> blocks) : blocks_(blocks) {
  for (u32 k = 0; k < blocks.size(); ++k) {
    const scene::WaterChannel& b = blocks[k];
    if (!readable(b)) continue;
    for (u32 i = 0; i + 1 < b.points.size(); ++i) {
      if (!drawn_water(b, i)) continue;
      const f64 reach =
          static_cast<f64>(std::max(std::max(b.wet_left_m[i], b.wet_right_m[i]),
                                    std::max(b.wet_left_m[i + 1], b.wet_right_m[i + 1])));
      const WorldPos& a = b.points[i];
      const WorldPos& c = b.points[i + 1];
      const i64 x0 = cell_of(std::min(a.x, c.x) - reach), x1 = cell_of(std::max(a.x, c.x) + reach);
      const i64 z0 = cell_of(std::min(a.z, c.z) - reach), z1 = cell_of(std::max(a.z, c.z) + reach);
      for (i64 iz = z0; iz <= z1; ++iz)
        for (i64 ix = x0; ix <= x1; ++ix)
          items_.push_back(Item{cell_key(ix, iz), k, i});
    }
  }
  std::sort(items_.begin(), items_.end(), [](const Item& p, const Item& q) {
    if (p.key != q.key) return p.key < q.key;
    if (p.block != q.block) return p.block < q.block;
    return p.stretch < q.stretch;
  });
}

bool ChannelWater::level_at(f64 x, f64 z, u32& block, f64& level) const {
  const u64 key = cell_key(cell_of(x), cell_of(z));
  auto it = std::lower_bound(items_.begin(), items_.end(), key,
                             [](const Item& p, u64 k) { return p.key < k; });
  for (; it != items_.end() && it->key == key; ++it) {
    const scene::WaterChannel& b = blocks_[it->block];
    const u32 i = it->stretch;
    const WorldPos& a = b.points[i];
    const WorldPos& c = b.points[i + 1];
    const f64 dx = c.x - a.x, dz = c.z - a.z;
    const f64 len = std::sqrt(dx * dx + dz * dz);
    if (!(len > 0.0)) continue;
    const f64 hx = dx / len, hz = dz / len;
    const f64 px = x - a.x, pz = z - a.z;
    const f64 u = px * hx + pz * hz;
    if (u < 0.0 || u > len) continue;  // a place past the stretch's ends is its neighbour's
    const f64 qx = px - hx * u, qz = pz - hz * u;
    const f64 distance = std::sqrt(qx * qx + qz * qz);
    const bool left = hx * pz - hz * px > 0.0;  // the draw's own side (systems/water `channel_at`)
    const f64 t = u / len;
    const f64 wa = static_cast<f64>(left ? b.wet_left_m[i] : b.wet_right_m[i]);
    const f64 wb = static_cast<f64>(left ? b.wet_left_m[i + 1] : b.wet_right_m[i + 1]);
    if (distance > wa + t * (wb - wa)) continue;
    block = it->block;
    level = a.y + t * (c.y - a.y);
    return true;
  }
  return false;
}

}  // namespace engine::channels

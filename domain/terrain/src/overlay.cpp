#include <core/hash/hash.h>
#include <domain/terrain/fixed.h>
#include <domain/terrain/overlay.h>

#include <bit>
#include <limits>

namespace engine::terrain {

namespace {

using namespace fx;

constexpr u32 k_magic = 0x31564F54u;  // "TOV1"
constexpr u32 k_vertices = k_overlay_cells * k_overlay_cells;
constexpr i64 k_rate_full = 255;

void put(Vector<u8>& out, u64 v, u32 bytes) {
  for (u32 i = 0; i < bytes; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xFFu));
}

u64 get(std::span<const u8> in, usize at, u32 bytes) noexcept {
  u64 v = 0;
  for (u32 i = 0; i < bytes; ++i)
    v |= static_cast<u64>(in[at + i]) << (8 * i);
  return v;
}

// The fill an absolute flux integral amounts to at a rate, mm: a floor of one function of time, so
// the amount between two times is the difference of two of these and splitting an interval never
// changes the sum.
i64 fill_mm(i64 flux_cm2, i64 rate, const OverlayRules& rules) noexcept {
  return floor_div(flux_cm2 * rules.fill_um_per_cm2 * rate, k_rate_full * 1000);
}

}  // namespace

const char* stamp_kind_name(StampKind kind) noexcept {
  switch (kind) {
    case StampKind::footprint: return "footprint";
    case StampKind::dig: return "dig";
    case StampKind::fill: return "fill";
  }
  return "unknown";
}

u64 OverlayRules::hash() const noexcept {
  u64 h = hash_bytes("engine.terrain.overlay", 22);
  h = hash_combine(h, static_cast<u64>(fill_um_per_cm2));
  h = hash_combine(h, static_cast<u64>(bury_mm));
  h = hash_combine(h, static_cast<u64>(max_depth_mm));
  h = hash_combine(h, static_cast<u64>(feedback.lag_unit_mm));
  h = hash_combine(h, static_cast<u64>(feedback.lag_max_units));
  h = hash_combine(h, static_cast<u64>(feedback.pit_volume_mm3));
  h = hash_combine(h, static_cast<u64>(feedback.pit_depth_mm));
  return hash_combine(h, static_cast<u64>(feedback.drift_volume_mm3));
}

void Overlay::reset(TileCoord tile, i64 tile_mm, i64 time_us) {
  tile_ = tile;
  tile_mm_ = tile_mm;
  as_of_us_ = time_us;
  last_nudge_day_ = std::numeric_limits<i32>::min();
  lag_ = TileLag{};
  cells_.assign(k_vertices, 0);
  rate_.assign(k_vertices, static_cast<u8>(k_rate_full));
  block_mask_ = 0;
  queue_count_ = 0;
}

void Overlay::set_fill_rates(const DuneField& field) {
  rate_.resize(k_vertices);
  const i64 s = spacing_mm();
  const i64 x0 = static_cast<i64>(tile_.x) * tile_mm_;
  const i64 z0 = static_cast<i64>(tile_.z) * tile_mm_;
  for (u32 j = 0; j < k_overlay_cells; ++j) {
    for (u32 i = 0; i < k_overlay_cells; ++i) {
      // Loose sand fills at the full rate; over a ridge the rate falls with the rock under the
      // vertex, and where the ridge is more than half the ground nothing moves at all.
      const i64 ridge = field.ridge_q16(x0 + i * s, z0 + j * s);
      const i64 rate = ridge >= 32768 ? 0 : (k_rate_full * (32768 - ridge)) / 32768;
      rate_[j * k_overlay_cells + i] = static_cast<u8>(rate);
    }
  }
}

bool Overlay::push(const Stamp& stamp, const WindRecord& wind, const OverlayRules& rules) {
  const i64 last = queue_count_ > 0 ? queue_[queue_count_ - 1].time_us : as_of_us_;
  if (stamp.time_us < last) return false;
  if (queue_count_ == k_stamp_queue) bake(wind, rules);
  queue_[queue_count_++] = stamp;
  return true;
}

void Overlay::advance(i64 time_us, const WindRecord& wind, const OverlayRules& rules) {
  bake(wind, rules);
  decay_to(time_us, wind, rules);
}

void Overlay::bake(const WindRecord& wind, const OverlayRules& rules) {
  for (u32 k = 0; k < queue_count_; ++k) {
    const Stamp& stamp = queue_[k];
    decay_to(stamp.time_us, wind, rules);
    apply(stamp, rules);
    if (stamp.kind == static_cast<u8>(StampKind::dig)) {
      // A pit big enough holds the dunes back a unit, at most once a game day (feedback.h).
      const i64 day = day_of(stamp.time_us);
      if (day > last_nudge_day_ &&
          dug_volume_mm3(rules.feedback.pit_depth_mm) >= rules.feedback.pit_volume_mm3) {
        for (u8& u : lag_.units)
          u = static_cast<u8>(min_i64(u + 1, rules.feedback.lag_max_units));
        last_nudge_day_ = day;
      }
    }
  }
  queue_count_ = 0;
}

void Overlay::decay_to(i64 time_us, const WindRecord& wind, const OverlayRules& rules) {
  if (time_us <= as_of_us_) return;
  if (block_mask_ != 0) {
    const i64 before = wind.integral(as_of_us_).magnitude;
    const i64 after = wind.integral(time_us).magnitude;
    i64 amount[k_rate_full + 1];
    for (i64 r = 0; r <= k_rate_full; ++r)
      amount[r] = fill_mm(after, r, rules) - fill_mm(before, r, rules);
    const i64 bury = rules.bury_mm;
    for (u32 b = 0; b < k_overlay_blocks * k_overlay_blocks; ++b) {
      if ((block_mask_ & (u64{1} << b)) == 0) continue;
      const u32 bx = (b % k_overlay_blocks) * k_overlay_block;
      const u32 bz = (b / k_overlay_blocks) * k_overlay_block;
      for (u32 j = bz; j < bz + k_overlay_block; ++j) {
        for (u32 i = bx; i < bx + k_overlay_block; ++i) {
          const u32 v = j * k_overlay_cells + i;
          i64 value = cells_[v];
          if (value == 0) continue;
          const i64 a = amount[rate_[v]];
          value = value > 0 ? max_i64(0, value - a) : min_i64(0, value + a);
          if (abs_i64(value) <= bury) value = 0;
          cells_[v] = static_cast<i16>(value);
        }
      }
    }
    refresh_blocks(block_mask_);
  }
  as_of_us_ = time_us;
}

void Overlay::apply(const Stamp& stamp, const OverlayRules& rules) {
  const i64 s = spacing_mm();
  const i64 x0 = static_cast<i64>(tile_.x) * tile_mm_;
  const i64 z0 = static_cast<i64>(tile_.z) * tile_mm_;
  const i64 r1 = max_i64(1, stamp.radius_mm);
  const i64 r2 = max_i64(1, stamp.radius2_mm);
  // The rim reaches 1.6 radii; nothing beyond it is touched.
  const i64 reach = (max_i64(r1, r2) * 8) / 5 + s;
  const i64 i_lo = max_i64(0, floor_div(stamp.x_mm - reach - x0, s));
  const i64 i_hi = min_i64(k_overlay_cells - 1, floor_div(stamp.x_mm + reach - x0, s) + 1);
  const i64 j_lo = max_i64(0, floor_div(stamp.z_mm - reach - z0, s));
  const i64 j_hi = min_i64(k_overlay_cells - 1, floor_div(stamp.z_mm + reach - z0, s) + 1);
  if (i_lo > i_hi || j_lo > j_hi) return;
  const i64 ax = cos_q15(stamp.yaw_turn) / 2;
  const i64 az = sin_q15(stamp.yaw_turn) / 2;
  const i64 depth = min_i64(abs_i64(stamp.depth_mm), rules.max_depth_mm);
  const i64 rim = min_i64(abs_i64(stamp.rim_mm), rules.max_depth_mm);
  const i64 cap = rules.max_depth_mm;
  for (i64 j = j_lo; j <= j_hi; ++j) {
    for (i64 i = i_lo; i <= i_hi; ++i) {
      const u32 v = static_cast<u32>(j * k_overlay_cells + i);
      if (rate_[v] == 0) continue;  // rock takes no print
      const i64 dx = x0 + i * s - stamp.x_mm;
      const i64 dz = z0 + j * s - stamp.z_mm;
      const i64 a = ((dx * ax + dz * az) >> 14) * 65536 / r1;
      const i64 b = ((dz * ax - dx * az) >> 14) * 65536 / r2;
      const i64 r =
          static_cast<i64>(isqrt(static_cast<u64>(((a * a) >> 16) + ((b * b) >> 16)) << 16));
      i64 value = cells_[v];
      if (r < k_one_q16) {
        // A flat floor out to half the radius, then a soft wall up to the rim.
        const i64 profile = falloff_q16(max_i64(0, (r - 32768) * 2));
        if (stamp.kind == static_cast<u8>(StampKind::fill)) {
          value = max_i64(value, (depth * profile) >> 16);
        } else {
          value = min_i64(value, -((depth * profile) >> 16));
        }
      } else if (rim > 0 && r < 104858 && stamp.kind != static_cast<u8>(StampKind::fill)) {
        // The sand pushed out raises a rim between 1 and 1.6 radii, highest at 1.25.
        const i64 off = abs_i64(r - 81920);
        value += (rim * falloff_q16((off * 65536) / 22938)) >> 16;
      }
      cells_[v] = static_cast<i16>(clamp_i64(value, -cap, cap));
    }
  }
  refresh_blocks(blocks_between(i_lo, j_lo, i_hi, j_hi));
}

void Overlay::declare_drift(const DriftDecl& drift, const WindRecord& wind,
                            const OverlayRules& rules) {
  // The drift is declared at the overlay's time, after anything queued before it.
  bake(wind, rules);
  const i64 s = spacing_mm();
  const i64 x0 = static_cast<i64>(tile_.x) * tile_mm_;
  const i64 z0 = static_cast<i64>(tile_.z) * tile_mm_;
  for (u32 j = 0; j < k_overlay_cells; ++j) {
    for (u32 i = 0; i < k_overlay_cells; ++i) {
      const u32 v = j * k_overlay_cells + i;
      if (rate_[v] == 0) continue;
      const i64 steady = drift_height_um(drift, x0 + i * s, z0 + j * s) / 1000;
      if (steady <= 0) continue;
      cells_[v] = static_cast<i16>(
          clamp_i64(min_i64(cells_[v], -steady), -rules.max_depth_mm, rules.max_depth_mm));
    }
  }
  refresh_blocks(~u64{0});
}

void Overlay::refresh_blocks(u64 candidates) noexcept {
  u64 mask = block_mask_ & ~candidates;
  for (u32 b = 0; b < k_overlay_blocks * k_overlay_blocks; ++b) {
    if ((candidates & (u64{1} << b)) == 0) continue;
    const u32 bx = (b % k_overlay_blocks) * k_overlay_block;
    const u32 bz = (b / k_overlay_blocks) * k_overlay_block;
    bool any = false;
    for (u32 j = bz; j < bz + k_overlay_block && !any; ++j) {
      for (u32 i = bx; i < bx + k_overlay_block; ++i) {
        if (cells_[j * k_overlay_cells + i] != 0) {
          any = true;
          break;
        }
      }
    }
    if (any) mask |= u64{1} << b;
  }
  block_mask_ = mask;
}

u64 Overlay::blocks_between(i64 i_lo, i64 j_lo, i64 i_hi, i64 j_hi) noexcept {
  u64 mask = 0;
  for (i64 bz = j_lo / k_overlay_block; bz <= j_hi / k_overlay_block; ++bz) {
    for (i64 bx = i_lo / k_overlay_block; bx <= i_hi / k_overlay_block; ++bx)
      mask |= u64{1} << (bz * k_overlay_blocks + bx);
  }
  return mask;
}

i32 Overlay::deviation_mm(i64 x, i64 z) const noexcept {
  const i64 s = spacing_mm();
  const i64 i = floor_div(x - static_cast<i64>(tile_.x) * tile_mm_ + s / 2, s);
  const i64 j = floor_div(z - static_cast<i64>(tile_.z) * tile_mm_ + s / 2, s);
  if (i < 0 || j < 0 || i >= k_overlay_cells || j >= k_overlay_cells || cells_.empty()) return 0;
  return cells_[static_cast<u32>(j * k_overlay_cells + i)];
}

bool Overlay::empty() const noexcept {
  return block_mask_ == 0 && lag_.zero() && queue_count_ == 0;
}

u32 Overlay::nonzero_blocks() const noexcept {
  return static_cast<u32>(std::popcount(block_mask_));
}

i64 Overlay::dug_volume_mm3(i32 below_mm) const noexcept {
  const i64 area = spacing_mm() * spacing_mm();
  i64 volume = 0;
  for (const i16 v : cells_) {
    if (v < -below_mm) volume += -static_cast<i64>(v) * area;
  }
  return volume;
}

void Overlay::write(Vector<u8>& out, const OverlayRules& rules) const {
  out.clear();
  if (empty()) return;
  put(out, k_magic, 4);
  put(out, k_overlay_record_version, 2);
  put(out, queue_count_, 2);
  put(out, static_cast<u32>(tile_.x), 4);
  put(out, static_cast<u32>(tile_.z), 4);
  put(out, static_cast<u32>(tile_mm_), 4);
  put(out,
      static_cast<u32>(clamp_i64(last_nudge_day_, std::numeric_limits<i32>::min(),
                                 std::numeric_limits<i32>::max())),
      4);
  put(out, static_cast<u64>(as_of_us_), 8);
  put(out, rules.hash(), 8);
  put(out, block_mask_, 8);
  for (const u8 u : lag_.units)
    put(out, u, 1);
  put(out, 0, 1);
  put(out, 0, 4);
  for (u32 b = 0; b < k_overlay_blocks * k_overlay_blocks; ++b) {
    if ((block_mask_ & (u64{1} << b)) == 0) continue;
    const u32 bx = (b % k_overlay_blocks) * k_overlay_block;
    const u32 bz = (b / k_overlay_blocks) * k_overlay_block;
    for (u32 j = bz; j < bz + k_overlay_block; ++j) {
      for (u32 i = bx; i < bx + k_overlay_block; ++i)
        put(out, static_cast<u16>(cells_[j * k_overlay_cells + i]), 2);
    }
  }
  for (u32 k = 0; k < queue_count_; ++k) {
    const Stamp& s = queue_[k];
    put(out, static_cast<u64>(s.time_us), 8);
    put(out, static_cast<u32>(s.x_mm), 4);
    put(out, static_cast<u32>(s.z_mm), 4);
    put(out, s.radius_mm, 2);
    put(out, s.radius2_mm, 2);
    put(out, s.yaw_turn, 2);
    put(out, static_cast<u16>(s.depth_mm), 2);
    put(out, static_cast<u16>(s.rim_mm), 2);
    put(out, s.kind, 1);
    put(out, 0, 5);
  }
}

bool Overlay::read(std::span<const u8> in, TileCoord tile, i64 tile_mm, const OverlayRules& rules,
                   std::string* error) {
  const auto fail = [&](const char* why) {
    if (error != nullptr) *error = why;
    return false;
  };
  reset(tile, tile_mm, 0);
  if (in.size() < k_overlay_header_bytes) return fail("an overlay record is at least 56 bytes");
  if (get(in, 0, 4) != k_magic) return fail("not an overlay record");
  if (get(in, 4, 2) != k_overlay_record_version)
    return fail("an overlay record of another version");
  const u32 count = static_cast<u32>(get(in, 6, 2));
  const TileCoord stored{static_cast<i32>(static_cast<u32>(get(in, 8, 4))),
                         static_cast<i32>(static_cast<u32>(get(in, 12, 4)))};
  if (!(stored == tile)) return fail("the overlay record is another tile's");
  if (static_cast<i64>(static_cast<i32>(get(in, 16, 4))) != tile_mm)
    return fail("the overlay record is for another tile size");
  if (get(in, 32, 8) != rules.hash())
    return fail("the overlay record was written under other rules");
  const u64 mask = get(in, 40, 8);
  const usize expected =
      k_overlay_header_bytes +
      static_cast<usize>(std::popcount(mask)) * k_overlay_block * k_overlay_block * 2 +
      static_cast<usize>(count) * k_stamp_bytes;
  if (count > k_stamp_queue || in.size() != expected)
    return fail("the overlay record's size is wrong");
  last_nudge_day_ = static_cast<i32>(static_cast<u32>(get(in, 20, 4)));
  as_of_us_ = static_cast<i64>(get(in, 24, 8));
  for (u32 b = 0; b < k_bands; ++b)
    lag_.units[b] = static_cast<u8>(get(in, 48 + b, 1));
  usize at = k_overlay_header_bytes;
  for (u32 b = 0; b < k_overlay_blocks * k_overlay_blocks; ++b) {
    if ((mask & (u64{1} << b)) == 0) continue;
    const u32 bx = (b % k_overlay_blocks) * k_overlay_block;
    const u32 bz = (b / k_overlay_blocks) * k_overlay_block;
    for (u32 j = bz; j < bz + k_overlay_block; ++j) {
      for (u32 i = bx; i < bx + k_overlay_block; ++i) {
        cells_[j * k_overlay_cells + i] = static_cast<i16>(static_cast<u16>(get(in, at, 2)));
        at += 2;
      }
    }
  }
  for (u32 k = 0; k < count; ++k) {
    Stamp& s = queue_[k];
    s = Stamp{};
    s.time_us = static_cast<i64>(get(in, at, 8));
    s.x_mm = static_cast<i32>(static_cast<u32>(get(in, at + 8, 4)));
    s.z_mm = static_cast<i32>(static_cast<u32>(get(in, at + 12, 4)));
    s.radius_mm = static_cast<u16>(get(in, at + 16, 2));
    s.radius2_mm = static_cast<u16>(get(in, at + 18, 2));
    s.yaw_turn = static_cast<u16>(get(in, at + 20, 2));
    s.depth_mm = static_cast<i16>(static_cast<u16>(get(in, at + 22, 2)));
    s.rim_mm = static_cast<i16>(static_cast<u16>(get(in, at + 24, 2)));
    s.kind = static_cast<u8>(get(in, at + 26, 1));
    at += k_stamp_bytes;
  }
  queue_count_ = count;
  refresh_blocks(~u64{0});
  if (block_mask_ != mask) return fail("the overlay record's block mask disagrees with its blocks");
  return true;
}

i64 Overlay::burial_time_us(const WindRecord& wind, const OverlayRules& rules) const {
  Overlay baked = *this;
  baked.bake(wind, rules);
  if (baked.block_mask_ == 0) return baked.as_of_us_;
  // The deepest deviation at each fill rate is the last of that rate to be buried.
  i64 worst[k_rate_full + 1] = {};
  for (u32 v = 0; v < k_vertices; ++v) {
    const i64 value = abs_i64(baked.cells_[v]);
    if (value == 0) continue;
    if (baked.rate_[v] == 0) return std::numeric_limits<i64>::max();
    worst[baked.rate_[v]] = max_i64(worst[baked.rate_[v]], value);
  }
  const i64 start = baked.as_of_us_;
  const i64 flux0 = wind.integral(start).magnitude;
  i64 latest = start;
  for (i64 r = 1; r <= k_rate_full; ++r) {
    if (worst[r] == 0) continue;
    const i64 need = worst[r] - rules.bury_mm;
    const i64 base = fill_mm(flux0, r, rules);
    const auto buried = [&](i64 t) {
      return fill_mm(wind.integral(t).magnitude, r, rules) - base >= need;
    };
    i64 hi = start + k_us_per_day;
    u32 doublings = 0;
    while (!buried(hi)) {
      if (++doublings > 40) return std::numeric_limits<i64>::max();
      hi = start + (hi - start) * 2;
    }
    i64 lo = start;
    while (hi - lo > 1) {
      const i64 mid = lo + (hi - lo) / 2;
      if (buried(mid))
        hi = mid;
      else
        lo = mid;
    }
    latest = max_i64(latest, hi);
  }
  return latest;
}

}  // namespace engine::terrain

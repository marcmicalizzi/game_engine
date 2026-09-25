#include <domain/terrain/feedback.h>
#include <domain/terrain/fixed.h>

namespace engine::terrain {

using namespace fx;

i64 drift_volume_mm3(const DriftDecl& d) noexcept {
  const i64 length_mm = length(d.to_x - d.from_x, d.to_z - d.from_z);
  return (length_mm * d.height_mm * d.reach_mm) / 2;
}

i64 drift_height_um(const DriftDecl& d, i64 x, i64 z) noexcept {
  if (d.reach_mm <= 0 || d.height_mm <= 0) return 0;
  const i64 fx_ = d.to_x - d.from_x;
  const i64 fz_ = d.to_z - d.from_z;
  const i64 len = length(fx_, fz_);
  if (len <= 0) return 0;
  const i64 px = x - d.from_x;
  const i64 pz = z - d.from_z;
  // Cheap rejection before any product that could be large: the drift lies within its face's
  // length plus its reach of the face's first end.
  if (abs_i64(px) > len + d.reach_mm || abs_i64(pz) > len + d.reach_mm) return 0;
  const i64 out = (px * d.normal_x_q14 + pz * d.normal_z_q14) >> 14;
  if (out < 0 || out >= d.reach_mm) return 0;
  const i64 along = (px * fx_ + pz * fz_) / len;
  if (along < 0 || along > len) return 0;
  return (static_cast<i64>(d.height_mm) * 1000 * (d.reach_mm - out)) / d.reach_mm;
}

u32 derived_lag_units(std::span<const DriftDecl> drifts, i64 day,
                      const FeedbackRules& rules) noexcept {
  i64 earliest = day + 1;
  for (const DriftDecl& d : drifts) {
    if (drift_volume_mm3(d) >= rules.drift_volume_mm3) earliest = min_i64(earliest, d.since_day);
  }
  if (earliest > day) return 0;
  return static_cast<u32>(min_i64(day - earliest, rules.lag_max_units));
}

LagField::LagField(i64 tile_mm, const FeedbackRules& rules) : tile_mm_(tile_mm), rules_(rules) {}

void LagField::set(TileCoord tile, TileLag lag) {
  for (u8& u : lag.units)
    u = static_cast<u8>(min_i64(u, rules_.lag_max_units));
  lag.reserved = 0;
  const u64 key = tile_key(tile);
  if (lag.zero()) {
    tiles_.erase(key);
    return;
  }
  tiles_[u64{key}] = lag;
}

TileLag LagField::get(TileCoord tile) const noexcept {
  const TileLag* found = tiles_.find_value(tile_key(tile));
  return found != nullptr ? *found : TileLag{};
}

i64 LagField::lag_mm(u32 slot, i64 x, i64 z) const noexcept {
  if (tiles_.empty()) return 0;
  // Tile centres sit at (i + 1/2) tile; interpolate between the four round the point.
  const i64 half = tile_mm_ / 2;
  const i64 i0 = floor_div(x - half, tile_mm_);
  const i64 j0 = floor_div(z - half, tile_mm_);
  const i64 tx = ((x - half - i0 * tile_mm_) * 65536) / tile_mm_;
  const i64 tz = ((z - half - j0 * tile_mm_) * 65536) / tile_mm_;
  const auto at = [&](i64 i, i64 j) -> i64 {
    return get(TileCoord{static_cast<i32>(i), static_cast<i32>(j)}).units[slot];
  };
  const i64 a = at(i0, j0);
  const i64 b = at(i0 + 1, j0);
  const i64 c = at(i0, j0 + 1);
  const i64 d = at(i0 + 1, j0 + 1);
  if ((a | b | c | d) == 0) return 0;
  const i64 top = a * (k_one_q16 - tx) + b * tx;
  const i64 bottom = c * (k_one_q16 - tx) + d * tx;
  const i64 units_q32 = top * (k_one_q16 - tz) + bottom * tz;
  return (units_q32 * rules_.lag_unit_mm) >> 32;
}

}  // namespace engine::terrain

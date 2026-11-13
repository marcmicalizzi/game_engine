#include "domain/scene_gen/seat.h"

#include <domain/scene_gen/scene_gen.h>

#include <cmath>

namespace engine::scene_gen {

namespace {

// cos and sin of 22.5 and 45 degrees, to the double's last bit: the rim's directions are these
// constants and their reflections, never a C library's sine, so every toolchain asks the same
// points.
constexpr f64 k_cos_22_5 = 0.92387953251128674;
constexpr f64 k_sin_22_5 = 0.38268343236508978;
constexpr f64 k_cos_45 = 0.70710678118654752;

// Half away from zero, so a negative offset is its positive's negation.
i64 round_away(f64 v) noexcept {
  return v < 0.0 ? -static_cast<i64>(std::floor(-v + 0.5)) : static_cast<i64>(std::floor(v + 0.5));
}

i64 height_mm(const Ground& ground, i64 x_mm, i64 z_mm) noexcept {
  return nearest_mm(ground.surface_mm(x_mm, z_mm));
}

bool in_hole(const Ground& ground, const Exclusion* exclusion, i64 x_mm, i64 z_mm) noexcept {
  return exclusion != nullptr && exclusion->excluded_mm(ground, x_mm, z_mm, k_exclude_holes);
}

}  // namespace

GroundSlope quantize_slope(f64 gx, f64 gz) noexcept {
  const auto word = [](f64 g) {
    const f64 steps = g * k_slope_unit;
    if (!(steps > -32767.0)) return static_cast<i16>(-32767);  // and NaN, which is no slope
    if (steps >= 32767.0) return static_cast<i16>(32767);
    return static_cast<i16>(round_away(steps));
  };
  GroundSlope out;
  out.gx = word(gx);
  out.gz = word(gz);
  if (std::isnan(gx)) out.gx = 0;
  if (std::isnan(gz)) out.gz = 0;
  return out;
}

void seat_rim_offset(i64 radius_mm, u32 k, i64& dx_mm, i64& dz_mm) noexcept {
  const f64 r = static_cast<f64>(radius_mm);
  // The first octant's three magnitudes, then every point a reflection of one of them.
  const i64 a = round_away(r * k_cos_22_5);
  const i64 b = round_away(r * k_sin_22_5);
  const i64 h = round_away(r * k_cos_45);
  static constexpr i8 k_table[16][4] = {
      // which magnitude for x and z (0: none, 1: r, 2: a, 3: h, 4: b) and their signs
      {1, 0, 1, 1},  {2, 4, 1, 1},  {3, 3, 1, 1},  {4, 2, 1, 1},   {0, 1, 1, 1},   {4, 2, -1, 1},
      {3, 3, -1, 1}, {2, 4, -1, 1}, {1, 0, -1, 1}, {2, 4, -1, -1}, {3, 3, -1, -1}, {4, 2, -1, -1},
      {0, 1, 1, -1}, {4, 2, 1, -1}, {3, 3, 1, -1}, {2, 4, 1, -1},
  };
  const auto magnitude = [&](i8 which) -> i64 {
    switch (which) {
      case 1: return radius_mm;
      case 2: return a;
      case 3: return h;
      case 4: return b;
      default: return 0;
    }
  };
  const i8* row = k_table[k % k_seat_rim_samples];
  dx_mm = magnitude(row[0]) * row[2];
  dz_mm = magnitude(row[1]) * row[3];
}

GroundDisc ground_disc(const Ground& ground, const Exclusion* exclusion, i64 x_mm, i64 z_mm,
                       i64 radius_mm) noexcept {
  GroundDisc out;
  if (in_hole(ground, exclusion, x_mm, z_mm)) {
    out.centre_hole = true;
    return out;
  }
  out.centre_mm = height_mm(ground, x_mm, z_mm);
  out.min_mm = out.max_mm = out.centre_mm;
  if (radius_mm <= 0) return out;
  // The rim, in its fixed order. Heights relative to the centre, so the sums stay small and exact.
  i64 dx[k_seat_rim_samples];
  i64 dz[k_seat_rim_samples];
  i64 dh[k_seat_rim_samples];
  u32 n = 0;
  for (u32 k = 0; k < k_seat_rim_samples; ++k) {
    i64 ox = 0;
    i64 oz = 0;
    seat_rim_offset(radius_mm, k, ox, oz);
    if (in_hole(ground, exclusion, x_mm + ox, z_mm + oz)) continue;
    const i64 h = height_mm(ground, x_mm + ox, z_mm + oz);
    out.min_mm = h < out.min_mm ? h : out.min_mm;
    out.max_mm = h > out.max_mm ? h : out.max_mm;
    dx[n] = ox;
    dz[n] = oz;
    dh[n] = h - out.centre_mm;
    ++n;
  }
  out.rim_samples = n;
  out.drop_mm = out.centre_mm - out.min_mm;
  if (n < 3) return out;
  // Least squares of dh = c + gx dx + gz dz over the samples: the centred sums as n times the
  // moment less the product of the sums, in integers (every term well inside an i64 for a rim of
  // up to a kilometre and heights to a hundred kilometres apart), then one 2 by 2 solve in f64.
  i64 sx = 0, sz = 0, sh = 0, sxx = 0, szz = 0, sxz = 0, sxh = 0, szh = 0;
  for (u32 i = 0; i < n; ++i) {
    sx += dx[i];
    sz += dz[i];
    sh += dh[i];
    sxx += dx[i] * dx[i];
    szz += dz[i] * dz[i];
    sxz += dx[i] * dz[i];
    sxh += dx[i] * dh[i];
    szh += dz[i] * dh[i];
  }
  const i64 m = static_cast<i64>(n);
  const f64 cxx = static_cast<f64>(m * sxx - sx * sx);
  const f64 czz = static_cast<f64>(m * szz - sz * sz);
  const f64 cxz = static_cast<f64>(m * sxz - sx * sz);
  const f64 cxh = static_cast<f64>(m * sxh - sx * sh);
  const f64 czh = static_cast<f64>(m * szh - sz * sh);
  const f64 det = cxx * czz - cxz * cxz;
  if (!(det > 0.0)) return out;  // the samples on one line: no plane
  out.gx = (cxh * czz - czh * cxz) / det;
  out.gz = (czh * cxx - cxh * cxz) / det;
  const f64 c =
      (static_cast<f64>(sh) - out.gx * static_cast<f64>(sx) - out.gz * static_cast<f64>(sz)) /
      static_cast<f64>(m);
  f64 worst = 0.0;
  for (u32 i = 0; i < n; ++i) {
    const f64 plane = c + out.gx * static_cast<f64>(dx[i]) + out.gz * static_cast<f64>(dz[i]);
    const f64 off = std::fabs(static_cast<f64>(dh[i]) - plane);
    worst = off > worst ? off : worst;
  }
  out.residual_mm = worst;
  return out;
}

i64 seated_y_mm(const GroundDisc& disc, i64 y_mm, i64 skirt_mm) noexcept {
  if (disc.centre_hole) return y_mm;
  const i64 highest = disc.min_mm + skirt_mm - k_seat_margin_mm;
  return highest < y_mm ? highest : y_mm;
}

SeatResult seat_on_ground(const Ground& ground, const Exclusion* exclusion, const MeshSeat& seat,
                          Vec3 scale, WorldPos& position) noexcept {
  SeatResult out;
  if (!seat.valid() || !ground.present()) return out;
  const f64 across = static_cast<f64>(std::fmax(std::fabs(scale.x), std::fabs(scale.z)));
  const i64 radius_mm = nearest_mm(static_cast<f64>(seat.radius_m) * across);
  const i64 skirt_mm =
      nearest_mm(static_cast<f64>(seat.skirt_m) * std::fabs(static_cast<f64>(scale.y)));
  const GroundDisc disc =
      ground_disc(ground, exclusion, nearest_mm(position.x), nearest_mm(position.z), radius_mm);
  if (disc.centre_hole) return out;
  out.seated = true;
  out.slope = disc.slope();
  const i64 y_mm = nearest_mm(position.y);
  const i64 seated = seated_y_mm(disc, y_mm, skirt_mm);
  if (seated < y_mm) {
    // Whole millimetres, in f64: the seated origin is the lowest ground plus the skirt less the
    // margin, exactly, and never a rounding of the instance's own height.
    const f64 y = static_cast<f64>(seated) / 1000.0;
    out.lowered_m = position.y - y;
    position.y = y;
  }
  return out;
}

}  // namespace engine::scene_gen

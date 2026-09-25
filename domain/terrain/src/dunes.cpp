#include <core/hash/hash.h>
#include <domain/terrain/dunes.h>
#include <domain/terrain/feedback.h>
#include <domain/terrain/fixed.h>

#include <cmath>

namespace engine::terrain {

namespace {

using namespace fx;

constexpr u64 k_tag_field = 0x44554E454649454Cull;  // "DUNEFIEL"
constexpr u64 k_tag_roll = 11;
constexpr u64 k_tag_grain = 12;

// The angle of repose of dry sand, 34 degrees: a slip face's slope, and so the width of the lee of
// a crest of a given height.
constexpr i64 k_tan_repose_q16 = 44205;
constexpr i64 k_cot_repose_q16 = 97161;
// The sand thins to 40% over a ridge's crest, as the renderer's heightfield always had it.
constexpr i64 k_ridge_thinning_q16 = 39322;
// Ripples: 12 cm apart, 6 mm high at the mean wind, drifting 1 cm a minute at it, and realigned to
// a new day's wind over two hours.
constexpr i64 k_ripple_mm = 120;
constexpr i64 k_ripple_um = 6'000;
constexpr i64 k_ripple_speed_um_per_s = 167;
constexpr i64 k_ripple_realign_us = 2 * 3600 * k_us_per_second;
constexpr i64 k_grain_mm = 40;
constexpr i64 k_grain_um = 1'500;

u64 cell_hash(u64 seed, u32 band, i64 i, i64 j) noexcept {
  u64 h = hash_combine(hash_combine(seed, k_tag_field), band);
  h = hash_combine(h, static_cast<u64>(i));
  return hash_combine(h, static_cast<u64>(j));
}

u64 sub(u64 h, u64 k) noexcept { return hash_combine(h, k); }

// The distance from p to the segment ab, mm; `far` when p is plainly beyond `limit` of it, so a
// point a continent away never multiplies its coordinates into an overflow.
i64 segment_distance(i64 px, i64 pz, const RidgeFeature& r, i64 limit) noexcept {
  const i64 far = limit + 1;
  if (px < min_i64(r.from_x, r.to_x) - limit || px > max_i64(r.from_x, r.to_x) + limit ||
      pz < min_i64(r.from_z, r.to_z) - limit || pz > max_i64(r.from_z, r.to_z) + limit) {
    return far;
  }
  const i64 abx = r.to_x - r.from_x;
  const i64 abz = r.to_z - r.from_z;
  const i64 apx = px - r.from_x;
  const i64 apz = pz - r.from_z;
  const i64 len2 = abx * abx + abz * abz;
  i64 t_q16 = 0;
  if (len2 > 0) t_q16 = clamp_i64(((apx * abx + apz * abz) * 65536) / len2, 0, k_one_q16);
  const i64 dx = apx - ((abx * t_q16) >> 16);
  const i64 dz = apz - ((abz * t_q16) >> 16);
  return length(dx, dz);
}

// sin^2-free profiles: a transverse crest's cross-section at signed distance `u` from the crest
// (positive is the lee), Q16 of its height.
// The divisions are multiplications by a Q32 reciprocal the gather worked out once per primitive
// (`Primitive::inv_*`): a point of a tile tests every primitive of the gather, and a division per
// profile per point was most of a tile's cost.
i64 cross_profile(i64 u, const Primitive& p) noexcept {
  if (u < 0) return falloff_q16((-u * p.inv_stoss) >> 16);
  const i64 s = (u * p.inv_width) >> 16;
  const i64 linear = max_i64(0, k_one_q16 - s);
  const i64 smooth = falloff_q16(s / 3);
  return (linear * p.sharp_q16 + smooth * (k_one_q16 - p.sharp_q16)) >> 16;
}

u32 reciprocal_q32(i64 v) noexcept { return static_cast<u32>((i64{1} << 32) / max_i64(2, v)); }

i64 lattice_value(u64 seed, i64 i, i64 j) noexcept {
  const u64 h = hash_combine(hash_combine(hash_combine(seed, k_tag_grain), static_cast<u64>(i)),
                             static_cast<u64>(j));
  return draw(h, -k_grain_um, k_grain_um);
}

}  // namespace

const char* band_name(Band band) noexcept {
  switch (band) {
    case Band::draa: return "draa";
    case Band::crest: return "crest";
    case Band::barchan: return "barchan";
  }
  return "unknown";
}

const char* detail_name(Detail detail) noexcept {
  switch (detail) {
    case Detail::full: return "full";
    case Detail::dunes: return "dunes";
    case Detail::coarse: return "coarse";
    case Detail::floor: return "floor";
  }
  return "unknown";
}

Detail detail_for_spacing(i64 spacing_mm) noexcept {
  return spacing_mm * 4 <= k_ripple_mm ? Detail::full : Detail::dunes;
}

Detail detail_for_distance(i64 distance_mm) noexcept {
  // Barchans are a metre high and ten across: past 2 km they are a pixel or two at 1080p and a
  // tile built for that distance leaves them out. Nearer, every band is drawn; ripples are the
  // renderer's near-field refinement, never a tile mesh's (detail_for_spacing).
  return distance_mm > 2'000'000 ? Detail::coarse : Detail::dunes;
}

i64 to_mm(f32 metres) noexcept {
  return static_cast<i64>(std::floor(static_cast<f64>(metres) * 1000.0 + 0.5));
}

u64 field_hash(const FieldDesc& desc) noexcept {
  u64 h = hash_bytes("engine.terrain.dunes", 20);
  h = hash_combine(h, k_generator_version);
  h = hash_combine(h, desc.seed);
  h = hash_combine(h, static_cast<u64>(desc.dune_height));
  h = hash_combine(h, static_cast<u64>(desc.wavelength));
  h = hash_combine(h, static_cast<u64>(desc.roll_q16));
  h = hash_combine(h, static_cast<u64>(desc.ridge_lag_q16));
  const WindParams& w = desc.wind;
  h = hash_combine(h, w.seed);
  h = hash_combine(h, w.prevailing_turn);
  h = hash_combine(h, w.seeded_direction ? 1u : 0u);
  h = hash_combine(h, static_cast<u64>(w.flux_cm2_per_day));
  h = hash_combine(h, static_cast<u64>(w.swing_turn));
  h = hash_combine(h, static_cast<u64>(w.jitter_turn));
  h = hash_combine(h, static_cast<u64>(w.seasonal_q16));
  h = hash_combine(h, static_cast<u64>(w.gust_q16));
  h = hash_combine(h, static_cast<u64>(w.calm_q16));
  h = hash_combine(h, desc.ridges.size());
  for (const RidgeFeature& r : desc.ridges) {
    h = hash_combine(hash_combine(h, static_cast<u64>(r.from_x)), static_cast<u64>(r.from_z));
    h = hash_combine(hash_combine(h, static_cast<u64>(r.to_x)), static_cast<u64>(r.to_z));
    h = hash_combine(h, static_cast<u64>(r.width));
  }
  h = hash_combine(h, desc.basins.size());
  for (const BasinFeature& b : desc.basins) {
    h = hash_combine(hash_combine(h, static_cast<u64>(b.x)), static_cast<u64>(b.z));
    h = hash_combine(h, static_cast<u64>(b.radius));
  }
  return h;
}

DuneField::DuneField(const FieldDesc& desc)
    : desc_(desc), wind_(desc.wind), hash_(field_hash(desc)) {
  const i64 h0_cm = max_i64(1, desc.dune_height / 10);
  const i64 lambda_cm = max_i64(100, desc.wavelength / 10);
  // Draa: long ridges two wavelengths apart, the dune height tall.
  band_[0] = BandParams{2 * lambda_cm, (h0_cm * 3) / 4, (h0_cm * 5) / 4, k_one_q16, 36045,
                        62259,         27525,           11796,           4551};
  // Crest segments at the wavelength, about half as tall, riding on them.
  band_[1] = BandParams{
      lambda_cm, (h0_cm * 3) / 10, (h0_cm * 6) / 10, 55706, 29491, 55706, 26214, 16384, 3641};
  // Barchans on the flats, sparse.
  band_[2] = BandParams{(lambda_cm * 7) / 10, h0_cm / 5, (h0_cm * 9) / 20, 26214, 0, 0, 0, 0, 0};
  for (u32 b = 0; b < k_bands; ++b) {
    const BandParams& p = band_[b];
    cell_[b] = p.cell_cm * 10;
    celerity_height_[b] = max_i64(10, (p.height_lo_cm + p.height_hi_cm) * 5);
    const i64 h_hi_um = p.height_hi_cm * 10'000;
    if (b == static_cast<u32>(Band::barchan)) {
      reach_[b] = (h_hi_um * 11) / 2000 + 10;
    } else {
      const i64 lee_smooth = 3 * ((h_hi_um * k_cot_repose_q16) / 65'536'000) + 1;
      const i64 cell = cell_[b];
      reach_[b] = ((cell * p.half_length_hi_q16) >> 16) + ((cell * p.stoss_q16) >> 16) +
                  lee_smooth + ((cell * p.bend_q16) >> 16) + 10;
    }
  }
  const u64 roll = sub(hash_combine(desc.seed, k_tag_field), k_tag_roll);
  const u32 roll_turn = static_cast<u32>(wind_.prevailing_turn()) + 12516u;
  roll_x_ = cos_q15(roll_turn) / 2;
  roll_z_ = sin_q15(roll_turn) / 2;
  roll_length_ = max_i64(1000, 11 * desc.wavelength);
  roll_phase_ = static_cast<u32>(roll >> 48);
  for (const RidgeFeature& r : desc.ridges)
    max_ridge_lag_ = max_i64(max_ridge_lag_, (r.width * desc.ridge_lag_q16) >> 16);
}

void DuneField::displacement(Band band, i64 time_us, i64& dx, i64& dz) const noexcept {
  // Bagnold: a dune's celerity is the sand flux over its height, so a band moves by the flux
  // integral over its height — cm^2 over mm, times 100 for mm^2.
  const FluxIntegral in = wind_.integral(time_us);
  const i64 h = celerity_height_[static_cast<u32>(band)];
  dx = floor_div(in.x * 100, h);
  dz = floor_div(in.z * 100, h);
}

i64 DuneField::max_lag_mm(const LagField* lag) const noexcept {
  return max_ridge_lag_ + (lag != nullptr ? lag->max_lag_mm() : 0);
}

DuneField::TimeShape DuneField::time_shape(i64 time_us) const noexcept {
  TimeShape s;
  const FluxIntegral r120 = wind_.between(time_us - 120 * k_us_per_day, time_us);
  const FluxIntegral r30 = wind_.between(time_us - 30 * k_us_per_day, time_us);
  s.r120x = r120.x;
  s.r120z = r120.z;
  s.r30x = r30.x;
  s.r30z = r30.z;
  // Three quarters of a month of the mean flux, blowing one way, is a slip face at the angle of
  // repose; the same against a crest's side, over four months, puts its slip face on the other
  // side.
  s.sharp_ref = max_i64(1, (30 * static_cast<i64>(desc_.wind.flux_cm2_per_day) * 3) / 4);
  // A barchan points down the month's resultant; with next to no wind it keeps to the prevailing
  // direction, which the small bias below makes continuous through a calm.
  const i64 bias = s.sharp_ref / 8;
  const i64 vx = r30.x + ((wind_.prevailing_x_q14() * bias) >> 14);
  const i64 vz = r30.z + ((wind_.prevailing_z_q14() * bias) >> 14);
  const i64 len = max_i64(1, length(vx, vz));
  s.barchan_x = static_cast<i32>((vx * k_one_q14) / len);
  s.barchan_z = static_cast<i32>((vz * k_one_q14) / len);
  return s;
}

bool DuneField::make_primitive(u32 band, i64 i, i64 j, const TimeShape& shape,
                               Primitive& out) const noexcept {
  const BandParams& p = band_[band];
  const u64 h = cell_hash(desc_.seed, band, i, j);
  if (unit_q16(sub(h, 0)) >= p.presence_q16) return false;
  const i64 cell = p.cell_cm;
  const i64 cx_cm = i * cell + cell / 4 + draw(sub(h, 1), 0, cell / 2);
  const i64 cz_cm = j * cell + cell / 4 + draw(sub(h, 2), 0, cell / 2);
  const i64 height_cm = draw(sub(h, 3), p.height_lo_cm, p.height_hi_cm);
  out = Primitive{};
  out.cx = cx_cm * 10;
  out.cz = cz_cm * 10;
  out.height = static_cast<i32>(height_cm * 10'000);
  out.band = static_cast<u8>(band);
  out.cell_hash = static_cast<u16>(h);
  const i64 h_mm = height_cm * 10;
  if (band == static_cast<u32>(Band::barchan)) {
    // A barchan: a dome 4 H upwind, 5.5 H downwind and 5.5 H either side, and a scoop 3.5 H in
    // radius whose rim is the brink, just downwind of the summit: its slip face falls at the angle
    // of repose inside the scoop, and the dome's flanks either side of it are the horns.
    out.kind = static_cast<u8>(PrimitiveKind::barchan);
    out.ax = shape.barchan_x;
    out.az = shape.barchan_z;
    out.half_length = static_cast<i32>((h_mm * 11) / 2);
    out.stoss = static_cast<i32>(h_mm * 4);
    out.bend = static_cast<i32>((h_mm * 11) / 2);
    out.reach = out.half_length + 10;
    out.side_q16 = k_one_q16;
    out.sharp_q16 = k_one_q16;
    out.lee = static_cast<i32>(
        max_i64(1, (static_cast<i64>(out.height) * k_cot_repose_q16) / 65'536'000));
    out.inv_half = reciprocal_q32(out.half_length);
    out.inv_stoss = reciprocal_q32(out.stoss);
    out.inv_width = reciprocal_q32(out.bend);
    return true;
  }
  out.kind = static_cast<u8>(PrimitiveKind::transverse);
  const u32 along = static_cast<u32>(wind_.prevailing_turn()) + k_quarter_turn +
                    static_cast<u32>(draw(sub(h, 4), -p.spread_turn, p.spread_turn));
  out.ax = cos_q15(along) / 2;
  out.az = sin_q15(along) / 2;
  const i64 half_q16 = draw(sub(h, 5), p.half_length_lo_q16, p.half_length_hi_q16);
  out.half_length = static_cast<i32>(((cell * half_q16) >> 16) * 10);
  out.stoss = static_cast<i32>(((cell * p.stoss_q16) >> 16) * 10);
  out.bend = static_cast<i32>(((cell * draw(sub(h, 6), -p.bend_q16, p.bend_q16)) >> 16) * 10);
  out.lee =
      static_cast<i32>(max_i64(1, (static_cast<i64>(out.height) * k_cot_repose_q16) / 65'536'000));
  out.reach = static_cast<i32>(out.half_length + out.stoss + 3 * out.lee + abs_i64(out.bend) + 10);
  out.inv_half = reciprocal_q32(out.half_length);
  out.inv_stoss = reciprocal_q32(out.stoss);
  out.inv_width = reciprocal_q32(out.lee);
  // The slip face's side and sharpness from the recent wind across the crest (its normal is
  // (az, -ax), which points down the prevailing wind).
  const i64 nx = out.az;
  const i64 nz = -out.ax;
  const i64 across120 = (shape.r120x * nx + shape.r120z * nz) >> 14;
  const i64 across30 = (shape.r30x * nx + shape.r30z * nz) >> 14;
  out.side_q16 =
      static_cast<i32>(clamp_i64((across120 * 65536) / shape.sharp_ref, -k_one_q16, k_one_q16));
  out.sharp_q16 =
      static_cast<i32>(clamp_i64((abs_i64(across30) * 65536) / shape.sharp_ref, 0, k_one_q16));
  return true;
}

bool DuneField::primitive(Band band, i64 cell_i, i64 cell_j, i64 time_us,
                          Primitive& out) const noexcept {
  return make_primitive(static_cast<u32>(band), cell_i, cell_j, time_shape(time_us), out);
}

void DuneField::gather(i64 x0, i64 z0, i64 x1, i64 z1, i64 time_us, const LagField* lag,
                       Gather& out) const {
  out.time_us = time_us;
  out.lag = lag;
  out.primitives.clear();
  const TimeShape shape = time_shape(time_us);
  const i64 lag_max = max_lag_mm(lag);
  for (u32 b = 0; b < k_bands; ++b) {
    displacement(static_cast<Band>(b), time_us, out.dx[b], out.dz[b]);
    out.band_begin[b] = out.primitives.size();
    const i64 margin = reach_[b] + lag_max;
    const i64 cell = cell_[b];
    const i64 i0 = floor_div(x0 - out.dx[b] - margin, cell);
    const i64 i1 = floor_div(x1 - out.dx[b] + margin, cell);
    const i64 j0 = floor_div(z0 - out.dz[b] - margin, cell);
    const i64 j1 = floor_div(z1 - out.dz[b] + margin, cell);
    for (i64 j = j0; j <= j1; ++j) {
      for (i64 i = i0; i <= i1; ++i) {
        Primitive prim;
        if (make_primitive(b, i, j, shape, prim)) out.primitives.push_back(prim);
      }
    }
  }
  out.band_begin[k_bands] = out.primitives.size();

  // The day's ripples and yesterday's, blended over the first two hours of a day.
  const i64 day = day_of(time_us);
  const i64 into_s = (time_us - day * k_us_per_day) / k_us_per_second;
  const WindDay& today = wind_.day(day);
  const WindDay& yesterday = wind_.day(day - 1);
  out.ripple_x = cos_q15(today.turn) / 2;
  out.ripple_z = sin_q15(today.turn) / 2;
  out.ripple_prev_x = cos_q15(yesterday.turn) / 2;
  out.ripple_prev_z = sin_q15(yesterday.turn) / 2;
  out.ripple_shift = (into_s * today.speed_q16 * k_ripple_speed_um_per_s) / 65'536'000;
  out.ripple_prev_shift = (86'400 * yesterday.speed_q16 * k_ripple_speed_um_per_s) / 65'536'000;
  const auto amp = [](i32 speed) {
    return (k_ripple_um * (32768 + min_i64(speed, k_one_q16) / 2)) >> 16;
  };
  out.ripple_amp = static_cast<i32>(amp(today.speed_q16));
  out.ripple_prev_amp = static_cast<i32>(amp(yesterday.speed_q16));
  out.ripple_blend_q16 = static_cast<i32>(
      min_i64(k_one_q16, ((time_us - day * k_us_per_day) * 65536) / k_ripple_realign_us));
}

i64 DuneField::band_sand(const Gather& gather, u32 band, i64 qx, i64 qz) const noexcept {
  // Within a band the primitives meet by their maximum, never their sum: two crests of one band
  // that overlap make a junction, not a dune twice as tall. A smooth maximum was the first draft
  // and is wrong here: it adds a bump wherever it blends against a primitive's zero, so the surface
  // jumped where a primitive's reach began, and it made the result depend on the primitives' order.
  // The maximum is continuous, and exactly the same whatever order a gather holds them in.
  i64 sand = 0;
  for (u32 k = gather.band_begin[band]; k < gather.band_begin[band + 1]; ++k) {
    const Primitive& p = gather.primitives[k];
    const i64 dx = qx - p.cx;
    const i64 dz = qz - p.cz;
    if (abs_i64(dx) > p.reach || abs_i64(dz) > p.reach) continue;
    if (dx * dx + dz * dz > static_cast<i64>(p.reach) * p.reach) continue;
    i64 h = 0;
    if (p.kind == static_cast<u8>(PrimitiveKind::transverse)) {
      const i64 a = (dx * p.ax + dz * p.az) >> 14;
      const i64 u0 = (dx * p.az - dz * p.ax) >> 14;
      const i64 abs_a = abs_i64(a);
      if (abs_a >= p.half_length) continue;
      const i64 sa = (abs_a * p.inv_half) >> 16;
      // Full height over the middle 60% of the crest, falling smoothly to nothing at its ends.
      const i64 taper = sa <= 39322 ? k_one_q16 : falloff_q16(((sa - 39322) * 5) / 2);
      // The ends lie `bend` downwind of the middle, on whichever side the slip face is.
      const i64 bend = (p.bend * ((sa * sa) >> 16)) >> 16;
      // Beyond the stoss on one side and the rounded lee on the other, neither profile has height.
      const i64 across = abs_i64(u0) - abs_i64(bend);
      if (across >= p.stoss && across >= 3 * p.lee) continue;
      const i64 plus = cross_profile(u0 - bend, p);
      const i64 minus = cross_profile(-u0 - bend, p);
      const i64 w_plus = (k_one_q16 + p.side_q16) / 2;
      const i64 profile = (plus * w_plus + minus * (k_one_q16 - w_plus)) >> 16;
      h = (((static_cast<i64>(p.height) * taper) >> 16) * profile) >> 16;
      sand = max_i64(sand, h);
    } else {
      const i64 x = (dx * p.ax + dz * p.az) >> 14;
      const i64 y = (dz * p.ax - dx * p.az) >> 14;
      const i64 xq = (x * (x < 0 ? p.inv_stoss : p.inv_half)) >> 16;
      const i64 yq = (y * p.inv_width) >> 16;
      const i64 rho2 = (xq * xq + yq * yq) >> 16;
      if (rho2 >= k_one_q16) continue;
      const i64 rho = static_cast<i64>(isqrt(static_cast<u64>(rho2) << 16));
      const i64 dome = (static_cast<i64>(p.height) * falloff_q16(rho)) >> 16;
      const i64 h_mm = p.height / 1000;
      const i64 scoop_r = (h_mm * 7) / 2;
      const i64 scoop_x = scoop_r + (h_mm * 3) / 10;
      const i64 lee = p.lee;
      const i64 d = length(x - scoop_x, y);
      const i64 face = max_i64(0, ((d - (scoop_r - lee)) * k_tan_repose_q16 * 1000) >> 16);
      h = min_i64(dome, face);
      sand = max_i64(sand, h);
    }
  }
  return sand;
}

void DuneField::features(i64 x, i64 z, i64& ridge, i64& flatten, i64& lag) const noexcept {
  ridge = 0;
  lag = 0;
  flatten = k_one_q16;
  for (const RidgeFeature& r : desc_.ridges) {
    const i64 w = max_i64(1, r.width);
    const i64 d = segment_distance(x, z, r, 2 * w);
    if (d < w) {
      // 1/2 + cos(pi d / w)/2, the renderer's cosine profile, from the integer cosine.
      const i64 profile = k_one_q15 + cos_q15(static_cast<u32>((d * 32768) / w));
      ridge = max_i64(ridge, profile);
    }
    if (d < 2 * w) {
      const i64 held =
          (((w * desc_.ridge_lag_q16) >> 16) * falloff_q16((d * 65536) / (2 * w))) >> 16;
      lag = max_i64(lag, held);
    }
  }
  for (const BasinFeature& b : desc_.basins) {
    const i64 radius = max_i64(1, b.radius);
    const i64 dx = x - b.x;
    const i64 dz = z - b.z;
    if (abs_i64(dx) >= radius || abs_i64(dz) >= radius) continue;
    const i64 r = (length(dx, dz) * 65536) / radius;
    if (r >= k_one_q16) continue;
    // smoothstep(0.35, 1, r): the dunes die away towards the basin's middle.
    const i64 t = ((r - 22938) * 65536) / (k_one_q16 - 22938);
    flatten = min_i64(flatten, k_one_q16 - falloff_q16(t));
  }
}

i32 DuneField::ridge_q16(i64 x, i64 z) const noexcept {
  i64 ridge = 0, flatten = 0, lag = 0;
  features(x, z, ridge, flatten, lag);
  return static_cast<i32>(ridge);
}

i32 DuneField::basin_q16(i64 x, i64 z) const noexcept {
  i64 best = 0;
  for (const BasinFeature& b : desc_.basins) {
    const i64 radius = max_i64(1, b.radius);
    const i64 dx = x - b.x;
    const i64 dz = z - b.z;
    if (abs_i64(dx) >= radius || abs_i64(dz) >= radius) continue;
    const i64 r = (length(dx, dz) * 65536) / radius;
    if (r < k_one_q16) best = max_i64(best, k_one_q16 - r);
  }
  return static_cast<i32>(best);
}

i64 DuneField::ridge_lag_mm(i64 x, i64 z) const noexcept {
  i64 ridge = 0, flatten = 0, lag = 0;
  features(x, z, ridge, flatten, lag);
  return lag;
}

i64 DuneField::floor_um(i64 x, i64 z) const noexcept {
  const i64 proj = floor_mod((x * roll_x_ + z * roll_z_) >> 14, roll_length_);
  const u32 phase = static_cast<u32>((proj * 65536) / roll_length_) + roll_phase_;
  const i64 amplitude = (desc_.dune_height * 1000 * desc_.roll_q16) >> 16;
  return (amplitude * sin_q15(phase)) >> 15;
}

i64 DuneField::detail_um(const Gather& g, i64 x, i64 z) const noexcept {
  const auto ripple = [&](i32 dx, i32 dz, i64 shift, i32 amp) {
    const i64 proj = floor_mod(((x * dx + z * dz) >> 14) - shift, k_ripple_mm);
    const i64 s = (proj * 65536) / k_ripple_mm;
    // A ripple's gentle windward three quarters and its steep lee quarter.
    const i64 tri = s < 49152 ? (s * 65536) / 49152 : ((k_one_q16 - s) * 65536) / 16384;
    return (amp * (tri - 32768)) >> 16;
  };
  const i64 today = ripple(g.ripple_x, g.ripple_z, g.ripple_shift, g.ripple_amp);
  i64 ripples = today;
  if (g.ripple_blend_q16 < k_one_q16) {
    const i64 before =
        ripple(g.ripple_prev_x, g.ripple_prev_z, g.ripple_prev_shift, g.ripple_prev_amp);
    ripples = (today * g.ripple_blend_q16 + before * (k_one_q16 - g.ripple_blend_q16)) >> 16;
  }
  // Grain: a value noise on a 4 cm lattice, bilinear between its points.
  const i64 gi = floor_div(x, k_grain_mm);
  const i64 gj = floor_div(z, k_grain_mm);
  const i64 tx = ((x - gi * k_grain_mm) * 65536) / k_grain_mm;
  const i64 tz = ((z - gj * k_grain_mm) * 65536) / k_grain_mm;
  const i64 a = lattice_value(desc_.seed, gi, gj);
  const i64 b = lattice_value(desc_.seed, gi + 1, gj);
  const i64 c = lattice_value(desc_.seed, gi, gj + 1);
  const i64 d = lattice_value(desc_.seed, gi + 1, gj + 1);
  const i64 top = a + (((b - a) * tx) >> 16);
  const i64 bottom = c + (((d - c) * tx) >> 16);
  return ripples + top + (((bottom - top) * tz) >> 16);
}

Sample DuneField::sample(const Gather& gather, i64 x, i64 z, Detail detail) const noexcept {
  Sample s;
  s.floor = floor_um(x, z);
  i64 ridge = 0, flatten = 0, ridge_lag = 0;
  features(x, z, ridge, flatten, ridge_lag);
  s.ridge_q16 = static_cast<u16>(min_i64(ridge, 65535));
  s.basin_q16 = static_cast<u16>(min_i64(basin_q16(x, z), 65535));
  if (detail == Detail::floor) return s;
  const u32 bands = detail == Detail::coarse ? 2u : k_bands;
  const i64 wx = wind_.prevailing_x_q14();
  const i64 wz = wind_.prevailing_z_q14();
  i64 sand = 0;
  for (u32 b = 0; b < bands; ++b) {
    const i64 lag = ridge_lag + (gather.lag != nullptr ? gather.lag->lag_mm(b, x, z) : 0);
    const i64 qx = x - gather.dx[b] + ((wx * lag) >> 14);
    const i64 qz = z - gather.dz[b] + ((wz * lag) >> 14);
    sand += band_sand(gather, b, qx, qz);
  }
  const i64 mask = (((k_one_q16 - ((k_ridge_thinning_q16 * ridge) >> 16)) * flatten) >> 16);
  s.sand = (sand * mask) >> 16;
  if (detail == Detail::full) s.detail = (detail_um(gather, x, z) * mask) >> 16;
  return s;
}

i64 DuneField::height_um(const Gather& gather, i64 x, i64 z, Detail detail) const noexcept {
  const Sample s = sample(gather, x, z, detail);
  return s.floor + s.sand + s.detail;
}

i64 DuneField::height_um(i64 x, i64 z, i64 time_us, Detail detail, const LagField* lag) const {
  Gather g;
  gather(x, z, x, z, time_us, lag, g);
  return height_um(g, x, z, detail);
}

f32 DuneField::ground_height(const void* context, f32 x, f32 z) noexcept {
  const auto* field = static_cast<const DuneField*>(context);
  return height_m(field->floor_um(to_mm(x), to_mm(z)));
}

}  // namespace engine::terrain

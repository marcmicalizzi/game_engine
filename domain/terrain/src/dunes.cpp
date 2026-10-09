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
// The fixed features' cap on the sand (terrain.md, "The repose limiter"): the slope a feature's
// mask may add to the sand under it, 0.04 of a tangent (the gap between 34 and 36 degrees is
// 0.052), the cap's own rise beyond, 30 degrees (tan, Q16), and "no cap".
constexpr i64 k_cap_slack_q16 = 2621;
constexpr i64 k_tan_cap_q16 = 37837;
constexpr i64 k_no_cap = i64{1} << 60;
// Ripples: 12 cm apart, 6 mm high at the mean wind, drifting 1 cm a minute at it, and realigned to
// a new day's wind over two hours.
constexpr i64 k_ripple_mm = 120;
constexpr i64 k_ripple_um = 6'000;
constexpr i64 k_ripple_speed_um_per_s = 167;
constexpr i64 k_ripple_realign_us = 2 * 3600 * k_us_per_second;
constexpr i64 k_grain_mm = 40;
constexpr i64 k_grain_um = 1'500;
// Up to this many primitives of a band in a gather, a point tests them all; past it, only the
// cells whose primitives can reach it (`Gather::grid`). Either way the same maximum, bit for bit.
constexpr u32 k_linear_primitives = 24;

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

// 1 / sqrt(1 + slope^2), Q16: what a distance across a crest line whose position drifts sideways
// at `slope_q16` per unit along it is shortened by to be measured across the local crest.
//
// A table of 257 exact values over slopes 0 to 4, interpolated linearly: the bit-by-bit square
// root this replaced cost a tile a quarter of its time, twice per crest a point visits. The
// interpolation is within one Q16 unit of the exact value (the curve's second derivative is at most
// one, and the step 1/64) and continuous; steeper drifts, which no band table draws, take the
// square root.
constexpr u64 isqrt_constexpr(u64 v) {
  u64 result = 0;
  u64 bit = u64{1} << 62;
  while (bit > v)
    bit >>= 2;
  while (bit != 0) {
    if (v >= result + bit) {
      v -= result + bit;
      result = (result >> 1) + bit;
    } else {
      result >>= 1;
    }
    bit >>= 2;
  }
  return result;
}

constexpr u32 k_across_steps = 256;
constexpr i64 k_across_step_q16 = 1024;  // slope 1/64
struct AcrossTable {
  i32 v[k_across_steps + 1] = {};
  constexpr AcrossTable() {
    for (u32 i = 0; i <= k_across_steps; ++i) {
      const u64 m = u64{i} * k_across_step_q16;
      const u64 len = isqrt_constexpr(u64{65536} * 65536 + m * m);
      v[i] = static_cast<i32>((u64{65536} * 65536) / len);
    }
  }
};
constexpr AcrossTable k_across_table;

i64 across_scale_q16(i64 slope_q16) noexcept {
  const i64 m = abs_i64(slope_q16);
  if (m == 0) return k_one_q16;
  const i64 i = m / k_across_step_q16;
  if (i >= k_across_steps) {
    const u64 len = isqrt(static_cast<u64>(k_one_q16) * k_one_q16 + static_cast<u64>(m * m));
    return static_cast<i64>((static_cast<u64>(k_one_q16) * k_one_q16) / len);
  }
  const i64 f = m - i * k_across_step_q16;
  const i64 a = k_across_table.v[i];
  const i64 b = k_across_table.v[i + 1];
  return a + ((b - a) * f) / k_across_step_q16;
}

// The most a band's crest line can drift sideways per unit along it, Q16: its meander's (2 pi
// sinuosity) and its bend's (2 bend / half length at the crest's end, with the widest bend over the
// shortest crest).
i64 drift_bound_q16(const BandDesc& p) noexcept {
  const i64 meander = (static_cast<i64>(p.sinuosity_q16) * 411775) >> 16;
  const i64 bend =
      (2 * static_cast<i64>(p.bend_q16) * k_one_q16) / max_i64(1, p.half_length_lo_q16);
  return meander + bend;
}

u32 reciprocal_q32(i64 v) noexcept { return static_cast<u32>((i64{1} << 32) / max_i64(2, v)); }

i64 lattice_value(u64 seed, i64 i, i64 j) noexcept {
  const u64 h = hash_combine(hash_combine(hash_combine(seed, k_tag_grain), static_cast<u64>(i)),
                             static_cast<u64>(j));
  return draw(h, -k_grain_um, k_grain_um);
}

}  // namespace

const char* primitive_kind_name(PrimitiveKind kind) noexcept {
  return kind == PrimitiveKind::barchan ? "barchan" : "transverse";
}

const char* band_couple_name(BandCouple couple) noexcept {
  switch (couple) {
    case BandCouple::none: return "none";
    case BandCouple::flanks: return "flanks";
    case BandCouple::floors: return "floors";
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
  // Storms enter only when there are some, and the band table only when there is one, so every
  // field described before either existed keeps its hash (and its golden).
  if (w.storms_per_year > 0) {
    h = hash_combine(h, 0x53544F524Dull);  // "STORM"
    for (const i32 v : {w.storms_per_year, w.storm_hours_min, w.storm_hours_max,
                        w.storm_spread_turn, w.storm_speed_q16})
      h = hash_combine(h, static_cast<u64>(static_cast<u32>(v)));
  }
  // The day's profile and the storms' transport gains, likewise only when a record has them.
  if (w.diurnal_q16 != 0 || w.veer_turn != 0) {
    h = hash_combine(h, 0x444955524E414Cull);  // "DIURNAL"
    for (const i32 v : {w.diurnal_q16, static_cast<i32>(w.diurnal_peak_turn), w.veer_turn,
                        static_cast<i32>(w.veer_phase_turn)})
      h = hash_combine(h, static_cast<u64>(static_cast<u32>(v)));
  }
  if (w.storm_gain_q16 != k_one_q16 || !w.storm_gains.empty()) {
    h = hash_combine(h, 0x4741494Eull);  // "GAIN"
    h = hash_combine(h, static_cast<u64>(static_cast<u32>(w.storm_gain_q16)));
    h = hash_combine(h, w.storm_gains.size());
    for (const WindParams::StormGain& g : w.storm_gains) {
      h = hash_combine(h, static_cast<u64>(static_cast<u32>(g.storm)));
      h = hash_combine(h, static_cast<u64>(static_cast<u32>(g.gain_q16)));
    }
  }
  if (!desc.bands.empty()) {
    h = hash_combine(h, 0x42414E44ull);  // "BAND"
    h = hash_combine(h, desc.bands.size());
    for (const BandDesc& b : desc.bands) {
      const u64 words[] = {static_cast<u64>(b.kind),
                           static_cast<u64>(b.cell_cm),
                           static_cast<u64>(b.height_lo_cm),
                           static_cast<u64>(b.height_hi_cm),
                           static_cast<u64>(b.presence_q16),
                           static_cast<u64>(b.half_length_lo_q16),
                           static_cast<u64>(b.half_length_hi_q16),
                           static_cast<u64>(b.stoss_q16),
                           static_cast<u64>(b.bend_q16),
                           static_cast<u64>(b.spread_turn),
                           static_cast<u64>(b.sinuosity_q16),
                           static_cast<u64>(b.sharp_cap_q16),
                           static_cast<u64>(b.side_days),
                           static_cast<u64>(b.sharp_days),
                           static_cast<u64>(b.couple),
                           static_cast<u64>(b.couple_mm),
                           b.far ? 1u : 0u};
      for (const u64 word : words)
        h = hash_combine(h, word);
    }
    // A stylized band's travel enters only when some band has one, so every table before it keeps
    // its hash.
    bool styled = false;
    for (const BandDesc& b : desc.bands)
      styled = styled || b.celerity_q16 != k_one_q16;
    if (styled) {
      h = hash_combine(h, 0x43454C4552ull);  // "CELER"
      for (const BandDesc& b : desc.bands)
        h = hash_combine(h, static_cast<u64>(static_cast<u32>(b.celerity_q16)));
    }
  }
  return h;
}

namespace {

void set_name(BandDesc& band, const char* name) noexcept {
  u32 i = 0;
  for (; name[i] != 0 && i + 1 < sizeof(band.name); ++i)
    band.name[i] = name[i];
  band.name[i] = 0;
}

// The widest a transverse crest of this band is across (its stoss and its rounded lee), and a
// barchan's width, cm: the least a cell may be.
i64 band_extent_cm(const BandDesc& b) noexcept {
  if (b.kind == PrimitiveKind::barchan) return b.height_hi_cm * 11;
  const i64 lee_cm = (b.height_hi_cm * k_cot_repose_q16) >> 16;
  return ((b.cell_cm * b.stoss_q16) >> 16) + 3 * lee_cm;
}

}  // namespace

Vector<BandDesc> default_bands(i64 dune_height_mm, i64 wavelength_mm) {
  const i64 h0_cm = max_i64(1, dune_height_mm / 10);
  const i64 lambda_cm = max_i64(100, wavelength_mm / 10);
  Vector<BandDesc> bands;
  bands.resize(3);
  // Draa: long ridges two wavelengths apart, the dune height tall.
  BandDesc& draa = bands[0];
  draa.cell_cm = 2 * lambda_cm;
  draa.height_lo_cm = (h0_cm * 3) / 4;
  draa.height_hi_cm = (h0_cm * 5) / 4;
  draa.presence_q16 = k_one_q16;
  draa.half_length_lo_q16 = 36045;
  draa.half_length_hi_q16 = 62259;
  draa.stoss_q16 = 27525;
  draa.bend_q16 = 11796;
  draa.spread_turn = 4551;
  set_name(draa, "draa");
  // Crest segments at the wavelength, about half as tall, riding on them.
  BandDesc& crest = bands[1];
  crest.cell_cm = lambda_cm;
  crest.height_lo_cm = (h0_cm * 3) / 10;
  crest.height_hi_cm = (h0_cm * 6) / 10;
  crest.presence_q16 = 55706;
  crest.half_length_lo_q16 = 29491;
  crest.half_length_hi_q16 = 55706;
  crest.stoss_q16 = 26214;
  crest.bend_q16 = 16384;
  crest.spread_turn = 3641;
  set_name(crest, "crest");
  // Barchans on the flats, sparse; dropped by the coarse detail.
  BandDesc& barchan = bands[2];
  barchan.kind = PrimitiveKind::barchan;
  barchan.cell_cm = (lambda_cm * 7) / 10;
  barchan.height_lo_cm = h0_cm / 5;
  barchan.height_hi_cm = (h0_cm * 9) / 20;
  barchan.presence_q16 = 26214;
  barchan.half_length_lo_q16 = 0;
  barchan.half_length_hi_q16 = 0;
  barchan.stoss_q16 = 0;
  barchan.bend_q16 = 0;
  barchan.spread_turn = 0;
  barchan.far = false;
  set_name(barchan, "barchan");
  return bands;
}

BandDesc band_from_metres(const BandMetres& m) noexcept {
  const auto cm = [](f32 v) {
    return static_cast<i64>(std::floor(static_cast<f64>(v) * 100.0 + 0.5));
  };
  const auto q16 = [](f32 v) {
    return static_cast<i32>(
        clamp_i64(static_cast<i64>(std::floor(static_cast<f64>(v) * 65536.0 + 0.5)),
                  -(i64{1} << 30), i64{1} << 30));
  };
  BandDesc b;
  b.kind = m.kind;
  b.cell_cm = cm(m.cell);
  b.height_lo_cm = cm(m.height_min);
  b.height_hi_cm = cm(m.height_max);
  b.presence_q16 = q16(m.share);
  b.half_length_lo_q16 = q16(m.length_min);
  b.half_length_hi_q16 = q16(m.length_max);
  b.stoss_q16 = q16(m.stoss);
  b.bend_q16 = q16(m.bend);
  b.sinuosity_q16 = q16(m.sinuosity);
  b.spread_turn = static_cast<i32>(clamp_i64(
      static_cast<i64>(std::floor(static_cast<f64>(m.spread_deg) * 65536.0 / 360.0 + 0.5)), 0,
      16384));
  b.sharp_cap_q16 = q16(m.sharpness);
  b.side_days = static_cast<i32>(std::min<u32>(m.side_days, 1u << 20));
  b.sharp_days = static_cast<i32>(std::min<u32>(m.sharp_days, 1u << 20));
  b.couple = m.couple;
  b.couple_mm = static_cast<i64>(std::floor(static_cast<f64>(m.couple_width) * 1000.0 + 0.5));
  b.far = m.far;
  b.celerity_q16 = static_cast<i32>(
      clamp_i64(static_cast<i64>(std::floor(static_cast<f64>(m.celerity_scale) * 65536.0 + 0.5)), 0,
                i64{1000} * 65536));
  set_name(b, m.name.c_str());
  return b;
}

bool validate_bands(std::span<const BandDesc> bands, std::string* error) {
  const auto fail = [&](const std::string& why) {
    if (error != nullptr) *error = why;
    return false;
  };
  if (bands.empty()) return fail("the band table is empty: leave it out for the default");
  if (bands.size() > k_max_bands)
    return fail("the band table has more than " + std::to_string(k_max_bands) + " bands");
  for (usize i = 0; i < bands.size(); ++i) {
    const BandDesc& b = bands[i];
    const std::string where = "band " + std::to_string(i) + " (" + b.name + ")";
    if (b.presence_q16 <= 0)
      return fail(where + " occupies no cells: a band with a zero share is not a band");
    if (b.presence_q16 > k_one_q16) return fail(where + " occupies more than every cell");
    if (b.height_lo_cm <= 0 || b.height_hi_cm < b.height_lo_cm)
      return fail(where + ": heights must be positive, the least first");
    if (b.height_hi_cm > 40'000) return fail(where + ": taller than 400 m");
    if (b.cell_cm > 2'000'000) return fail(where + ": a cell larger than 20 km");
    if (b.cell_cm < band_extent_cm(b)) {
      return fail(where + ": its cell (" + std::to_string(b.cell_cm) +
                  " cm) is smaller than its own dune (" + std::to_string(band_extent_cm(b)) +
                  " cm across)");
    }
    if (b.kind == PrimitiveKind::transverse &&
        (b.half_length_lo_q16 <= 0 || b.half_length_hi_q16 < b.half_length_lo_q16 ||
         b.half_length_hi_q16 > 2 * k_one_q16 || b.stoss_q16 <= 0 || b.stoss_q16 > k_one_q16 ||
         b.bend_q16 < 0 || b.bend_q16 > k_one_q16 / 2 || b.sinuosity_q16 < 0 ||
         b.sinuosity_q16 > k_one_q16 / 4)) {
      return fail(where + ": a crest's length, stoss, bend or sinuosity is out of range");
    }
    if (b.sharp_cap_q16 < 0 || b.sharp_cap_q16 > k_one_q16)
      return fail(where + ": sharpness must be within [0, 1]");
    if (b.side_days < 1 || b.side_days > 3650 || b.sharp_days < 1 || b.sharp_days > 3650)
      return fail(where + ": the slip face's windows must be 1 to 3650 days");
    if (b.celerity_q16 <= 0 || b.celerity_q16 > 1000 * k_one_q16)
      return fail(where + ": celerity_scale must be within (0, 1000]");
    if (b.couple != BandCouple::none && (i == 0 || b.couple_mm <= 0))
      return fail(where + ": couples to the bands before it, and needs some and a width");
    if (i > 0 && b.height_hi_cm > bands[i - 1].height_hi_cm)
      return fail(where + " is taller than the band before it: order the table tallest first");
  }
  return true;
}

DuneField::DuneField(const FieldDesc& desc)
    : desc_(desc), wind_(desc.wind), hash_(field_hash(desc)) {
  bands_ = desc.bands.empty() ? default_bands(desc.dune_height, desc.wavelength) : desc.bands;
  if (bands_.size() > k_max_bands) bands_.resize(k_max_bands);
  for (u32 b = 0; b < bands_.size(); ++b) {
    // The lee zone's fade: six times the tallest later band's height over the tangent of the
    // angle of repose, so a band fading out across it loses its height at a sixth of the repose
    // slope or less (terrain.md, "The repose limiter"). The last band absorbs nothing.
    i64 later_cm = 0;
    for (u32 c = b + 1; c < bands_.size(); ++c)
      later_cm = max_i64(later_cm, bands_[c].height_hi_cm);
    absorb_[b] = later_cm > 0 ? (later_cm * 60 * k_cot_repose_q16) >> 16 : 0;
  }
  // How far a feature's cap reaches before it stands above the tallest sand the bands can stack.
  i64 stack_um = 0;
  for (const BandDesc& p : bands_)
    stack_um += p.height_hi_cm * 10'000;
  cap_reach_ = (stack_um * 65536) / (k_tan_cap_q16 * 1000) + 1;
  for (u32 b = 0; b < bands_.size(); ++b) {
    const BandDesc& p = bands_[b];
    cell_[b] = p.cell_cm * 10;
    // Bagnold's height, the middle of the band's range, mm — over a stylized band's scale, which
    // is the same thing as its travel times the scale and keeps the displacement one division.
    const i64 bagnold = max_i64(10, (p.height_lo_cm + p.height_hi_cm) * 5);
    celerity_height_[b] = p.celerity_q16 == k_one_q16
                              ? bagnold
                              : max_i64(1, (bagnold * k_one_q16) / max_i64(1, p.celerity_q16));
    const i64 h_hi_um = p.height_hi_cm * 10'000;
    if (p.kind == PrimitiveKind::barchan) {
      reach_[b] = (h_hi_um * 11) / 2000 + 10 + absorb_[b];
    } else {
      const i64 lee_smooth = 3 * ((h_hi_um * k_cot_repose_q16) / 65'536'000) + 1;
      const i64 cell = cell_[b];
      drift_q16_[b] = drift_bound_q16(p);
      const i64 across = max_i64((cell * p.stoss_q16) >> 16, lee_smooth + absorb_[b]);
      reach_[b] = ((cell * p.half_length_hi_q16) >> 16) +
                  (across * k_one_q16) / across_scale_q16(drift_q16_[b]) +
                  ((cell * p.bend_q16) >> 16) + ((cell * p.sinuosity_q16) >> 16) + 10;
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
  // Each band's mean height (`band_mean_um`), what a lattice too coarse to carry it reads: the
  // maximum of its primitives over 32 x 32 points a quarter of a cell apart, eight cells either way
  // of the origin, at time 0. A band's primitives are a function of their cell, so any eight cells
  // are as good as these; sixty-four of them put the mean within a few percent of the band's. The
  // gather is filtered to the band's own cell, so it holds no band finer than this one: the
  // mega-draa's 19 km would otherwise gather millions of waves.
  for (u32 b = 0; b < bands_.size(); ++b) {
    const i64 cell = cell_[b];
    Gather g;
    gather(-4 * cell, -4 * cell, 4 * cell, 4 * cell, 0, nullptr, cell / k_carried_cells, g);
    i64 sum = 0;
    constexpr i64 k_side = 32;
    for (i64 j = 0; j < k_side; ++j) {
      for (i64 i = 0; i < k_side; ++i) {
        const i64 x = -4 * cell + (i * 8 * cell) / k_side + cell / 8;
        const i64 z = -4 * cell + (j * 8 * cell) / k_side + cell / 8;
        sum += band_value<false>(g, b, x - g.dx[b], z - g.dz[b]).height;
      }
    }
    mean_um_[b] = sum / (k_side * k_side);
  }
}

void DuneField::displacement(u32 band, i64 time_us, i64& dx, i64& dz) const noexcept {
  // Bagnold: a dune's celerity is the sand flux over its height, so a band moves by the flux
  // integral over its height — cm^2 over mm, times 100 for mm^2.
  const FluxIntegral in = wind_.integral(time_us);
  const i64 h = celerity_height_[band];
  dx = floor_div(in.x * 100, h);
  dz = floor_div(in.z * 100, h);
}

i64 DuneField::max_lag_mm(const LagField* lag) const noexcept {
  return max_ridge_lag_ + (lag != nullptr ? lag->max_lag_mm() : 0);
}

DuneField::TimeShape DuneField::time_shape(u32 band, i64 time_us) const noexcept {
  TimeShape s;
  const BandDesc& b = bands_[band];
  const FluxIntegral r120 = wind_.between(time_us - b.side_days * k_us_per_day, time_us);
  const FluxIntegral r30 = wind_.between(time_us - b.sharp_days * k_us_per_day, time_us);
  s.r120x = r120.x;
  s.r120z = r120.z;
  s.r30x = r30.x;
  s.r30z = r30.z;
  // Three quarters of a month of the mean flux, blowing one way, is a slip face at the angle of
  // repose; the same against a crest's side, over four months, puts its slip face on the other
  // side.
  s.sharp_ref = max_i64(1, (b.sharp_days * static_cast<i64>(desc_.wind.flux_cm2_per_day) * 3) / 4);
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
  const BandDesc& p = bands_[band];
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
  if (p.kind == PrimitiveKind::barchan) {
    // A barchan: a dome 4 H upwind, 5.5 H downwind and 5.5 H either side, and a scoop 3.5 H in
    // radius whose rim is the brink, just downwind of the summit: its slip face falls at the angle
    // of repose inside the scoop, and the dome's flanks either side of it are the horns.
    out.kind = static_cast<u8>(PrimitiveKind::barchan);
    out.ax = shape.barchan_x;
    out.az = shape.barchan_z;
    out.half_length = static_cast<i32>((h_mm * 11) / 2);
    out.stoss = static_cast<i32>(h_mm * 4);
    out.bend = static_cast<i32>((h_mm * 11) / 2);
    out.absorb = static_cast<i32>(absorb_[band]);
    out.reach = out.half_length + 10 + out.absorb;
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
  out.absorb = static_cast<i32>(absorb_[band]);
  // The crest's ends fall over 0.4 of its half length, or over six times its height if that is
  // longer (at most 0.95 of it): a tall crest that ended over the same share would stand its ends
  // steeper than its slip face, and where a meander turns the crest across that slope the two add.
  // Six is what keeps their sum under 36 degrees at the erg's worst meander (terrain.md, "The
  // repose limiter", has the arithmetic).
  const i64 taper = (static_cast<i64>(out.height) / 1000 * 6 * 65536) / max_i64(1, out.half_length);
  out.taper_q16 = static_cast<u16>(min_i64(62259, max_i64(26214, taper)));
  out.inv_taper = reciprocal_q32(out.taper_q16);
  out.inv_half = reciprocal_q32(out.half_length);
  out.inv_stoss = reciprocal_q32(out.stoss);
  out.inv_width = reciprocal_q32(out.lee);
  if (p.sinuosity_q16 > 0) {
    // Draws of their own, so a band without sinuosity draws nothing more than it did.
    out.meander = static_cast<i32>(((cell * p.sinuosity_q16) >> 16) * 10);
    out.inv_meander = reciprocal_q32(cell * 10);
    out.meander_phase = static_cast<u16>(sub(h, 7) >> 48);
  }
  // Everything the crest can reach from its centre: along it, then across it — the stoss, or the
  // rounded lee and the lee zone's fade past it, widened by the most its line drifts — the bend,
  // and the meander.
  // Its own drift, not the band's most: the meander's 2 pi meander / cell and the bend's 2 bend /
  // half length at its ends.
  const i64 drift =
      ((static_cast<i64>(out.meander) * 411775 * static_cast<i64>(out.inv_meander)) >> 32) +
      ((2 * abs_i64(out.bend) * k_one_q16) / max_i64(1, out.half_length));
  out.widen = static_cast<i32>(
      (max_i64(out.stoss, 3 * static_cast<i64>(out.lee) + out.absorb) * k_one_q16) /
          across_scale_q16(drift) +
      1);
  out.reach =
      static_cast<i32>(out.half_length + out.widen + abs_i64(out.bend) + abs_i64(out.meander) + 10);
  // The slip face's side and sharpness from the recent wind across the crest (its normal is
  // (az, -ax), which points down the prevailing wind).
  const i64 nx = out.az;
  const i64 nz = -out.ax;
  const i64 across120 = (shape.r120x * nx + shape.r120z * nz) >> 14;
  const i64 across30 = (shape.r30x * nx + shape.r30z * nz) >> 14;
  out.side_q16 =
      static_cast<i32>(clamp_i64((across120 * 65536) / shape.sharp_ref, -k_one_q16, k_one_q16));
  out.sharp_q16 = static_cast<i32>(
      clamp_i64((abs_i64(across30) * 65536) / shape.sharp_ref, 0, p.sharp_cap_q16));
  return true;
}

bool DuneField::primitive(u32 band, i64 cell_i, i64 cell_j, i64 time_us,
                          Primitive& out) const noexcept {
  return make_primitive(band, cell_i, cell_j, time_shape(band, time_us), out);
}

void DuneField::gather(i64 x0, i64 z0, i64 x1, i64 z1, i64 time_us, const LagField* lag,
                       Gather& out) const {
  gather(x0, z0, x1, z1, time_us, lag, 0, out);
}

void DuneField::gather(i64 x0, i64 z0, i64 x1, i64 z1, i64 time_us, const LagField* lag,
                       i64 filter_mm, Gather& out) const {
  out.time_us = time_us;
  out.lag = lag;
  out.filter_mm = filter_mm > 0 ? filter_mm : 0;
  out.primitives.clear();
  out.grid.clear();
  const i64 lag_max = max_lag_mm(lag);
  const u32 bands = bands_.size();
  out.bands = bands;
  for (u32 b = 0; b < bands; ++b) {
    displacement(b, time_us, out.dx[b], out.dz[b]);
    out.band_begin[b] = out.primitives.size();
    if (!carries(b, out.filter_mm)) {
      // Read as its mean: nothing to gather, whatever the region.
      out.grid_i0[b] = 0;
      out.grid_j0[b] = 0;
      out.grid_ni[b] = 0;
      out.grid_nj[b] = 0;
      out.grid_begin[b] = out.grid.size();
      continue;
    }
    const TimeShape shape = time_shape(b, time_us);
    const i64 margin = reach_[b] + lag_max;
    const i64 cell = cell_[b];
    const i64 i0 = floor_div(x0 - out.dx[b] - margin, cell);
    const i64 i1 = floor_div(x1 - out.dx[b] + margin, cell);
    const i64 j0 = floor_div(z0 - out.dz[b] - margin, cell);
    const i64 j1 = floor_div(z1 - out.dz[b] + margin, cell);
    out.grid_i0[b] = i0;
    out.grid_j0[b] = j0;
    out.grid_ni[b] = static_cast<u32>(i1 - i0 + 1);
    out.grid_nj[b] = static_cast<u32>(j1 - j0 + 1);
    out.grid_begin[b] = out.grid.size();
    for (i64 j = j0; j <= j1; ++j) {
      for (i64 i = i0; i <= i1; ++i) {
        Primitive prim;
        if (make_primitive(b, i, j, shape, prim)) {
          out.grid.push_back(static_cast<i32>(out.primitives.size()));
          out.primitives.push_back(prim);
        } else {
          out.grid.push_back(-1);
        }
      }
    }
  }
  out.band_begin[bands] = out.primitives.size();

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
  // A whole day's seconds as an i64: as the literal 86'400 the first product was an `int`, and a
  // day whose wind beat 0.38 of the mean overflowed it (UB, fatal under linux-clang-asan; on every
  // other build a wrapped, wrong shift, so yesterday's ripples were not where the day left them).
  out.ripple_prev_shift =
      (k_us_per_day / k_us_per_second * yesterday.speed_q16 * k_ripple_speed_um_per_s) / 65'536'000;
  const auto amp = [](i32 speed) {
    return (k_ripple_um * (32768 + min_i64(speed, k_one_q16) / 2)) >> 16;
  };
  out.ripple_amp = static_cast<i32>(amp(today.speed_q16));
  out.ripple_prev_amp = static_cast<i32>(amp(yesterday.speed_q16));
  out.ripple_blend_q16 = static_cast<i32>(
      min_i64(k_one_q16, ((time_us - day * k_us_per_day) * 65536) / k_ripple_realign_us));
}

template <bool k_slope>
DuneField::Value DuneField::band_value(const Gather& gather, u32 band, i64 qx,
                                       i64 qz) const noexcept {
  // Within a band the primitives meet by their maximum, never their sum: two crests of one band
  // that overlap make a junction, not a dune twice as tall. A smooth maximum was the first draft
  // and is wrong here: it adds a bump wherever it blends against a primitive's zero, so the surface
  // jumped where a primitive's reach began, and it made the result depend on the primitives' order.
  // The maximum is continuous, and exactly the same whatever order a gather holds them in — which
  // is also why visiting only the cells that can reach the point (below) changes no bit. The lee
  // slope bound is the maximum of the primitives' bounds, for the same reasons: the slope of a
  // maximum is the slope of whichever primitive is the maximum there, which is at most its bound.
  Value out;
  const auto visit = [&](const Primitive& p) {
    const i64 dx = qx - p.cx;
    const i64 dz = qz - p.cz;
    if (abs_i64(dx) > p.reach || abs_i64(dz) > p.reach) return;
    if (dx * dx + dz * dz > static_cast<i64>(p.reach) * p.reach) return;
    const Value v = primitive_value<k_slope>(p, dx, dz);
    out.height = max_i64(out.height, v.height);
    out.slope = max_i64(out.slope, v.slope);
    out.inside = max_i64(out.inside, v.inside);
  };
  const u32 first = gather.band_begin[band];
  const u32 last = gather.band_begin[band + 1];
  if (last - first <= k_linear_primitives) {
    // A band with a handful of primitives in the gather (every band of the default field over a
    // tile): the list itself is cheaper than the cells round the point.
    for (u32 k = first; k < last; ++k)
      visit(gather.primitives[k]);
    return out;
  }
  const i64 cell = cell_[band];
  const i64 reach = reach_[band];
  const i64 gi0 = gather.grid_i0[band];
  const i64 gj0 = gather.grid_j0[band];
  const i64 ni = gather.grid_ni[band];
  const i64 nj = gather.grid_nj[band];
  const i64 i_lo = max_i64(gi0, floor_div(qx - reach, cell));
  const i64 i_hi = min_i64(gi0 + ni - 1, floor_div(qx + reach, cell));
  const i64 j_lo = max_i64(gj0, floor_div(qz - reach, cell));
  const i64 j_hi = min_i64(gj0 + nj - 1, floor_div(qz + reach, cell));
  for (i64 j = j_lo; j <= j_hi; ++j) {
    const i64 row = static_cast<i64>(gather.grid_begin[band]) + (j - gj0) * ni;
    for (i64 i = i_lo; i <= i_hi; ++i) {
      const i32 slot = gather.grid[static_cast<u32>(row + (i - gi0))];
      if (slot >= 0) visit(gather.primitives[static_cast<u32>(slot)]);
    }
  }
  return out;
}

namespace {

// The lee zone across a crest (terrain.md, "The repose limiter"): nothing upwind of the fade,
// rising to one at the brink over `absorb`, one over the slip face and the rounded lee (three lee
// widths), and falling back to nothing over `absorb` past it.
i64 lee_zone(i64 u, i64 lee, i64 absorb) noexcept {
  const i64 end = 3 * lee;
  if (absorb <= 0) return 0;  // the last band: nothing after it to scale
  if (u <= -absorb) return 0;
  if (u < 0) return k_one_q16 - falloff_q16(((u + absorb) * 65536) / absorb);
  if (u <= end) return k_one_q16;
  if (u >= end + absorb) return 0;
  return falloff_q16(((u - end) * 65536) / absorb);
}

// 6 s (1 - s), Q16: the slope of `falloff_q16` at s, per unit of s.
i64 falloff_slope_q16(i64 s) noexcept {
  if (s <= 0 || s >= k_one_q16) return 0;
  return (6 * s * (k_one_q16 - s)) >> 16;
}

// A bound on the slope of one side of a crest's profile at `u` across it, Q16 (a tangent), for a
// crest `h_mm` tall: its stoss's own slope upwind of the brink and the angle of repose over the
// lee zone, whichever is more. Continuous in u, which the slope of the profile itself is not (it
// jumps at the brink), because the bands after it are scaled by it (terrain.md, "The repose
// limiter").
i64 side_slope_q16(i64 u, i64 h_mm, const Primitive& p) noexcept {
  const i64 lee = (k_tan_repose_q16 * lee_zone(u, p.lee, p.absorb)) >> 16;
  if (u >= 0) return lee;
  const i64 v = (-u * p.inv_stoss) >> 16;
  const i64 stoss = (h_mm * falloff_slope_q16(v) * static_cast<i64>(p.inv_stoss)) >> 32;
  return max_i64(stoss, lee);
}

}  // namespace

template <bool k_slope>
DuneField::Value DuneField::primitive_value(const Primitive& p, i64 dx, i64 dz) noexcept {
  Value out;
  const i64 h_mm = p.height / 1000;
  if (p.kind == static_cast<u8>(PrimitiveKind::transverse)) {
    const i64 a = (dx * p.ax + dz * p.az) >> 14;
    i64 u0 = (dx * p.az - dz * p.ax) >> 14;
    const i64 abs_a = abs_i64(a);
    if (abs_a >= p.half_length) return out;
    // The crest line's sideways drift along it, Q16: the meander's and the bend's.
    i64 meander_slope = 0;
    if (p.meander != 0) {
      // The crest line meanders: one wave a cell along it, `meander` either side.
      const u32 angle = static_cast<u32>((a * p.inv_meander) >> 16) + p.meander_phase;
      u0 -= (static_cast<i64>(p.meander) * sin_q15(angle)) >> 15;
      const i64 swing = (static_cast<i64>(p.meander) * cos_q15(angle)) >> 15;    // mm
      meander_slope = (swing * 411775 * static_cast<i64>(p.inv_meander)) >> 32;  // 2 pi, Q16
    }
    const i64 sa = (abs_a * p.inv_half) >> 16;
    // Full height over the middle of the crest, falling smoothly to nothing at its ends over
    // `taper_q16` of the half length (0.4 unless the crest is tall; make_primitive).
    const i64 taper_start = k_one_q16 - p.taper_q16;
    const i64 taper_s = sa <= taper_start ? 0 : ((sa - taper_start) * p.inv_taper) >> 16;
    const i64 taper = falloff_q16(taper_s);
    // The ends lie `bend` downwind of the middle, on whichever side the slip face is.
    const i64 bend = (p.bend * ((sa * sa) >> 16)) >> 16;
    // Beyond the stoss on one side and the lee and its zone on the other, nothing.
    const i64 across = abs_i64(u0) - abs_i64(bend);
    if (across >= p.widen) return out;
    // Measured across the local crest, not across the crest's mean line: a crest that bends or
    // meanders would otherwise stand its slip face steeper than the angle of repose by the secant
    // of its drift (a meander of 0.08 of a cell, 37 degrees).
    const i64 bend_slope =
        ((2 * static_cast<i64>(p.bend) * sa * static_cast<i64>(p.inv_half)) >> 32) *
        (a < 0 ? -1 : 1);
    const i64 up = (((u0 - bend) * across_scale_q16(meander_slope + bend_slope)) >> 16);
    const i64 um = (((-u0 - bend) * across_scale_q16(meander_slope - bend_slope)) >> 16);
    const i64 plus = cross_profile(up, p);
    const i64 minus = cross_profile(um, p);
    const i64 w_plus = (k_one_q16 + p.side_q16) / 2;
    const i64 profile = (plus * w_plus + minus * (k_one_q16 - w_plus)) >> 16;
    out.height = (((static_cast<i64>(p.height) * taper) >> 16) * profile) >> 16;
    if constexpr (k_slope) {
      // Its slope bound: across the crest, the two sides' bounds as the profile blends them, times
      // the taper; along it, the taper's own slope under the profile. Continuous everywhere, since
      // each part is (the taper's slope is zero where it starts and where it ends).
      const i64 side = (side_slope_q16(up, h_mm, p) * w_plus +
                        side_slope_q16(um, h_mm, p) * (k_one_q16 - w_plus)) >>
                       16;
      const i64 along_mm = (((h_mm * falloff_slope_q16(taper_s)) >> 16) * profile) >> 16;
      const i64 along =
          (((along_mm * static_cast<i64>(p.inv_taper)) >> 16) * static_cast<i64>(p.inv_half)) >> 16;
      out.slope = ((side * taper) >> 16) + along;
      // How deep inside its footprint the point is, mm: from the stoss's edge or the rounded lee's
      // end, blended as the profile blends its two sides, and from the crest's end.
      const auto depth = [&](i64 u) { return max_i64(0, min_i64(u + p.stoss, 3 * p.lee - u)); };
      const i64 across_depth = (depth(up) * w_plus + depth(um) * (k_one_q16 - w_plus)) >> 16;
      out.inside = min_i64(across_depth, p.half_length - abs_a);
    }
    return out;
  }
  const i64 x = (dx * p.ax + dz * p.az) >> 14;
  const i64 y = (dz * p.ax - dx * p.az) >> 14;
  const i64 xq = (x * (x < 0 ? p.inv_stoss : p.inv_half)) >> 16;
  const i64 yq = (y * p.inv_width) >> 16;
  const i64 rho2 = (xq * xq + yq * yq) >> 16;
  if (rho2 >= k_one_q16) return out;
  const i64 rho = static_cast<i64>(isqrt(static_cast<u64>(rho2) << 16));
  const i64 dome = (static_cast<i64>(p.height) * falloff_q16(rho)) >> 16;
  const i64 scoop_r = (h_mm * 7) / 2;
  const i64 scoop_x = scoop_r + (h_mm * 3) / 10;
  const i64 d = length(x - scoop_x, y);
  const i64 face = max_i64(0, ((d - (scoop_r - p.lee)) * k_tan_repose_q16 * 1000) >> 16);
  out.height = min_i64(dome, face);
  if constexpr (k_slope) {
    // Its slope bound: the dome's, over its narrowest radius, and the angle of repose over the
    // scoop (the slip face), fading out over `absorb` past its rim and to nothing at the dome's
    // edge. The narrowest radius's reciprocal is the largest of the three.
    const i64 inv_radius = max_i64(max_i64(p.inv_stoss, p.inv_half), p.inv_width);
    const i64 dome_slope = (h_mm * falloff_slope_q16(rho) * inv_radius) >> 32;
    const i64 body = min_i64(k_one_q16, ((k_one_q16 - rho) * 65536) / 9830);
    const i64 zone = d < scoop_r ? k_one_q16
                     : p.absorb > 0 && d < scoop_r + p.absorb
                         ? falloff_q16(((d - scoop_r) * 65536) / p.absorb)
                         : 0;
    out.slope = max_i64(dome_slope, (((k_tan_repose_q16 * zone) >> 16) * body) >> 16);
    out.inside = ((k_one_q16 - rho) << 16) / max_i64(1, inv_radius);
  }
  return out;
}

void DuneField::features(i64 x, i64 z, i64& ridge, i64& flatten, i64& lag, i64& squeeze,
                         i64& cap) const noexcept {
  ridge = 0;
  lag = 0;
  squeeze = 0;
  flatten = k_one_q16;
  cap = k_no_cap;
  for (const RidgeFeature& r : desc_.ridges) {
    const i64 w = max_i64(1, r.width);
    const i64 d = segment_distance(x, z, r, 2 * w + cap_reach_);
    // The cap (terrain.md, "The repose limiter"): within two widths of the ridge's line, where its
    // thinning and its lag's squeeze vary, no more sand than keeps height times their gradients
    // (0.6 pi / 2w and 1.5 L / w at most) under k_cap_slack; past that, rising at k_tan_cap until
    // it passes the tallest sand the bands can stack.
    if (d - 2 * w <= cap_reach_) {
      const i64 flat_um = (k_cap_slack_q16 * w * 1000) /
                          (61'763 + 3 * static_cast<i64>(desc_.ridge_lag_q16) / 2);  // 0.94 + 1.5 L
      cap = min_i64(cap, flat_um + ((max_i64(0, d - 2 * w) * k_tan_cap_q16 * 1000) >> 16));
    }
    if (d < w) {
      // 1/2 + cos(pi d / w)/2, the renderer's cosine profile, from the integer cosine.
      const i64 profile = k_one_q15 + cos_q15(static_cast<u32>((d * 32768) / w));
      ridge = max_i64(ridge, profile);
    }
    if (d < 2 * w) {
      const i64 held =
          (((w * desc_.ridge_lag_q16) >> 16) * falloff_q16((d * 65536) / (2 * w))) >> 16;
      lag = max_i64(lag, held);
      // The lag is w L falloff(d / 2w), so its gradient is at most L/2 |falloff'(u)| = 3 L u (1 -
      // u) with u = d / 2w: a lattice looked up through it is squeezed by at most 1 + that (the
      // largest singular value of I + wind grad(lag)), and a face at the angle of repose steepened
      // by as much. The bound, not the gradient, because the bound of the largest of several
      // ridges' lags is continuous where the gradient of it is not.
      const i64 u = (d * 65536) / (2 * w);
      const i64 bound =
          (((3 * static_cast<i64>(desc_.ridge_lag_q16) * u) >> 16) * (k_one_q16 - u)) >> 16;
      squeeze = max_i64(squeeze, bound);
    }
  }
  for (const BasinFeature& b : desc_.basins) {
    const i64 radius = max_i64(1, b.radius);
    const i64 dx = x - b.x;
    const i64 dz = z - b.z;
    const i64 reach = radius + cap_reach_;
    if (abs_i64(dx) >= reach || abs_i64(dz) >= reach) continue;
    const i64 d = length(dx, dz);
    // The cap: within the radius, where the flattening varies (by 1.5 / 0.65 R at most), no more
    // sand than keeps height times that under k_cap_slack; past it, rising at k_tan_cap.
    const i64 flat_um = (k_cap_slack_q16 * radius * 1000 * 65) / (150 * 65536);
    cap = min_i64(cap, flat_um + ((max_i64(0, d - radius) * k_tan_cap_q16 * 1000) >> 16));
    const i64 r = (d * 65536) / radius;
    if (r >= k_one_q16) continue;
    // smoothstep(0.35, 1, r): the dunes die away towards the basin's middle.
    const i64 t = ((r - 22938) * 65536) / (k_one_q16 - 22938);
    flatten = min_i64(flatten, k_one_q16 - falloff_q16(t));
  }
}

i32 DuneField::ridge_q16(i64 x, i64 z) const noexcept {
  i64 ridge = 0, flatten = 0, lag = 0, squeeze = 0, cap = 0;
  features(x, z, ridge, flatten, lag, squeeze, cap);
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
  i64 ridge = 0, flatten = 0, lag = 0, squeeze = 0, cap = 0;
  features(x, z, ridge, flatten, lag, squeeze, cap);
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

void DuneField::ripple_wind(i64 time_us, f64& x, f64& z) const noexcept {
  // The gather's rule (`gather`, the ripple terms): today's direction and yesterday's, and the
  // blend over the day's first two hours, from the same integer inputs. The gather blends the two
  // patterns' heights; one direction is the angle between them, turned along the shorter arc so a
  // day boundary, where the blend is 0 and yesterday's is the day before's today, is no step.
  const i64 day = day_of(time_us);
  const WindDay& today = wind_.day(day);
  const WindDay& yesterday = wind_.day(day - 1);
  const f64 blend = static_cast<f64>(min_i64(k_one_q16, ((time_us - day * k_us_per_day) * 65536) /
                                                            k_ripple_realign_us)) /
                    65536.0;
  const f64 to =
      std::atan2(static_cast<f64>(sin_q15(today.turn)), static_cast<f64>(cos_q15(today.turn)));
  const f64 from = std::atan2(static_cast<f64>(sin_q15(yesterday.turn)),
                              static_cast<f64>(cos_q15(yesterday.turn)));
  constexpr f64 k_full_turn = 6.28318530717958647692;
  f64 delta = std::remainder(to - from, k_full_turn);
  if (delta <= -k_full_turn * 0.5) delta += k_full_turn;
  const f64 angle = from + delta * blend;
  x = std::cos(angle);
  z = std::sin(angle);
}

namespace {

// One band laid on the bands before it (terrain.md, "The repose limiter"): given its slope bound
// (Q16), the bound on the slope of the bands before it (`base_slope`) and how deep inside their
// footprints the point is (`inside`, mm), returns the share of its height the band stands at
// (Q16) and adds the slope it brings to `base_slope`. The share is the room the base leaves,
// (tan 34 - base) over what the band can add per unit of its share — its own slope, at most tan
// 34, and for a coupled band the coupling's fade too — so the sum stays at the angle of repose;
// over a slip face the base is tan 34 and the share nothing, which is absorption.
i64 stack_band(const BandDesc& band, i64 slope, i64 inside, i64& base_slope) noexcept {
  const i64 free = k_tan_repose_q16 - base_slope;
  if (free <= 0) return 0;
  if (band.couple == BandCouple::none) {
    const i64 room = k_one_q16 - (base_slope * 65536) / k_tan_repose_q16;
    base_slope += (slope * room) >> 16;
    return room;
  }
  // On the flanks of the bands before it only, or on the floors between them only, fading across
  // `couple_mm` inside their footprints. Across a width and not a height of their sand: their sand
  // rises as steeply as their faces do, so a fade across a few metres of it stood a tall band up
  // over a few metres of ground at the foot of a slip face. The fade's own slope is the band's
  // height times its rate — at most the band's tallest, times 1.5 over the width, times 1.25 for
  // a depth measured across a drifting crest — and it goes into the budget with the band's own:
  // the bound uses the band's tallest rather than its height here, so the room does not vary as
  // fast as the band does.
  const i64 width = max_i64(1, band.couple_mm);
  const i64 t = (inside * 65536) / width;
  const i64 fade = falloff_q16(t);
  const i64 couple = band.couple == BandCouple::floors ? fade : k_one_q16 - fade;
  const i64 fade_slope = (band.height_hi_cm * 10 * falloff_slope_q16(t) * 5) / (4 * width);
  const i64 room = min_i64(k_one_q16, (free * 65536) / (k_tan_repose_q16 + fade_slope));
  base_slope += (room * (((couple * slope) >> 16) + fade_slope)) >> 16;
  return (room * couple) >> 16;
}

}  // namespace

Sample DuneField::sample(const Gather& gather, i64 x, i64 z, Detail detail) const noexcept {
  Sample s;
  s.floor = floor_um(x, z);
  i64 ridge = 0, flatten = 0, ridge_lag = 0, squeeze = 0, cap = 0;
  features(x, z, ridge, flatten, ridge_lag, squeeze, cap);
  s.ridge_q16 = static_cast<u16>(min_i64(ridge, 65535));
  s.basin_q16 = static_cast<u16>(min_i64(basin_q16(x, z), 65535));
  if (detail == Detail::floor) return s;
  const i64 wx = wind_.prevailing_x_q14();
  const i64 wz = wind_.prevailing_z_q14();
  i64 sand = 0;
  i64 base_slope = 0;  // Q16: a bound on the slope of the bands so far
  i64 inside = 0;      // mm: how deep inside their footprints the point is
  for (u32 b = 0; b < gather.bands; ++b) {
    const BandDesc& band = bands_[b];
    if (detail == Detail::coarse && !band.far) continue;
    if (!carries(b, gather.filter_mm)) {
      // A band the lattice cannot carry (renderer.md, "Ground to the horizon"): its mean, at the
      // share the bands before it leave it — on a coupled band's flanks or floors as it would
      // stand there — and flat, so it adds no slope to the bands after it and no footprint.
      sand += (mean_um_[b] * stack_band(band, 0, inside, base_slope)) >> 16;
      continue;
    }
    const i64 lag = ridge_lag + (gather.lag != nullptr ? gather.lag->lag_mm(lag_slot(b), x, z) : 0);
    const i64 qx = x - gather.dx[b] + ((wx * lag) >> 14);
    const i64 qz = z - gather.dz[b] + ((wz * lag) >> 14);
    const Value v = b + 1 < gather.bands || detail == Detail::full
                        ? band_value<true>(gather, b, qx, qz)
                        : band_value<false>(gather, b, qx, qz);
    sand += (v.height * stack_band(band, v.slope, inside, base_slope)) >> 16;
    inside = max_i64(inside, v.inside);
  }
  const i64 mask = (((k_one_q16 - ((k_ridge_thinning_q16 * ridge) >> 16)) * flatten) >> 16);
  // Capped by the features, then thinned over the rock and flattened into a basin. Scaling a tall
  // dune across a ridge's or a basin's edge is a slope of its own — its height times the mask's
  // gradient — so the sand is capped first, low enough where the masks vary that the product stays
  // at the angle of repose, and rising at 30 degrees beyond: the smaller of two surfaces is no
  // steeper than the steeper of them (terrain.md, "The repose limiter").
  s.sand = (min_i64(sand, cap) * mask) >> 16;
  // A band held back by a ridge is squeezed along the wind; it stands lower by as much, so its
  // faces stay at the angle of repose (terrain.md, "The repose limiter").
  if (squeeze > 0) s.sand = (s.sand * k_one_q16) / (k_one_q16 + squeeze);
  if (detail == Detail::full) {
    // Ripples do not survive on a slip face either.
    const i64 room = max_i64(0, k_one_q16 - (base_slope * 65536) / k_tan_repose_q16);
    s.detail = (((detail_um(gather, x, z) * mask) >> 16) * room) >> 16;
  }
  return s;
}

i64 DuneField::band_weight_q16(const Gather& gather, u32 b, i64 x, i64 z) const noexcept {
  // The evaluation's own loop up to band b: the share of its height it stands at.
  i64 ridge = 0, flatten = 0, ridge_lag = 0, squeeze = 0, cap = 0;
  features(x, z, ridge, flatten, ridge_lag, squeeze, cap);
  const i64 wx = wind_.prevailing_x_q14();
  const i64 wz = wind_.prevailing_z_q14();
  i64 inside = 0;
  i64 base_slope = 0;
  for (u32 c = 0; c <= b; ++c) {
    const i64 lag = ridge_lag + (gather.lag != nullptr ? gather.lag->lag_mm(lag_slot(c), x, z) : 0);
    const Value v = band_value<true>(gather, c, x - gather.dx[c] + ((wx * lag) >> 14),
                                     z - gather.dz[c] + ((wz * lag) >> 14));
    const i64 share = stack_band(bands_[c], v.slope, inside, base_slope);
    if (c == b) return share;
    inside = max_i64(inside, v.inside);
  }
  return 0;
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

f64 DuneField::ground_height(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const auto* field = static_cast<const DuneField*>(context);
  return static_cast<f64>(height_m(field->floor_um(x_mm, z_mm)));
}

}  // namespace engine::terrain

#pragma once

// **An open sea's waves from a wind-wave spectrum** (docs/subsystems/water.md, "The spectrum";
// docs/experiments/ocean-surface.md, "Stage 1b"; roadmap S93). An open water body with a wind
// replaces the pond's four sines with `k_water_waves` Gerstner (trochoidal) waves drawn from the
// JONSWAP spectrum (Hasselmann et al. 1973) at that wind and the body's fetch, each turned from the
// wind by Donelan–Banner spreading (Donelan, Hamilton and Hui 1985; Banner 1990): one wave per band
// of equal log width over [0.5, 4] times the peak frequency, its amplitude the band's variance as
// one sine, its direction the spreading's cumulative distribution inverted at a hashed uniform, its
// phase hashed from the seed, its wavenumber from the deep-water dispersion relation, and its
// frequency rounded to whole cycles of the water's period, so the clock still wraps with no jump.
// The constants and their sources are on the ocean-surface page.
//
// The CPU makes the table once per body when the scene is loaded (`water_spectrum`); the GPU scene
// packs it into the material table after the water material's next entry (`set_water_spectrum`);
// the resolve's water branch sums it per pixel (water.slang, `water_normal_spectrum`), and the
// double-precision reference (domain/gfx/tests/water_reference.h) sums the same table.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/gfx/visibility_resolve.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine::gfx {

// The waves an open sea sums, and the material entries their table takes (32 bytes a wave, 112 an
// entry): 16 waves are 512 bytes, five entries.
inline constexpr u32 k_water_waves = 16;
inline constexpr u32 k_water_wave_entries = 5;

// `k_material_water_spectrum`: the water material's waves are the table in the five entries after
// its next one (`set_water_spectrum`) instead of the four sines. Only an open body with a wind.
inline constexpr u32 k_material_water_spectrum = 256u;
// A water class bit (visibility_resolve.h, "The flags word's classes"; R141).
static_assert(k_material_water_spectrum == k_material_water_spectrum_bit &&
              (k_material_water_flags & k_material_water_spectrum) != 0);

// One wave of the table, mirrored by water.slang's `WaterWave` (32 bytes).
struct WaterWave {
  Vec2 direction{1.0f, 0.0f};  // unit, over (x, z): where the crests travel
  f32 k = 0.0f;                // the wavenumber, radians a metre
  f32 amplitude = 0.0f;        // metres
  f32 cycles = 0.0f;           // whole cycles a `k_water_period_s`
  f32 phase = 0.0f;            // at the clock's zero, cycles in [0, 1)
  f32 steepness = 0.0f;        // Gerstner's Q k a: how far the crest sharpens the normal
  // (5/8) (omega_p / omega)^4: how fast the wave's amplitude falls where the sea is younger than
  // the table's (`water_grown_share`; S100 stage B).
  f32 grow = 0.0f;
};
static_assert(sizeof(WaterWave) == 32);
static_assert(sizeof(WaterWave) * k_water_waves <= sizeof(ResolveMaterial) * k_water_wave_entries);

inline constexpr f64 k_water_spectrum_g = 9.81;
// JONSWAP's mean peak enhancement and its widths below and above the peak (Hasselmann et al.
// 1973): the shape of a sea whose wind is not known — a swell, the second train's split — since a
// wind sea's own shape is its development's (`jonswap_shape`).
inline constexpr f64 k_jonswap_gamma = 3.3;
inline constexpr f64 k_jonswap_sigma_low = 0.07;
inline constexpr f64 k_jonswap_sigma_high = 0.09;
// The band the waves are drawn from, as multiples of the peak frequency: below half of it JONSWAP
// holds almost nothing, and above four times it the waves are shorter than a sixteenth of the peak
// wavelength, which the footprint fades first.
inline constexpr f64 k_water_band_low = 0.5;
inline constexpr f64 k_water_band_high = 4.0;
// The sum's steepness: each wave keeps its own k a unless the waves' together pass this, when all
// are scaled down alike, so no crest loops over itself.
inline constexpr f64 k_water_max_steepness = 0.6;

// **A wind sea stops growing** (S100 stage B; docs/experiments/shore.md, "The spectrum for every
// body"; W136-2, docs/experiments/wind.md, "Full development"): the fetch laws below have no upper
// end, and a real sea is fully developed where JONSWAP's peak law, f_p U / g = 3.5 (g F /
// U^2)^(-1/3) (Hasselmann et al. 1973), meets Pierson and Moskowitz's 1964 fully developed peak,
// f_p U / g = 0.13: g F / U^2 = (22 / (2 pi 0.13))^3 = 19,539, so a longer fetch is taken as that
// one: 98 km at 7 m/s. (Until W136-2 it was 2.2e4, the meeting taken with the exponent 0.33 while
// the peak law here is the cube root, so the developed peak stood at 0.125 and its height 8 % over
// the limit.) A longer period than that wind's is swell, which a body says with `swell_period_s`.
inline constexpr f64 k_water_developed_nu = 0.13;
inline constexpr f64 k_water_developed_fetch =
    (22.0 / (2.0 * 3.141592653589793 * k_water_developed_nu)) *
    (22.0 / (2.0 * 3.141592653589793 * k_water_developed_nu)) *
    (22.0 / (2.0 * 3.141592653589793 * k_water_developed_nu));
inline f64 water_grown_fetch(f64 u10, f64 fetch_m) noexcept {
  return std::min(fetch_m, k_water_developed_fetch * u10 * u10 / k_water_spectrum_g);
}
// JONSWAP's peak law: ω_p = 22 (g² / (U₁₀ F))^(1/3), with U₁₀ the wind 10 m up (m/s) and F the
// fetch (m), kept to the fully developed one.
inline f64 jonswap_peak(f64 u10, f64 fetch_m) noexcept {
  return 22.0 * std::cbrt(k_water_spectrum_g * k_water_spectrum_g /
                          (u10 * water_grown_fetch(u10, fetch_m)));
}
// **The spectrum's shape is its development's** (W136-2; docs/experiments/wind.md, "Full
// development"). JONSWAP's fitted laws — α = 0.076 (g F / U²)^-0.22 for the tail, the peak law
// above, and a constant γ = 3.3 — do not integrate to JONSWAP's own measured energy, and held to
// full development they give a sea a third higher than Pierson and Moskowitz's (0.33 U² / g against
// 0.24). Lewis and Allos (1990, "JONSWAP's parameters: sorting out the inconsistencies", Ocean Eng.
// 17) refit the JONSWAP data with the shape a function of the dimensionless peak frequency ν = f_p
// U / g: γ = 5.87 ν^0.86, σ_a = 0.0547 ν^0.32, σ_b = 0.0783 ν^0.16, α = 0.0317 ν^0.67. At ν = 0.13
// they are Pierson and Moskowitz's (γ 1.02, α 0.0081), and at a young sea's ν = 0.35 the integral
// is JONSWAP's energy law, g H / U² = 1.6e-3 (g F / U²)^0.5, within a percent. γ is kept to [1, 7],
// the range measured seas take.
struct JonswapShape {
  f64 alpha = 0.0;
  f64 gamma = k_jonswap_gamma;
  f64 sigma_low = k_jonswap_sigma_low;
  f64 sigma_high = k_jonswap_sigma_high;
};
inline f64 jonswap_nu(f64 u10, f64 fetch_m) noexcept {
  return jonswap_peak(u10, fetch_m) * u10 / (2.0 * 3.141592653589793 * k_water_spectrum_g);
}
inline JonswapShape jonswap_shape_at(f64 nu) noexcept {
  const f64 v = std::max(nu, k_water_developed_nu);
  JonswapShape s;
  s.alpha = 0.0317 * std::pow(v, 0.67);
  s.gamma = std::clamp(5.87 * std::pow(v, 0.86), 1.0, 7.0);
  s.sigma_low = 0.0547 * std::pow(v, 0.32);
  s.sigma_high = 0.0783 * std::pow(v, 0.16);
  return s;
}
inline JonswapShape jonswap_shape(f64 u10, f64 fetch_m) noexcept {
  return jonswap_shape_at(jonswap_nu(u10, fetch_m));
}
// The Phillips constant of the sea of `u10` over `fetch_m`: its shape's.
inline f64 jonswap_alpha(f64 u10, f64 fetch_m) noexcept {
  return jonswap_shape(u10, fetch_m).alpha;
}

// **Every wave is a narrow band, not a line** (S100 stage B; water.md, "The spectrum"): a band of
// the spectrum is a carrier at its centre under a slowly varying complex envelope (Longuet-Higgins
// 1957: a narrow-band Gaussian sea is a carrier times a Rayleigh envelope), so each of the sixteen
// is drawn as a (e1 sin + e2 cos) with (e1, e2) smooth value noise over cells `k_water_group_along`
// wavelengths long the way the wave travels and `k_water_group_across` wide along its crest: the
// crests are short, the waves come in groups, and nothing lines up or repeats. The band's width is
// the table's own: neighbouring bands are 1.30 apart in wavenumber, whose beat is 1 / 0.26 = 3.8
// wavelengths; a crest 2.5 wavelengths long is the spread Donelan and Banner give the band about
// its own direction. The envelope travels at the deep-water group speed, half the phase speed: a
// whole number of cells a clock period, over a lattice periodic in that number, so the clock wraps
// with no jump.
inline constexpr f64 k_water_group_along = 4.0;
inline constexpr f64 k_water_group_across = 2.5;
// The envelope's gain: each component is value noise in [-1, 1], whose variance at a random point
// of a smoothstep lattice is (26/35)^2 / 3; the gain makes each component's 1/2, so a wave keeps
// the variance a^2 / 2 of the sine it replaces: sqrt(3/2) 35/26.
inline constexpr f64 k_water_envelope_gain = 1.6486932;
// How far the crests' sharpening may tilt the normal: the envelopes' sum has a Rayleigh tail.
inline constexpr f64 k_water_max_lift = 0.7;
// The share of the slope variance a pixel's footprint leaves out of the normal that is drawn back
// at the pixel's own scale (water.slang, `water_micro_normal`); the rest widens the lobe.
inline constexpr f64 k_water_micro_share = 0.7;
// The envelope's cells a clock period for a wave of `cycles`: its group travels cycles / 2
// wavelengths.
inline f64 water_envelope_cells(f64 cycles) noexcept {
  return std::max(1.0, std::floor(cycles / (2.0 * k_water_group_along) + 0.5));
}
// The lattice's hash: the cell along (already reduced modulo the cells), the cell across, and the
// wave's own salt (its phase's bits), to two values in [-1, 1].
inline u32 water_envelope_hash(u32 along, u32 across, u32 salt) noexcept {
  u32 h = along * 0x8DA6B343u ^ across * 0xD8163841u ^ salt * 0xCB1AB31Fu;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}
// **Where the sea is younger than the table's** (a closed body's lee): `grown` is the share of the
// table's fetch the wind has blown over at a place. The local peak is at omega_p grown^(-1/3), and
// below it the spectrum falls as exp(-(5/4)(omega_p'/omega)^4), so the amplitude of a wave of the
// table there is its own times exp(-grow (grown^(-4/3) - 1)), grow = (5/8)(omega_p/omega)^4: the
// long waves go first and the lee is ripples, then nothing (the Phillips constant's slow rise
// towards the lee, a factor grown^-0.11 in amplitude, is left out).
inline f64 water_grown_share(f64 grow, f64 grown) noexcept {
  const f64 g = std::clamp(grown, 1e-4, 1.0);
  return std::exp(-grow * (std::pow(g, -4.0 / 3.0) - 1.0));
}
// S(ω) = α g² ω⁻⁵ exp(−(5/4)(ω_p/ω)⁴) γ^r, r = exp(−(ω − ω_p)² / (2 σ² ω_p²)): m² s.
inline f64 jonswap(f64 omega, const JonswapShape& shape, f64 peak) noexcept {
  const f64 sigma = omega <= peak ? shape.sigma_low : shape.sigma_high;
  const f64 d = omega - peak;
  const f64 r = std::exp(-d * d / (2.0 * sigma * sigma * peak * peak));
  const f64 q = peak / omega;
  const f64 q4 = q * q * q * q;
  const f64 w5 = omega * omega * omega * omega * omega;
  return shape.alpha * k_water_spectrum_g * k_water_spectrum_g / w5 * std::exp(-1.25 * q4) *
         std::pow(shape.gamma, r);
}
// The mean shape's (γ 3.3, σ 0.07 / 0.09) at Phillips constant `alpha`: a swell's, and the second
// train's split, whose wind is not known.
inline f64 jonswap(f64 omega, f64 alpha, f64 peak) noexcept {
  JonswapShape mean;
  mean.alpha = alpha;
  return jonswap(omega, mean, peak);
}
// **The one height every reader takes** (W136-2): g H_m0 / U² of the sea grown over the
// dimensionless fetch χ = g F / U², the spectrum at that fetch's shape integrated over [0.3, 6] of
// its peak by the midpoint rule in 4,096 steps (all but a few parts in a thousand of its variance).
inline f64 jonswap_height_of_chi(f64 chi) noexcept {
  const f64 u = 10.0;
  const f64 fetch = chi * u * u / k_water_spectrum_g;
  const f64 peak = jonswap_peak(u, fetch);
  const JonswapShape shape = jonswap_shape(u, fetch);
  const u32 steps = 4096;
  const f64 lo = 0.3 * peak;
  const f64 dw = (6.0 - 0.3) * peak / static_cast<f64>(steps);
  f64 m0 = 0.0;
  for (u32 i = 0; i < steps; ++i)
    m0 += jonswap(lo + (static_cast<f64>(i) + 0.5) * dw, shape, peak) * dw;
  return 4.0 * std::sqrt(m0) * k_water_spectrum_g / (u * u);
}
// Tabulated once at 257 fetches log-spaced from χ = 0.01 to full development and read by linear
// interpolation of ln H over ln χ (the curve is smooth there: the table and the integral agree to
// a few parts in a million), so a sea state that steps the wind's history 145 times a call takes
// it at a table's cost, and its inverse is the table's.
inline constexpr u32 k_sea_height_nodes = 257;
inline constexpr f64 k_sea_height_chi_low = 0.01;
struct SeaHeightTable {
  f64 log_chi_low = 0.0;
  f64 step = 0.0;
  f64 log_h[k_sea_height_nodes]{};
};
inline const SeaHeightTable& sea_height_table() noexcept {
  static const SeaHeightTable table = [] {
    SeaHeightTable t;
    t.log_chi_low = std::log(k_sea_height_chi_low);
    t.step = (std::log(k_water_developed_fetch) - t.log_chi_low) /
             static_cast<f64>(k_sea_height_nodes - 1);
    for (u32 i = 0; i < k_sea_height_nodes; ++i)
      t.log_h[i] =
          std::log(jonswap_height_of_chi(std::exp(t.log_chi_low + t.step * static_cast<f64>(i))));
    return t;
  }();
  return table;
}
// The significant height (m) of the sea `u10` raises over `fetch_m`, kept to full development.
inline f64 water_sea_height(f64 u10, f64 fetch_m) noexcept {
  if (!(u10 > 0.0) || !(fetch_m > 0.0)) return 0.0;
  const SeaHeightTable& t = sea_height_table();
  const f64 x = (std::log(k_water_spectrum_g * water_grown_fetch(u10, fetch_m) / (u10 * u10)) -
                 t.log_chi_low) /
                t.step;
  const u32 last = k_sea_height_nodes - 1;
  const u32 i = x <= 0.0 ? 0u : std::min(static_cast<u32>(x), last - 1);
  const f64 f = x - static_cast<f64>(i);
  const f64 log_h = t.log_h[i] + (t.log_h[i + 1] - t.log_h[i]) * f;
  return std::exp(log_h) * u10 * u10 / k_water_spectrum_g;
}
// The fetch over which `u10` raises a sea of `height_m`: the table's own inverse. A height past
// what `u10` raises fully developed answers twice the developed fetch, so a caller capping at it
// sees the excess.
inline f64 water_fetch_for_height(f64 u10, f64 height_m) noexcept {
  if (!(u10 > 0.0) || !(height_m > 0.0)) return 0.0;
  const SeaHeightTable& t = sea_height_table();
  const f64 developed = k_water_developed_fetch * u10 * u10 / k_water_spectrum_g;
  const f64 log_h = std::log(height_m * k_water_spectrum_g / (u10 * u10));
  const u32 last = k_sea_height_nodes - 1;
  if (log_h > t.log_h[last] + 1e-12) return 2.0 * developed;
  u32 i = 0;
  if (log_h > t.log_h[0]) {
    u32 lo = 0;
    u32 hi = last;
    while (hi - lo > 1) {
      const u32 mid = (lo + hi) / 2;
      if (t.log_h[mid] < log_h)
        lo = mid;
      else
        hi = mid;
    }
    i = lo;
  }
  const f64 f = std::min((log_h - t.log_h[i]) / (t.log_h[i + 1] - t.log_h[i]), 1.0);
  const f64 log_chi = t.log_chi_low + t.step * (static_cast<f64>(i) + f);
  return std::min(std::exp(log_chi) * u10 * u10 / k_water_spectrum_g, developed);
}
// Donelan–Banner's β at r = ω / ω_p: D(θ) = β / (2 tanh(βπ)) sech²(βθ).
inline f64 donelan_banner_beta(f64 r) noexcept {
  if (r < 0.95) return 2.61 * std::pow(r, 1.3);
  if (r < 1.6) return 2.28 * std::pow(r, -1.3);
  return std::pow(10.0, -0.4 + 0.8393 * std::exp(-0.567 * std::log(r * r)));
}

// **A young sea is broader above its peak** (W136-3; docs/experiments/wind.md, "A wind sea's
// spread"): Hasselmann et al. (1980, "Directional wave spectra observed during JONSWAP 1973",
// J. Phys. Oceanogr. 10) fit Mitsuyasu's cos^(2s)(θ/2) to the JONSWAP data with s = 9.77 (f /
// f_p)^μ above the peak, μ = −2.33 − 1.45 (U / c_p − 1.17): the younger the sea (the larger the
// wind over the peak's speed, U / c_p = 2π ν), the faster the spread opens above the peak. At full
// development (ν 0.13, U / c_p 0.82) μ is −1.82, which is Donelan and Banner's own fall above the
// peak, so their β is kept there and a younger sea's opens by Hasselmann's difference, β ∝ √s:
// β = β_DB(r) r^(Δμ / 2), Δμ = −1.45 (2π ν − 2π 0.13). At the peak and below it the two fits agree
// that the spread does not depend on the sea's age (Donelan, Hamilton and Hui 1985 found none over
// 0.83 < U / c_p < 5); Mitsuyasu et al. 1975's s_p = 11.5 (U / c_p)^−2.5, much broader at a young
// peak, is the third fit and was not taken.
inline f64 wind_sea_beta(f64 r, f64 nu) noexcept {
  const f64 beta = donelan_banner_beta(r);
  if (!(r > 1.0)) return beta;
  const f64 two_pi = 2.0 * 3.141592653589793;
  const f64 dmu = -1.45 * two_pi * (std::max(nu, k_water_developed_nu) - k_water_developed_nu);
  return beta * std::pow(r, 0.5 * dmu);
}

// **A band's crests are as long as its directions are narrow** (W136-3): a band whose directions
// spread by σ_θ decorrelates across its crest over about λ / (2π σ_θ), so its envelope's width
// along the crest (`k_water_group_across` wavelengths at Donelan and Banner's peak, β 2.28) scales
// with β: a young sea's short waves crest for a wavelength, a far swell's for five. The width is
// carried as the length of the wave's `direction` (the shader and the reference divide it out),
// kept to [1, 8] wavelengths.
inline constexpr f64 k_water_peak_beta = 2.28;
inline constexpr f64 k_water_across_min = 1.0;
inline constexpr f64 k_water_across_max = 8.0;
inline f64 water_crest_scale(f64 beta) noexcept {
  return std::clamp(k_water_group_across * beta / k_water_peak_beta, k_water_across_min,
                    k_water_across_max) /
         k_water_group_across;
}

// **The share of a band's waves breaking** (W136-3; wind.md, "How steep a band stands"): Banner,
// Gemmrich and Farmer (2002, "Multiscale measurements of ocean wave breaking probability", J. Phys.
// Oceanogr. 32) found a band's breaking probability set by its saturation σ = (2π)⁴ f⁵ F(f) / 2g²
// = ω⁵ S(ω) / 2g² normalized by the directional spread at the peak, with a threshold common to
// 1 < f / f_p < 2.5; Banner and Morison (2010, Ocean Modelling 33) wrote it P = 33 (σ̃ − 0.0045)
// over the threshold. A Pierson–Moskowitz tail (σ = α / 2 = 0.004 over a peak spread of 0.40 rad)
// stands at σ̃ = 0.010, P = 0.18: the short waves of any wind sea break; a young gale's peak, 0.24.
inline constexpr f64 k_breaking_threshold = 0.0045;
inline constexpr f64 k_breaking_slope = 33.0;
inline f64 water_breaking_probability(f64 omega, f64 density) noexcept {
  const f64 w2 = omega * omega;
  const f64 saturation = w2 * w2 * omega * density / (2.0 * k_water_spectrum_g * k_water_spectrum_g);
  const f64 theta = 0.9068996821171089 / k_water_peak_beta;  // sech²'s σ at the peak, radians
  return std::clamp(k_breaking_slope * (saturation / theta - k_breaking_threshold), 0.0, 1.0);
}

// **Each band its own steepness** (W136-3): a band's Gerstner sharpening is its own k a. Where the
// table's sum passes `k_water_max_steepness` (no crest may loop), the bands that are not breaking
// give way first and the breaking ones keep their sharp crests; only if those alone pass it are all
// scaled alike, as before. (Measured: the sum is 0.48 for any fully developed sea and 0.66 for the
// beach's young gale, so the old shared scale bound only the young seas, and by under a tenth.)
inline void water_band_steepness(const f64 (&ka)[k_water_waves], const f64 (&breaking)[k_water_waves],
                                 WaterWave (&out)[k_water_waves]) noexcept {
  f64 hard = 0.0;
  f64 soft = 0.0;
  for (u32 i = 0; i < k_water_waves; ++i)
    (breaking[i] > 0.0 ? hard : soft) += ka[i];
  f64 scale_hard = 1.0;
  f64 scale_soft = 1.0;
  if (hard + soft > k_water_max_steepness) {
    if (hard >= k_water_max_steepness) {
      scale_hard = k_water_max_steepness / hard;
      scale_soft = 0.0;
    } else {
      scale_soft = (k_water_max_steepness - hard) / soft;
    }
  }
  for (u32 i = 0; i < k_water_waves; ++i)
    out[i].steepness = static_cast<f32>(ka[i] * (breaking[i] > 0.0 ? scale_hard : scale_soft));
}

// An integer hash of the seed, the wave and what it is drawn for, in (0, 1): never 0 or 1, so the
// spreading's inverse never meets atanh(±1).
inline f64 water_wave_uniform(u32 seed, u32 wave, u32 salt) noexcept {
  u32 h = seed * 0x9E3779B9u + (wave + 1u) * 0x85EBCA6Bu + (salt + 1u) * 0xC2B2AE35u;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return (static_cast<f64>(h >> 8) + 0.5) * (1.0 / 16777216.0);
}

// The table for a wind of `u10` m/s along `wind` (unit, over (x, z)) over `fetch_m` of water, the
// phases and directions from `seed`. A wind of 0 makes no waves (every amplitude 0).
inline void water_spectrum(f64 u10, f64 fetch_m, Vec2 wind, u32 seed,
                           WaterWave (&out)[k_water_waves]) noexcept {
  for (WaterWave& w : out)
    w = WaterWave{};
  if (!(u10 > 0.0) || !(fetch_m > 0.0)) return;
  const f64 peak = jonswap_peak(u10, fetch_m);
  const JonswapShape shape = jonswap_shape(u10, fetch_m);
  const f64 lo = std::log(k_water_band_low * peak);
  const f64 step = (std::log(k_water_band_high * peak) - lo) / static_cast<f64>(k_water_waves);
  const f64 two_pi = 6.283185307179586;
  const f64 wx = static_cast<f64>(wind.x);
  const f64 wz = static_cast<f64>(wind.y);
  const f64 nu = jonswap_nu(u10, fetch_m);
  f64 ka[k_water_waves];
  f64 breaking[k_water_waves];
  for (u32 i = 0; i < k_water_waves; ++i) {
    const f64 a0 = std::exp(lo + step * static_cast<f64>(i));
    const f64 a1 = std::exp(lo + step * static_cast<f64>(i + 1));
    const f64 omega = std::sqrt(a0 * a1);  // the band's geometric centre
    const f64 density = jonswap(omega, shape, peak);
    const f64 amplitude = std::sqrt(2.0 * density * (a1 - a0));
    const f64 k = omega * omega / k_water_spectrum_g;
    const f64 beta = wind_sea_beta(omega / peak, nu);
    const f64 u = water_wave_uniform(seed, i, 1);
    const f64 theta = std::atanh(std::tanh(beta * 3.141592653589793) * (2.0 * u - 1.0)) / beta;
    const f64 c = std::cos(theta);
    const f64 s = std::sin(theta);
    const f64 dx = wx * c - wz * s;
    const f64 dz = wx * s + wz * c;
    const f64 len = std::sqrt(dx * dx + dz * dz) / water_crest_scale(beta);
    WaterWave& w = out[i];
    w.direction = Vec2{static_cast<f32>(dx / len), static_cast<f32>(dz / len)};
    breaking[i] = water_breaking_probability(omega, density);
    w.k = static_cast<f32>(k);
    w.amplitude = static_cast<f32>(amplitude);
    w.cycles = static_cast<f32>(std::max(1.0, std::floor(omega * k_water_period_s / two_pi + 0.5)));
    w.phase = static_cast<f32>(water_wave_uniform(seed, i, 0));
    const f64 q = peak / omega;
    w.grow = static_cast<f32>(0.625 * q * q * q * q);
    ka[i] = k * amplitude;
  }
  water_band_steepness(ka, breaking, out);
}

// The table into the `k_water_wave_entries` entries that follow the water material's next one
// (`entries` points at the first), raw: no reader takes them for a surface (no cluster names
// them), and the branch reads them as `WaterWave`s. Sets `k_material_water_spectrum`.
inline void set_water_spectrum(ResolveMaterial& water, ResolveMaterial* entries,
                               const WaterWave (&waves)[k_water_waves]) noexcept {
  water.flags |= k_material_water_spectrum;
  for (u32 e = 0; e < k_water_wave_entries; ++e)
    entries[e] = ResolveMaterial{};
  std::memcpy(static_cast<void*>(entries), waves, sizeof(WaterWave) * k_water_waves);
}

}  // namespace engine::gfx

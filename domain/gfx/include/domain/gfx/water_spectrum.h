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
// JONSWAP's peak enhancement and its widths below and above the peak (Hasselmann et al. 1973).
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
// body"): the fetch laws below have no upper end, and a real sea is fully developed where the
// dimensionless fetch g F / U^2 reaches about 2.2e4 (the fetch at which JONSWAP's peak law meets
// Pierson and Moskowitz's 1964 fully developed peak, f_p U / g = 0.13: (3.5 / 0.13)^(1/0.33);
// Hasselmann et al. 1973, section 2.4), so a longer fetch is taken as that one: 110 km at 7 m/s.
// A longer period than that wind's is swell, which a body says with `swell_period_s`.
inline constexpr f64 k_water_developed_fetch = 2.2e4;
inline f64 water_grown_fetch(f64 u10, f64 fetch_m) noexcept {
  return std::min(fetch_m, k_water_developed_fetch * u10 * u10 / k_water_spectrum_g);
}
// JONSWAP's fetch laws: the peak frequency ω_p = 22 (g² / (U₁₀ F))^(1/3) and the Phillips
// constant α = 0.076 (U₁₀² / (F g))^0.22, with U₁₀ the wind 10 m up (m/s) and F the fetch (m),
// kept to the fully developed one.
inline f64 jonswap_peak(f64 u10, f64 fetch_m) noexcept {
  return 22.0 * std::cbrt(k_water_spectrum_g * k_water_spectrum_g /
                          (u10 * water_grown_fetch(u10, fetch_m)));
}
inline f64 jonswap_alpha(f64 u10, f64 fetch_m) noexcept {
  return 0.076 * std::pow(u10 * u10 / (water_grown_fetch(u10, fetch_m) * k_water_spectrum_g), 0.22);
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
inline f64 jonswap(f64 omega, f64 alpha, f64 peak) noexcept {
  const f64 sigma = omega <= peak ? k_jonswap_sigma_low : k_jonswap_sigma_high;
  const f64 d = omega - peak;
  const f64 r = std::exp(-d * d / (2.0 * sigma * sigma * peak * peak));
  const f64 q = peak / omega;
  const f64 q4 = q * q * q * q;
  const f64 w5 = omega * omega * omega * omega * omega;
  return alpha * k_water_spectrum_g * k_water_spectrum_g / w5 * std::exp(-1.25 * q4) *
         std::pow(k_jonswap_gamma, r);
}
// Donelan–Banner's β at r = ω / ω_p: D(θ) = β / (2 tanh(βπ)) sech²(βθ).
inline f64 donelan_banner_beta(f64 r) noexcept {
  if (r < 0.95) return 2.61 * std::pow(r, 1.3);
  if (r < 1.6) return 2.28 * std::pow(r, -1.3);
  return std::pow(10.0, -0.4 + 0.8393 * std::exp(-0.567 * std::log(r * r)));
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
  const f64 alpha = jonswap_alpha(u10, fetch_m);
  const f64 lo = std::log(k_water_band_low * peak);
  const f64 step = (std::log(k_water_band_high * peak) - lo) / static_cast<f64>(k_water_waves);
  const f64 two_pi = 6.283185307179586;
  const f64 wx = static_cast<f64>(wind.x);
  const f64 wz = static_cast<f64>(wind.y);
  f64 steepness = 0.0;
  f64 ka[k_water_waves];
  for (u32 i = 0; i < k_water_waves; ++i) {
    const f64 a0 = std::exp(lo + step * static_cast<f64>(i));
    const f64 a1 = std::exp(lo + step * static_cast<f64>(i + 1));
    const f64 omega = std::sqrt(a0 * a1);  // the band's geometric centre
    const f64 amplitude = std::sqrt(2.0 * jonswap(omega, alpha, peak) * (a1 - a0));
    const f64 k = omega * omega / k_water_spectrum_g;
    const f64 beta = donelan_banner_beta(omega / peak);
    const f64 u = water_wave_uniform(seed, i, 1);
    const f64 theta = std::atanh(std::tanh(beta * 3.141592653589793) * (2.0 * u - 1.0)) / beta;
    const f64 c = std::cos(theta);
    const f64 s = std::sin(theta);
    const f64 dx = wx * c - wz * s;
    const f64 dz = wx * s + wz * c;
    const f64 len = std::sqrt(dx * dx + dz * dz);
    WaterWave& w = out[i];
    w.direction = Vec2{static_cast<f32>(dx / len), static_cast<f32>(dz / len)};
    w.k = static_cast<f32>(k);
    w.amplitude = static_cast<f32>(amplitude);
    w.cycles = static_cast<f32>(std::max(1.0, std::floor(omega * k_water_period_s / two_pi + 0.5)));
    w.phase = static_cast<f32>(water_wave_uniform(seed, i, 0));
    const f64 q = peak / omega;
    w.grow = static_cast<f32>(0.625 * q * q * q * q);
    ka[i] = k * amplitude;
    steepness += ka[i];
  }
  const f64 scale = steepness > k_water_max_steepness ? k_water_max_steepness / steepness : 1.0;
  for (u32 i = 0; i < k_water_waves; ++i)
    out[i].steepness = static_cast<f32>(ka[i] * scale);
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

#pragma once

// **Wave ripples on a sandy bed, as closed forms** (roadmap S102; the owner, 2026-11-12: sand
// under water should carry ripples as the dunes do; the design and the sources:
// docs/experiments/shore-mechanics.md, "Stage 4: the sand"). Under a wave the water at the bed
// moves to and fro, and a bed of sand answers with ripples whose crests lie across that motion — along the
// wave's crests — and whose size follows from the motion's reach and the grain. These are the
// published forms in double, held by a case (`water_tests.cpp`), **and nothing draws them yet**:
// the bottom's normal is the volume's, which the channels' row held when this was written.
//
// As the coastal engineering reference page on wave ripples gives them (the papers not opened):
//   the orbit's amplitude at the bed   A = a / sinh(k d), a the wave's amplitude, U0 = A omega;
//   the mobility number                psi = U0^2 / ((s - 1) g D), s = 2.65 for quartz;
//   the wavelength (Nielsen 1981, regular oscillation)  lambda = A (2.2 - 0.345 psi^0.34);
//   the steepness (Soulsby and Whitehouse 2005)  eta / lambda = 0.15 (1 - exp(-(5000 D / A)^3.5)).
// Nielsen's form reaches zero near psi = 230: the bed is swept flat, which under a breaking sea is
// the surf. A bed coarser than sand (2 mm) carries none.

#include <core/base/types.h>
#include <domain/gfx/water_swash.h>

#include <algorithm>
#include <cmath>

namespace engine::gfx {

inline constexpr f64 k_ripple_quartz = 2.65;      // the grain's density over the water's
inline constexpr f64 k_ripple_sand_max_m = 2e-3;  // sand ends at 2 mm (the Wentworth scale)

struct WaveRipple {
  f64 orbit_m = 0.0;   // A: how far the water at the bed moves either way
  f64 mobility = 0.0;  // psi
  f64 length_m = 0.0;  // crest to crest; 0: none
  f64 height_m = 0.0;  // crest to trough
};

// The ripples under a wave of height `height_m` and period `period_s` over `depth_m` of water on a
// bed of grains `grain_m` across.
inline WaveRipple water_wave_ripple(f64 height_m, f64 period_s, f64 depth_m, f64 grain_m) noexcept {
  WaveRipple r;
  if (!(height_m > 0.0) || !(period_s > 0.0) || !(depth_m > 0.0) || !(grain_m > 0.0)) return r;
  const f64 omega = 2.0 * 3.14159265358979323846 / period_s;
  const f64 k = water_wavenumber(omega * omega / k_swash_g, depth_m);
  r.orbit_m = 0.5 * height_m / std::sinh(std::min(k * depth_m, 30.0));
  const f64 speed = r.orbit_m * omega;
  r.mobility = speed * speed / ((k_ripple_quartz - 1.0) * k_swash_g * grain_m);
  if (grain_m > k_ripple_sand_max_m) return r;
  r.length_m = r.orbit_m * std::max(2.2 - 0.345 * std::pow(r.mobility, 0.34), 0.0);
  const f64 steepness = 0.15 * (1.0 - std::exp(-std::pow(5000.0 * grain_m / r.orbit_m, 3.5)));
  r.height_m = r.length_m * steepness;
  return r;
}

}  // namespace engine::gfx

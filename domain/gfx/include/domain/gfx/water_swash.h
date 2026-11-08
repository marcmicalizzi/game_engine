#pragma once

// **The swash** (docs/subsystems/water.md, "The shore"; the grounding and the derivation,
// docs/experiments/shore.md, "The swash"; roadmap S94): where a body's waterline is at a time, and
// how long ago the front last reached a height. The CPU half of it — the wave a wind raises over a
// fetch, Stockdon et al.'s run-up, the period rounded onto the water's clock — is computed once per
// body when the GPU scene writes its shore record (`gfx::WaterShore`); the per-pixel half, the
// front's profile and its last crossing, is mirrored in double here so the tests can hold the
// shader's closed form against a brute-force scan (water.slang, "the swash", is the same formulas
// in float, operation for operation).
//
// The front's height above the still level at body-local point p and time t, metres:
//
//   c = t / T + phi(p)          cycles; k = floor(c) the wave, u = c - k its phase in [0, 1)
//   A_k = R G(k mod N) m(p)     the wave's reach: the group's shape G, the place's jitter m
//   z(t) = z_lo + (A_k - z_lo) 4 s (1 - s),  z_lo = -rho R
//   s = u / 2q for u < q, else 1/2 + (u - q) / 2(1 - q)
//
// a ballistic swash (the front launched up the slope at the bore's collapse and decelerated by
// gravity, x_s = u0 t - g sin(beta) t^2 / 2: a parabola in time; Shen and Meyer 1963, Hughes 1992,
// Baldock and Holmes 1999), with the uprush the first q of the period and the backwash the rest, so
// the front decelerates up the slope and accelerates back, and both halves parabolic. Every wave
// starts and ends at z_lo, so the profile is continuous from one wave to the next whatever their
// reaches. G is a group of N waves whose reach rises from about 0.57 R to R and back (wave groups:
// every few waves larger); phi and m are value noise of the body-local point, so the front is
// ragged along the shore and its phase drifts along the beach.

#include <core/base/types.h>

#include <algorithm>
#include <cmath>

namespace engine::gfx {

inline constexpr f64 k_swash_g = 9.81;
// The principal lunar semidiurnal constituent's period: 12.4206012 h (M2; Doodson 1921, the
// constituent's speed 28.9841042 degrees an hour).
inline constexpr f64 k_water_m2_period_s = 12.4206012 * 3600.0;
// The swash's shape (water.md, "The shore"): waves a group, the uprush's share of the period, and
// how far below the still level the backwash runs as a share of R.
inline constexpr u32 k_swash_group = 7;
inline constexpr f64 k_swash_uprush = 0.4;
inline constexpr f64 k_swash_rundown = 0.2;
// The water's clock wraps every 1,024 s (gfx::k_water_period_s); the swash's period is rounded so a
// whole number of groups fits in it, and the profile does not jump at the wrap.
inline constexpr f64 k_swash_clock_s = 1024.0;
// The foreshore slope used when the map gives none, and the range a measured one is kept to.
inline constexpr f64 k_swash_slope_default = 0.1;
inline constexpr f64 k_swash_slope_min = 0.01;
inline constexpr f64 k_swash_slope_max = 0.5;

// The wave a wind of `u10` m/s raises over `fetch_m` metres of water (JONSWAP, Hasselmann et al.
// 1973, the same fetch laws `water_spectrum` takes): the peak period 2 pi / omega_p and the
// spectral significant height H_m0 = 4 sqrt(m0), m0 the spectrum's variance integrated over
// [0.3, 6] omega_p. Zero and zero with no wind.
struct SwashWave {
  f64 height_m = 0.0;
  f64 period_s = 0.0;
};
inline SwashWave swash_wave(f64 u10, f64 fetch_m) noexcept;

// Stockdon et al. (2006), eq. 19: the 2% exceedance run-up, metres above the still level, from the
// deep-water significant height H0, the peak period T (L0 = g T^2 / 2 pi) and the foreshore slope.
inline f64 stockdon_r2(f64 h0, f64 t, f64 beta) noexcept {
  if (!(h0 > 0.0) || !(t > 0.0)) return 0.0;
  const f64 l0 = k_swash_g * t * t / (2.0 * 3.14159265358979323846);
  const f64 hl = h0 * l0;
  const f64 setup = 0.35 * beta * std::sqrt(hl);
  const f64 swash = std::sqrt(hl * (0.563 * beta * beta + 0.004));
  return 1.1 * (setup + 0.5 * swash);
}

// The period on the water's clock: `period_s` rounded so `k_swash_clock_s` holds a whole number of
// groups (at least one).
inline f64 swash_period(f64 period_s) noexcept {
  if (!(period_s > 0.0)) return 0.0;
  const f64 group = static_cast<f64>(k_swash_group);
  const f64 groups = std::max(1.0, std::floor(k_swash_clock_s / (period_s * group) + 0.5));
  return k_swash_clock_s / (groups * group);
}

// The group's shape: wave j of the group reaches G(j) of R, 0.57 at the group's ends, 1 in its
// middle.
inline f64 swash_group(u32 j) noexcept {
  const f64 x = (static_cast<f64>(j % k_swash_group) + 0.5) / static_cast<f64>(k_swash_group);
  return 0.55 + 0.45 * (0.5 - 0.5 * std::cos(2.0 * 3.14159265358979323846 * x));
}

// The profile within a wave, 4 s (1 - s) of the phase u: 0 at u = 0 and 1, 1 at u = q.
inline f64 swash_shape(f64 u) noexcept {
  const f64 q = k_swash_uprush;
  const f64 s = u < q ? u / (2.0 * q) : 0.5 + (u - q) / (2.0 * (1.0 - q));
  return 4.0 * s * (1.0 - s);
}

// One place's swash: R and T the body's (T rounded), phi the place's phase offset in cycles and m
// its reach's jitter.
struct SwashPlace {
  f64 run_up_m = 0.0;
  f64 period_s = 0.0;
  f64 phase = 0.0;
  f64 jitter = 1.0;
};

inline f64 swash_reach(const SwashPlace& p, i64 wave) noexcept {
  const i64 n = static_cast<i64>(k_swash_group);
  const u32 j = static_cast<u32>(((wave % n) + n) % n);
  return p.run_up_m * swash_group(j) * p.jitter;
}

// The front's height above the still level at time t.
inline f64 swash_front(const SwashPlace& p, f64 t) noexcept {
  if (!(p.run_up_m > 0.0) || !(p.period_s > 0.0)) return 0.0;
  const f64 c = t / p.period_s + p.phase;
  const f64 k = std::floor(c);
  const f64 z_lo = -k_swash_rundown * p.run_up_m;
  const f64 a = swash_reach(p, static_cast<i64>(k));
  return z_lo + (a - z_lo) * swash_shape(c - k);
}

// **The last time the front stood at or above h**, as the age t - t_last in seconds: 0 while it
// does (the place is under the sheet), and a very large number when no wave of the group reaches h.
// Closed form (shore.md, "When the front last passed"): within wave k the front is at or above h
// for u in [u1, u2], where 4 s (1 - s) = c_k = (h - z_lo) / (A_k - z_lo) gives s = (1 -+ sqrt(1 -
// c_k)) / 2, mapped back through the two halves' s(u); the front left h last at the end of the most
// recent such interval — this wave's if u is past u2, else the last earlier wave that reached h.
// The group is periodic in k, so the last N waves hold every reach there is: the search back is at
// most N waves, and a height none of them reaches was never reached.
inline constexpr f64 k_swash_never = 1e9;
inline f64 swash_age(const SwashPlace& p, f64 t, f64 h) noexcept {
  if (!(p.run_up_m > 0.0) || !(p.period_s > 0.0)) return k_swash_never;
  const f64 z_lo = -k_swash_rundown * p.run_up_m;
  if (h <= z_lo) return 0.0;
  const f64 c = t / p.period_s + p.phase;
  const f64 kf = std::floor(c);
  const i64 k = static_cast<i64>(kf);
  const f64 u = c - kf;
  const f64 q = k_swash_uprush;
  for (u32 j = 0; j <= k_swash_group; ++j) {
    const f64 a = swash_reach(p, k - static_cast<i64>(j));
    const f64 ck = (h - z_lo) / (a - z_lo);
    if (!(ck <= 1.0)) continue;
    const f64 r = std::sqrt(std::max(0.0, 1.0 - ck));
    const f64 u1 = (1.0 - r) * q;             // s1 = (1 - r) / 2 on the uprush: u = 2 q s
    const f64 u2 = q + r * (1.0 - q);         // s2 = (1 + r) / 2 on the backwash
    const f64 now = u + static_cast<f64>(j);  // this wave's phase, counted from wave k - j
    if (j == 0 && u < u1) continue;           // this wave has not reached h yet
    if (now <= u2) return 0.0;                // under the sheet
    return (now - u2) * p.period_s;
  }
  return k_swash_never;
}

inline SwashWave swash_wave(f64 u10, f64 fetch_m) noexcept {
  SwashWave w;
  if (!(u10 > 0.0) || !(fetch_m > 0.0)) return w;
  const f64 g = k_swash_g;
  const f64 peak = 22.0 * std::cbrt(g * g / (u10 * fetch_m));
  const f64 alpha = 0.076 * std::pow(u10 * u10 / (fetch_m * g), 0.22);
  // m0 by the midpoint rule over [0.3, 6] omega_p in 4,096 steps (the spectrum is smooth there and
  // holds all but a few parts in a thousand of its variance in it).
  const u32 steps = 4096;
  const f64 lo = 0.3 * peak;
  const f64 hi = 6.0 * peak;
  const f64 dw = (hi - lo) / static_cast<f64>(steps);
  f64 m0 = 0.0;
  for (u32 i = 0; i < steps; ++i) {
    const f64 omega = lo + (static_cast<f64>(i) + 0.5) * dw;
    const f64 sigma = omega <= peak ? 0.07 : 0.09;
    const f64 d = omega - peak;
    const f64 r = std::exp(-d * d / (2.0 * sigma * sigma * peak * peak));
    const f64 q = peak / omega;
    m0 += alpha * g * g / (omega * omega * omega * omega * omega) *
          std::exp(-1.25 * q * q * q * q) * std::pow(3.3, r) * dw;
  }
  w.height_m = 4.0 * std::sqrt(m0);
  w.period_s = 2.0 * 3.14159265358979323846 / peak;
  return w;
}

}  // namespace engine::gfx

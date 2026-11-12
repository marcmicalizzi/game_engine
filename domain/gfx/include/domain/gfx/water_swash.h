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
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/gfx/water_spectrum.h>

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

// **The sheet's duration is the slope's, not the clock's** (roadmap S102;
// docs/experiments/shore-mechanics.md, "The swash is the slope's"). A front launched up a slope
// beta and slowed by gravity alone climbs a height E in sqrt(2 E / g) / beta seconds (Shen and
// Meyer 1963: the shoreline is a projectile, x_s = u0 t - g beta t^2 / 2, whose top is u0^2 / 2g
// high whatever the slope); the uprush being `k_swash_uprush` of the whole (the backwash is the
// longer half), a wave's sheet lasts that over `k_swash_uprush`, in periods of the wave:
//
//   D = sqrt(2 E / g) / (beta q T)
//
// On a steep shore D < 1: the sheet is up and back before the next wave, and the beach waits bare.
// On a gentle one D > 1: **the next bore arrives over the backwash**, and the waterline is the
// highest of the sheets still on the beach (Brocchini and Baldock 2008 review the interaction; the
// ratio D is their natural swash period over the incident one). Kept to [k_swash_span_min,
// k_swash_span_max] periods: past five the beach's swash is the infragravity band's, which no train
// of one period draws (Stockdon et al.'s second term).
inline constexpr f64 k_swash_span_min = 0.25;
inline constexpr f64 k_swash_span_max = 5.0;
// The sheets that can stand on a beach at once: the span's ceiling.
inline constexpr u32 k_swash_overlap = 5;
// The run-up's ceiling on a steep shore, in deep-water wave heights: Stockdon et al.'s fit is a
// sandy beach's (their slopes 0.01 to 0.16) and grows without bound with the slope, where a wave
// on a steep rough slope surges to a height that stops growing: Van der Meer (1988) puts the 2%
// run-up on a rock slope at no more than 1.97 significant wave heights however long the wave
// (his `d`; Hunt's 1959 limit for a smooth slope is 2.5, EurOtop 2018's "around 3").
inline constexpr f64 k_swash_steep_cap = 1.97;

// Stockdon et al.'s formula with the slope taken out: R2 = scale (0.35 beta + sqrt(0.563 beta^2 +
// 0.004) / 2), scale = 1.1 sqrt(H0 L0) — what a body's surface carries (`WaterSurface::
// reach_scale`) so a pixel finds its own stretch of shore's run-up from the slope there.
inline f64 swash_reach_scale(f64 h0, f64 t) noexcept {
  if (!(h0 > 0.0) || !(t > 0.0)) return 0.0;
  return 1.1 * std::sqrt(h0 * k_swash_g * t * t / (2.0 * 3.14159265358979323846));
}
inline f64 swash_local_run_up(f64 scale, f64 cap, f64 beta) noexcept {
  return std::min(scale * (0.35 * beta + 0.5 * std::sqrt(0.563 * beta * beta + 0.004)), cap);
}

// **The ceiling of a place's run-up** (`WaterSurface::reach_cap`): the steep shore's, and — a
// front's reach being its R times at most 1.32 (the along-shore variation and the lobes) — a tenth
// under the height the signed depth map knows its bank to (`bank_max_m`; 0: no bank range).
inline f64 swash_reach_cap(f64 h0, f64 bank_max_m) noexcept {
  const f64 cap = k_swash_steep_cap * h0;
  return bank_max_m > 0.0 ? std::min(cap, 0.68 * bank_max_m) : cap;
}
// The most any stretch of a body's shore can run up: the steepest slope a field keeps, under the
// ceiling. What a lifted body's shore record carries for its bounds and the ground's band.
inline f64 swash_highest_run_up(f64 h0, f64 t, f64 bank_max_m) noexcept {
  return swash_local_run_up(swash_reach_scale(h0, t), swash_reach_cap(h0, bank_max_m),
                            k_swash_slope_max);
}

// The 2% run-up on a shore of slope beta: Stockdon et al.'s, no higher than the steep shore's
// ceiling.
inline f64 swash_run_up(f64 h0, f64 t, f64 beta) noexcept {
  return swash_local_run_up(swash_reach_scale(h0, t), k_swash_steep_cap * h0, beta);
}

// One place's swash: R and T the body's (T rounded), phi the place's phase offset in cycles and m
// its reach's jitter. `slope` is the foreshore's there; 0 keeps S94's clocked sheet, one period
// long whatever the beach (the tests' closed forms before S102).
struct SwashPlace {
  f64 run_up_m = 0.0;
  f64 period_s = 0.0;
  f64 phase = 0.0;
  f64 jitter = 1.0;
  f64 slope = 0.0;
};

inline f64 swash_reach(const SwashPlace& p, i64 wave) noexcept {
  const i64 n = static_cast<i64>(k_swash_group);
  const u32 j = static_cast<u32>(((wave % n) + n) % n);
  return p.run_up_m * swash_group(j) * p.jitter;
}

// How long a wave's sheet of reach `reach` stays on the place's slope, in periods.
inline f64 swash_span(const SwashPlace& p, f64 reach) noexcept {
  if (!(p.slope > 0.0)) return 1.0;
  const f64 excursion = std::max(reach + k_swash_rundown * p.run_up_m, 0.0);
  return std::clamp(
      std::sqrt(2.0 * excursion / k_swash_g) / (p.slope * k_swash_uprush * p.period_s),
      k_swash_span_min, k_swash_span_max);
}

// The front's height above the still level at time t: the highest of the sheets on the beach.
inline f64 swash_front(const SwashPlace& p, f64 t) noexcept {
  if (!(p.run_up_m > 0.0) || !(p.period_s > 0.0)) return 0.0;
  const f64 c = t / p.period_s + p.phase;
  const f64 k = std::floor(c);
  const f64 u = c - k;
  const f64 z_lo = -k_swash_rundown * p.run_up_m;
  f64 z = z_lo;
  for (u32 i = 0; i < k_swash_overlap; ++i) {
    const f64 a = swash_reach(p, static_cast<i64>(k) - static_cast<i64>(i));
    const f64 v = (u + static_cast<f64>(i)) / swash_span(p, a);
    if (v < 1.0) z = std::max(z, z_lo + (a - z_lo) * swash_shape(v));
  }
  return z;
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
  // **With sheets that overlap** (S102) a wave's interval over h is [u1, u2] of its own span, and
  // the front left h last at the latest end among the waves that have reached it: the least of
  // their ages. A wave j back cannot give an age under (j - span_max) periods, so the search ends
  // once that passes the best found; the group and the longest span bound it.
  f64 best = k_swash_never;
  for (u32 j = 0; j < k_swash_group + k_swash_overlap; ++j) {
    if ((static_cast<f64>(j) - k_swash_span_max) * p.period_s >= best) break;
    const f64 a = swash_reach(p, k - static_cast<i64>(j));
    const f64 ck = (h - z_lo) / (a - z_lo);
    if (!(ck <= 1.0)) continue;
    const f64 span = swash_span(p, a);
    const f64 r = std::sqrt(std::max(0.0, 1.0 - ck));
    const f64 u1 = (1.0 - r) * q * span;        // s1 = (1 - r) / 2 on the uprush: u = 2 q s
    const f64 u2 = (q + r * (1.0 - q)) * span;  // s2 = (1 + r) / 2 on the backwash
    const f64 now = u + static_cast<f64>(j);    // this wave's phase, counted from wave k - j
    if (now < u1) continue;                     // this wave has not reached h yet
    if (now <= u2) return 0.0;                  // under the sheet
    best = std::min(best, (now - u2) * p.period_s);
  }
  return best;
}

// ---- the wave as height (roadmap S100; docs/subsystems/water.md, "The mesh"; the grounding,
// docs/experiments/shore.md, "The wave as height")
// -------------------------------------------------
//
// A water body with a wind (or an open one with a tide) is a deformed instance: the pool pass lifts
// every vertex by the body's height function (water_waves.slang, `water_vertex_lift`; mirrored in
// double by domain/gfx/tests/water_reference.h), and the resolve's normal is the same function's
// gradient. Offshore the body's own waves (an open body's sixteen, a closed one's four sines); near
// the shore **the depth governs**: the wind's peak wave shoals by linear theory's coefficient
// (Green's law, H ~ d^(-1/4), in shallow water) until it breaks at H = gamma d (McCowan 1894;
// Battjes and Janssen 1978 for a random sea), its height then held at gamma d to the shore, its
// wavenumber the dispersion relation's at the local depth — so its crests slow and bunch — and its
// phase the
// **arrival time** of the wave at the place (`water_arrival_field`), so neighbouring places are one
// train, not independent oscillators. Over the bank the vertex is the swash's sheet where the front
// covers it and parked under the ground where it does not.
inline constexpr f64 k_water_breaking_gamma = 0.78;  // McCowan (1894): H_b = 0.78 d
// How far under the ground a bank vertex the swash does not cover is parked, metres.
inline constexpr f64 k_water_bank_park_m = 0.05;
// The mesh's sheet reaches this share of the smooth front's reach (S102): a pixel's own reach has
// lobes of a tenth and a twentieth either way that a vertex cannot carry, and the mesh's edge has
// to lie under every pixel's sheet, or the water pixels past a pixel's front are bared in strips
// with the mesh's straight edges (water_waves.slang, `water_surface_front`).
inline constexpr f64 k_water_mesh_inside = 0.85;
// `WaterSurface::flags`.
inline constexpr u32 k_water_surface_spectrum = 1u;  // the waves are the sixteen below
inline constexpr u32 k_water_surface_open = 2u;      // open: the lift fades in from the outline
// The deep-water source of the arrival field: water at least this many deep-water peak wavelengths
// over 2 pi deep (k0 d >= 1: tanh(1) = 0.76, where shoaling starts to matter) starts the train.
inline constexpr f64 k_water_train_deep_kd = 1.0;
// Where the train takes over from the offshore waves: over k0 d in [0.35, 1].
inline constexpr f64 k_water_train_shallow_kd = 0.35;
// The depth below which the celerity is held (the shallow-water speed at 2 cm).
inline constexpr f64 k_water_celerity_floor_m = 0.02;

// The field's floats a texel: the signed depth (m), the train's arrival (s), `grown` — the share
// of the body's fetch the wind has blown over to reach the texel (`water_fetch_field`; 1 over an
// open body) — and the foreshore slope of the texel's own stretch of shore
// (`water_shore_slope_field`; S102, the spare until then).
inline constexpr u32 k_water_field_channels = 4;
// **One body's height function, for the GPU** (632 bytes; water_waves.slang's `WaterSurface`). The
// GPU scene writes one per deformed body at load; the pool pass reaches it through the body's
// `DeformDesc::joints` and the resolve through its `WaterShore::surface`.
struct WaterSurface {
  // f32 [4 * size * size]: (signed depth m, arrival s, grown, 0) per texel, rows along +z
  u64 field = 0;
  Vec2 origin{};  // body-local (x, z) of texel (0, 0)'s centre: the depth map's grid
  f32 texel_m = 0.0f;
  u32 size = 0;
  Vec2 wind{1.0f, 0.0f};  // the body's wind (unit, x z)
  f32 height_m = 0.0f;    // the wind's significant height H_m0 offshore
  f32 period_s = 0.0f;    // the peak period on the water's clock (swash_period)
  f32 k0 = 0.0f;          // the peak's deep-water wavenumber, (2 pi / T)^2 / g
  f32 run_up_m = 0.0f;    // Stockdon's R2
  f32 slope = 0.1f;       // the foreshore slope
  f32 tide_m = 0.0f;      // an open body's M2 amplitude
  f32 fade_m = 0.0f;      // an open body's: the lift fades in over this from the outline (UV.x)
  u32 flags = 0;          // k_water_surface_*
  f32 amplitude = 0.0f;   // the four sines' (a closed body's): the material's numbers
  f32 wavelength = 0.0f;
  f32 chop = 0.0f;
  u32 seed = 0;
  Vec2 direction{1.0f, 0.0f};
  f32 along_m = 0.0f;  // the swash reach's along-shore scale
  // **What the mesh resolves** (S100 stage B; water.md, "The mesh"): the spacing of its vertices
  // along a ring and far from the outline, the spacing of its rings at the outline, and how much a
  // ring's spacing grows per metre in from it (the rings' geometric grade less one). The pool's
  // lift leaves out what four vertices a wavelength cannot carry; the normal keeps it.
  f32 mesh_far_m = 0.0f;
  f32 mesh_near_m = 0.0f;
  f32 mesh_grade = 0.0f;
  // **A swell's train** (roadmap S102: an open body's `swell_height_m` at `swell_period_s` from
  // `swell_direction_deg`): when `height_m`, `period_s` and `k0` are a swell's, this is the wind
  // sea's own peak wavenumber (deep water, 1/m), which then says where the wind's table calms
  // towards the shore; the swell's train is drawn over the whole body, since a swell is there
  // offshore too. 0: the train is the wind's peak wave, taking over from the table in the shallows.
  f32 swell = 0.0f;
  // **The run-up of a place is its slope's** (S102): R = min(reach_scale (0.35 beta + sqrt(0.563
  // beta^2 + 0.004) / 2), reach_cap) with beta the field's fourth channel — Stockdon et al.'s
  // formula with 1.1 sqrt(H0 L0) taken out, under the steep shore's ceiling and the height the
  // depth map's bank is known to. 0: `run_up_m` everywhere (a surface with no field).
  f32 reach_scale = 0.0f;
  f32 reach_cap = 0.0f;
  f32 pad = 0.0f;
  WaterWave waves[k_water_waves];  // the wind's spectrum (k_water_surface_spectrum)
};
static_assert(sizeof(WaterSurface) == 632);

// **The dispersion relation at depth**: the wavenumber k with omega^2 = g k tanh(k d), from the
// deep-water k0 = omega^2 / g — Fenton and McKee's (1990) explicit approximation
// k d = k0 d coth((k0 d)^(3/4))^(2/3), exact in both limits, then two Newton steps on\n// k d
// tanh(k d) = k0 d (one leaves 1.5e-4 of omega^2; two, 1e-8). Water at or above the level\n// (d <=
// 0) is answered at 1 mm.
inline f64 water_wavenumber(f64 k0, f64 d) noexcept {
  const f64 depth = std::max(d, 1e-3);
  const f64 x0 = k0 * depth;
  f64 y = x0 * std::pow(1.0 / std::tanh(std::pow(x0, 0.75)), 2.0 / 3.0);
  for (u32 i = 0; i < 2; ++i) {
    const f64 th = std::tanh(y);
    y -= (y * th - x0) / (th + y * (1.0 - th * th));
  }
  return y / depth;
}

// Linear theory's shoaling coefficient H / H0 = sqrt(cg0 / cg) at wavenumber k and depth d: 1 in
// deep water, sqrt(1 / (2 n tanh(k d))) with n = (1 + 2kd / sinh 2kd) / 2, and in shallow water
// Green's law: n -> 1 and tanh(k d) -> k d = sqrt(k0 d), so H / H0 -> (4 k0 d)^(-1/4), H ~
// d^(-1/4).
inline f64 water_shoaling(f64 k, f64 d) noexcept {
  const f64 kd = k * std::max(d, 1e-3);
  const f64 n = 2.0 * kd > 30.0 ? 0.5 : 0.5 * (1.0 + 2.0 * kd / std::sinh(2.0 * kd));
  return std::sqrt(0.5 / (n * std::tanh(kd)));
}

// The wave's height at depth d: shoaled until it breaks, then the saturated bore gamma d. 0 on the
// bank.
inline f64 water_wave_height(f64 h0, f64 k0, f64 d) noexcept {
  if (!(d > 0.0) || !(h0 > 0.0)) return 0.0;
  const f64 shoaled = h0 * water_shoaling(water_wavenumber(k0, d), d);
  return std::min(shoaled, k_water_breaking_gamma * d);
}

// **Every texel takes its nearest marked texel's value** (S102): `distance` is 0 at the marked
// texels and `k_water_carry_none` elsewhere, and on return every texel a marked one can be reached
// from holds that one's `value` and its distance in the 3-4 chamfer metric (three a step along an
// axis, four a diagonal: a two-pass transform, integer, in a fixed order). What carries a
// waterline's numbers — its arrival, its slope — onto the bank behind it and the water at its foot.
inline constexpr u32 k_water_carry_none = 0xffffffffu;
inline void water_carry_nearest(u32 size, Vector<f64>& value, Vector<u32>& distance) noexcept {
  const i64 n = size;
  const auto relax = [&](u32 here, i64 i, i64 j, u32 cost) {
    if (i < 0 || j < 0 || i >= n || j >= n) return;
    const u32 there = static_cast<u32>(j * n + i);
    if (distance[there] == k_water_carry_none) return;
    if (distance[there] + cost < distance[here]) {
      distance[here] = distance[there] + cost;
      value[here] = value[there];
    }
  };
  for (i64 j = 0; j < n; ++j) {
    for (i64 i = 0; i < n; ++i) {
      const u32 here = static_cast<u32>(j * n + i);
      relax(here, i - 1, j, 3);
      relax(here, i, j - 1, 3);
      relax(here, i - 1, j - 1, 4);
      relax(here, i + 1, j - 1, 4);
    }
  }
  for (i64 j = n - 1; j >= 0; --j) {
    for (i64 i = n - 1; i >= 0; --i) {
      const u32 here = static_cast<u32>(j * n + i);
      relax(here, i + 1, j, 3);
      relax(here, i, j + 1, 3);
      relax(here, i + 1, j + 1, 4);
      relax(here, i - 1, j + 1, 4);
    }
  }
}

// **The arrival field**: the time the wave train's crest takes to reach each texel from deep water,
// seconds — the eikonal |grad tau| = 1 / c(d) with c = omega / k(d) the local phase speed, solved
// on the depth map's grid by fast sweeping (Zhao 2005; Godunov's upwind update, four sweep orders,
// `rounds` times). The source is every texel at least 1 / k0 deep (`k_water_train_deep_kd`), or the
// deepest fifth when none is; the bank (d <= 0) takes the arrival of the nearest water the train
// reached (until S102 it was crossed at 10 km/s; the reason is with the sweeps below), so its
// arrival is its own stretch of shoreline's. Rays so bend towards the shallows and the crests come
// in along the depth's contours. `depth` and `out` are size * size, rows along +z; deterministic (a
// fixed sweep order in double).
//
// **The train comes from somewhere** (roadmap S102; docs/experiments/shore-mechanics.md, "A wave
// has a direction"): with a `direction` (the way the waves travel over deep water, x z, need not be
// unit; zero: none) the source is not a still pool whose every texel the crest leaves at once but a
// **plane wave**: a source texel's arrival is its distance along the direction over the phase speed
// at the source's own shallowest depth, less the least of them, so the wave enters the shallows at
// an angle and the eikonal turns it — Snell's law, sin(theta) / c constant along a straight coast —
// and the crest reaches one end of a beach before the other. With none every source texel is 0, as
// it was (S100): crests leave the deep water along its contour.
inline void water_arrival_field(const f32* depth, u32 size, f64 texel_m, f64 period_s, f32* out,
                                u32 rounds = 3, Vec2 direction = Vec2{0.0f, 0.0f}) noexcept {
  const u32 count = size * size;  // a depth map is at most 1,024 a side
  if (size < 2 || !(period_s > 0.0) || !(texel_m > 0.0)) {
    for (u32 i = 0; i < count; ++i)
      out[i] = 0.0f;
    return;
  }
  const f64 omega = 2.0 * 3.14159265358979323846 / period_s;
  const f64 k0 = omega * omega / k_swash_g;
  f64 deepest = 0.0;
  for (u32 i = 0; i < count; ++i)
    deepest = std::max(deepest, static_cast<f64>(depth[i]));
  const f64 source = std::min(k_water_train_deep_kd / k0, 0.8 * deepest);
  constexpr f64 k_far = 1e30;
  // tau in double, then the slowness per texel (texel_m / c).
  Vector<f64> tau(count);
  Vector<f64> step(count);
  // The plane wave's slowness along x and z, seconds a texel: the direction over the phase speed
  // where the source ends.
  const f64 dir_x = static_cast<f64>(direction.x);
  const f64 dir_z = static_cast<f64>(direction.y);
  const f64 dir_len = std::sqrt(dir_x * dir_x + dir_z * dir_z);
  f64 slow_x = 0.0;
  f64 slow_z = 0.0;
  if (dir_len > 0.0 && deepest > 0.0) {
    const f64 c_source = omega / water_wavenumber(k0, std::max(source, k_water_celerity_floor_m));
    slow_x = dir_x / dir_len * texel_m / c_source;
    slow_z = dir_z / dir_len * texel_m / c_source;
  }
  f64 first = k_far;
  for (u32 i = 0; i < count; ++i) {
    const f64 d = static_cast<f64>(depth[i]);
    const bool src = deepest > 0.0 && d >= source;
    tau[i] = k_far;
    if (src) {
      tau[i] = slow_x * static_cast<f64>(i % size) + slow_z * static_cast<f64>(i / size);
      first = std::min(first, tau[i]);
    }
    // A source texel is marked by a step of 0: the sweeps leave it as it was set. **The bank is a
    // wall to the sweeps** (a step under 0; S102): until then it was crossed at 10 km/s so that it
    // took its shoreline's arrival, and so it carried the earliest arrival on a shore along the
    // whole of it — harmless while every part of a beach was reached at once, and with a direction
    // the whole beach's swash in step again, with the lead running back out into the shallows. It
    // now takes its nearest stretch of waterline's arrival, below.
    step[i] = -1.0;
    if (src)
      step[i] = 0.0;
    else if (d > 0.0)
      step[i] = texel_m / (omega / water_wavenumber(k0, std::max(d, k_water_celerity_floor_m)));
  }
  for (u32 i = 0; i < count; ++i)
    if (step[i] == 0.0) tau[i] -= first;
  if (deepest > 0.0) {
    const i64 n = size;
    for (u32 r = 0; r < rounds; ++r) {
      for (u32 order = 0; order < 4; ++order) {
        const bool fx = (order & 1u) != 0;
        const bool fz = (order & 2u) != 0;
        for (i64 jj = 0; jj < n; ++jj) {
          const i64 j = fz ? n - 1 - jj : jj;
          for (i64 ii = 0; ii < n; ++ii) {
            const i64 i = fx ? n - 1 - ii : ii;
            const u32 at = static_cast<u32>(j * n + i);
            if (!(step[at] > 0.0)) continue;
            const f64 a = std::min(i > 0 ? tau[at - 1] : k_far, i + 1 < n ? tau[at + 1] : k_far);
            const f64 b =
                std::min(j > 0 ? tau[at - size] : k_far, j + 1 < n ? tau[at + size] : k_far);
            const f64 f = step[at];
            const f64 lo = std::min(a, b);
            if (!(lo < k_far)) continue;
            f64 t = lo + f;
            if (std::fabs(a - b) < f)
              t = 0.5 * (a + b + std::sqrt(2.0 * f * f - (a - b) * (a - b)));
            if (t < tau[at]) tau[at] = t;
          }
        }
      }
    }
  }
  // The bank, and any water the train cannot reach, take the arrival of the nearest water it did
  // reach: a place on the beach has its own stretch of shoreline's arrival — the swash's phase.
  Vector<u32> distance(count);
  for (u32 i = 0; i < count; ++i)
    distance[i] = tau[i] < k_far ? 0u : k_water_carry_none;
  water_carry_nearest(size, tau, distance);
  for (u32 i = 0; i < count; ++i)
    out[i] = tau[i] < k_far ? static_cast<f32>(tau[i]) : 0.0f;
}

// **The fetch at every place** (S100 stage B; docs/experiments/shore.md, "The spectrum for every
// body"): how far the wind has blown over the body's water to reach each texel, metres — w . grad F
// = 1 over water with F = 0 where the wind comes off the land, by a first-order upwind difference
// swept once in the wind's order, F = (h + |wx| F_x + |wz| F_z) / (|wx| + |wz|) with F_x and F_z
// the upwind neighbours along each axis (exact along the axes, and smeared across the wind by the
// scheme's own diffusion, sqrt(h x) wide after x: a lee's edge is soft, as a wind's shadow is).
// Over land the value is carried on and decays with an e-folding of `k_water_fetch_land_m`, so a
// beach's texels hold the sea's fetch at its waterline, and an island casts a lee. `wind` is unit,
// over (x, z); `depth` and `out` are size * size, rows along +z. Returns the longest fetch over
// water. Deterministic: a fixed order in double.
inline constexpr f64 k_water_fetch_land_m = 20.0;
inline f64 water_fetch_field(const f32* depth, u32 size, f64 texel_m, Vec2 wind,
                             f32* out) noexcept {
  const u32 count = size * size;
  f64 wx = std::fabs(static_cast<f64>(wind.x));
  f64 wz = std::fabs(static_cast<f64>(wind.y));
  if (size < 2 || !(texel_m > 0.0) || !(wx + wz > 0.0)) {
    for (u32 i = 0; i < count; ++i)
      out[i] = 0.0f;
    return 0.0;
  }
  const bool fx = wind.x < 0.0f;
  const bool fz = wind.y < 0.0f;
  const f64 step = texel_m / (wx + wz);
  const f64 decay = std::exp(-step / k_water_fetch_land_m);
  Vector<f64> fetch(count);
  f64 longest = 0.0;
  const i64 n = size;
  for (i64 jj = 0; jj < n; ++jj) {
    const i64 j = fz ? n - 1 - jj : jj;
    for (i64 ii = 0; ii < n; ++ii) {
      const i64 i = fx ? n - 1 - ii : ii;
      const u32 at = static_cast<u32>(j * n + i);
      const f64 a = ii > 0 ? fetch[static_cast<u32>(j * n + (fx ? i + 1 : i - 1))] : 0.0;
      const f64 b = jj > 0 ? fetch[static_cast<u32>((fz ? j + 1 : j - 1) * n + i)] : 0.0;
      const f64 up = (wx * a + wz * b) / (wx + wz);
      if (depth[at] > 0.0f) {
        fetch[at] = up + step;
        longest = std::max(longest, fetch[at]);
      } else {
        fetch[at] = up * decay;
      }
    }
  }
  for (u32 i = 0; i < count; ++i)
    out[i] = static_cast<f32>(fetch[i]);
  return longest;
}

// The foreshore slope of a body's waterline as one number, from its signed depth map's 16-bit
// texels (water.md, "The shore"; S94): the mean of the slope across every pair of neighbouring
// texels whose depths straddle zero, kept to [k_swash_slope_min, k_swash_slope_max]; the default
// where the map crosses nowhere. A texel's depth is texel / 65535 (depth_max + bank_max) -
// bank_max. (The renderer's GPU scene held this until S102; it is the body's number for the scales
// that are a body's — the lobes, a record with no surface — where a pixel's own stretch of shore
// is `water_shore_slope_field`'s.)
inline f64 water_mean_shore_slope(const u16* texels, u32 size, f64 texel_m, f64 depth_max,
                                  f64 bank_max) noexcept {
  if (size < 2 || !(texel_m > 0.0)) return k_swash_slope_default;
  const f64 range = depth_max + bank_max;
  const auto depth = [&](u32 i, u32 j) {
    return static_cast<f64>(texels[j * size + i]) / 65535.0 * range - bank_max;
  };
  f64 sum = 0.0;
  u64 count = 0;
  for (u32 j = 0; j < size; ++j) {
    for (u32 i = 0; i < size; ++i) {
      const f64 d = depth(i, j);
      if (i + 1 < size) {
        const f64 e = depth(i + 1, j);
        if ((d > 0.0) != (e > 0.0)) {
          sum += std::fabs(e - d);
          ++count;
        }
      }
      if (j + 1 < size) {
        const f64 e = depth(i, j + 1);
        if ((d > 0.0) != (e > 0.0)) {
          sum += std::fabs(e - d);
          ++count;
        }
      }
    }
  }
  if (count == 0) return k_swash_slope_default;
  return std::clamp(sum / static_cast<f64>(count) / texel_m, k_swash_slope_min, k_swash_slope_max);
}

// **The foreshore slope at every place** (roadmap S102; docs/experiments/shore-mechanics.md, "The
// swash is the slope's"): the slope of the ground where the still level meets it, at the nearest
// point of the waterline to each texel — so a place on the beach, and the water at its foot, know
// the slope *their* stretch of shore has, and the swash's reach and duration there are that
// stretch's, where S94 gave a whole body one number. The waterline's texels are the wet ones with a
// dry neighbour; each one's slope is the map's gradient there (central differences of the signed
// depth, which is continuous across the waterline), averaged over the waterline's texels within
// three of it, so the staircase a grid makes of a slanting shore is not read as a change of slope;
// every other texel takes the nearest one's by a two-pass 3-4 chamfer (integer distances, a fixed
// order). Kept to [k_swash_slope_min, k_swash_slope_max]; the default where the map crosses
// nowhere. `depth` and `out` are size * size, rows along +z.
inline void water_shore_slope_field(const f32* depth, u32 size, f64 texel_m, f32* out) noexcept {
  const u32 count = size * size;
  for (u32 i = 0; i < count; ++i)
    out[i] = static_cast<f32>(k_swash_slope_default);
  if (size < 2 || !(texel_m > 0.0)) return;
  const i64 n = size;
  const auto at = [&](i64 i, i64 j) {
    return static_cast<u32>(std::clamp<i64>(j, 0, n - 1) * n + std::clamp<i64>(i, 0, n - 1));
  };
  // The waterline's texels and the gradient at each.
  Vector<f64> raw(count);
  Vector<u8> seed(count);
  bool any = false;
  for (i64 j = 0; j < n; ++j) {
    for (i64 i = 0; i < n; ++i) {
      const u32 here = at(i, j);
      raw[here] = 0.0;
      seed[here] = 0;
      if (!(depth[here] > 0.0f)) continue;
      const bool dry_beside = !(depth[at(i - 1, j)] > 0.0f) || !(depth[at(i + 1, j)] > 0.0f) ||
                              !(depth[at(i, j - 1)] > 0.0f) || !(depth[at(i, j + 1)] > 0.0f);
      if (!dry_beside) continue;
      const f64 wx = static_cast<f64>(std::min<i64>(i + 1, n - 1) - std::max<i64>(i - 1, 0));
      const f64 wz = static_cast<f64>(std::min<i64>(j + 1, n - 1) - std::max<i64>(j - 1, 0));
      const f64 gx =
          (static_cast<f64>(depth[at(i + 1, j)]) - static_cast<f64>(depth[at(i - 1, j)])) /
          (wx * texel_m);
      const f64 gz =
          (static_cast<f64>(depth[at(i, j + 1)]) - static_cast<f64>(depth[at(i, j - 1)])) /
          (wz * texel_m);
      raw[here] = std::sqrt(gx * gx + gz * gz);
      seed[here] = 1;
      any = true;
    }
  }
  if (!any) return;
  // Each waterline texel's slope, averaged along the waterline; then the nearest one's everywhere.
  constexpr u32 k_none = 0xffffffffu;
  Vector<f64> slope(count);
  Vector<u32> distance(count);
  for (i64 j = 0; j < n; ++j) {
    for (i64 i = 0; i < n; ++i) {
      const u32 here = at(i, j);
      slope[here] = 0.0;
      distance[here] = k_none;
      if (seed[here] == 0) continue;
      f64 sum = 0.0;
      u32 hits = 0;
      for (i64 b = std::max<i64>(j - 3, 0); b <= std::min<i64>(j + 3, n - 1); ++b) {
        for (i64 a = std::max<i64>(i - 3, 0); a <= std::min<i64>(i + 3, n - 1); ++a) {
          const u32 there = static_cast<u32>(b * n + a);
          if (seed[there] == 0) continue;
          sum += raw[there];
          ++hits;
        }
      }
      slope[here] = sum / static_cast<f64>(hits);
      distance[here] = 0;
    }
  }
  water_carry_nearest(size, slope, distance);
  for (u32 i = 0; i < count; ++i)
    out[i] = static_cast<f32>(std::clamp(slope[i], k_swash_slope_min, k_swash_slope_max));
}

inline SwashWave swash_wave(f64 u10, f64 fetch_m) noexcept {
  SwashWave w;
  if (!(u10 > 0.0) || !(fetch_m > 0.0)) return w;
  const f64 g = k_swash_g;
  const f64 peak = jonswap_peak(u10, fetch_m);
  const f64 alpha = jonswap_alpha(u10, fetch_m);
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

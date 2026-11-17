#pragma once

// **One wind, and the sea it raises** (docs/subsystems/wind.md; the grounding and every number's
// source, docs/experiments/wind.md; roadmap W136). A scene's wind (`engine.scene.Wind`, under its
// `Sky`) is asked at a place and a time of the sky's clock (renderer/clocks.h: the weather follows
// the day), and every reader takes the same answer: the sea's and a lake's waves through the sea
// state below, the trees' sway through the gust.
//
// **The wind** (`wind_at`): the mean wind 10 m up — the ten-minute mean a station reports, which
// is what the wave-growth laws are written in — and where it blows to. A fixed wind is one number;
// a track keys it through a period and blends linearly between keys; a climate draws it from a
// seed as a smooth Gaussian process of the clock (knots `synoptic_h` apart, each a hashed standard
// normal, blended by a cosine and renormalised so every instant is N(0, 1)) mapped through the
// normal's distribution onto a Weibull distribution of speeds with the climate's mean and shape,
// a direction that wanders about the prevailing one the same way, and the day's cycle on top. Every
// one is a closed function of the time: no state, no frame count.
//
// **The gust** (`wind_gust`): the instantaneous wind at a place, the mean plus turbulence with
// Kaimal's spectrum (IEC 61400-1's form and length scale) at the intensity the ground's roughness
// gives, I = 1 / ln(z / z0), as a sum of sines on a fixed grid of frequencies whose phases travel
// down the wind (Taylor's frozen turbulence), so a gust crosses a scene as it does a field of
// grass.
//
// **The sea** (`sea_state`): the sea does not follow the wind at once. A wind sea grows with fetch
// and with duration by the JONSWAP laws the water's spectrum already draws (water_spectrum.h), and
// the coastal engineering manual's duration law (the time a sea takes to grow over a fetch), by the
// manual's equivalent-duration method stepped over the wind's history: each step re-expresses the
// sea's energy under the step's wind as an equivalent fetch, adds the step's duration, and caps the
// result at the fetch and at full development. Energy the wind can no longer hold — it dropped, or
// it turned by more than 45 degrees, or it fell under the wind that raises no waves — leaves the
// wind sea as swell with the period it had, and the swell drains out of the water at its group
// speed over the fetch. The steps are on a fixed grid of the clock and the history is a fixed
// window, so the state at t is a function of t alone: a capture at t is the same picture however it
// was reached. A fixed wind's sea is its fetch-limited sea, exactly the table the water drew before
// the scene had a wind.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/gfx/water_spectrum.h>

#include <algorithm>
#include <cmath>

namespace engine::gfx {

inline constexpr f64 k_wind_pi = 3.141592653589793;
inline constexpr f64 k_wind_hour_s = 3600.0;
inline constexpr f64 k_wind_day_s = 86400.0;
// The strongest wind a scene may name, m/s: past Beaufort 12's 32.7.
inline constexpr f64 k_wind_max_m_s = 60.0;
// The height the wind is said at, metres: the WMO's standard 10 m.
inline constexpr f64 k_wind_height_m = 10.0;

enum class WindKind : u8 { none = 0, fixed = 1, track = 2, climate = 3 };

struct WindKey {
  f64 hour = 0.0;
  f64 speed_m_s = 0.0;
  f64 to_deg = 0.0;
};
struct WindClimate {
  f64 mean_m_s = 6.0;
  f64 weibull_k = 2.0;
  f64 to_deg = 0.0;
  f64 veer_deg = 35.0;
  f64 synoptic_h = 48.0;
  f64 diurnal = 0.3;
  f64 peak_hour = 15.0;
  u32 seed = 1;
};
// A scene's wind as the renderer read it from `engine.scene.Wind` (the same numbers, plain, since
// gfx does not read scenes).
struct WindDesc {
  WindKind kind = WindKind::none;
  f64 speed_m_s = 0.0;
  f64 to_deg = 0.0;
  Vector<WindKey> track{};
  f64 period_h = 24.0;
  WindClimate climate{};
  f64 roughness_m = 0.03;
  bool varies() const noexcept { return kind == WindKind::track || kind == WindKind::climate; }
};

// The wind at a time: the ten-minute mean 10 m up and where it blows to (unit, over x and z).
struct WindSample {
  f64 speed_m_s = 0.0;
  Vec2 to{1.0f, 0.0f};
};

// In f32 as a water body's `direction_deg` is turned (scene_water.cpp, `shading_of`), so a scene
// wind and a body's own direction of the same degrees are the same bits.
inline Vec2 wind_unit(f64 to_deg) noexcept {
  const f32 a = radians(static_cast<f32>(to_deg));
  return Vec2{std::cos(a), std::sin(a)};
}

namespace wind_detail {
// A hashed standard normal for knot `knot` of stream `salt` (Box-Muller over two hashed uniforms).
inline f64 normal(u32 seed, i64 knot, u32 salt) noexcept {
  const u32 k = static_cast<u32>(static_cast<u64>(knot) & 0xffffffffu);
  const f64 u1 = water_wave_uniform(seed ^ 0x5bd1e995u, k, salt * 2u);
  const f64 u2 = water_wave_uniform(seed ^ 0x5bd1e995u, k, salt * 2u + 1u);
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * k_wind_pi * u2);
}
// The process at `hours`: knots `spacing_h` apart blended by a cosine and renormalised, so its
// value is N(0, 1) at every instant and smooth between knots.
inline f64 process(u32 seed, u32 salt, f64 hours, f64 spacing_h) noexcept {
  const f64 x = hours / spacing_h;
  const f64 f = std::floor(x);
  const f64 frac = x - f;
  const i64 i = static_cast<i64>(f);
  const f64 w1 = 0.5 - 0.5 * std::cos(k_wind_pi * frac);
  const f64 w0 = 1.0 - w1;
  return (w0 * normal(seed, i, salt) + w1 * normal(seed, i + 1, salt)) /
         std::sqrt(w0 * w0 + w1 * w1);
}
// The shorter way from degrees `a` to degrees `b`, in (-180, 180].
inline f64 turn_deg(f64 a, f64 b) noexcept {
  f64 d = std::fmod(b - a, 360.0);
  if (d > 180.0) d -= 360.0;
  if (d <= -180.0) d += 360.0;
  return d;
}
}  // namespace wind_detail

// The wind of a climate at sky time `time_s` (seconds since the world's epoch, which is midnight).
inline WindSample climate_wind_at(const WindClimate& c, f64 time_s) noexcept {
  const f64 hours = time_s / k_wind_hour_s;
  const f64 spacing = std::max(c.synoptic_h, 1.0);
  const f64 z = wind_detail::process(c.seed, 1u, hours, spacing);
  // The normal's distribution, then Weibull's inverse: λ (-ln(1 - u))^(1/k), λ = mean / Γ(1 + 1/k).
  const f64 u = std::clamp(0.5 * std::erfc(-z / std::sqrt(2.0)), 1e-12, 1.0 - 1e-12);
  const f64 k = std::clamp(c.weibull_k, 1.0, 4.0);
  const f64 lambda = c.mean_m_s / std::tgamma(1.0 + 1.0 / k);
  f64 speed = lambda * std::pow(-std::log(1.0 - u), 1.0 / k);
  // The day: a cosine about the weather's own strength, peaking at `peak_hour`.
  const f64 day_h = std::fmod(std::fmod(hours, 24.0) + 24.0, 24.0);
  speed *= 1.0 + std::clamp(c.diurnal, 0.0, 1.0) *
                     std::cos(2.0 * k_wind_pi * (day_h - c.peak_hour) / 24.0);
  const f64 dir = c.to_deg + c.veer_deg * wind_detail::process(c.seed, 2u, hours, spacing);
  return WindSample{std::clamp(speed, 0.0, k_wind_max_m_s), wind_unit(dir)};
}

// The wind of a track at sky time `time_s`: the keys' linear blend, wrapping over the period.
inline WindSample track_wind_at(const Vector<WindKey>& keys, f64 period_h, f64 time_s) noexcept {
  if (keys.empty()) return WindSample{0.0, Vec2{1.0f, 0.0f}};
  if (keys.size() == 1) return WindSample{keys[0].speed_m_s, wind_unit(keys[0].to_deg)};
  const f64 period = std::max(period_h, 1e-6);
  const f64 h = std::fmod(std::fmod(time_s / k_wind_hour_s, period) + period, period);
  u32 next = 0;
  while (next < keys.size() && keys[next].hour <= h)
    ++next;
  const WindKey& a = keys[next == 0 ? keys.size() - 1 : next - 1];
  const WindKey& b = keys[next == keys.size() ? 0 : next];
  f64 span = b.hour - a.hour;
  f64 into = h - a.hour;
  if (span <= 0.0) span += period;
  if (into < 0.0) into += period;
  const f64 t = span > 0.0 ? std::clamp(into / span, 0.0, 1.0) : 0.0;
  const f64 speed = a.speed_m_s + (b.speed_m_s - a.speed_m_s) * t;
  const f64 dir = a.to_deg + wind_detail::turn_deg(a.to_deg, b.to_deg) * t;
  return WindSample{speed, wind_unit(dir)};
}

// **The wind at a time** (the place does not change the mean: the weather is larger than a scene).
inline WindSample wind_at(const WindDesc& w, f64 time_s) noexcept {
  switch (w.kind) {
    case WindKind::fixed: return WindSample{w.speed_m_s, wind_unit(w.to_deg)};
    case WindKind::track: return track_wind_at(w.track, w.period_h, time_s);
    case WindKind::climate: return climate_wind_at(w.climate, time_s);
    case WindKind::none: break;
  }
  return WindSample{0.0, Vec2{1.0f, 0.0f}};
}

// The turbulence intensity over ground of roughness `z0` at 10 m: 1 / ln(z / z0) (the log law's
// neutral surface layer; Wieringa 1993's classes).
inline f64 wind_turbulence(f64 z0) noexcept {
  return 1.0 / std::log(k_wind_height_m / std::clamp(z0, 1e-5, 2.0));
}

// Kaimal's longitudinal spectrum in IEC 61400-1's form, S(f) = 4 σ² L / U / (1 + 6 f L / U)^(5/3),
// L = 8.1 Λ with Λ = 0.7 z below 60 m: 56.7 m at 10 m.
inline constexpr f64 k_wind_kaimal_length_m = 8.1 * 0.7 * k_wind_height_m;
inline constexpr u32 k_wind_gust_sines = 8;
inline constexpr f64 k_wind_gust_f_low = 0.004;  // Hz: a four-minute lull, inside the spectral gap
inline constexpr f64 k_wind_gust_f_high = 0.5;   // Hz: a two-second gust

// **The gust**: the instantaneous wind speed at `place` (x, z) and sky time `time_s`, for a wind
// whose mean there is `mean`: the mean plus Kaimal's turbulence frozen into the flow and carried
// down the wind at the mean speed. Never below zero.
inline f64 wind_gust(const WindDesc& w, const WindSample& mean, Vec2 place, f64 time_s) noexcept {
  const f64 u = mean.speed_m_s;
  if (!(u > 0.0)) return 0.0;
  const f64 sigma = wind_turbulence(w.roughness_m) * u;
  const f64 along = static_cast<f64>(place.x) * static_cast<f64>(mean.to.x) +
                    static_cast<f64>(place.y) * static_cast<f64>(mean.to.y);
  const f64 tau = time_s - along / std::max(u, 1.0);
  const f64 ratio = std::log(k_wind_gust_f_high / k_wind_gust_f_low);
  const u32 seed = w.kind == WindKind::climate ? w.climate.seed : 1u;
  f64 sum = 0.0;
  for (u32 j = 0; j < k_wind_gust_sines; ++j) {
    const f64 f0 = k_wind_gust_f_low * std::exp(ratio * static_cast<f64>(j) / k_wind_gust_sines);
    const f64 f1 =
        k_wind_gust_f_low * std::exp(ratio * static_cast<f64>(j + 1) / k_wind_gust_sines);
    const f64 f = std::sqrt(f0 * f1);
    const f64 n = f * k_wind_kaimal_length_m / u;
    const f64 s =
        4.0 * sigma * sigma * k_wind_kaimal_length_m / u / std::pow(1.0 + 6.0 * n, 5.0 / 3.0);
    const f64 a = std::sqrt(2.0 * s * (f1 - f0));
    const f64 phase = water_wave_uniform(seed ^ 0x2545f491u, j, 7u);
    sum += a * std::sin(2.0 * k_wind_pi * (f * tau + phase));
  }
  return std::max(u + sum, 0.0);
}

// ---- the sea ----------------------------------------------------------------------------------

// Under this wind no waves are raised (Kahma and Donelan 1988: the first wavelets at about 1 m/s).
inline constexpr f64 k_sea_min_wind_m_s = 1.0;
// A wind that turns by more than this leaves the old sea behind as swell (the manual's 45 degrees).
inline constexpr f64 k_sea_turn_cos = 0.70710678118654752;
// The equivalent-duration method's step and its history: a fixed grid of the clock, so the state is
// a function of the time alone. 72 h covers a gale's growth over 1,000 km (33 h) and a swell's
// drain over a 100 km fetch many times over.
inline constexpr f64 k_sea_step_s = 1800.0;
inline constexpr f64 k_sea_history_s = 72.0 * 3600.0;
// The coastal engineering manual's duration law (EM 1110-2-1100 Part II-2, eq. II-2-35): the time a
// sea under wind u takes to grow over fetch x, t = 77.23 x^0.67 / (u^0.34 g^0.33), SI.
inline f64 sea_duration_s(f64 u10, f64 fetch_m) noexcept {
  if (!(fetch_m > 0.0) || !(u10 > 0.0)) return 0.0;
  return 77.23 * std::pow(fetch_m, 0.67) /
         (std::pow(u10, 0.34) * std::pow(k_water_spectrum_g, 0.33));
}
inline f64 sea_fetch_of_duration(f64 u10, f64 duration_s) noexcept {
  if (!(duration_s > 0.0) || !(u10 > 0.0)) return 0.0;
  return std::pow(duration_s * std::pow(u10, 0.34) * std::pow(k_water_spectrum_g, 0.33) / 77.23,
                  1.0 / 0.67);
}
// JONSWAP integrated at unit Phillips constant and unit peak, over [0.3, 6] of the peak by the
// midpoint rule in 4,096 steps — the integral `swash_wave` takes, so a sea's height here is the
// shore's. A sea's variance is alpha * this / peak^4.
inline f64 sea_jonswap_unit() noexcept {
  static const f64 unit = [] {
    const u32 steps = 4096;
    const f64 dx = (6.0 - 0.3) / static_cast<f64>(steps);
    f64 m0 = 0.0;
    for (u32 i = 0; i < steps; ++i)
      m0 += jonswap(0.3 + (static_cast<f64>(i) + 0.5) * dx, 1.0, 1.0) * dx;
    return m0;
  }();
  return unit;
}
// The significant height of the wind sea of `u10` over `fetch_m` (capped at full development).
inline f64 sea_height(f64 u10, f64 fetch_m) noexcept {
  if (!(u10 > 0.0) || !(fetch_m > 0.0)) return 0.0;
  const f64 p = jonswap_peak(u10, fetch_m);
  return 4.0 * std::sqrt(jonswap_alpha(u10, fetch_m) * sea_jonswap_unit() / (p * p * p * p));
}
// The fetch over which `u10` raises a sea of height `height_m`, by the uncapped laws (the height
// goes as the fetch to the power 2/3 - 0.11).
inline f64 sea_fetch_for(f64 u10, f64 height_m) noexcept {
  if (!(u10 > 0.0) || !(height_m > 0.0)) return 0.0;
  const f64 g = k_water_spectrum_g;
  const f64 f1 = 1000.0;
  const f64 alpha = 0.076 * std::pow(u10 * u10 / (f1 * g), 0.22);
  const f64 p = 22.0 * std::cbrt(g * g / (u10 * f1));
  const f64 h1 = 4.0 * std::sqrt(alpha * sea_jonswap_unit() / (p * p * p * p));
  return f1 * std::pow(height_m / h1, 1.0 / (2.0 / 3.0 - 0.11));
}
inline f64 sea_group_speed(f64 period_s) noexcept {
  return k_water_spectrum_g * period_s / (4.0 * k_wind_pi);
}

struct SeaState {
  f64 wind_m_s = 0.0;  // the wind now
  Vec2 wind_to{1.0f, 0.0f};
  f64 sea_fetch_m = 0.0;  // the wind sea's equivalent fetch under the wind now; 0: none
  Vec2 sea_to{1.0f, 0.0f};
  f64 sea_height_m = 0.0;  // its significant height (Hm0)
  f64 sea_period_s = 0.0;  // its peak period
  f64 swell_height_m = 0.0;
  f64 swell_period_s = 0.0;
  Vec2 swell_to{1.0f, 0.0f};
  f64 height_m() const noexcept {
    return std::sqrt(sea_height_m * sea_height_m + swell_height_m * swell_height_m);
  }
};

namespace wind_detail {
inline void to_swell(SeaState& s, f64 height, f64 period, Vec2 to) noexcept {
  const f64 e0 = s.swell_height_m * s.swell_height_m;
  const f64 e1 = height * height;
  if (!(e1 > 0.0)) return;
  const f64 e = e0 + e1;
  s.swell_period_s = (e0 * s.swell_period_s + e1 * period) / e;
  const f64 x = e0 * static_cast<f64>(s.swell_to.x) + e1 * static_cast<f64>(to.x);
  const f64 z = e0 * static_cast<f64>(s.swell_to.y) + e1 * static_cast<f64>(to.y);
  const f64 len = std::sqrt(x * x + z * z);
  if (len > 0.0) s.swell_to = Vec2{static_cast<f32>(x / len), static_cast<f32>(z / len)};
  s.swell_height_m = std::sqrt(e);
}
}  // namespace wind_detail

// One step of the history: `dt` seconds of wind `w` over water whose fetch is `fetch_m`.
inline void sea_step(SeaState& s, const WindSample& w, f64 dt, f64 fetch_m) noexcept {
  const f64 u = w.speed_m_s;
  const bool blows = u >= k_sea_min_wind_m_s && fetch_m > 0.0;
  // The sea the wind no longer drives becomes swell.
  if (s.sea_height_m > 0.0) {
    const f64 c = static_cast<f64>(s.sea_to.x) * static_cast<f64>(w.to.x) +
                  static_cast<f64>(s.sea_to.y) * static_cast<f64>(w.to.y);
    if (!blows || c < k_sea_turn_cos) {
      wind_detail::to_swell(s, s.sea_height_m, s.sea_period_s, s.sea_to);
      s.sea_height_m = 0.0;
      s.sea_period_s = 0.0;
      s.sea_fetch_m = 0.0;
    }
  }
  // The swell drains out of the water at its group speed: energy e-folds over fetch / c_g.
  if (s.swell_height_m > 0.0) {
    const f64 tau = std::max(fetch_m, 1.0) / sea_group_speed(std::max(s.swell_period_s, 0.5));
    s.swell_height_m *= std::exp(-0.5 * dt / tau);
    if (s.swell_height_m < 1e-3) s.swell_height_m = 0.0;
  }
  s.wind_m_s = u;
  s.wind_to = w.to;
  if (!blows) return;
  const f64 cap = water_grown_fetch(u, fetch_m);
  f64 fe = 0.0;
  if (s.sea_height_m > 0.0) {
    fe = sea_fetch_for(u, s.sea_height_m);
    if (fe > cap) {
      const f64 held = sea_height(u, cap);
      const f64 excess = s.sea_height_m * s.sea_height_m - held * held;
      if (excess > 0.0) wind_detail::to_swell(s, std::sqrt(excess), s.sea_period_s, s.sea_to);
      fe = cap;
    }
  }
  fe = std::min(sea_fetch_of_duration(u, sea_duration_s(u, fe) + dt), cap);
  s.sea_fetch_m = fe;
  s.sea_to = w.to;
  s.sea_height_m = sea_height(u, fe);
  s.sea_period_s = fe > 0.0 ? 2.0 * k_wind_pi / jonswap_peak(u, fe) : 0.0;
}

// **The sea state at sky time `time_s`** over water of fetch `fetch_m`: the steps of the history
// on the clock's fixed grid, the last one partial, each at the wind of its middle. A fixed wind's
// is its fetch-limited sea, without stepping.
inline SeaState sea_state(const WindDesc& w, f64 time_s, f64 fetch_m) noexcept {
  SeaState s;
  if (w.kind == WindKind::none) return s;
  if (!w.varies()) {
    const WindSample now = wind_at(w, time_s);
    s.wind_m_s = now.speed_m_s;
    s.wind_to = now.to;
    if (now.speed_m_s >= k_sea_min_wind_m_s && fetch_m > 0.0) {
      s.sea_fetch_m = water_grown_fetch(now.speed_m_s, fetch_m);
      s.sea_to = now.to;
      s.sea_height_m = sea_height(now.speed_m_s, s.sea_fetch_m);
      s.sea_period_s = 2.0 * k_wind_pi / jonswap_peak(now.speed_m_s, s.sea_fetch_m);
    }
    return s;
  }
  const f64 grid = std::floor(time_s / k_sea_step_s) * k_sea_step_s;
  f64 a = grid - k_sea_history_s;
  while (a < time_s) {
    const f64 b = std::min(a + k_sea_step_s, time_s);
    sea_step(s, wind_at(w, 0.5 * (a + b)), b - a, fetch_m);
    a = b;
  }
  const WindSample now = wind_at(w, time_s);
  s.wind_m_s = now.speed_m_s;
  s.wind_to = now.to;
  return s;
}

// ---- the sea as the water's table ---------------------------------------------------------------

// **The bands a body's table is drawn on**, radians a second: a fixed wind's is [0.5, 4] of its
// peak (exactly `water_spectrum`'s); a varying wind's spans every state the wind can raise, so the
// bands never move and no wave's phase jumps as the sea grows.
struct WaterWaveGrid {
  f64 lo = 0.0;
  f64 hi = 0.0;
  bool valid() const noexcept { return hi > lo && lo > 0.0; }
};
// The widest a grid may be (a band's width ln(32) / 16 = 0.22 against a fixed wind's 0.13).
inline constexpr f64 k_water_grid_max_ratio = 32.0;

// The table for `s` on `grid`, the phases and spreads from `seed`. With no swell and `grid` the
// fixed wind's this is `water_spectrum(s.wind_m_s, fetch, s.sea_to, seed)` to the bit.
inline void water_spectrum_on_grid(const SeaState& s, WaterWaveGrid grid, u32 seed,
                                   WaterWave (&out)[k_water_waves]) noexcept {
  for (WaterWave& w : out)
    w = WaterWave{};
  if (!grid.valid()) return;
  const bool sea = s.sea_height_m > 0.0 && s.sea_fetch_m > 0.0 && s.wind_m_s > 0.0;
  const bool swell = s.swell_height_m > 0.0 && s.swell_period_s > 0.0;
  const f64 peak = sea ? jonswap_peak(s.wind_m_s, s.sea_fetch_m) : 0.0;
  const f64 alpha = sea ? jonswap_alpha(s.wind_m_s, s.sea_fetch_m) : 0.0;
  const f64 swell_peak = swell ? 2.0 * k_wind_pi / s.swell_period_s : 0.0;
  // The swell's spectrum: JONSWAP's shape at its own peak, scaled to its variance H^2 / 16.
  const f64 swell_alpha = swell ? s.swell_height_m * s.swell_height_m / 16.0 *
                                      (swell_peak * swell_peak * swell_peak * swell_peak) /
                                      sea_jonswap_unit()
                                : 0.0;
  const f64 lo = std::log(grid.lo);
  const f64 step = (std::log(grid.hi) - lo) / static_cast<f64>(k_water_waves);
  const f64 two_pi = 6.283185307179586;
  f64 steepness = 0.0;
  f64 ka[k_water_waves];
  for (u32 i = 0; i < k_water_waves; ++i) {
    const f64 a0 = std::exp(lo + step * static_cast<f64>(i));
    const f64 a1 = std::exp(lo + step * static_cast<f64>(i + 1));
    const f64 omega = std::sqrt(a0 * a1);
    const f64 s_sea = sea ? jonswap(omega, alpha, peak) : 0.0;
    const f64 s_swell = swell ? jonswap(omega, swell_alpha, swell_peak) : 0.0;
    f64 density = s_sea;
    if (swell) density += s_swell;
    const f64 amplitude = std::sqrt(2.0 * density * (a1 - a0));
    const f64 k = omega * omega / k_water_spectrum_g;
    // The band's direction and spread: the wind sea's where it holds the band, the swell's where
    // the swell does (Donelan-Banner about each one's own peak).
    const bool swell_band = swell && (!sea || s_swell > s_sea);
    const Vec2 about = swell_band ? s.swell_to : s.sea_to;
    const f64 beta = donelan_banner_beta(omega / (swell_band   ? swell_peak
                                                  : peak > 0.0 ? peak
                                                               : omega));
    const f64 u = water_wave_uniform(seed, i, 1);
    const f64 theta = std::atanh(std::tanh(beta * 3.141592653589793) * (2.0 * u - 1.0)) / beta;
    const f64 wx = static_cast<f64>(about.x);
    const f64 wz = static_cast<f64>(about.y);
    const f64 c = std::cos(theta);
    const f64 sn = std::sin(theta);
    const f64 dx = wx * c - wz * sn;
    const f64 dz = wx * sn + wz * c;
    const f64 len = std::sqrt(dx * dx + dz * dz);
    WaterWave& w = out[i];
    w.direction = Vec2{static_cast<f32>(dx / len), static_cast<f32>(dz / len)};
    w.k = static_cast<f32>(k);
    w.amplitude = static_cast<f32>(amplitude);
    w.cycles = static_cast<f32>(std::max(1.0, std::floor(omega * k_water_period_s / two_pi + 0.5)));
    w.phase = static_cast<f32>(water_wave_uniform(seed, i, 0));
    if (sea) {
      const f64 q = peak / omega;
      w.grow = static_cast<f32>(0.625 * q * q * q * q);
    }
    ka[i] = k * amplitude;
    steepness += ka[i];
  }
  const f64 scale = steepness > k_water_max_steepness ? k_water_max_steepness / steepness : 1.0;
  for (u32 i = 0; i < k_water_waves; ++i)
    out[i].steepness = static_cast<f32>(ka[i] * scale);
}

// What a varying wind raises over `fetch_m`, sampled over its period (a track) or sixty days (a
// climate) every `step_s`: the grid every state fits (the longest peak's half to the youngest
// sea's fourfold, no wider than `k_water_grid_max_ratio`, keeping the long end) and the largest
// height, for the instance's bounds. A fixed wind's grid is its own peak's.
struct WaterWindRange {
  WaterWaveGrid grid{};
  f64 max_height_m = 0.0;
  f64 max_period_s = 0.0;  // the period of the state with the largest height
  SeaState max_state{};    // that state
  f64 max_wind_m_s = 0.0;  // the strongest wind sampled
};
inline WaterWindRange water_wind_range(const WindDesc& w, f64 fetch_m) noexcept {
  WaterWindRange r;
  f64 lo = 0.0;
  f64 hi = 0.0;
  const auto take = [&](const SeaState& s) {
    if (s.sea_height_m > 0.01) {
      const f64 p = jonswap_peak(s.wind_m_s, s.sea_fetch_m);
      lo = lo > 0.0 ? std::min(lo, p) : p;
      hi = std::max(hi, p);
    }
    if (s.swell_height_m > 0.01) {
      const f64 p = 2.0 * k_wind_pi / s.swell_period_s;
      lo = lo > 0.0 ? std::min(lo, p) : p;
      hi = std::max(hi, p);
    }
    r.max_wind_m_s = std::max(r.max_wind_m_s, s.wind_m_s);
    if (s.height_m() > r.max_height_m) {
      r.max_state = s;
      r.max_height_m = s.height_m();
      r.max_period_s = s.sea_height_m >= s.swell_height_m ? s.sea_period_s : s.swell_period_s;
    }
  };
  if (!w.varies()) {
    take(sea_state(w, 0.0, fetch_m));
  } else {
    const f64 span =
        w.kind == WindKind::track ? std::max(w.period_h, 1.0) * k_wind_hour_s : 60.0 * k_wind_day_s;
    const u32 samples = 240;
    for (u32 i = 0; i < samples; ++i)
      take(sea_state(w, span * (static_cast<f64>(i) + 0.5) / samples, fetch_m));
  }
  if (lo > 0.0) {
    r.grid.lo = k_water_band_low * lo;
    r.grid.hi = std::min(k_water_band_high * hi, k_water_grid_max_ratio * r.grid.lo);
    if (!(r.grid.hi > r.grid.lo)) r.grid.hi = k_water_band_high * lo;
  }
  return r;
}

}  // namespace engine::gfx

// The scene's wind and the sea it raises (wind.h; docs/subsystems/gfx.md, "The wind a scene names";
// the grounding, docs/experiments/wind.md; roadmap W136): no device. The numbers checked here are
// the page's.

#include <domain/gfx/water_spectrum.h>
#include <domain/gfx/water_swash.h>
#include <domain/gfx/wind.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace engine;
using namespace engine::gfx;

namespace {

WindDesc fixed_wind(f64 speed, f64 to_deg) {
  WindDesc w;
  w.kind = WindKind::fixed;
  w.speed_m_s = speed;
  w.to_deg = to_deg;
  return w;
}

// Calm until 06:00, a gale by 12:00 (20 m/s), held to 18:00, calm again by midnight.
WindDesc rising_wind() {
  WindDesc w;
  w.kind = WindKind::track;
  w.period_h = 24.0;
  w.track.push_back(WindKey{0.0, 0.0, 205.0});
  w.track.push_back(WindKey{6.0, 0.0, 205.0});
  w.track.push_back(WindKey{12.0, 20.0, 205.0});
  w.track.push_back(WindKey{18.0, 20.0, 205.0});
  w.track.push_back(WindKey{23.0, 0.0, 205.0});
  return w;
}

}  // namespace

TEST_CASE("wind: a fixed wind's sea is the table the body's own wind drew, to the bit") {
  // The beach's 8 m/s over 60 km and the forest lake's 7 m/s over 0.9 km.
  const f64 cases[][3] = {{8.0, 60000.0, 205.0}, {7.0, 900.0, 30.0}, {20.0, 300000.0, -90.0}};
  for (const auto& c : cases) {
    const WindDesc w = fixed_wind(c[0], c[2]);
    const SeaState s = sea_state(w, 12345.0, c[1]);
    const WaterWindRange range = water_wind_range(w, c[1]);
    WaterWave grid[k_water_waves];
    water_spectrum_on_grid(s, range.grid, 11u, grid);
    WaterWave reference[k_water_waves];
    water_spectrum(c[0], c[1], wind_unit(c[2]), 11u, reference);
    CHECK(std::memcmp(grid, reference, sizeof(grid)) == 0);
    // The height the shore reads is `swash_wave`'s.
    const SwashWave shore = swash_wave(c[0], c[1]);
    CHECK(std::abs(s.sea_height_m - shore.height_m) < 1e-9 * shore.height_m + 1e-12);
  }
}

// **The fully developed sea is Pierson and Moskowitz's** (W136-2; wind.md, "Full development"):
// past the developed fetch the spectrum is theirs — the peak at f_p U / g = 0.13 and the shape
// Lewis and Allos's refit gives there (γ 1.02, α 0.0081) — so the height is their integral,
// 4 sqrt(α / 5) / (2π 0.13)² U² / g = 0.2413 U² / g, within the band's few parts in a thousand and
// γ's 1.6 %; until W136-2 it was 0.329. A young sea's height is JONSWAP's measured energy law,
// g H / U² = 1.6e-3 (g F / U²)^0.5, within the two fits' disagreement (the table: 4 % at most from
// χ = 10 to 1,000). The table's inverse is its own.
TEST_CASE("wind: the fully developed sea is Pierson and Moskowitz's, and a young one JONSWAP's") {
  const f64 g = k_water_spectrum_g;
  const f64 pm = 4.0 * std::sqrt(0.0081 / 5.0) / std::pow(2.0 * k_wind_pi * 0.13, 2.0);
  for (const f64 u : {5.0, 10.0, 20.0, 30.0}) {
    const f64 developed = sea_height(u, 1e9) * g / (u * u);
    MESSAGE(u << " m/s, fully developed: g H / U^2 = " << developed << " (Pierson-Moskowitz " << pm
              << ")");
    CHECK(developed == doctest::Approx(pm).epsilon(0.01));
    CHECK(jonswap_nu(u, 1e9) == doctest::Approx(k_water_developed_nu).epsilon(1e-9));
    const JonswapShape shape = jonswap_shape(u, 1e9);
    CHECK(shape.alpha == doctest::Approx(0.0081).epsilon(0.01));
    CHECK(shape.gamma == doctest::Approx(1.0).epsilon(0.02));
  }
  for (const f64 chi : {10.0, 100.0, 1000.0}) {
    const f64 u = 10.0;
    const f64 fetch = chi * u * u / g;
    const f64 h = sea_height(u, fetch) * g / (u * u);
    const f64 law = 1.6e-3 * std::sqrt(chi);
    MESSAGE("chi " << chi << ": g H / U^2 = " << h << " against JONSWAP's energy law " << law);
    CHECK(h == doctest::Approx(law).epsilon(0.05));
    // The table read against the integral it was made from.
    CHECK(h == doctest::Approx(jonswap_height_of_chi(chi)).epsilon(1e-5));
    // Its inverse.
    CHECK(sea_fetch_for(u, sea_height(u, fetch)) == doctest::Approx(fetch).epsilon(1e-9));
  }
  // A sea higher than the wind holds fully developed answers past its developed fetch.
  CHECK(sea_fetch_for(10.0, 4.0) > k_water_developed_fetch * 100.0 / g);
}

TEST_CASE("wind: a track blends its keys and wraps over its period") {
  const WindDesc w = rising_wind();
  CHECK(wind_at(w, 3.0 * 3600.0).speed_m_s == doctest::Approx(0.0));
  CHECK(wind_at(w, 9.0 * 3600.0).speed_m_s == doctest::Approx(10.0));
  CHECK(wind_at(w, 15.0 * 3600.0).speed_m_s == doctest::Approx(20.0));
  CHECK(wind_at(w, 23.5 * 3600.0).speed_m_s == doctest::Approx(0.0));
  CHECK(wind_at(w, (24.0 + 9.0) * 3600.0).speed_m_s == doctest::Approx(10.0));
  CHECK(wind_at(w, -15.0 * 3600.0).speed_m_s == doctest::Approx(10.0));  // 09:00 the day before
  // The direction by the shorter way round: 350 to 10 passes through 0.
  WindDesc turn;
  turn.kind = WindKind::track;
  turn.track.push_back(WindKey{0.0, 5.0, 350.0});
  turn.track.push_back(WindKey{12.0, 5.0, 10.0});
  const WindSample mid = wind_at(turn, 6.0 * 3600.0);
  CHECK(mid.to.x == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(std::abs(mid.to.y) < 1e-6f);
}

TEST_CASE("wind: a climate has its mean and its Weibull spread, and is a function of the time") {
  WindDesc w;
  w.kind = WindKind::climate;
  w.climate.mean_m_s = 7.0;
  w.climate.diurnal = 0.0;
  w.climate.synoptic_h = 24.0;
  w.climate.seed = 3;
  f64 sum = 0.0, sum2 = 0.0;
  const u32 n = 20000;
  for (u32 i = 0; i < n; ++i) {
    const f64 u = wind_at(w, (static_cast<f64>(i) * 7.3 + 0.5) * 3600.0).speed_m_s;
    sum += u;
    sum2 += u * u;
  }
  const f64 mean = sum / n;
  const f64 sd = std::sqrt(sum2 / n - mean * mean);
  // Rayleigh (k = 2): the standard deviation is 0.523 of the mean.
  MESSAGE("climate mean " << mean << " sd " << sd);
  CHECK(mean == doctest::Approx(7.0).epsilon(0.05));
  CHECK(sd / mean == doctest::Approx(0.523).epsilon(0.08));
  // The same time, the same wind; a continuous process (no step at a knot).
  CHECK(wind_at(w, 86400.0 * 3.0).speed_m_s == wind_at(w, 86400.0 * 3.0).speed_m_s);
  const f64 knot = 24.0 * 3600.0 * 5.0;
  CHECK(std::abs(wind_at(w, knot - 1.0).speed_m_s - wind_at(w, knot + 1.0).speed_m_s) < 0.05);
}

TEST_CASE("wind: the gust has the turbulence the ground's roughness gives") {
  WindDesc w = fixed_wind(10.0, 0.0);
  w.roughness_m = 0.03;
  const WindSample mean = wind_at(w, 0.0);
  f64 sum = 0.0, sum2 = 0.0;
  const u32 n = 40000;
  for (u32 i = 0; i < n; ++i) {
    const f64 g = wind_gust(w, mean, Vec2{0.0f, 0.0f}, static_cast<f64>(i) * 0.25);
    sum += g;
    sum2 += g * g;
  }
  const f64 m = sum / n;
  const f64 sd = std::sqrt(sum2 / n - m * m);
  // I = 1 / ln(10 / 0.03) = 0.172; the sines carry the band [0.004, 0.5] Hz of Kaimal's spectrum,
  // which at 10 m/s is 0.77 of its variance: 0.151 of the mean.
  MESSAGE("gust mean " << m << " sd " << sd);
  CHECK(m == doctest::Approx(10.0).epsilon(0.03));
  CHECK(sd / 10.0 > 0.13);
  CHECK(sd / 10.0 < 0.18);
  // Frozen turbulence: the gust at x downwind arrives x / U later.
  CHECK(wind_gust(w, mean, Vec2{50.0f, 0.0f}, 105.0) ==
        doctest::Approx(wind_gust(w, mean, Vec2{0.0f, 0.0f}, 100.0)).epsilon(1e-9));
}

TEST_CASE("wind: the sea grows with duration to the fetch's sea, and not past it") {
  const f64 fetch = 60000.0;
  const WindDesc w = rising_wind();
  const auto at = [&](f64 hours) { return sea_state(w, hours * 3600.0, fetch); };
  // Calm: no wind sea (the evening's swell is still there: below).
  CHECK(at(5.0).sea_height_m == 0.0);
  // Rising: younger and lower than the fetch-limited sea of the same wind.
  const SeaState rising = at(10.0);
  const f64 fetch_limited = sea_height(rising.wind_m_s, fetch);
  MESSAGE("10:00 wind " << rising.wind_m_s << " Hs " << rising.sea_height_m << " Tp "
                        << rising.sea_period_s << " against the fetch's " << fetch_limited);
  CHECK(rising.sea_height_m > 0.1);
  CHECK(rising.sea_height_m < fetch_limited);
  // Held at 20 m/s from 12:00, by 18:00 the sea is the fetch's (the manual's duration over 60 km at
  // 20 m/s is about 5.8 h).
  const SeaState held = at(18.0);
  CHECK(held.sea_height_m == doctest::Approx(sea_height(20.0, fetch)).epsilon(1e-6));
  MESSAGE("18:00 Hs " << held.sea_height_m << " Tp " << held.sea_period_s);
  // Never past the fetch-limited sea of the wind it grew under (the last step's is the wind of
  // its middle, up to half a step before the time asked, so a falling wind's sea is that one's).
  for (f64 h = 0.0; h < 48.0; h += 0.25) {
    const SeaState s = at(h);
    const f64 u = std::max(s.wind_m_s, wind_at(w, h * 3600.0 - k_sea_step_s).speed_m_s);
    if (s.sea_height_m > 0.0) CHECK(s.sea_height_m <= sea_height(u, fetch) * (1.0 + 1e-9));
  }
}

TEST_CASE("wind: the sea outlives its wind as swell, which drains away") {
  const f64 fetch = 60000.0;
  const WindDesc w = rising_wind();
  const SeaState gale = sea_state(w, 18.0 * 3600.0, fetch);
  const SeaState after = sea_state(w, 23.5 * 3600.0, fetch);  // calm since 23:00
  MESSAGE("23:30 swell " << after.swell_height_m << " m at " << after.swell_period_s << " s");
  CHECK(after.sea_height_m == 0.0);
  CHECK(after.swell_height_m > 0.3 * gale.sea_height_m);
  CHECK(after.swell_period_s > 0.5 * gale.sea_period_s);
  // It drains: its energy e-folds over the fetch at its group speed (60 km at 6.6 s: 3.2 h), so by
  // 05:00 its height is about e^(-5.5 / 6.4) of 23:30's, and the calm has a residual swell.
  const SeaState dawn = sea_state(w, 29.0 * 3600.0, fetch);
  MESSAGE("05:00 swell " << dawn.swell_height_m << " m");
  CHECK(dawn.sea_height_m == 0.0);
  CHECK(dawn.swell_height_m < 0.55 * after.swell_height_m);
  CHECK(dawn.swell_height_m > 0.25 * after.swell_height_m);
  // A lake (fetch 900 m) loses its swell in minutes: from the calm's first step (23:00) its height
  // falls by exp(-dt / 2 tau) a step, tau = 900 m over the group speed of the swell's own period
  // (about a second: the falling wind's last seas), so three steps on it is that to the third.
  const SeaState calm_lake = sea_state(w, 23.0 * 3600.0, 900.0);
  const f64 tau = 900.0 / sea_group_speed(calm_lake.swell_period_s);
  const f64 lake_after = sea_state(w, 24.5 * 3600.0, 900.0).height_m();
  const f64 expected = calm_lake.height_m() * std::exp(-0.5 * 3.0 * k_sea_step_s / tau);
  MESSAGE("lake: 23:00 " << calm_lake.height_m() << " m at " << calm_lake.swell_period_s
                         << " s (tau " << tau << " s), 00:30 " << lake_after << " m");
  // (Within a tenth: the first calm step merges the last wind sea, 1 m/s's, into the swell, which
  // moves its period and so its tau a little.)
  CHECK(lake_after == doctest::Approx(expected).epsilon(0.1));
  CHECK(lake_after < 0.05 * sea_state(w, 18.0 * 3600.0, 900.0).sea_height_m);
}

TEST_CASE("wind: the sea state is a function of the time, continuous across the steps") {
  const WindDesc w = rising_wind();
  const f64 fetch = 60000.0;
  for (f64 t = 7.0 * 3600.0; t < 14.0 * 3600.0; t += 1800.0) {
    const SeaState a = sea_state(w, t - 0.5, fetch);
    const SeaState b = sea_state(w, t + 0.5, fetch);
    CHECK(std::abs(a.height_m() - b.height_m()) < 2e-3 * std::max(a.height_m(), 0.01));
    // And its peak (W136-2: the equivalent fetch is the wind now's, not the last step's).
    CHECK(std::abs(a.sea_period_s - b.sea_period_s) < 2e-3 * std::max(a.sea_period_s, 0.1));
  }
  const SeaState x = sea_state(w, 40000.0, fetch);
  const SeaState y = sea_state(w, 40000.0, fetch);
  CHECK(std::memcmp(&x, &y, sizeof(x)) == 0);
}

TEST_CASE("wind: a varying wind's table is on one grid, and its waves follow the state") {
  const WindDesc w = rising_wind();
  const f64 fetch = 60000.0;
  const WaterWindRange range = water_wind_range(w, fetch);
  REQUIRE(range.grid.valid());
  MESSAGE("grid " << range.grid.lo << " to " << range.grid.hi << " rad/s, the largest sea "
                  << range.max_height_m << " m at " << range.max_period_s << " s");
  CHECK(range.grid.hi / range.grid.lo <= k_water_grid_max_ratio * (1.0 + 1e-12));
  WaterWave calm[k_water_waves];
  WaterWave gale[k_water_waves];
  water_spectrum_on_grid(SeaState{}, range.grid, 11u, calm);  // glass: no sea, no swell
  water_spectrum_on_grid(sea_state(w, 18.0 * 3600.0, fetch), range.grid, 11u, gale);
  f64 m0 = 0.0;
  for (u32 i = 0; i < k_water_waves; ++i) {
    CHECK(calm[i].amplitude == 0.0f);
    // The bands never move: the same wavenumber, cycles and phase at every state.
    CHECK(calm[i].k == gale[i].k);
    CHECK(calm[i].cycles == gale[i].cycles);
    CHECK(calm[i].phase == gale[i].phase);
    m0 += 0.5 * static_cast<f64>(gale[i].amplitude) * static_cast<f64>(gale[i].amplitude);
  }
  // The bands hold the sea's variance within a fifth (the grid is coarser than a fixed wind's).
  const f64 hs = 4.0 * std::sqrt(m0);
  MESSAGE("gale table Hs " << hs << " against " << sea_height(20.0, fetch));
  CHECK(hs == doctest::Approx(sea_height(20.0, fetch)).epsilon(0.2));
}

// ---- the swell a scene names (W136-3; gfx.md, "A swell a scene names") ------------------------

namespace {
// The beach's 8 m/s over 60 km beside a 4 m, 14 s swell from 1,000 km travelling to 200 degrees.
WindDesc swell_day(f64 wind_m_s) {
  WindDesc w = fixed_wind(wind_m_s, 205.0);
  if (wind_m_s <= 0.0) w.kind = WindKind::fixed;
  w.swell.kind = SwellKind::fixed;
  w.swell.height_m = 4.0;
  w.swell.period_s = 14.0;
  w.swell.to_deg = 200.0;
  w.swell.distance_km = 1000.0;
  return w;
}
f64 table_hs(const WaterWave (&t)[k_water_waves]) {
  f64 m0 = 0.0;
  for (const WaterWave& w : t)
    m0 += 0.5 * static_cast<f64>(w.amplitude) * static_cast<f64>(w.amplitude);
  return 4.0 * std::sqrt(m0);
}
}  // namespace

TEST_CASE("wind: a named swell is in the sea state and the table, at its own period") {
  const f64 fetch = 60000.0;
  for (const f64 wind : {0.0, 8.0, 20.0}) {
    const WindDesc w = swell_day(wind);
    const SeaState s = sea_state(w, 3600.0 * 15.0, fetch);
    CHECK(s.groundswell_height_m == 4.0);
    CHECK(s.groundswell_period_s == 14.0);
    CHECK(s.groundswell_s == doctest::Approx(25.0 + 50.0 * std::log10(2.0)));
    const f64 want = std::sqrt(16.0 + s.sea_height_m * s.sea_height_m);
    CHECK(s.height_m() == doctest::Approx(want));
    const WaterWindRange range = water_wind_range(w, fetch);
    REQUIRE(range.grid.valid());
    CHECK(range.max_groundswell_m == 4.0);
    CHECK(range.grid.hi / range.grid.lo <= k_water_grid_max_ratio * (1.0 + 1e-9));
    WaterWave t[k_water_waves];
    water_spectrum_on_grid(s, range.grid, 11u, t);
    // The bands hold the two seas' variance (the swell integrated over each band) within 5 %.
    const f64 hs = table_hs(t);
    MESSAGE(wind << " m/s beside 4 m at 14 s: table Hs " << hs << " against " << want << ", grid "
                 << range.grid.lo << " to " << range.grid.hi);
    CHECK(hs == doctest::Approx(want).epsilon(0.05));
    // The swell's band stands at its period exactly (the grid slid onto it), along its direction.
    u32 top = 0;
    for (u32 i = 1; i < k_water_waves; ++i)
      if (t[i].amplitude > t[top].amplitude) top = i;
    const f64 step = std::log(range.grid.hi / range.grid.lo) / k_water_waves;
    const f64 centre = range.grid.lo * std::exp(step * (top + 0.5));
    CHECK(centre == doctest::Approx(2.0 * k_wind_pi / 14.0).epsilon(1e-9));
    const Vec2 to = wind_unit(200.0);
    // Within three of its spread's standard deviations (16 degrees at 1,000 km's s of 40).
    const f32 len = std::sqrt(t[top].direction.x * t[top].direction.x +
                              t[top].direction.y * t[top].direction.y);
    CHECK((t[top].direction.x * to.x + t[top].direction.y * to.y) / len > 0.75f);
  }
  // No swell named: the table is the wind's, to the bit (the case above this one's first).
  WindDesc none = fixed_wind(8.0, 205.0);
  WaterWave a[k_water_waves];
  water_spectrum_on_grid(sea_state(none, 0.0, fetch), water_wind_range(none, fetch).grid, 11u, a);
  WaterWave b[k_water_waves];
  water_spectrum(8.0, fetch, wind_unit(205.0), 11u, b);
  CHECK(std::memcmp(a, b, sizeof(a)) == 0);
}

TEST_CASE("wind: a swell's track disperses, and its spread narrows with distance") {
  // A storm 3,000 km away: the period arriving t hours after its first waves is 4 pi D / (g t),
  // keyed every six hours from 17 s down; the blend is linear in frequency, which is the
  // dispersion's own law (f = g t / 4 pi D), so between two keys the period is the formula's.
  SwellDesc s;
  s.kind = SwellKind::track;
  s.period_h = 48.0;
  const f64 d = 3.0e6;
  const f64 t0 = 4.0 * k_wind_pi * d / (9.81 * 17.0);
  for (u32 k = 0; k < 8; ++k) {
    const f64 t = t0 + 6.0 * 3600.0 * k;
    s.track.push_back(SwellKey{6.0 * k, 3.0, 4.0 * k_wind_pi * d / (9.81 * t), 180.0});
  }
  for (const f64 h : {3.0, 9.0, 27.0}) {
    const SwellSample at = swell_at(s, h * 3600.0);
    const f64 want = 4.0 * k_wind_pi * d / (9.81 * (t0 + h * 3600.0));
    MESSAGE(h << " h: " << at.period_s << " s against the dispersion's " << want);
    CHECK(at.period_s == doctest::Approx(want).epsilon(1e-9));
  }
  CHECK(swell_spread_s(100.0) == doctest::Approx(25.0));
  CHECK(swell_spread_s(5000.0) == doctest::Approx(75.0));
  CHECK(swell_spread_s(0.0) == doctest::Approx(swell_spread_s(k_swell_default_km)));
  // sech²'s standard deviation π / (β √12) against cos^(2s)(θ/2)'s √(2 / s).
  for (const f64 sp : {25.0, 75.0}) {
    const f64 beta = spread_beta_of_s(sp);
    CHECK(k_wind_pi / (beta * std::sqrt(12.0)) == doctest::Approx(std::sqrt(2.0 / sp)));
  }
}

TEST_CASE("wind: the breaker a swell makes is the surf similarity's") {
  // Battjes 1974: 4 m at 14 s spills on a 1:30 beach and plunges on a 1:5 shingle bank.
  const f64 gentle = surf_similarity(1.0 / 30.0, 4.0, 14.0);
  const f64 steep = surf_similarity(0.2, 4.0, 14.0);
  MESSAGE("xi 1:30 " << gentle << ", 1:5 " << steep);
  CHECK(gentle == doctest::Approx(0.292).epsilon(0.01));
  CHECK(breaker_type(gentle) == BreakerType::spilling);
  CHECK(breaker_type(steep) == BreakerType::plunging);
  CHECK(breaker_type(surf_similarity(0.5, 0.5, 14.0)) == BreakerType::surging);
}

// **The live wind's arithmetic** (W151; gfx.md, "The live wind"; the wind page, "A gust on
// demand"): the Beaufort speeds the keys step through sit inside their forces' bands (the scale's
// table: force 8 is 17.2 to 20.7 m/s, 12 at least 32.7), and the nearest force of each is itself;
// the 3 s gust factor is 1.24 over the sea, 1.46 over grassland and 1.89 over a forest; and a gust
// held from rest stands at half its level at five seconds and at all of it at ten, holds there, and
// let go dies to half in five and to nothing in ten, whatever the seconds were cut into.
TEST_CASE("wind: the live wind's ladder, its gust factor and its gust's envelope") {
  const f64 low[13] = {0.0, 0.3, 1.6, 3.4, 5.5, 8.0, 10.8, 13.9, 17.2, 20.8, 24.5, 28.5, 32.7};
  const f64 high[13] = {0.2, 1.5, 3.3, 5.4, 7.9, 10.7, 13.8, 17.1, 20.7, 24.4, 28.4, 32.6, 60.0};
  for (u32 f = 0; f <= k_beaufort_max; ++f) {
    CHECK(beaufort_speed(f) >= low[f]);
    CHECK(beaufort_speed(f) <= high[f]);
    CHECK(beaufort_force(beaufort_speed(f)) == f);
  }
  CHECK(beaufort_speed(8) == doctest::Approx(18.92).epsilon(1e-3));
  CHECK(wind_gust_factor(0.0002) == doctest::Approx(1.24).epsilon(0.005));
  CHECK(wind_gust_factor(0.03) == doctest::Approx(1.46).epsilon(0.005));
  CHECK(wind_gust_factor(0.5) == doctest::Approx(1.885).epsilon(0.005));

  f64 fine = 0.0;
  f64 coarse = 0.0;
  for (u32 t = 0; t < 1200; ++t)  // five seconds at 240 ticks a second
    fine = wind_gust_phase_step(fine, true, 1.0 / 240.0);
  coarse = wind_gust_phase_step(coarse, true, 5.0);
  CHECK(wind_gust_level(fine) == doctest::Approx(0.5).epsilon(1e-9));
  CHECK(fine == doctest::Approx(coarse).epsilon(1e-9));
  for (u32 t = 0; t < 1200; ++t)
    fine = wind_gust_phase_step(fine, true, 1.0 / 240.0);
  CHECK(wind_gust_level(fine) == doctest::Approx(1.0).epsilon(1e-9));
  fine = wind_gust_phase_step(fine, true, 30.0);  // held: it stands
  CHECK(wind_gust_level(fine) == 1.0);
  fine = wind_gust_phase_step(fine, false, 5.0);
  CHECK(wind_gust_level(fine) == doctest::Approx(0.5).epsilon(1e-9));
  fine = wind_gust_phase_step(fine, false, 5.0);
  CHECK(wind_gust_level(fine) == 0.0);
  // The shape: Hermite's smoothstep, within 0.011 of IEC 61400-1's half cosine.
  f64 worst = 0.0;
  for (u32 i = 0; i <= 100; ++i) {
    const f64 p = static_cast<f64>(i) / 100.0;
    worst = std::max(worst, std::abs(wind_gust_level(p) - 0.5 * (1.0 - std::cos(k_wind_pi * p))));
  }
  CHECK(worst < 0.011);
  WindLive live;
  CHECK(wind_live_speed(live, 0.03) == 0.0);  // unset: no live wind
  live.set = true;
  live.mean_m_s = 10.0;
  live.gust = 1.0;
  CHECK(wind_live_speed(live, 0.03) == doctest::Approx(10.0 * wind_gust_factor(0.03)));
}

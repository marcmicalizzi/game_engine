// The scene's wind and the sea it raises (wind.h; docs/subsystems/wind.md; the grounding,
// docs/experiments/wind.md; roadmap W136): no device. The numbers checked here are the page's.

#include <domain/gfx/water_spectrum.h>
#include <domain/gfx/water_swash.h>
#include <domain/gfx/wind.h>

#include <doctest/doctest.h>

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
  // Calm: glass.
  CHECK(at(5.0).height_m() == 0.0);
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
  // Never past the fetch-limited sea, at any hour.
  for (f64 h = 0.0; h < 48.0; h += 0.25) {
    const SeaState s = at(h);
    if (s.wind_m_s >= k_sea_min_wind_m_s)
      CHECK(s.sea_height_m <= sea_height(s.wind_m_s, fetch) * (1.0 + 1e-9));
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
  // A lake (fetch 900 m) loses its swell in minutes.
  CHECK(sea_state(w, 23.5 * 3600.0, 900.0).height_m() < 1e-3);
}

TEST_CASE("wind: the sea state is a function of the time, continuous across the steps") {
  const WindDesc w = rising_wind();
  const f64 fetch = 60000.0;
  for (f64 t = 7.0 * 3600.0; t < 14.0 * 3600.0; t += 1800.0) {
    const SeaState a = sea_state(w, t - 0.5, fetch);
    const SeaState b = sea_state(w, t + 0.5, fetch);
    CHECK(std::abs(a.height_m() - b.height_m()) < 2e-3 * std::max(a.height_m(), 0.01));
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
  water_spectrum_on_grid(sea_state(w, 5.0 * 3600.0, fetch), range.grid, 11u, calm);
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

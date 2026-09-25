#pragma once

// The seeded wind record (docs/subsystems/terrain.md, "The wind record"; ADR-0043). Wind is what
// moves the dunes, and the dunes must be a function of time rather than a simulation, so the wind
// has to be something whose *integral* to any time t costs the same whatever t is.
//
// **One day at a time, one period, prefix sums.** The record is a table of days over a period of
// `k_record_years` years: each day a direction and a strength, drawn from the seed with a seasonal
// structure (the direction swings about the prevailing one over the year, the strength peaks in
// one season, a share of days are calm, and every day has its own gusts), and from the strength
// the day's **sand flux** by Bagnold's cube law (flux grows as the cube of the shear velocity), as
// the volume of sand that crosses a line one centimetre wide: cm^3 per cm, cm^2 a day. The record
// repeats after its period, so the flux integral to a time t is
//
//     I(t) = cycles * total  +  prefix[day in the period]  +  flux[that day] * (time into the day)
//
// three lookups, whatever t is. The daily sequence may be anything the seed draws — storms, calms,
// reversals — because nothing about it has to telescope or have a closed form of its own; the
// period is what buys the closed form. Eight years is long enough that nobody notices a repeat of
// the daily weather and short enough that the table and its prefix sums are 125 KiB, built in well
// under a millisecond.
//
// Everything is integer: the table is built from the engine's hash and the integer sine of
// fixed.h, and I(t) is exact, so the integral from t1 to t2 is `integral(t2) - integral(t1)`
// exactly, and the sum of the daily fluxes a stepwise simulation would add up day by day is the
// same number (the tests check both).

#include <core/base/types.h>
#include <core/containers/vector.h>

namespace engine::terrain {

inline constexpr i32 k_days_per_year = 365;
inline constexpr i32 k_record_years = 8;
inline constexpr i32 k_record_days = k_days_per_year * k_record_years;
inline constexpr i64 k_us_per_second = 1'000'000;
inline constexpr i64 k_us_per_day = 86'400 * k_us_per_second;

// The day a game time falls in, rounding towards negative infinity.
i64 day_of(i64 time_us) noexcept;

struct WindParams {
  u64 seed = 1;
  // Where the sand moves *to*, as a binary angle about +y from +x (65536 a turn), and whether to
  // draw it from the seed instead. The prevailing wind of the dune field.
  u16 prevailing_turn = 0;
  bool seeded_direction = true;
  // The mean daily sand flux over the record, cm^2 a day (volume per unit width). 5,479 is
  // 200 m^2 a year: a strong trade-wind desert, where a 1 m barchan moves 200 m a year.
  i32 flux_cm2_per_day = 5479;
  // How far the direction swings about the prevailing one over a year (binary angle; 5,461 is
  // 30 degrees), and how far one day strays from the season's direction (2,731, 15 degrees).
  i32 swing_turn = 5461;
  i32 jitter_turn = 2731;
  // The strength's seasonal amplitude and each day's gusts, Q16 of the mean strength; the share
  // of days that are calm (no sand moves), Q16.
  i32 seasonal_q16 = 26214;
  i32 gust_q16 = 22938;
  i32 calm_q16 = 9830;
};

// One day of the record: 20 bytes (tests/size_table.cpp).
struct WindDay {
  i32 fx = 0;         // the day's sand flux towards +x, cm^2
  i32 fz = 0;         // towards +z
  i32 magnitude = 0;  // |flux|, cm^2; 0 on a calm day
  i32 speed_q16 = 0;  // the wind's strength relative to the record's mean, Q16
  u16 turn = 0;       // where the sand moves to, binary angle
  u16 calm = 0;       // 1 on a calm day
};

// A flux integral: sand moved per unit width, as a vector and as the integral of its magnitude
// (the scalar is what fills a footprint; the vector is what moves a dune). cm^2.
struct FluxIntegral {
  i64 x = 0;
  i64 z = 0;
  i64 magnitude = 0;
  constexpr bool operator==(const FluxIntegral&) const = default;
};
constexpr FluxIntegral operator-(const FluxIntegral& a, const FluxIntegral& b) noexcept {
  return FluxIntegral{a.x - b.x, a.z - b.z, a.magnitude - b.magnitude};
}

class WindRecord {
 public:
  explicit WindRecord(const WindParams& params = {});

  const WindParams& params() const noexcept { return params_; }
  u16 prevailing_turn() const noexcept { return prevailing_; }
  // The prevailing direction as a unit vector, Q14.
  i32 prevailing_x_q14() const noexcept { return prevailing_x_; }
  i32 prevailing_z_q14() const noexcept { return prevailing_z_; }

  // Any day, before the epoch or after the period: the record wraps.
  const WindDay& day(i64 day_index) const noexcept;
  // The flux integral from time 0 to `time_us` (negative before the epoch): closed form, the same
  // cost at any time.
  FluxIntegral integral(i64 time_us) const noexcept;
  FluxIntegral between(i64 from_us, i64 to_us) const noexcept {
    return integral(to_us) - integral(from_us);
  }
  // The period's totals: what one cycle of the record moves.
  const FluxIntegral& period_total() const noexcept { return prefix_[k_record_days]; }

 private:
  WindParams params_;
  u16 prevailing_ = 0;
  i32 prevailing_x_ = 0;
  i32 prevailing_z_ = 0;
  Vector<WindDay> days_;         // k_record_days
  Vector<FluxIntegral> prefix_;  // k_record_days + 1: prefix_[d] is days [0, d)
};

}  // namespace engine::terrain

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
// **Storm hours.** A day's flux spread evenly over it is the strongest thing a day can say, and a
// sandstorm is a few hours, not a day. So the record also draws `storms_per_year` storms a year
// from the seed: each an interval of a few hours inside one day, with its own direction (within
// `storm_spread_turn` of the prevailing wind) and a strength that rises and falls over it (a half
// sine, peaking at `storm_speed_q16` of the record's mean strength), whose flux is the cube law's
// on the same scale as the days'. A storm day's flux is its own day's plus the storm's, so the
// prefix sums hold the storms and a period's total counts them; within a storm day the integral
// follows a cumulative table of its 24 hours rather than a straight line. I(t) is still the
// period's cycles, the prefix to the day and the day's own profile — the line, or the storm's table
// — and `wind_at(t)` answers the wind in the hour holding t from the same numbers, so a dune, a
// footprint's refill, the dust and the gameplay all read one wind (terrain.md, "Storms").
//
// **The day** (terrain.md, "The day's wind"). Between a storm and a season is the day: the wind
// rises with the day's heat, peaks in the afternoon and falls calm at night, and veers through the
// day. With `diurnal_q16` or `veer_turn` set, every day's flux is spread over its hours by one
// shared profile — a strength of `1 + diurnal cos(day angle - peak)`, cubed and normalized to the
// day's own total, and a direction the day's own plus `veer sin(day angle - veer phase)` — a
// closed function of the time of day, so the record is still a function of the seed and the time.
// The profile *redistributes* a day's sand and never adds to it: a day's magnitude is its own to
// the unit, and its net vector is shortened only by the veer (a wind that turns moves less sand
// one way). I(t) on such a day is its hours' cumulative flux, a straight line within the hour, as
// on a storm day. Off (the default), a day is a straight line exactly as before.
//
// **A storm's transport gain** (terrain.md, "A storm scales transport"). A storm may move `gain`
// times the sand its wind moves: its hours' flux is multiplied by the gain before it enters the
// prefix sums, so everything that reads the integral — the dunes, the overlay, the renderer's
// cadence — moves by the gained transport, while the clock, the direction, the day and `wind_at`'s
// strength run as they did. It is a stylization, like a band's `celerity_scale`: real dunes do not
// visibly walk in a storm. A gain of 1 (the default) is the record as before, to the bit.
//
// **The wind rose** (`rose`): the transport that moved towards each of `k_rose_sectors` directions
// between two times, from prefix sums per sector beside the magnitude's, so a structure's drift can
// read how much sand came from where. The sectors' sum is the magnitude integral exactly.
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
  // Storms (terrain.md, "Storms"). None by default, so a record described before storms existed
  // is the same bytes. Each lasts `storm_hours_min` to `storm_hours_max` hours of one day, blows
  // within `storm_spread_turn` of the prevailing wind (10,923 is 60 degrees), and peaks at
  // `storm_speed_q16` of the record's mean strength (2.5: about 16 times a mean day's flux an hour,
  // by the cube law).
  i32 storms_per_year = 0;
  i32 storm_hours_min = 3;
  i32 storm_hours_max = 12;
  i32 storm_spread_turn = 10923;
  i32 storm_speed_q16 = 163840;
  // The day (terrain.md, "The day's wind"). Off by default, so a record described before the day
  // existed is the same bytes. `diurnal_q16`: the strength's daily swing, Q16 of the day's strength
  // (65536 falls calm at the night's lowest; more is calm for longer); `diurnal_peak_turn`: when it
  // peaks, as a binary angle of the day (40,960 is 15:00); `veer_turn`: the most the direction
  // strays from the day's either side over the day (a binary angle; 5,461 is 30 degrees);
  // `veer_phase_turn`: when the veer passes through zero turning positive (32,768 is noon).
  i32 diurnal_q16 = 0;
  u16 diurnal_peak_turn = 40960;
  i32 veer_turn = 0;
  u16 veer_phase_turn = 32768;
  // A storm's transport gain, Q16 (terrain.md, "A storm scales transport"): its hours' flux times
  // this. `storm_gains` overrides it for the period's storms by their index in day order (the
  // order `WindRecord::storms` lists them; they repeat every period).
  i32 storm_gain_q16 = 65536;
  struct StormGain {
    i32 storm = 0;
    i32 gain_q16 = 65536;
  };
  Vector<StormGain> storm_gains{};
};

// The most a storm's transport gain may be: 1,000 times.
inline constexpr i32 k_max_storm_gain_q16 = 1000 * 65536;

// The most storms a year a record may hold: a period's storms are numbered in a byte.
inline constexpr i32 k_max_storms_per_year = 31;

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

// The wind rose's sectors: sixteen of 22.5 degrees, sector k centred on the binary angle k * 4096
// (where the sand moves *to*; it blew from sector k + 8). Sixteen resolves the day's veer (a sector
// is 22.5 degrees and a veer of 30 either side crosses two or three) and a storm's 60-degree spread
// into several, and a period's prefix sums are 374 KB; eight would put a storm and the day it blows
// in into one sector, and thirty-two doubles the table for a drift that cannot tell 11 degrees.
inline constexpr u32 k_rose_sectors = 16;
constexpr u32 rose_sector(u16 turn) noexcept {
  return ((static_cast<u32>(turn) + 2048u) & 0xFFFFu) >> 12;
}

// One day of the record: 32 bytes (tests/size_table.cpp). The flux is 64-bit since storms have a
// transport gain: a storm day at a gain of hundreds over a strong wind passes 2^31 cm^2.
struct WindDay {
  i64 fx = 0;         // the day's sand flux towards +x, cm^2, its storm's included (gained)
  i64 fz = 0;         // towards +z
  i64 magnitude = 0;  // |flux| summed over its hours, cm^2; 0 on a calm day without a storm
  i32 speed_q16 = 0;  // the day's own wind's strength relative to the record's mean, Q16
  u16 turn = 0;       // where the day's own wind moves the sand to, binary angle
  u8 calm = 0;        // 1 on a calm day
  u8 storm = 0;       // 1 + the index of the day's storm in the record's, 0 for none
};

// A storm of the record: which day, which hours, where it blows, and its hours' cumulative flux.
struct WindStorm {
  i32 day = 0;         // in the period
  i32 first_hour = 0;  // [first_hour, first_hour + hours) of that day
  i32 hours = 0;
  u16 turn = 0;          // where it moves the sand to
  i32 peak_q16 = 0;      // its strongest hour's strength, Q16 of the record's mean
  i32 gain_q16 = 65536;  // its transport gain (WindParams::storm_gain_q16, or its override)
  // The day's flux to the start of each of its hours, storm and day together: 25 entries, the
  // last the day's total, the storm's gained. cm^2.
  FluxIntegral hour[25];
  // The storm's own share of `hour[h].magnitude`, gained: what the rose files under its direction.
  i64 storm_magnitude[25] = {};
};

// The wind in the hour holding a time: its sand flux over that hour, where it moves the sand, how
// strong it is, and whether it is a storm's. What everything that needs "the wind now" reads.
struct WindAt {
  FluxIntegral flux;  // over the hour, cm^2
  u16 turn = 0;       // of the hour's flux (the day's own direction in a calm)
  i32 speed_q16 = 0;  // the stronger of the day's wind and the storm's this hour
  bool storm = false;
  i32 gain_q16 = 65536;  // the transport gain on the hour's storm share (1 outside a storm)
  i32 hour = 0;          // of the day, 0..23
  i64 day = 0;
};

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
  // **The magnitude integral without its truncation**: `integral(time_us).magnitude` in `whole`,
  // and in `fraction`, within [0, 1), the share of the next cm^2 that the day's straight line (or
  // the hour's) has reached by `time_us`, so `whole + fraction` is the integral as a continuous
  // function of time — piecewise linear, the same line `integral` floors. `integral` is in whole
  // cm^2, which is right for the sand a dune moves by and a staircase for anything drawn from the
  // integral continuously: at the record's mean flux a cm^2 takes about sixteen game seconds to
  // blow, so a ripple driven by the whole number held still for those seconds and then jumped
  // (renderer.md, "Ripples that move"). The same integer arithmetic as `integral`, with the
  // remainder of its one division kept.
  void magnitude_at(i64 time_us, i64& whole, f64& fraction) const noexcept;
  // The period's totals: what one cycle of the record moves, storms included.
  const FluxIntegral& period_total() const noexcept { return prefix_[k_record_days]; }
  // The wind in the hour holding `time_us`.
  WindAt wind_at(i64 time_us) const noexcept;
  // The period's storms, in day order.
  const Vector<WindStorm>& storms() const noexcept { return storms_; }
  // Whether the day's profile is on (`diurnal_q16` or `veer_turn`).
  bool diurnal() const noexcept { return diurnal_; }

  // **The wind rose**: the transport integral from time 0 to `time_us` towards each sector
  // (`rose_sector`), cm^2 — closed form like `integral`, and summing to its magnitude exactly.
  void rose(i64 time_us, i64 (&out)[k_rose_sectors]) const noexcept;
  // Between two times: the difference, so any split adds up.
  void rose_between(i64 from_us, i64 to_us, i64 (&out)[k_rose_sectors]) const noexcept;

 private:
  // A day's cumulative flux to the start of hour `h` (0..24) under the day's profile.
  FluxIntegral profile_at(const WindDay& day, i32 h) const noexcept;
  // The hour's direction on a profiled day, and its strength.
  u16 profile_turn(const WindDay& day, i32 h) const noexcept;
  // The rose of a day's first `h` whole hours plus `frac` microseconds of hour h, into `out`.
  void rose_in_day(i32 r, i64 into, i64 (&out)[k_rose_sectors]) const noexcept;
  WindParams params_;
  u16 prevailing_ = 0;
  i32 prevailing_x_ = 0;
  i32 prevailing_z_ = 0;
  Vector<WindDay> days_;         // k_record_days
  Vector<FluxIntegral> prefix_;  // k_record_days + 1: prefix_[d] is days [0, d)
  Vector<WindStorm> storms_;
  // The day's profile (`diurnal`): cumulative weight to each hour, Q30 (the last exactly 2^30),
  // the weighted unit vector of the veer, Q30, each hour's veer and strength factor (Q16).
  bool diurnal_ = false;
  i64 weight_[25] = {};
  i64 along_[25] = {};   // sum of w cos(veer)
  i64 across_[25] = {};  // sum of w sin(veer)
  i32 veer_[24] = {};
  i32 strength_[24] = {};
  // The rose's prefix sums: (k_record_days + 1) * k_rose_sectors, day-major.
  Vector<i64> rose_prefix_;
};

}  // namespace engine::terrain

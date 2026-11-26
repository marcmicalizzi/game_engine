#pragma once

// The ephemeris (docs/subsystems/sky.md, "Where the sun and the moon are"): the sun and the moon on
// a world's calendar, at its latitude, at a game time — seconds since the world's epoch, which is
// midnight on the calendar's `day_of_year` (the terrain's wind reads the same clock).
//
// **Low precision, on purpose.** The sun is the Astronomical Almanac's low-precision formula (its
// mean longitude and anomaly and the two terms of its equation of centre, good to a hundredth of a
// degree for a century either side of 2000); the moon is Meeus' main terms (the equation of centre,
// the evection and the variation, and the latitude's first term, good to a few tenths of a degree).
// The world has a calendar and no year, so the elements are the year 2000's, run on continuously
// from the calendar's day. What the sky needs is right: the sun's declination through the seasons
// and its hour through the day, the moon a month round the sky and five degrees off the ecliptic,
// rising fifty minutes later each night with its phase, and the stars turning about the pole once a
// sidereal day. Neither parallax (the moon's is up to a degree) nor refraction (half a degree at
// the horizon) nor precession is modelled.
//
// **The game's hour is apparent solar time**: the sun is due south at 12:00 every day (north of the
// tropics), which is what a player and a scene's wind (whose peak is at a local hour) mean by the
// time; the equation of time moves the stars, not the sun.
//
// **The world's frame**: y up, north -z, east +x (the renderer's "looking north" is along -z).

#include <core/base/types.h>

namespace engine::sky {

// A world's calendar: where it is and what its epoch is.
struct Calendar {
  f64 latitude_deg = 30.0;  // north positive
  f64 day_of_year = 80.0;   // the day the epoch (game time 0, midnight) begins; 1 is January 1
  f64 moon_age_days = 0.0;  // the moon's mean age at the epoch, days since new
};

inline constexpr f64 k_synodic_month_days = 29.530588853;
inline constexpr f64 k_day_s = 86400.0;

// A direction as three doubles.
struct Direction {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};

struct Ephemeris {
  Direction sun;   // world, towards the sun
  Direction moon;  // world, towards the moon
  // Celestial (equatorial, x towards the vernal equinox, z the north pole) directions of both.
  Direction sun_celestial;
  Direction moon_celestial;
  // The world directions of the celestial frame's axes.
  Direction celestial_x;
  Direction celestial_y;
  Direction celestial_z;
  f64 sun_distance_au = 1.0;
  f64 moon_distance_km = 385000.0;
  f64 moon_elongation_rad = 0.0;  // the angle between the sun and the moon, seen from here
  f64 moon_lit = 0.0;             // the illuminated fraction, (1 - cos elongation) / 2
  f64 local_sidereal_rad = 0.0;
  f64 day_of_year = 1.0;  // fractional, 1 at January 1 00:00
  f64 hour = 0.0;         // apparent solar time, [0, 24)
  f64 sun_declination_rad = 0.0;
};

// Where everything is `time_s` game seconds after the epoch.
void ephemeris(const Calendar& calendar, f64 time_s, Ephemeris& out) noexcept;

// **The civil form** (W185; docs/subsystems/sky.md, "The sky by place and civil time"): a place
// and a moment on the real calendar, where the solar form above has a latitude and a day of no
// year. Game time 0 is the moment; game time runs on from it in UT, so the date advances as the
// clock passes midnight. The sun is the same Almanac formula and the moon the same terms, at the
// moment's own days from J2000.0 instead of 2000's run on from a day of the year; the sky turns by
// Greenwich mean sidereal time plus the longitude rather than by the hour. The Ephemeris's `hour`
// is the local apparent solar time and its `day_of_year` the clock's own (1 at the clock's January
// 1 00:00, back to 1 at each new year). Neither ΔT (about 70 s: 0.0008° of the sun, 0.01° of the
// moon), nutation (0.005°), the moon's parallax (up to 1°) nor refraction (0.57° at the horizon,
// 0.1° at 10° up) is modelled.
struct CivilCalendar {
  f64 latitude_deg = 0.0;   // north positive
  f64 longitude_deg = 0.0;  // east positive
  f64 days_j2000 = 0.0;     // the moment in UT days from 2000-01-01 12:00 (JD 2451545.0)
  f64 utc_offset_h = 0.0;   // the clock's offset from UTC, east positive: for the reported date
};

// Days from 2000-01-01 to a Gregorian date, by the calendar's leap rule (a March-based year, so
// the leap day is the year's last): 0 for 2000-01-01, -10957 for 1970-01-01. And back.
i64 days_from_civil(i32 year, u32 month, u32 day) noexcept;
void civil_from_days(i64 days, i32& year, u32& month, u32& day) noexcept;
// Whether a year, month and day name a day of the Gregorian calendar.
bool valid_civil_date(i32 year, u32 month, u32 day) noexcept;
// The UT days from J2000.0 of a clock's date and time of day (hours) at its offset from UTC.
f64 days_from_j2000(i32 year, u32 month, u32 day, f64 time_of_day_h, f64 utc_offset_h) noexcept;

// Where everything is `time_s` seconds of UT after the civil calendar's moment.
void ephemeris(const CivilCalendar& calendar, f64 time_s, Ephemeris& out) noexcept;

// The world direction of a celestial one, through the frame an ephemeris gave.
Direction to_world(const Ephemeris& e, const Direction& celestial) noexcept;

// The moon's illuminance outside the atmosphere in the sun's (at one AU): a Lambertian sphere of
// albedo `albedo`, `radius_km` at `distance_km`, lit by the sun `sun_distance_au` away and seen at
// phase angle pi - elongation: (2/3) A (R / d)^2 ((pi - i) cos i + sin i) / pi / R_sun^2.
f64 moon_illuminance(f64 albedo, f64 radius_km, f64 distance_km, f64 elongation_rad,
                     f64 sun_distance_au) noexcept;

inline constexpr f64 k_moon_radius_km = 1737.4;
inline constexpr f64 k_moon_albedo = 0.12;
inline constexpr f64 k_sun_radius_km = 695700.0;
inline constexpr f64 k_au_km = 149597870.7;

}  // namespace engine::sky

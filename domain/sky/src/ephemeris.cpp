#include <domain/sky/ephemeris.h>

#include <cmath>

namespace engine::sky {

namespace {

constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_rad = k_pi / 180.0;

f64 wrap_deg(f64 a) noexcept {
  const f64 r = std::fmod(a, 360.0);
  return r < 0.0 ? r + 360.0 : r;
}

Direction unit(f64 x, f64 y, f64 z) noexcept {
  const f64 l = std::sqrt(x * x + y * y + z * z);
  return l > 0.0 ? Direction{x / l, y / l, z / l} : Direction{0.0, 1.0, 0.0};
}

// Ecliptic longitude and latitude to the celestial (equatorial) frame, by the obliquity.
Direction ecliptic_to_celestial(f64 lambda, f64 beta, f64 obliquity) noexcept {
  const f64 x = std::cos(beta) * std::cos(lambda);
  const f64 y = std::cos(beta) * std::sin(lambda);
  const f64 z = std::sin(beta);
  const f64 c = std::cos(obliquity);
  const f64 s = std::sin(obliquity);
  return unit(x, y * c - z * s, y * s + z * c);
}

// The sun at `n` days from J2000.0: the Astronomical Almanac's low-precision formula. Fills the
// sun's celestial direction, distance and declination; gives back its mean longitude (degrees),
// the obliquity (radians) and its right ascension (radians), which the moon and the frame read.
struct SunTerms {
  f64 mean_longitude = 0.0;
  f64 obliquity = 0.0;
  f64 ra = 0.0;
};

SunTerms sun_at(f64 n, Ephemeris& out) noexcept {
  SunTerms t;
  t.mean_longitude = wrap_deg(280.460 + 0.9856474 * n);
  const f64 anomaly = wrap_deg(357.528 + 0.9856003 * n) * k_rad;
  const f64 lambda_sun =
      (t.mean_longitude + 1.915 * std::sin(anomaly) + 0.020 * std::sin(2.0 * anomaly)) * k_rad;
  out.sun_distance_au = 1.00014 - 0.01671 * std::cos(anomaly) - 0.00014 * std::cos(2.0 * anomaly);
  t.obliquity = (23.439 - 0.0000004 * n) * k_rad;
  out.sun_celestial = ecliptic_to_celestial(lambda_sun, 0.0, t.obliquity);
  t.ra = std::atan2(out.sun_celestial.y, out.sun_celestial.x);
  out.sun_declination_rad = std::asin(out.sun_celestial.z);
  return t;
}

// The moon at `n` days from J2000.0 (Meeus' main terms), its mean longitude `moon_mean` (degrees,
// wrapped) given, then the elongation and the lit fraction against the sun already in `out`.
void moon_at(f64 n, f64 moon_mean, const SunTerms& sun, Ephemeris& out) noexcept {
  const f64 moon_anomaly = wrap_deg(134.963 + 13.064993 * n) * k_rad;
  const f64 argument = wrap_deg(93.272 + 13.229350 * n) * k_rad;
  const f64 mean_elongation = wrap_deg(moon_mean - sun.mean_longitude) * k_rad;
  const f64 lambda_moon = (moon_mean + 6.289 * std::sin(moon_anomaly) +
                           1.274 * std::sin(2.0 * mean_elongation - moon_anomaly) +
                           0.658 * std::sin(2.0 * mean_elongation)) *
                          k_rad;
  const f64 beta_moon = 5.128 * std::sin(argument) * k_rad;
  out.moon_distance_km = 385001.0 - 20905.0 * std::cos(moon_anomaly);
  out.moon_celestial = ecliptic_to_celestial(lambda_moon, beta_moon, sun.obliquity);
  const f64 cos_elongation = out.sun_celestial.x * out.moon_celestial.x +
                             out.sun_celestial.y * out.moon_celestial.y +
                             out.sun_celestial.z * out.moon_celestial.z;
  out.moon_elongation_rad =
      std::acos(cos_elongation < -1.0 ? -1.0 : (cos_elongation > 1.0 ? 1.0 : cos_elongation));
  out.moon_lit = 0.5 * (1.0 - cos_elongation);
}

// The celestial frame in the world from `out.local_sidereal_rad`: the pole P stands the latitude
// above the northern (-z) horizon, M is the celestial equator's point on the meridian, W is west; a
// direction of hour angle H and declination d is cos d cos H M + cos d sin H W + sin d P, and H =
// LST - RA. Then the sun's and the moon's world directions.
void frame_at(f64 latitude_deg, Ephemeris& out) noexcept {
  const f64 phi = latitude_deg * k_rad;
  const Direction p{0.0, std::sin(phi), -std::cos(phi)};
  const Direction m{0.0, std::cos(phi), std::sin(phi)};
  const Direction w{-1.0, 0.0, 0.0};
  const f64 cl = std::cos(out.local_sidereal_rad);
  const f64 sl = std::sin(out.local_sidereal_rad);
  out.celestial_x = unit(cl * m.x + sl * w.x, cl * m.y + sl * w.y, cl * m.z + sl * w.z);
  out.celestial_y = unit(sl * m.x - cl * w.x, sl * m.y - cl * w.y, sl * m.z - cl * w.z);
  out.celestial_z = p;
  out.sun = to_world(out, out.sun_celestial);
  out.moon = to_world(out, out.moon_celestial);
}

// Floor division, for days before 2000 and a March-based year before 0.
i64 floor_div(i64 a, i64 b) noexcept {
  const i64 q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

}  // namespace

// ---- the civil calendar: the Gregorian leap rule over a March-based year, whose 400 years are
// 146,097 days; 0000-03-01 of that count is day -730,425 from 2000-01-01 ----
i64 days_from_civil(i32 year, u32 month, u32 day) noexcept {
  const i64 y = static_cast<i64>(year) - (month <= 2 ? 1 : 0);
  const i64 era = floor_div(y, 400);
  const i64 yoe = y - era * 400;                                   // [0, 399]
  const i64 mp = static_cast<i64>(month) + (month > 2 ? -3 : 9);   // March is 0
  const i64 doy = (153 * mp + 2) / 5 + static_cast<i64>(day) - 1;  // [0, 365]
  const i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + doe - 730425;
}

void civil_from_days(i64 days, i32& year, u32& month, u32& day) noexcept {
  const i64 z = days + 730425;
  const i64 era = floor_div(z, 146097);
  const i64 doe = z - era * 146097;
  const i64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const i64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const i64 mp = (5 * doy + 2) / 153;
  day = static_cast<u32>(doy - (153 * mp + 2) / 5 + 1);
  month = static_cast<u32>(mp < 10 ? mp + 3 : mp - 9);
  year = static_cast<i32>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

bool valid_civil_date(i32 year, u32 month, u32 day) noexcept {
  if (month < 1 || month > 12 || day < 1 || day > 31) return false;
  i32 y = 0;
  u32 m = 0;
  u32 d = 0;
  civil_from_days(days_from_civil(year, month, day), y, m, d);
  return y == year && m == month && d == day;
}

f64 days_from_j2000(i32 year, u32 month, u32 day, f64 time_of_day_h, f64 utc_offset_h) noexcept {
  // J2000.0 is 2000-01-01 12:00, half a day after the date count's zero.
  return static_cast<f64>(days_from_civil(year, month, day)) - 0.5 +
         (time_of_day_h - utc_offset_h) / 24.0;
}

void ephemeris(const Calendar& calendar, f64 time_s, Ephemeris& out) noexcept {
  out = Ephemeris{};
  // Days since the calendar's January 1 at noon, which is where J2000's elements count from: the
  // epoch is midnight at the start of `day_of_year`.
  const f64 days = time_s / k_day_s;
  const f64 n = (calendar.day_of_year - 1.0) + days - 0.5;
  out.day_of_year = calendar.day_of_year + days;
  const f64 day_fraction = days - std::floor(days);
  out.hour = day_fraction * 24.0;

  // ---- the sun: the Astronomical Almanac's low-precision formula ----
  const SunTerms sun = sun_at(n, out);

  // ---- the local sidereal time: the game's hour is apparent solar time, so the sun's hour angle
  // is the hour's from noon and the sidereal time is the sun's right ascension plus it ----
  const f64 hour_angle_sun = (out.hour - 12.0) * 15.0 * k_rad;
  out.local_sidereal_rad = sun.ra + hour_angle_sun;

  // ---- the moon: Meeus' main terms, its mean longitude moved so its mean age at the epoch is the
  // calendar's ----
  const f64 n0 = (calendar.day_of_year - 1.0) - 0.5;
  const f64 elongation_rate = 13.176396 - 0.9856474;  // degrees a day: the synodic month
  const f64 elongation_at_epoch = -62.144 + elongation_rate * n0;  // J2000's mean elongation then
  const f64 wanted = 360.0 * calendar.moon_age_days / k_synodic_month_days;
  const f64 shift = wanted - elongation_at_epoch;
  const f64 moon_mean = wrap_deg(218.316 + 13.176396 * n + shift);
  moon_at(n, moon_mean, sun, out);
  frame_at(calendar.latitude_deg, out);
}

void ephemeris(const CivilCalendar& calendar, f64 time_s, Ephemeris& out) noexcept {
  out = Ephemeris{};
  // The moment's own days from J2000.0, in UT (ΔT, about 70 s, is not modelled).
  const f64 n = calendar.days_j2000 + time_s / k_day_s;
  const SunTerms sun = sun_at(n, out);
  // The moon at its own mean longitude on that day: its age is the real one.
  moon_at(n, wrap_deg(218.316 + 13.176396 * n), sun, out);
  // Greenwich mean sidereal time, the USNO's approximate form (hours; 0.1 s a century lost), plus
  // the longitude: the sky turns by the Earth's rotation, and the sun's hour angle follows from it.
  const f64 gmst_h = 18.697374558 + 24.06570982441908 * n;
  out.local_sidereal_rad = wrap_deg(gmst_h * 15.0 + calendar.longitude_deg) * k_rad;
  const f64 hour_angle = wrap_deg((out.local_sidereal_rad - sun.ra) / k_rad + 180.0);
  out.hour = hour_angle / 15.0;  // apparent solar time: noon when the sun is on the meridian
  // The clock's date and time of day: days from the clock's 2000-01-01 00:00.
  const f64 clock_days = n + 0.5 + calendar.utc_offset_h / 24.0;
  const f64 whole = std::floor(clock_days);
  i32 year = 0;
  u32 month = 0;
  u32 day = 0;
  civil_from_days(static_cast<i64>(whole), year, month, day);
  out.day_of_year = static_cast<f64>(static_cast<i64>(whole) - days_from_civil(year, 1, 1)) + 1.0 +
                    (clock_days - whole);
  frame_at(calendar.latitude_deg, out);
}

Direction to_world(const Ephemeris& e, const Direction& d) noexcept {
  return unit(e.celestial_x.x * d.x + e.celestial_y.x * d.y + e.celestial_z.x * d.z,
              e.celestial_x.y * d.x + e.celestial_y.y * d.y + e.celestial_z.y * d.z,
              e.celestial_x.z * d.x + e.celestial_y.z * d.y + e.celestial_z.z * d.z);
}

f64 moon_illuminance(f64 albedo, f64 radius_km, f64 distance_km, f64 elongation_rad,
                     f64 sun_distance_au) noexcept {
  const f64 phase = k_pi - elongation_rad;  // the sun-moon-eye angle, parallax aside
  const f64 lambert = ((k_pi - phase) * std::cos(phase) + std::sin(phase)) / k_pi;
  const f64 size = radius_km / distance_km;
  const f64 sun = 1.0 / (sun_distance_au * sun_distance_au);
  return (2.0 / 3.0) * albedo * size * size * (lambert > 0.0 ? lambert : 0.0) * sun;
}

}  // namespace engine::sky

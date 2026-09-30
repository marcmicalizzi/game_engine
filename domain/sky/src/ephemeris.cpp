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

}  // namespace

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
  const f64 mean_longitude = wrap_deg(280.460 + 0.9856474 * n);
  const f64 anomaly = wrap_deg(357.528 + 0.9856003 * n) * k_rad;
  const f64 lambda_sun =
      (mean_longitude + 1.915 * std::sin(anomaly) + 0.020 * std::sin(2.0 * anomaly)) * k_rad;
  out.sun_distance_au = 1.00014 - 0.01671 * std::cos(anomaly) - 0.00014 * std::cos(2.0 * anomaly);
  const f64 obliquity = (23.439 - 0.0000004 * n) * k_rad;
  out.sun_celestial = ecliptic_to_celestial(lambda_sun, 0.0, obliquity);
  const f64 ra_sun = std::atan2(out.sun_celestial.y, out.sun_celestial.x);
  out.sun_declination_rad = std::asin(out.sun_celestial.z);

  // ---- the local sidereal time: the game's hour is apparent solar time, so the sun's hour angle
  // is the hour's from noon and the sidereal time is the sun's right ascension plus it ----
  const f64 hour_angle_sun = (out.hour - 12.0) * 15.0 * k_rad;
  out.local_sidereal_rad = ra_sun + hour_angle_sun;

  // ---- the moon: Meeus' main terms, its mean longitude moved so its mean age at the epoch is the
  // calendar's ----
  const f64 n0 = (calendar.day_of_year - 1.0) - 0.5;
  const f64 elongation_rate = 13.176396 - 0.9856474;  // degrees a day: the synodic month
  const f64 elongation_at_epoch = -62.144 + elongation_rate * n0;  // J2000's mean elongation then
  const f64 wanted = 360.0 * calendar.moon_age_days / k_synodic_month_days;
  const f64 shift = wanted - elongation_at_epoch;
  const f64 moon_mean = wrap_deg(218.316 + 13.176396 * n + shift);
  const f64 moon_anomaly = wrap_deg(134.963 + 13.064993 * n) * k_rad;
  const f64 argument = wrap_deg(93.272 + 13.229350 * n) * k_rad;
  const f64 mean_elongation = wrap_deg(moon_mean - mean_longitude) * k_rad;
  const f64 lambda_moon = (moon_mean + 6.289 * std::sin(moon_anomaly) +
                           1.274 * std::sin(2.0 * mean_elongation - moon_anomaly) +
                           0.658 * std::sin(2.0 * mean_elongation)) *
                          k_rad;
  const f64 beta_moon = 5.128 * std::sin(argument) * k_rad;
  out.moon_distance_km = 385001.0 - 20905.0 * std::cos(moon_anomaly);
  out.moon_celestial = ecliptic_to_celestial(lambda_moon, beta_moon, obliquity);
  const f64 cos_elongation = out.sun_celestial.x * out.moon_celestial.x +
                             out.sun_celestial.y * out.moon_celestial.y +
                             out.sun_celestial.z * out.moon_celestial.z;
  out.moon_elongation_rad =
      std::acos(cos_elongation < -1.0 ? -1.0 : (cos_elongation > 1.0 ? 1.0 : cos_elongation));
  out.moon_lit = 0.5 * (1.0 - cos_elongation);

  // ---- the celestial frame in the world: the pole P stands the latitude above the northern (-z)
  // horizon, M is the celestial equator's point on the meridian, W is west; a direction of hour
  // angle H and declination d is cos d cos H M + cos d sin H W + sin d P, and H = LST - RA ----
  const f64 phi = calendar.latitude_deg * k_rad;
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

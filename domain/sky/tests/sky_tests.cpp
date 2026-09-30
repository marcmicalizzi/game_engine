// The sky capability (docs/subsystems/sky.md): the ephemeris against what the sky does — the sun's
// height at noon through the seasons, its rising in the east and its night, the moon's month and
// its phase from its own age, its declination's range, the stars turning once a sidereal day about
// a pole that stands the latitude high in the north — the air a turbidity makes, the star table's
// counts and its Milky Way, and the provider "earth" through the registry, on the one clock.
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/sky.h>
#include <domain/sky/earth.h>
#include <domain/sky/ephemeris.h>
#include <domain/sky/sky.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::sky;

namespace {

constexpr f64 k_pi64 = 3.14159265358979323846;
constexpr f64 k_deg = 180.0 / k_pi64;

f64 elevation_deg(const Direction& d) { return std::asin(d.y) * k_deg; }
f64 dot(const Direction& a, const Direction& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
f64 angle_deg(const Direction& a, const Direction& b) {
  const f64 c = dot(a, b);
  return std::acos(c < -1.0 ? -1.0 : (c > 1.0 ? 1.0 : c)) * k_deg;
}

Ephemeris at(f64 latitude, f64 day, f64 hour, f64 moon_age = 0.0) {
  Calendar c;
  c.latitude_deg = latitude;
  c.day_of_year = day;
  c.moon_age_days = moon_age;
  Ephemeris e;
  ephemeris(c, hour * 3600.0, e);
  return e;
}

}  // namespace

TEST_CASE("sky: the sun's noon, rising and night follow the latitude and the season") {
  // The March equinox (day 80, the 20th) at 30 degrees north: due south at noon, 60 degrees up.
  const Ephemeris noon = at(30.0, 80.0, 12.0);
  CHECK(std::fabs(elevation_deg(noon.sun) - 60.0) < 0.6);
  CHECK(noon.sun.z > 0.99 * std::cos(60.0 / k_deg));  // south is +z
  CHECK(std::fabs(noon.sun.x) < 1e-6);                // on the meridian: apparent solar time
  CHECK(noon.hour == doctest::Approx(12.0));
  // Six in the morning: on the horizon, in the east (+x); six in the evening in the west.
  const Ephemeris morning = at(30.0, 80.0, 6.0);
  CHECK(std::fabs(elevation_deg(morning.sun)) < 0.6);
  CHECK(morning.sun.x > 0.99);
  const Ephemeris evening = at(30.0, 80.0, 18.0);
  CHECK(evening.sun.x < -0.99);
  // Midnight: as far under the northern horizon as noon was over the southern.
  CHECK(std::fabs(elevation_deg(at(30.0, 80.0, 0.0).sun) + 60.0) < 0.6);
  // The June solstice (day 172): 90 - 30 + 23.44 at noon, and the long day's sunrise north of east.
  const Ephemeris june = at(30.0, 172.0, 12.0);
  CHECK(std::fabs(elevation_deg(june.sun) - 83.44) < 0.3);
  CHECK(std::fabs(june.sun_declination_rad * k_deg - 23.44) < 0.1);
  CHECK(at(30.0, 172.0, 6.0).sun.y > 0.1);  // already well up at six
  CHECK(at(30.0, 172.0, 6.0).sun.z < 0.0);  // and north of east
  // December (day 355): 90 - 30 - 23.44.
  CHECK(std::fabs(elevation_deg(at(30.0, 355.0, 12.0).sun) - 36.56) < 0.3);
  // The equator at an equinox: overhead at noon. A pole in its summer: the same height all day.
  CHECK(elevation_deg(at(0.0, 80.0, 12.0).sun) > 89.4);
  const f64 pole_noon = elevation_deg(at(90.0, 172.0, 12.0).sun);
  const f64 pole_midnight = elevation_deg(at(90.0, 172.0, 0.0).sun);
  CHECK(std::fabs(pole_noon - 23.44) < 0.3);
  CHECK(std::fabs(pole_noon - pole_midnight) < 0.1);
  // The southern hemisphere's noon sun is in the north (-z).
  CHECK(at(-30.0, 80.0, 12.0).sun.z < -0.4);
  // Continuous through midnight and over a year's end.
  const Ephemeris before = at(30.0, 80.0, 23.999);
  const Ephemeris after = at(30.0, 80.0, 24.001);
  CHECK(angle_deg(before.sun, after.sun) < 0.05);
  CHECK(angle_deg(at(30.0, 365.0, 23.999).sun, at(30.0, 365.0, 24.001).sun) < 0.05);
}

TEST_CASE(
    "sky: the celestial pole stands the latitude high in the north, and the stars turn about "
    "it once a sidereal day") {
  for (const f64 latitude : {-35.0, 0.0, 31.1, 64.0}) {
    const Ephemeris e = at(latitude, 120.0, 21.5);
    CHECK(std::fabs(elevation_deg(e.celestial_z) - latitude) < 1e-9);
    if (latitude > 0.0) CHECK(e.celestial_z.z < 0.0);  // north is -z
    // Orthonormal and right-handed: x cross y is z.
    CHECK(std::fabs(dot(e.celestial_x, e.celestial_y)) < 1e-12);
    CHECK(std::fabs(dot(e.celestial_x, e.celestial_z)) < 1e-12);
    const Direction cxy{e.celestial_x.y * e.celestial_y.z - e.celestial_x.z * e.celestial_y.y,
                        e.celestial_x.z * e.celestial_y.x - e.celestial_x.x * e.celestial_y.z,
                        e.celestial_x.x * e.celestial_y.y - e.celestial_x.y * e.celestial_y.x};
    CHECK(dot(cxy, e.celestial_z) > 1.0 - 1e-12);
  }
  // A star at the celestial equator: back where it was after a sidereal day, a degree west of it
  // after a solar one.
  Calendar c;
  c.latitude_deg = 31.1;
  c.day_of_year = 200.0;
  const Direction star{0.6, 0.8, 0.0};
  Ephemeris e0, e_sidereal, e_solar;
  ephemeris(c, 3600.0 * 22.0, e0);
  ephemeris(c, 3600.0 * 22.0 + 86164.0905, e_sidereal);
  ephemeris(c, 3600.0 * 22.0 + 86400.0, e_solar);
  // The game's hour is apparent solar time, whose day is up to half a minute longer or shorter
  // than the mean one through the year (the equation of time's rate), so the star is back to within
  // that half minute's eighth of a degree.
  CHECK(angle_deg(to_world(e0, star), to_world(e_sidereal, star)) < 0.13);
  const f64 drift = angle_deg(to_world(e0, star), to_world(e_solar, star));
  CHECK(drift > 0.95);
  CHECK(drift < 1.02);
  // The sun's world direction is its celestial one through the frame.
  CHECK(angle_deg(to_world(e0, e0.sun_celestial), e0.sun) < 1e-9);
}

TEST_CASE("sky: the moon's phase is its age, and it keeps a month") {
  // The elongation is the mean age's plus the moon's and the sun's periodic terms (the equations of
  // centre, the evection, the variation), which reach about ten degrees, and the moon gains twelve
  // degrees a day on the sun, so each case expects the mean elongation at its own hour.
  // Full at the epoch: opposite the sun, lit whole, up at midnight in the south.
  const Ephemeris full = at(30.0, 80.0, 0.0, 14.765);
  CHECK(full.moon_elongation_rad * k_deg > 172.0);
  CHECK(full.moon_lit > 0.99);
  CHECK(full.moon.y > 0.5);
  // New at the epoch: beside the sun.
  const Ephemeris fresh = at(30.0, 80.0, 12.0, 0.0);
  const f64 fresh_mean = 360.0 * 0.5 / k_synodic_month_days;
  CHECK(std::fabs(fresh.moon_elongation_rad * k_deg - fresh_mean) < 11.0);
  CHECK(fresh.moon_lit < 0.03);
  // First quarter: ninety degrees east of the sun, half lit.
  const Ephemeris quarter = at(30.0, 80.0, 18.0, 7.38);
  const f64 quarter_mean = 360.0 * (7.38 + 0.75) / k_synodic_month_days;
  CHECK(std::fabs(quarter.moon_elongation_rad * k_deg - quarter_mean) < 11.0);
  CHECK(std::fabs(quarter.moon_lit - 0.5 * (1.0 - std::cos(quarter_mean / k_deg))) < 0.1);
  // A synodic month later the phase is back; the perturbations run on other periods, so to a few
  // degrees.
  Calendar c;
  c.latitude_deg = 30.0;
  c.day_of_year = 80.0;
  c.moon_age_days = 3.0;
  Ephemeris a, b;
  ephemeris(c, 5.0 * 86400.0, a);
  ephemeris(c, 5.0 * 86400.0 + k_synodic_month_days * 86400.0, b);
  CHECK(std::fabs(a.moon_elongation_rad - b.moon_elongation_rad) * k_deg < 6.0);
  // It rises about fifty minutes later each day, so at one hour its hour angle falls behind the
  // sky's by about 12 degrees a day.
  f64 sum = 0.0;
  for (u32 d = 0; d < 28; ++d) {
    Ephemeris e0, e1;
    ephemeris(c, (d * 24.0 + 20.0) * 3600.0, e0);
    ephemeris(c, ((d + 1) * 24.0 + 20.0) * 3600.0, e1);
    sum += std::fabs(angle_deg(e0.moon_celestial, e1.moon_celestial));
  }
  CHECK(sum / 28.0 > 11.0);
  CHECK(sum / 28.0 < 15.5);
  // Its declination over a year stays inside the ecliptic's tilt plus its own orbit's.
  f64 most = 0.0;
  for (u32 d = 0; d < 365 * 4; ++d) {
    Ephemeris e;
    ephemeris(c, d * 21600.0, e);
    most = std::fmax(most, std::fabs(std::asin(e.moon_celestial.z) * k_deg));
  }
  CHECK(most > 18.0);
  CHECK(most < 29.2);
  // Its light: about a fifth of a lux at full, nothing new.
  const f64 full_light =
      moon_illuminance(k_moon_albedo, k_moon_radius_km, 384400.0, k_pi64, 1.0) * 128000.0;
  CHECK(full_light > 0.15);
  CHECK(full_light < 0.3);
  CHECK(moon_illuminance(k_moon_albedo, k_moon_radius_km, 384400.0, 0.0, 1.0) < 1e-20);
  // Half lit is about an eleventh of full (a Lambertian sphere's phase law: 1 / pi at 90 degrees).
  const f64 half = moon_illuminance(k_moon_albedo, k_moon_radius_km, 384400.0, 0.5 * k_pi64, 1.0);
  CHECK(half / (full_light / 128000.0) == doctest::Approx(1.0 / k_pi64).epsilon(1e-9));
  MESSAGE("full moon " << full_light << " lux");
}

TEST_CASE("sky: the air a turbidity makes") {
  const scene_gen::Atmosphere clean = earth_atmosphere(1.0);
  const scene_gen::Atmosphere clear = earth_atmosphere(3.0);
  // The aerosol's vertical optical depth at 550 nm is (T - 1) clean atmospheres', 0.108 each.
  CHECK(static_cast<f64>(clear.mie_extinction.y * clear.mie_height_km) ==
        doctest::Approx(2.0 * 13.558e-3 * 8.0).epsilon(1e-5));
  // Never less than Hillaire's own faint aerosol.
  CHECK(static_cast<f64>(clean.mie_extinction.y * clean.mie_height_km) ==
        doctest::Approx(4.40e-3 * 1.2).epsilon(1e-5));
  // Bluer light is taken out more; nine tenths of what is taken is scattered.
  CHECK(clear.mie_extinction.z > clear.mie_extinction.y);
  CHECK(clear.mie_extinction.y > clear.mie_extinction.x);
  CHECK(clear.mie_scattering.y == doctest::Approx(0.9f * clear.mie_extinction.y));
  // The Rayleigh sky and the ozone are Earth's, whatever the haze.
  CHECK(clear.rayleigh_scattering.z == doctest::Approx(33.1e-3f));
  CHECK(clear.ozone_peak_km == 25.0f);
}

TEST_CASE("sky: the star table has the sky's counts, its colours and a Milky Way") {
  const Vector<scene_gen::Star> stars = earth_stars(1, k_star_magnitude_limit);
  REQUIRE(stars.size() == star_count_for_magnitude(k_star_magnitude_limit));
  CHECK(stars.size() > 8000);
  CHECK(stars.size() < 9200);
  u32 brighter_than_3 = 0;
  u32 faint = 0;
  u32 faint_in_band = 0;
  const f64 pole_ra = 192.85948 / k_deg;
  const f64 pole_dec = 27.12825 / k_deg;
  const f64 pole[3] = {std::cos(pole_dec) * std::cos(pole_ra),
                       std::cos(pole_dec) * std::sin(pole_ra), std::sin(pole_dec)};
  f32 previous = -100.0f;
  for (const scene_gen::Star& s : stars) {
    CHECK(s.magnitude >= previous);  // brightest first
    previous = s.magnitude;
    const f64 len =
        std::sqrt(static_cast<f64>(s.direction.x * s.direction.x + s.direction.y * s.direction.y +
                                   s.direction.z * s.direction.z));
    CHECK(std::fabs(len - 1.0) < 1e-5);
    const f64 lum = 0.2126 * static_cast<f64>(s.color.x) + 0.7152 * static_cast<f64>(s.color.y) +
                    0.0722 * static_cast<f64>(s.color.z);
    CHECK(std::fabs(lum - 1.0) < 1e-4);
    if (s.magnitude < 3.0f) ++brighter_than_3;
    if (s.magnitude > 5.0f) {
      ++faint;
      const f64 b = std::asin(static_cast<f64>(s.direction.x) * pole[0] +
                              static_cast<f64>(s.direction.y) * pole[1] +
                              static_cast<f64>(s.direction.z) * pole[2]) *
                    k_deg;
      if (std::fabs(b) < 15.0) ++faint_in_band;
    }
  }
  CHECK(stars[0].magnitude < -1.0f);  // a Sirius
  CHECK(brighter_than_3 > 140);
  CHECK(brighter_than_3 < 200);
  // A band 15 degrees either side of the galactic equator is 26% of the sky; the faint stars crowd
  // it.
  const f64 share = static_cast<f64>(faint_in_band) / static_cast<f64>(faint);
  CHECK(share > 0.35);
  MESSAGE("stars: " << stars.size() << ", brighter than 3: " << brighter_than_3
                    << ", faint stars within 15 degrees of the galactic plane: " << share);
  // The same table every time it is made.
  const Vector<scene_gen::Star> again = earth_stars(1, k_star_magnitude_limit);
  REQUIRE(again.size() == stars.size());
  bool same = true;
  for (u32 i = 0; i < stars.size(); ++i) {
    same = same && again[i].direction.x == stars[i].direction.x &&
           again[i].magnitude == stars[i].magnitude && again[i].color.z == stars[i].color.z;
  }
  CHECK(same);
  CHECK(star_irradiance(-26.74) == doctest::Approx(1.0));
  CHECK(star_irradiance(0.0) == doctest::Approx(1.99e-11).epsilon(0.01));
}

TEST_CASE("sky: the provider \"earth\" through the registry, on the world's clock") {
  const scene_gen::SkyProviderDesc* desc =
      scene_gen::GeneratorRegistry::global().find_sky(scene_gen::k_default_sky);
  REQUIRE(desc != nullptr);
  scene::Sky entry;
  entry.latitude_deg = 31.1f;
  entry.day_of_year = 250.0f;
  entry.moon_age_days = 12.4f;
  entry.turbidity = 2.5f;
  scene_gen::SkyProvider provider;
  std::string error;
  REQUIRE_MESSAGE(desc->make(entry, provider, &error), error);
  REQUIRE(provider.valid());
  CHECK(provider.stars().size() == star_count_for_magnitude(k_star_magnitude_limit));
  CHECK(provider.atmosphere().mie_g == doctest::Approx(0.76f));
  // The state is the ephemeris of the entry's calendar at the time, whose hour is the time of day
  // of game time: the clock the wind's day reads (hour 0 midnight).
  const f64 t = 1095.0 * 86400.0 + 15.25 * 3600.0;
  scene_gen::SkyState s;
  provider.state(t, s);
  CHECK(s.hour == doctest::Approx(15.25));
  CHECK(s.day_of_year == doctest::Approx(250.0 + 1095.0 + 15.25 / 24.0));
  Calendar c;
  c.latitude_deg = static_cast<f64>(entry.latitude_deg);
  c.day_of_year = static_cast<f64>(entry.day_of_year);
  c.moon_age_days = static_cast<f64>(entry.moon_age_days);
  Ephemeris e;
  ephemeris(c, t, e);
  CHECK(s.sun.x == static_cast<f32>(e.sun.x));
  CHECK(s.moon.y == static_cast<f32>(e.moon.y));
  CHECK(s.sun_radius > 4.4e-3f);
  CHECK(s.sun_radius < 4.8e-3f);
  CHECK(s.moon_radius > 4.2e-3f);
  CHECK(s.moon_radius < 4.9e-3f);
  // Without stars the table is empty.
  entry.stars = false;
  scene_gen::SkyProvider dark;
  REQUIRE(desc->make(entry, dark, &error));
  CHECK(dark.stars().empty());
  // Refusals say which field.
  entry.latitude_deg = 95.0f;
  scene_gen::SkyProvider refused;
  CHECK_FALSE(desc->make(entry, refused, &error));
  CHECK(error.find("latitude") != std::string::npos);
  entry.latitude_deg = 30.0f;
  entry.turbidity = 0.5f;
  CHECK_FALSE(desc->make(entry, refused, &error));
  CHECK(error.find("turbidity") != std::string::npos);
}

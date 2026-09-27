// The sun's day, with no device (lighting.h, "The sun's day"; docs/subsystems/renderer.md): a day
// that has not moved is the sun every frame had before there was a day, to the bit; the day is a
// circle round the pole through the start, a turn a game day, westward; and the sun fades over
// twilight and nowhere else.
#include <core/base/types.h>
#include <core/math/math.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr f64 k_half_turn = 3.14159265358979323846;
constexpr f64 k_deg = 180.0 / k_half_turn;

bool same_bits(const Vec3& a, const Vec3& b) {
  return std::bit_cast<u32>(a.x) == std::bit_cast<u32>(b.x) &&
         std::bit_cast<u32>(a.y) == std::bit_cast<u32>(b.y) &&
         std::bit_cast<u32>(a.z) == std::bit_cast<u32>(b.z);
}

f64 dot3(const Vec3& a, const f64 (&b)[3]) {
  return static_cast<f64>(a.x) * b[0] + static_cast<f64>(a.y) * b[1] + static_cast<f64>(a.z) * b[2];
}

f64 elevation_deg(const Vec3& d) { return std::asin(static_cast<f64>(d.y)) * k_deg; }

// The azimuth the way `--sun` spells it: in the ground plane from +x (east) towards +z (south).
f64 azimuth_deg(const Vec3& d) {
  const f64 a = std::atan2(static_cast<f64>(d.z), static_cast<f64>(d.x)) * k_deg;
  return a < 0.0 ? a + 360.0 : a;
}

}  // namespace

TEST_CASE("lighting: a day that has not moved is the sun every frame had, to the bit") {
  // The vector every frame was lit by before the sun could move (2026-09-27), and before it had a
  // day (this change): a run whose sun rate is zero draws exactly what it drew.
  const Vec3 old = normalize(Vec3{0.4f, 0.8f, 0.45f});
  const SunArc arc;
  CHECK(same_bits(sun_on_arc(arc, 0.0), old));
  const RenderSettings plain;
  const LightingOptions options = lighting_options(plain, 0.0);
  CHECK(same_bits(options.sun, old));
  CHECK(options.sun_intensity == 1.0f);
  CHECK(same_bits(lighting_options(plain).sun, old));  // the default argument is the start

  // What the frame hands the resolve and the reference: the sun, and the intensity it always had.
  const SceneData scene;
  FrameLighting now;
  frame_lighting(scene, 0, options, now);
  FrameLighting before;
  frame_lighting(scene, 0, LightingOptions{}, before);
  CHECK(same_bits(now.sun.xyz(), old));
  CHECK(std::bit_cast<u32>(now.sun.w) == std::bit_cast<u32>(1.0f));
  CHECK(same_bits(now.sun.xyz(), before.sun.xyz()));
  CHECK(now.sun.w == before.sun.w);

  // A `--sun` of any elevation — below the horizon too, which the day would fade — and any tilt is
  // `sun_direction` at the start, shining at 1: the frame it drew before the day existed.
  const f32 starts[][2] = {{120.0f, 20.0f}, {-30.0f, 80.0f}, {270.0f, 1.0f}, {10.0f, -12.0f}};
  for (const auto& s : starts) {
    RenderSettings settings;
    settings.sun_azimuth_deg = s[0];
    settings.sun_elevation_deg = s[1];
    const Vec3 was = sun_direction(static_cast<f64>(s[0]), static_cast<f64>(s[1]));
    const LightingOptions at_start = lighting_options(settings, 0.0);
    CHECK(same_bits(at_start.sun, was));
    CHECK(at_start.sun_intensity == 1.0f);
    for (const f64 tilt : {0.0, 40.0, 90.0}) {
      SunArc tilted = sun_arc(settings);
      tilted.tilt_deg = tilt;
      CHECK(same_bits(sun_on_arc(tilted, 0.0), was));
    }
  }
}

TEST_CASE("lighting: the sun's day is a circle round the pole through the start, westward") {
  const SunArc arc;  // the default start at the default tilt
  const Vec3 start = sun_on_arc(arc, 0.0);
  const f64 tilt = arc.tilt_deg / k_deg;
  const f64 pole[3] = {0.0, std::sin(tilt), -std::cos(tilt)};
  const f64 from_pole = dot3(start, pole);  // the sine of the declination
  const f64 declination = std::asin(from_pole) * k_deg;

  // A circle: unit length, and the same angle from the pole all day.
  f64 worst_length = 0.0;
  f64 worst_circle = 0.0;
  f64 highest = -1.0;
  Vec3 noon{};
  f64 set_azimuth = 0.0;
  f64 above_s = 0.0;
  Vec3 previous = start;
  f64 worst_step = 0.0;
  for (u32 i = 1; i <= 1440; ++i) {  // every game minute of a day
    const f64 t = static_cast<f64>(i) * 60.0;
    const Vec3 d = sun_on_arc(arc, t);
    worst_length = std::fmax(worst_length, std::fabs(static_cast<f64>(length(d)) - 1.0));
    worst_circle = std::fmax(worst_circle, std::fabs(dot3(d, pole) - from_pole));
    if (d.y > highest) {
      highest = d.y;
      noon = d;
    }
    if (previous.y >= 0.0f && d.y < 0.0f) set_azimuth = azimuth_deg(d);
    if (d.y >= 0.0f) above_s += 60.0;
    // Continuous: a minute of game time turns it by at most a minute of the day's angle.
    const f64 step = std::acos(std::fmin(1.0, static_cast<f64>(dot(previous, d))));
    worst_step = std::fmax(worst_step, step);
    previous = d;
  }
  CHECK(worst_length <= 1.0e-6);
  CHECK(worst_circle <= 1.0e-6);
  CHECK(worst_step <= 2.0 * k_half_turn / 1440.0 + 1.0e-6);
  // A turn a day: back where it started, to a float's rounding.
  CHECK(static_cast<f64>(length(sun_on_arc(arc, k_sun_day_s) - start)) <= 1.0e-6);
  CHECK(static_cast<f64>(length(sun_on_arc(arc, 365.0 * k_sun_day_s) - start)) <= 1.0e-6);
  // A second in, it has moved a second's worth of a turn round the pole, and no more.
  const f64 radius = std::sqrt(1.0 - from_pole * from_pole);
  const f64 second = static_cast<f64>(length(sun_on_arc(arc, 1.0) - start));
  CHECK(second == doctest::Approx(2.0 * k_half_turn / k_sun_day_s * radius).epsilon(0.05));

  // Westward: the default start is a morning sun in the south-east, so an hour later it is higher
  // and further south; it culminates due south at 90 - tilt + declination; and twelve hours after
  // the start it is well below the horizon, and dark.
  const Vec3 hour = sun_on_arc(arc, 3'600.0);
  CHECK(hour.y > start.y);
  CHECK(azimuth_deg(hour) > azimuth_deg(start));
  CHECK(elevation_deg(noon) == doctest::Approx(90.0 - arc.tilt_deg + declination).epsilon(0.001));
  CHECK(std::fabs(noon.x) < 0.01f);
  CHECK(noon.z > 0.0f);
  const Vec3 night = sun_on_arc(arc, 12.0 * 3'600.0);
  CHECK(elevation_deg(night) < -30.0);
  CHECK(sun_intensity(night) == 0.0f);
  char line[200];
  std::snprintf(line, sizeof(line),
                "the default start's day at tilt %.0f: declination %.2f deg, culminates at %.2f "
                "deg due south, sets at azimuth %.1f deg, %.1f hours above the horizon",
                arc.tilt_deg, declination, elevation_deg(noon), set_azimuth, above_s / 3'600.0);
  MESSAGE(std::string(line));

  // The tilt is the latitude. At the equator a sun rising due east climbs straight up and is
  // overhead six hours later; at a pole it circles at one elevation all day.
  SunArc equator;
  equator.azimuth_deg = 0.0;
  equator.elevation_deg = 0.0;
  equator.tilt_deg = 0.0;
  CHECK(sun_on_arc(equator, 6.0 * 3'600.0).y > 0.99999f);
  CHECK(std::fabs(sun_on_arc(equator, 3'600.0).z) < 1.0e-6f);  // straight up, not south
  SunArc pole_arc;
  pole_arc.elevation_deg = 20.0;
  pole_arc.tilt_deg = 90.0;
  for (u32 h = 1; h < 24; ++h) {
    CHECK(elevation_deg(sun_on_arc(pole_arc, h * 3'600.0)) == doctest::Approx(20.0).epsilon(1e-4));
  }
}

TEST_CASE("lighting: the sun fades over twilight, and only there") {
  // Full at and above the horizon, nothing past k_sun_twilight_deg below it, and in between a
  // smoothstep that only ever falls as the sun sinks.
  const auto at = [](f64 elevation) {
    const f64 e = elevation / k_deg;
    return sun_intensity(Vec3{static_cast<f32>(std::cos(e)), static_cast<f32>(std::sin(e)), 0.0f});
  };
  CHECK(at(90.0) == 1.0f);
  CHECK(at(0.5) == 1.0f);
  CHECK(at(0.0) == 1.0f);
  CHECK(at(-k_sun_twilight_deg) == 0.0f);
  CHECK(at(-30.0) == 0.0f);
  CHECK(at(-90.0) == 0.0f);
  CHECK(at(-3.0) == doctest::Approx(0.5).epsilon(0.01));
  f32 last = 1.0f;
  for (i32 i = 0; i <= 700; ++i) {
    const f32 v = at(-0.01 * i);
    CHECK(v <= last);
    CHECK(v >= 0.0f);
    last = v;
  }
  // The day's frames shine by it; the start of the day shines at 1.
  RenderSettings low;
  low.sun_azimuth_deg = 180.0f;  // due west, two degrees up: ten minutes or so before sunset
  low.sun_elevation_deg = 2.0f;
  f32 previous = 1.0f;
  bool faded = false;
  for (u32 m = 1; m <= 600; ++m) {  // ten game hours, a game minute at a time
    const LightingOptions o = lighting_options(low, 60.0 * m);
    CHECK(o.sun_intensity == sun_intensity(o.sun));
    faded = faded || o.sun_intensity < 1.0f;
    if (o.sun.y < 0.0f && previous < 1.0f) CHECK(o.sun_intensity <= previous + 1.0e-6f);
    previous = o.sun_intensity;
  }
  CHECK(faded);
}

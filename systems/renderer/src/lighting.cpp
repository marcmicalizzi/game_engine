#include <foundation/tunables/tunables.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>

#include <algorithm>
#include <cmath>

namespace engine::renderer {

namespace {

// Read when a frame is lit (docs/subsystems/apps.md, "--sun"; renderer.md, "The dunes in
// time-lapse"). A request's own sun (`RenderSettings::sun_*`, `engine-view --sun`) wins.
tunables::Float sun_azimuth_deg{"renderer.sun.azimuth_deg", k_default_sun_azimuth_deg, -360.0,
                                360.0,
                                "Where the renderer's stand-in sun stands: degrees in the ground "
                                "plane from +x towards +z"};
tunables::Float sun_elevation_deg{"renderer.sun.elevation_deg", k_default_sun_elevation_deg, -90.0,
                                  90.0,
                                  "Where the renderer's stand-in sun stands: degrees above the "
                                  "horizon"};
// The day (lighting.h, "The sun's day"; docs/subsystems/renderer.md). The tilt is read when a frame
// is lit, the rate by the host that runs the day.
tunables::Float sun_arc_tilt_deg{"renderer.sun.arc_tilt_deg", k_default_sun_arc_tilt_deg, 0.0, 90.0,
                                 "The tilt of the stand-in sun's daily arc: the celestial pole's "
                                 "elevation over the northern (-z) horizon, degrees, which is the "
                                 "latitude (0 the equator, 90 a pole)"};
tunables::Float sun_rate{"renderer.sun.rate", 0.0, 0.0, 1.0e7,
                         "Game seconds per real second the stand-in sun's day runs at (0: the sun "
                         "stands still); engine-view's --sun-rate and [ and ] set it for a run"};

constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_rad = k_pi / 180.0;

}  // namespace

Vec3 sun_direction(f64 azimuth_deg, f64 elevation_deg) noexcept {
  // The defaults are the vector every frame had before the sun could move, to the bit.
  if (azimuth_deg == k_default_sun_azimuth_deg && elevation_deg == k_default_sun_elevation_deg)
    return normalize(Vec3{0.4f, 0.8f, 0.45f});
  const f64 az = azimuth_deg * k_rad;
  const f64 el = elevation_deg * k_rad;
  return normalize(Vec3{static_cast<f32>(std::cos(el) * std::cos(az)),
                        static_cast<f32>(std::sin(el)),
                        static_cast<f32>(std::cos(el) * std::sin(az))});
}

SunArc sun_arc(const RenderSettings& settings) {
  SunArc arc;
  arc.azimuth_deg = settings.sun_azimuth_deg.has_value()
                        ? static_cast<f64>(*settings.sun_azimuth_deg)
                        : sun_azimuth_deg.get();
  arc.elevation_deg = settings.sun_elevation_deg.has_value()
                          ? static_cast<f64>(*settings.sun_elevation_deg)
                          : sun_elevation_deg.get();
  arc.tilt_deg = sun_arc_tilt_deg.get();
  return arc;
}

Vec3 sun_on_arc(const SunArc& arc, f64 time_s) noexcept {
  // The start of the day is the sun a frame had before there was a day, to the bit.
  if (time_s == 0.0 || !std::isfinite(time_s)) {
    return sun_direction(arc.azimuth_deg, arc.elevation_deg);
  }
  // The start in f64, by the formula `sun_direction` evaluates, and the pole: north (-z) raised by
  // the tilt towards the zenith.
  const f64 az = arc.azimuth_deg * k_rad;
  const f64 el = arc.elevation_deg * k_rad;
  const f64 s[3] = {std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az)};
  const f64 tilt = std::clamp(arc.tilt_deg, 0.0, 90.0) * k_rad;
  const f64 p[3] = {0.0, std::sin(tilt), -std::cos(tilt)};
  // The day's turn, reduced to the part of a turn first so a year of game time — 365 turns — keeps
  // the angle of the day exact. Westward is a negative turn about the pole: it carries a sun in
  // the east up and towards the south (p x east = (0, -cos, -sin) is down and north).
  const f64 turns = time_s / k_sun_day_s;
  const f64 angle = -2.0 * k_pi * (turns - std::floor(turns));
  const f64 c = std::cos(angle);
  const f64 sn = std::sin(angle);
  // Rodrigues: s cos + (p x s) sin + p (p . s)(1 - cos), which keeps the angle from the pole — the
  // declination — and the length.
  const f64 ps = p[0] * s[0] + p[1] * s[1] + p[2] * s[2];
  const f64 cross[3] = {p[1] * s[2] - p[2] * s[1], p[2] * s[0] - p[0] * s[2],
                        p[0] * s[1] - p[1] * s[0]};
  const f64 k = ps * (1.0 - c);
  return normalize(Vec3{static_cast<f32>(s[0] * c + cross[0] * sn + p[0] * k),
                        static_cast<f32>(s[1] * c + cross[1] * sn + p[1] * k),
                        static_cast<f32>(s[2] * c + cross[2] * sn + p[2] * k)});
}

f32 sun_intensity(const Vec3& direction) noexcept {
  const f64 y = static_cast<f64>(direction.y);
  if (y >= 0.0) return 1.0f;
  const f64 below = std::sin(k_sun_twilight_deg * k_rad);
  if (!(y > -below)) return 0.0f;
  const f64 x = (y + below) / below;  // 0 at the end of twilight, 1 at the horizon
  return static_cast<f32>(x * x * (3.0 - 2.0 * x));
}

f64 sun_rate_tunable() { return sun_rate.get(); }

LightingOptions lighting_options(const RenderSettings& settings, f64 sun_time_s) {
  LightingOptions out;
  out.lights = settings.lights;
  out.orbit = settings.orbit_lights;
  out.sun = sun_on_arc(sun_arc(settings), sun_time_s);
  // Today's sun shines at 1 wherever it stands until its day moves it (lighting.h).
  out.sun_intensity = sun_time_s == 0.0 ? 1.0f : sun_intensity(out.sun);
  return out;
}

void frame_lighting(const SceneData& scene, u64 frame_index, const LightingOptions& options,
                    FrameLighting& out) {
  out = FrameLighting{};
  out.sky = k_sky;
  out.sun = Vec4{options.sun, options.sun_intensity};
  out.ground = Vec4{scene.ground_albedo, 0.0f};
  // The ray's offset (lighting.h, `k_shadow_bias_*`): the float part sized by the scene's reach,
  // the grid part left to the shader, which knows whose grid a pixel is on.
  out.shadow_bias = k_shadow_bias_relative * (length(scene.center) + scene.radius);
  out.shadow_bias_steps = k_shadow_bias_steps;
  if (!options.lights) return;

  const Vec3 center = scene.center;
  const f32 radius = scene.radius;
  const f32 orbit = 1.35f * radius;
  const f32 angle = options.orbit ? static_cast<f32>(frame_index) * 0.013f : 0.0f;
  out.lights[0].position_radius =
      Vec4{center + Vec3{std::cos(angle) * orbit, 0.70f * radius, std::sin(angle) * orbit},
           4.0f * radius};
  out.lights[0].color_intensity = Vec4{1.0f, 0.78f, 0.55f, orbit * orbit};
  out.lights[1].position_radius =
      Vec4{center + Vec3{-std::cos(angle * 0.7f) * orbit, -0.35f * radius,
                         -std::sin(angle * 0.7f) * orbit},
           4.0f * radius};
  out.lights[1].color_intensity = Vec4{0.50f, 0.68f, 1.0f, 0.8f * orbit * orbit};
  out.light_count = k_frame_lights;
}

}  // namespace engine::renderer

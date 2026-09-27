#include <foundation/tunables/tunables.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>

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

}  // namespace

Vec3 sun_direction(f64 azimuth_deg, f64 elevation_deg) noexcept {
  // The defaults are the vector every frame had before the sun could move, to the bit.
  if (azimuth_deg == k_default_sun_azimuth_deg && elevation_deg == k_default_sun_elevation_deg)
    return normalize(Vec3{0.4f, 0.8f, 0.45f});
  constexpr f64 k_rad = 3.14159265358979323846 / 180.0;
  const f64 az = azimuth_deg * k_rad;
  const f64 el = elevation_deg * k_rad;
  return normalize(Vec3{static_cast<f32>(std::cos(el) * std::cos(az)),
                        static_cast<f32>(std::sin(el)),
                        static_cast<f32>(std::cos(el) * std::sin(az))});
}

LightingOptions lighting_options(const RenderSettings& settings) {
  LightingOptions out;
  out.lights = settings.lights;
  out.orbit = settings.orbit_lights;
  const f64 az = settings.sun_azimuth_deg.has_value() ? static_cast<f64>(*settings.sun_azimuth_deg)
                                                      : sun_azimuth_deg.get();
  const f64 el = settings.sun_elevation_deg.has_value()
                     ? static_cast<f64>(*settings.sun_elevation_deg)
                     : sun_elevation_deg.get();
  out.sun = sun_direction(az, el);
  return out;
}

void frame_lighting(const SceneData& scene, u64 frame_index, const LightingOptions& options,
                    FrameLighting& out) {
  out = FrameLighting{};
  out.sky = k_sky;
  out.sun = Vec4{options.sun, 1.0f};
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

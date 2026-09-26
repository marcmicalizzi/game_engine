#include <systems/renderer/lighting.h>
#include <systems/renderer/scene.h>

#include <cmath>

namespace engine::renderer {

void frame_lighting(const SceneData& scene, u64 frame_index, bool lights, FrameLighting& out) {
  out = FrameLighting{};
  out.sky = k_sky;
  out.sun = Vec4{normalize(Vec3{0.4f, 0.8f, 0.45f}), 1.0f};
  out.ground = Vec4{scene.ground_albedo, 0.0f};
  out.shadow_bias = 1.0e-3f * scene.radius;
  if (!lights) return;

  const Vec3 center = scene.center;
  const f32 radius = scene.radius;
  const f32 orbit = 1.35f * radius;
  const f32 angle = static_cast<f32>(frame_index) * 0.013f;
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

#include <systems/renderer/eye_shell.h>

namespace engine::renderer {

bool fit_eye_shell_volume(Vec3 eye, f32 radius, CasterVolume& out) noexcept {
  out = CasterVolume{};
  if (!(radius > 0.0f)) return false;
  // The cube's six faces, inward, each `radius` from the eye: a sphere wholly outside one of them
  // is wholly outside the reach too, so the planes only let the pass stop sooner; the reach is the
  // test that decides.
  const Vec3 axes[3] = {Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}};
  u32 n = 0;
  for (const Vec3& axis : axes) {
    for (const f32 sign : {-1.0f, 1.0f}) {
      Plane& p = out.planes[n++];
      p.normal = axis * sign;
      p.d = radius - dot(p.normal, eye);
    }
  }
  out.plane_count = n;
  out.reach = Vec4{eye.x, eye.y, eye.z, radius};
  return true;
}

}  // namespace engine::renderer

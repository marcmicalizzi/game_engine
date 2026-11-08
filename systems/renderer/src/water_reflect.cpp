#include <systems/renderer/water_reflect.h>

#include <domain/gfx/visibility_resolve.h>
#include <foundation/tunables/tunables.h>

namespace engine::renderer {

namespace {

tunables::Int water_reflect{
    "renderer.water.reflect", 1, 0, 2,
    "The water reflects the scene through one ray a water pixel where the device traces: 0 off (the "
    "sky alone), 1 on, 2 on in even frames and off in odd ones, to measure both from one flight "
    "(water.md, Shading; R129)"};
tunables::Bool water_reflect_volume{
    "renderer.water.reflect_volume", true,
    "Build what the camera reflected in the water's plane sees into the frame's structures as "
    "casters, so the water reflects what no view holds (water.md, Shading; R129)"};
tunables::Int water_reflect_shadow{
    "renderer.water.reflect_shadow", 1, 0, 2,
    "The reflected surface's sun shadow: 0 none, 1 one ray at half rate (a checkerboard of each "
    "2x2 block), 2 one ray every pixel (water.md, Shading; R129)"};

}  // namespace

bool fit_water_reflect_volume(const Mat4& view_proj, f32 level, f32 reach,
                              const CasterBounds& bounds, CasterVolume& out) noexcept {
  out = CasterVolume{};
  // The eye is the frame's origin: under the plane it sees no reflection of what stands above.
  if (!(level < 0.0f)) return false;
  Plane frustum[6];
  if (!closed_frustum(view_proj, bounds, frustum)) return false;
  // A point q is in the mirrored frustum when its mirror p = (q.x, 2 level - q.y, q.z) is in the
  // frustum: n.p + d = (n.x, -n.y, n.z).q + (d + 2 level n.y).
  for (u32 i = 0; i < 6; ++i) {
    const Plane& p = frustum[i];
    out.planes[i] = Plane{Vec3{p.normal.x, -p.normal.y, p.normal.z},
                          p.d + 2.0f * level * p.normal.y};
  }
  // Only what stands above the water is in it.
  out.planes[6] = Plane{Vec3{0.0f, 1.0f, 0.0f}, -level};
  out.plane_count = 7;
  if (reach > 0.0f) out.reach = Vec4{0.0f, 0.0f, 0.0f, reach};
  return true;
}

bool water_reflect_tunable(u64 frame_index) noexcept {
  switch (water_reflect.get()) {
    case 0: return false;
    case 2: return (frame_index & 1u) == 0u;
    default: return true;
  }
}

bool water_reflect_volume_tunable() noexcept { return water_reflect_volume.get(); }

u32 water_reflect_shadow_flags() noexcept {
  switch (water_reflect_shadow.get()) {
    case 0: return gfx::k_shadow_water_reflect_unshadowed;
    case 2: return gfx::k_shadow_water_reflect_full;
    default: return 0u;
  }
}

}  // namespace engine::renderer

#pragma once

// The matrix a pixel's direction goes through (shaders/view_ray.slang, `view_ray_direction`;
// docs/subsystems/gfx.md, "The sky", the fifth numerical lesson). `SkyView::clip_to_ray`,
// `RayVisibilityParams::clip_to_ray` and `PathTraceParams::clip_to_ray` all hold one of these.
//
// **Why not the inverse view-projection.** A direction is a function of the camera's rotation and
// projection alone, and the inverse of the whole view-projection carries the eye's world position
// in it: unprojecting a point near the eye in world coordinates and subtracting the eye is a
// difference of two numbers the size of the camera's distance from the origin, which at two
// kilometres is a quarter of a millimetre of noise in a point a decimetre away. This matrix is the
// inverse of the projection times the view's rotation, with the eye at the origin, so a pixel's
// direction is the same bits wherever the camera stands.
//
// It is exact for every projection a `renderer::ViewSet` draws with: the symmetric one, a
// surround's off-axis monitors (the off-axis rectangle is in the projection, the monitor's yaw in
// the rotation), and a Panini view, whose output pixel the resolve first maps into its rectilinear
// source — which is what this matrix inverts. A description by basis vectors and frustum extents
// would be a second copy of the projection to keep in step with the first.

#include <core/math/math.h>

namespace engine::gfx {

// `view` is a rigid world-to-eye matrix (`look_at`); its translation is dropped, not inverted.
inline Mat4 clip_to_ray(const Mat4& projection, const Mat4& view) noexcept {
  Mat4 rotation = view;
  rotation.at(0, 3) = 0.0f;
  rotation.at(1, 3) = 0.0f;
  rotation.at(2, 3) = 0.0f;
  return inverse(projection * rotation);
}

}  // namespace engine::gfx

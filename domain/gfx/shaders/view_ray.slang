// The eye's ray through a pixel (docs/subsystems/gfx.md, "The sky", the fifth numerical lesson;
// include/domain/gfx/view_ray.h builds the matrix). One function, and every reader of a pixel's
// direction calls it: the resolve's uncovered pixels (sky.slang, `sky_pixel_direction`), the ray
// path's primary rays (ray_visibility.slang) and the reference path tracer's camera rays
// (path_trace.slang).
//
// **The direction is a function of the camera's rotation and projection alone.** `clip_to_ray` is
// the inverse of the projection times the view's rotation, with the eye at the origin and no
// translation in it, so nothing here is a world position. It used to be the inverse of the whole
// view-projection: a point a decimetre in front of the eye, unprojected in world coordinates, less
// the eye. Two kilometres from the origin a float holds a quarter of a millimetre, and that
// difference of two positions a decimetre apart was wrong by parts in a thousand — a tenth of a
// degree, several pixels at 11520 x 2160, and different every time the camera moved: the sky
// jittered behind the ground. Fifty kilometres out it was degrees.
//
// Depth 0 is the point at infinity under a reversed-Z projection with no far plane: its w is zero
// and its xyz is the direction itself, so there is no division by w either. With a far plane
// (depth 0 there is the far plane, w > 0) xyz is still a point on the ray from the eye, and the
// normalized answer is the same; nothing else here depends on which.
//
// No bindings and no entry points, like brdf.slang and sky.slang.

#ifndef ENGINE_VIEW_RAY_SLANG
#define ENGINE_VIEW_RAY_SLANG

// The world direction of the eye's ray through clip-space `ndc` (x right, y up, each in [-1, 1]).
float3 view_ray_direction(float4x4 clip_to_ray, float2 ndc) {
  return normalize(mul(clip_to_ray, float4(ndc, 0.0, 1.0)).xyz);
}

#endif  // ENGINE_VIEW_RAY_SLANG

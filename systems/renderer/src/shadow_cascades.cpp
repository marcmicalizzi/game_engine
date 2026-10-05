#include <systems/renderer/shadow_cascades.h>
#include <systems/renderer/view_set.h>

#include <cmath>

namespace engine::renderer {

namespace {

struct Dvec {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};

Dvec dvec(Vec3 v) noexcept {
  return {static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)};
}

// The largest squared distance from `centre` to any of `points`.
f64 farthest(const Dvec* points, u32 count, Dvec centre) noexcept {
  f64 worst = 0.0;
  for (u32 i = 0; i < count; ++i) {
    const f64 dx = points[i].x - centre.x;
    const f64 dy = points[i].y - centre.y;
    const f64 dz = points[i].z - centre.z;
    const f64 d = dx * dx + dy * dy + dz * dz;
    worst = d > worst ? d : worst;
  }
  return worst;
}

// Up to a 64th of the radius's power of two, so float noise in the corners cannot move a
// cascade's texel size from one frame to the next.
f32 quantize_radius(f32 r) noexcept {
  if (!(r > 0.0f)) return r;
  const f32 step = std::exp2(std::floor(std::log2(r)) - 6.0f);
  return std::ceil(r / step) * step;
}

// `gfx::shadow_snap` **in the world's light space, in f64** (renderer.md, "Cascaded shadow maps"):
// the centre `center` of the frame whose eye is `eye`, put back in the world and projected on the
// light's right and up, made whole multiples of `texel` there, and the move that took expressed in
// the frame again. The grid is the world's, so a texel is the same piece of the world for as long
// as the sun stands still, wherever the eye is and whichever cell it is in. In f64 because the
// coordinates are as large as the distance from the world's origin: 1e7 texels of a centimetre
// 100 km out, where f64 still has a hundred-millionth of a texel to spare (and a float32 had 3 cm).
Vec3 snap_in_world(const gfx::ShadowLight& light, Vec3 center, f32 texel, WorldPos eye) noexcept {
  if (!(texel > 0.0f)) return center;
  const DVec3 from = (eye - WorldPos::origin()) + DVec3{center};
  const DVec3 right{light.right};
  const DVec3 up{light.up};
  const f64 t = static_cast<f64>(texel);
  const f64 x = dot(from, right);
  const f64 y = dot(from, up);
  const f64 sx = std::round(x / t) * t;
  const f64 sy = std::round(y / t) * t;
  return narrow(DVec3{center} + right * (sx - x) + up * (sy - y));
}

}  // namespace

void fit_shadow_cascades(const ViewSet& views, const Camera& camera, Vec3 towards_sun,
                         WorldPos scene_center_world, f32 scene_radius, const ShadowFit& fit,
                         ShadowCascades& out) noexcept {
  out = ShadowCascades{};
  out.resolution = fit.resolution > 0 ? fit.resolution : 1u;
  out.light = gfx::shadow_light(towards_sun);
  const u32 wanted = fit.cascades < 1                            ? 1u
                     : fit.cascades > gfx::k_max_shadow_cascades ? gfx::k_max_shadow_cascades
                                                                 : fit.cascades;
  // The frame's space: the eye at the origin (ADR-0053). The scene's centre is measured from it in
  // f64 and rounded once; everything below is relative to the eye.
  const Vec3 eye{};
  const Vec3 forward = camera_forward(camera);
  const Vec3 scene_center = relative(scene_center_world, camera.position);
  const f32 to_center = length(scene_center - eye);
  const f32 far = fit.distance > 0.0f ? fit.distance : to_center + scene_radius;
  // Where geometry can begin: the scene's near side, or the camera's near plane when the camera is
  // inside the bounds. The first slice starts there, and the splits are logarithmic from it —
  // but never from under a thousandth of the distance, which is what a camera standing inside a
  // terrain would otherwise spend its first cascade on.
  f32 first = to_center - scene_radius;
  first = first > camera.znear ? first : camera.znear;
  if (!(first < far)) first = far * 0.5f;
  f32 near = first > far * k_shadow_near_ratio ? first : far * k_shadow_near_ratio;
  if (!(near < far)) near = far * 0.5f;
  out.splits[0] = first < near ? first : near;
  for (u32 i = 1; i <= wanted; ++i) {
    out.splits[i] = near * std::pow(far / near, static_cast<f32>(i) / static_cast<f32>(wanted));
  }
  out.splits[wanted] = far;

  // Each view's frame and its frustum as tangents at unit distance, exactly as ViewSet::update
  // builds its matrices: the head's forward turned by the view's yaw about the head's up.
  const Vec3 world_up{0.0f, 1.0f, 0.0f};
  const Vec3 head_right = normalize(cross(forward, world_up));
  const Vec3 head_up = cross(head_right, forward);
  struct ViewFrame {
    Vec3 forward, right, up;
    f32 left, right_tan, bottom, top;
  };
  ViewFrame frames[k_max_views];
  const u32 view_count = views.size() < k_max_views ? views.size() : k_max_views;
  for (u32 v = 0; v < view_count; ++v) {
    const View& view = views[v];
    ViewFrame& f = frames[v];
    f.forward = view.yaw == 0.0f
                    ? forward
                    : normalize(forward * std::cos(view.yaw) + head_right * std::sin(view.yaw));
    f.right = normalize(cross(f.forward, view.yaw == 0.0f ? world_up : head_up));
    f.up = cross(f.right, f.forward);
    if (view.symmetric) {
      const f32 ty = std::tan(camera.fov_y * 0.5f);
      f.left = -ty * view.aspect;
      f.right_tan = ty * view.aspect;
      f.bottom = -ty;
      f.top = ty;
    } else {
      f.left = view.left;
      f.right_tan = view.right;
      f.bottom = view.bottom;
      f.top = view.top;
    }
  }

  const Dvec d_eye = dvec(eye);
  const Dvec d_forward = dvec(forward);
  for (u32 c = 0; c < wanted; ++c) {
    // The slice's corners in every view.
    Dvec corners[k_max_views * 8];
    u32 count = 0;
    for (u32 v = 0; v < view_count; ++v) {
      const ViewFrame& f = frames[v];
      for (u32 end = 0; end < 2; ++end) {
        const f32 d = out.splits[c + end];
        for (u32 k = 0; k < 4; ++k) {
          const f32 tx = (k & 1) != 0 ? f.right_tan : f.left;
          const f32 ty = (k & 2) != 0 ? f.top : f.bottom;
          corners[count++] = dvec(eye + f.forward * d + f.right * (tx * d) + f.up * (ty * d));
        }
      }
    }
    // The smallest sphere centred on the forward axis that holds them: the farthest corner's
    // distance is convex along the axis, so a ternary search over the corners' own span finds it.
    f64 lo = 1.0e300;
    f64 hi = -1.0e300;
    for (u32 i = 0; i < count; ++i) {
      const f64 t = (corners[i].x - d_eye.x) * d_forward.x +
                    (corners[i].y - d_eye.y) * d_forward.y + (corners[i].z - d_eye.z) * d_forward.z;
      lo = t < lo ? t : lo;
      hi = t > hi ? t : hi;
    }
    auto at = [&](f64 t) {
      return Dvec{d_eye.x + d_forward.x * t, d_eye.y + d_forward.y * t, d_eye.z + d_forward.z * t};
    };
    for (u32 iteration = 0; iteration < 80; ++iteration) {
      const f64 a = lo + (hi - lo) / 3.0;
      const f64 b = hi - (hi - lo) / 3.0;
      if (farthest(corners, count, at(a)) <= farthest(corners, count, at(b))) {
        hi = b;
      } else {
        lo = a;
      }
    }
    const f64 t = 0.5 * (lo + hi);
    const Dvec centre = at(t);
    Vec3 center{static_cast<f32>(centre.x), static_cast<f32>(centre.y), static_cast<f32>(centre.z)};
    f32 radius = quantize_radius(static_cast<f32>(std::sqrt(farthest(corners, count, centre))));
    // A sphere that would hold the whole scene is replaced by the scene's own bounds, which are
    // smaller and hold every receiver there is, so this cascade is the last one.
    const bool whole_scene = length(center - scene_center) + scene_radius <= radius;
    if (whole_scene) {
      center = scene_center;
      radius = quantize_radius(scene_radius);
    }
    if (!(radius > 0.0f)) radius = 1.0e-3f;  // a scene of one point still gets a cascade
    const f32 texel = 2.0f * radius / static_cast<f32>(out.resolution);
    center = snap_in_world(out.light, center, texel, camera.position);
    // Every caster between the light and the sphere: to the far side of the scene towards the
    // light, and at least the sphere's own radius. A thousandth more so that a caster exactly on
    // the bounds is inside the depth range rather than on its clipping edge.
    const f32 beyond = dot(scene_center - center, out.light.direction) + scene_radius;
    const f32 towards = (beyond > radius ? beyond : radius) * 1.001f;
    out.cascades[c] = gfx::make_shadow_cascade(out.light, center, radius, towards, out.resolution,
                                               k_shadow_bias_texels);
    out.centers[c] = center;
    out.radii[c] = radius;
    out.scene_bounds[c] = whole_scene;
    out.count = c + 1;
    if (whole_scene) {
      out.splits[c + 1] = far;
      break;
    }
  }
}

void shadow_map_params(const ShadowCascades& cascades, u32 texture, u32 tiles, u32 sampler,
                       gfx::ShadowMapParams& out) noexcept {
  out = gfx::ShadowMapParams{};
  for (u32 c = 0; c < cascades.count && c < gfx::k_max_shadow_cascades; ++c)
    out.cascades[c] = cascades.cascades[c];
  out.light_right = Vec4{cascades.light.right, 0.0f};
  out.light_up = Vec4{cascades.light.up, 0.0f};
  out.light_dir = Vec4{cascades.light.direction, 0.0f};
  out.cascade_count = cascades.count;
  out.resolution = cascades.resolution;
  out.texture = texture;
  out.sampler = sampler;
  out.max_slope = k_shadow_max_slope;
  out.normal_offset = k_shadow_normal_offset_texels;
  out.tiles = tiles > cascades.count ? tiles : cascades.count;
}

}  // namespace engine::renderer

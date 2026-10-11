#include "viewport_math.h"

#include <cmath>

namespace engine::editor {

PointerScale pointer_scale(u32 window_width, u32 window_height, u32 pixel_width,
                           u32 pixel_height) noexcept {
  PointerScale s;
  if (window_width > 0 && pixel_width > 0)
    s.x = static_cast<f32>(pixel_width) / static_cast<f32>(window_width);
  if (window_height > 0 && pixel_height > 0)
    s.y = static_cast<f32>(pixel_height) / static_cast<f32>(window_height);
  return s;
}

namespace {

// Clip space to the frame's space at reversed-Z depth `depth`.
bool unproject(const Mat4& inverse_view_proj, f32 ndc_x, f32 ndc_y, f32 depth, Vec3& out) noexcept {
  const Vec4 p = inverse_view_proj * Vec4(ndc_x, ndc_y, depth, 1.0f);
  if (!(std::fabs(p.w) > 1.0e-20f)) return false;
  out = Vec3{p.x / p.w, p.y / p.w, p.z / p.w};
  return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

f32 distance_to_segment(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by, f32& along) noexcept {
  const f32 dx = bx - ax;
  const f32 dy = by - ay;
  const f32 len2 = dx * dx + dy * dy;
  along = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
  along = along < 0.0f ? 0.0f : (along > 1.0f ? 1.0f : along);
  const f32 qx = ax + dx * along - px;
  const f32 qy = ay + dy * along - py;
  return std::sqrt(qx * qx + qy * qy);
}

}  // namespace

bool pixel_ray(const Mat4& view_proj, f32 px, f32 py, u32 width, u32 height, Ray& out) noexcept {
  if (width == 0 || height == 0) return false;
  const f32 ndc_x = px / static_cast<f32>(width) * 2.0f - 1.0f;
  const f32 ndc_y = 1.0f - py / static_cast<f32>(height) * 2.0f;  // pixels run down, clip y up
  const Mat4 inv = inverse(view_proj);
  Vec3 near_point;
  Vec3 far_point;
  if (!unproject(inv, ndc_x, ndc_y, 1.0f, near_point) ||
      !unproject(inv, ndc_x, ndc_y, 0.5f, far_point))
    return false;
  const Vec3 d = far_point - near_point;
  const f32 len = length(d);
  if (!(len > 0.0f)) return false;
  out.origin = near_point;
  out.direction = d * (1.0f / len);
  return true;
}

bool project_point(const Mat4& view_proj, Vec3 p, u32 width, u32 height, f32& px,
                   f32& py) noexcept {
  const Vec4 clip = view_proj * Vec4(p, 1.0f);
  if (!(clip.w > 1.0e-6f)) return false;
  px = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<f32>(width);
  py = (0.5f - clip.y / clip.w * 0.5f) * static_cast<f32>(height);
  return true;
}

bool closest_on_axis(const Ray& ray, Vec3 origin, Vec3 axis, f32& t) noexcept {
  // The lines origin + t a and o + s d, both directions unit length: the nearest points solve
  // [1 -b; b -1] [t s] = [-a.w  -d.w] with b = a.d and w = origin - o.
  const Vec3 w = origin - ray.origin;
  const f32 b = dot(axis, ray.direction);
  const f32 denom = 1.0f - b * b;
  // Within a degree of parallel (sin^2 of one degree is 3.0e-4): the pointer cannot say where on
  // the axis it is, and the answer would run to infinity.
  if (!(denom > 3.0e-4f)) return false;
  const f32 aw = dot(axis, w);
  const f32 dw = dot(ray.direction, w);
  const f32 tt = (b * dw - aw) / denom;
  const f32 s = (dw - b * aw) / denom;
  if (!(s >= 0.0f) || !std::isfinite(tt)) return false;
  t = tt;
  return true;
}

Vec3 axis_direction(u32 axis) noexcept {
  switch (axis) {
    case 0: return Vec3{1.0f, 0.0f, 0.0f};
    case 1: return Vec3{0.0f, 1.0f, 0.0f};
    case 2: return Vec3{0.0f, 0.0f, 1.0f};
    default: return Vec3{};
  }
}

const char* axis_name(u32 axis) noexcept {
  switch (axis) {
    case 0: return "x";
    case 1: return "y";
    case 2: return "z";
    default: return "";
  }
}

GizmoHandles gizmo_handles(const Mat4& view_proj, Vec3 centre, f32 fov_y, u32 width, u32 height,
                           f32 pixels) noexcept {
  GizmoHandles h;
  h.centre = centre;
  if (width == 0 || height == 0) return h;
  const Vec4 clip = view_proj * Vec4(centre, 1.0f);
  if (!(clip.w > 1.0e-6f)) return h;
  // At depth w a picture `height` pixels tall spans 2 w tan(fov / 2) metres.
  h.length = pixels * 2.0f * clip.w * std::tan(fov_y * 0.5f) / static_cast<f32>(height);
  if (!project_point(view_proj, centre, width, height, h.cx, h.cy)) return h;
  h.visible = true;
  for (u32 a = 0; a < k_axis_count; ++a) {
    h.shown[a] = project_point(view_proj, centre + axis_direction(a) * h.length, width, height,
                               h.ex[a], h.ey[a]);
  }
  return h;
}

u32 hover_axis(const GizmoHandles& handles, f32 px, f32 py, f32 tolerance) noexcept {
  if (!handles.visible) return k_no_axis;
  u32 best = k_no_axis;
  f32 best_distance = tolerance;
  for (u32 a = 0; a < k_axis_count; ++a) {
    if (!handles.shown[a]) continue;
    f32 along = 0.0f;
    const f32 d =
        distance_to_segment(px, py, handles.cx, handles.cy, handles.ex[a], handles.ey[a], along);
    if (along < k_handle_dead_zone) continue;
    if (d <= best_distance) {
      best_distance = d;
      best = a;
    }
  }
  return best;
}

bool begin_axis_drag(u32 axis, WorldPos anchor, WorldPos eye, const Ray& ray,
                     AxisDrag& out) noexcept {
  if (axis >= k_axis_count) return false;
  f32 t = 0.0f;
  if (!closest_on_axis(ray, relative(anchor, eye), axis_direction(axis), t)) return false;
  out = AxisDrag{};
  out.axis = axis;
  out.anchor = anchor;
  out.start = t;
  out.delta = 0.0f;
  return true;
}

bool update_axis_drag(AxisDrag& drag, WorldPos eye, const Ray& ray) noexcept {
  if (drag.axis >= k_axis_count) return false;
  f32 t = 0.0f;
  if (!closest_on_axis(ray, relative(drag.anchor, eye), axis_direction(drag.axis), t)) return false;
  drag.delta = t - drag.start;
  return true;
}

WorldPos dragged_translation(WorldPos start, const AxisDrag& drag) noexcept {
  // Only the dragged axis is added to, so the other two keep their bits exactly.
  const f64 delta = static_cast<f64>(drag.delta);
  WorldPos out = start;
  if (drag.axis == 0) out.x += delta;
  if (drag.axis == 1) out.y += delta;
  if (drag.axis == 2) out.z += delta;
  return out;
}

}  // namespace engine::editor

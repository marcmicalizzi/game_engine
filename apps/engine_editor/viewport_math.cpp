#include "viewport_math.h"

#include <cmath>
#include <initializer_list>

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

void ring_basis(u32 axis, Vec3& u, Vec3& v) noexcept {
  // y x z = x, z x x = y, x x y = z.
  const u32 a = axis < k_axis_count ? axis : 1u;
  u = axis_direction((a + 1) % k_axis_count);
  v = axis_direction((a + 2) % k_axis_count);
}

GizmoRings gizmo_rings(const Mat4& view_proj, Vec3 centre, f32 fov_y, u32 width, u32 height,
                       f32 pixels) noexcept {
  GizmoRings r;
  r.centre = centre;
  if (width == 0 || height == 0) return r;
  const Vec4 clip = view_proj * Vec4(centre, 1.0f);
  if (!(clip.w > 1.0e-6f)) return r;
  r.radius = pixels * 2.0f * clip.w * std::tan(fov_y * 0.5f) / static_cast<f32>(height);
  if (!project_point(view_proj, centre, width, height, r.cx, r.cy)) return r;
  r.visible = true;
  for (u32 a = 0; a < k_axis_count; ++a) {
    Vec3 u;
    Vec3 v;
    ring_basis(a, u, v);
    for (u32 i = 0; i < k_ring_segments; ++i) {
      const f32 t =
          6.28318530717958647692f * static_cast<f32>(i) / static_cast<f32>(k_ring_segments);
      const Vec3 p = centre + (u * std::cos(t) + v * std::sin(t)) * r.radius;
      r.front[a][i] = project_point(view_proj, p, width, height, r.px[a][i], r.py[a][i]);
    }
  }
  return r;
}

namespace {

f32 distance_to_ring(const GizmoRings& rings, u32 a, f32 px, f32 py) noexcept {
  f32 best = 3.4e38f;
  for (u32 i = 0; i < k_ring_segments; ++i) {
    const u32 j = (i + 1) % k_ring_segments;
    if (!rings.front[a][i] || !rings.front[a][j]) continue;
    f32 along = 0.0f;
    const f32 d = distance_to_segment(px, py, rings.px[a][i], rings.py[a][i], rings.px[a][j],
                                      rings.py[a][j], along);
    best = d < best ? d : best;
  }
  return best;
}

DVec3 to_d(Vec3 v) noexcept {
  return DVec3{static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)};
}
f64 dot_d(DVec3 a, DVec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
DVec3 cross_d(DVec3 a, DVec3 b) noexcept {
  return DVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

}  // namespace

u32 hover_ring(const GizmoRings& rings, f32 px, f32 py, f32 tolerance) noexcept {
  if (!rings.visible) return k_no_axis;
  if (distance_to_ring(rings, 1, px, py) <= tolerance) return 1;  // y first: the primary ring
  u32 best = k_no_axis;
  f32 best_distance = tolerance;
  for (const u32 a : {0u, 2u}) {
    const f32 d = distance_to_ring(rings, a, px, py);
    if (d <= best_distance) {
      best_distance = d;
      best = a;
    }
  }
  return best;
}

bool ring_point(const Mat4& view_proj, const GizmoRings& rings, u32 axis, f32 t, u32 width,
                u32 height, f32& px, f32& py) noexcept {
  if (!rings.visible || axis >= k_axis_count) return false;
  Vec3 u;
  Vec3 v;
  ring_basis(axis, u, v);
  return project_point(view_proj, rings.centre + (u * std::cos(t) + v * std::sin(t)) * rings.radius,
                       width, height, px, py);
}

bool ray_on_ring_plane(WorldPos pivot, u32 axis, WorldPos eye, const Ray& ray,
                       DVec3& direction) noexcept {
  if (axis >= k_axis_count) return false;
  const DVec3 n = to_d(axis_direction(axis));
  const DVec3 d = to_d(ray.direction);
  const f64 facing = dot_d(n, d);
  // Within a degree of edge-on (sin of one degree): the pointer cannot say where in the plane it
  // is.
  if (!(std::fabs(facing) > 0.0174524064372835)) return false;
  // The ray's origin in the world, then everything about the pivot in f64.
  const DVec3 o = (eye + to_d(ray.origin)) - pivot;
  const f64 s = -dot_d(n, o) / facing;
  if (!(s >= 0.0) || !std::isfinite(s)) return false;
  DVec3 hit = o + d * s;
  hit = hit - n * dot_d(n, hit);  // in the plane exactly
  const f64 len = std::sqrt(dot_d(hit, hit));
  if (!(len > 1.0e-9)) return false;
  direction = hit / len;
  return true;
}

bool begin_ring_drag(u32 axis, WorldPos pivot, WorldPos eye, const Ray& ray,
                     RingDrag& out) noexcept {
  DVec3 dir;
  if (!ray_on_ring_plane(pivot, axis, eye, ray, dir)) return false;
  out = RingDrag{};
  out.axis = axis;
  out.pivot = pivot;
  out.last = dir;
  return true;
}

bool update_ring_drag(RingDrag& drag, WorldPos eye, const Ray& ray) noexcept {
  DVec3 dir;
  if (!ray_on_ring_plane(drag.pivot, drag.axis, eye, ray, dir)) return false;
  const DVec3 n = to_d(axis_direction(drag.axis));
  // The signed angle from the last direction to this one, about the axis.
  drag.radians += std::atan2(dot_d(n, cross_d(drag.last, dir)), dot_d(drag.last, dir));
  drag.last = dir;
  return true;
}

f64 ring_drag_degrees(const RingDrag& drag, bool snap) noexcept {
  const f64 degrees = drag.radians * (180.0 / 3.14159265358979323846);
  if (!snap) return degrees;
  return std::round(degrees / k_rotate_snap_degrees) * k_rotate_snap_degrees + 0.0;
}

Quat axis_quat(u32 axis, f64 degrees) noexcept {
  const f64 half = degrees * (3.14159265358979323846 / 360.0);
  const f64 s = std::sin(half);
  const Vec3 a = axis_direction(axis < k_axis_count ? axis : 1u);
  return Quat{static_cast<f32>(a.x * s), static_cast<f32>(a.y * s), static_cast<f32>(a.z * s),
              static_cast<f32>(std::cos(half))};
}

namespace {

struct DQuat {
  f64 x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};
DQuat mul(DQuat a, DQuat b) noexcept {
  return DQuat{
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
DQuat about(Vec3 axis, f64 degrees) noexcept {
  const f64 half = degrees * (3.14159265358979323846 / 360.0);
  const f64 s = std::sin(half);
  return DQuat{axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

}  // namespace

Quat turned_rotation(Quat rotation, f32 yaw_deg, u32 axis, f64 degrees) noexcept {
  DQuat r{rotation.x, rotation.y, rotation.z, rotation.w};
  f64 len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
  if (!(len > 1.0e-12) || !std::isfinite(len)) {
    r = DQuat{};
    len = 1.0;
  }
  r = DQuat{r.x / len, r.y / len, r.z / len, r.w / len};
  const DQuat g = about(axis_direction(axis < k_axis_count ? axis : 1u), degrees);
  DQuat out;
  if (axis == 1 || yaw_deg == 0.0f) {
    out = mul(g, r);
  } else {
    const DQuat y = about(Vec3{0.0f, 1.0f, 0.0f}, static_cast<f64>(yaw_deg));
    const DQuat y_inverse{-y.x, -y.y, -y.z, y.w};
    out = mul(mul(mul(y_inverse, g), y), r);
  }
  const f64 n = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
  out = DQuat{out.x / n, out.y / n, out.z / n, out.w / n};
  // The canonical sign: q and -q are the same turn, and the document keeps one of them.
  const f64 first = out.w != 0.0 ? out.w : (out.x != 0.0 ? out.x : (out.y != 0.0 ? out.y : out.z));
  if (first < 0.0) out = DQuat{-out.x, -out.y, -out.z, -out.w};
  return Quat{static_cast<f32>(out.x) + 0.0f, static_cast<f32>(out.y) + 0.0f,
              static_cast<f32>(out.z) + 0.0f, static_cast<f32>(out.w) + 0.0f};
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

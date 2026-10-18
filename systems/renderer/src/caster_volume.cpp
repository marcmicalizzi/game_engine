#include <core/math/world.h>
#include <systems/renderer/caster_volume.h>

#include <cmath>

namespace engine::renderer {

namespace {

// Below this a frustum plane is taken as parallel to the light rather than facing away from it:
// it stays a bound, and its offset is the support of the swept points (below), so it is still
// conservative and gives up at most `T` times this of tightness. It also keeps a silhouette edge's
// cross product with the light well away from zero.
constexpr f64 k_parallel = 1.0e-6;

DVec3 cross(DVec3 a, DVec3 b) noexcept {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

DVec3 normal_of(const Plane& p) noexcept { return DVec3{p.normal}; }

// The point where three planes meet, or false when two of them are parallel.
bool meet(const Plane& a, const Plane& b, const Plane& c, DVec3& out) noexcept {
  const DVec3 na = normal_of(a);
  const DVec3 nb = normal_of(b);
  const DVec3 nc = normal_of(c);
  const DVec3 bc = cross(nb, nc);
  const f64 det = dot(na, bc);
  if (!(std::fabs(det) > 1.0e-12)) return false;
  const f64 da = static_cast<f64>(a.d);
  const f64 db = static_cast<f64>(b.d);
  const f64 dc = static_cast<f64>(c.d);
  out = (bc * (-da) + cross(nc, na) * (-db) + cross(na, nb) * (-dc)) / det;
  return true;
}

// The closed frustum's eight corners: index x | y << 1 | z << 2 is the corner of the left (x = 0)
// or right plane, the bottom (y = 0) or top, and the near (z = 0) or far.
bool corners_of(const Plane planes[6], DVec3 out[8]) noexcept {
  for (u32 i = 0; i < 8; ++i) {
    if (!meet(planes[0 + (i & 1)], planes[2 + ((i >> 1) & 1)], planes[4 + ((i >> 2) & 1)], out[i]))
      return false;
  }
  return true;
}

// The plane with inward normal `n` (unit) that every one of `points` is inside, touching the
// nearest: its offset is the support of the points along -n, widened by `slack`. The normal is
// rounded to float32 first and the offset computed from the rounded one, so the plane the GPU reads
// holds the points whatever the rounding did.
Plane support_plane(DVec3 n, const DVec3* points, u32 count, f64 slack) noexcept {
  const Vec3 normal = narrow(n);
  const DVec3 rounded{normal};
  f64 lowest = 1.0e300;
  for (u32 i = 0; i < count; ++i) {
    const f64 s = dot(rounded, points[i]);
    lowest = s < lowest ? s : lowest;
  }
  return Plane{normal, static_cast<f32>(slack - lowest)};
}

// What the float arithmetic of the test costs, as a margin on every plane: a 2^-20 of the largest
// coordinate the volume reaches, sixteen ulps of it.
f64 slack_of(const DVec3* points, u32 count) noexcept {
  f64 largest = 0.0;
  for (u32 i = 0; i < count; ++i) {
    const f64 l = length(points[i]);
    largest = l > largest ? l : largest;
  }
  return largest * (1.0 / 1048576.0);
}

void push(CasterVolume& out, const Plane& plane) noexcept {
  // Leaving a plane out only makes the volume larger, so a set that would overflow (none does: a
  // perspective frustum's silhouette is at most six edges) stays conservative.
  if (out.plane_count < gfx::k_caster_volume_planes) out.planes[out.plane_count++] = plane;
}

// Planes through the edges between a kept face and a dropped one, each holding `direction` (the
// directional light's) or the point `apex` (a point light's), turned so the frustum is inside.
void add_silhouette(const DVec3 corners[8], const bool kept[6], const DVec3* points, u32 count,
                    f64 slack, const DVec3* direction, const DVec3* apex,
                    CasterVolume& out) noexcept {
  DVec3 centroid{};
  for (u32 i = 0; i < 8; ++i)
    centroid = centroid + corners[i];
  centroid = centroid * 0.125;
  for (u32 a = 0; a < 6; ++a) {
    for (u32 b = a + 1; b < 6; ++b) {
      const u32 axis_a = a / 2;
      const u32 axis_b = b / 2;
      if (axis_a == axis_b) continue;  // opposite faces share no edge
      if (kept[a] == kept[b]) continue;
      const u32 axis_c = 3 - axis_a - axis_b;
      const u32 base = ((a & 1) << axis_a) | ((b & 1) << axis_b);
      const DVec3 e0 = corners[base];
      const DVec3 e1 = corners[base | (1u << axis_c)];
      const DVec3 along = direction != nullptr ? *direction : *apex - e0;
      DVec3 m = cross(e1 - e0, along);
      const f64 len = length(m);
      if (!(len > 1.0e-12 * (length(e1 - e0) * length(along) + 1.0e-300))) continue;
      m = m / len;
      if (dot(m, centroid - e0) < 0.0) m = -m;
      push(out, support_plane(m, points, count, slack));
    }
  }
}

}  // namespace

bool closed_frustum(const Mat4& view_proj, const CasterBounds& bounds, Plane out[6]) noexcept {
  const Frustum frustum = frustum_from_view_proj(view_proj);
  if (frustum.plane_count < 5) return false;
  for (u32 i = 0; i < 5; ++i)
    out[i] = frustum.planes[i];
  // The deepest a point of the scene can be along the view's axis is its distance from the eye,
  // which is at most the far side of its bounds (the eye is the origin). Never on the near plane.
  const f32 znear = -out[4].d;
  f32 deepest = length(bounds.scene_center) + bounds.scene_radius;
  deepest = deepest > 2.0f * znear ? deepest : 2.0f * znear;
  const Plane closing{out[4].normal * -1.0f, deepest};
  out[5] = frustum.plane_count == 6 && frustum.planes[5].d < deepest ? frustum.planes[5] : closing;
  return true;
}

bool fit_directional_caster_volume(const Mat4& view_proj, Vec3 towards_light, f32 distance,
                                   const CasterBounds& bounds, CasterVolume& out) noexcept {
  out = CasterVolume{};
  const f64 light_length = length(DVec3{towards_light});
  if (!(light_length > 0.0)) return false;
  const DVec3 light = DVec3{towards_light} / light_length;
  Plane planes[6];
  if (!closed_frustum(view_proj, bounds, planes)) return false;
  DVec3 points[16];
  if (!corners_of(planes, points)) return false;
  f64 lowest = 1.0e300;
  f64 highest = -1.0e300;
  for (u32 i = 0; i < 8; ++i) {
    const f64 s = dot(light, points[i]);
    lowest = s < lowest ? s : lowest;
    highest = s > highest ? s : highest;
  }
  // The cap: `distance` past the frustum's farthest corner along the light where it is set, the
  // far side of the scene's bounds towards the light otherwise and never past it, and never short
  // of the frustum.
  const f64 scene_far =
      dot(light, DVec3{bounds.scene_center}) + static_cast<f64>(bounds.scene_radius);
  f64 cap = scene_far;
  if (distance > 0.0f) {
    const f64 reach = highest + static_cast<f64>(distance);
    cap = reach < cap ? reach : cap;
  }
  cap = cap > highest ? cap : highest;
  // Every point of the frustum reaches the cap within this much of the light's direction, so the
  // frustum and its copy moved this far are the hull's points.
  const f64 sweep = cap - lowest;
  for (u32 i = 0; i < 8; ++i)
    points[8 + i] = points[i] + light * sweep;
  const f64 slack = slack_of(points, 16);

  bool kept[6];
  for (u32 i = 0; i < 6; ++i) {
    kept[i] = dot(normal_of(planes[i]), light) >= -k_parallel;
    if (kept[i]) push(out, support_plane(normal_of(planes[i]), points, 16, slack));
  }
  add_silhouette(points, kept, points, 16, slack, &light, nullptr, out);
  const Vec3 cap_normal = narrow(-light);
  const f64 cap_offset = -dot(DVec3{cap_normal}, light) * cap;  // the rounded normal's own offset
  push(out, Plane{cap_normal, static_cast<f32>(cap_offset + slack)});
  return out.plane_count > 0;
}

bool fit_point_caster_volume(const Mat4& view_proj, Vec3 light, f32 reach,
                             const CasterBounds& bounds, CasterVolume& out) noexcept {
  out = CasterVolume{};
  Plane planes[6];
  if (!closed_frustum(view_proj, bounds, planes)) return false;
  DVec3 points[9];
  if (!corners_of(planes, points)) return false;
  const DVec3 apex{light};
  points[8] = apex;
  bool kept[6];
  bool inside = true;
  for (u32 i = 0; i < 6; ++i) {
    kept[i] = dot(normal_of(planes[i]), apex) + static_cast<f64>(planes[i].d) >= 0.0;
    inside = inside && kept[i];
  }
  // A light inside the frustum: every segment from it to a receiver is inside the frustum too.
  if (inside) return false;
  const f64 slack = slack_of(points, 9);
  for (u32 i = 0; i < 6; ++i) {
    if (kept[i]) push(out, support_plane(normal_of(planes[i]), points, 9, slack));
  }
  add_silhouette(points, kept, points, 9, slack, nullptr, &apex, out);
  if (reach > 0.0f) out.reach = Vec4{light, reach};
  return out.plane_count > 0;
}

bool caster_volume_contains_sphere(const CasterVolume& volume, Vec3 center, f32 radius) noexcept {
  if (volume.plane_count == 0) return false;
  for (u32 p = 0; p < volume.plane_count; ++p) {
    const Plane& plane = volume.planes[p];
    if (dot(plane.normal, center) + plane.d < -radius) return false;
  }
  if (volume.reach.w >= 0.0f && length(center - Vec3{volume.reach.x, volume.reach.y,
                                                     volume.reach.z}) > volume.reach.w + radius) {
    return false;
  }
  return true;
}

void set_caster_volumes(gfx::CullParams& params, std::span<const CasterVolume> volumes) noexcept {
  u32 count = 0;
  for (const CasterVolume& volume : volumes) {
    if (volume.plane_count == 0 || count >= gfx::k_max_caster_volumes) continue;
    for (u32 p = 0; p < gfx::k_caster_volume_planes; ++p) {
      const Plane& plane = volume.planes[p];
      params.caster_planes[count][p] =
          p < volume.plane_count ? Vec4{plane.normal, plane.d} : Vec4{};
    }
    params.caster_reach[count] = volume.reach;
    params.caster_plane_counts[count] = volume.plane_count;
    ++count;
  }
  for (u32 v = count; v < gfx::k_max_caster_volumes; ++v) {
    params.caster_plane_counts[v] = 0;
    params.caster_reach[v] = Vec4{};
  }
  params.caster_volume_count = count;
}

}  // namespace engine::renderer

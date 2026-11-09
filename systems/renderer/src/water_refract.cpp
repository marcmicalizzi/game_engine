#include <core/math/world.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/visibility_resolve.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/water_refract.h>

#include <cmath>

namespace engine::renderer {

namespace {

tunables::Int water_refract{
    "renderer.water.refract", 1, 0, 2,
    "In the water's volume tier, where the device traces, one ray a water pixel along the "
    "refracted "
    "view finds what stands in the water before the ground: 0 off (the ground alone), 1 on, 2 on "
    "in even frames and off in odd ones, to measure both from one flight (water.md, What is under "
    "the water; S86)"};
tunables::Bool water_refract_volume{
    "renderer.water.refract_volume", true,
    "Build what the refracted view reaches outside the frustum, under the water, into the frame's "
    "structures as casters (water.md, What is under the water; S86)"};

struct D2 {
  f64 x = 0.0;
  f64 z = 0.0;
};

DVec3 cross3(DVec3 a, DVec3 b) noexcept {
  return DVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

f64 cross2(D2 o, D2 a, D2 b) noexcept {
  return (a.x - o.x) * (b.z - o.z) - (a.z - o.z) * (b.x - o.x);
}

// The point where three planes meet, in double; false when two are parallel.
bool meet3(const Plane& a, const Plane& b, const Plane& c, DVec3& out) noexcept {
  const DVec3 na{a.normal}, nb{b.normal}, nc{c.normal};
  const DVec3 bc = cross3(nb, nc);
  const f64 det = dot(na, bc);
  if (!(det > 1e-12 || det < -1e-12)) return false;
  out = (bc * -static_cast<f64>(a.d) + cross3(nc, na) * -static_cast<f64>(b.d) +
         cross3(na, nb) * -static_cast<f64>(c.d)) /
        det;
  return true;
}

}  // namespace

f32 water_refract_path_m(Vec3 extinction) noexcept {
  f32 clearest = extinction.x < extinction.y ? extinction.x : extinction.y;
  clearest = clearest < extinction.z ? clearest : extinction.z;
  clearest = clearest > 1e-4f ? clearest : 1e-4f;
  return gfx::k_water_refract_extinctions / clearest;
}

bool fit_water_refract_volume(const Mat4& view_proj, f32 level, f32 floor, f32 spread_m, f32 reach,
                              const CasterBounds& bounds, CasterVolume& out) noexcept {
  out = CasterVolume{};
  // The eye is the frame's origin: under the level it looks up through the surface, which this
  // volume is not for.
  if (!(level < 0.0f) || !(floor < level)) return false;
  Plane frustum[6];
  if (!closed_frustum(view_proj, bounds, frustum)) return false;
  // The frustum's eight corners (left/right, bottom/top, near/far), then where its twelve edges
  // cross the level: the cross-section's vertices.
  DVec3 corners[8];
  for (u32 i = 0; i < 8; ++i) {
    if (!meet3(frustum[0 + (i & 1)], frustum[2 + ((i >> 1) & 1)], frustum[4 + ((i >> 2) & 1)],
               corners[i]))
      return false;
  }
  const f64 y = static_cast<f64>(level);
  D2 points[12];
  u32 count = 0;
  for (u32 a = 0; a < 8; ++a) {
    for (u32 bit = 1; bit < 8; bit <<= 1) {
      if ((a & bit) != 0) continue;  // each edge once, from its corner with the bit clear
      const DVec3 p = corners[a];
      const DVec3 q = corners[a | bit];
      const f64 dp = p.y - y;
      const f64 dq = q.y - y;
      if ((dp > 0.0 && dq > 0.0) || (dp < 0.0 && dq < 0.0)) continue;
      const f64 t = dp == dq ? 0.0 : dp / (dp - dq);
      points[count++] = D2{p.x + (q.x - p.x) * t, p.z + (q.z - p.z) * t};
    }
  }
  if (count < 3) return false;
  // Their convex hull, counter-clockwise (Andrew's monotone chain over twelve points at most).
  for (u32 i = 1; i < count; ++i) {
    const D2 v = points[i];
    u32 j = i;
    while (j > 0 && (points[j - 1].x > v.x || (points[j - 1].x == v.x && points[j - 1].z > v.z))) {
      points[j] = points[j - 1];
      --j;
    }
    points[j] = v;
  }
  D2 hull[26];
  u32 h = 0;
  for (u32 i = 0; i < count; ++i) {
    while (h >= 2 && cross2(hull[h - 2], hull[h - 1], points[i]) <= 0.0)
      --h;
    hull[h++] = points[i];
  }
  const u32 lower = h + 1;
  for (u32 i = count - 1; i-- > 0;) {
    while (h >= lower && cross2(hull[h - 2], hull[h - 1], points[i]) <= 0.0)
      --h;
    hull[h++] = points[i];
  }
  const u32 edges = h - 1;  // the last point is the first again
  if (edges < 3) return false;
  // What the float test costs, as on the other volumes: a 2^-20 of the farthest coordinate.
  f64 largest = 0.0;
  for (u32 i = 0; i < edges; ++i) {
    const f64 l = hull[i].x * hull[i].x + hull[i].z * hull[i].z;
    largest = l > largest ? l : largest;
  }
  const f64 slack = std::sqrt(largest) * (1.0 / 1048576.0);
  const f64 spread = static_cast<f64>(spread_m) + slack;
  u32 n = 0;
  out.planes[n++] = Plane{Vec3{0.0f, -1.0f, 0.0f}, level};  // under the surface
  out.planes[n++] = Plane{Vec3{0.0f, 1.0f, 0.0f}, -floor};  // above the floor
  if (edges + 2 <= gfx::k_caster_volume_planes) {
    // Each edge's inward plane, vertical, pushed out by the spread.
    for (u32 i = 0; i < edges; ++i) {
      const D2 a = hull[i];
      const D2 b = hull[i + 1];
      // Counter-clockwise in (x, z) seen from +y is clockwise seen from above; the inward normal
      // of edge a->b is the side the hull's other points are on.
      f64 nx = -(b.z - a.z);
      f64 nz = b.x - a.x;
      const f64 len = std::sqrt(nx * nx + nz * nz);
      if (!(len > 0.0)) continue;
      nx /= len;
      nz /= len;
      const D2 other = hull[(i + 2) % edges];
      if (nx * (other.x - a.x) + nz * (other.z - a.z) < 0.0) {
        nx = -nx;
        nz = -nz;
      }
      const Vec3 normal{static_cast<f32>(nx), 0.0f, static_cast<f32>(nz)};
      const f64 rx = static_cast<f64>(normal.x);
      const f64 rz = static_cast<f64>(normal.z);
      // The offset from the rounded normal, the lowest of the hull's points along it.
      f64 lowest = 1.0e300;
      for (u32 k = 0; k < edges; ++k) {
        const f64 s = rx * hull[k].x + rz * hull[k].z;
        lowest = s < lowest ? s : lowest;
      }
      out.planes[n++] = Plane{normal, static_cast<f32>(spread - lowest)};
    }
  } else {
    // More edges than the block holds: the hull's box, pushed out the same.
    f64 x0 = 1.0e300, x1 = -1.0e300, z0 = 1.0e300, z1 = -1.0e300;
    for (u32 k = 0; k < edges; ++k) {
      x0 = hull[k].x < x0 ? hull[k].x : x0;
      x1 = hull[k].x > x1 ? hull[k].x : x1;
      z0 = hull[k].z < z0 ? hull[k].z : z0;
      z1 = hull[k].z > z1 ? hull[k].z : z1;
    }
    out.planes[n++] = Plane{Vec3{1.0f, 0.0f, 0.0f}, static_cast<f32>(spread - x0)};
    out.planes[n++] = Plane{Vec3{-1.0f, 0.0f, 0.0f}, static_cast<f32>(spread + x1)};
    out.planes[n++] = Plane{Vec3{0.0f, 0.0f, 1.0f}, static_cast<f32>(spread - z0)};
    out.planes[n++] = Plane{Vec3{0.0f, 0.0f, -1.0f}, static_cast<f32>(spread + z1)};
  }
  out.plane_count = n;
  if (reach > 0.0f) out.reach = Vec4{0.0f, 0.0f, 0.0f, reach};
  return true;
}

bool water_refract_tunable(u64 frame_index) noexcept {
  switch (water_refract.get()) {
    case 0: return false;
    case 2: return (frame_index & 1u) == 0u;
    default: return true;
  }
}

bool water_refract_volume_tunable() noexcept { return water_refract_volume.get(); }

}  // namespace engine::renderer

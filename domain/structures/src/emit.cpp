// The emitters the shell and the interior share (emit.h).

#include "emit.h"

namespace engine::structures::detail {

namespace {

// Newell's normal: exact for a planar polygon whatever its collinear points.
bool wound_toward(const Vec3* points, u32 n, Vec3 facing) noexcept {
  f64 nx = 0, ny = 0, nz = 0;
  for (u32 i = 0; i < n; ++i) {
    const Vec3 a = points[i], b = points[(i + 1) % n];
    nx += static_cast<f64>(a.y - b.y) * static_cast<f64>(a.z + b.z);
    ny += static_cast<f64>(a.z - b.z) * static_cast<f64>(a.x + b.x);
    nz += static_cast<f64>(a.x - b.x) * static_cast<f64>(a.y + b.y);
  }
  return nx * static_cast<f64>(facing.x) + ny * static_cast<f64>(facing.y) +
             nz * static_cast<f64>(facing.z) >=
         0;
}

void fan(MeshPart& part, const Vec3* points, const Vec2* uvs, const Vec3* normals, Vec3 normal,
         u32 n, Vec3 facing) {
  const bool keep = wound_toward(points, n, facing);
  const u32 base = part.positions.size();
  for (u32 k = 0; k < n; ++k) {
    const u32 i = keep ? k : n - 1 - k;
    part.positions.push_back(points[i]);
    part.normals.push_back(normals != nullptr ? normals[i] : normal);
    part.uvs.push_back(uvs[i]);
  }
  for (u32 k = 1; k + 1 < n; ++k) {
    part.indices.push_back(base);
    part.indices.push_back(base + k);
    part.indices.push_back(base + k + 1);
  }
}

}  // namespace

void turn_deg(i32 degrees, f64& sine, f64& cosine) noexcept {
  const i32 deg = degrees < 0 ? 0 : (degrees > 90 ? 90 : degrees);
  // Over 45 degrees the two change places, so the series runs on at most a quarter of pi, where
  // nine terms are past an f64's last bit.
  const bool swap = deg > 45;
  const f64 x = static_cast<f64>(swap ? 90 - deg : deg) * 0.017453292519943295;
  const f64 x2 = x * x;
  f64 s = 1.0, c = 1.0;
  for (i32 k = 9; k >= 1; --k) {
    s = 1.0 - s * x2 / static_cast<f64>((2 * k) * (2 * k + 1));
    c = 1.0 - c * x2 / static_cast<f64>((2 * k - 1) * (2 * k));
  }
  s *= x;
  sine = swap ? c : s;
  cosine = swap ? s : c;
}

void polygon(MeshPart& part, const Vec3* points, const Vec2* uvs, u32 n, Vec3 normal) {
  fan(part, points, uvs, nullptr, normal, n, normal);
}

void polygon_smooth(MeshPart& part, const Vec3* points, const Vec2* uvs, const Vec3* normals, u32 n,
                    Vec3 facing) {
  fan(part, points, uvs, normals, facing, n, facing);
}

void box(MeshPart& part, const Frame2& frame, f64 s0, f64 s1, f64 y0, f64 y1, f64 q0, f64 q1,
         u32 grain) {
  const f64 s[2] = {s0, s1}, y[2] = {y0, y1}, q[2] = {q0, q1};
  const Vec3 along = frame.dir(), left = frame.left();
  const Vec3 up{0, 1, 0};
  // Each face: the axis it is square to (0 s, 1 y, 2 q), the side, and its two in-plane axes.
  for (u32 axis = 0; axis < 3; ++axis) {
    const u32 ua = axis == 0 ? 2u : 0u;  // the face's first in-plane axis
    const u32 va = axis == 1 ? 2u : 1u;  // and its second
    for (u32 side = 0; side < 2; ++side) {
      Vec3 points[4];
      Vec2 uvs[4];
      const u32 corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
      for (u32 c = 0; c < 4; ++c) {
        u32 pick[3] = {0, 0, 0};
        pick[axis] = side;
        pick[ua] = corner[c][0];
        pick[va] = corner[c][1];
        const f64 coords[3] = {s[pick[0]], y[pick[1]], q[pick[2]]};
        points[c] = frame.at(coords[0], coords[1], coords[2]);
        // u along the grain where the face holds it.
        const f64 u = grain == va ? coords[va] : coords[ua];
        const f64 v = grain == va ? coords[ua] : coords[va];
        uvs[c] = {static_cast<f32>(u * k_m), static_cast<f32>(v * k_m)};
      }
      const f32 sign = side == 0 ? -1.0f : 1.0f;
      const Vec3 normal = (axis == 0 ? along : (axis == 1 ? up : left)) * sign;
      polygon(part, points, uvs, 4, normal);
    }
  }
}

void box3(MeshPart& part, const Frame3& frame, f64 a0, f64 a1, f64 b0, f64 b1, f64 c0, f64 c1,
          u32 grain) {
  const f64 lo[3] = {a0, b0, c0}, hi[3] = {a1, b1, c1};
  for (u32 axis = 0; axis < 3; ++axis) {
    const u32 ua = axis == 0 ? 2u : 0u;
    const u32 va = axis == 1 ? 2u : 1u;
    for (u32 side = 0; side < 2; ++side) {
      Vec3 points[4];
      Vec2 uvs[4];
      const u32 corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
      for (u32 c = 0; c < 4; ++c) {
        u32 pick[3] = {0, 0, 0};
        pick[axis] = side;
        pick[ua] = corner[c][0];
        pick[va] = corner[c][1];
        f64 coords[3];
        for (u32 k = 0; k < 3; ++k)
          coords[k] = pick[k] == 0 ? lo[k] : hi[k];
        f64 p[3];
        for (u32 k = 0; k < 3; ++k)
          p[k] = frame.o[k] + frame.axis[0][k] * coords[0] + frame.axis[1][k] * coords[1] +
                 frame.axis[2][k] * coords[2];
        points[c] = metres(p[0], p[1], p[2]);
        const f64 u = grain == va ? coords[va] : coords[ua];
        const f64 v = grain == va ? coords[ua] : coords[va];
        uvs[c] = {static_cast<f32>(u * k_m), static_cast<f32>(v * k_m)};
      }
      const f64 sign = side == 0 ? -1.0 : 1.0;
      const Vec3 normal{static_cast<f32>(frame.axis[axis][0] * sign),
                        static_cast<f32>(frame.axis[axis][1] * sign),
                        static_cast<f32>(frame.axis[axis][2] * sign)};
      polygon(part, points, uvs, 4, normal);
    }
  }
}

bool triangulate(const Vector<i32>& outline, Vector<u32>& triangles) {
  triangles.clear();
  const u32 n = outline.size() / 2;
  if (n < 3 || outline.size() % 2 != 0) return false;
  const auto x = [&](u32 i) { return static_cast<i64>(outline[2 * i]); };
  const auto z = [&](u32 i) { return static_cast<i64>(outline[2 * i + 1]); };
  i64 area2 = 0;
  for (u32 i = 0; i < n; ++i) {
    const u32 j = (i + 1) % n;
    area2 += x(i) * z(j) - x(j) * z(i);
  }
  if (area2 == 0) return false;
  const i64 sense = area2 > 0 ? 1 : -1;
  const auto turn = [&](u32 a, u32 b, u32 c) {
    return ((x(b) - x(a)) * (z(c) - z(a)) - (z(b) - z(a)) * (x(c) - x(a))) * sense;
  };
  Vector<u32> ring;
  for (u32 i = 0; i < n; ++i)
    ring.push_back(i);
  u32 guard = 0;
  while (ring.size() > 3) {
    bool clipped = false;
    for (u32 k = 0; k < ring.size(); ++k) {
      const u32 a = ring[(k + ring.size() - 1) % ring.size()], b = ring[k];
      const u32 c = ring[(k + 1) % ring.size()];
      const i64 t = turn(a, b, c);
      if (t < 0) continue;
      bool ear = true;
      if (t > 0) {
        for (u32 m = 0; m < ring.size() && ear; ++m) {
          const u32 p = ring[m];
          if (p == a || p == b || p == c) continue;
          if (turn(a, b, p) >= 0 && turn(b, c, p) >= 0 && turn(c, a, p) >= 0) ear = false;
        }
        if (!ear) continue;
        triangles.push_back(a);
        triangles.push_back(b);
        triangles.push_back(c);
      }
      // A straight corner (t == 0) is dropped with no triangle.
      for (u32 m = k; m + 1 < ring.size(); ++m)
        ring[m] = ring[m + 1];
      ring.resize(ring.size() - 1);
      clipped = true;
      break;
    }
    if (!clipped || ++guard > 4 * n) return false;
  }
  if (turn(ring[0], ring[1], ring[2]) <= 0) return ring.size() == 3 && !triangles.empty();
  triangles.push_back(ring[0]);
  triangles.push_back(ring[1]);
  triangles.push_back(ring[2]);
  return true;
}

}  // namespace engine::structures::detail

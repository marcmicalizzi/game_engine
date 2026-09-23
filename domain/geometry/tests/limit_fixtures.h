#pragma once

// Neutral control meshes for the limit-surface and surface-binding tests and the bench
// (docs/subsystems/geometry.md, "Limit surfaces and surface bindings"). All of them are generated
// from a formula, so no test carries data from outside the repository.
//
// The **ringed disk** is the authoring side's fixture topology exactly: a centre vertex, then
// `rings` rings of `ring_size` vertices numbered outward and counter-clockwise, a fan of triangles
// `(0, i, i + 1)` around the centre and between ring k and ring k + 1 the strip
// `(inner_j, outer_j, outer_j+1), (inner_j, outer_j+1, inner_j+1)`. With 5 rings of 24 it is the
// 121-node, 216-triangle top surface of the neutral torso fixture, with its valence-24 pole — and
// the face list is identical to the one the authoring side's Loop matrices were computed on
// (checked once against the packet; the test asserts the counts that follow from it).

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <cmath>
#include <numbers>

namespace engine::geometry::fixture {

// splitmix64: a fixed stream, so every run of every build sees the same "random" cage.
class Random {
 public:
  explicit Random(u64 seed) : state_(seed) {}
  u64 next() {
    u64 z = (state_ += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
  }
  // Uniform in [lo, hi).
  f32 uniform(f32 lo, f32 hi) {
    const f64 unit = static_cast<f64>(next() >> 11) * 0x1.0p-53;
    return static_cast<f32>(static_cast<f64>(lo) + unit * static_cast<f64>(hi - lo));
  }

 private:
  u64 state_;
};

inline Vector<u32> ringed_disk_faces(u32 rings, u32 ring_size) {
  Vector<u32> faces;
  for (u32 j = 0; j < ring_size; ++j) {
    faces.push_back(0);
    faces.push_back(1 + j);
    faces.push_back(1 + (j + 1) % ring_size);
  }
  for (u32 k = 1; k < rings; ++k) {
    const u32 inner = 1 + (k - 1) * ring_size;
    const u32 outer = 1 + k * ring_size;
    for (u32 j = 0; j < ring_size; ++j) {
      const u32 j1 = (j + 1) % ring_size;
      faces.push_back(inner + j);
      faces.push_back(outer + j);
      faces.push_back(outer + j1);
      faces.push_back(inner + j);
      faces.push_back(outer + j1);
      faces.push_back(inner + j1);
    }
  }
  return faces;
}

inline u32 ringed_disk_vertices(u32 rings, u32 ring_size) { return 1 + rings * ring_size; }

// A dome over the ringed disk: ring k at normalized radius k / rings on an ellipse of semi-axes
// `a` and `b` in the xy plane, raised along +z by `height * (1 - r^2)^2`. Counter-clockwise seen
// from +z, so the triangles face +z. The neutral fixture's footprint is a = 0.070 m, b = 0.086 m.
inline Vector<Vec3> ringed_disk_dome(u32 rings, u32 ring_size, f32 a, f32 b, f32 height) {
  Vector<Vec3> positions;
  positions.push_back(Vec3{0.0f, 0.0f, height});
  for (u32 k = 1; k <= rings; ++k) {
    const f64 r = static_cast<f64>(k) / static_cast<f64>(rings);
    const f64 lift = static_cast<f64>(height) * (1.0 - r * r) * (1.0 - r * r);
    for (u32 j = 0; j < ring_size; ++j) {
      const f64 phi = 2.0 * std::numbers::pi * static_cast<f64>(j) / static_cast<f64>(ring_size);
      positions.push_back(Vec3{static_cast<f32>(static_cast<f64>(a) * r * std::cos(phi)),
                               static_cast<f32>(static_cast<f64>(b) * r * std::sin(phi)),
                               static_cast<f32>(lift)});
    }
  }
  return positions;
}

// A regular triangulated sheet of `n x m` vertices (vertex (i, j) at index j * n + i), each quad
// split along its (i, j)-(i + 1, j + 1) diagonal, counter-clockwise seen from +z. Every interior
// vertex has valence 6 and its neighbours at +-(1, 0), +-(0, 1), +-(1, 1): the three directions of
// the quartic box spline Loop's scheme generalizes.
inline Vector<u32> sheet_faces(u32 n, u32 m) {
  Vector<u32> faces;
  for (u32 j = 0; j + 1 < m; ++j) {
    for (u32 i = 0; i + 1 < n; ++i) {
      const u32 v00 = j * n + i;
      const u32 v10 = v00 + 1;
      const u32 v01 = v00 + n;
      const u32 v11 = v01 + 1;
      faces.push_back(v00);
      faces.push_back(v10);
      faces.push_back(v11);
      faces.push_back(v00);
      faces.push_back(v11);
      faces.push_back(v01);
    }
  }
  return faces;
}

// A closed torus of `n x m` vertices, every vertex of valence 6: the regular closed surface.
inline Vector<u32> torus_faces(u32 n, u32 m) {
  Vector<u32> faces;
  for (u32 j = 0; j < m; ++j) {
    for (u32 i = 0; i < n; ++i) {
      const u32 v00 = j * n + i;
      const u32 v10 = j * n + (i + 1) % n;
      const u32 v01 = ((j + 1) % m) * n + i;
      const u32 v11 = ((j + 1) % m) * n + (i + 1) % n;
      faces.push_back(v00);
      faces.push_back(v10);
      faces.push_back(v11);
      faces.push_back(v00);
      faces.push_back(v11);
      faces.push_back(v01);
    }
  }
  return faces;
}

inline Vector<Vec3> torus_positions(u32 n, u32 m, f32 major, f32 minor) {
  Vector<Vec3> positions;
  for (u32 j = 0; j < m; ++j) {
    const f64 v = 2.0 * std::numbers::pi * static_cast<f64>(j) / static_cast<f64>(m);
    for (u32 i = 0; i < n; ++i) {
      const f64 u = 2.0 * std::numbers::pi * static_cast<f64>(i) / static_cast<f64>(n);
      const f64 ring = static_cast<f64>(major) + static_cast<f64>(minor) * std::cos(v);
      positions.push_back(Vec3{static_cast<f32>(ring * std::cos(u)),
                               static_cast<f32>(ring * std::sin(u)),
                               static_cast<f32>(static_cast<f64>(minor) * std::sin(v))});
    }
  }
  return positions;
}

// A half-disk fan: vertex 0 at the origin with `k` triangles around it to vertices 1 .. k + 1 on a
// half circle, so vertex 0 is a boundary vertex with k faces, 1 and k + 1 have one face each, and
// the rest two.
inline Vector<u32> fan_faces(u32 k) {
  Vector<u32> faces;
  for (u32 i = 0; i < k; ++i) {
    faces.push_back(0);
    faces.push_back(1 + i);
    faces.push_back(2 + i);
  }
  return faces;
}

}  // namespace engine::geometry::fixture

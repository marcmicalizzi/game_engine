#pragma once

// The geometric queries the tissue validators are built from (docs/subsystems/tissue.md,
// "Validators"): nearest points with a sign, rays, triangle-triangle intersection, generalized
// winding numbers and tetrahedron quality. Private to the module. Content-build code, in double
// throughout, deterministic (every traversal breaks ties by index), and not a hot path: study019's
// largest query set is 7,009 rays against 28,032 triangles, a few times per state.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <limits>
#include <span>

namespace engine::tissue::query {

struct D3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};

inline D3 d3(Vec3 v) noexcept {
  return D3{static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)};
}
inline D3 operator+(D3 a, D3 b) noexcept { return D3{a.x + b.x, a.y + b.y, a.z + b.z}; }
inline D3 operator-(D3 a, D3 b) noexcept { return D3{a.x - b.x, a.y - b.y, a.z - b.z}; }
inline D3 operator*(D3 a, f64 s) noexcept { return D3{a.x * s, a.y * s, a.z * s}; }
inline f64 dot(D3 a, D3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline D3 cross(D3 a, D3 b) noexcept {
  return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f64 length(D3 a) noexcept;
D3 unit(D3 a) noexcept;  // zero for a zero vector
// The angle between two vectors in degrees, as atan2(|a x b|, a . b): accurate near zero, where
// acos of a dot product of two f32 unit vectors has a floor of about 0.026 degrees (acos(1 - d) is
// about sqrt(2 d), and d is the normalization's 1e-7) that would read as a real difference.
f64 angle_deg(D3 a, D3 b) noexcept;

inline constexpr u32 k_none = ~0u;

// A bounding-volume hierarchy over a triangle soup: median split on the longest centroid axis, at
// most four triangles a leaf, boxes in double. Owns its positions (as doubles) and triangles.
class TriangleBvh {
 public:
  void build(std::span<const Vec3> positions, std::span<const u32> triangles);
  void build(std::span<const D3> positions, std::span<const u32> triangles);
  u32 triangle_count() const noexcept { return static_cast<u32>(triangles_.size() / 3); }
  bool empty() const noexcept { return triangles_.empty(); }
  const Vector<D3>& positions() const noexcept { return positions_; }
  const Vector<u32>& triangles() const noexcept { return triangles_; }
  D3 corner(u32 triangle, u32 k) const noexcept { return positions_[triangles_[3 * triangle + k]]; }

  // The nearest point of the surface to `p`: which triangle, its barycentrics, and which feature
  // of it (0 the interior, 1 to 3 a corner, 4 to 6 the edge from corner k - 4 to the next). Ties go
  // to the lower triangle index.
  struct Nearest {
    u32 triangle = k_none;
    f64 distance = std::numeric_limits<f64>::infinity();
    D3 point;
    f64 b[3] = {1.0, 0.0, 0.0};
    u32 feature = 0;
  };
  Nearest nearest(D3 p) const;

  // The first triangle a ray meets at t in (t_min, t_max], and the cosine between the ray and that
  // triangle's normal (positive: the ray leaves through the triangle's front).
  struct Hit {
    u32 triangle = k_none;
    f64 t = std::numeric_limits<f64>::infinity();
    f64 facing = 0.0;
  };
  Hit first_hit(D3 origin, D3 direction, f64 t_min, f64 t_max) const;

  // Pairs of triangles, one from each hierarchy, that intersect. Counts every pair and keeps the
  // first `max_witnesses` in (this triangle, other triangle) order.
  u64 intersections(const TriangleBvh& other, u32 max_witnesses,
                    Vector<std::pair<u32, u32>>* witnesses) const;

  // Every pair of triangles, one from each hierarchy, whose leaves' boxes come within `distance` of
  // each other: the candidates an exact distance test then decides, sorted (this, other). A
  // hierarchy paired with itself lists (i, j) and (j, i) and (i, i).
  void candidate_pairs(const TriangleBvh& other, f64 distance,
                       Vector<std::pair<u32, u32>>& out) const;

 private:
  struct Node {
    D3 lo;
    D3 hi;
    u32 first = 0;  // a leaf's first entry of order_, or an inner node's left child
    u32 count = 0;  // triangles in a leaf; 0 for an inner node
  };
  void build_nodes();
  Vector<D3> positions_;
  Vector<u32> triangles_;
  Vector<Node> nodes_;
  Vector<u32> order_;
};

// A surface with a sign: the nearest point, and the side of it the query point is on by the
// angle-weighted pseudonormal of the nearest feature (Bærentzen and Aanæs, 2005), which is the
// correct sign on a closed surface and a local one on an open crop, where far from the surface the
// sign is not defined at all — which is why a containment row pairs it with an intersection count.
class SignedSurface {
 public:
  void build(std::span<const Vec3> positions, std::span<const u32> triangles);
  void build(std::span<const D3> positions, std::span<const u32> triangles);
  struct Result {
    f64 distance = 0.0;  // signed: positive on the side the triangles' winding faces
    u32 triangle = k_none;
    D3 point;
    D3 normal;  // the pseudonormal the sign came from, unit
  };
  Result query(D3 p) const;
  const TriangleBvh& bvh() const noexcept { return bvh_; }
  // The unit normal of a triangle.
  D3 face_normal(u32 triangle) const noexcept { return face_normals_[triangle]; }

 private:
  void build_normals();
  TriangleBvh bvh_;
  Vector<D3> face_normals_;
  Vector<D3> vertex_normals_;  // angle weighted
  Vector<u64> edge_keys_;      // sorted (min << 32 | max)
  Vector<D3> edge_normals_;    // parallel to edge_keys_
};

// Whether two triangles intersect (Möller 1997, with the coplanar case), in double.
bool triangles_intersect(D3 a0, D3 a1, D3 a2, D3 b0, D3 b1, D3 b2) noexcept;

// Whether two triangles intersect, decided so that a nearly coplanar pair is not guessed at: an
// edge of one crossing the other where the two are apart by more than a tolerance of 1e-10 of
// their size, and the pair tested in their common plane where every corner of each lies within
// that tolerance of the other's plane. `triangles_intersect` (Möller's test as published, which
// the version-1 rows use) is exact for a pair in general position and unreliable for a nearly
// coplanar one, and a surface's own chords are nearly coplanar wherever the surface is flat: the
// layered model's rows, which test a surface against itself, use this one.
bool triangles_cross(D3 a0, D3 a1, D3 a2, D3 b0, D3 b1, D3 b2) noexcept;

// The least distance from a point to a triangle, and between two segments (Ericson §5.1.9), in
// double.
f64 point_triangle_distance(D3 p, D3 a, D3 b, D3 c) noexcept;
f64 segment_distance(D3 p0, D3 p1, D3 q0, D3 q1) noexcept;
// The least distance between two triangles: zero when they cross (`triangles_cross`), and
// otherwise the least of the six vertex-to-triangle and nine edge-to-edge distances, which is where
// two disjoint triangles' closest pair lies. In double, with no directed rounding: a measurement,
// not a bound.
f64 triangle_distance(D3 a0, D3 a1, D3 a2, D3 b0, D3 b1, D3 b2) noexcept;

// The generalized winding number of `p` with respect to a closed, consistently wound triangle mesh
// (Jacobson, Kavan and Sorkine-Hornung 2013; each triangle's solid angle by Van Oosterom and
// Strackee 1983): 1 inside, 0 outside, whatever a ray would have hit, which is why the containment
// row uses it rather than a parity count.
f64 winding_number(D3 p, std::span<const D3> positions, std::span<const u32> triangles) noexcept;

// The tetrahedron's signed volume, positive when (b - a, c - a, d - a) is a right-handed frame.
f64 tet_volume(D3 a, D3 b, D3 c, D3 d) noexcept;

// The signed inverse condition number of a linear tetrahedron against the regular one (Knupp's
// condition-number quality, the SICN Gmsh reports): 3 det(A) / (|A|_F |adj A|_F), A the element's
// edge matrix times the inverse of the regular tetrahedron's. 1 for a regular tetrahedron, 0 for a
// flat one, negative for an inverted one. Checked against Gmsh on study019's 846 cells: the minimum
// agrees to 1e-15.
f64 tet_sicn(D3 a, D3 b, D3 c, D3 d) noexcept;

// The volume a triangle sweeps when its corners move linearly from (a, b, c) to (a', b', c'),
// signed along the triangle's normal: exact for linear motion, and summed over a closed surface it
// is exactly the change of the enclosed volume. What "swept" means in the volume report.
f64 swept_volume(D3 a, D3 b, D3 c, D3 a1, D3 b1, D3 c1) noexcept;

// The enclosed volume of a closed, outward-wound triangle mesh (the divergence theorem).
f64 enclosed_volume(std::span<const D3> positions, std::span<const u32> triangles) noexcept;

}  // namespace engine::tissue::query

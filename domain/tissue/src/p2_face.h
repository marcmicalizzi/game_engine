#pragma once

// The six-node faces of a ten-node region's material boundary (docs/subsystems/tissue.md, "The
// material boundary"): which of a cell's nodes a face is, where a point of it is, its chords and
// how far the curved face can be from them. Private to the module; in double, deterministic.
//
// **Faces are Gmsh's.** A ten-node cell's local face k is Gmsh's `faces_tetra[k]` — (0, 2, 1),
// (0, 1, 3), (0, 3, 2), (3, 1, 2) — wound outward (counter-clockwise seen from outside a positive
// cell), and its six nodes are the three corners in that order, then the edge nodes of (a, b),
// (b, c), (c, a): Gmsh's six-node triangle (`MSH` type 9), since the cell's edge nodes are in
// Gmsh's order (cells.h).
//
// **A point of a face** is x(l) = sum over the corners of l_i (2 l_i - 1) c_i plus 4 l_i l_j m_ij
// over its edges, for barycentrics l over its corners in the face's order.
//
// **The chords** are the four triangles through the face's six nodes, (c0, m01, m20),
// (m01, c1, m12), (m20, m12, c2), (m01, m12, m20), wound as the face: the same-node subdivision's
// boundary (cells.h), which is what the engine's geometric rows test a curved face by.
//
// **How far the face can be from its chords** is bounded by the Bernstein form: the face is a
// quadratic Bézier triangle, each chord is its restriction to a quarter of the parameter domain
// with its own control net (by blossoming), and the difference between a quarter and its chord
// lies in the convex hull of 0 and the three edge-control differences, so the largest of their
// lengths bounds it (CONTACT.md's construction, at one level of subdivision). Evaluated in double
// with no directed rounding: a reported deviation, not a certificate.

#include "mesh_query.h"

#include <core/base/types.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::tissue::p2 {

using query::D3;

inline constexpr u32 k_face_corners[4][3] = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {3, 1, 2}};
inline constexpr u32 k_face_nodes[4][6] = {
    {0, 2, 1, 6, 5, 4}, {0, 1, 3, 4, 9, 7}, {0, 3, 2, 7, 8, 6}, {3, 1, 2, 9, 5, 8}};
// The cell corner each face does not have.
inline constexpr u32 k_face_opposite[4] = {3, 2, 1, 0};
// The chords, as indices into the face's six nodes.
inline constexpr u32 k_chords[4][3] = {{0, 3, 5}, {3, 1, 4}, {5, 4, 2}, {3, 4, 5}};

// The point of a face at barycentrics `l` over its corners.
inline D3 evaluate(const D3 x[6], const f64 l[3]) noexcept {
  D3 out;
  for (u32 i = 0; i < 3; ++i)
    out = out + x[i] * (l[i] * (2.0 * l[i] - 1.0));
  out = out + x[3] * (4.0 * l[0] * l[1]) + x[4] * (4.0 * l[1] * l[2]) + x[5] * (4.0 * l[2] * l[0]);
  return out;
}

// The two tangents of the face at `l`, along (l1 - l0) and (l2 - l0): the columns of its map from
// the reference triangle.
inline void tangents(const D3 x[6], const f64 l[3], D3& t1, D3& t2) noexcept {
  // d x / d l_i treating the three as independent.
  const D3 d0 = x[0] * (4.0 * l[0] - 1.0) + x[3] * (4.0 * l[1]) + x[5] * (4.0 * l[2]);
  const D3 d1 = x[1] * (4.0 * l[1] - 1.0) + x[3] * (4.0 * l[0]) + x[4] * (4.0 * l[2]);
  const D3 d2 = x[2] * (4.0 * l[2] - 1.0) + x[4] * (4.0 * l[1]) + x[5] * (4.0 * l[0]);
  t1 = d1 - d0;
  t2 = d2 - d0;
}

// The 7-point, degree-5 rule on the reference triangle (Dunavant 1985), weights summing to one.
struct QuadraturePoint {
  f64 l[3];
  f64 w;
};
inline constexpr f64 k_q_a1 = 0.059715871789770;
inline constexpr f64 k_q_b1 = 0.470142064105115;
inline constexpr f64 k_q_a2 = 0.797426985353087;
inline constexpr f64 k_q_b2 = 0.101286507323456;
inline constexpr QuadraturePoint k_quadrature[7] = {
    {{1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0}, 0.225},    {{k_q_a1, k_q_b1, k_q_b1}, 0.132394152788506},
    {{k_q_b1, k_q_a1, k_q_b1}, 0.132394152788506}, {{k_q_b1, k_q_b1, k_q_a1}, 0.132394152788506},
    {{k_q_a2, k_q_b2, k_q_b2}, 0.125939180544827}, {{k_q_b2, k_q_a2, k_q_b2}, 0.125939180544827},
    {{k_q_b2, k_q_b2, k_q_a2}, 0.125939180544827}};

// The face's area by the rule above (exact for a straight face; a close estimate of a curved one).
inline f64 area(const D3 x[6]) noexcept {
  f64 sum = 0.0;
  for (const QuadraturePoint& q : k_quadrature) {
    D3 t1;
    D3 t2;
    tangents(x, q.l, t1, t2);
    sum += q.w * query::length(query::cross(t1, t2));
  }
  return 0.5 * sum;
}

// The least area element |t1 x t2| over the thirteen points the face is sampled at: its corners,
// its edge midpoints and the rule's seven. Positive where the face's map is regular there.
inline f64 least_metric(const D3 x[6]) noexcept {
  f64 least = std::numeric_limits<f64>::infinity();
  const f64 samples[6][3] = {{1, 0, 0},     {0, 1, 0},     {0, 0, 1},
                             {0.5, 0.5, 0}, {0, 0.5, 0.5}, {0.5, 0, 0.5}};
  const auto at = [&](const f64 l[3]) {
    D3 t1;
    D3 t2;
    tangents(x, l, t1, t2);
    least = std::min(least, query::length(query::cross(t1, t2)));
  };
  for (const auto& l : samples)
    at(l);
  for (const QuadraturePoint& q : k_quadrature)
    at(q.l);
  return least;
}

// The blossom of the face's quadratic Bézier form at two parameter points.
inline D3 blossom(const D3 x[6], const f64 u[3], const f64 w[3]) noexcept {
  // Control points: the corners, and C_ij = 2 m_ij - (c_i + c_j) / 2 on each edge.
  const D3 c01 = x[3] * 2.0 - (x[0] + x[1]) * 0.5;
  const D3 c12 = x[4] * 2.0 - (x[1] + x[2]) * 0.5;
  const D3 c20 = x[5] * 2.0 - (x[2] + x[0]) * 0.5;
  D3 out = x[0] * (u[0] * w[0]) + x[1] * (u[1] * w[1]) + x[2] * (u[2] * w[2]);
  out = out + c01 * (u[0] * w[1] + u[1] * w[0]) + c12 * (u[1] * w[2] + u[2] * w[1]) +
        c20 * (u[2] * w[0] + u[0] * w[2]);
  return out;
}

// An upper bound on how far the curved face is from its four chords (above).
inline f64 chord_deviation(const D3 x[6]) noexcept {
  constexpr f64 h = 0.5;
  const f64 corners[4][3][3] = {{{1, 0, 0}, {h, h, 0}, {h, 0, h}},
                                {{h, h, 0}, {0, 1, 0}, {0, h, h}},
                                {{h, 0, h}, {0, h, h}, {0, 0, 1}},
                                {{h, h, 0}, {0, h, h}, {h, 0, h}}};
  f64 worst = 0.0;
  for (const auto& sub : corners) {
    D3 p[3];
    for (u32 k = 0; k < 3; ++k)
      p[k] = blossom(x, sub[k], sub[k]);
    for (u32 k = 0; k < 3; ++k) {
      const u32 l = (k + 1) % 3;
      const D3 edge = blossom(x, sub[k], sub[l]);
      worst = std::max(worst, query::length(edge - (p[k] + p[l]) * 0.5));
    }
  }
  return worst;
}

}  // namespace engine::tissue::p2

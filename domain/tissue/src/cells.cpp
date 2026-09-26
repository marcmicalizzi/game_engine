#include "cells.h"

#include <algorithm>
#include <limits>

namespace engine::tissue::cells {

namespace {

using query::cross;
using query::dot;

// The node on the edge between corners a and b (Gmsh's order), ~0 on the diagonal.
constexpr u32 k_edge_node[4][4] = {{~0u, 4, 6, 7}, {4, ~0u, 5, 9}, {6, 5, ~0u, 8}, {7, 9, 8, ~0u}};

f64 det3(D3 a, D3 b, D3 c) noexcept { return dot(a, cross(b, c)); }

// The Jacobian's columns, d x / d xi_k for k = 1, 2, 3 with l_0 = 1 - xi_1 - xi_2 - xi_3, at the
// reference corners. Each is linear in l: with g_a = d x / d l_a (the l treated as independent),
// g_a = (4 l_a - 1) x_a + sum over b != a of 4 l_b x_ab, which at corner m is 3 x_a when m = a and
// 4 x_am - x_a otherwise; a column is g_k - g_0.
void corner_columns(const D3 x[10], D3 c[3][4]) noexcept {
  const auto g = [&](u32 a, u32 m) {
    return a == m ? x[a] * 3.0 : x[k_edge_node[a][m]] * 4.0 - x[a];
  };
  for (u32 k = 1; k <= 3; ++k)
    for (u32 m = 0; m < 4; ++m)
      c[k - 1][m] = g(k, m) - g(0, m);
}

// det J of three linear columns in Bernstein form over the simplex whose corners the columns are
// given at: det J = sum over ordered corner triples (i, j, l) of l_i l_j l_l det(c1_i, c2_j, c3_l),
// and grouping the triples by their multiset a, whose Bernstein polynomial is (3! / a!) l^a, the
// coefficient is the mean of det(c1_i, c2_j, c3_l) over the triple's distinct orderings.
// `corner_min` is the least coefficient at a corner, which is det J there exactly.
void bernstein(const D3 c[3][4], f64 out[20], f64& corner_min) noexcept {
  corner_min = std::numeric_limits<f64>::infinity();
  u32 n = 0;
  for (u32 i = 0; i < 4; ++i)
    for (u32 j = i; j < 4; ++j)
      for (u32 l = j; l < 4; ++l) {
        const u32 p[6][3] = {{i, j, l}, {i, l, j}, {j, i, l}, {j, l, i}, {l, i, j}, {l, j, i}};
        f64 sum = 0.0;
        u32 count = 0;
        for (u32 q = 0; q < 6; ++q) {
          bool repeated = false;
          for (u32 r = 0; r < q; ++r)
            repeated = repeated || (p[r][0] == p[q][0] && p[r][1] == p[q][1] && p[r][2] == p[q][2]);
          if (repeated) continue;
          sum += det3(c[0][p[q][0]], c[1][p[q][1]], c[2][p[q][2]]);
          ++count;
        }
        out[n] = sum / static_cast<f64>(count);
        if (i == j && j == l) corner_min = std::min(corner_min, out[n]);
        ++n;
      }
}

struct Bary {
  f64 l[4] = {0.0, 0.0, 0.0, 0.0};
};

Bary middle(const Bary& a, const Bary& b) noexcept {
  Bary out;
  for (u32 m = 0; m < 4; ++m)
    out.l[m] = 0.5 * (a.l[m] + b.l[m]);
  return out;
}

void visit(const D3 c[3][4], const Bary p[4], u32 depth, u32 max_depth, JacobianCheck& out,
           bool& uncertain) noexcept {
  // The columns at this piece's corners: linear in l, so a barycentric mix of the cell's.
  D3 s[3][4];
  for (u32 k = 0; k < 3; ++k)
    for (u32 n = 0; n < 4; ++n) {
      D3 v;
      for (u32 m = 0; m < 4; ++m)
        v = v + c[k][m] * p[n].l[m];
      s[k][n] = v;
    }
  f64 b[20];
  f64 corner_min = 0.0;
  bernstein(s, b, corner_min);
  const f64 lo = *std::min_element(b, b + 20);
  const f64 hi = *std::max_element(b, b + 20);
  out.sampled = std::min(out.sampled, corner_min);
  out.depth = std::max(out.depth, depth);
  if (!(corner_min > 0.0)) {
    out.result = JacobianCheck::Result::inverted;
    out.lower = std::min(out.lower, lo);
    out.upper = std::max(out.upper, hi);
    return;
  }
  if (lo > 0.0 || depth == max_depth) {
    out.lower = std::min(out.lower, lo);
    out.upper = std::max(out.upper, hi);
    if (!(lo > 0.0)) uncertain = true;
    return;
  }
  // Eight pieces: the four corners and the inner octahedron around the (01, 23) diagonal.
  const Bary m01 = middle(p[0], p[1]);
  const Bary m02 = middle(p[0], p[2]);
  const Bary m03 = middle(p[0], p[3]);
  const Bary m12 = middle(p[1], p[2]);
  const Bary m13 = middle(p[1], p[3]);
  const Bary m23 = middle(p[2], p[3]);
  const Bary pieces[8][4] = {{p[0], m01, m02, m03}, {m01, p[1], m12, m13}, {m02, m12, p[2], m23},
                             {m03, m13, m23, p[3]}, {m01, m23, m12, m02},  {m01, m23, m02, m03},
                             {m01, m23, m03, m13},  {m01, m23, m13, m12}};
  for (const auto& piece : pieces) {
    visit(c, piece, depth + 1, max_depth, out, uncertain);
    if (out.result == JacobianCheck::Result::inverted) return;
  }
}

}  // namespace

u32 nodes_per_cell(CageKind kind) noexcept {
  switch (kind) {
    case CageKind::Tetrahedral: return 4;
    case CageKind::TetrahedralQuadratic: return 10;
    case CageKind::Lattice:
    case CageKind::Shell: return 0;
  }
  return 0;
}

BlockKind cell_block(CageKind kind) noexcept {
  return kind == CageKind::TetrahedralQuadratic ? BlockKind::QuadraticTetrahedra
                                                : BlockKind::Tetrahedra;
}

const char* cage_kind_name(CageKind kind) noexcept {
  switch (kind) {
    case CageKind::Tetrahedral: return "Tetrahedral";
    case CageKind::Lattice: return "Lattice";
    case CageKind::Shell: return "Shell";
    case CageKind::TetrahedralQuadratic: return "TetrahedralQuadratic";
  }
  return "unknown";
}

void jacobian_bernstein(const D3 x[10], f64 out[20]) noexcept {
  D3 c[3][4];
  corner_columns(x, c);
  f64 corner_min = 0.0;
  bernstein(c, out, corner_min);
}

f64 quadratic_volume(const D3 x[10]) noexcept {
  f64 b[20];
  jacobian_bernstein(x, b);
  f64 sum = 0.0;
  for (const f64 v : b)
    sum += v;
  return sum / 120.0;
}

JacobianCheck quadratic_jacobian(const D3 x[10], u32 max_depth) noexcept {
  D3 c[3][4];
  corner_columns(x, c);
  JacobianCheck out;
  out.lower = std::numeric_limits<f64>::infinity();
  out.upper = -std::numeric_limits<f64>::infinity();
  out.sampled = std::numeric_limits<f64>::infinity();
  Bary corners[4];
  for (u32 m = 0; m < 4; ++m)
    corners[m].l[m] = 1.0;
  bool uncertain = false;
  visit(c, corners, 0, max_depth, out, uncertain);
  if (out.result != JacobianCheck::Result::inverted && uncertain)
    out.result = JacobianCheck::Result::uncertain;
  return out;
}

void subdivide(const u32 cell[10], const D3 x[10], u32 out[32]) noexcept {
  // The corner tetrahedra: each a half-size copy of the cell at one of its corners.
  constexpr u32 k_corners[4][4] = {{0, 4, 6, 7}, {4, 1, 5, 9}, {6, 5, 2, 8}, {7, 9, 8, 3}};
  // The octahedron of the six edge nodes, split around one of its three diagonals, each ring
  // wound so that the four pieces are positive in a positive straight-sided cell.
  constexpr u32 k_octahedron[3][4][4] = {
      {{4, 8, 5, 6}, {4, 8, 6, 7}, {4, 8, 7, 9}, {4, 8, 9, 5}},   // diagonal (4, 8): 01 to 23
      {{6, 9, 4, 5}, {6, 9, 5, 8}, {6, 9, 8, 7}, {6, 9, 7, 4}},   // diagonal (6, 9): 02 to 13
      {{7, 5, 4, 6}, {7, 5, 6, 8}, {7, 5, 8, 9}, {7, 5, 9, 4}}};  // diagonal (7, 5): 03 to 12
  const f64 lengths[3] = {query::length(x[4] - x[8]), query::length(x[6] - x[9]),
                          query::length(x[7] - x[5])};
  u32 diagonal = 0;
  for (u32 d = 1; d < 3; ++d)
    if (lengths[d] < lengths[diagonal]) diagonal = d;
  u32 n = 0;
  for (const auto& t : k_corners)
    for (const u32 k : t)
      out[n++] = cell[k];
  for (const auto& t : k_octahedron[diagonal])
    for (const u32 k : t)
      out[n++] = cell[k];
}

}  // namespace engine::tissue::cells

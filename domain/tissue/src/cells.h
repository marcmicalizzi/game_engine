#pragma once

// The cells a region is made of (docs/subsystems/tissue.md, "The ten-node cell"): four-node
// tetrahedra, the runtime's kind, and ten-node (quadratic) tetrahedra, the kind a certified
// reference body is solved on. Private to the module; content-build code, in double, deterministic.
//
// **A ten-node cell is a quadratic map from the reference tetrahedron**, x(l) = sum N_i(l) x_i over
// the barycentric coordinates l with N_v = l_v (2 l_v - 1) at a corner and N_ab = 4 l_a l_b at the
// node of edge (a, b). Its Jacobian's columns are linear in l, so det J is a **cubic**, and a cubic
// on a tetrahedron is exactly twenty Bernstein coefficients (Johnen, Remacle and Geuzaine 2013,
// the validity test Gmsh runs on curved elements). Two things follow at once, and nothing here
// approximates either:
//
//   - the cell's volume, the integral of det J, is exactly the coefficients' mean times the
//   reference
//     tetrahedron's volume (every cubic Bernstein polynomial integrates to a twentieth of it);
//   - det J lies between the least and the greatest coefficient, and at a corner it *is* the
//   corner's
//     coefficient, so a least coefficient above zero proves the cell valid and a corner at or below
//     zero proves it inverted. Between the two, the cell is split into eight and each piece's
//     coefficients recomputed, to a stated depth.
//
// The node order is Gmsh's (`MSH` type 11): the corners, then the nodes of edges (0, 1), (1, 2),
// (0, 2), (0, 3), (2, 3), (1, 3). VTK's names the last two the other way round.
//
// **The same-node linear subdivision** is what a row whose meaning is linear walks: the four corner
// tetrahedra and the inner octahedron split along its shortest diagonal at the construction, eight
// four-node cells over the cell's own ten nodes. It is conforming across cells whatever diagonal
// each cell chose (every face of a cell is split into the same four triangles), so its boundary is
// the piecewise-linear surface through every boundary node.

#include "mesh_query.h"

#include <core/base/types.h>

#include <schemas/tissue.h>

namespace engine::tissue::cells {

using query::D3;

// Gmsh's edge order for the ten-node tetrahedron: node 4 + k lies on the edge between corners
// k_quadratic_edges[k][0] and k_quadratic_edges[k][1].
inline constexpr u32 k_quadratic_edges[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {2, 3}, {1, 3}};

// Nodes a cell of a cage kind this build reads has (4 or 10), and 0 for a kind it does not.
u32 nodes_per_cell(CageKind kind) noexcept;
// The block kind a cage kind's cells are stored in (Tetrahedra, QuadraticTetrahedra).
BlockKind cell_block(CageKind kind) noexcept;
const char* cage_kind_name(CageKind kind) noexcept;

// The twenty Bernstein coefficients of a quadratic tetrahedron's det J over the reference
// tetrahedron, in the order of the multi-indices (i <= j <= l) over the corners.
void jacobian_bernstein(const D3 x[10], f64 out[20]) noexcept;

// The exact signed volume of a quadratic tetrahedron: the integral of det J, the Bernstein
// coefficients' sum over 120. A straight-sided cell (every edge node at its edge's midpoint) gives
// its corner tetrahedron's volume.
f64 quadratic_volume(const D3 x[10]) noexcept;

// Whether det J stays positive over the whole cell. The search stops at the first piece that proves
// the cell inverted, so `lower` and `upper` bound det J over the whole cell only when it is not.
struct JacobianCheck {
  enum class Result : u8 { positive, inverted, uncertain };
  Result result = Result::positive;
  f64 lower = 0.0;    // a lower bound on det J over the cell: the least coefficient of the pieces
  f64 upper = 0.0;    // an upper bound on det J over the cell
  f64 sampled = 0.0;  // the least det J found at a point: an upper bound on its minimum
  u32 depth = 0;      // the deepest subdivision the answer needed
};
JacobianCheck quadratic_jacobian(const D3 x[10], u32 max_depth) noexcept;

// The eight four-node cells of the same-node linear subdivision of the ten-node cell `cell` (node
// indices), whose node positions are `x`: out[4 k .. 4 k + 3] is the k-th, positively oriented when
// the cell is straight-sided and positive. The inner octahedron is split along the shortest of its
// diagonals (4, 8), (6, 9), (7, 5) at `x`, the first on a tie.
void subdivide(const u32 cell[10], const D3 x[10], u32 out[32]) noexcept;

}  // namespace engine::tissue::cells

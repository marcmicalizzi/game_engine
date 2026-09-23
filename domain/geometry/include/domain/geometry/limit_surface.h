#pragma once

// Loop limit surfaces as fixed sparse operators (docs/subsystems/geometry.md, "Limit surfaces and
// surface bindings"; docs/plan/05-simulation.md §5.16, the 2026-09-23 addendum's item 3;
// docs/plan/07-content-pipeline.md §7.11).
//
// Why it exists. A deformable region's render surface follows a coarse control cage, and the
// neutral torso studies showed that the interpolant between the two decides what the viewer sees:
// resampling the cage's facets puts a crease at every cage edge (a render-edge dihedral ratio of
// three to eleven across cage edges against non-crossing ones), and cubic PN patches, which shade
// smoothly, still jump 19 degrees in geometric normal at patch edges and are not linear in the
// nodes, so the transfer's volume bias moves with the state. The plan therefore asks for an
// interpolant that is **linear in the cage's nodes and G1**: a subdivision limit surface. Every
// point of it is a fixed linear combination of the control positions, so the whole dense surface
// is one sparse matrix evaluated once at build time, and the per-frame cost is one small matvec
// (`apply`) — 83,233 multiply-adds of a Vec3 for the fixture's 121-node cage at level 3.
//
// **The rules are the authoring side's, exactly** (the round-two engine response's §1.2, and the
// `loop_reference.py` the Blender side ran), because the engine and the authoring tools must
// compute the same surface to the float for a binding authored on one to mean the same on the
// other:
//
//   interior edge point  3/8 (a + b) + 1/8 (c + d), c and d the two opposite vertices
//   boundary edge point  1/2 (a + b)
//   interior vertex      (1 - n beta) v + beta sum(n_i),
//                        beta = (5/8 - (3/8 + cos(2 pi / n) / 4)^2) / n
//   boundary vertex      3/4 v + 1/8 (l + r), l and r its two boundary neighbours
//   corner vertex        v (Hoppe's corner rule; the caller tags corners)
//   limit, interior      (k v + sum(n_i)) / (k + n), k = 3 / (8 beta)
//   limit, boundary      (l + 4 v + r) / 6, the cubic B-spline of the boundary polygon
//   limit, corner        v
//
// that is, Loop's scheme with Hoppe et al.'s (1994) boundary and corner rules and the standard
// edge mask next to a boundary. The refined numbering is the reference's too: a level keeps the
// previous level's vertices at their indices and appends one vertex per edge in lexicographic
// order of `(min, max)`, and a triangle `(a, b, c)` becomes `(a, ab, ca), (b, bc, ab),
// (c, ca, bc), (ab, bc, ca)`, so the operator built here is row for row and column for column the
// matrix the authoring side exported.
//
// **What "G1" does and does not cover here.** Loop's limit is tangent-plane continuous at every
// interior vertex, including the fixture's valence-24 pole (where it is flat, a known weak point
// of high valences). At a boundary vertex with `k` faces, these rules have cross-boundary
// eigenvalues `3/8 + cos(m pi / k) / 4` beside the boundary curve's `1/2`; the tangent plane is
// spanned by the first cross eigenvector and the curve's tangent only while the second cross
// eigenvalue stays below `1/2`, which holds for `k <= 5`, ties it at `k = 6` and fails above (the
// reason later schemes, Biermann, Levin and Zorin 2000 among them, changed the boundary rules).
// The authoring side's rules are kept as they are, so a control boundary vertex with six or more
// faces is a content fault; `LoopLimitSurface::max_boundary_faces` reports the largest.
//
// **Normals.** Limit normals are not linear in the nodes (they are a normalized cross product), so
// what is linear, and emitted, is the pair of **limit tangent operators**: `tangent_u` and
// `tangent_v`, whose cross product is the limit normal at each refined vertex (`limit_normals`).
// Interior vertices use the eigenvector masks `sum cos(2 pi i / n) p_i` and `sum sin(2 pi i / n)
// p_i` over the one-ring in counter-clockwise order; a boundary vertex with `k` faces uses the
// boundary polygon's tangent `p_0 - p_k` and the cross-boundary left eigenvector of *these* rules,
// derived in docs/subsystems/geometry.md and checked by the tests against deeper subdivision. They
// are two more matvecs of about the same width, which is why the surface binding does **not** use
// them per frame: the binding's offset transport uses the dense triangle's own normal, as the
// authoring side's binding does, and the tangent operators serve validation (a render surface's
// deviation from the limit normal) and a dense surface drawn on its own.
//
// Deterministic: every weight is composed in double in a fixed order (the stencil's own order,
// columns ascending) and rounded to f32 once, with the rounding residual put on the row's largest
// entry so a position row sums to 1 and a tangent row to 0 as closely as f32 can say it.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <span>
#include <string>

namespace engine::geometry {

// A sparse matrix in compressed rows. Row r's entries are `[row_offsets[r], row_offsets[r + 1])`
// of `column_index` and `weight`, columns strictly increasing within a row. Rows are what the
// operator produces (refined vertices), columns what it consumes (control vertices). The widths
// are the ones the brief fixed for the first cut: a `u32` column costs two bytes a nonzero over a
// `u16` one, and the cage-size limit (800 nodes, ADR-0029) would fit in sixteen bits; that
// narrowing is a measured change for later, not an assumption to build in now.
struct CsrMatrix {
  u32 column_count = 0;
  Vector<u32> row_offsets;   // rows() + 1 entries; empty for a matrix of no rows
  Vector<u32> column_index;  // one per nonzero
  Vector<f32> weight;        // one per nonzero

  u32 rows() const noexcept { return row_offsets.empty() ? 0 : row_offsets.size() - 1; }
  u32 nonzeros() const noexcept { return weight.size(); }
  u32 max_row_width() const noexcept;
  // The three arrays' bytes: what a region holds resident for the operator.
  usize bytes() const noexcept;
};

// `out[r] = sum over row r of weight * columns[column]`: the per-frame matvec. Rows in order and
// each row's entries in order, so the matrix and `out` stream; the cage is copied once into 16-byte
// (x, y, z, 0) lanes on the stack, which keeps it in L1 and makes an entry one 128-bit load, and
// each row accumulates in four partial sums (entry e into sum e mod 4, added pairwise at the end)
// so that a row is not one chain of dependent additions. A matrix of more than 1,024 columns —
// wider than any cage the budgets allow — takes a scalar path that sums in the same order.
// `columns.size()` must equal `matrix.column_count` and `out.size()` `matrix.rows()`; `out` must
// not alias `columns`.
void apply(const CsrMatrix& matrix, std::span<const Vec3> columns, std::span<Vec3> out) noexcept;

inline constexpr u32 k_loop_max_level = 4;

enum class LoopParentKind : u32 { vertex = 0, edge = 1, face = 2 };

// Where a refined vertex came from on the control mesh: a control vertex (it *is* that vertex,
// refined), a point on a control edge (`index` into `LoopLimitSurface::control_edges`), or a point
// inside a control face.
struct LoopParent {
  LoopParentKind kind = LoopParentKind::vertex;
  u32 index = 0;
};

struct LoopSurfaceOptions {
  u32 level = 3;  // subdivision steps before the limit mask, 0 to k_loop_max_level
  // Control vertices that stay where they are (Hoppe's corner rule): weight 1 on themselves at
  // every level and in the limit. Sorted or not, duplicates allowed. Empty for a smooth boundary.
  std::span<const u32> corners;
  bool tangents = true;  // also build `tangent_u` and `tangent_v`
};

struct LoopLimitSurface {
  u32 level = 0;
  u32 control_vertex_count = 0;
  // Two per control edge, `a < b`, in lexicographic order: what an edge `LoopParent` indexes.
  Vector<u32> control_edges;
  // Three per refined triangle, wound like the control triangle it lies in.
  Vector<u32> faces;
  Vector<u32> face_parent;           // the control triangle each refined triangle lies in
  Vector<LoopParent> vertex_parent;  // one per refined vertex
  // Refined vertex x control vertex. `limit` maps control positions to the limit positions of the
  // refined vertices; its rows are nonnegative and sum to 1.
  CsrMatrix limit;
  // The limit tangents at the refined vertices (empty unless asked for): rows sum to 0, and
  // `cross(tangent_u x, tangent_v x)` points to the side the triangles' counter-clockwise winding
  // faces. At a corner, where the limit has no tangent plane, they span the corner's own fan.
  CsrMatrix tangent_u;
  CsrMatrix tangent_v;
  // The most faces around one control boundary vertex that is not a corner. At six or more,
  // Hoppe's rules are not tangent-plane continuous at that vertex (see the header comment).
  u32 max_boundary_faces = 0;

  u32 vertex_count() const noexcept { return vertex_parent.size(); }
  u32 triangle_count() const noexcept { return faces.size() / 3; }
};

// Builds the level-`options.level` refinement of a triangle control mesh and its limit operator.
// The control mesh must be an oriented 2-manifold, boundaries allowed: every index in range, no
// triangle with a repeated corner, every edge shared by one or two triangles (two wound in
// opposite directions), and every vertex's triangles one fan. Positions are not an input: the
// operator is a function of the topology alone, and `apply` evaluates it on any state.
bool build_loop_limit_surface(std::span<const u32> control_faces, u32 control_vertex_count,
                              const LoopSurfaceOptions& options, LoopLimitSurface& out,
                              std::string* error = nullptr);

// Unit limit normals from the two tangent operators' outputs; a vertex whose tangents are parallel
// or zero gets the zero vector rather than NaN.
void limit_normals(std::span<const Vec3> tangent_u, std::span<const Vec3> tangent_v,
                   std::span<Vec3> out) noexcept;

}  // namespace engine::geometry

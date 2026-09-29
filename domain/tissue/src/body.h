#pragma once

// A region read as a body (docs/subsystems/tissue.md, "Cage derivation"): its nodes, cells and
// states, each cell's material mixed from the phases, and the three geometric tools the cage
// derivation, the `cage.*` rows and the soft body are built from — locating a point in the cells
// and evaluating a state's field there, the volume two tetrahedra share, and a region's boundary.
// Private to the module; content-build code, in double, deterministic: every traversal breaks ties
// by index, and nothing here calls the C library for a decision (the square root is the only
// function it uses, and IEEE 754 fixes it), so the same input gives the same numbers on every
// toolchain, as the content build's derived data must (ADR-0035).

#include "cells.h"
#include "mesh_query.h"

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/tissue/tissue_file.h>

#include <string>
#include <string_view>

namespace engine::tissue::detail {

using query::D3;

struct BodyState {
  std::string name;                  // "construction" for the first
  const RegionState* def = nullptr;  // null for the construction
  Vector<Vec3> nodes;
};

// A region's nodes, cells, states and per-cell materials, read straight from its blocks.
struct Body {
  const Region* region = nullptr;
  u32 per_cell = 4;          // 4, or 10 for a ten-node region
  Vector<Vec3> nodes;        // the construction
  Vector<u32> cells;         // `per_cell` node indices each
  Vector<u32> tets;          // the linear cells: the cells, or the same-node subdivision (cells.h)
  Vector<u32> parent;        // each linear cell's cell
  Vector<BodyState> states;  // the construction first, then the definition's in its order
  // Each cell's phase fractions, `phase_count` a cell, the first phase's being what the others
  // leave (as region.materials reads them), and the mixed moduli and density.
  u32 phase_count = 0;
  Vector<f64> fractions;
  Vector<f64> bulk_pa;
  Vector<f64> shear_pa;
  Vector<f64> density;

  bool quadratic() const noexcept { return per_cell == 10; }
  u32 cell_count() const noexcept { return per_cell == 0 ? 0u : cells.size() / per_cell; }
  const u32* cell(u32 c) const noexcept { return cells.data() + per_cell * c; }
  // A cell's exact signed volume at `x`: a tetrahedron's, or a ten-node cell's integral of det J.
  f64 cell_volume(u32 c, const Vector<Vec3>& x) const noexcept;
  // The state of this name, or null.
  const BodyState* state(std::string_view name) const noexcept;
  // The first state whose role is `role`, or null.
  const BodyState* state_with_role(StateRole role) const noexcept;
};

// Reads the region `name` of `file` (every block by kind and count, every index in range, a
// fraction per cell for every phase past the first). False, with a sentence, otherwise.
bool load_body(const TissueFile& file, std::string_view name, Body& out, std::string* error);

// The nodes a node set of the region names, or false.
bool read_node_set(const TissueFile& file, const Region& region, std::string_view set,
                   Vector<u32>& out);

// The mass and volume of a body's cells at the construction, exact (a ten-node cell's integral of
// det J), and of its linear cells.
struct BodyMass {
  f64 volume_m3 = 0.0;
  f64 mass_kg = 0.0;
  f64 linear_volume_m3 = 0.0;
  f64 linear_mass_kg = 0.0;
};
BodyMass body_mass(const Body& body);

// ---- locating a point in the cells
// ---------------------------------------------------------------
//
// Where a point of the construction lies in a body's cells, as the cell and its barycentric
// coordinates in the cell's reference tetrahedron: exact for a four-node cell, by Newton's method
// on the quadratic map for a ten-node one (from the corner tetrahedron's coordinates, to 1e-12 of
// the cell's size). A point inside no cell — outside the body — takes the cell it is least outside
// of, its coordinates unclamped, so evaluating a field there extends the cell's own polynomial:
// `outside` says by how much (the most negative coordinate's magnitude; 0 inside).
class CellLocator {
 public:
  void build(const Body& body);
  struct Location {
    u32 cell = query::k_none;
    f64 l[4] = {1.0, 0.0, 0.0, 0.0};
    f64 outside = 0.0;
  };
  Location locate(D3 p) const;
  // The field's value at a location: the cell's shape functions over its nodes' values.
  D3 interpolate(const Location& at, const Vector<Vec3>& field) const;

 private:
  bool try_cell(u32 c, D3 p, Location& out) const;
  const Body* body_ = nullptr;
  Vector<D3> lo_;
  Vector<D3> hi_;
  // A uniform grid over the cells' boxes: each voxel lists the cells whose box meets it.
  D3 origin_;
  f64 voxel_ = 1.0;
  u32 dims_[3] = {1, 1, 1};
  Vector<u32> voxel_start_;
  Vector<u32> voxel_cells_;
};

// The shape functions of a cell at barycentric coordinates `l`: 4 or 10 values (Gmsh's order).
void shape_functions(const f64 l[4], u32 per_cell, f64 out[10]) noexcept;

// ---- the volume two tetrahedra share
// -------------------------------------------------------------
//
// Tetrahedron `a` clipped by the four half-spaces of tetrahedron `b`, as a set of tetrahedra (a
// piece that keeps one corner is a tetrahedron, one that keeps two or three a prism split in
// three), their volumes summed: exact up to rounding, and a function of the eight points alone.
// Both are taken as positively oriented; `b`'s faces are its planes.
f64 tet_overlap(const D3 a[4], const D3 b[4]) noexcept;

// ---- boundaries ---------------------------------------------------------------------------------

// The faces used by one tetrahedron of `tets`, wound outward (the corner opposite each behind it):
// the linear boundary of a body at `nodes`.
Vector<u32> boundary_faces(const Vector<u32>& tets, const Vector<Vec3>& nodes);

}  // namespace engine::tissue::detail

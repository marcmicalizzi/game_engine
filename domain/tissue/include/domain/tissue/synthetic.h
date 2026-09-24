#pragma once

// A synthetic tissue definition, generated from formulas (docs/subsystems/tissue.md, "The
// synthetic definition"): the tests' fixture, `engine-content tissue example`'s output — a complete
// interchange the authoring side can read as a worked example of the format — and the end-to-end
// test's input. Nothing in it is measured or anatomical.
//
//   the skin       a 41 x 41 grid over [-0.1, 0.1]^2 m at z = 0, quads, facing +z; the observation
//                  raises it by up to 1 mm inside its declared domain
//   the region     a slab cut from a torus: tube radii 45 and 60 mm about a 300 mm major circle
//                  whose axis is y, u in [-0.15, 0.15] rad along it and v in [-0.6, 0.6] rad around
//                  the tube; 7 x 7 nodes on each of 3 layers (147), 72 hexahedra of 6 tetrahedra
//                  (432). The outer layer is the top sheet, 2 mm under the skin at its apex; the
//                  inner layer the support sheet; the closed shell stitches their limit surfaces
//   the frame      a box 240 mm square whose top is 4 mm under the slab's lowest node
//   the states     construction; "standing", sagged 0.5 mm by inverse statics (the reference);
//                  "rest", 1 mm above the construction; "pressed", 3 mm down and 1 mm along y at
//                  the centre with the rim fixed (a response)
//   the binding    the skin over the slab, footpoints from the nearest-point binder under the
//                  limit-interpolated rule, stored as authored records

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/limit_surface.h>
#include <domain/tissue/tissue_file.h>

namespace engine::tissue {

namespace synthetic {
inline constexpr u32 k_nu = 7;  // nodes along u (along the major circle)
inline constexpr u32 k_nv = 7;  // nodes along v (around the tube)
inline constexpr u32 k_layers = 3;
inline constexpr u32 k_grid = 41;  // skin vertices a side
inline constexpr f64 k_major = 0.30;
inline constexpr f64 k_outer = 0.060;
inline constexpr f64 k_inner = 0.045;
inline constexpr f64 k_cover = 0.002;
inline constexpr f64 k_u = 0.15;
inline constexpr f64 k_v = 0.60;

// The region node at (i along u, j along v) on a layer, 0 the top.
constexpr u32 node(u32 i, u32 j, u32 layer) { return layer * k_nu * k_nv + j * k_nu + i; }
}  // namespace synthetic

struct SyntheticTissue {
  TissueFile file;
  geometry::LoopLimitSurface top;  // the top sheet's level-3 surface, with its tangent operators
  Vector<Vec3> reference_nodes;    // the "standing" state
};

SyntheticTissue make_synthetic_tissue();

}  // namespace engine::tissue

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
//
// **The quadratic variant** (`SyntheticOptions::quadratic`) is the same slab as ten-node cells, a
// reference body: a node on each of the 698 edges (845 nodes in all, so no runtime cage), placed
// on the torus at the mean of its ends' parameters, which curves the cells a little as a mesher's
// boundary-fitted ones are, and moved in each state by the states' bump at its own place; the
// cells in Gmsh's order; each sheet's control triangle split in four over its edges' nodes (288
// a sheet, at Loop level 2, so the dense surface is the four-node slab's 4,608 triangles), so the
// sheets still carry every moving surface node; the side wall's and the support's edge nodes in the
// rim and posterior sets. `role` is `Reference`.

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

struct SyntheticOptions {
  // Ten-node cells and a reference body (above).
  bool quadratic = false;
};

SyntheticTissue make_synthetic_tissue(const SyntheticOptions& options = {});

// ---- the layered model's worked examples (docs/subsystems/tissue.md, "The layered model's
// fixtures") --------------------------------------------------------------------------------------
//
// Two neutral mechanical fixtures in the shape of the design packet's (RUNS.md), generated from
// formulas: nothing is solved, measured or anatomical, and every state is a prescribed shape.
// Both are ten-node reference bodies with straight-sided cells (edge nodes at the midpoints) and
// neither declares an observation or a Loop sheet: their skin, where they have one, is the
// material boundary.
//
//   the slab       160 x 80 x 15 mm of fat (8 x 4 x 2 hexahedra of six cells, 384 ten-node cells)
//                  0.7 mm above a closed base frame; a uniform thickness field over its lower
//                  surface, partitioned into two depots (x < 0, x > 0); a mechanical skin on its
//                  upper surface, sampled at every node of it; a self-contact pair of that skin
//                  (offset 1 mm, locality 5 mm) with one incident exclusion, and the lower surface
//                  against the base (offset 0.1 mm); states rest, reference and a 3% compression
//                  with a 0.25 mm upward bias; a certificate record of the reference, uncertified
//   the fusiform   120 mm long, radius 5 mm at the ends and 15 mm at the middle (12 x 2 x 2
//                  hexahedra, 288 ten-node cells), between two box handles 0.7 mm beyond its ends:
//                  an essential attachment at the origin with a patch and a transition collar, a
//                  spring at the insertion; the active rest driver `fusiform-arch-v1` (activation
//                  and pose angle, two samples: the identity and a = 0.5 at 60 degrees); states
//                  rest, reference and "posed", the insertion handle turned 60 degrees about y
//                  and the body bent smoothly to follow it; contact against both handles, the
//                  origin's patch excluded against its own
//   the tied slab  the slab held at both ends by box jaws across a declared 0.4 mm gap: each end
//                  face a material surface, the patch of an essential grip whose interface is
//                  `Separated` (Attachment version 3), and a unilateral pair against its jaw with
//                  no exclusion; the jaws move in with the 3% compression by FrameStates
//
// Each declares its requirements: the records, block kinds, rows and laws its meaning depends on.
TissueFile make_layered_slab();
TissueFile make_layered_fusiform();
TissueFile make_layered_tied_slab();

}  // namespace engine::tissue

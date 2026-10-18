#pragma once

// The date palm (docs/subsystems/flora.md, "The date palm"): `engine.flora.Palm` in, a mesh out.
//
// **What comes out.** An `assets::MeshData` with up to three primitives, each its own material:
//
//   0  the trunk      bark brown, one-sided and closed at the apex, built with normal cones on
//   1  the crown      frond green, two-sided geometry, built with normal cones off
//   2  the skirt      dead-frond tan, two-sided, cones off; absent when `dead_skirt_fronds` is 0
//
// A two-sided material is `double_sided`, which is what `flora::build_flora_container` reads to
// build that primitive's clusters with `normal_cones = false` (geometry.md, "Normal cones"): a
// leaflet is seen from both faces, and a cone fit to one face would drop it seen from the other.
// The materials are flat PBR constants for now; textures are roadmap row G60.
//
// **Deterministic.** The same parameters give the same bytes on every toolchain and at either CPU
// baseline: the solver's arithmetic is IEEE `+ - * /` and `sqrt` in f64 with contraction off
// (ADR-0035), its sines and cosines are `det::sin`/`det::cos` (core/math/deterministic.h), its
// randomness a splitmix64 over the seed, and it runs on one thread.

#include <core/base/types.h>
#include <domain/assets/gltf.h>

#include <schemas/flora.h>
#include <string>

namespace engine::flora {

// How the solver spent the triangle budget (flora.md, "The budget"): the detail it chose and what
// each part came to. `steps` is how many rungs of the ladder it went down; 0 is the full detail.
struct PalmReport {
  u32 triangles = 0;
  u32 trunk_triangles = 0;
  u32 crown_triangles = 0;
  u32 skirt_triangles = 0;
  u32 vertices = 0;
  u32 rings = 0;               // leaf-scar rings up the trunk
  u32 rows_per_ring = 0;       // trunk vertex rows each ring's profile is drawn with: 4, 2 or 1
  u32 trunk_segments = 0;      // around the trunk
  u32 rachis_sides = 0;        // vertices across a rachis: 3 (a keel) or 2 (a ribbon)
  u32 leaflet_segments = 0;    // along a leaflet: 3, 2 or 1 (5, 3 or 1 triangles)
  u32 leaflets_per_side = 0;   // what was asked, or fewer and wider past the last rung
  f32 leaflet_width_m = 0.0f;  // as built: widened when leaflets were thinned
  u32 steps = 0;
};

// The ranges `engine.flora.Palm` documents. False, with a sentence naming the first field out of
// range and its range, for parameters the solver does not take.
bool palm_in_range(const Palm& params, std::string& error);

// Builds the palm. False, with a sentence, when a parameter is out of range or the budget cannot
// hold even the solver's coarsest palm with these counts (the sentence says how many it needs).
// `out` is replaced.
bool solve_palm(const Palm& params, assets::MeshData& out, PalmReport* report = nullptr,
                std::string* error = nullptr);

}  // namespace engine::flora

#pragma once

// The mechanical repair of **UV-degenerate triangles** (docs/subsystems/geometry.md, "UV-degenerate
// triangles: the repair"): the triangles of an atlas island too small to be sampled as itself, in
// a mesh whose atlas is otherwise sound.
//
// Why it exists. Experiment E10 (docs/experiments/e10-generated-props.md) put twenty generated
// props through the content build, and eight failed the atlas check on one island of **zero**
// texels made of a single triangle, in an atlas whose median island was thousands of texels; both
// TRELLIS.2 samples failed the same way. That is not fragmentation — nothing needs repacking or
// rebaking — but a UV-hygiene fault the generator left behind: an island of no area is one no LOD
// level can sample, and no seam-respecting simplification can coarsen it. So the build folds it
// into the island beside it, which is a function of the mesh alone and needs no texture work.
//
// **What "UV-degenerate" means, and why it is a property of the island.** The first reading of
// E10 was "a triangle whose three UVs coincide", and a per-triangle floor on UV area was the first
// rule written. Measured on the sources it catches nothing: across the twenty Meshy files exactly
// one triangle has zero UV area, and it lies inside a sound island, while twelve of them had an
// island of zero texels in the container. The zero is the container's — it stores UVs as half
// floats, whose step between 0.5 and 1 is two texels of a 4096 atlas, so an island under a texel
// stores as a point. And a per-triangle threshold high enough to catch those would flag millions
// of legitimate triangles of a dense mesh (4.7 million under a texel on one 7.5-million-triangle
// cactus), every one of them inside a sound island. The defect is the island, so the rule is:
//
// 1. A triangle is **UV-degenerate** when the atlas island it belongs to covers less than
//    `threshold_texels` texels of a 4096 atlas (`k_uv_degenerate_texels`, one texel: the line
//    below which an island cannot be sampled as itself), with the area measured on the UVs **as
//    the container stores them** (`encode_half2`), because that is what the renderer samples and
//    what `engine-content stats` measures. A tiny triangle inside a sound island is never one.
// 2. A UV-degenerate triangle that is also **geometrically** degenerate — thinner than one step
//    of the mesh's 16-bit position grid (`quantize_positions`), below what the rasterizers
//    resolve — is **dropped**: it covers no pixel at any zoom the grid serves.
// 3. Every other UV-degenerate triangle is **refolded** into the island it borders: each corner
//    takes the UV — and the normal — that island already has at the corner's position, and a
//    corner the island does not reach takes the point of the shared edge nearest to it (the
//    triangle's UV collapses onto that edge). An island of several triangles folds in from its
//    border, one ring at a time. The normal comes along because the corner is meant to *become*
//    the island's vertex there: a remesher that splits its normals along every chart border (E10's
//    second pass, every one of 22,862 seam pairs on one prop) would otherwise leave a corner with
//    the right UV and a different normal, which the weld keeps apart, and the island by index.
// 4. A triangle with no sound island beside it is left as it is and counted as unrepaired.
//
// "Island" and "beside" are decided on **atlas vertices** — a position and a UV taken together,
// which is what the weld and the LOD builder's seam rule key a seam on — so the answer is the same
// whether or not the source was indexed, and the repair runs **before the weld**: a corner that
// took its neighbour's UV is then bit-identical to that neighbour's vertex in position and UV, and
// the one weld that follows merges the two whenever the skin binding (the one weld key this does
// not touch) agrees too. Nothing here decides whether two vertices may merge; the weld does.
//
// Deterministic: everything is visited in the index order of the input and in sorted-key order,
// never in hash or thread order, so the same mesh gives the same bytes on every machine.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <span>
#include <string>

namespace engine::geometry {

// The island area, in texels of a 4096 atlas as stored, under which an island's triangles are
// UV-degenerate: one texel, the line under which an island cannot be sampled as itself at any mip,
// which is also the E10 harness's island check.
inline constexpr f64 k_uv_degenerate_texels = 1.0;

// A run of the index buffer repaired on its own: a glTF primitive, in practice. A neighbour is
// only ever looked for inside the same run, because two primitives are two materials and one's
// atlas says nothing about the other's. `uv_mapped` false leaves the run's triangles alone (a
// material that samples no texture does not care what its UVs are); its `first` still moves when
// an earlier run loses triangles.
struct UvRepairRange {
  u32 first = 0;  // first index; a multiple of three
  u32 count = 0;  // indices; a multiple of three
  bool uv_mapped = true;
};

struct UvRepairOptions {
  f64 threshold_texels = k_uv_degenerate_texels;
};

// What the repair did, for the build's `repair` line. `triangles` is the sum of the three
// outcomes after it.
struct UvRepairReport {
  f64 threshold_texels = 0.0;
  u32 islands = 0;     // islands under the threshold, in uv-mapped runs
  u32 triangles = 0;   // their triangles: the UV-degenerate ones
  u32 refolded = 0;    // given their neighbouring island's UVs
  u32 dropped = 0;     // also thinner than a grid step: removed from the index buffer
  u32 unrepaired = 0;  // no sound island beside them, or dropping would have emptied the run
  u32 corners_from_neighbours = 0;  // atlas vertices that took the island's UV at their position
  u32 corners_on_edge = 0;          // atlas vertices the island does not reach, put on an edge
  bool changed() const noexcept { return refolded != 0 || dropped != 0; }
};

// Repairs `uvs`, `normals` and `indices` in place, as the rule above says, and rewrites `ranges`
// when triangles are dropped (runs keep their order; indices after a dropped triangle move down).
// `uvs` must be parallel to `positions`, and `normals` either parallel or empty; a mesh with no
// UVs has nothing to repair and is left alone. Only vertices of an island under the threshold are
// ever written. Returns false, with a sentence in `error`, only for input that is malformed (an
// index out of range, a run outside the buffer or not whole triangles, runs out of order or
// overlapping, a stream of the wrong length); the mesh is then unchanged.
bool repair_uv_degenerate_triangles(std::span<const Vec3> positions, Vector<Vec3>& normals,
                                    Vector<Vec2>& uvs, Vector<u32>& indices,
                                    std::span<UvRepairRange> ranges, const UvRepairOptions& options,
                                    UvRepairReport& report, std::string* error = nullptr);

// The UV area of one triangle, in texels of a 4096 atlas, from the UVs given.
f64 uv_area_texels(Vec2 a, Vec2 b, Vec2 c) noexcept;

// The same, from the UVs as the container stores them: each rounded to a half float first. This is
// the area the renderer samples and the number the threshold above is compared with.
f64 stored_uv_area_texels(Vec2 a, Vec2 b, Vec2 c) noexcept;

}  // namespace engine::geometry

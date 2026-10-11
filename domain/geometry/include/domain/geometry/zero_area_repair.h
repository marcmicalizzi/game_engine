#pragma once

// The mechanical repair of **zero-area triangles** (docs/subsystems/geometry.md, "Zero-area
// triangles: the repair"): a triangle with three distinct corner indices whose three positions
// span no area — collinear, or two or three of them at one point.
//
// Why it exists. Experiment E10 (docs/experiments/e10-generated-props.md, "Quad smart-mesh
// exports") put ten of Tripo's quad ("smart mesh") exports through the content build on
// 2026-10-09, and seven were refused at import with `geometry.degenerate_triangle`: each on a
// triangle with three distinct indices and no area, in a mesh of ten thousand sound ones — what a
// quad remesher leaves behind when it triangulates a quad one of whose corners lies on the
// opposite edge. Such a triangle covers no pixel at any zoom, has no normal and no UV area, and
// no LOD level or cluster cone needs it; refusing the whole asset for it made a mechanical fault
// a person's job. So the build drops it, counts it, and says so, as it does for the
// UV-degenerate islands of the sibling repair (`uv_repair.h`), and both builders run it at the
// same point so they build the same bytes.
//
// **The rule, and why exact zero.** A triangle is dropped when its three corner indices differ
// and `cross(p1 - p0, p2 - p0)` is exactly the zero vector in f32 — the test the importer's
// validation refused it with, operation for operation, so the repair covers exactly the class
// that was being refused for area and nothing the importer accepted moves a byte. It is not an
// area threshold: a relative floor (an area under some fraction of the bounds squared) would
// reach triangles that build and draw today and change every mesh that has one, for no failure
// it fixes, and the cone refit and the position grid already ignore a sliver thinner than a grid
// step. A zero-area triangle is also always below the UV repair's own drop line (thinner than a
// step of the 16-bit grid), so anything this drops, that repair would have dropped had its island
// been sub-texel: the two read as one family. The test is deterministic on every toolchain,
// because the tree compiles without floating-point contraction (ADR-0035): each product rounds
// on its own, so two coincident corners give an exact zero and nothing else depends on the
// compiler.
//
// What it leaves alone: a triangle that **repeats an index** (an indexing fault, not geometry; the
// content build still refuses it, and its diagnostic now says what this repair removed first),
// and a run's **last** triangle, which is never dropped, so a primitive is never emptied (it is
// counted as unrepaired, and the content build refuses a primitive with no area at all).
//
// Deterministic: triangles are visited in index order, the survivors keep their order, and runs
// keep theirs.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <span>
#include <string>

namespace engine::geometry {

// A run of the index buffer repaired on its own: a glTF primitive, in practice. A run is never
// emptied, and its `first` moves down when an earlier run loses triangles.
struct ZeroAreaRepairRange {
  u32 first = 0;  // first index; a multiple of three
  u32 count = 0;  // indices; a multiple of three
};

// What the repair did, for the build's `repair` line. `triangles` is `collinear + coincident`,
// and also `dropped + unrepaired`.
struct ZeroAreaRepairReport {
  u32 triangles = 0;   // zero-area triangles with three distinct corner indices
  u32 collinear = 0;   // of those, three distinct positions on one line
  u32 coincident = 0;  // of those, two or three corners at one position
  u32 dropped = 0;     // removed from the index buffer
  u32 unrepaired = 0;  // kept because dropping it would have emptied its run
  u32 repeated = 0;    // triangles that repeat an index: not this repair's, left alone
  bool changed() const noexcept { return dropped != 0; }
};

// The test, exactly as the importer's validation applied it: the cross product of the two edges
// from the first corner is the zero vector in f32.
bool spans_zero_area(Vec3 a, Vec3 b, Vec3 c) noexcept;

// Drops the zero-area triangles of `indices`, as the rule above says, and rewrites `ranges` (runs
// keep their order; indices after a dropped triangle move down). Vertices are never touched, so
// every per-vertex stream stays as it was; a vertex only a dropped triangle used is left
// unreferenced, for the weld to drop. Returns false, with a sentence in `error`, only for input
// that is malformed (an index out of range, a run outside the buffer or not whole triangles, runs
// out of order or overlapping); the mesh is then unchanged.
bool repair_zero_area_triangles(std::span<const Vec3> positions, Vector<u32>& indices,
                                std::span<ZeroAreaRepairRange> ranges, ZeroAreaRepairReport& report,
                                std::string* error = nullptr);

}  // namespace engine::geometry

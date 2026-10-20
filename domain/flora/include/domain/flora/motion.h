#pragma once

// The motion contract (docs/subsystems/flora.md, "Motion attributes"): what every species' solver
// emits beside its mesh so that a deform stage can sway a tree — bend its trunk, swing its fronds
// and branches — through the deformed-vertex pool without knowing which species it is.
//
// **Per part** (`geometry::ClusterFileMotionPart`, section 35 of the container): a part is what
// bends as one — the trunk, a frond, a branch — and records its `pivot` (where it meets its parent,
// mesh space; the trunk's is its base), its rest `direction` (the unit tangent it leaves the pivot
// along), its `length` (arc length, root to tip), and its `parent` (`geometry::k_motion_no_parent`
// for the trunk). Ids are dense, the trunk is 0, a parent's id is below its children's, and the
// ids are **stable across the budget ladder**: no rung drops or merges a part, so the same
// parameters at any budget name the same parts.
//
// **Per vertex** (section 36, one record per cluster vertex): its part, and its **bend weight** —
// 0 at the part's root rising to 1 at its tip, by arc length along the part's own axis. A vertex
// off the axis (a leaflet, a leaf card, the far side of the trunk) carries the weight of its
// station on the axis: the point of the axis it was built from, so a leaflet moves rigidly with
// the piece of rachis it stands on. The pivot and the rest direction are the part's, reached
// through the part id, rather than repeated per vertex: 32 bytes a part instead of 24 a vertex.
//
// A tree whose weights are all zero is the rest pose, which is what the renderer draws today.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/geometry/cluster_file.h>
#include <domain/geometry/cluster_lod.h>

namespace engine::flora {

inline constexpr u32 k_trunk_part = 0;
using MotionPart = geometry::ClusterFileMotionPart;

// What a solver emits beside its `assets::MeshData`: the parts, and per source vertex (parallel to
// `MeshData::positions`) its part and its bend weight in [0, 1].
struct Motion {
  Vector<geometry::ClusterFileMotionPart> parts;
  Vector<u32> vertex_part;
  Vector<f32> vertex_weight;
  // The structural table (structure.h; flora.md, "The structural table"): empty for a species that
  // emits none (the palm, the broadleaf, the conifer), otherwise one record per part in id order.
  Vector<geometry::ClusterFileStructuralPart> structure;
};

// The weight as the container stores it: a 16-bit unorm, rounded to nearest.
u16 encode_bend_weight(f32 weight) noexcept;
f32 decode_bend_weight(u16 weight) noexcept;

// The per-cluster-vertex records of section 36 for a mesh built from the solver's vertices: each
// cluster vertex's record is its source vertex's (`ClusterMesh::vertex_source`), for a mesh whose
// every primitive's DAG was built over the solver's whole vertex array and merged
// (`geometry::merge_cluster_lod` shifts each part's source indices by the counts before it, so the
// solver's vertex is the source index modulo the vertex count). False when the motion streams are
// empty or uneven, or a part id does not fit the record's 16 bits.
bool motion_records(const Motion& motion, const geometry::ClusterLodMesh& mesh,
                    Vector<geometry::ClusterFileMotionVertex>& out);

}  // namespace engine::flora

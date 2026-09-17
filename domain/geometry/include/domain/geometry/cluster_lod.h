#pragma once

// The cluster LOD DAG (docs/plan/04-renderer.md §4.3, ADR-0005): every level of detail of a
// mesh as clusters in the shared format, plus the two error bounds per cluster that make a
// view-dependent, crack-free cut possible in one pass with no hierarchy traversal:
//
//   render cluster c  <=>  screen_error(c.own) <= threshold  &&  screen_error(c.parent) > threshold
//
// `own` is the bound of the group c was simplified from (error 0 for original geometry),
// `parent` the bound of c's own group's simplified output (infinite when the group could not
// be simplified further). Errors are monotonic along every path of the DAG, so exactly one
// cluster on each path passes for any threshold. The GPU pass in domain/gfx/shaders/
// cluster_cull.slang evaluates the same test as `lod_selects` below.
//
// Built with meshoptimizer's clusterlod (demo/clusterlod.h, MIT): clusterize, partition into
// groups, simplify each group, repeat. Positions only for now; attributes, quantization, and
// pages follow.

#include <domain/geometry/cluster.h>

namespace engine::geometry {

// GPU-mirrored; keep in step with ClusterLodDesc in the shaders. 48 bytes.
struct ClusterLodDesc {
  Vec4 own;          // xyz center, w radius, in mesh space
  Vec4 parent;       // xyz center, w radius
  f32 own_error;     // mesh-space error of this cluster (0: original geometry)
  f32 parent_error;  // mesh-space error of the coarser version; k_lod_terminal_error when none
  u32 level;         // DAG depth; 0 is the original geometry
  u32 group;         // index of the group this cluster belongs to
};

inline constexpr f32 k_lod_terminal_error = 3.402823466e+38f;  // FLT_MAX: never coarse enough

struct ClusterLodOptions {
  u32 max_triangles = 124;   // per cluster, 4..256
  u32 max_vertices = 64;     // per cluster, at most 255
  bool ray_tracing = false;  // clusterlod's RT-oriented defaults (smaller, spatially compact)
};

struct ClusterLodMesh {
  ClusterMesh mesh;                  // every cluster of every level; level 0 comes first
  Vector<ClusterLodDesc> lod;        // parallel to mesh.clusters
  Vector<u32> level_cluster_counts;  // clusters per DAG level
  u32 group_count = 0;
  u32 leaf_triangle_count = 0;  // level 0 total, equal to the source triangle count
};

bool build_cluster_lod(std::span<const Vec3> positions, std::span<const u32> indices,
                       const ClusterLodOptions& options, ClusterLodMesh& out,
                       std::string* error = nullptr, const AttributeSource& attributes = {});

// View-dependent selection, the reference for the GPU pass. Errors are projected to screen
// pixels the way clusterlod documents: error / max(distance - radius, znear) * proj_scale.
struct LodView {
  Vec3 camera{};
  f32 znear = 0.1f;
  // cot(fov_y / 2) * viewport_height / 2: mesh-space size at distance 1 to pixels.
  f32 proj_scale = 0.0f;
  f32 threshold_px = 1.0f;
};
f32 projected_error(Vec4 sphere, f32 error, const LodView& view) noexcept;
bool lod_selects(const ClusterLodDesc& lod, const LodView& view) noexcept;
// Appends the indices of the selected clusters; returns how many.
u32 select_lod(const ClusterLodMesh& mesh, const LodView& view, Vector<u32>& out);
// The same with a threshold on raw mesh-space error (no camera): own <= t < parent.
u32 select_lod_raw(const ClusterLodMesh& mesh, f32 threshold, Vector<u32>& out);

// Level 0 covers every source triangle exactly once; every cluster's own error is at most its
// parent error; the raw cut is non-empty for every threshold; the triangle count of the raw
// cut never grows with the threshold.
bool validate_cluster_lod(const ClusterLodMesh& mesh, std::span<const u32> source_indices,
                          std::string* error = nullptr);

}  // namespace engine::geometry

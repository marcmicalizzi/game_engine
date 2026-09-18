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
  bool normal_cones = true;  // false: k_cone_none on every cluster (see ClusterBuildOptions)
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

// Merges DAGs built over parts of one mesh (a glTF primitive per material, for example) into
// one: the vertex, attribute, and triangle streams are concatenated with the offsets shifted,
// source vertex indices and group indices shift by the parts before, level counts add up, and
// the clusters are reordered so every level-0 cluster comes first (the direct-draw convention;
// the cut test is per cluster and does not care about order). `part_of_cluster`, when given,
// receives the part index of every merged cluster in output order, which is how a caller maps
// clusters back to materials. Fails on an empty list or an empty part.
bool merge_cluster_lod(std::span<const ClusterLodMesh> parts, ClusterLodMesh& out,
                       Vector<u32>* part_of_cluster = nullptr, std::string* error = nullptr);

// Where one mesh landed in the buffers merge_cluster_meshes built, and the 16-bit grid its
// positions stayed on. A renderer uploads one of these per mesh (`gfx::MeshDesc` mirrors the
// first two fields and the grid) and indexes it by an instance's mesh id.
struct ClusterMeshPart {
  u32 first_cluster = 0;  // in the merged `mesh.clusters`
  u32 cluster_count = 0;
  u32 leaf_cluster_count = 0;  // the level-0 clusters of this mesh
  // Where they start, relative to `first_cluster`. Zero for a mesh merged the default way, whose
  // leaves come first; `cluster_count - leaf_cluster_count` for a mesh that was laid out in pages
  // before the merge, because a paged mesh is ordered coarse to fine and its leaves are last.
  u32 first_leaf_cluster = 0;
  u32 first_vertex = 0;  // in the merged vertex, attribute, and quantized streams
  // This mesh's run of the scene's page table (`merge_paged_cluster_meshes`), zero and empty when
  // the scene carries none.
  u32 first_page = 0;
  u32 page_count = 0;
  Vec3 quant_origin{};  // this mesh's own grid, unchanged by the merge
  f32 quant_scale = 1.0f;
};

// Whether a merge reorders each mesh's clusters so its level-0 clusters come first.
//
// `leaves_first` is the scene convention: a direct draw of the leaves is then the first
// `leaf_cluster_count` clusters of the mesh. `keep` leaves a mesh's clusters in the order it
// arrived in, which is what a mesh laid out in **pages** needs: `build_cluster_pages` orders the
// clusters coarse to fine so that each page is a contiguous run of them, and pulling the leaves
// to the front would leave every page naming clusters that are no longer beside each other.
enum class ClusterOrder : u8 { leaves_first, keep };

// Merges DAGs built over *separate meshes* into the one set of buffers a scene draws from: the
// vertex, attribute, triangle, and quantized streams are concatenated with the offsets shifted,
// exactly as merge_cluster_lod does, with two differences that matter for a scene.
//
//   - Each mesh keeps its **own 16-bit position grid**. `quantized` is concatenated untouched
//     (three u16 per vertex at the merged vertex index, the whole stream padded to an even
//     count), and `parts_out[m]` reports the origin and step to decode a vertex of mesh m with.
//     Quantizing the scene on one grid instead would spend the 16 bits on the distance between
//     meshes rather than on each mesh's own detail.
//   - Every mesh's clusters stay **contiguous**, so `{first_cluster, cluster_count}` names a
//     mesh's clusters and an instance reaches cluster `first_cluster + local`. Within a mesh the
//     level-0 clusters still come first, so a direct draw of the leaves is the first
//     `leaf_cluster_count` of them.
//
// `out.quant_origin`/`quant_scale` are the first mesh's, so a single-mesh merge behaves as
// before; every other mesh's grid is in `parts_out`, and `dequantize_position` on the merged
// mesh is therefore only correct for the first. Fails on an empty list or an empty part.
//
// `order` is `ClusterOrder::keep` when the meshes have been laid out in pages; see the enum, and
// `merge_paged_cluster_meshes` in cluster_pages.h, which is what a caller with page tables uses.
bool merge_cluster_meshes(std::span<const ClusterLodMesh> parts, ClusterLodMesh& out,
                          Vector<ClusterMeshPart>& parts_out, std::string* error = nullptr,
                          ClusterOrder order = ClusterOrder::leaves_first);

// Level 0 covers every source triangle exactly once; every cluster's own error is at most its
// parent error; the raw cut is non-empty for every threshold; the triangle count of the raw
// cut never grows with the threshold. For a merged mesh, `source_indices` is the parts' index
// lists concatenated with each shifted by the source vertex counts of the parts before it.
bool validate_cluster_lod(const ClusterLodMesh& mesh, std::span<const u32> source_indices,
                          std::string* error = nullptr);

}  // namespace engine::geometry

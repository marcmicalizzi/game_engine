#pragma once

// **The collision's own copy of what its proxies are cut from** (T138; scene_collision.md, "The
// collision's own copy"). A proxy is a mesh's cluster DAG cut at `mesh_error_m` over its largest
// scale (scene_collision.h), and until T138 every cut read the scene's merged float positions,
// packed triangles and per-vertex source indices — about 2.1 GB on the forest's staging, kept for
// the whole session by any host with a walker. This holds only what a cut can select: for every
// mesh that can have a body, the clusters whose parent's error is above the smallest threshold a
// placement of it will ask for (`Mesh::floor`), and of those only the triangles that stay in a
// proxy — for a mesh with the motion contract, its wood (`renderer::flora_part_collides`) — with
// the positions and weld keys of the vertices they use. A host builds it **before** anything gives
// the scene's floats back (`renderer::release_uploaded_streams`, `attach_page_source`) and hands it
// to `SceneCollision::create`; a proxy cut from it is the proxy the scene's own floats give, word
// for word, whenever the threshold is at or above the mesh's floor.
//
// **The floor.** A cut selects a cluster when its own error is within the threshold and its
// parent's is not, so every cluster a threshold `t` selects has a parent's error above `t`, and
// keeping the clusters whose parent's error is above `floor` keeps every cut at `t >= floor`. The
// threshold is `mesh_error_m` over the placement's largest scale, so the floor is set by the
// largest scale a mesh will be placed at: the scene's own instances of it say theirs, and a
// streamed entry's tile, which the load cannot see, is allowed `k_copy_scale_headroom` times the
// mesh's fit. A placement larger than that is cut at the floor instead — coarser than
// `mesh_error_m` by the ratio — and counted (`Stats::proxies_coarsened`).

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <span>

namespace engine::geometry {
struct ClusterLodMesh;
struct ClusterMeshPart;
}  // namespace engine::geometry
namespace engine::renderer {
struct SceneData;
struct SourceMesh;
}  // namespace engine::renderer

namespace engine::scene_collision {

// How much larger than its fit a streamed entry may place a mesh and still have its proxy cut at
// exactly `mesh_error_m` (above, "The floor"): four times, past every scatter range and tree
// recipe in the tree, for a copy that holds the clusters two levels finer than the default cut.
inline constexpr f32 k_copy_scale_headroom = 4.0f;

class ProxySource {
 public:
  // One copied cluster: its DAG errors (a mesh with no DAG copies its leaves as `0` and the
  // terminal error, so every threshold selects them), its run of `points` and of `triangles`, and
  // how many of its triangles a tree's wood left out (the cut counts them as it selects it).
  struct Cluster {
    f32 own_error = 0.0f;
    f32 parent_error = 0.0f;
    u32 first_point = 0;
    u32 first_triangle = 0;
    u32 triangle_count = 0;
    u32 left_out = 0;
  };
  // One mesh's run of `clusters`, and the smallest threshold it cuts exactly. `copied` false: the
  // mesh gets no body (the terrain's, a water body's, a far forest tile's, one nothing places).
  struct Mesh {
    u32 first_cluster = 0;
    u32 cluster_count = 0;
    f32 floor = 0.0f;
    bool copied = false;
  };
  // What `build` measured, for its log line and the tests.
  struct Counts {
    u32 meshes = 0;    // copied
    u32 clusters = 0;  // copied, of `scene_clusters` in the copied meshes
    u64 scene_clusters = 0;
    u64 triangles = 0;  // copied, of `scene_triangles` in the copied meshes
    u64 scene_triangles = 0;
    u64 scene_float_bytes = 0;  // what the scene's float streams held when it was built
  };

  // Every mesh of `scene` that can have a body, at `mesh_error_m`, from the scene's float streams:
  // call it before they are released. A scene whose floats are gone already copies nothing and
  // says so in its log line; its placements then have no body.
  void build(const renderer::SceneData& scene, f32 mesh_error_m);
  // One mesh into this copy at its end, cut exactly at every threshold at or above `least` (its
  // `Mesh::floor`); returns its index. `tree`: a mesh with the motion contract, which keeps its
  // wood. A tile's own mesh (`SceneCollision::tile_proxy_for`) is copied this way, into scratch, at
  // the one threshold asked.
  u32 add(const geometry::ClusterLodMesh& lod, const geometry::ClusterMeshPart& part,
          const renderer::SourceMesh* tree, f32 least);
  void clear() noexcept;

  // The mesh at `index`, or null past the end or for one not copied.
  const Mesh* mesh(u32 index) const noexcept {
    return index < meshes_.size() && meshes_[index].copied ? &meshes_[index] : nullptr;
  }
  std::span<const Cluster> clusters(const Mesh& mesh) const noexcept {
    return {clusters_.data() + mesh.first_cluster, mesh.cluster_count};
  }
  // A copied vertex's position, in its mesh's frame, and its weld key (the scene's source index).
  std::span<const Vec3> points() const noexcept { return {points_.data(), points_.size()}; }
  std::span<const u32> sources() const noexcept { return {sources_.data(), sources_.size()}; }
  // A copied triangle: three 8-bit corners into its cluster's run of points, packed as the scene's
  // are; and the flora part of its first corner (0 for a mesh with no motion contract).
  std::span<const u32> triangles() const noexcept { return {triangles_.data(), triangles_.size()}; }
  std::span<const u16> parts() const noexcept { return {parts_.data(), parts_.size()}; }

  // What it holds on the host, by capacity.
  u64 bytes() const noexcept;
  const Counts& counts() const noexcept { return counts_; }

 private:
  Vector<Mesh> meshes_;
  Vector<Cluster> clusters_;
  Vector<Vec3> points_;
  Vector<u32> sources_;
  Vector<u32> triangles_;
  Vector<u16> parts_;
  Counts counts_;
};

}  // namespace engine::scene_collision

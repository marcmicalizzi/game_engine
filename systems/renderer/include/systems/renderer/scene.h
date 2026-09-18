#pragma once

// The scene, before the GPU (docs/subsystems/renderer.md). `SceneDesc` is what a caller asks
// for — mesh files and instances of them — and `SceneData` is what loading one produces: one
// merged cluster LOD DAG, one `geometry::ClusterMeshPart` per mesh, the materials and images
// each mesh named, the instance table in GPU shape, and the bounding sphere the camera frames.
//
// Loading is where the derived-data cache lives (AGENTS.md, [geometry](geometry.md)): a
// `.clusters` container named outright is read as it is, a glTF is looked up in
// `<ddc>/clusters/<key>.clusters` and built into it on a miss, and the container either app
// writes is the one both read. The cost of a load is the import and the clustering, which is
// why engine-host keeps a `SceneData` across calls and rebuilds only the GPU side when the
// settings change.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_lod.h>
#include <domain/gfx/cluster_cull.h>

#include <string>

namespace engine::renderer {

// One instance of a scene as a file, a grid, or a caller describes it.
struct SceneInstance {
  u32 mesh = 0;
  Transform3 transform;
};

// What to load. An empty mesh path is the procedural heightfield engine-view has always drawn,
// which is the only scene the engine can render with no content at all and therefore the one
// the renderer's own tests and a bring-up on a new machine start from.
struct SceneDesc {
  Vector<std::string> meshes;       // glTF, GLB, or .clusters; one empty path = the heightfield
  Vector<SceneInstance> instances;  // empty: one identity instance of every mesh
  u32 grid_instances = 0;           // n: n x n copies of mesh 0, varied in rotation and scale
  u32 heightfield_grid = 257;       // vertices a side, when a mesh path is empty
  std::string ddc;                  // derived-data root; empty and `cache` looks one up
  bool cache = true;                // read and write <ddc>/clusters/<key>.clusters
};

// Reads `{"meshes":[{"path":"..."}],"instances":[{"mesh":0,"translation":[x,y,z],
// "rotation":[x,y,z,w],"scale":[x,y,z]}]}`. Mesh paths resolve against the file's own
// directory, so a scene file is movable as a unit. Every field but the mesh list is optional;
// an empty instance list gives one identity instance of every mesh.
bool read_scene_file(const std::string& path, SceneDesc& out, std::string& error);

// One mesh of the scene as it arrived: the materials and images it named, and which primitive —
// and so which material — each of its clusters came from. The DAG itself has been moved into
// the scene's merged mesh by then.
struct SourceMesh {
  assets::MeshData data;        // materials and images; the positions are spent by now
  Vector<u32> part_of_cluster;  // the part index of every cluster, mesh-local
  Vector<i32> part_material;    // the material of every part, -1 for none
  std::string image_dir;        // what the relative image paths are relative to
  u32 primitives = 0;           // glTF primitives merged into it; 0 from a container
  const char* cache = "none";   // "file", "hit", "miss", or "none"
};

// A loaded scene: everything the GPU upload reads, and everything a summary reports.
struct SceneData {
  geometry::ClusterLodMesh lod;             // every mesh of the scene, merged
  Vector<geometry::ClusterMeshPart> parts;  // one per mesh: its clusters and its 16-bit grid
  Vector<SourceMesh> sources;               // parallel to `parts`
  // One per instance, in order of `first_pair`, with `world`, `mesh`, `first_pair`,
  // `scale_max`, and the uniform-scale flag filled in. `material_base` and `deform` are the GPU
  // scene's to fill, because both depend on tables it lays out.
  Vector<gfx::InstanceDesc> instances;
  Vec3 center{};             // the scene's bounding sphere: what the camera frames
  f32 radius = 10.0f;        // and what every distance, bias, and light reach scales with
  u32 pair_count = 0;        // the prefix sum the cull dispatch covers
  bool heightfield = false;  // the procedural scene: procedural materials and fixed bounds
  u32 mesh_primitives = 0;   // of the first mesh, as the summary reports it
  const char* mesh_cache = "none";
  i64 build_ns = 0;  // import, weld, cluster, page, and merge

  u32 cluster_count() const noexcept { return lod.mesh.clusters.size(); }
  u32 leaf_count() const noexcept {
    return lod.level_cluster_counts.empty() ? 0u : lod.level_cluster_counts[0];
  }
};

// Loads every mesh of `desc`, merges them into one set of buffers, expands the instances, and
// computes the bounds. `error` says which mesh failed and why.
bool load_scene(const SceneDesc& desc, SceneData& out, std::string& error);

// The bounding sphere of a range of a mesh's clusters, in its own space; tighter than the box
// diagonal, which matters for elongated meshes.
void mesh_bounds(const geometry::ClusterLodMesh& lod, u32 first, u32 count, Vec3& center,
                 f32& radius);

}  // namespace engine::renderer

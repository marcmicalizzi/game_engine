#pragma once

// The far forest's load-time half (docs/subsystems/renderer.md, "The far forest"): which streamed
// authored instances it takes, the per-tile meshes it merges them into, the derived-data entries
// those are kept in, and what it appends to the load. far_forest.h has what the load hands on.

#include <systems/renderer/far_forest.h>
#include <systems/renderer/scene.h>

#include <span>
#include <string>

namespace engine::jobs {
class JobSystem;
}

namespace engine::renderer {

// The entry's format and builder version: part of every far tile's cache key, bumped when a
// change makes an old entry wrong.
inline constexpr u32 k_far_forest_version = 1;
// The defaults of `renderer.far_forest.min_height_m` and `.max_triangles` (renderer.md, "What it
// takes"): an instance shorter than this draws no far form (the understory), and a mesh whose
// coarsest form is heavier than this many triangles an instance draws none either.
inline constexpr f32 k_far_min_height_m = 4.0f;
inline constexpr u32 k_far_max_triangles = 512;

// One streamed authored instance, as the load had it before handing it to the world.
struct FarForestInstance {
  u32 mesh = 0;
  Transform3 transform;
  WorldPos origin{};
};

// A mesh's fit as the load computes it (`fit_parts`): the uniform scale and the point that goes to
// the origin. Identity for a mesh with no fit.
struct FarForestFit {
  f32 scale = 1.0f;
  Vec3 anchor{};
};

struct FarForestOptions {
  f32 tile_size = 64.0f;  // the world's tile, metres
  f32 min_height_m = k_far_min_height_m;
  u32 max_triangles = k_far_max_triangles;
  std::string ddc;  // the derived-data root the entries are read from and written to
  bool cache = false;
  jobs::JobSystem* pool = nullptr;  // null: every tile on the calling thread
};
// The options' two numbers from their tunables.
void far_forest_tunables(FarForestOptions& out) noexcept;

// What the load appends to: the far meshes after every mesh it has (a path, a description, a
// source and a DAG each) and their instances after its prefix.
struct FarForestScene {
  Vector<std::string>& meshes;
  Vector<SceneMeshInfo>& mesh_info;
  Vector<SourceMesh>& sources;
  Vector<geometry::ClusterLodMesh>& dags;
  Vector<u32>& form_source;  // grown with the meshes, each its own source; may be empty
  Vector<u8>& form_rigid;    // likewise, 1 (rigid)
  Vector<SceneInstance>& prefix;
};

// Builds the far forest over `instances` (meshes indexing `fits`, `scene.sources` and
// `scene.dags`), appending one mesh and one prefix instance per world tile that has a far tree,
// and fills `out`. The tile meshes are the same bytes whatever `options.pool` (each is built by
// one job, in a fixed order). False, with a sentence, only for a builder failure; an instance the
// rules leave out is counted, not refused.
bool build_far_forest(std::span<const FarForestInstance> instances,
                      std::span<const FarForestFit> fits, const FarForestOptions& options,
                      FarForestScene scene, FarForestData& out, std::string& error);

// A far tile's mesh as its derived-data entry holds it (after a header): the cluster DAG's streams
// and the part of every cluster. What the determinism test compares.
void far_tile_bytes(const geometry::ClusterLodMesh& mesh, std::span<const u32> part_of_cluster,
                    std::string& out);

}  // namespace engine::renderer

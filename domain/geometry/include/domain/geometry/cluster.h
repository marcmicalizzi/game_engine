#pragma once

// The cluster geometry format, runtime side (docs/plan/04-renderer.md §4.3, ADR-0005). A mesh
// is a list of clusters, each a small set of vertices (at most 64 today) and local-index
// triangles (at most 124), with a bounding sphere. Mesh shaders draw one cluster per workgroup,
// the software rasterizer takes clusters below a projected size, and acceleration structures
// are built from the same clusters (Phase 2). The layout below is what the GPU reads through
// device addresses, so it is fixed and pinned by the size table.
//
// v0 stores positions as three floats per vertex in cluster order. Per-cluster 16-bit
// quantization, packed normals and UVs, normal cones for backface culling, the LOD DAG with
// error bounds (meshoptimizer clusterlod), and fixed-size pages for streaming follow.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <span>
#include <string>

namespace engine::geometry {

// GPU-mirrored; keep in step with the ClusterDesc struct in shaders.
struct ClusterDesc {
  u32 vertex_offset = 0;    // first vertex in ClusterMesh::vertices
  u32 triangle_offset = 0;  // first packed triangle in ClusterMesh::triangles
  u32 vertex_count = 0;
  u32 triangle_count = 0;
  Vec3 center{};  // bounding sphere
  f32 radius = 0.0f;
};

struct ClusterBuildOptions {
  u32 max_vertices = 64;    // at most 255 (local indices are bytes)
  u32 max_triangles = 124;  // at most 512 and a multiple of 4 (meshoptimizer)
  // 0 optimizes for reuse and locality; up to 1 trades that for tighter normal cones.
  f32 cone_weight = 0.0f;
};

struct ClusterMesh {
  Vector<ClusterDesc> clusters;
  Vector<Vec3> vertices;      // cluster-ordered copies of source positions
  Vector<u32> vertex_source;  // source vertex index per entry of `vertices`
  Vector<u32> triangles;      // per triangle: local i0 | i1 << 8 | i2 << 16
  u32 source_vertex_count = 0;
  u32 source_triangle_count = 0;

  static constexpr u32 unpack(u32 packed, u32 corner) noexcept {
    return (packed >> (8 * corner)) & 0xff;
  }
  static constexpr u32 pack(u32 i0, u32 i1, u32 i2) noexcept { return i0 | (i1 << 8) | (i2 << 16); }
};

// Splits an indexed triangle mesh into clusters. `indices` holds three entries per triangle.
// Fails (with `error`) on empty input, a non-multiple-of-three index count, an index out of
// range, or options outside the limits above.
bool build_clusters(std::span<const Vec3> positions, std::span<const u32> indices,
                    const ClusterBuildOptions& options, ClusterMesh& out,
                    std::string* error = nullptr);

// Checks the invariants tests rely on: offsets and counts in range, counts within the limits,
// every source triangle present exactly once, every vertex inside its cluster's sphere.
bool validate_clusters(const ClusterMesh& mesh, std::span<const u32> source_indices,
                       const ClusterBuildOptions& options, std::string* error = nullptr);

}  // namespace engine::geometry

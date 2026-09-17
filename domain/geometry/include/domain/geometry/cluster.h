#pragma once

// The cluster geometry format, runtime side (docs/plan/04-renderer.md §4.3, ADR-0005). A mesh
// is a list of clusters, each a small set of vertices (at most 64 today) and local-index
// triangles (at most 124), with a bounding sphere. Mesh shaders draw one cluster per workgroup,
// the software rasterizer takes clusters below a projected size, and acceleration structures
// are built from the same clusters (Phase 2). The layout below is what the GPU reads through
// device addresses, so it is fixed and pinned by the size table.
//
// v1 stores positions twice: as three floats per vertex in cluster order (what the acceleration
// structure builders read) and as three u16 on one mesh-wide grid (what the rasterizers and the
// resolve read, six bytes a vertex instead of twelve), plus packed normals and UVs per vertex
// and a normal cone per cluster for backface culling; the LOD DAG with error bounds is
// cluster_lod.h. Fixed-size pages for streaming follow.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <span>
#include <string>

namespace engine::geometry {

// A packed normal cone that never culls: zero axis, cutoff 1 (127/127).
inline constexpr u32 k_cone_none = 0x7f000000u;

// GPU-mirrored; keep in step with the ClusterDesc struct in shaders. 48 bytes.
struct ClusterDesc {
  u32 vertex_offset = 0;    // first vertex in ClusterMesh::vertices
  u32 triangle_offset = 0;  // first packed triangle in ClusterMesh::triangles
  u32 vertex_count = 0;
  u32 triangle_count = 0;
  Vec3 center{};  // bounding sphere
  f32 radius = 0.0f;
  // Normal cone for backface culling (meshoptimizer's form): the cluster is entirely
  // backfacing when dot(normalize(cone_apex - camera), axis) >= cutoff. `cone` packs the axis
  // as three snorm8 (bytes 0..2, x/127) and the cutoff as a snorm8 (byte 3); a cutoff of 1
  // (k_cone_none) marks a cluster that is never culled: two-sided, or normals too spread.
  Vec3 cone_apex{};
  u32 cone = k_cone_none;
};

struct NormalCone {
  Vec3 axis{};        // decoded as stored: within a snorm8 step of unit length
  f32 cutoff = 1.0f;  // sin of the half-angle of the normal spread; 1 never culls
};

// Snorm8 packing of a cone; the cutoff rounds up so quantization never culls more.
u32 encode_cone(Vec3 axis, f32 cutoff) noexcept;
NormalCone decode_cone(u32 packed) noexcept;
// The CPU reference of the cull pass's cone test, on the packed values it reads.
bool cluster_backfacing(const ClusterDesc& cluster, Vec3 camera) noexcept;

// GPU-mirrored per-vertex attributes, 8 bytes, cluster-ordered like ClusterMesh::vertices: an
// octahedral normal in two snorm16 and a UV in two half floats. Keep in step with the shaders.
struct VertexAttributes {
  u32 normal_oct = 0;
  u32 uv_half2 = 0;
};

// Optional source attributes for the builders, indexed like the source positions. Empty normals
// are computed from the faces (area-weighted, smooth); empty UVs are zero.
struct AttributeSource {
  std::span<const Vec3> normals;
  std::span<const Vec2> uvs;
};

u32 encode_normal_oct(Vec3 normal) noexcept;
Vec3 decode_normal_oct(u32 packed) noexcept;
u32 encode_half2(Vec2 v) noexcept;
Vec2 decode_half2(u32 packed) noexcept;
// Area-weighted smooth normals; isolated vertices get +Y.
void compute_vertex_normals(std::span<const Vec3> positions, std::span<const u32> indices,
                            Vector<Vec3>& out);

// Merges vertices whose position, normal, and UV are bit-identical and drops unreferenced
// ones, rewriting `indices` in place and compacting the streams (`normals` and `uvs` take part
// when they are parallel to `positions`, and are left alone otherwise). Exporters often write
// unindexed or seam-split meshes; without welding, the 64-vertex cluster limit caps clusters at
// 21 triangles and the LOD builder has no connectivity to simplify across. Vertices that
// differ in any attribute stay distinct, so seams keep their normals and UVs. Returns the new
// vertex count.
u32 weld_vertices(Vector<Vec3>& positions, Vector<Vec3>& normals, Vector<Vec2>& uvs,
                  std::span<u32> indices);

struct ClusterBuildOptions {
  u32 max_vertices = 64;    // at most 255 (local indices are bytes)
  u32 max_triangles = 124;  // at most 512 and a multiple of 4 (meshoptimizer)
  // 0 optimizes for reuse and locality; up to 1 trades that for tighter normal cones.
  f32 cone_weight = 0.0f;
  // False stores k_cone_none on every cluster, for two-sided meshes that must never be
  // backface culled.
  bool normal_cones = true;
};

struct ClusterMesh {
  Vector<ClusterDesc> clusters;
  Vector<Vec3> vertices;                // cluster-ordered copies of source positions
  Vector<u32> vertex_source;            // source vertex index per entry of `vertices`
  Vector<VertexAttributes> attributes;  // parallel to `vertices`
  Vector<u32> triangles;                // per triangle: local i0 | i1 << 8 | i2 << 16
  // Positions on the mesh-wide 16-bit grid: three u16 per vertex, cluster-ordered like
  // `vertices`, padded with one zero to an even count so a shader may read the last triple as
  // two 32-bit words. `quantize_positions` fills all three fields; a mesh with no vertices keeps
  // the identity grid.
  Vector<u16> quantized;
  Vec3 quant_origin{};     // the AABB minimum of `vertices`
  f32 quant_scale = 1.0f;  // the largest AABB extent / 65535; 1 for a degenerate mesh
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
                    std::string* error = nullptr, const AttributeSource& attributes = {});

// Fills `mesh.attributes` from the source attributes through `vertex_source`; the builders call
// it, and it is public so a mesh built elsewhere can be given attributes later.
void fill_cluster_attributes(ClusterMesh& mesh, std::span<const Vec3> positions,
                             std::span<const u32> indices, const AttributeSource& attributes);

// Fills `quantized`, `quant_origin`, and `quant_scale` from `vertices`: one grid over the whole
// mesh, origin at the AABB minimum, step = the largest AABB extent / 65535 (1 when the mesh is a
// point), and q = round((p - origin) / step) clamped to 0..65535 per axis. Every builder calls
// it last, so a merged mesh gets one grid over all of its parts. Because every cluster's copy of
// a shared vertex quantizes the same source position on the same grid, the copies land on the
// same integer triple and welded meshes stay crack-free: quantization moves a seam's two copies
// by exactly the same amount. Public so a mesh built elsewhere can be quantized later; call it
// again after changing `vertices`.
void quantize_positions(ClusterMesh& mesh);
// The CPU reference of the shaders' load_position: the grid point of `vertex` as a float
// position. Out-of-range vertices read as the origin.
Vec3 dequantize_position(const ClusterMesh& mesh, u32 vertex) noexcept;

// Checks the invariants tests rely on: offsets and counts in range, counts within the limits,
// every source triangle present exactly once, every vertex inside its cluster's sphere, every
// triangle normal inside its cluster's normal cone (when the cone is not k_cone_none), and a
// quantized position stream that is present, padded to an even count, and within half a grid
// step of every float position.
bool validate_clusters(const ClusterMesh& mesh, std::span<const u32> source_indices,
                       const ClusterBuildOptions& options, std::string* error = nullptr);

}  // namespace engine::geometry

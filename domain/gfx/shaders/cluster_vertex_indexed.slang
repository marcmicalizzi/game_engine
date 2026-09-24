// The baseline tier's culled draw (docs/subsystems/gfx.md, "Baseline tier", plan 04 §4.3): one
// hardware run of the visible list as **one indexed draw of exactly the triangles its clusters
// hold**, from the region the cull pass allocated and vertex_expand.slang filled
// (gfx::VertexDrawHeader). An index is `slot << 8 | local vertex`, so the vertex shader gets back
// to the entry and the cluster vertex from the index alone, and the post-transform cache runs a
// cluster's shared vertices once rather than once per triangle corner.
//
// Why indexed and not three vertices a triangle: on the TITAN Xp the vertex path's raster pass is
// the vertex stage and nothing else — killing every triangle after the vertex shader, or dropping
// the fragment atomics, changes it by 1% — and it costs about 5 ns per 32-lane warp of real
// vertices. Drawing a cluster's capacity (cluster_vertex.slang's vs_cluster) runs three lanes per
// triangle corner and collapses the ones past the cluster's count, and those collapsed warps turn
// out to be nearly free; the three-per-corner is what is not. Drawing only the real triangles,
// still a lane per corner, moved the pass by -23% to +7%; indexing, which shares the vertices,
// took 42 to 48% off it on the helmet grid, the expansion included, and put it under the software
// rasterizer's everywhere measured (docs/experiments/e1-pascal-rerun.md, "After").
//
// The fragment stage names the triangle with SV_PrimitiveID, which counts the draw's triangles, so
// the vertex stage hands it `(pair << 8) - the cluster's first triangle in the draw` as a flat
// value and the sum is the visibility id — the same word the mesh path, the capacity draw and the
// software rasterizer write (the scene's pair, scene.slang's `pair_of`; the arithmetic is modulo
// 2^32 and the sum is exact). SV_PrimitiveID in a fragment shader of a vertex pipeline is the
// SPIR-V Geometry capability, so this module needs the device's geometryShader feature, which is
// why it is a module of its own: without the feature the renderer draws the capacity draw from
// cluster_vertex.slang, which declares nothing of the kind. The renderer also wants
// fullDrawIndexUint32 for it, because from a run's 65,537th cluster `slot << 8` passes 2^24 - 1.
//
// The fallback: a survivor whose triangles did not fit the region's index array is flagged in its
// record and drawn here by vs_cluster_fallback, the capacity draw instanced over the slots up to
// the highest one that overflowed, with every entry that did fit collapsed. Its fragment stage is
// cluster_vertex.slang's fs_visibility, whose input is the same flat id at location 0.

#include "scene.slang"

// Same layout as gfx::ClusterDrawParams (128 bytes). `visible` is the run's region's records,
// {instance, cluster, first triangle, overflowed} per slot, not the visible list: everything the
// draw needs of an entry is in its record, which saves the vertex stage one dependent load.
struct VertexParams {
  column_major float4x4 view_proj;
  uint64_t clusters;
  uint64_t mesh;  // MeshDesc[]: the quantized positions and each mesh's grid
  uint64_t triangles;
  uint visible_offset;         // this run's first entry in the whole scene's visible list
  uint triangles_per_cluster;  // the fallback's capacity: its vertex count / 3
  uint64_t records;            // uint4[] per slot of the run's region
  uint64_t visibility;         // uint64_t[width * height]
  uint width;
  uint height;
  uint64_t instances;  // InstanceDesc[]
};

[[vk::push_constant]] ConstantBuffer<VertexParams> g_params;

struct IndexedVertex {
  float4 position : SV_Position;
  // (pair << 8) - the cluster's first triangle in the draw: add SV_PrimitiveID for the id
  nointerpolation uint id_base : VISIBILITY_ID;
};

// The position of `local` of the entry's cluster — the grid or this frame's deformed-vertex pool —
// and the scene's pair the entry names, which is what the id carries.
float4 clip_position(uint4 record, uint visible_index, uint local, out uint pair) {
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const InstanceDesc instance = instances[record.x];
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[record.y];
  MeshDesc* meshes = (MeshDesc*)g_params.mesh;
  const MeshDesc mesh = meshes[instance.mesh];
  pair = pair_of(instance, mesh, record.y);
  const uint base = deform_base(mesh, instance, visible_index);
  const float3 p = load_position(mesh, base, cluster.vertex_offset, local);
  const float3 world = mul(instance.world, float4(p, 1.0)).xyz;
  return mul(g_params.view_proj, float4(world, 1.0));
}

[shader("vertex")]
IndexedVertex vs_cluster_indexed(uint index : SV_VulkanVertexID) {
  const uint slot = index >> 8;
  uint4* records = (uint4*)g_params.records;
  const uint4 record = records[slot];
  const uint visible_index = g_params.visible_offset + slot;
  IndexedVertex out;
  uint pair;
  out.position = clip_position(record, visible_index, index & 0xff, pair);
  out.id_base = (pair << 8) - record.z;
  return out;
}

// cluster_mesh.slang's fs_visibility with the triangle taken from SV_PrimitiveID, which is what
// fills the same buffer the same way.
[shader("fragment")]
void fs_visibility_indexed(IndexedVertex v, uint primitive_id : SV_PrimitiveID) {
  const uint2 pixel = uint2(v.position.xy);
  if (pixel.x >= g_params.width || pixel.y >= g_params.height) return;
  const uint id = v.id_base + primitive_id;
  const uint64_t value = (uint64_t(asuint(v.position.z)) << 32) | uint64_t(id);
  Atomic<uint64_t>* visibility = (Atomic<uint64_t>*)g_params.visibility;
  visibility[pixel.y * g_params.width + pixel.x].max(value);
}

// Matches cluster_vertex.slang's ClusterVertex, so its fs_visibility is this stage's fragment
// stage.
struct FallbackVertex {
  float4 position : SV_Position;
  nointerpolation uint id : VISIBILITY_ID;  // pair << 8 | triangle
};

[shader("vertex")]
FallbackVertex vs_cluster_fallback(uint vertex_id : SV_VulkanVertexID,
                                   uint slot : SV_VulkanInstanceID) {
  uint4* records = (uint4*)g_params.records;
  const uint4 record = records[slot];
  const uint visible_index = g_params.visible_offset + slot;
  const uint triangle = vertex_id / 3;
  const uint corner = vertex_id - triangle * 3;
  FallbackVertex out;
  out.id = 0;                                  // a degenerate triangle is never rasterized
  out.position = float4(0.0, 0.0, 0.0, 0.0);  // degenerate unless the entry overflowed
  if (record.w == 0) return out;              // the indexed draw drew it
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[record.y];
  if (triangle >= cluster.triangle_count) return out;
  uint* triangles = (uint*)g_params.triangles;
  const uint packed = triangles[cluster.triangle_offset + triangle];
  uint pair;
  out.position = clip_position(record, visible_index, (packed >> (8 * corner)) & 0xff, pair);
  out.id = (pair << 8) | (triangle & 0xff);
  return out;
}

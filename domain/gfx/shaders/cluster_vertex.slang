// The baseline tier's cluster rasterizer (docs/plan/02-architecture.md §2.7, plan 04 §4.3): the
// same scene, visibility buffer, and cull pass as the mesh-shader path, drawn through a plain
// vertex shader for GPUs without mesh shaders (Maxwell, Pascal, integrated parts). One instance
// of the draw per entry of the visible list and a fixed vertex count of three times the cluster
// triangle capacity: the vertex shader maps SV_VertexID to a local triangle and corner, pulls
// the position through device addresses as the mesh shader does, moves it into the scene
// instance's frame, and collapses the vertices of triangles past the cluster's count so they
// never rasterize. Driven by vkCmdDrawIndirect with {vertex_count, instance_count = survivors,
// 0, 0}, where the cull pass counts into the second word (CullParams::count_index = 1).
// SV_InstanceID is therefore the **visible index**, which is what the visibility id carries.

#include "scene.slang"

// Same layout as MeshParams in cluster_mesh.slang (128 bytes); `triangles_per_cluster` is the
// mesh path's unused pad.
struct VertexParams {
  column_major float4x4 view_proj;
  uint64_t clusters;
  uint64_t mesh;  // MeshDesc[]: the quantized positions and each mesh's grid
  uint64_t triangles;
  uint visible_offset;  // this draw's first entry in the whole scene's visible list
  uint triangles_per_cluster;  // triangle capacity per cluster: vertex_count / 3 of the draw
  uint64_t visible;            // uint2[]: {instance, cluster} per entry; 0 draws {0, i} in order
  uint64_t visibility;         // uint64_t[width * height]
  uint width;
  uint height;
  uint64_t instances;  // InstanceDesc[]
};

[[vk::push_constant]] ConstantBuffer<VertexParams> g_params;

struct ClusterVertex {
  float4 position : SV_Position;
  nointerpolation uint id : VISIBILITY_ID;  // visible index << 8 | triangle
};

[shader("vertex")]
ClusterVertex vs_cluster(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
  const uint visible_index = g_params.visible_offset + instance_id;
  const uint2 entry = visible_entry(g_params.visible, instance_id);
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const InstanceDesc instance = instances[entry.x];
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[entry.y];
  const uint triangle = vertex_id / 3;
  const uint corner = vertex_id - triangle * 3;
  ClusterVertex out;
  out.id = (visible_index << 8) | (triangle & 0xff);
  if (triangle >= cluster.triangle_count) {
    out.position = float4(0.0, 0.0, 0.0, 0.0);  // degenerate: all three corners coincide
    return out;
  }
  uint* triangles = (uint*)g_params.triangles;
  const uint packed = triangles[cluster.triangle_offset + triangle];
  const uint local = (packed >> (8 * corner)) & 0xff;
  MeshDesc* meshes = (MeshDesc*)g_params.mesh;
  const float3 p = load_position(meshes[instance.mesh], cluster.vertex_offset + local);
  const float3 world = mul(instance.world, float4(p, 1.0)).xyz;
  out.position = mul(g_params.view_proj, float4(world, 1.0));
  return out;
}

// Identical to cluster_mesh.slang's fs_visibility so both paths fill the same buffer.
[shader("fragment")]
void fs_visibility(ClusterVertex v) {
  const uint2 pixel = uint2(v.position.xy);
  if (pixel.x >= g_params.width || pixel.y >= g_params.height) return;
  const uint64_t value = (uint64_t(asuint(v.position.z)) << 32) | uint64_t(v.id);
  Atomic<uint64_t>* visibility = (Atomic<uint64_t>*)g_params.visibility;
  visibility[pixel.y * g_params.width + pixel.x].max(value);
}

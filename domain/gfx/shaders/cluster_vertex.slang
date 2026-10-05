// The baseline tier's cluster rasterizer (docs/plan/02-architecture.md §2.7, plan 04 §4.3): the
// same scene, visibility buffer, and cull pass as the mesh-shader path, drawn through a plain
// vertex shader for GPUs without mesh shaders (Maxwell, Pascal, integrated parts). One instance
// of the draw per entry of the visible list and a fixed vertex count of three times the cluster
// triangle capacity: the vertex shader maps SV_VertexID to a local triangle and corner, pulls
// the position through device addresses as the mesh shader does, moves it into the scene
// instance's frame, and collapses the vertices of triangles past the cluster's count so they
// never rasterize. Driven by vkCmdDrawIndirect with {vertex_count, instance_count = survivors,
// 0, 0}, where the cull pass counts into the second word (CullParams::count_index = 1).
// SV_InstanceID is therefore the **visible index**, which leads to the entry's (instance, cluster)
// and so to the scene's pair, which is what the visibility id carries (scene.slang, `pair_of`).

#include "scene.slang"

// Same layout as MeshParams in cluster_mesh.slang (128 bytes).
struct VertexParams {
  column_major float4x4 view_proj;
  uint64_t clusters;
  uint64_t mesh;  // MeshDesc[]: the quantized positions and each mesh's grid
  uint64_t triangles;
  uint visible_offset;  // this draw's first entry in the whole scene's visible list
  uint extent;          // the visibility buffer's width | height << 16 (gfx::draw_extent)
  uint64_t visible;            // uint2[]: {instance, cluster} per entry; 0 draws {0, i} in order
  uint64_t visibility;         // uint64_t[width * height]
  uint64_t eye;        // FrameEye*: the frame's origin; 0 is the world's
  uint64_t instances;  // InstanceDesc[]
};

[[vk::push_constant]] ConstantBuffer<VertexParams> g_params;

struct ClusterVertex {
  float4 position : SV_Position;
  nointerpolation uint id : VISIBILITY_ID;  // pair << 8 | triangle
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
  if (triangle >= cluster.triangle_count) {
    out.id = 0;  // never rasterized
    out.position = float4(0.0, 0.0, 0.0, 0.0);  // degenerate: all three corners coincide
    return out;
  }
  MeshDesc* meshes = (MeshDesc*)g_params.mesh;
  const MeshDesc mesh = meshes[instance.mesh];
  out.id = (pair_of(instance, mesh, entry.y) << 8) | (triangle & 0xff);
  uint* triangles = (uint*)g_params.triangles;
  const uint packed = triangles[cluster.triangle_offset + triangle];
  const uint local = (packed >> (8 * corner)) & 0xff;
  const uint base = deform_base(mesh, instance, visible_index);
  const float3 p = load_position(mesh, base, cluster.vertex_offset, local);
  const float3 from_eye = instance_from_eye(instance, load_eye(g_params.eye));
  const float3 q = instance_point(instance, from_eye, p);
  out.position = mul(g_params.view_proj, float4(q, 1.0));
  return out;
}

// Identical to cluster_mesh.slang's fs_visibility so both paths fill the same buffer.
[shader("fragment")]
void fs_visibility(ClusterVertex v) {
  const uint2 pixel = uint2(v.position.xy);
  if (pixel.x >= extent_width(g_params.extent) || pixel.y >= extent_height(g_params.extent)) return;
  const uint64_t value = (uint64_t(asuint(v.position.z)) << 32) | uint64_t(v.id);
  Atomic<uint64_t>* visibility = (Atomic<uint64_t>*)g_params.visibility;
  visibility[pixel.y * extent_width(g_params.extent) + pixel.x].max(value);
}

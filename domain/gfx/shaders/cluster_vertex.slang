// The baseline tier's cluster rasterizer (docs/plan/02-architecture.md §2.7, plan 04 §4.3): the
// same clusters, visibility buffer, and cull pass as the mesh-shader path, drawn through a
// plain vertex shader for GPUs without mesh shaders (Maxwell, Pascal, integrated parts). One
// instance per visible cluster and a fixed vertex count of three times the cluster triangle
// capacity: the vertex shader maps SV_VertexID to a local triangle and corner, pulls the
// position through device addresses like the mesh shader does, and collapses the vertices of
// triangles past the cluster's count so they never rasterize. Driven by vkCmdDrawIndirect with
// {vertex_count, instance_count = survivors, 0, 0}, where the cull pass counts into the second
// word (CullParams::count_index = 1).

struct ClusterDesc {
  uint vertex_offset;
  uint triangle_offset;
  uint vertex_count;
  uint triangle_count;
  float3 center;
  float radius;
  float3 cone_apex;  // normal cone for backface culling (cull pass only)
  uint cone;         // axis snorm8 x, y, z in bytes 0..2, cutoff snorm8 in byte 3
};

struct Position {
  float x;
  float y;
  float z;
};

// Same layout as MeshParams in cluster_mesh.slang (120 bytes); `triangles_per_cluster` is the
// mesh path's unused pad.
struct VertexParams {
  column_major float4x4 view_proj;
  uint64_t clusters;
  uint64_t vertices;
  uint64_t triangles;
  uint cluster_count;
  uint triangles_per_cluster;  // triangle capacity per cluster: vertex_count / 3 of the draw
  uint64_t visible;            // uint[]: cluster index per instance; 0 draws clusters in order
  uint64_t visibility;         // uint64_t[width * height]
  uint width;
  uint height;
};

[[vk::push_constant]] ConstantBuffer<VertexParams> g_params;

struct ClusterVertex {
  float4 position : SV_Position;
  nointerpolation uint id : VISIBILITY_ID;  // cluster << 8 | triangle
};

[shader("vertex")]
ClusterVertex vs_cluster(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
  uint cluster_id = instance_id;
  if (g_params.visible != 0) {
    uint* visible = (uint*)g_params.visible;
    cluster_id = visible[instance_id];
  }
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[cluster_id];
  const uint triangle = vertex_id / 3;
  const uint corner = vertex_id - triangle * 3;
  ClusterVertex out;
  out.id = (cluster_id << 8) | (triangle & 0xff);
  if (triangle >= cluster.triangle_count) {
    out.position = float4(0.0, 0.0, 0.0, 0.0);  // degenerate: all three corners coincide
    return out;
  }
  uint* triangles = (uint*)g_params.triangles;
  const uint packed = triangles[cluster.triangle_offset + triangle];
  const uint local = (packed >> (8 * corner)) & 0xff;
  Position* positions = (Position*)g_params.vertices;
  const Position p = positions[cluster.vertex_offset + local];
  out.position = mul(g_params.view_proj, float4(p.x, p.y, p.z, 1.0));
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

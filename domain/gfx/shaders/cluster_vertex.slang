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

// Mirrors gfx::MeshDesc (32 bytes): the mesh-wide 16-bit position grid and the stream of three
// u16 per vertex on it. Duplicated in every shader that reads positions, like ClusterDesc.
struct MeshDesc {
  float4 quant;        // xyz grid origin, w grid step
  uint64_t quantized;  // uint16_t[3 * vertex_count], padded to an even count
  uint64_t pad;
};

// Three u16 at 3 * vertex, read as the 32-bit words that contain them. The stream is padded to
// an even u16 count on the CPU, so the last word read is always inside the buffer.
float3 load_position(MeshDesc* mesh, uint vertex) {
  uint* words = (uint*)mesh->quantized;
  const uint i = vertex * 3;
  const uint w0 = words[i >> 1];
  const uint w1 = words[(i + 1) >> 1];
  const uint w2 = words[(i + 2) >> 1];
  const uint x = (i & 1) != 0 ? (w0 >> 16) : (w0 & 0xffff);
  const uint y = ((i + 1) & 1) != 0 ? (w1 >> 16) : (w1 & 0xffff);
  const uint z = ((i + 2) & 1) != 0 ? (w2 >> 16) : (w2 & 0xffff);
  return mesh->quant.xyz + float3(float(x), float(y), float(z)) * mesh->quant.w;
}

// Same layout as MeshParams in cluster_mesh.slang (120 bytes); `triangles_per_cluster` is the
// mesh path's unused pad.
struct VertexParams {
  column_major float4x4 view_proj;
  uint64_t clusters;
  uint64_t mesh;  // MeshDesc: the quantized positions and their grid
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
  MeshDesc* mesh = (MeshDesc*)g_params.mesh;
  const float3 p = load_position(mesh, cluster.vertex_offset + local);
  out.position = mul(g_params.view_proj, float4(p, 1.0));
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

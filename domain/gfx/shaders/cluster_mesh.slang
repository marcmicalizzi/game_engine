// Mesh-shader cluster rasterization (docs/plan/04-renderer.md §4.3): one workgroup per cluster,
// geometry pulled through device addresses, and a visibility ID written per pixel:
// cluster index in the high 24 bits, triangle index within the cluster in the low 8.
// Layout of ClusterDesc mirrors domain/geometry/cluster.h (32 bytes).

struct ClusterDesc {
  uint vertex_offset;
  uint triangle_offset;
  uint vertex_count;
  uint triangle_count;
  float3 center;
  float radius;
};

struct Position {
  float x;
  float y;
  float z;
};

struct MeshParams {
  column_major float4x4 view_proj;
  uint64_t clusters;
  uint64_t vertices;
  uint64_t triangles;
  uint cluster_count;
};

[[vk::push_constant]] ConstantBuffer<MeshParams> g_params;

struct MeshVertex {
  float4 position : SV_Position;
  nointerpolation uint cluster_id : CLUSTER_ID;
};

// The fragment stage only sees a defined SV_PrimitiveID when the mesh stage writes one.
struct MeshPrimitive {
  uint primitive_id : SV_PrimitiveID;
};

static const uint k_max_vertices = 64;
static const uint k_max_triangles = 124;

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void mesh_main(uint3 group_thread : SV_GroupThreadID, uint3 group : SV_GroupID,
               out vertices MeshVertex verts[k_max_vertices],
               out indices uint3 tris[k_max_triangles],
               out primitives MeshPrimitive prims[k_max_triangles]) {
  const uint cluster_id = group.x;
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[cluster_id];
  SetMeshOutputCounts(cluster.vertex_count, cluster.triangle_count);

  const uint t = group_thread.x;
  if (t < cluster.vertex_count) {
    Position* positions = (Position*)g_params.vertices;
    const Position p = positions[cluster.vertex_offset + t];
    verts[t].position = mul(g_params.view_proj, float4(p.x, p.y, p.z, 1.0));
    verts[t].cluster_id = cluster_id;
  }
  if (t < cluster.triangle_count) {
    uint* triangles = (uint*)g_params.triangles;
    const uint packed = triangles[cluster.triangle_offset + t];
    tris[t] = uint3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
    prims[t].primitive_id = t;
  }
}

[shader("fragment")]
uint fs_main(MeshVertex v, uint primitive_id : SV_PrimitiveID) : SV_Target {
  return (v.cluster_id << 8) | (primitive_id & 0xff);
}

// Debug view for windows and captures: one color per cluster, shaded a little per triangle.
[shader("fragment")]
float4 fs_color(MeshVertex v, uint primitive_id : SV_PrimitiveID) : SV_Target {
  const uint h = v.cluster_id * 2654435761u;
  const float3 base = float3((h >> 0) & 255, (h >> 8) & 255, (h >> 16) & 255) / 255.0;
  const float shade = 0.7 + 0.3 * frac(float(primitive_id) * 0.6180339887);
  return float4(base * shade, 1.0);
}

// Mesh-shader cluster rasterization (docs/plan/04-renderer.md §4.3): one workgroup per entry of
// the cull pass's visible list, geometry pulled through device addresses, and a visibility ID
// written per pixel: the scene's **pair** in the high 24 bits, the triangle within the cluster in
// the low 8 (scene.slang, `pair_of`: the id is the scene's and not the list's, so a depth tie goes
// to the same surface whatever order the frame drew in). The entry names the (instance, cluster)
// pair, so positions come off the 16-bit grid of that instance's mesh (six bytes a vertex) and go
// through the instance's world matrix before the view projection. scene.slang holds the structs the CPU pins with size tables.

#include "scene.slang"

struct MeshParams {
  column_major float4x4 view_proj;
  uint64_t clusters;
  uint64_t mesh;  // MeshDesc[]: the quantized positions and each mesh's grid
  uint64_t triangles;
  uint visible_offset;  // this draw's first entry in the whole scene's visible list
  uint pad;
  uint64_t visible;     // uint2[]: {instance, cluster} per entry; 0 draws {0, i} in index order
  uint64_t visibility;  // uint64_t[width * height]: depth << 32 | id, written with atomic max
  uint width;           // visibility buffer size in pixels
  uint height;
  uint64_t instances;  // InstanceDesc[]
};

[[vk::push_constant]] ConstantBuffer<MeshParams> g_params;

struct MeshVertex {
  float4 position : SV_Position;
  nointerpolation uint pair : PAIR;  // the scene's (instance, cluster) pair: the id's high bits
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
  const uint local = group.x;
  const uint visible_index = g_params.visible_offset + local;
  const uint2 entry = visible_entry(g_params.visible, local);
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const InstanceDesc instance = instances[entry.x];
  const uint cluster_id = entry.y;
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[cluster_id];
  SetMeshOutputCounts(cluster.vertex_count, cluster.triangle_count);

  const uint t = group_thread.x;
  if (t < cluster.vertex_count) {
    MeshDesc* meshes = (MeshDesc*)g_params.mesh;
    const MeshDesc mesh = meshes[instance.mesh];
    // One table read per workgroup, not per vertex: where this frame's pool holds this entry's
    // deformed vertices, or k_no_pool_slot for the rest pose off the grid.
    const uint base = deform_base(mesh, instance, visible_index);
    const float3 p = load_position(mesh, base, cluster.vertex_offset, t);
    const float3 world = mul(instance.world, float4(p, 1.0)).xyz;
    verts[t].position = mul(g_params.view_proj, float4(world, 1.0));
    verts[t].pair = pair_of(instance, mesh, cluster_id);
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
  return (v.pair << 8) | (primitive_id & 0xff);
}

// Debug view for windows and captures: one color per cluster, shaded a little per triangle.
[shader("fragment")]
float4 fs_color(MeshVertex v, uint primitive_id : SV_PrimitiveID) : SV_Target {
  const uint h = v.cluster_id * 2654435761u;
  const float3 base = float3((h >> 0) & 255, (h >> 8) & 255, (h >> 16) & 255) / 255.0;
  const float shade = 0.7 + 0.3 * frac(float(primitive_id) * 0.6180339887);
  return float4(base * shade, 1.0);
}

// Visibility-buffer path: no attachments; each fragment folds its reversed-Z depth and its
// visibility id into one 64-bit word and takes the nearest with an atomic max. Depth is in the
// high half, so max keeps the nearest surface, and at equal depth the larger id: the larger
// (pair, triangle) in the scene's order, which is the tie rule every rasterizer shares
// (docs/subsystems/gfx.md, "The tie rule"). The software rasterizer (cluster_sw_raster.slang)
// writes the same buffer the same way.
[shader("fragment")]
void fs_visibility(MeshVertex v, uint primitive_id : SV_PrimitiveID) {
  const uint2 pixel = uint2(v.position.xy);
  if (pixel.x >= g_params.width || pixel.y >= g_params.height) return;
  const uint id = (v.pair << 8) | (primitive_id & 0xff);
  const uint64_t value = (uint64_t(asuint(v.position.z)) << 32) | uint64_t(id);
  Atomic<uint64_t>* visibility = (Atomic<uint64_t>*)g_params.visibility;
  visibility[pixel.y * g_params.width + pixel.x].max(value);
}

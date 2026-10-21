// Mesh-shader cluster rasterization (docs/plan/04-renderer.md §4.3): one workgroup per entry of
// the cull pass's visible list, geometry pulled through device addresses, and a visibility ID
// written per pixel: the scene's **pair** in the high 24 bits, the triangle within the cluster in
// the low 8 (scene.slang, `pair_of`: the id is the scene's and not the list's, so a depth tie goes
// to the same surface whatever order the frame drew in). The entry names the (instance, cluster)
// pair, so positions come off the 16-bit grid of that instance's mesh (six bytes a vertex) and go
// into the frame, whose origin is the eye, through `instance_point` before the view projection. scene.slang holds the structs the CPU pins with size tables.

#include "alpha_mask.slang"
#include "scene.slang"

struct MeshParams {
  column_major float4x4 view_proj;
  uint64_t clusters;
  uint64_t mesh;  // MeshDesc[]: the quantized positions and each mesh's grid
  uint64_t triangles;
  uint visible_offset;  // this draw's first entry in the whole scene's visible list
  uint extent;          // the visibility buffer's width | height << 16 (gfx::draw_extent)
  uint64_t visible;     // uint2[]: {instance, cluster} per entry; 0 draws {0, i} in index order
  uint64_t visibility;  // uint64_t[width * height]: depth << 32 | id, written with atomic max
  uint64_t eye;        // FrameEye*: the frame's origin; 0 is the world's
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
  // An alpha-tested pair is the masked variant's (mesh_main_masked, below): this draw leaves it out,
  // and in a frame with no masked material the eye carries no tables and nothing is left out.
  const FrameEye eye = load_eye(g_params.eye);
  const bool keep = !pair_masked(load_mask_tables(g_params.eye, eye), instance, cluster_id);
  const uint vertex_count = keep ? cluster.vertex_count : 0u;
  const uint triangle_count = keep ? cluster.triangle_count : 0u;
  SetMeshOutputCounts(vertex_count, triangle_count);

  const uint t = group_thread.x;
  if (t < vertex_count) {
    MeshDesc* meshes = (MeshDesc*)g_params.mesh;
    const MeshDesc mesh = meshes[instance.mesh];
    // One table read per workgroup, not per vertex: where this frame's pool holds this entry's
    // deformed vertices, or k_no_pool_slot for the rest pose off the grid.
    const uint base = deform_base(mesh, instance, visible_index);
    const float3 p = load_position(mesh, base, cluster.vertex_offset, t);
    // The instance's translation from the frame's eye (scene.slang): the frame's origin is the eye,
    // so no absolute position is formed and a vertex by the eye is as fine as a float is there.
    const float3 from_eye = instance_from_eye(instance, eye);
    const float3 q = instance_point(instance, from_eye, p);
    verts[t].position = mul(g_params.view_proj, float4(q, 1.0));
    verts[t].pair = pair_of(instance, mesh, cluster_id);
    verts[t].cluster_id = cluster_id;
  }
  if (t < triangle_count) {
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
  if (pixel.x >= extent_width(g_params.extent) || pixel.y >= extent_height(g_params.extent)) return;
  const uint id = (v.pair << 8) | (primitive_id & 0xff);
  const uint64_t value = (uint64_t(asuint(v.position.z)) << 32) | uint64_t(id);
  Atomic<uint64_t>* visibility = (Atomic<uint64_t>*)g_params.visibility;
  visibility[pixel.y * extent_width(g_params.extent) + pixel.x].max(value);
}

// ---- alpha-tested clusters (docs/subsystems/renderer.md, "Alpha-tested geometry") -------------
//
// The second draw of a frame with a masked material: the same run of the visible list, keeping only
// the alpha-tested pairs mesh_main left out, with the UV per vertex and the pair's material flat.
// The fragment stage samples the mask with the quad's derivatives and discards below the cutoff,
// then writes exactly the word fs_visibility writes, so the tie rule, the ids and the resolve are
// the opaque path's.
struct MaskedMeshVertex {
  float4 position : SV_Position;
  nointerpolation uint pair : PAIR;
  nointerpolation uint material : MATERIAL;  // the pair's index in the material table
  float2 uv : TEXCOORD0;
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void mesh_main_masked(uint3 group_thread : SV_GroupThreadID, uint3 group : SV_GroupID,
                      out vertices MaskedMeshVertex verts[k_max_vertices],
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
  const FrameEye eye = load_eye(g_params.eye);
  const MaskTables masks = load_mask_tables(g_params.eye, eye);
  const bool keep = pair_masked(masks, instance, cluster_id);
  const uint vertex_count = keep ? cluster.vertex_count : 0u;
  const uint triangle_count = keep ? cluster.triangle_count : 0u;
  SetMeshOutputCounts(vertex_count, triangle_count);

  const uint t = group_thread.x;
  if (t < vertex_count) {
    MeshDesc* meshes = (MeshDesc*)g_params.mesh;
    const MeshDesc mesh = meshes[instance.mesh];
    const uint base = deform_base(mesh, instance, visible_index);
    const float3 p = load_position(mesh, base, cluster.vertex_offset, t);
    const float3 q = instance_point(instance, instance_from_eye(instance, eye), p);
    verts[t].position = mul(g_params.view_proj, float4(q, 1.0));
    verts[t].pair = pair_of(instance, mesh, cluster_id);
    verts[t].material = pair_material(masks, instance, cluster_id);
    verts[t].uv = vertex_uv(masks, cluster.vertex_offset + t);
  }
  if (t < triangle_count) {
    uint* triangles = (uint*)g_params.triangles;
    const uint packed = triangles[cluster.triangle_offset + t];
    tris[t] = uint3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
    prims[t].primitive_id = t;
  }
}

// fs_visibility behind the mask.
[shader("fragment")]
void fs_visibility_masked(MaskedMeshVertex v, uint primitive_id : SV_PrimitiveID) {
  const FrameEye eye = load_eye(g_params.eye);
  const MaskTables masks = load_mask_tables(g_params.eye, eye);
  if (!fade_keeps(masks, v.pair, uint2(v.position.xy))) discard;  // the cross-fade (R91)
  if (!mask_keeps(mask_material(masks, v.material), v.uv)) discard;
  const uint2 pixel = uint2(v.position.xy);
  if (pixel.x >= extent_width(g_params.extent) || pixel.y >= extent_height(g_params.extent)) return;
  const uint id = (v.pair << 8) | (primitive_id & 0xff);
  const uint64_t value = (uint64_t(asuint(v.position.z)) << 32) | uint64_t(id);
  Atomic<uint64_t>* visibility = (Atomic<uint64_t>*)g_params.visibility;
  visibility[pixel.y * extent_width(g_params.extent) + pixel.x].max(value);
}

// The cascades' depth-only draw of a masked cluster: depth where the mask keeps the pixel, and
// nothing else (renderer.md, "Shadows").
[shader("fragment")]
void fs_mask_depth(MaskedMeshVertex v) {
  const FrameEye eye = load_eye(g_params.eye);
  const MaskTables masks = load_mask_tables(g_params.eye, eye);
  if (!fade_keeps(masks, v.pair, uint2(v.position.xy))) discard;  // the cross-fade (R91)
  if (!mask_keeps(mask_material(masks, v.material), v.uv)) discard;
}
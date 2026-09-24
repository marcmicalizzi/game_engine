// Software rasterization of small clusters (docs/plan/04-renderer.md §4.3, experiment E1): one
// 128-thread workgroup per entry of the software visible list, vertices moved into the
// instance's frame and transformed to pixel space in shared memory, then one thread per triangle
// walks the triangle's bounding box with edge functions and writes depth << 32 | id into the
// visibility buffer with a 64-bit atomic max, exactly as the hardware path's fs_visibility does.
// The id's high 24 bits are the scene's **pair** of the workgroup's entry (scene.slang, `pair_of`),
// the same word the mesh and vertex paths write, so a depth tie between this pass and a hardware
// one goes to the same surface either way. Meant for clusters a few dozen pixels across, where a hardware triangle costs
// more in setup than in coverage; the cull pass decides which pairs come here. No clipping: a
// triangle with a vertex behind the camera is skipped, and clusters that close go through the
// hardware path anyway.

#include "scene.slang"

// Same layout as MeshParams in cluster_mesh.slang (128 bytes).
struct RasterParams {
  column_major float4x4 view_proj;
  uint64_t clusters;
  uint64_t mesh;  // MeshDesc[]: the quantized positions and each mesh's grid
  uint64_t triangles;
  uint visible_offset;  // this draw's first entry in the whole scene's visible list
  uint pad;
  uint64_t visible;     // uint2[]: {instance, cluster} per entry; 0 takes {0, i} in order
  uint64_t visibility;  // uint64_t[width * height]
  uint width;
  uint height;
  uint64_t instances;  // InstanceDesc[]
};

[[vk::push_constant]] ConstantBuffer<RasterParams> g_params;

static const uint k_max_vertices = 255;
static const uint k_max_bbox_pixels = 64 * 64;  // larger triangles belong to the hardware path

groupshared float3 s_screen[k_max_vertices];  // pixel x, pixel y, depth
groupshared bool s_in_front[k_max_vertices];

[shader("compute")]
[numthreads(128, 1, 1)]
void sw_raster_main(uint3 group_thread : SV_GroupThreadID, uint3 group : SV_GroupID) {
  const uint local = group.x;
  const uint visible_index = g_params.visible_offset + local;
  const uint2 entry = visible_entry(g_params.visible, local);
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const InstanceDesc instance = instances[entry.x];
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[entry.y];
  const float2 size = float2(float(g_params.width), float(g_params.height));

  // Vertices: mesh space to world to clip space to pixel space, with the same y flip as the
  // hardware viewport.
  MeshDesc* meshes = (MeshDesc*)g_params.mesh;
  const MeshDesc mesh = meshes[instance.mesh];
  // Once per workgroup: this entry's block in the frame's deformed-vertex pool, or the rest pose.
  const uint base = deform_base(mesh, instance, visible_index);
  for (uint v = group_thread.x; v < cluster.vertex_count; v += 128) {
    const float3 p = load_position(mesh, base, cluster.vertex_offset, v);
    const float3 world = mul(instance.world, float4(p, 1.0)).xyz;
    const float4 clip = mul(g_params.view_proj, float4(world, 1.0));
    const bool in_front = clip.w > 1e-6;
    const float3 ndc = in_front ? clip.xyz / clip.w : float3(0.0, 0.0, 0.0);
    s_screen[v] = float3((ndc.x * 0.5 + 0.5) * size.x, (0.5 - ndc.y * 0.5) * size.y, ndc.z);
    s_in_front[v] = in_front;
  }
  GroupMemoryBarrierWithGroupSync();

  const uint t = group_thread.x;
  if (t >= cluster.triangle_count) return;
  uint* triangles = (uint*)g_params.triangles;
  const uint packed = triangles[cluster.triangle_offset + t];
  const uint i0 = packed & 0xff;
  const uint i1 = (packed >> 8) & 0xff;
  const uint i2 = (packed >> 16) & 0xff;
  if (!s_in_front[i0] || !s_in_front[i1] || !s_in_front[i2]) return;
  const float3 v0 = s_screen[i0];
  const float3 v1 = s_screen[i1];
  const float3 v2 = s_screen[i2];

  // Signed area: negative for front faces (counter-clockwise in y-up space becomes clockwise in
  // pixel space). Both windings rasterize, matching a hardware pipeline with culling off.
  float area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
  if (area == 0.0) return;
  const float sign = area < 0.0 ? -1.0 : 1.0;
  area = abs(area);

  const float2 lo = min(min(v0.xy, v1.xy), v2.xy);
  const float2 hi = max(max(v0.xy, v1.xy), v2.xy);
  if (hi.x < 0.0 || hi.y < 0.0 || lo.x >= size.x || lo.y >= size.y) return;
  const int2 min_px = int2(max(floor(lo), float2(0.0, 0.0)));
  const int2 max_px = int2(min(ceil(hi), size - 1.0));
  if (uint(max_px.x - min_px.x + 1) * uint(max_px.y - min_px.y + 1) > k_max_bbox_pixels) return;

  const uint id = (pair_of(instance, mesh, entry.y) << 8) | (t & 0xff);
  Atomic<uint64_t>* visibility = (Atomic<uint64_t>*)g_params.visibility;
  const float inv_area = 1.0 / area;
  for (int y = min_px.y; y <= max_px.y; ++y) {
    for (int x = min_px.x; x <= max_px.x; ++x) {
      const float2 p = float2(float(x) + 0.5, float(y) + 0.5);
      // Edge functions, oriented so that inside is positive for either winding.
      const float w0 = sign * ((v2.x - v1.x) * (p.y - v1.y) - (v2.y - v1.y) * (p.x - v1.x));
      const float w1 = sign * ((v0.x - v2.x) * (p.y - v2.y) - (v0.y - v2.y) * (p.x - v2.x));
      const float w2 = sign * ((v1.x - v0.x) * (p.y - v0.y) - (v1.y - v0.y) * (p.x - v0.x));
      if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;
      // Depth (z/w) is affine in screen space, so barycentric interpolation is exact.
      const float depth = (w0 * v0.z + w1 * v1.z + w2 * v2.z) * inv_area;
      const uint64_t value = (uint64_t(asuint(depth)) << 32) | uint64_t(id);
      visibility[uint(y) * g_params.width + uint(x)].max(value);
    }
  }
}

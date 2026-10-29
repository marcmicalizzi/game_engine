// **An alpha-tested cluster's coverage**, baked once when a scene is made (docs/subsystems/
// renderer.md, "The opaque shadow proxy"; roadmap R109): the fraction of the cluster's surface its
// mask keeps, one byte per cluster, which the resolve's shadow rays read to treat a dense cluster
// as solid without looking its mask up (`ResolveParams::shadow_coverage`, visibility_resolve.slang).
//
// One workgroup per listed cluster, its lanes striding over the cluster's triangles. Each triangle
// is sampled at the centroids of its n x n equal-area sub-triangles (n from the cluster's triangle
// count, so a two-triangle card gets as many samples as a leafy hundred-triangle cluster), each
// sample asks the mask exactly what a shadow ray's candidate asks (`mask_keeps_level0`, at level 0
// through the material's UV transform), and a triangle's kept fraction is weighted by its area on
// the mesh's 16-bit grid — the grid is one step on every axis, so the raw integers' cross product
// is the area up to a constant the ratio cancels. The result is the kept area over the whole area,
// rounded to 255ths and OR-ed into byte `cluster & 3` of word `cluster >> 2` of a zeroed table.
//
// Only alpha-tested clusters are listed: anything else is solid to a shadow ray already, and a
// cluster the bake never wrote reads 0, which is "ask the mask".

#include "alpha_mask.slang"

// Mirrors renderer::AlphaCoverageParams (64 bytes).
struct AlphaCoverageParams {
  uint64_t clusters;    // ClusterDesc[]
  uint64_t triangles;   // uint[]: packed local indices
  uint64_t quantized;   // the scene's 16-bit positions, three per vertex
  uint64_t attributes;  // VertexAttributes[]: the UVs
  uint64_t materials;   // ResolveMaterial[]
  uint64_t list;        // uint2[count]: {cluster, its material}
  uint64_t out;         // uint[(cluster_count + 3) / 4], zeroed: the coverage bytes
  uint count;
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<AlphaCoverageParams> g_params;

static const uint k_alpha_coverage_workgroup = 64;  // renderer::k_alpha_coverage_workgroup
// Groups per row of the 2D dispatch: a scene can list more clusters than one dimension holds.
static const uint k_alpha_coverage_row = 65535;

groupshared float g_kept[k_alpha_coverage_workgroup];
groupshared float g_area[k_alpha_coverage_workgroup];

int3 grid_position(uint vertex) {
  uint* words = (uint*)g_params.quantized;
  const uint i = vertex * 3;
  const uint w0 = words[i >> 1];
  const uint w1 = words[(i + 1) >> 1];
  const uint w2 = words[(i + 2) >> 1];
  const uint x = (i & 1) != 0 ? (w0 >> 16) : (w0 & 0xffff);
  const uint y = ((i + 1) & 1) != 0 ? (w1 >> 16) : (w1 & 0xffff);
  const uint z = ((i + 2) & 1) != 0 ? (w2 >> 16) : (w2 & 0xffff);
  return int3(int(x), int(y), int(z));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void alpha_coverage_main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
  const uint item = group.y * k_alpha_coverage_row + group.x;
  const uint lane = thread.x;
  float kept = 0.0;
  float area = 0.0;
  uint cluster_index = 0;
  if (item < g_params.count) {
    uint2* list = (uint2*)g_params.list;
    const uint2 entry = list[item];
    cluster_index = entry.x;
    ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
    const ClusterDesc cluster = clusters[cluster_index];
    MaskTables tables = no_mask_tables();
    tables.materials = g_params.materials;
    tables.attributes = g_params.attributes;
    const ResolveMaterial material = mask_material(tables, entry.y);
    // Samples a triangle: n^2 of them, at least 256 a cluster and at most 64 a triangle.
    const uint per_cluster = 256;
    uint n = 2;
    while (n < 8 && n * n * max(cluster.triangle_count, 1u) < per_cluster)
      ++n;
    const float inv_n = 1.0 / float(n);
    for (uint t = lane; t < cluster.triangle_count; t += k_alpha_coverage_workgroup) {
      uint* packed_triangles = (uint*)g_params.triangles;
      const uint packed = packed_triangles[cluster.triangle_offset + t];
      const uint v0 = cluster.vertex_offset + (packed & 0xffu);
      const uint v1 = cluster.vertex_offset + ((packed >> 8) & 0xffu);
      const uint v2 = cluster.vertex_offset + ((packed >> 16) & 0xffu);
      const float3 p0 = float3(grid_position(v0));
      const float3 e1 = float3(grid_position(v1)) - p0;
      const float3 e2 = float3(grid_position(v2)) - p0;
      const float a = length(cross(e1, e2));
      if (!(a > 0.0)) continue;
      const float2 uv0 = vertex_uv(tables, v0);
      const float2 uv1 = vertex_uv(tables, v1);
      const float2 uv2 = vertex_uv(tables, v2);
      uint hits = 0;
      for (uint i = 0; i < n; ++i) {
        for (uint j = 0; i + j < n; ++j) {
          // The upward sub-triangle (i, j) and, where there is one, the downward beside it.
          float2 b = float2((float(i) + 1.0 / 3.0) * inv_n, (float(j) + 1.0 / 3.0) * inv_n);
          if (mask_keeps_level0(material, uv0 + b.x * (uv1 - uv0) + b.y * (uv2 - uv0))) ++hits;
          if (i + j + 1 < n) {
            b = float2((float(i) + 2.0 / 3.0) * inv_n, (float(j) + 2.0 / 3.0) * inv_n);
            if (mask_keeps_level0(material, uv0 + b.x * (uv1 - uv0) + b.y * (uv2 - uv0))) ++hits;
          }
        }
      }
      kept += a * float(hits) / float(n * n);
      area += a;
    }
  }
  g_kept[lane] = kept;
  g_area[lane] = area;
  GroupMemoryBarrierWithGroupSync();
  for (uint stride = k_alpha_coverage_workgroup / 2; stride > 0; stride >>= 1) {
    if (lane < stride) {
      g_kept[lane] += g_kept[lane + stride];
      g_area[lane] += g_area[lane + stride];
    }
    GroupMemoryBarrierWithGroupSync();
  }
  if (lane == 0 && item < g_params.count) {
    // A cluster with no area keeps its mask: 0 is "ask the mask".
    const float fraction = g_area[0] > 0.0 ? g_kept[0] / g_area[0] : 0.0;
    const uint coverage = min(uint(fraction * 255.0 + 0.5), 255u);
    uint* out = (uint*)g_params.out;
    InterlockedOr(out[cluster_index >> 2], coverage << ((cluster_index & 3u) * 8u));
  }
}

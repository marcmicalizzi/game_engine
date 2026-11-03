// **The opacity micromaps' bake** (gfx/opacity_micromap.h; docs/subsystems/renderer.md, "Opacity
// micromaps"; roadmap R120), once when a scene is made: one thread per alpha-tested triangle, in two
// modes of one entry point.
//
//   mode 0, classify: the triangle's level — the smallest whose micro-triangles cover at most
//           `target_texels` of its mask's level-0 texels, then the cap — and which of the four
//           states its micro-triangles took, as `level | seen << 4` into `codes`.
//   mode 1, write: for a triangle the host gave data (its index is an entry, not a special one),
//           every micro-triangle's state, sixteen to a 32-bit word, at the entry's offset.
//
// **A micro-triangle's state is conservative** (opacity_micromap.h): the ray's test is
// `mask_keeps_level0` (alpha_mask.slang) — the base colour's alpha at level 0, bilinear through the
// material's sampler and UV transform, times the factor, against the cutoff — and a bilinear sample
// is a convex combination of its 2x2 texels. So the micro-triangle's UV box, widened by
// `k_texel_margin`, is walked 2x2 quad by quad with `GatherAlpha` (the same texels, the same
// addressing, level 0), and it is opaque only if every texel keeps by `k_alpha_margin`, transparent
// only if every one drops by it, and unknown otherwise; an unknown one is "unknown opaque" where its
// centre sample keeps, which is what a forced two-state answer would read.
//
// The micro-triangle order is the format's (gfx::micromap_micro_triangle mirrors `micro_triangle`).

#include "alpha_mask.slang"

// Mirrors gfx::MicromapBakeParams (104 bytes).
struct MicromapBakeParams {
  uint64_t clusters;
  uint64_t triangles;
  uint64_t attributes;
  uint64_t materials;
  uint64_t items;     // uint2 per item: {cluster, material}
  uint64_t refs;      // uint2 per triangle: {item, triangle within the cluster}
  uint64_t codes;     // uint per triangle (mode 0 out)
  uint64_t indices;   // int per triangle (mode 1 in)
  uint64_t entries;   // uint2 per entry: {data offset, level | format << 16} (mode 1 in)
  uint64_t data;      // uint[] (mode 1 out)
  uint64_t counters;  // uint64_t[3] (mode 1 out): transparent, opaque, unknown micro-triangles
  uint count;
  uint mode;  // bits 0..7: 0 classify, 1 write; bits 8..11: the level cap
  float target_texels;  // a micro-triangle's most level-0 texels
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<MicromapBakeParams> g_params;

static const uint k_workgroup = 64;  // gfx::k_micromap_bake_workgroup
static const uint k_row = 65535;     // gfx::k_micromap_bake_row
static const float k_alpha_margin = 1.0 / 512.0;   // gfx::k_micromap_alpha_margin
static const float k_texel_margin = 1.0 / 64.0;    // gfx::k_micromap_texel_margin
static const uint k_max_quads = 1024;              // gfx::k_micromap_max_quads
static const uint k_transparent = 0;
static const uint k_opaque = 1;
static const uint k_unknown_transparent = 2;
static const uint k_unknown_opaque = 3;

// Corners of micro-triangle `index` at `level` in barycentrics (weights of vertices 1 and 2):
// the children of (c0, c1, c2) are (c0, c01, c20), (c20, c12, c01), (c01, c1, c12), (c12, c20, c2),
// the digits of `index` most significant first.
void micro_triangle(uint index, uint level, out float2 b0, out float2 b1, out float2 b2) {
  b0 = float2(0.0, 0.0);
  b1 = float2(1.0, 0.0);
  b2 = float2(0.0, 1.0);
  for (uint l = level; l > 0; --l) {
    const uint digit = (index >> (2u * (l - 1u))) & 3u;
    const float2 m01 = (b0 + b1) * 0.5;
    const float2 m12 = (b1 + b2) * 0.5;
    const float2 m20 = (b2 + b0) * 0.5;
    if (digit == 0u) {
      b1 = m01;
      b2 = m20;
    } else if (digit == 1u) {
      b0 = m20;
      b1 = m12;
      b2 = m01;
    } else if (digit == 2u) {
      b0 = m01;
      b2 = m12;
    } else {
      b0 = m12;
      b1 = m20;
    }
  }
}

// What one texel's alpha says: 1 keeps, 2 drops, 3 too near the cutoff to say.
uint texel_says(float alpha, float cutoff) {
  if (alpha >= cutoff + k_alpha_margin) return 1u;
  if (alpha < cutoff - k_alpha_margin) return 2u;
  return 3u;
}

// The state of the micro-triangle with UV corners t0..t2 (the material's UV transform applied).
uint micro_state(ResolveMaterial material, float2 dims, float2 t0, float2 t1, float2 t2) {
  const float factor = material.uv_offset.w;
  const float cutoff = mask_cutoff(material);
  if (material.albedo_texture == k_no_texture) {
    const uint says = texel_says(factor, cutoff);
    return says == 1u ? k_opaque : (says == 2u ? k_transparent : k_unknown_opaque);
  }
  // In texel space a bilinear sample at p reads texels floor(p - 0.5) and the next ones.
  const float2 lo = min(min(t0, t1), t2) * dims - 0.5 - k_texel_margin;
  const float2 hi = max(max(t0, t1), t2) * dims - 0.5 + k_texel_margin;
  const int2 q0 = int2(floor(lo));
  const int2 q1 = int2(floor(hi));
  const uint2 extent = uint2(q1 - q0 + 1);
  uint says = 0u;
  if (extent.x * extent.y > k_max_quads || !all(isfinite(lo)) || !all(isfinite(hi))) {
    says = 3u;
  } else {
    Texture2D<float4> texture = g_textures[NonUniformResourceIndex(material.albedo_texture)];
    SamplerState sampler = g_samplers[NonUniformResourceIndex(material.sampler)];
    for (int y = q0.y; y <= q1.y && says != 3u; ++y) {
      for (int x = q0.x; x <= q1.x && says != 3u; ++x) {
        // The corner the four texels (x, y)..(x + 1, y + 1) share: a gather there reads exactly them,
        // wrapped or clamped as the sampler addresses them.
        const float2 uv = (float2(float(x), float(y)) + 1.0) / dims;
        const float4 a = texture.GatherAlpha(sampler, uv) * factor;
        says |= texel_says(a.x, cutoff) | texel_says(a.y, cutoff) | texel_says(a.z, cutoff) |
                texel_says(a.w, cutoff);
      }
    }
  }
  if (says == 1u) return k_opaque;
  if (says == 2u) return k_transparent;
  // Unknown: the sub-state is the centre's answer, for a two-state reading of the same map.
  const float2 centre = (t0 + t1 + t2) * (1.0 / 3.0);
  float alpha = factor *
                g_textures[NonUniformResourceIndex(material.albedo_texture)]
                    .SampleLevel(g_samplers[NonUniformResourceIndex(material.sampler)], centre, 0)
                    .a;
  return alpha >= cutoff ? k_unknown_opaque : k_unknown_transparent;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void omm_bake_main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
  const uint t = (group.y * k_row + group.x) * k_workgroup + thread.x;
  if (t >= g_params.count) return;
  const uint mode = g_params.mode & 0xffu;
  const uint cap = (g_params.mode >> 8) & 0xfu;
  uint2* refs = (uint2*)g_params.refs;
  const uint2 ref = refs[t];
  uint2* items = (uint2*)g_params.items;
  const uint2 item = items[ref.x];
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[item.x];
  MaskTables tables = no_mask_tables();
  tables.materials = g_params.materials;
  tables.attributes = g_params.attributes;
  const ResolveMaterial material = mask_material(tables, item.y);
  uint* packed_triangles = (uint*)g_params.triangles;
  const uint packed = packed_triangles[cluster.triangle_offset + ref.y];
  const float2 uv0 = material_uv(material, vertex_uv(tables, cluster.vertex_offset + (packed & 0xffu)));
  const float2 uv1 =
      material_uv(material, vertex_uv(tables, cluster.vertex_offset + ((packed >> 8) & 0xffu)));
  const float2 uv2 =
      material_uv(material, vertex_uv(tables, cluster.vertex_offset + ((packed >> 16) & 0xffu)));
  float2 dims = float2(1.0, 1.0);
  if (material.albedo_texture != k_no_texture) {
    uint w = 1u;
    uint h = 1u;
    g_textures[NonUniformResourceIndex(material.albedo_texture)].GetDimensions(w, h);
    dims = float2(float(max(w, 1u)), float(max(h, 1u)));
  }

  uint level = 0u;
  if (mode == 0u) {
    const float2 e1 = (uv1 - uv0) * dims;
    const float2 e2 = (uv2 - uv0) * dims;
    const float area = abs(e1.x * e2.y - e1.y * e2.x) * 0.5;
    float micro = area;
    while (level < cap && micro > g_params.target_texels) {
      micro *= 0.25;
      ++level;
    }
  } else {
    int* indices = (int*)g_params.indices;
    const int index = indices[t];
    if (index < 0) return;  // a special index: no data
    uint2* entries = (uint2*)g_params.entries;
    const uint2 entry = entries[uint(index)];
    level = entry.y & 0xffffu;
    const uint count = 1u << (2u * level);
    uint* data = (uint*)g_params.data;
    const uint base = entry.x >> 2;
    uint counts[4] = { 0u, 0u, 0u, 0u };
    for (uint w = 0u; w * 16u < count; ++w) {
      uint word = 0u;
      for (uint k = 0u; k < 16u && w * 16u + k < count; ++k) {
        float2 b0, b1, b2;
        micro_triangle(w * 16u + k, level, b0, b1, b2);
        const uint state = micro_state(material, dims, uv0 + b0.x * (uv1 - uv0) + b0.y * (uv2 - uv0),
                                       uv0 + b1.x * (uv1 - uv0) + b1.y * (uv2 - uv0),
                                       uv0 + b2.x * (uv1 - uv0) + b2.y * (uv2 - uv0));
        word |= state << (2u * k);
        counts[state] += 1u;
      }
      data[base + w] = word;
    }
    Atomic<uint64_t>* counters = (Atomic<uint64_t>*)g_params.counters;
    counters[0].add(uint64_t(counts[k_transparent]));
    counters[1].add(uint64_t(counts[k_opaque]));
    counters[2].add(uint64_t(counts[k_unknown_transparent] + counts[k_unknown_opaque]));
    return;
  }
  uint seen = 0u;
  const uint count = 1u << (2u * level);
  for (uint m = 0u; m < count && seen != 0xfu; ++m) {
    float2 b0, b1, b2;
    micro_triangle(m, level, b0, b1, b2);
    const uint state = micro_state(material, dims, uv0 + b0.x * (uv1 - uv0) + b0.y * (uv2 - uv0),
                                   uv0 + b1.x * (uv1 - uv0) + b1.y * (uv2 - uv0),
                                   uv0 + b2.x * (uv1 - uv0) + b2.y * (uv2 - uv0));
    seen |= 1u << state;
    // Two states seen already decide that the triangle needs data; the rest cannot undo it.
    if (countbits(seen) > 1u) break;
  }
  uint* codes = (uint*)g_params.codes;
  codes[t] = level | (seen << 4);
}

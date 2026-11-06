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
  uint64_t counters;  // uint64_t[13][4] (mode 1 out): per level, transparent, opaque, unknown,
                      // and the unknown ones the point samples agree on; each times the entry's uses
  uint64_t keys;      // uint2 per triangle (mode 0 out): the dedup key over material and UVs
  uint64_t writers;   // uint2 per entry: {its triangle, its uses} (modes 1 and 2 in)
  uint count;
  uint mode;  // bits 0..7: 0 classify, 1 write, 2 verify; bits 8..11: the level cap
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

// One bilinear sample of the mask at level 0, as the ray loop's `mask_keeps_level0` reads it.
bool point_keeps(ResolveMaterial material, float2 uv) {
  const float factor = material.uv_offset.w;
  if (material.albedo_texture == k_no_texture) return factor >= mask_cutoff(material);
  const float alpha =
      factor * g_textures[NonUniformResourceIndex(material.albedo_texture)]
                   .SampleLevel(g_samplers[NonUniformResourceIndex(material.sampler)], uv, 0)
                   .a;
  return alpha >= mask_cutoff(material);
}

// A masked triangle's material and its UVs with the material's transform applied: what its states
// are a function of, and so what two triangles must share to share an entry.
struct BakeTriangle {
  ResolveMaterial material;
  uint material_index;
  float2 uv0;
  float2 uv1;
  float2 uv2;
  float2 dims;
};

BakeTriangle load_triangle(uint t) {
  uint2* refs = (uint2*)g_params.refs;
  const uint2 ref = refs[t];
  uint2* items = (uint2*)g_params.items;
  const uint2 item = items[ref.x];
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[item.x];
  MaskTables tables = no_mask_tables();
  tables.materials = g_params.materials;
  tables.attributes = g_params.attributes;
  BakeTriangle out;
  out.material_index = item.y;
  out.material = mask_material(tables, item.y);
  uint* packed_triangles = (uint*)g_params.triangles;
  const uint packed = packed_triangles[cluster.triangle_offset + ref.y];
  out.uv0 = material_uv(out.material, vertex_uv(tables, cluster.vertex_offset + (packed & 0xffu)));
  out.uv1 = material_uv(out.material,
                        vertex_uv(tables, cluster.vertex_offset + ((packed >> 8) & 0xffu)));
  out.uv2 = material_uv(out.material,
                        vertex_uv(tables, cluster.vertex_offset + ((packed >> 16) & 0xffu)));
  out.dims = float2(1.0, 1.0);
  if (out.material.albedo_texture != k_no_texture) {
    uint w = 1u;
    uint h = 1u;
    g_textures[NonUniformResourceIndex(out.material.albedo_texture)].GetDimensions(w, h);
    out.dims = float2(float(max(w, 1u)), float(max(h, 1u)));
  }
  return out;
}

uint mix32(uint h, uint k) {
  k *= 0xcc9e2d51u;
  k = (k << 15) | (k >> 17);
  k *= 0x1b873593u;
  h ^= k;
  h = (h << 13) | (h >> 19);
  return h * 5u + 0xe6546b64u;
}

uint finish32(uint h) {
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  return h ^ (h >> 16);
}

// The dedup key: two hashes of the material and the six UV words, under two seeds. Equal keys only
// propose a share; the verify pass compares the words themselves.
uint2 triangle_key(BakeTriangle tri) {
  const uint words[7] = { tri.material_index, asuint(tri.uv0.x), asuint(tri.uv0.y),
                          asuint(tri.uv1.x),  asuint(tri.uv1.y), asuint(tri.uv2.x),
                          asuint(tri.uv2.y) };
  uint a = 0x9747b28cu;
  uint b = 0x5bd1e995u;
  for (uint i = 0u; i < 7u; ++i) {
    a = mix32(a, words[i]);
    b = mix32(b, words[i] ^ 0xa5a5a5a5u);
  }
  return uint2(finish32(a ^ 28u), finish32(b ^ 0x1cu));
}

bool same_triangle(BakeTriangle a, BakeTriangle b) {
  return a.material_index == b.material_index && all(asuint(a.uv0) == asuint(b.uv0)) &&
         all(asuint(a.uv1) == asuint(b.uv1)) && all(asuint(a.uv2) == asuint(b.uv2));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void omm_bake_main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
  const uint i = (group.y * k_row + group.x) * k_workgroup + thread.x;
  if (i >= g_params.count) return;
  const uint mode = g_params.mode & 0xffu;
  const uint cap = (g_params.mode >> 8) & 0xfu;
  uint* codes = (uint*)g_params.codes;
  uint2* writers = (uint2*)g_params.writers;

  if (mode == 2u) {
    // Verify: a triangle whose index names an entry another triangle writes must be that
    // triangle's material and UVs word for word, or the host gives it an entry of its own.
    int* indices = (int*)g_params.indices;
    const int index = indices[i];
    if (index < 0) return;
    const uint writer = writers[uint(index)].x;
    if (writer == i) return;
    if (!same_triangle(load_triangle(i), load_triangle(writer))) codes[i] |= 0x80000000u;
    return;
  }

  // Mode 1 runs one thread an entry, on the triangle that writes it; mode 0 one a triangle.
  const uint t = mode == 1u ? writers[i].x : i;
  const BakeTriangle tri = load_triangle(t);
  const ResolveMaterial material = tri.material;
  const float2 uv0 = tri.uv0;
  const float2 uv1 = tri.uv1;
  const float2 uv2 = tri.uv2;
  const float2 dims = tri.dims;

  uint level = 0u;
  uint wanted = 0u;
  if (mode == 0u) {
    const float2 e1 = (uv1 - uv0) * dims;
    const float2 e2 = (uv2 - uv0) * dims;
    const float area = abs(e1.x * e2.y - e1.y * e2.x) * 0.5;
    float micro = area;
    // The level the texel target asks for, up to the format's 12, and the cap's.
    while (wanted < 12u && micro > g_params.target_texels) {
      micro *= 0.25;
      ++wanted;
    }
    level = min(wanted, cap);
  } else {
    const uint2 entry = ((uint2*)g_params.entries)[i];
    const uint uses = writers[i].y;
    level = entry.y & 0xffffu;
    const uint count = 1u << (2u * level);
    uint* data = (uint*)g_params.data;
    const uint base = entry.x >> 2;
    uint counts[5] = { 0u, 0u, 0u, 0u, 0u };
    for (uint w = 0u; w * 16u < count; ++w) {
      uint word = 0u;
      for (uint k = 0u; k < 16u && w * 16u + k < count; ++k) {
        float2 b0, b1, b2;
        micro_triangle(w * 16u + k, level, b0, b1, b2);
        const float2 t0 = uv0 + b0.x * (uv1 - uv0) + b0.y * (uv2 - uv0);
        const float2 t1 = uv0 + b1.x * (uv1 - uv0) + b1.y * (uv2 - uv0);
        const float2 t2 = uv0 + b2.x * (uv1 - uv0) + b2.y * (uv2 - uv0);
        const uint state = micro_state(material, dims, t0, t1, t2);
        word |= state << (2u * k);
        counts[state] += 1u;
        if (state >= k_unknown_transparent) {
          // Diagnosis only (R120 stage 3): an unknown micro-triangle whose corners and centre all
          // give one answer is unknown because of the filter's footprint, not because the mask's
          // edge is seen to cross it.
          const bool c = point_keeps(material, (t0 + t1 + t2) * (1.0 / 3.0));
          if (point_keeps(material, t0) == c && point_keeps(material, t1) == c &&
              point_keeps(material, t2) == c) {
            counts[4] += 1u;
          }
        }
      }
      data[base + w] = word;
    }
    Atomic<uint64_t>* counters = (Atomic<uint64_t>*)g_params.counters;
    const uint row = level * 4u;
    counters[row + 0u].add(uint64_t(counts[k_transparent]) * uses);
    counters[row + 1u].add(uint64_t(counts[k_opaque]) * uses);
    counters[row + 2u].add(uint64_t(counts[k_unknown_transparent] + counts[k_unknown_opaque]) *
                           uses);
    counters[row + 3u].add(uint64_t(counts[4]) * uses);
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
  codes[t] = level | (seen << 4) | (wanted << 8);
  ((uint2*)g_params.keys)[t] = triangle_key(tri);
}

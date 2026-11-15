// The alpha test every reader of an alpha-tested material shares (docs/subsystems/renderer.md,
// "Alpha-tested geometry"; roadmap R65, plan 04 §4.3): the rasterizers' masked variants, the
// cascades' masked depth draw, the primary and shadow ray loops and the reference path tracer. No
// entry points. It includes material.slang for the material mirror, its UV transform and the
// bindless arrays, so a module that includes it binds the scene's bindless set, which every one of
// those pipelines does.
//
// **The mask is the base colour's alpha** times the base colour factor's (glTF's rule), and a
// pixel or a hit is kept where that is at or above the material's cutoff (`gfx::set_alpha_mask`
// packs both into `uv_offset.zw`). A rasterizer samples it with the quad's own derivatives, one tap
// at the level of the footprint's shorter axis and with no anisotropy (`mask_keeps`), which is a
// level the chain's coverage was kept at (texture.md, "Alpha-tested masks"); a ray has no quad, and samples
// level 0, whose coverage is the one every level is held to: a ray cone could pick the level, and
// nothing here needs one yet.

#ifndef ENGINE_ALPHA_MASK_SLANG
#define ENGINE_ALPHA_MASK_SLANG

#include "material.slang"
#include "scene.slang"

ResolveMaterial mask_material(MaskTables tables, uint material) {
  ResolveMaterial* materials = (ResolveMaterial*)tables.materials;
  return materials[material];
}

float mask_cutoff(ResolveMaterial material) { return material.uv_offset.z; }

// A fragment stage's test, with the quad's derivatives of the interpolated UV.
//
// **One tap of one level, and the level is the footprint's shorter axis's** (renderer.md,
// "Alpha-tested geometry"; G203). The material's own sampler is anisotropic: it picks that same
// level and then averages up to sixteen taps along the longer axis — an average the build never
// scaled, so on a card seen at a slant (most of a crown's) a thin mask's alpha falls under the
// cutoff and the card draws nothing. One tap is a point sample of a level the build held to level
// 0's coverage, so it passes in that share whatever the slant: across the shorter axis the level
// is the pixel's own, and along the longer one the test is a sample, as a ray's is everywhere.
// The level of the *longer* axis would be the filtered answer and is the wrong one for the same
// reason a far level is hard to hold: a card at a slant reaches the chain's last few texels, and
// an atlas's tiles are mixed together well before them. The shorter axis is held to a sixteenth
// of the longer, the sampler's own limit. Two gradients of one length at right angles ask the
// sampler for a ratio of one, which is one trilinear tap.
bool mask_keeps(ResolveMaterial material, float2 uv) {
  float alpha = material.uv_offset.w;
  if (material.albedo_texture != k_no_texture) {
    const float2 st = material_uv(material, uv);
    const float2 dx = ddx(st);
    const float2 dy = ddy(st);
    const float lx = dot(dx, dx);
    const float ly = dot(dy, dy);
    const float2 minor = lx <= ly ? dx : dy;
    const float longer = sqrt(max(lx, ly));
    const float shorter = sqrt(min(lx, ly));
    // The tap's footprint: the shorter axis's length, or a sixteenth of the longer's if that is
    // more, along the shorter axis's direction (any direction, if the quad gave none).
    const float reach = max(shorter, longer * (1.0 / 16.0));
    const float2 along = shorter > 0.0 ? minor * (reach / shorter) : float2(reach, 0.0);
    alpha *= g_textures[NonUniformResourceIndex(material.albedo_texture)]
                 .SampleGrad(g_samplers[NonUniformResourceIndex(material.sampler)], st, along,
                             float2(-along.y, along.x))
                 .a;
  }
  return alpha >= mask_cutoff(material);
}

// A ray's test, at level 0.
bool mask_keeps_level0(ResolveMaterial material, float2 uv) {
  float alpha = material.uv_offset.w;
  if (material.albedo_texture != k_no_texture) {
    alpha *= g_textures[NonUniformResourceIndex(material.albedo_texture)]
                 .SampleLevel(g_samplers[NonUniformResourceIndex(material.sampler)],
                              material_uv(material, uv), 0)
                 .a;
  }
  return alpha >= mask_cutoff(material);
}

// The UV a ray's candidate hit stands on: triangle `primitive` of `cluster` at the hit's two
// barycentrics (the weights of its second and third corners, as a ray query reports them).
float2 hit_uv(MaskTables tables, uint64_t triangles, ClusterDesc cluster, uint primitive,
              float2 barycentrics) {
  uint* packed_triangles = (uint*)triangles;
  const uint packed = packed_triangles[cluster.triangle_offset + primitive];
  const float2 uv0 = vertex_uv(tables, cluster.vertex_offset + (packed & 0xffu));
  const float2 uv1 = vertex_uv(tables, cluster.vertex_offset + ((packed >> 8) & 0xffu));
  const float2 uv2 = vertex_uv(tables, cluster.vertex_offset + ((packed >> 16) & 0xffu));
  return uv0 + barycentrics.x * (uv1 - uv0) + barycentrics.y * (uv2 - uv0);
}

// ---- the cross-fade (renderer.md, "The cross-fade"; R91) ----------------------------------------
//
// A pixel's stable threshold: a hash of its integer position and a seed (the drawn pair's instance,
// which both sides of a transition share, so their pixels are complementary — never the pair, which
// differs across it), in 0 .. 65534. Nothing in it is the frame's, so a still view does not shimmer.
uint fade_hash(uint2 pixel, uint seed) {
  uint h = pixel.x * 0x8da6b343u ^ pixel.y * 0xd8163841u ^ seed * 0xcb1ab31fu;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return ((h >> 16) * 65535u) >> 16;
}

// Screen-door transparency between two levels of a fading transition: the cull pass wrote the
// drawn pair's two splits (unorm16, own low, parent high) and its instance to the table, and the
// pixel is kept where its hash is at or past the own split and below the parent's — so of every
// path of the DAG exactly one cluster keeps each pixel, the visibility buffer keeps one id a pixel
// and the tie rule stands. A pair outside every band has splits 0 and 65535 and keeps everything.
// No table (`fade` 0, a frame with no fade) keeps everything and reads nothing.
bool fade_keeps(MaskTables tables, uint pair, uint2 pixel) {
  if (tables.fade == 0) return true;
  uint2* table = (uint2*)tables.fade;
  const uint2 entry = table[pair];
  const uint low = entry.x & 0xffffu;
  const uint high = entry.x >> 16;
  if (low == 0u && high == 0xffffu) return true;
  const uint h = fade_hash(pixel, entry.y);
  return h >= low && h < high;
}

// **The ray loops' rule** for a non-opaque candidate of pair (instance, cluster): an alpha-tested
// pair's mask decides, and anything else non-opaque — a shadow caster, built non-opaque so the
// primary rays can leave it out — is solid. True keeps the hit. A masked pair inside a fade band is
// also kept only where the ray's pixel keeps it (`fade_keeps`, with `pair` its scene pair).
bool candidate_keeps(MaskTables tables, uint64_t clusters, uint64_t triangles,
                     InstanceDesc instance, uint cluster_index, uint primitive,
                     float2 barycentrics, uint pair, uint2 pixel) {
  if (tables.materials == 0) return true;
  const uint material = pair_material(tables, instance, cluster_index);
  if (!material_is_masked(tables, material)) return true;
  if (!fade_keeps(tables, pair, pixel)) return false;
  ClusterDesc* all = (ClusterDesc*)clusters;
  const float2 uv = hit_uv(tables, triangles, all[cluster_index], primitive, barycentrics);
  return mask_keeps_level0(mask_material(tables, material), uv);
}

#endif  // ENGINE_ALPHA_MASK_SLANG

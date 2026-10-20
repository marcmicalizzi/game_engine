// The alpha test every reader of an alpha-tested material shares (docs/subsystems/renderer.md,
// "Alpha-tested geometry"; roadmap R65, plan 04 §4.3): the rasterizers' masked variants, the
// cascades' masked depth draw, the primary and shadow ray loops and the reference path tracer. No
// entry points. It includes material.slang for the material mirror, its UV transform and the
// bindless arrays, so a module that includes it binds the scene's bindless set, which every one of
// those pipelines does.
//
// **The mask is the base colour's alpha** times the base colour factor's (glTF's rule), and a
// pixel or a hit is kept where that is at or above the material's cutoff (`gfx::set_alpha_mask`
// packs both into `uv_offset.zw`). A rasterizer samples it with the quad's own derivatives, which is
// the level the chain's coverage was kept at (texture.md, "Alpha-tested masks"); a ray has no quad,
// and samples level 0: a ray cone could pick the level, and nothing here needs one yet.

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
bool mask_keeps(ResolveMaterial material, float2 uv) {
  float alpha = material.uv_offset.w;
  if (material.albedo_texture != k_no_texture) {
    alpha *= g_textures[NonUniformResourceIndex(material.albedo_texture)]
                 .Sample(g_samplers[NonUniformResourceIndex(material.sampler)],
                         material_uv(material, uv))
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

// **The ray loops' rule** for a non-opaque candidate of pair (instance, cluster): an alpha-tested
// pair's mask decides, and anything else non-opaque — a shadow caster, built non-opaque so the
// primary rays can leave it out — is solid. True keeps the hit.
bool candidate_keeps(MaskTables tables, uint64_t clusters, uint64_t triangles,
                     InstanceDesc instance, uint cluster_index, uint primitive,
                     float2 barycentrics) {
  if (tables.materials == 0) return true;
  const uint material = pair_material(tables, instance, cluster_index);
  if (!material_is_masked(tables, material)) return true;
  ClusterDesc* all = (ClusterDesc*)clusters;
  const float2 uv = hit_uv(tables, triangles, all[cluster_index], primitive, barycentrics);
  return mask_keeps_level0(mask_material(tables, material), uv);
}

#endif  // ENGINE_ALPHA_MASK_SLANG

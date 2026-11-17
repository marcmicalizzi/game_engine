// **Which level of each texture the frame's samples asked for** (systems/renderer/src/
// texture_feedback.h; docs/subsystems/texture.md, "Levels resident by what the frame samples";
// roadmap R172). A measurement pass, off unless `renderer.textures.feedback` names a file: one
// thread a pixel of a view's visibility buffer, which takes the pixel's triangle and barycentrics
// exactly as the resolve does (`reconstruct_screen_kept`), the UV's derivatives from that triangle
// (`screen_uv_gradients_at`, through the material's scale and transform), and for every textured
// slot of the material the level a 16x anisotropic trilinear sample there reads first:
//
//   rho_x, rho_y = |duv/dx|, |duv/dy| in the texture's own texels (its level-0 size)
//   lambda      = log2(max(rho_max / 16, rho_min))        (Vulkan's anisotropic LOD, N <= 16)
//
// and keeps, per bindless slot, the smallest lambda any pixel had (an atomic minimum of a fixed-point
// code, one per wave where a wave's lanes name one slot). The finest level the frame read from that
// texture is `max(0, floor(lambda))`: trilinear filtering blends that level and the next.
//
// What it does not see, and the page says so: the layered ground's and the water's own lookups
// (their materials are skipped: their textures are not the material's five slots at the material's
// UV), a deformed instance's pose (its rest pose stands in: the UV footprint of a pixel moves with
// the surface, not with the pose), and the level-0 reads of rays (the bounce's colour at a hit, a
// traced mask test), which read whatever level 0 the image holds.

#include "../../../domain/gfx/shaders/material.slang"

// Mirrors renderer::TextureFeedbackParams (texture_feedback.cpp; 208 bytes).
struct TextureFeedbackParams {
  column_major float4x4 view_proj;
  uint64_t visibility;  // this view's region: uint64_t[width * height]
  uint64_t clusters;
  uint64_t mesh;
  uint64_t triangles;
  uint64_t instances;
  uint64_t attributes;
  uint64_t materials;
  uint64_t cluster_materials;
  uint64_t pairs;  // uint2[pair_count]: the scene's {instance, cluster} per pair
  uint64_t out;    // uint[slot_count]: the smallest code per bindless sampled-image slot
  uint width;
  uint height;
  uint slot_count;
  uint pad;
  FrameEye eye;
};

struct TextureFeedbackPush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<TextureFeedbackPush> g_feedback;

// lambda in 1/256 steps, offset so that a magnified sample (lambda < 0, level 0) is still ordered:
// code = (lambda + 32) * 256, clamped to [0, 64 * 256). 0xffffffff is "never sampled".
static const float k_feedback_offset = 32.0;
static const float k_feedback_steps = 256.0;
// gfx::k_material_terrain_layers (terrain_material.slang, which only the ground's variants include):
// the layered ground samples its layers' textures at their own scales, not the material's slots.
static const uint k_feedback_terrain_layers = 512u;

uint feedback_code(float2 duv_dx, float2 duv_dy, uint slot) {
  uint w;
  uint h;
  uint levels;
  g_textures[NonUniformResourceIndex(slot)].GetDimensions(0, w, h, levels);
  const float2 size = float2(float(w), float(h));
  const float rx = length(duv_dx * size);
  const float ry = length(duv_dy * size);
  const float rmax = max(rx, ry);
  const float rmin = min(rx, ry);
  const float rho = max(rmax * (1.0 / 16.0), rmin);
  const float lambda = rho > 0.0 ? log2(rho) : -k_feedback_offset;
  const float code = clamp((lambda + k_feedback_offset) * k_feedback_steps, 0.0,
                           2.0 * k_feedback_offset * k_feedback_steps - 1.0);
  return uint(code);
}

// One slot's code into the table: once per wave where every active lane names the same slot.
void feedback_note(TextureFeedbackParams* params, uint slot, uint code) {
  if (slot == k_no_texture || slot >= params->slot_count) return;
  uint* out = (uint*)params->out;
  if (WaveActiveAllEqual(slot)) {
    const uint least = WaveActiveMin(code);
    if (WaveIsFirstLane()) InterlockedMin(out[slot], least);
  } else {
    InterlockedMin(out[slot], code);
  }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void texture_feedback_main(uint3 id : SV_DispatchThreadID) {
  TextureFeedbackParams* params = (TextureFeedbackParams*)g_feedback.params;
  if (id.x >= params->width || id.y >= params->height) return;
  const uint64_t value = ((uint64_t*)params->visibility)[id.y * params->width + id.x];
  if (value == 0) return;
  const uint id_bits = uint(value & 0xffffffffu);
  const uint pair = id_bits >> 8;
  const uint2 entry = ((uint2*)params->pairs)[pair];
  SceneGeometry geo;
  geo.clusters = params->clusters;
  geo.mesh = params->mesh;
  geo.triangles = params->triangles;
  geo.instances = params->instances;
  geo.attributes = params->attributes;
  geo.materials = params->materials;
  geo.cluster_materials = params->cluster_materials;
  geo.visible = 0;
  geo.normal_pool = 0;
  geo.eye = params->eye;
  InstanceDesc instance = ((InstanceDesc*)params->instances)[entry.x];
  const ResolveMaterial material = material_of(geo, instance, entry.y);
  const uint skip = k_material_water | k_material_ground_detail | k_feedback_terrain_layers;
  if ((material.flags & k_material_mipped) == 0 || (material.flags & skip) != 0) return;
  // The rest pose stands in for a deformed instance's (the header above says why).
  instance.deform = k_invalid_deform;
  float3 from_eye;
  TriangleFetch tri;
  ScreenBarycentrics at;
  const float2 center = float2(id.xy) + 0.5;
  const Surface surface =
      reconstruct_screen_kept(geo, instance, 0u, entry.y, id_bits & 0xffu, center,
                              params->view_proj, float3(0.0, 0.0, 0.0), params->width,
                              params->height, from_eye, tri, at);
  float2 duv_dx;
  float2 duv_dy;
  screen_uv_gradients_at(geo, tri, at, duv_dx, duv_dy);
  duv_dx = material_duv(material, duv_dx * material.uv_scale);
  duv_dy = material_duv(material, duv_dy * material.uv_scale);
  const uint slots[5] = {material.albedo_texture, material.metallic_roughness_texture,
                         material.normal_texture, material.occlusion_texture,
                         material.emissive_texture};
  for (uint s = 0; s < 5; ++s) {
    const uint slot = slots[s];
    if (slot == k_no_texture) continue;
    feedback_note(params, slot, feedback_code(duv_dx, duv_dy, slot));
  }
}

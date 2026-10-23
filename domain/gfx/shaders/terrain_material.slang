// Optional native surface evaluator. Included only in MATERIAL_TERRAIN shader modules; ordinary
// scenes retain their existing shaders. Layout: gfx::k_material_terrain_layers (112-byte entries).
#ifndef ENGINE_TERRAIN_MATERIAL_SLANG
#define ENGINE_TERRAIN_MATERIAL_SLANG

static const uint k_material_terrain_layers = 512u;
static const uint k_material_terrain_map = 1024u;

// A repeat phase from the frame's integer cell, never an absolute floating world position.
// The millimetre pitch is shared by every tile and mip. Take the unsigned magnitude's remainder
// and apply the sign explicitly: the negative-i64 probe exposed unsigned residue from `%` on the
// development compiler/device combination (native-terrain-materials.md). Local derivatives stay
// unwrapped, so crossing a repeat never selects a coarse mip.
float terrain_phase(int cell, float local, float from_eye, float residual, int64_t pitch) {
  const int64_t mm = int64_t(cell) * int64_t(64000);
  const uint64_t magnitude = uint64_t(mm < 0 ? -mm : mm);
  const uint64_t remainder = magnitude % uint64_t(pitch);
  const float signed_phase = (mm < 0 ? -float(remainder) : float(remainder)) / float(pitch);
  return frac(signed_phase + (local + residual + from_eye) * (1000.0 / float(pitch)));
}

float3 terrain_phase3(FrameEye eye, float3 point, float pitch_mm) {
  const int64_t pitch = int64_t(pitch_mm);
  return float3(terrain_phase(eye.cell.x, eye.local.x, point.x, eye.residual.x, pitch),
                terrain_phase(eye.cell.y, eye.local.y, point.y, eye.residual.y, pitch),
                terrain_phase(eye.cell.z, eye.local.z, point.z, eye.residual.z, pitch));
}

// Project the map's normal slopes onto the surface tangent plane. Flat maps contribute zero,
// including on oblique and negative-facing surfaces; each projection supplies its own axes.
float3 terrain_project_slope(float3 v, float scale, float3 n, float3 t, float3 b) {
  const float3 slope = (t * v.x + b * v.y) * (scale / max(v.z, 0.01));
  return slope - n * dot(n, slope);
}

float3 terrain_slope(ResolveMaterial layer, float2 uv, float2 dx, float2 dy, float3 n,
                     float3 t, float3 b, bool filtered) {
  if (layer.normal_texture == k_no_texture || layer.normal_scale == 0.0) return float3(0.0);
  const uint slot = slot_sampler(layer, layer.samplers_mr_normal, 16);
  float4 texel;
  if (filtered && (layer.flags & k_material_mipped) != 0)
    texel = g_textures[NonUniformResourceIndex(layer.normal_texture)].SampleGrad(
        g_samplers[NonUniformResourceIndex(slot)], uv, dx, dy);
  else
    texel = g_textures[NonUniformResourceIndex(layer.normal_texture)].SampleLevel(
        g_samplers[NonUniformResourceIndex(slot)], uv, 0);
  const float3 v = normal_map_vector(layer, texel);
  return terrain_project_slope(v, layer.normal_scale, n, t, b);
}

MaterialSample sample_terrain_material(SceneGeometry geo, ResolveMaterial root, float2 map_uv,
                                      float2 map_dx, float2 map_dy, float3 point, float3 n,
                                      float3 dp_dx, float3 dp_dy, float distance_m, bool filtered,
                                      out float3 shading_normal) {
  // All layouts use the same map frame. Reconstruct it from the position rather than half UVs,
  // whose quantization changes with a chunk's extent and would move a painted boundary at LOD.
  const int64_t x_mm = int64_t(geo.eye.cell.x) * int64_t(64000) - int64_t(asint(root.uv_transform.x));
  const int64_t z_mm = int64_t(geo.eye.cell.z) * int64_t(64000) - int64_t(asint(root.uv_transform.y));
  map_uv = (float2(float(x_mm), float(z_mm)) +
            (geo.eye.local.xz + geo.eye.residual.xz + point.xz) * 1000.0) * root.uv_transform.z;
  map_dx = dp_dx.xz * (1000.0 * root.uv_transform.z);
  map_dy = dp_dy.xz * (1000.0 * root.uv_transform.z);
  MaterialSample result = filtered ? sample_material_grad(root, map_uv, map_dx, map_dy)
                                   : sample_material(root, map_uv);
  float3 slope = float3(0.0);
  ResolveMaterial* table = (ResolveMaterial*)geo.materials;
  const uint first = asuint(root.uv_offset.z);
  const uint count = asuint(root.uv_offset.w);
  float3 axes = n * n;
  axes *= axes;
  axes /= max(axes.x + axes.y + axes.z, 1e-12);
  const float fade = 1.0 - smoothstep(root.normal_scale, root.occlusion_strength, distance_m);
  for (uint i = 0; i < count; ++i) {
    const ResolveMaterial layer = table[first + i];
    float coverage = layer.uv_offset.x;
    if (layer.occlusion_texture != k_no_texture) {
      const uint slot = slot_sampler(layer, layer.samplers_occlusion_emissive, 0);
      if (filtered && (layer.flags & k_material_mipped) != 0)
        coverage *= g_textures[NonUniformResourceIndex(layer.occlusion_texture)].SampleGrad(
            g_samplers[NonUniformResourceIndex(slot)], map_uv, map_dx, map_dy).r;
      else
        coverage *= g_textures[NonUniformResourceIndex(layer.occlusion_texture)].SampleLevel(
            g_samplers[NonUniformResourceIndex(slot)], map_uv, 0).r;
    }
    if (coverage <= 0.0) continue;
    float3 colour = float3(0.0);
    float roughness = 0.0;
    float3 bump = float3(0.0);
    const bool mapped = (layer.flags & k_material_terrain_map) != 0;
    if (mapped) {
      const MaterialSample sample = filtered ? sample_material_grad(layer, map_uv, map_dx, map_dy)
                                             : sample_material(layer, map_uv);
      colour = sample.albedo;
      roughness = sample.roughness;
      bump = terrain_slope(layer, map_uv, map_dx, map_dy, n, float3(1, 0, 0),
                           float3(0, 0, 1), filtered);
    } else if (fade <= 0.0) {
      colour = layer.uv_transform.xyz;
      roughness = layer.uv_transform.w;
    } else {
      const float3 phase = terrain_phase3(geo.eye, point, layer.uv_scale);
      const float scale = 1000.0 / layer.uv_scale;
      const float3 dx = dp_dx * scale;
      const float3 dy = dp_dy * scale;
      [unroll] for (uint axis = 0; axis < 3; ++axis) {
        const float2 uv = axis == 0 ? phase.zy : (axis == 1 ? phase.xz : phase.xy);
        const float2 du = axis == 0 ? dx.zy : (axis == 1 ? dx.xz : dx.xy);
        const float2 dv = axis == 0 ? dy.zy : (axis == 1 ? dy.xz : dy.xy);
        const float3 t = axis == 0 ? float3(0, 0, 1) : float3(1, 0, 0);
        const float3 b = axis == 1 ? float3(0, 0, 1) : float3(0, 1, 0);
        const MaterialSample sample = filtered ? sample_material_grad(layer, uv, du, dv)
                                               : sample_material(layer, uv);
        colour += axes[axis] * sample.albedo;
        roughness += axes[axis] * sample.roughness;
        bump += axes[axis] * terrain_slope(layer, uv, du, dv, n, t, b, filtered);
      }
      // The means are computed from linear decoded texels at scene preparation, so this policy
      // also works in a decoded/no-cache scene, whose images carry only one mip level.
      colour = lerp(layer.uv_transform.xyz, colour, fade);
      roughness = lerp(layer.uv_transform.w, roughness, fade);
      bump *= fade;
    }
    result.albedo = lerp(result.albedo, colour, coverage);
    result.roughness = lerp(result.roughness, roughness, coverage);
    slope = lerp(slope, bump, coverage);
  }
  result.metallic = 0.0;
  result.emissive = float3(0.0);
  result.detail_mask = 0.0;
  shading_normal = normalize(n + slope);
  return result;
}
#endif

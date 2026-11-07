// **Indirect light: the world-space cache's passes** (docs/subsystems/renderer.md, "Indirect light:
// the world-space cache"; roadmap R69 stage 1; gi_cache.h has the layout and the rules).
//
// Two entry points, both reading a view's own resolve block (`ResolveParams`), so they see the
// frame exactly as the resolve will:
//
//   - `gi_sample_main`, a thread per sampled pixel: one pixel in every `gi_stride` of the view's
//     visibility region, at an offset that turns with the frame, so every pixel is sampled once in
//     `gi_stride` frames. The pixel is decoded and reconstructed as fs_resolve does it, its cell
//     found at its level and at the next coarser one (inserted where missing), and one
//     cosine-weighted ray traced from it against the frame's top-level structure — the shadow
//     rays' own query (`shadow_ray_blocked`, this file includes the resolve's source), so the same
//     masks, micromaps and caster bit. A ray that escapes carries the environment in its direction
//     (`sky_environment`: the sky-view table above the horizon, the ground the sky's frame lights
//     below it — what the reference tracer's escaped rays see), one that hits carries nothing
//     (stage 1a: the sky's visibility; the bounce is 1b's). What is added is that radiance's ratio
//     to the luminance of the harmonics at the pixel's normal, in fixed point, to both cells' sums
//     for this frame.
//   - `gi_fold_main`, a thread per cell: the frame's sum into the running mean, and an old cell
//     freed (`k_gi_reset` clears them all, the cache's first frame).
//
// **Why one pass touches and traces.** A separate trace pass over a list of the frame's touched
// cells would trace from one representative pixel per cell, and which pixel wins a cell's slot in
// the list is the order threads reach an atomic — the picture would depend on it. Tracing from
// every sampled pixel and adding integers instead makes a cell's sum the same whatever the order.

#define RESOLVE_RAY_SHADOWS 1
// The resolve's two entry points are left out, as for the half pass (resolve_half_sun.slang).
#define RESOLVE_HALF_PASS 1
#include "visibility_resolve.slang"
#include "sampling.slang"

// The environment along `dir`: what an escaped ray carries.
float3 gi_environment(ResolveParams* params, float3 dir) {
  if (params->sky_params != 0) {
    SkyParams* sky = (SkyParams*)params->sky_params;
    return sky_environment(sky, (SkyFrame*)sky->frame, dir);
  }
  const float3 ground = ground_radiance(params->sky.rgb, params->ground.rgb, params->sun);
  return environment_radiance(params->sky.rgb, ground, dir);
}

// The ambient the resolve would take at `n` without the cache: the harmonics' luminance is the
// scale every sample is a ratio to.
float3 gi_harmonics(ResolveParams* params, float3 n) {
  if (params->sky_params != 0) {
    SkyParams* sky = (SkyParams*)params->sky_params;
    return sky_ambient((SkyFrame*)sky->frame, n);
  }
  const float3 ground = ground_radiance(params->sky.rgb, params->ground.rgb, params->sun);
  return ambient_radiance(params->sky.rgb, ground, n);
}

// **Stage 1b: the bounce** (`k_gi_bounce`). What a ray brings back: the environment where it
// escapes; where it hits, the hit point's radiance towards it — its material's albedo (the base
// colour at level 0, decoded through `hit_visible_entry` and the material table as the reference
// tracer decodes a hit) lit by the sun under one shadow ray, and by the cache's own value at the
// hit's cell, which is what carries the light of the bounces before it, so the cache converges to
// more than one bounce. A hit whose cells are empty takes the harmonics there, as the resolve would.
// The nearest surface is found with the shadow rays' masks decided as they decide them (a masked
// candidate by its mask at level 0, the opaque proxy's solid clusters, a caster as solid), kept
// nearest rather than first.
float3 gi_bounce(ResolveParams* params, float3 origin, float3 dir) {
  RayQuery<RAY_FLAG_NONE> query;
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = dir;
  ray.TMin = 0.0;
  ray.TMax = 1e30;
  const bool masked = (params->shadow_flags & k_shadow_alpha_masks) != 0;
  const uint flags =
      masked ? ((params->shadow_flags & k_shadow_micromap_2state) != 0 ? k_ray_flag_force_omm_2_state
                                                                        : 0u)
             : RAY_FLAG_FORCE_OPAQUE;
  query.TraceRayInline(g_scenes[NonUniformResourceIndex(params->scene)], flags, k_ray_mask_caster,
                       ray);
  MaskTables masks = no_mask_tables();
  masks.materials = params->materials;
  masks.cluster_materials = params->cluster_materials;
  masks.attributes = params->attributes;
  masks.fade = params->fade;
  while (query.Proceed()) {
    if (query.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) continue;
    bool is_static = false;
    const uint2 entry = hit_pair(
        params->visible, params->instances, params->mesh,
        hit_visible_entry(params->hit_entries, params->instances, query.CandidateGeometryIndex(),
                          query.CandidateInstanceID()),
        query.CandidateInstanceID(), is_static);
    InstanceDesc* instances = (InstanceDesc*)params->instances;
    const InstanceDesc instance = instances[entry.x];
    uint pair = 0u;
    if (params->fade != 0) {
      MeshDesc* meshes = (MeshDesc*)params->fade_meshes;
      pair = pair_of(instance, meshes[instance.mesh], entry.y);
    }
    if (params->shadow_coverage != 0 && entry.y < params->shadow_coverage_count) {
      uint* coverage = (uint*)params->shadow_coverage;
      const uint byte = (coverage[entry.y >> 2] >> ((entry.y & 3u) * 8u)) & 0xffu;
      if (byte >= params->shadow_opaque_coverage) {
        if (fade_keeps(masks, pair, g_fade_pixel)) query.CommitNonOpaqueTriangleHit();
        continue;
      }
    }
    if (candidate_keeps_counted(masks, params->clusters, params->triangles, instance, entry.y,
                                query.CandidatePrimitiveIndex(),
                                query.CandidateTriangleBarycentrics(), pair, g_fade_pixel)) {
      query.CommitNonOpaqueTriangleHit();
    }
  }
  if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return gi_environment(params, dir);
  const SceneGeometry geo = geometry_of(params);
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  const uint visible_index = hit_visible_entry(params->hit_entries, params->instances,
                                               query.CommittedGeometryIndex(),
                                               query.CommittedInstanceID());
  bool is_static = false;
  const uint2 entry = hit_pair(params->visible, params->instances, params->mesh, visible_index,
                               query.CommittedInstanceID(), is_static);
  const InstanceDesc instance = instances[entry.x];
  const TriangleFetch tri =
      fetch_triangle(geo, instance, visible_index, entry.y, query.CommittedPrimitiveIndex() & 0xff);
  const float2 bary = query.CommittedTriangleBarycentrics();
  const Surface hit =
      surface_at(geo, instance, tri, float3(1.0 - bary.x - bary.y, bary.x, bary.y), origin);
  const ResolveMaterial material = material_of(geo, instance, entry.y);
  const float2 surface_uv =
      hit.uv + terrain_uv_offset(instance, params->terrain_uv_x0_mm, params->terrain_uv_z0_mm,
                                 params->terrain_uv_per_mm);
  const MaterialSample sampled =
      sample_material(material, material_uv(material, surface_uv * material.uv_scale));
  float3 n = hit.normal;
  if (dot(n, hit.geometric) < 0.0) n = -n;
  BrdfSurface surface;
  surface.albedo = sampled.albedo;
  surface.normal = n;
  surface.view = -dir;
  surface.roughness = sampled.roughness;
  surface.metallic = sampled.metallic;
  float3 light = sampled.emissive;
  const float3 sun_dir = params->sun.xyz;
  if (dot(n, sun_dir) > 0.0) {
    const float bias = ray_offset(geo, instance, params->shadow_bias, params->shadow_bias_steps);
    if (!shadow_ray_blocked(params, k_shadow_sun, hit.position + hit.geometric * bias, sun_dir,
                            1e30)) {
      float3 sun = float3(params->sun.w);
      if (params->sky_params != 0) {
        SkyParams* sky = (SkyParams*)params->sky_params;
        sun = sky_light_at(sky, hit.position, sun_dir, sky->sun_illuminance.rgb);
      }
      light += brdf_direct(surface, sun_dir, sun);
    }
  }
  // The cache at the hit, and **nothing** where its cells are empty: only the picture's pixels
  // sample, so a surface out of every view (the wall behind the eye, a trunk's far side) has no
  // cells, and taking the open sky's harmonics there lit the inside of the box as if it had no
  // walls (the box went from FLIP 0.20 under `sky` to 0.44). Nothing there is the reference's
  // two-bounce answer for that hit — its own light and no more — so it errs dark, by the bounces
  // past the second off surfaces nobody sees.
  float3 ratio;
  float3 ambient = float3(0.0);
  if (gi_read(params->gi_cells, params->gi_count, params->eye, hit.position,
              gi_level(length(hit.position - params->camera.xyz), params->gi_cell_scale),
              gi_face(hit.normal), ratio)) {
    ambient = ratio * gi_luminance(gi_harmonics(params, n));
  }
  return light + ambient * sampled.albedo * (1.0 - sampled.metallic);
}

void gi_add(ResolveParams* params, uint slot, uint3 fixed) {
  Atomic<uint>* words = (Atomic<uint>*)params->gi_cells + slot * k_gi_cell_words;
  words[1].store(params->gi_frame);
  words[2].add(1u);
  words[3].add(fixed.x);
  words[4].add(fixed.y);
  words[5].add(fixed.z);
}

void gi_sample_pixel(ResolveParams* params, uint t) {
  const uint pixels = params->width * params->height;
  const uint stride = params->gi_stride;
  const uint offset = (params->gi_frame + pcg_hash(t)) % stride;
  const uint index = t * stride + offset;
  if (index >= pixels) return;
  const uint2 p = uint2(index % params->width, index / params->width);
  if (params->coverage != 0) {
    uint* tiles = (uint*)params->coverage;
    if (tiles[(p.y >> 5) * params->coverage_pitch + (p.x >> 5)] == 0) return;
  }
  uint64_t* visibility = (uint64_t*)params->visibility;
  const uint64_t value = visibility[p.y * params->width + p.x];
  if (value == 0) return;
  const uint id = uint(value & 0xffffffffu);
  const uint pair = id >> 8;
  uint2 entry;
  if (params->pairs != 0) {
    entry = ((uint2*)params->pairs)[pair];
  } else {
    entry = visible_entry(params->visible, entry_of_pair(params->pair_entries, pair));
  }
  const SceneGeometry geo = geometry_of(params);
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  const InstanceDesc instance = instances[entry.x];
  const uint visible_index =
      instance.deform == k_invalid_deform ? 0u : entry_of_pair(params->pair_entries, pair);
  float3 from_eye;
  const Surface s = reconstruct_screen(geo, instance, visible_index, entry.y, id & 0xff,
                                       float2(p) + 0.5, params->view_proj, params->camera.xyz,
                                       params->width, params->height, from_eye);
  const float base = gi_luminance(gi_harmonics(params, s.normal));
  if (!(base > 0.0)) return;
  // One cosine-weighted direction about the interpolated normal; one that the triangle's own plane
  // hides is mirrored through that plane, so the ray never starts into its own surface.
  uint rng = rng_seed(p, params->gi_frame, 0x9e3779b9u);
  float3 tangent;
  float3 bitangent;
  onb_from_normal(s.normal, tangent, bitangent);
  float3 dir = to_world(sample_cosine_hemisphere(rng_next2(rng)), tangent, bitangent, s.normal);
  const float below = dot(dir, s.geometric);
  if (below <= 0.0) dir = normalize(dir - 2.0 * below * s.geometric);
  const float ray_bias = ray_offset(geo, instance, params->shadow_bias, params->shadow_bias_steps);
  g_fade_pixel = p;
  const float3 origin = s.position + s.geometric * ray_bias;
  float3 radiance = float3(0.0);
  if ((params->gi_flags & k_gi_bounce) != 0u) {
    radiance = gi_bounce(params, origin, dir);
  } else if (!shadow_ray_blocked(params, k_shadow_sun, origin, dir, 1e30)) {
    radiance = gi_environment(params, dir);
  }
  const float3 ratio = min(radiance / base, float3(k_gi_ratio_max));
  const uint3 fixed = uint3(ratio * k_gi_fixed + 0.5);
  // Both the pixel's level and the next coarser, so a pixel whose level just changed as the eye
  // moved reads a warm cell one level up rather than the harmonics.
  const uint level = gi_level(length(s.position - params->camera.xyz), params->gi_cell_scale);
  const uint face = gi_face(s.normal);
  const int64_t3 mm = gi_point_mm(params->eye, s.position);
  for (uint l = level; l < min(level + 2u, k_gi_levels); ++l) {
    const int64_t size = gi_cell_mm(l);
    const int64_t3 c = int64_t3(terrain_floor_div(mm.x, size), terrain_floor_div(mm.y, size),
                                terrain_floor_div(mm.z, size));
    const uint slot = gi_insert(params->gi_cells, params->gi_count, gi_hash(c, l, face));
    if (slot != k_gi_no_slot) gi_add(params, slot, fixed);
  }
}

// One thread per sampled pixel of the view: `ceil(width * height / gi_stride)` of them.
[shader("compute")]
[numthreads(64, 1, 1)]
void gi_sample_main(uint3 id : SV_DispatchThreadID) {
  ResolveParams* params = (ResolveParams*)g_push.params;
  // The cache's first frame: the fold clears what the buffer held, so nothing is added before it.
  if (params->gi_cells == 0 || params->gi_stride == 0 || (params->gi_flags & k_gi_reset) != 0u) {
    return;
  }
  if ((params->shadow_flags & k_shadow_sun) == 0 || params->scene == k_no_scene) return;
  gi_sample_pixel(params, id.y * k_gi_dispatch_pitch * 64u + id.x);
}

// One thread per cell: the frame's samples into the mean, a cell nothing reached for
// `k_gi_max_age` frames freed, or with `k_gi_reset` every cell cleared.
[shader("compute")]
[numthreads(64, 1, 1)]
void gi_fold_main(uint3 id : SV_DispatchThreadID) {
  ResolveParams* params = (ResolveParams*)g_push.params;
  const uint cell = id.y * k_gi_dispatch_pitch * 64u + id.x;
  if (params->gi_cells == 0 || cell >= params->gi_count) return;
  uint* words = (uint*)params->gi_cells + cell * k_gi_cell_words;
  if ((params->gi_flags & k_gi_reset) != 0u) {
    for (uint i = 0u; i < k_gi_cell_words; ++i) words[i] = 0u;
    return;
  }
  if (words[0] == 0u) return;
  const uint n = words[2];
  if (n == 0u) {
    if (params->gi_frame - words[1] > k_gi_max_age) {
      for (uint i = 0u; i < k_gi_cell_words; ++i) words[i] = 0u;
    }
    return;
  }
  const float3 frame_mean = float3(words[3], words[4], words[5]) / (k_gi_fixed * float(n));
  const uint previous = words[7] >> 16;
  const uint total = min(previous + n, max(k_gi_max_samples, n));
  const float rate = float(n) / float(total);
  const float3 old_mean =
      float3(f16tof32(words[6] & 0xffffu), f16tof32(words[6] >> 16), f16tof32(words[7] & 0xffffu));
  const float3 mean = previous == 0u ? frame_mean : lerp(old_mean, frame_mean, rate);
  words[2] = 0u;
  words[3] = 0u;
  words[4] = 0u;
  words[5] = 0u;
  words[6] = f32tof16(mean.r) | (f32tof16(mean.g) << 16);
  words[7] = f32tof16(mean.b) | (min(total, 0xffffu) << 16);
}

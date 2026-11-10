// **What a ray that met the frame's geometry brings back** (docs/subsystems/water.md, "Shading";
// roadmap R129). The nearest surface along a ray against the frame's top-level structure, and that
// surface's radiance towards the ray: its emission, its base colour lit by the sun under one shadow
// ray (or none, as the caller says), and the ambient the resolve would give it.
//
// It is gi_update.slang's bounce (R69 stage 1b) copied rather than shared, because the world-space
// cache's files are R69's and a reflection wants a little more than a bounce: the texture level the
// ray's footprint selects rather than level 0, the BSDF's specular share of the ambient, and the
// cache's own ambient read through `scene_ambient` (which is the harmonics, to the bit, wherever
// the frame has no cache) rather than the bounce's "nothing where the cells are empty". The two
// decode a hit the same way: `hit_visible_entry`, `hit_pair`, `fetch_triangle`, `material_of`, as
// the reference path tracer decodes one.
//
// **The nearest surface, with the shadow rays' masks.** The query traces with the caster bit
// (`k_ray_mask_caster`), so it meets what a shadow ray meets and nothing the caster policy leaves
// untraced, and decides each non-opaque candidate as the shadow rays do — a masked pair by its mask
// at level 0, the opaque proxy's solid clusters, the two-state micromaps, a caster as solid — but
// keeps the nearest rather than the first. So the cone-culled and off-frustum casters, and the
// mirrored run's (renderer's water_reflect.h), are solid to it; the primary rays pass through them
// because they cull non-opaque geometry, which this ray must not. Without the masks bit it forces
// every candidate opaque, as the shadow rays do.
//
// **A terrain hit** (a root material with `k_material_terrain_layers`, only in the terrain modules)
// takes the ground's own material at the hit through `terrain_hit_material` (terrain_material.slang):
// the masks' weights there, at the mip the ray's footprint selects, blending the layers' mean colours
// and roughness — what a far pixel of it reads when the layers' maps are their means (renderer.md,
// "What a far pixel reads") — with the root under a thin cover, the snow's and the beach's responses,
// the porous ground's wetness and, in the water modules, the shore's wet band (S99). Not the
// stochastic sampling or the detail maps, which a reflection rippled by the waves would not show and
// would pay for. Until 2026-11-09 (R129's second step) it took the record's **first** layer's mean
// alone: the means were the cheap half of the rule and the weights the half it skipped, so the
// forest's tan bank reflected as its meadow (the POC's item 24). Every other hit reads its base
// colour at the mip its footprint selects (`k_material_mipped`), no normal map and no ground detail.
//
// Included by water_reflect.slang, inside visibility_resolve.slang with RESOLVE_RAY_SHADOWS and
// RESOLVE_WATER: the ray-traced water modules alone. Every other module is the bytes it was.

// The nearest surface a ray met: the query's committed hit.
struct SceneHit {
  uint instance_id;  // the top-level record's custom index: the scene instance
  uint geometry;     // its geometry index: the visible entry, through `hit_visible_entry`
  uint primitive;
  float2 bary;
  float t;
};

// The nearest surface along `ray`, with the shadow rays' rule for a non-opaque candidate (above).
// False when the ray met nothing before `ray.TMax`.
bool trace_scene_hit(ResolveParams* params, RayDesc ray, out SceneHit hit) {
  hit.instance_id = 0u;
  hit.geometry = 0u;
  hit.primitive = 0u;
  hit.bary = float2(0.0, 0.0);
  hit.t = 0.0;
  RayQuery<RAY_FLAG_NONE> query;
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
  if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
  hit.instance_id = query.CommittedInstanceID();
  hit.geometry = query.CommittedGeometryIndex();
  hit.primitive = query.CommittedPrimitiveIndex();
  hit.bary = query.CommittedTriangleBarycentrics();
  hit.t = query.CommittedRayT();
  return true;
}

// The hit's radiance towards the ray's origin (above). `footprint_m` is the ray's width at the hit,
// metres, which picks the base colour's mip; `shadow` asks one shadow ray towards the sun from the
// hit, and without it the sun reaches the hit unshadowed (the caller's half rate).
float3 shade_hit(ResolveParams* params, RayDesc ray, SceneHit hit, float footprint_m, bool shadow) {
  const SceneGeometry geo = geometry_of(params);
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  const uint visible_index =
      hit_visible_entry(params->hit_entries, params->instances, hit.geometry, hit.instance_id);
  bool is_static = false;
  const uint2 entry = hit_pair(params->visible, params->instances, params->mesh, visible_index,
                               hit.instance_id, is_static);
  const InstanceDesc instance = instances[entry.x];
  const TriangleFetch tri =
      fetch_triangle(geo, instance, visible_index, entry.y, hit.primitive & 0xff);
  const Surface surf = surface_at(geo, instance, tri,
                                  float3(1.0 - hit.bary.x - hit.bary.y, hit.bary.x, hit.bary.y),
                                  ray.Origin);
  const ResolveMaterial material = material_of(geo, instance, entry.y);
  float3 albedo = float3(0.0);
  float roughness = 1.0;
  float metallic = 0.0;
  float3 emissive = float3(0.0);
  bool sampled_here = true;
#ifdef MATERIAL_TERRAIN
  if ((material.flags & k_material_terrain_layers) != 0) {
    // The shore's wet band (S99) where the bank meets the water, as the resolve finds it for a
    // ground pixel: the first thing a reflection of a shore shows. Only in the water modules, which
    // are the ones that hold the scene's shore records.
    float shore_s = 0.0;
    float shore_film = 0.0;
#ifdef RESOLVE_WATER
    const WaterWetGround wet =
        water_ground_wetness(params, material, instance, surf.position, footprint_m);
    shore_s = wet.s;
    shore_film = (params->shadow_flags & k_shadow_wet_film_off) != 0 ? -1.0 : wet.film;
#endif
    MaterialSample ground;
    if (terrain_hit_material(geo, material, surf.position, footprint_m, ground, shore_s,
                             shore_film)) {
      albedo = ground.albedo;
      roughness = ground.roughness;
      metallic = ground.metallic;
      emissive = float3(0.0);
      sampled_here = false;
    }
  }
#endif
  if (sampled_here) {
    const float2 surface_uv =
        surf.uv + terrain_uv_offset(instance, params->terrain_uv_x0_mm, params->terrain_uv_z0_mm,
                                    params->terrain_uv_per_mm);
    const float2 uv = material_uv(material, surface_uv * material.uv_scale);
    // The mip the ray's footprint selects: the triangle's UV per metre (the square root of its UV
    // area over its area) times the footprint, along both axes, as a pixel that wide would.
    float2 duv_dx = float2(0.0, 0.0);
    float2 duv_dy = float2(0.0, 0.0);
    if ((material.flags & k_material_mipped) != 0 && geo.attributes != 0) {
      VertexAttributes* attributes = (VertexAttributes*)geo.attributes;
      const float2 t0 = decode_half2(attributes[tri.local[0]].uv_half2);
      const float2 e1 = decode_half2(attributes[tri.local[1]].uv_half2) - t0;
      const float2 e2 = decode_half2(attributes[tri.local[2]].uv_half2) - t0;
      const float area_uv = abs(e1.x * e2.y - e1.y * e2.x);
      const float area_m = length(cross(tri.p[1] - tri.p[0], tri.p[2] - tri.p[0]));
      const float width = footprint_m * sqrt(area_uv / max(area_m, 1e-12));
      duv_dx = material_duv(material, float2(width, 0.0) * material.uv_scale);
      duv_dy = material_duv(material, float2(0.0, width) * material.uv_scale);
    }
    const MaterialSample sampled = sample_material_grad(material, uv, duv_dx, duv_dy);
    albedo = sampled.albedo;
    roughness = sampled.roughness;
    metallic = sampled.metallic;
    emissive = sampled.emissive;
  }
  float3 n = surf.normal;
  if (dot(n, surf.geometric) < 0.0) n = -n;
  BrdfSurface surface;
  surface.albedo = albedo;
  surface.normal = n;
  surface.view = -ray.Direction;
  surface.roughness = roughness;
  surface.metallic = metallic;
  SkyParams* sky = (SkyParams*)params->sky_params;
  const bool has_sky = params->sky_params != 0;
  float3 light = emissive;
  const float3 sun_dir = params->sun.xyz;
  if (dot(n, sun_dir) > 0.0) {
    bool blocked = false;
    if (shadow) {
      const float bias = ray_offset(geo, instance, params->shadow_bias, params->shadow_bias_steps);
      blocked = shadow_ray_blocked(params, k_shadow_sun, surf.position + surf.geometric * bias,
                                   sun_dir, 1e30);
    }
    if (!blocked) {
      float3 sun = float3(params->sun.w);
      if (has_sky) sun = sky_light_at(sky, surf.position, sun_dir, sky->sun_illuminance.rgb);
      light += brdf_direct(surface, sun_dir, sun);
    }
  }
  // The ambient the resolve would give this surface: the sky's harmonics (or the stand-in's
  // hemisphere), through the world-space cache where the frame reads one.
  float3 ambient;
  if (has_sky) {
    ambient = sky_ambient((SkyFrame*)sky->frame, n);
  } else {
    const float3 ground = ground_radiance(params->sky.rgb, params->ground.rgb, params->sun);
    ambient = ambient_radiance(params->sky.rgb, ground, n);
  }
  ambient = scene_ambient(params->gi_cells, params->gi_flags, params->gi_count,
                          params->gi_cell_scale, params->eye, surf.position,
                          length(surf.position - params->camera.xyz), n, ambient);
  const float n_dot_v = max(dot(n, surface.view), 0.0);
  light += ambient * (albedo * (1.0 - metallic) +
                      brdf_env_specular(brdf_f0(albedo, metallic), roughness, n_dot_v));
  return light;
}

// **What is under the water is seen** (docs/subsystems/water.md, "What is under the water"; roadmap
// S86). The volume tier refracts the view at the wave normal and marches the body's depth map to
// the ground (`water_path_map`); the depth map knows only the ground, so a boulder in a pool, a
// sunken log or a tree's submerged base was cut off at the waterline. Here, after the march, one
// ray per water pixel along the refracted direction against the frame's top-level structure, from
// just under the surface **out to the ground distance the march found** (and no farther than the
// water lets light through: ln 255 extinction lengths of its clearest channel, past which 1 of 255
// remains). A hit nearer than the ground is something standing in the water: it is shaded as lying
// under it and replaces the ground's light; where the ray meets nothing, or meets the ground itself,
// the ground stays the march's answer and the terrain material's colour, to the bit.
//
// **The ground is the depth map's, never the ray's.** The frame's structures hold the ground's own
// mesh too, at the frame's cut, which may stand off the map's bilinear ground by its LOD's error. A
// hit on a terrain level's tile, or within `k_water_refract_ground_m` (plus 0.5% of the eye's
// distance to the surface) above the map's ground under it, is the ground: rejected, the march's
// answer kept. Below that margin an object's last few centimetres above the bed read as the bed.
//
// **The hit's light under the water.** Its material is decoded as `shade_hit` (hit_shade.slang)
// decodes one — the base colour at the mip the ray's footprint selects, a terrain hit's first
// layer's mean — and lit by the light the water lets down to it: the sun's beam refracted at the
// level surface (Schlick's share in, Snell's direction) and dimmed by Beer–Lambert over its path
// from the hit up to the surface, `depth / cos θl` (no caustics: the waves' focusing of the beam is
// not drawn); and the sky's light that entered (`(1 − F0)` of a level surface's) dimmed over 1.25
// times the depth, as the ground's is, on a face turned by its normal from a downwelling field
// (`(1 + n.y) / 2`: a uniform upper hemisphere's irradiance on a tilted face). Then the eye's path
// in the water from the surface to the hit, exactly as the ground's: Beer–Lambert, the
// transmission, and the light the water scatters towards the eye on the way (`water_volume` with a
// black bottom). `shade_hit` itself is not called because its light is one sum and the water
// dims its two parts differently; a request stands for a `shade_hit` that returns them apart.
//
// **The hit's sun shadow.** `shade_hit`'s own shadow ray from the hit would meet the water's
// surface over it, which casts as every surface does, and black out the sun under the water at the
// half of the pixels it traces at; so the shadow is the frame's own query asked where the sun's
// refracted ray from the hit leaves the surface, as the bottom's is, at every pixel with a hit (a
// shadow under the surface is softened by the waves anyway). Nothing under the water casts on the
// bottom: the bottom's query starts at the surface too.
//
// Included by visibility_resolve.slang with RESOLVE_RAY_SHADOWS and RESOLVE_WATER, after
// water_reflect.slang (whose include brings hit_shade.slang): the ray-traced water modules alone.

static const uint k_shadow_water_refract = 4096u;         // gfx::k_shadow_water_refract
static const float k_water_refract_min_m = 0.15;          // gfx::k_water_refract_min_m
static const float k_water_refract_ground_m = 0.08;       // gfx::k_water_refract_ground_m
static const float k_water_refract_extinctions = 5.5413;  // gfx::k_water_refract_extinctions

// The sun's beam under a level surface at `depth`: the irradiance `sun` arriving at the surface
// (normal to the beam), Schlick's share of it let in at the sun's angle, dimmed by Beer–Lambert over
// the refracted path up to the surface, `depth / cos θl`. `l` the beam's direction under the
// surface (towards the sun) and `cos_l` its cosine with the vertical. Mirrored by
// water_reference.h's `submerged_sun`.
float3 water_submerged_sun(float3 extinction, float3 sun_dir, float3 sun, float depth,
                           out float3 l, out float cos_l) {
  l = water_sun_under(sun_dir, cos_l);
  const float3 sun_in = sun * (1.0 - brdf_f_schlick(float3(k_water_f0), max(sun_dir.y, 0.0)).x);
  return sun_in * exp(-extinction * (max(depth, 0.0) / cos_l));
}

// The volume tier's light from under the surface with what stands in the water in it: `under` (the
// ground's, from `water_volume`) where the refracted ray meets nothing nearer than the ground, else
// the hit's (above). `sun` is the sun's irradiance at the surface unshadowed and `sun_surface` its
// shadow there, `ambient` a level surface's sky light: the values `water_pixel_volume` took.
float3 water_refraction(ResolveParams* params, SceneGeometry geo, WaterOptics o, WaterPath path,
                        Surface s, float3 local, bool has_map, WaterDepthMap map, float3 sun,
                        float sun_surface, float3 ambient, float ray_bias, float3 under) {
  if ((params->shadow_flags & k_shadow_water_refract) == 0 || params->scene == k_no_scene) {
    return under;
  }
  if (!(path.length >= k_water_refract_min_m)) return under;
  const float down = max(-path.t.y, 1e-3);
  const float clearest = max(min(o.extinction.x, min(o.extinction.y, o.extinction.z)), 1e-4);
  RayDesc ray;
  ray.Origin = s.position + path.t * (ray_bias / down);
  ray.Direction = path.t;
  ray.TMin = 0.0;
  ray.TMax = min(path.length, k_water_refract_extinctions / clearest);
  SceneHit hit;
  if (!trace_scene_hit(params, ray, hit)) return under;
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  if (instances[hit.instance_id].terrain != 0u) return under;  // a terrain level's tile
  const float t = hit.t + ray_bias / down;                       // from the surface
  const float depth = t * down;
  const float to_surface = length(s.position - params->camera.xyz);
  const float margin = k_water_refract_ground_m + 0.005 * to_surface;
  if (has_map) {
    const float2 at = local.xz + path.t.xz * t;
    if (water_depth_sample(map, at) - depth < margin) return under;
  } else if (t > path.length - margin / down) {
    return under;
  }

  // The hit, decoded as `shade_hit` decodes one.
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
    const TerrainRecord rec = ((TerrainRecord*)geo.materials)[terrain_record_index(material)];
    if (rec.layer_count != 0u) {
      ResolveMaterial* table = (ResolveMaterial*)geo.materials;
      const ResolveMaterial layer = table[rec.first_layer];
      albedo = terrain_layer_mean_colour(layer);
      roughness = terrain_layer_mean_roughness(layer);
      sampled_here = false;
    }
  }
#endif
  if (sampled_here) {
    const float2 surface_uv =
        surf.uv + terrain_uv_offset(instance, params->terrain_uv_x0_mm, params->terrain_uv_z0_mm,
                                    params->terrain_uv_per_mm);
    const float2 uv = material_uv(material, surface_uv * material.uv_scale);
    // The mip the ray's footprint selects: this view's pixel angle times the eye-to-surface-to-hit
    // path, times the triangle's UV per metre (shade_hit's rule).
    float2 duv_dx = float2(0.0, 0.0);
    float2 duv_dy = float2(0.0, 0.0);
    if ((material.flags & k_material_mipped) != 0 && geo.attributes != 0) {
      const float p11 = length(params->view_proj[1].xyz);
      const float angle = 2.0 / max(p11 * float(params->height), 1e-6);
      const float footprint_m = angle * (to_surface + t);
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

  // The light the water lets down to it (above).
  float3 light = emissive;
  float3 l;
  float cos_l;
  const float3 beam = water_submerged_sun(o.extinction, params->sun.xyz, sun, depth, l, cos_l);
  if (dot(n, l) > 0.0 && any(beam > float3(0.0))) {
    // Its shadow: where the sun's refracted ray from the hit leaves the surface.
    const float3 exit = surf.position + l * (depth / cos_l);
    const float vis = shadow_visibility(params, k_shadow_sun, 0u,
                                        float3(exit.x, s.position.y, exit.z), s.geometric,
                                        ray_bias, params->sun.xyz, 1e30);
    light += brdf_direct(surface, l, beam * vis);
  }
  const float3 sky_in = ambient * (1.0 - k_water_f0) *
                        exp(-o.extinction * (depth * k_water_diffuse_path)) * (0.5 + 0.5 * n.y);
  const float n_dot_v = max(dot(n, surface.view), 0.0);
  light += sky_in * (albedo * (1.0 - metallic) +
                     brdf_env_specular(brdf_f0(albedo, metallic), roughness, n_dot_v));

  // The eye's path in the water from the surface to the hit, as the ground's: dimmed, passed by
  // the transmission, and the water's own light gathered along it (`water_volume`, a black bottom).
  WaterPath p = path;
  p.length = t;
  p.depth = depth;
  const float3 scattered = water_volume(o, p, float3(0.0, 1.0, 0.0), float3(0.0),
                                        params->sun.xyz, sun, sun_surface, 0.0, ambient);
  return light * exp(-o.extinction * t) * o.transmission + scattered;
}

// **A smooth surface reflects the scene** (docs/subsystems/renderer.md, "Reflections on smooth
// surfaces"; roadmap R71). Until then only water traced a reflection ray, and every other surface
// took its specular share of the ambient: the sky's harmonics at its normal, a flat tint on
// curtain-wall glass at roughness 0.06 with no horizon, no neighbour and no glint.
//
// **Which pixels trace.** By the material's own numbers, never its name: a pixel of a material that
// is not water, not the ground's layers and not foliage, whose roughness (after the normal map's
// variance) is under `k_reflect_roughness_end`, and whose reflected share — the BSDF's own
// `brdf_env_specular` at this view, times the fade below — is over `k_reflect_min_share` in some
// channel. Glass, polished stone and metal alike.
//
// **One ray along the mirror direction, deterministic.** A frame is right by itself here, with no
// temporal pass behind it, so the form is the one that needs none: the view reflected off the
// shading normal, one ray, no jitter. It is right where the lobe is narrower than what the ray's
// hit would resolve: GGX at alpha = roughness^2 falls to half its peak at tan(theta_h) = 0.644
// alpha, so the reflected lobe's half width at half maximum is about 1.29 roughness^2 radians —
// **0.74 degrees at roughness 0.10**, where the ray replaces the ambient wholly, and **2.9 degrees
// at 0.20**, where it has faded out: in between the pixel is the ambient and the ray mixed
// linearly in roughness, so a surface over the band draws what it drew before. A glossy cone (a
// lobe of several rays, or one ray against a prefiltered hit) is the later stage: renderer.md says
// what it needs.
//
// **What the ray meets.** The frame's top-level structure with the shadow rays' masks
// (`trace_scene_hit`, hit_shade.slang), out to `k_surface_reflect_m`; where the frame keeps casters,
// the renderer adds the **eye's shell** (renderer's surface_reflect.h) so that what stands behind
// the eye within the shell's radius — the facade across the street, a car at the kerb — is in it.
// **The hit** is `shade_hit` as the water takes it, its sun shadow traced at every pixel (a mirror
// shows a checkerboard the water's ripples hide), then seen through the air along the ray. A
// reflection seen in a reflection takes the hit's own ambient and sky: no second ray.
//
// **A second ray attaches here** (R235, glass seen through): `surface_trace` takes any origin and
// direction and returns what the ray met, shaded and seen through the air, or the caller's miss.
// A refracted or transmitted view calls it with its own direction and its own miss.
//
// Included by visibility_resolve.slang with RESOLVE_RAY_SHADOWS: the ray-traced modules alone, and
// in them every line below sits behind `k_scene_reflect`, a specialization constant a pipeline sets
// only where its scene has such a material (`gfx::k_spec_scene_reflect`), so the driver folds the
// branch out of every other scene's resolve before it allocates a register.

#ifndef RESOLVE_WATER
#include "hit_shade.slang"  // the water modules have it through water_reflect.slang
#endif

static const uint k_shadow_surface_reflect = 1u << 23;             // gfx::k_shadow_surface_reflect
static const uint k_shadow_surface_reflect_diagnostic = 1u << 24;  // ..._diagnostic
static const float k_reflect_roughness_full = 0.10;   // gfx::k_reflect_roughness_full
static const float k_reflect_roughness_end = 0.20;    // gfx::k_reflect_roughness_end
static const float k_reflect_min_share = 1.0 / 256.0;  // gfx::k_reflect_min_share
static const float k_surface_reflect_diagnostic_scale = 1.0e9;
// The ray's reach, the water's (gfx::k_water_reflect_m): what a facade or a lake reflects past it is sky.
static const float k_surface_reflect_m = 4000.0;

// gfx::k_spec_scene_reflect: true only where the pipeline's scene has a material under
// `k_reflect_roughness_end` and the frame may trace (renderer's `resolve_specialization`).
[vk::constant_id(71)] const bool k_scene_reflect = false;

// The ray replaces the ambient by this much: 1 to `k_reflect_roughness_full`, 0 from
// `k_reflect_roughness_end`, linear between.
float surface_reflect_weight(float roughness) {
  return saturate((k_reflect_roughness_end - roughness) /
                  (k_reflect_roughness_end - k_reflect_roughness_full));
}

// Whether a material may trace at all: not water, not the ground's layers (each has its own
// reflection or none), not foliage (its branch is its own), and a roughness factor under the end.
// (512 is `k_material_terrain_layers`, which only the terrain modules declare.)
bool surface_reflect_material(ResolveMaterial material) {
  return (material.flags & (k_material_water | 512u)) == 0u &&
         !material_is_foliage(material) && material.albedo.w < k_reflect_roughness_end;
}

// What a ray from `origin` along `dir` meets within `k_surface_reflect_m`, shaded by `shade_hit` and
// seen through the air along the ray, or `miss`; `path_m` is the eye's distance to the origin, which
// with the ray's length gives its width at the hit (the texture level). `met` says which.
float3 surface_trace(ResolveParams* params, float3 origin, float3 dir, float path_m, float3 miss,
                     out bool met) {
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = dir;
  ray.TMin = 0.0;
  ray.TMax = k_surface_reflect_m;
  SceneHit hit;
  met = trace_scene_hit(params, ray, hit);
  if (!met) return miss;
  // The ray's width at the hit, as the water's: this view's pixel angle times the whole path.
  const float p11 = length(params->view_proj[1].xyz);
  const float angle = 2.0 / max(p11 * float(params->height), 1e-6);
  float3 radiance = shade_hit(params, ray, hit, angle * (path_m + hit.t), true);
  if (params->sky_params != 0) {
    SkyParams* sky_params = (SkyParams*)params->sky_params;
    float3 inscatter = float3(0.0);
    float transmittance = 1.0;
    sky_aerial(sky_params, dir, hit.t, inscatter, transmittance);
    radiance = radiance * transmittance + inscatter;
  }
  return radiance;
}

// The rays this frame's resolve traced off smooth surfaces, one u32 behind the capped lights'
// pixels (`light_cap_count`, the visible list's padding word), counted a wave at a time by a lane
// that is not a helper.
void surface_reflect_count(ResolveParams* params) {
  if (params->light_cap_count == 0 || IsHelperLane()) return;
  const uint n = WaveActiveCountBits(true);
  if (WaveIsFirstLane()) InterlockedAdd(*(uint*)(params->light_cap_count + 4u), n);
}

// The radiance the pixel's specular share takes in place of `ambient`: the scene along the mirror
// direction where the pixel traces, mixed with `ambient` by the fade; `ambient` itself where it
// does not. `share` is the pixel's `brdf_env_specular`. `traced` says whether a ray left.
float3 surface_reflection(ResolveParams* params, Surface s, BrdfSurface surface, float3 share,
                          float ray_bias, float3 ambient, out bool traced) {
  traced = false;
  if ((params->shadow_flags & k_shadow_surface_reflect) == 0u || params->scene == k_no_scene) {
    return ambient;
  }
  const float w = surface_reflect_weight(surface.roughness);
  if (!(w * max(share.r, max(share.g, share.b)) > k_reflect_min_share)) return ambient;
  const float3 r = reflect(-surface.view, surface.normal);
  // A shading normal tilted past the view can send the mirror under the surface; that pixel keeps
  // the ambient.
  if (!(dot(r, s.geometric) > 1.0e-4)) return ambient;
  float3 miss;
  if (params->sky_params != 0) {
    SkyParams* sky = (SkyParams*)params->sky_params;
    miss = sky_environment(sky, (SkyFrame*)sky->frame, r);
  } else {
    miss = environment_radiance(params->sky.rgb,
                                ground_radiance(params->sky.rgb, params->ground.rgb, params->sun),
                                r);
  }
  traced = true;
  surface_reflect_count(params);
  bool met = false;
  const float3 seen = surface_trace(params, s.position + s.geometric * ray_bias, r,
                                    length(s.position - params->camera.xyz), miss, met);
  if ((params->shadow_flags & k_shadow_surface_reflect_diagnostic) != 0u) {
    return float3(met ? 0.0 : 1.0, met ? 1.0 : 0.0, 0.0) * k_surface_reflect_diagnostic_scale;
  }
  return lerp(ambient, seen, w);
}

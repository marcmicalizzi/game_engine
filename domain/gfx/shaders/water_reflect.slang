// **The water reflects the scene** (docs/subsystems/water.md, "Shading"; roadmap R129). One ray per
// water pixel, from the reconstructed surface point lifted by `ray_offset` along +y, in the view
// reflected off the wave normal (so the reflection ripples as the sky's does), against the frame's
// top-level structure, out to `k_water_reflect_m`. What it meets is shaded by `shade_hit`
// (hit_shade.slang) and seen through the air between the surface and the hit; where it meets
// nothing the sky stands, as it always did. The result is what the branch's Fresnel term takes in
// place of the sky; the sun's glitter is untouched.
//
// **What the structure holds for it.** The frame's own cut, drawn and cast — and, behind every
// view's own volumes, the renderer's **mirrored caster volume** (renderer's water_reflect.h): the
// view's frustum reflected in the water's plane, above the plane and within `k_water_reflect_m`, so
// a tree over the eye's shoulder or a mountain above the frame's top edge, which the frame's cut
// does not hold, is built into the structure as a caster and met here. One plane a frame; a body at
// another level still traces against everything the structure holds.
//
// **The hit's sun shadow at half rate** (the default): one shadow ray from the hit at two pixels of
// every 2x2 block, those where `(x ^ y) & 1` is 0 — a checkerboard — and the sun unshadowed at the
// other two. `k_shadow_water_reflect_full` traces it at every pixel, `_unshadowed` at none.
//
// Included by visibility_resolve.slang with RESOLVE_RAY_SHADOWS and RESOLVE_WATER: the ray-traced
// water modules alone, so the plain modules and the plain water modules are the bytes they were.

#include "hit_shade.slang"

static const uint k_shadow_water_reflect = 64u;            // gfx::k_shadow_water_reflect
static const uint k_shadow_water_reflect_full = 128u;      // gfx::k_shadow_water_reflect_full
static const uint k_shadow_water_reflect_unshadowed = 256u;  // gfx::..._unshadowed
static const float k_water_reflect_m = 4000.0;             // gfx::k_water_reflect_m

// **The diagnostic** (`renderer.water.reflect_shadow` 3; water.md, "Reflections of the scene"): both
// rate bits at once, which no rate sets. Each water pixel's reflection is a colour saying what its
// rays met — red where the rippled ray met nothing, green where it met a surface, and blue added
// where the ray the same pixel would send off a flat mirror met one — scaled past any exposure, so
// a capture's channels read as bits. What tells the stripes' causes apart: a caster the chain or the
// budget dropped is a miss of both rays (red), the waves sending the ray off what the mirror holds
// is magenta, a hit is green or cyan.
static const float k_water_reflect_diagnostic_scale = 1.0e9;
bool water_reflect_diagnostic(uint flags) {
  const uint both = k_shadow_water_reflect_full | k_shadow_water_reflect_unshadowed;
  return (flags & both) == both;
}

// Whether this pixel's reflected hit asks its shadow ray (above).
bool water_reflect_shadowed(uint flags, uint2 pixel) {
  if ((flags & k_shadow_water_reflect_unshadowed) != 0) return false;
  if ((flags & k_shadow_water_reflect_full) != 0) return true;
  return ((pixel.x ^ pixel.y) & 1u) == 0u;
}

// The pixel's reflection along `r`, or `sky` — what the branch reflected before — where the frame
// asks for none, the ray leaves along the surface, or it meets nothing.
float3 water_reflection(ResolveParams* params, Surface s, float3 r, float ray_bias, float3 sky) {
  if ((params->shadow_flags & k_shadow_water_reflect) == 0 || params->scene == k_no_scene) {
    return sky;
  }
  if (!(r.y > 1.0e-4)) return sky;
  RayDesc ray;
  ray.Origin = s.position + float3(0.0, ray_bias, 0.0);
  ray.Direction = r;
  ray.TMin = 0.0;
  ray.TMax = k_water_reflect_m;
  SceneHit hit;
  if (water_reflect_diagnostic(params->shadow_flags)) {
    const bool rippled = trace_scene_hit(params, ray, hit);
    // The flat mirror's ray: the view reflected off the level surface, from the same point.
    const float3 d = normalize(s.position - params->camera.xyz);
    RayDesc flat = ray;
    flat.Direction = float3(d.x, -d.y, d.z);
    const bool mirror = flat.Direction.y > 1.0e-4 && trace_scene_hit(params, flat, hit);
    return float3(rippled ? 0.0 : 1.0, rippled ? 1.0 : 0.0, mirror ? 1.0 : 0.0) *
           k_water_reflect_diagnostic_scale;
  }
  if (!trace_scene_hit(params, ray, hit)) return sky;
  // The ray's width at the hit: this view's pixel angle (two over the projection's y scale and the
  // height; the eye is the frame's origin, so the view-projection's second row is the scale times a
  // unit vector) times the mirrored path's length, the eye to the surface and on to the hit.
  const float p11 = length(params->view_proj[1].xyz);
  const float angle = 2.0 / max(p11 * float(params->height), 1e-6);
  const float footprint_m = angle * (length(s.position - params->camera.xyz) + hit.t);
  float3 radiance = shade_hit(params, ray, hit, footprint_m,
                              water_reflect_shadowed(params->shadow_flags, g_fade_pixel));
  // The air between the surface and the hit, out of the frame's aerial table along the ray's
  // direction: the table is the eye's, which the surface is a few metres from, so this is the air
  // over a path the eye's own view along that direction would cross.
  if (params->sky_params != 0) {
    SkyParams* sky_params = (SkyParams*)params->sky_params;
    float3 inscatter = float3(0.0);
    float transmittance = 1.0;
    sky_aerial(sky_params, r, hit.t, inscatter, transmittance);
    radiance = radiance * transmittance + inscatter;
  }
  return radiance;
}

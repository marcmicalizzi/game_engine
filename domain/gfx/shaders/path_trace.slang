// The reference path tracer (docs/plan/04-renderer.md §4.8, docs/subsystems/renderer.md
// "Reference renderer"). Unidirectional path tracing with next-event estimation, importance
// sampling of the same BSDF the resolve evaluates, Russian roulette, and progressive
// accumulation — against the **same** acceleration structures the real-time frame built from its
// own visible list, so a reference ray shades the material the resolve would have shaded on that
// pixel and the comparison is about transport rather than about two different scenes.
//
// Everything the picture depends on other than the integrator is shared code: `material.slang`
// for the triangle fetch, the attributes, the tangent frame, the textures, the light falloffs,
// the sky and the display transform, `brdf.slang` for the BSDF, `sampling.slang` for the
// densities. The only thing this file owns is the transport.
//
// **What is sampled and what is not.** The sun and the analytic lights are delta lights, so they
// are estimated by next-event estimation alone and never hit by a BSDF ray; multiple importance
// sampling therefore weights nothing today, which is why `mis_power_heuristic` appears in the
// sampler and not here. **Emissive geometry is radiance only**: the emissive factor times the
// emissive texture (`sample_material`) is added when a ray lands on the surface, with weight 1 —
// the camera ray sees it directly and a bounce ray picks it up as light arriving from that
// direction — and nothing samples it directly. That is unbiased and needs no light list, and it
// converges slowly where a small bright emitter is most of a scene's light; next-event
// estimation towards emissive triangles, with the MIS weight against the BSDF ray it would then
// need, is the step after area lights and is not v1's. The sky is not sampled directly either —
// it is what a ray that escapes carries — so a scene lit only by the sky converges through BSDF
// sampling. **Occlusion maps are never applied here**: they are a baked stand-in for the
// hemisphere visibility this integrator computes by tracing, so multiplying them in would count
// the same occlusion twice.
//
// **The sky has the two roles the resolve gives it, on purpose.** A primary miss returns the
// background color the resolve writes for an empty pixel, in linear (`sky^2.2`, so the display
// transform gives the same bytes back), and an escaped secondary ray returns the environment the
// resolve's whole indirect light stands for: the sky's uniform radiance above the horizon and the
// lit ground below it (`environment_radiance`). They are not the same function,
// which is a property of the real-time path's sky model rather than of this file: mirroring it
// is what lets the two pictures be compared at all. `k_pt_uniform_sky` replaces both with a
// constant, which is the white furnace the integrator is checked with.

#include "brdf.slang"
#include "display.slang"
#include "ground_detail.slang"
#include "material.slang"
#include "sampling.slang"
#include "scene.slang"
#include "sky.slang"
#include "view_ray.slang"

[[vk::binding(3, 0)]] RaytracingAccelerationStructure g_scenes[];

// Mirrors gfx::PathTraceParams (352 bytes).
struct PathTraceParams {
  column_major float4x4 clip_to_ray;  // clip to the world direction of a primary ray (view_ray.slang)
  float4 camera;  // xyz the eye in the frame's space (PathTraceParams::eye)
  float4 sky;
  float4 sun;     // xyz normalized direction towards the light, w intensity
  float4 ground;  // rgb the ground's albedo below the horizon, as ResolveParams::ground; w unused
  uint64_t accum;   // float4[width * height]
  uint64_t output;  // uint[width * height] packed RGBA8
  uint64_t clusters;
  uint64_t mesh;
  uint64_t triangles;
  uint64_t instances;
  uint64_t attributes;
  uint64_t materials;
  uint64_t cluster_materials;
  uint64_t visible;
  uint64_t lights;
  uint width;
  uint height;
  uint light_count;
  uint scene;
  uint spp;
  uint sample_base;
  uint max_bounces;
  uint seed;
  uint flags;
  float ray_bias;  // world units along the geometric normal, off the surface (ray_offset)
  uint64_t coverage;  // uint[width * height]: 1 where a primary ray hit geometry
  uint background;    // packed RGBA8 of the sky, quantized on the CPU
  float ray_bias_steps;  // plus this many steps of the hit surface's own grid (ray_offset)
  uint64_t ground_detail;  // GroundDetail*, read for a k_material_ground_detail material; 0: none
  uint64_t sky_params;     // SkyParams* (sky.slang): the frame's physical sky; 0: the stand-in
  // The frame's origin (scene.slang): `camera` is the eye in its space, and so is every hit, light
  // and ray origin the tracer forms, as the frame's top-level structure is.
  FrameEye eye;
  // The scene grid's UV frame in millimetres, the resolve's: a world tile's material lookup places
  // its corner in it (scene.slang, `terrain_uv_offset`).
  int terrain_uv_x0_mm;
  int terrain_uv_z0_mm;
  uint terrain_uv_size_mm;
  uint terrain_uv_pad;
};

struct PathTracePush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<PathTracePush> g_push;

static const uint k_pt_uniform_sky = 1u;
static const uint k_pt_pixel_center = 2u;
static const uint k_pt_dither = 4u;  // the output encode dithers by the 8-bit output's 255 steps
static const uint k_pt_roulette_depth = 3u;

SceneGeometry geometry_of(PathTraceParams* params) {
  SceneGeometry geo;
  geo.clusters = params->clusters;
  geo.mesh = params->mesh;
  geo.triangles = params->triangles;
  geo.instances = params->instances;
  geo.attributes = params->attributes;
  geo.materials = params->materials;
  geo.cluster_materials = params->cluster_materials;
  geo.visible = params->visible;
  // The reference tracer has no deformed normal pool of its own: it traces the frame's own
  // structures, but it is a *reference*, and a morphed scene's comparison against it is the next
  // step rather than this one.
  geo.normal_pool = 0;
  geo.eye = params->eye;
  return geo;
}

// The radiance a ray that hit nothing carries. See the header note: the two roles are the
// resolve's, and the furnace flag collapses them into one. An escaped secondary ray sees the
// resolve's two halves: the sky above the horizon, and below it the ground the resolve's
// hemisphere term puts there — the scene's ground albedo under this frame's sun and sky.
//
// **With a sky** (`sky_params`) the two roles are the sky's: a primary miss sees what the resolve
// draws for an uncovered pixel (`sky_background`: the sky, the discs and the stars through the air),
// and an escaped secondary ray the sky's scattered light above the horizon and its ground below it
// (`sky_environment`), never the discs, which next-event estimation reaches.
float3 environment(PathTraceParams* params, float3 direction, bool primary) {
  if ((params->flags & k_pt_uniform_sky) != 0) return params->sky.rgb;
  if (params->sky_params != 0) {
    SkyParams* sky = (SkyParams*)params->sky_params;
    if (primary) return sky_background(sky, direction, sky->views[0].pixel.x);
    return sky_environment(sky, (SkyFrame*)sky->frame, direction);
  }
  if (primary) return pow(max(params->sky.rgb, 0.0), 2.2);
  const float3 ground = ground_radiance(params->sky.rgb, params->ground.rgb, params->sun);
  return environment_radiance(params->sky.rgb, ground, direction);
}

// Anything between `origin` and `t_max` along `direction`? One terminate-on-first-hit query, the
// same shape the resolve's shadow ray has.
bool occluded(PathTraceParams* params, float3 origin, float3 direction, float t_max) {
  if (t_max <= 0.0) return false;
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0;
  ray.TMax = t_max;
  const uint flags = RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
  RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
  query.TraceRayInline(g_scenes[NonUniformResourceIndex(params->scene)], flags, 0xff, ray);
  while (query.Proceed()) {
  }
  return query.CommittedStatus() != COMMITTED_NOTHING;
}

// Next-event estimation: the sun and every analytic light, each with one shadow ray, each
// weighted by the same `brdf_direct` the resolve calls. Delta lights, so no MIS weight. `bias` is
// how far `shadow_origin` sits off the surface (`ray_offset`), which a light's ray stops short by.
float3 next_event(PathTraceParams* params, BrdfSurface surface, float3 position,
                  float3 shadow_origin, float bias) {
  float3 direct = float3(0.0);
  SkyParams* sky = (SkyParams*)params->sky_params;
  const bool has_sky = params->sky_params != 0;
  if (params->sun.w > 0.0 && dot(surface.normal, params->sun.xyz) > 0.0 &&
      !occluded(params, shadow_origin, params->sun.xyz, 1e30)) {
    float3 sun = float3(params->sun.w);
    if (has_sky) sun = sky_light_at(sky, position, params->sun.xyz, sky->sun_illuminance.rgb);
    direct += brdf_direct(surface, params->sun.xyz, sun);
  }
  ResolveLight* lights = (ResolveLight*)params->lights;
  for (uint i = 0; i < params->light_count; ++i) {
    float3 light_dir;
    float3 radiance;
    float light_distance;
    if (!light_sample(lights[i], position, light_dir, radiance, light_distance)) continue;
    if (dot(surface.normal, light_dir) <= 0.0) continue;
    if (occluded(params, shadow_origin, light_dir, light_distance - bias)) continue;
    if (has_sky && lights[i].direction_type.w >= 1.5) {
      radiance = sky_light_at(sky, position, light_dir, radiance);
    }
    direct += brdf_direct(surface, light_dir, radiance);
  }
  return direct;
}

// One camera path. `rng` is the pixel's stream for this sample index, so the whole path is a
// function of (pixel, sample, seed) and nothing else. `hit_primary` says whether the first ray
// found geometry, which is what the tonemap needs to tell a background pixel from a dark one.
float3 trace_path(PathTraceParams* params, float3 origin, float3 direction, inout uint rng,
                  out bool hit_primary) {
  hit_primary = false;
  const SceneGeometry geo = geometry_of(params);
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  float3 radiance = float3(0.0);
  float3 throughput = float3(1.0);
  // With a sky, the air between the eye and the first hit: everything the path gathers from the hit
  // on crosses it, so it is applied once, at the end, as the resolve applies it to its pixel.
  float3 air_inscatter = float3(0.0);
  float air_transmittance = 1.0;
  for (uint bounce = 0;; ++bounce) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = 1e30;
    // The camera ray culls non-opaque geometry and every later ray forces opacity. The one
    // non-opaque geometry is a **shadow caster** (clas_records.slang): a cluster the cone test
    // kept out of the picture because it faces away from the camera. The first hit therefore
    // stays the real-time picture's, as ray_visibility.slang's does, while a bounce or a shadow
    // ray is stopped by it as by any other surface.
    const uint flags = bounce == 0 ? RAY_FLAG_CULL_NON_OPAQUE : RAY_FLAG_FORCE_OPAQUE;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(g_scenes[NonUniformResourceIndex(params->scene)], flags, 0xff, ray);
    while (query.Proceed()) {
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
      radiance += throughput * environment(params, direction, bounce == 0);
      break;
    }
    if (bounce == 0) {
      hit_primary = true;
      if (params->sky_params != 0) {
        const float t = query.CommittedRayT();
        sky_aerial((SkyParams*)params->sky_params, direction, t, air_inscatter, air_transmittance);
      }
    }

    // The hit, decoded exactly as ray_visibility.slang decodes one: the CLAS record's base
    // geometry index is the visible entry, the primitive index the triangle within the cluster.
    const uint visible_index = query.CommittedGeometryIndex();
    const uint2 entry = visible_entry(params->visible, visible_index);
    const InstanceDesc instance = instances[entry.x];
    const uint triangle = query.CommittedPrimitiveIndex() & 0xff;
    const TriangleFetch tri = fetch_triangle(geo, instance, visible_index, entry.y, triangle);
    const float2 bary = query.CommittedTriangleBarycentrics();
    const float3 b = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    Surface s = surface_at(geo, instance, tri, b, origin);

    const ResolveMaterial material = material_of(geo, instance, entry.y);
    // A world tile's UVs are from its corner: the resolve's offset puts them back in the grid's frame.
    const float2 surface_uv =
        s.uv + terrain_uv_offset(instance, params->terrain_uv_x0_mm, params->terrain_uv_z0_mm,
                                 params->terrain_uv_size_mm);
    const float2 uv = material_uv(material, surface_uv * material.uv_scale);
    const MaterialSample sampled = sample_material(material, uv);
    float3 shading_normal = s.normal;
    if (material.normal_texture != k_no_texture) {
      float3 tangent;
      float3 bitangent;
      if (tangent_frame(geo, tri, s.normal, tangent, bitangent)) {
        shading_normal = apply_normal_map(material, uv, s.normal, tangent, bitangent);
      }
    }
    // A shading normal that ended up behind the surface is turned to the side the ray came from:
    // a path must not be swallowed by an interpolated normal. The resolve turns it the same way
    // (since 2026-09-25; it used to let the BSDF return nothing and keep only its ambient), so the
    // two agree on thin two-sided geometry. A normal that merely leans away from the ray on the
    // ray's side is `brdf_eval`'s clamp's to handle, and ends the path below.
    if (dot(shading_normal, s.geometric) < 0.0) shading_normal = -shading_normal;

    // The ground's detail, the resolve's function of (x, z) (ground_detail.slang) **unfiltered**:
    // a ray has no pixel footprint, and the jittered samples a reference pixel averages are the
    // filter, so a converged reference is what the resolve's fade and roughness transfer stand for.
    float3 albedo = sampled.albedo;
    float roughness = sampled.roughness;
    if ((material.flags & k_material_ground_detail) != 0 && params->ground_detail != 0) {
      // The hit as its triangle's first corner and the rest from it, so the detail takes it from
      // its frame's origin without a world coordinate's rounding (gfx.md, "Far from the origin").
      const float3 from_corner = b.y * (tri.p[1] - tri.p[0]) + b.z * (tri.p[2] - tri.p[0]);
      const GroundShading ground = ground_detail_shade(
          params->ground_detail, tri.p[0], from_corner, shading_normal, float3(0.0, 0.0, 0.0),
          float3(0.0, 0.0, 0.0), sampled.detail_mask, albedo, roughness, false);
      shading_normal = ground.normal;
      albedo = ground.albedo;
      roughness = ground.roughness;
    }

    radiance += throughput * sampled.emissive;

    BrdfSurface surface;
    surface.albedo = albedo;
    surface.normal = shading_normal;
    surface.view = -direction;
    surface.roughness = roughness;
    surface.metallic = sampled.metallic;

    // Every ray from here leaves the surface by the resolve's own offset (`ray_offset`): the hit
    // is rebuilt off the mesh's grid by `fetch_triangle` exactly as a rasterized pixel is, so it
    // has the same gap to the float surface the structures hold, and the two integrators must
    // start their shadow rays alike or a comparison measures the offset.
    const float bias = ray_offset(geo, instance, params->ray_bias, params->ray_bias_steps);
    const float3 front = dot(s.geometric, surface.view) >= 0.0 ? s.geometric : -s.geometric;
    const float3 shadow_origin = s.position + front * bias;
    radiance += throughput * next_event(params, surface, s.position, shadow_origin, bias);

    if (bounce >= params->max_bounces) break;

    // ---- the next direction -------------------------------------------------------------
    const float n_dot_v = dot(surface.normal, surface.view);
    if (n_dot_v <= 0.0) break;
    float3 tangent;
    float3 bitangent;
    onb_from_normal(surface.normal, tangent, bitangent);
    const float3 v_local = to_local(surface.view, tangent, bitangent, surface.normal);
    const float alpha = brdf_alpha(surface.roughness);
    const float3 fresnel = brdf_f_schlick(brdf_f0(surface.albedo, surface.metallic), n_dot_v);
    const float p_specular =
        specular_probability(surface.albedo, surface.metallic, n_dot_v, fresnel);
    float3 l_local;
    if (rng_next(rng) < p_specular) {
      const float3 h = sample_ggx_vndf(v_local, alpha, rng_next2(rng));
      l_local = reflect(-v_local, h);
    } else {
      l_local = sample_cosine_hemisphere(rng_next2(rng));
    }
    if (l_local.z <= 0.0) break;
    const float pdf = bsdf_pdf(v_local, l_local, alpha, p_specular);
    if (!(pdf > 0.0)) break;
    const float3 next = to_world(l_local, tangent, bitangent, surface.normal);
    const float3 weight = brdf_eval(surface, next) * (l_local.z / pdf);
    throughput *= weight;
    if (all(throughput <= 0.0)) break;

    // Russian roulette after a few bounces: a short path is never cut, a dim long one usually is,
    // and the survivors carry the weight of the ones that were not, so the estimator is unbiased.
    if (bounce + 1 >= k_pt_roulette_depth) {
      const float survive = clamp(max(throughput.x, max(throughput.y, throughput.z)), 0.0, 0.95);
      if (rng_next(rng) >= survive) break;
      throughput /= max(survive, 1e-6);
    }

    origin = s.position + (dot(next, front) >= 0.0 ? front : -front) * bias;
    direction = next;
  }
  if (hit_primary && params->sky_params != 0) {
    radiance = radiance * air_transmittance + air_inscatter;
  }
  return radiance;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void path_trace_main(uint3 id : SV_DispatchThreadID) {
  PathTraceParams* params = (PathTraceParams*)g_push.params;
  if (id.x >= params->width || id.y >= params->height) return;
  const uint pixel = id.y * params->width + id.x;
  float4* accum = (float4*)params->accum;
  const float2 size = float2(float(params->width), float(params->height));
  const bool centered = (params->flags & k_pt_pixel_center) != 0;

  float3 sum = float3(0.0);
  bool covered = false;
  for (uint s = 0; s < params->spp; ++s) {
    uint rng = rng_seed(id.xy, params->sample_base + s, params->seed);
    const float2 jitter = centered ? float2(0.5, 0.5) : rng_next2(rng);
    // Pixel to clip space: row 0 is the top of the image, clip y points up — the same map
    // ray_visibility.slang uses, so a reference ray and a real-time ray start together — and to a
    // direction by the one function every reader of a pixel's direction calls, the resolve's
    // uncovered pixels included, so a primary miss looks where the resolve's sky does.
    const float2 ndc = float2((float(id.x) + jitter.x) / size.x * 2.0 - 1.0,
                              1.0 - (float(id.y) + jitter.y) / size.y * 2.0);
    const float3 origin = params->camera.xyz;
    const float3 direction = view_ray_direction(params->clip_to_ray, ndc);
    bool hit_primary = false;
    sum += trace_path(params, origin, direction, rng, hit_primary);
    covered = covered || hit_primary;
  }
  const float4 previous = params->sample_base == 0 ? float4(0.0) : accum[pixel];
  accum[pixel] = float4(previous.rgb + sum, previous.w + float(params->spp));
  uint* coverage = (uint*)params->coverage;
  const uint before = params->sample_base == 0 ? 0u : coverage[pixel];
  coverage[pixel] = before | (covered ? 1u : 0u);
}

// The accumulator to the bytes a UNORM target would hold, through the display transform the
// resolve ends on, so a reference image and a real-time image are in the same space.
//
// **A pixel nothing covered is written the way the resolve writes it**: the real-time path
// returns `sky` for an empty pixel as a *display-space* colour, straight into a UNORM target,
// while this accumulator holds linear radiance and would have to round-trip through
// `pow(sky, 2.2)` and back. That round trip is exact to about a part in ten million — and
// `sky.g` = 0.70 sits at 178.4999969 of 255, a hair under the byte boundary, so the round trip
// moves it to 179 and puts a one-byte difference on **every background pixel** of every
// comparison. A flat one-byte offset over most of a frame is not a rendering difference and FLIP
// should not be measuring it. Where something *did* cover the pixel the transform runs as usual,
// including on a silhouette pixel whose box filter mixed sky and surface, which is correct.
// Under `k_pt_uniform_sky` the sky is a radiance rather than a colour, so there is nothing to
// bypass and the transform runs everywhere.
[shader("compute")]
[numthreads(8, 8, 1)]
void tonemap_main(uint3 id : SV_DispatchThreadID) {
  PathTraceParams* params = (PathTraceParams*)g_push.params;
  if (id.x >= params->width || id.y >= params->height) return;
  const uint pixel = id.y * params->width + id.x;
  float4* accum = (float4*)params->accum;
  uint* coverage = (uint*)params->coverage;
  uint* output = (uint*)params->output;
  const float4 value = accum[pixel];
  float3 mean = value.w > 0.0 ? value.rgb / value.w : float3(0.0);
  // With a sky there is no flat background to hand over, and every pixel goes through the frame's
  // exposure and the transfer curve, as the resolve's do.
  const bool has_sky = params->sky_params != 0 && (params->flags & k_pt_uniform_sky) == 0;
  const bool background =
      coverage[pixel] == 0 && (params->flags & k_pt_uniform_sky) == 0 && !has_sky;
  if (background) {
    output[pixel] = params->background;
    return;
  }
  if (has_sky) {
    SkyParams* sky = (SkyParams*)params->sky_params;
    mean = sky_tone(sky, mean * sky_exposure((SkyFrame*)sky->frame));
  }
  // The resolve's output encode (display.slang), dithered at this pixel by the 8-bit output's steps
  // when the frame dithers, so a reference and a real-time picture carry the same noise.
  const uint steps = (params->flags & k_pt_dither) != 0 ? 255u : 0u;
  const float3 encoded = saturate(display_output(mean, id.xy, steps));
  const uint r = uint(encoded.x * 255.0 + 0.5);
  const uint g = uint(encoded.y * 255.0 + 0.5);
  const uint b = uint(encoded.z * 255.0 + 0.5);
  output[pixel] = r | (g << 8) | (b << 16) | (255u << 24);
}

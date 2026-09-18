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
// sampler and not here. Emissive geometry is added when a ray lands on it, with weight 1, for
// the same reason: nothing samples it directly yet. The sky is not sampled directly either — it
// is what a ray that escapes carries — so a scene lit only by the sky converges through BSDF
// sampling. Area and environment sampling, and the MIS that comes with them, are the next step.
//
// **The sky has the two roles the resolve gives it, on purpose.** A primary miss returns the
// background color the resolve writes for an empty pixel, in linear (`sky^2.2`, so the display
// transform gives the same bytes back), and an escaped secondary ray returns `sky_radiance`, the
// hemisphere term the resolve adds as its whole indirect light. They are not the same function,
// which is a property of the real-time path's sky model rather than of this file: mirroring it
// is what lets the two pictures be compared at all. `k_pt_uniform_sky` replaces both with a
// constant, which is the white furnace the integrator is checked with.

#include "brdf.slang"
#include "material.slang"
#include "sampling.slang"
#include "scene.slang"

[[vk::binding(3, 0)]] RaytracingAccelerationStructure g_scenes[];

// Mirrors gfx::PathTraceParams (240 bytes).
struct PathTraceParams {
  column_major float4x4 inv_view_proj;
  float4 camera;  // xyz eye position
  float4 sky;
  float4 sun;  // xyz normalized direction towards the light, w intensity
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
  float ray_bias;
  uint64_t coverage;  // uint[width * height]: 1 where a primary ray hit geometry
  uint background;    // packed RGBA8 of the sky, quantized on the CPU
  uint pad;
};

struct PathTracePush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<PathTracePush> g_push;

static const uint k_pt_uniform_sky = 1u;
static const uint k_pt_pixel_center = 2u;
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
  return geo;
}

// The radiance a ray that hit nothing carries. See the header note: the two roles are the
// resolve's, and the furnace flag collapses them into one.
float3 environment(PathTraceParams* params, float3 direction, bool primary) {
  if ((params->flags & k_pt_uniform_sky) != 0) return params->sky.rgb;
  if (primary) return pow(max(params->sky.rgb, 0.0), 2.2);
  return sky_radiance(params->sky.rgb, direction);
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
// weighted by the same `brdf_direct` the resolve calls. Delta lights, so no MIS weight.
float3 next_event(PathTraceParams* params, BrdfSurface surface, float3 position,
                  float3 shadow_origin) {
  float3 direct = float3(0.0);
  if (params->sun.w > 0.0 && dot(surface.normal, params->sun.xyz) > 0.0 &&
      !occluded(params, shadow_origin, params->sun.xyz, 1e30)) {
    direct += brdf_direct(surface, params->sun.xyz, float3(params->sun.w));
  }
  ResolveLight* lights = (ResolveLight*)params->lights;
  for (uint i = 0; i < params->light_count; ++i) {
    float3 light_dir;
    float3 radiance;
    float light_distance;
    if (!light_sample(lights[i], position, light_dir, radiance, light_distance)) continue;
    if (dot(surface.normal, light_dir) <= 0.0) continue;
    if (occluded(params, shadow_origin, light_dir, light_distance - params->ray_bias)) continue;
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
  for (uint bounce = 0;; ++bounce) {
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0;
    ray.TMax = 1e30;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(g_scenes[NonUniformResourceIndex(params->scene)], RAY_FLAG_FORCE_OPAQUE,
                         0xff, ray);
    while (query.Proceed()) {
    }
    if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
      radiance += throughput * environment(params, direction, bounce == 0);
      break;
    }
    if (bounce == 0) hit_primary = true;

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
    const float2 uv = s.uv * material.uv_scale;
    const MaterialSample sampled = sample_material(material, uv);
    float3 shading_normal = s.normal;
    if (material.normal_texture != k_no_texture) {
      float3 tangent;
      float3 bitangent;
      if (tangent_frame(geo, tri, s.normal, tangent, bitangent)) {
        shading_normal = apply_normal_map(material, uv, s.normal, tangent, bitangent);
      }
    }
    // A shading normal that ended up behind the surface is turned to the side the ray came from.
    // The resolve does not do this — it lets the BSDF return nothing and keeps only the ambient —
    // and the difference is real on thin two-sided geometry. It is the reference that is right:
    // a path must not be swallowed by an interpolated normal.
    if (dot(shading_normal, s.geometric) < 0.0) shading_normal = -shading_normal;

    radiance += throughput * sampled.emissive;

    BrdfSurface surface;
    surface.albedo = sampled.albedo;
    surface.normal = shading_normal;
    surface.view = -direction;
    surface.roughness = sampled.roughness;
    surface.metallic = sampled.metallic;

    const float3 front = dot(s.geometric, surface.view) >= 0.0 ? s.geometric : -s.geometric;
    const float3 shadow_origin = s.position + front * params->ray_bias;
    radiance += throughput * next_event(params, surface, s.position, shadow_origin);

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

    origin = s.position + (dot(next, front) >= 0.0 ? front : -front) * params->ray_bias;
    direction = next;
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
    // ray_visibility.slang uses, so a reference ray and a real-time ray start together.
    const float2 ndc = float2((float(id.x) + jitter.x) / size.x * 2.0 - 1.0,
                              1.0 - (float(id.y) + jitter.y) / size.y * 2.0);
    const float4 ahead = mul(params->inv_view_proj, float4(ndc, 0.5, 1.0));
    const float3 origin = params->camera.xyz;
    const float3 direction = normalize(ahead.xyz / ahead.w - origin);
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
  const float3 mean = value.w > 0.0 ? value.rgb / value.w : float3(0.0);
  const bool background =
      coverage[pixel] == 0 && (params->flags & k_pt_uniform_sky) == 0;
  if (background) {
    output[pixel] = params->background;
    return;
  }
  const float3 encoded = saturate(display_encode(mean));
  const uint r = uint(encoded.x * 255.0 + 0.5);
  const uint g = uint(encoded.y * 255.0 + 0.5);
  const uint b = uint(encoded.z * 255.0 + 0.5);
  output[pixel] = r | (g << 8) | (b << 16) | (255u << 24);
}

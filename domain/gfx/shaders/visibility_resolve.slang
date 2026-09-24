// Fullscreen resolve of the 64-bit visibility buffer (docs/plan/04-renderer.md §4.3 step 4):
// the first material pass. A pixel's id is `visible_index << 8 | triangle`, so the visible list
// gives back the (instance, cluster) pair the rasterizer drew; the cluster and the triangle lead
// to the three vertices, whose positions come off the 16-bit grid of that instance's mesh — or,
// for a deformed instance, out of the frame's deformed-vertex pool (MeshDesc, load_position) —
// and go through the instance's world matrix, exactly as the
// rasterizer moved them. Screen-space barycentrics against the pixel center, made
// perspective-correct with the vertices' clip-space w, reconstruct the surface position and
// interpolate the per-vertex normal and UV (octahedral snorm16 normals, half-float UVs, from
// geometry::VertexAttributes; without attributes the triangle plane gives a flat normal), and
// normals go through the instance's cofactor matrix so a non-uniform scale stays perpendicular.
// The cluster's material index plus the instance's `material_base` selects albedo, roughness,
// metallic, and emissive, and up to three optional textures from the bindless set: albedo, a
// glTF metallic-roughness map (G roughness, B metallic) that multiplies both factors, and a
// tangent-space normal map that perturbs the shading normal in a frame solved from the hit
// triangle's own world-space positions and UVs. The shared BSDF (brdf.slang) then shades a
// directional sun, N analytic point and spot lights, and a sky hemisphere. Debug modes show
// cluster colors, triangle shading, depth, the interpolated normal before any map, or UVs.
// Empty pixels (value 0: depth 0 is the far plane and id 0 never occurs there) take the sky
// color.
//
// **The material layer is shared, not copied** (docs/plan/04-renderer.md §4.8): the structs, the
// triangle fetch and the attribute interpolation, the tangent frame, the texture reads, the light
// falloffs, the sky and the display transform all live in `material.slang`, which the reference
// path tracer includes too. What is left in this file is the part that is the *resolve's*: the
// visibility-buffer decode, the screen-space reconstruction it needs because a rasterized pixel
// carries no barycentrics, the debug view modes, the Panini resample, and the shadow rays.
//
// Ray-traced shadows (docs/plan/04-renderer.md §4.4, "Shadows: RT for the sun and hero lights"):
// with RESOLVE_RAY_SHADOWS the fragment stage traces one terminate-on-first-hit ray per shadowing
// light, from the reconstructed surface point offset along the geometric normal, against the
// top-level structure in bindless slot `ResolveParams::scene`. That structure is the one the
// frame built from the same visible list the rasterizer drew from, so a pixel is shadowed by
// exactly the geometry the picture holds. Ambient and emissive are unaffected. The define exists
// because `g_scenes[]` is binding 3 of the bindless set, which a device without acceleration
// structures does not have: visibility_resolve_rt.slang is this file with the rays, and the
// plain module is what every other device draws with.

#include "brdf.slang"
#include "material.slang"
#include "scene.slang"

#ifdef RESOLVE_RAY_SHADOWS
[[vk::binding(3, 0)]] RaytracingAccelerationStructure g_scenes[];
#endif

// One resolve per view (docs/plan/04-renderer.md §4.6): the pass runs once for each view of a
// `renderer::ViewSet` with that view's viewport rectangle, so `SV_Position` is in the *target's*
// pixels and `view_x`/`view_y` take it back to a view-local pixel of that view's own region of
// the visibility buffer. A **Panini** view goes one step further: its region holds a wider
// rectilinear picture, and `panini_x`/`source_x`/`panini_d` map each output pixel back into it.
//
// Read through a device address; mirrors gfx::ResolveParams (256 bytes).
struct ResolveParams {
  float4 sky;     // rgb, shown for empty pixels and used as the hemisphere ambient
  float4 sun;     // xyz normalized direction towards the light, w intensity
  float4 camera;  // xyz position
  column_major float4x4 view_proj;
  uint64_t visibility;         // uint64_t[width * height]
  uint64_t clusters;           // ClusterDesc[]
  uint64_t mesh;               // MeshDesc[]: the positions and each mesh's grid
  uint64_t triangles;          // uint[]: packed local indices
  uint64_t materials;          // ResolveMaterial[]
  uint64_t cluster_materials;  // uint[cluster_count]: material index per cluster
  uint64_t attributes;         // VertexAttributes[] parallel to vertices; 0 = flat shading
  uint64_t lights;             // ResolveLight[light_count]
  uint width;
  uint height;
  uint mode;  // 0 cluster colors, 1 with triangle shade, 2 depth, 3 shaded, 4 normals, 5 uvs,
              // 6 the sun's shadow alone
  uint light_count;
  uint64_t instances;  // InstanceDesc[]
  uint64_t visible;    // uint2[]: the cull pass's visible list; 0 reads {0, visible_index}
  uint scene;          // bindless slot of the top-level structure; 0xffffffff for none
  uint shadow_flags;   // bit 0 the sun casts, bit 1 the analytic lights cast
  float shadow_bias;   // world units along the geometric normal, off the surface
  uint view_x;         // this view's rectangle of the color target
  uint view_y;
  uint out_width;
  uint out_height;
  float panini_d;      // Panini parameter; the resample is off when panini_x is 0
  float panini_x;      // the picture's half-width in image units
  float source_x;      // the rectilinear source's half-width, tan(half the horizontal fov)
  uint sky_is_clear;   // 1: the pass cleared the target to `sky`, so an empty pixel is discarded
  uint pad;
  uint64_t coverage;    // uint[] per 32x32 tile of this view's region; 0: no mask, read every word
  uint coverage_pitch;  // tiles per row of that mask
  uint pad2;
  uint64_t normal_pool;  // deformed normals, parallel to the deformed-vertex pool; 0 for none
  uint64_t pad3;
};

struct ResolvePush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<ResolvePush> g_push;

static const uint k_no_scene = 0xffffffffu;
static const uint k_shadow_sun = 1u;
static const uint k_shadow_lights = 2u;

struct FullscreenOut {
  float4 position : SV_Position;
};

// What an **empty** pixel is. At the owner's 11520 x 2160 the pass is bandwidth-bound and the
// sky is most of a frame's pixels whenever the camera looks at any of it — the read of the
// visibility word cannot be avoided (it is how the pixel is known to be empty), but the color
// write can, because the pass clears the whole target to `sky` before it draws and so the bytes
// the shader would write are already there. `sky_is_clear` is the caller saying it cleared to
// exactly this value; the fragment is then killed and the ROP does nothing. A caller that
// cleared to something else leaves the flag at zero and the sky is written as it always was.
float4 sky_pixel(ResolveParams* params) {
  if (params->sky_is_clear != 0) discard;
  return params->sky;
}

[shader("vertex")]
FullscreenOut vs_fullscreen(uint vertex_id : SV_VertexID) {
  // One triangle covering the viewport: (-1,-1), (3,-1), (-1,3).
  FullscreenOut out;
  const float x = vertex_id == 1 ? 3.0 : -1.0;
  const float y = vertex_id == 2 ? 3.0 : -1.0;
  out.position = float4(x, y, 0.0, 1.0);
  return out;
}

// One color per (instance, cluster), so two instances of a mesh are told apart in the debug view.
float3 id_color(uint2 entry) {
  const uint h = entry.y * 2654435761u ^ entry.x * 2246822519u;
  return float3((h >> 0) & 255, (h >> 8) & 255, (h >> 16) & 255) / 255.0;
}

// The scene addresses this block reads a hit through, in the shape material.slang takes them.
SceneGeometry geometry_of(ResolveParams* params) {
  SceneGeometry geo;
  geo.clusters = params->clusters;
  geo.mesh = params->mesh;
  geo.triangles = params->triangles;
  geo.instances = params->instances;
  geo.attributes = params->attributes;
  geo.materials = params->materials;
  geo.cluster_materials = params->cluster_materials;
  geo.visible = params->visible;
  geo.normal_pool = params->normal_pool;
  return geo;
}

// Where an output pixel of a Panini view reads its rectilinear source (docs/plan/04-renderer.md
// §4.6, experiment E9). The forward map of the Panini projection of parameter d takes a direction
// at azimuth theta and elevation phi from the view axis to
//
//     X = (d + 1) sin(theta) / (d + cos(theta)) = S sin(theta),   S = (d + 1) / (d + cos(theta))
//     Y = S tan(phi)
//
// and the rectilinear source puts the same direction at (tan(theta), tan(phi) / cos(theta)). This
// inverts the first equation — with k = X / (d + 1) it is sin(theta) - k cos(theta) = k d, whose
// solution is theta = atan(k) + asin(k d / sqrt(1 + k^2)) — and forward-maps the result. The
// vertical parameter cancels: the source's normalized y is ndc.y / (S cos(theta)), because the
// output's half-height and the source's are the same tan(fov_y / 2) at theta = 0 and the ratio of
// the two vertical scales is all that is left. At d = 0 the whole map is the identity to within
// the round trip through atan and tan, which is what the d = 0 test measures.
float2 panini_source_ndc(ResolveParams* params, float2 ndc) {
  const float d = params->panini_d;
  const float k = ndc.x * params->panini_x / (d + 1.0);
  const float root = sqrt(1.0 + k * k);
  const float theta = atan(k) + asin(clamp(k * d / root, -1.0, 1.0));
  const float cos_theta = cos(theta);
  const float s = (d + 1.0) / max(d + cos_theta, 1e-6);
  return float2(tan(theta) / params->source_x, ndc.y / max(s * cos_theta, 1e-6));
}

// Is anything between `origin` and a light `t_max` away in direction `direction`? One ray query
// with the terminate-on-first-hit flag, which is what a shadow ray wants: any hit ends the
// traversal and no hit shader runs. False without the shadow bit, without a scene, when the light
// is below the surface (the BSDF returns nothing there anyway), or when the bias has already
// eaten the whole distance to the light.
bool shadow_ray_blocked(ResolveParams* params, uint bit, float3 origin, float3 direction,
                        float t_max) {
#ifdef RESOLVE_RAY_SHADOWS
  if ((params->shadow_flags & bit) == 0 || params->scene == k_no_scene || t_max <= 0.0) {
    return false;
  }
  const uint flags = RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH;
  RayDesc ray;
  ray.Origin = origin;
  ray.Direction = direction;
  ray.TMin = 0.0;
  ray.TMax = t_max;
  RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
  query.TraceRayInline(g_scenes[NonUniformResourceIndex(params->scene)], flags, 0xff, ray);
  while (query.Proceed()) {
  }
  return query.CommittedStatus() != COMMITTED_NOTHING;
#else
  return false;
#endif
}

[shader("fragment")]
float4 fs_resolve(FullscreenOut in) : SV_Target {
  ResolveParams* params = (ResolveParams*)g_push.params;
  // SV_Position is in the target's pixels; this view's rectangle starts at (view_x, view_y).
  // Without a resample the picture's extent *is* the source's, so `out_width` and `out_height` are
  // read only where they can differ — which is why a caller that draws one rectilinear view fills
  // neither and gets what it always did.
  const uint2 out_pixel = uint2(in.position.xy) - uint2(params->view_x, params->view_y);
  const bool resample = params->panini_x > 0.0;
  const uint out_width = resample ? params->out_width : params->width;
  const uint out_height = resample ? params->out_height : params->height;
  if (out_pixel.x >= out_width || out_pixel.y >= out_height) return sky_pixel(params);
  // Where in this view's visibility region the output pixel reads. Without a resample that is the
  // pixel's own centre; with one it is wherever the Panini map sends it, which is also the point
  // the barycentrics below are taken at, so the reconstruction stays sub-pixel accurate.
  float2 source = float2(out_pixel) + 0.5;
  if (resample) {
    const float2 ndc = float2((source.x / float(out_width)) * 2.0 - 1.0,
                              1.0 - (source.y / float(out_height)) * 2.0);
    const float2 src_ndc = panini_source_ndc(params, ndc);
    source = float2((src_ndc.x * 0.5 + 0.5) * float(params->width),
                    (0.5 - src_ndc.y * 0.5) * float(params->height));
    if (source.x < 0.0 || source.y < 0.0) return sky_pixel(params);
  }
  const uint2 pixel = uint2(source);
  if (pixel.x >= params->width || pixel.y >= params->height) return sky_pixel(params);
  // A whole 32x32 tile with nothing in it: the visibility word is not read at all, which is the
  // fixed cost the discard above could not touch. The mask is the Hi-Z build's by-product and is
  // exact — the pass that wrote it read every word of the tile — so a tile it calls empty holds
  // no surface and a tile it calls covered is resolved exactly as before. Zero means no mask.
  if (params->coverage != 0) {
    uint* tiles = (uint*)params->coverage;
    if (tiles[(pixel.y >> 5) * params->coverage_pitch + (pixel.x >> 5)] == 0) {
      return sky_pixel(params);
    }
  }
  uint64_t* visibility = (uint64_t*)params->visibility;
  const uint64_t value = visibility[pixel.y * params->width + pixel.x];
  if (value == 0) return sky_pixel(params);
  const uint id = uint(value & 0xffffffffu);
  const float depth = asfloat(uint(value >> 32));
  const uint2 entry = visible_entry(params->visible, id >> 8);
  switch (params->mode) {
    case 0: return float4(id_color(entry), 1.0);
    case 1:
      return float4(id_color(entry) * (0.7 + 0.3 * frac(float(id & 0xff) * 0.6180339887)), 1.0);
    case 2: return float4(depth, depth, depth, 1.0);
    default: break;
  }
  const SceneGeometry geo = geometry_of(params);
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  const InstanceDesc instance = instances[entry.x];
  const Surface s = reconstruct_screen(geo, instance, id >> 8, entry.y, id & 0xff, source,
                                       params->view_proj, params->camera.xyz, params->width,
                                       params->height);
  if (params->mode == 4) return float4(s.normal * 0.5 + 0.5, 1.0);
  if (params->mode == 5) return float4(frac(s.uv), 0.0, 1.0);

  const ResolveMaterial material = material_of(geo, instance, entry.y);
  const float2 uv = s.uv * material.uv_scale;
  const MaterialSample sampled = sample_material(material, uv);
  const float3 albedo = sampled.albedo;
  float3 shading_normal = s.normal;
  if (material.normal_texture != k_no_texture) {
    float3 tangent;
    float3 bitangent;
    // The triangle a second time rather than one held across the reconstruction, which is what
    // this block always did: the frame is needed only where a normal map is.
    const TriangleFetch tri = fetch_triangle(geo, instance, id >> 8, entry.y, id & 0xff);
    if (tangent_frame(geo, tri, s.normal, tangent, bitangent)) {
      shading_normal = apply_normal_map(material, uv, s.normal, tangent, bitangent);
    }
  }
  BrdfSurface surface;
  surface.albedo = albedo;
  surface.normal = shading_normal;
  surface.view = normalize(params->camera.xyz - s.position);
  surface.roughness = sampled.roughness;
  surface.metallic = sampled.metallic;

  // Shadow rays leave from the surface offset along the geometric normal, so that the quantized
  // position the resolve reconstructs cannot start the ray inside the float positions the
  // acceleration structure was built from.
  const float3 shadow_origin = s.position + s.geometric * params->shadow_bias;

  // The shadow view (ResolveMode::Shadow): the sun's shadow query and nothing else, in three flat
  // values a test or a person can count. White where the sun reaches the surface, black where the
  // shadow ray is blocked, and mid grey where the surface faces away from the sun, so no ray is
  // traced. It asks exactly the question the shaded view below asks, with the same normal and the
  // same origin, so a pixel that is black here is a pixel the sun term is missing from there.
  if (params->mode == 6) {
    if (dot(surface.normal, params->sun.xyz) <= 0.0) return float4(0.5, 0.5, 0.5, 1.0);
    return shadow_ray_blocked(params, k_shadow_sun, shadow_origin, params->sun.xyz, 1e30)
               ? float4(0.0, 0.0, 0.0, 1.0)
               : float4(1.0, 1.0, 1.0, 1.0);
  }

  // The sun: one direction, one intensity, no falloff; a hit anywhere along the ray removes it.
  float3 color = float3(0.0);
  if (dot(surface.normal, params->sun.xyz) > 0.0 &&
      !shadow_ray_blocked(params, k_shadow_sun, shadow_origin, params->sun.xyz, 1e30)) {
    color = brdf_direct(surface, params->sun.xyz, float3(params->sun.w));
  }
  // The analytic lights: inverse square windowed to zero at the radius, spots by their cone, and
  // a shadow ray that stops short of the light itself.
  ResolveLight* lights = (ResolveLight*)params->lights;
  for (uint i = 0; i < params->light_count; ++i) {
    float3 light_dir;
    float3 radiance;
    float light_distance;
    if (!light_sample(lights[i], s.position, light_dir, radiance, light_distance)) continue;
    if (dot(surface.normal, light_dir) > 0.0 &&
        shadow_ray_blocked(params, k_shadow_lights, shadow_origin, light_dir,
                           light_distance - params->shadow_bias)) {
      continue;
    }
    color += brdf_direct(surface, light_dir, radiance);
  }
  // The sky hemisphere is the whole indirect term until the lighting passes exist: the diffuse
  // albedo takes it by normal elevation, and a Fresnel-weighted sliver of it at the view angle
  // keeps metals, which have no diffuse lobe, from going black. The path tracer picks the same
  // function up as the radiance an escaped ray carries, which is what makes the two comparable.
  const float3 ambient = sky_radiance(params->sky.rgb, surface.normal);
  const float n_dot_v = max(dot(surface.normal, surface.view), 0.0);
  color += ambient * (albedo * (1.0 - surface.metallic) +
                      brdf_f_schlick(brdf_f0(albedo, surface.metallic), n_dot_v));
  color += sampled.emissive;
  return float4(display_encode(color), 1.0);  // linear to display on a UNORM swapchain
}

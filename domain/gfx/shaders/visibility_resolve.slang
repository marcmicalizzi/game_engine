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
#include "scene.slang"

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[];
[[vk::binding(2, 0)]] SamplerState g_samplers[];
#ifdef RESOLVE_RAY_SHADOWS
[[vk::binding(3, 0)]] RaytracingAccelerationStructure g_scenes[];
#endif

// Mirrors gfx::ResolveMaterial (64 bytes).
struct ResolveMaterial {
  float4 albedo;    // rgb linear, w roughness
  float4 emissive;  // rgb linear, w metallic
  uint albedo_texture;  // bindless sampled-image slot; 0xffffffff for none
  uint sampler;         // bindless sampler slot
  float uv_scale;
  uint flags;
  uint metallic_roughness_texture;  // glTF packing: G roughness, B metallic
  uint normal_texture;              // tangent space, UNORM, remapped to -1..1
  float normal_scale;               // scales the map's xy before it is normalized
  uint pad;
};

// Mirrors gfx::ResolveLight (64 bytes).
struct ResolveLight {
  float4 position_radius;  // xyz world position, w radius of influence
  float4 color_intensity;  // rgb linear color, w radiant intensity scale
  float4 direction_type;   // xyz spot direction (towards the lit surface), w 0 point, 1 spot
  float4 spot;             // x cos inner, y cos outer, zw unused
};

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
  uint mode;  // 0 cluster colors, 1 with triangle shade, 2 depth, 3 shaded, 4 normals, 5 uvs
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
  uint2 pad;
};

struct ResolvePush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<ResolvePush> g_push;

static const uint k_no_texture = 0xffffffffu;
static const uint k_no_scene = 0xffffffffu;
static const uint k_shadow_sun = 1u;
static const uint k_shadow_lights = 2u;

struct FullscreenOut {
  float4 position : SV_Position;
};

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

float edge(float2 a, float2 b, float2 c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

float sign_or_one(float v) { return v < 0.0 ? -1.0 : 1.0; }

// Inverse of geometry::encode_normal_oct: two snorm16 in a uint.
float3 decode_normal_oct(uint packed) {
  const float x = float(int(packed << 16) >> 16) / 32767.0;
  const float y = float(int(packed) >> 16) / 32767.0;
  float3 n = float3(x, y, 1.0 - abs(x) - abs(y));
  if (n.z < 0.0) n.xy = float2((1.0 - abs(y)) * sign_or_one(x), (1.0 - abs(x)) * sign_or_one(y));
  return normalize(n);
}

float2 decode_half2(uint packed) { return float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16)); }

struct Surface {
  float3 position;
  float3 normal;     // shading normal: the interpolated vertex normals, or the triangle plane
  float3 geometric;  // the triangle's own plane normal, facing the camera: what a ray offsets by
  float2 uv;
};

// Position, normal, and UV, in world space, of the triangle the pixel's id names: the instance
// and the cluster of visible entry `id >> 8`, and the triangle `id & 0xff` within that cluster.
Surface reconstruct(ResolveParams* params, InstanceDesc instance, uint2 entry, uint id,
                    float2 pixel_center) {
  ClusterDesc* clusters = (ClusterDesc*)params->clusters;
  uint* triangles = (uint*)params->triangles;
  MeshDesc* meshes = (MeshDesc*)params->mesh;
  const MeshDesc mesh = meshes[instance.mesh];
  const ClusterDesc cluster = clusters[entry.y];
  const uint packed = triangles[cluster.triangle_offset + (id & 0xff)];
  float3 p[3];
  float4 clip[3];
  float2 screen[3];
  uint local[3];
  const float2 size = float2(float(params->width), float(params->height));
  for (uint i = 0; i < 3; ++i) {
    local[i] = cluster.vertex_offset + ((packed >> (8 * i)) & 0xff);
    p[i] = mul(instance.world, float4(load_position(mesh, instance, local[i]), 1.0)).xyz;
    clip[i] = mul(params->view_proj, float4(p[i], 1.0));
    const float2 ndc = clip[i].xy / clip[i].w;
    screen[i] = float2((ndc.x * 0.5 + 0.5) * size.x, (0.5 - ndc.y * 0.5) * size.y);
  }
  // Screen-space barycentrics, then perspective correction through 1/w.
  const float area = edge(screen[0], screen[1], screen[2]);
  float3 b;
  if (abs(area) > 1e-12) {
    b.x = edge(screen[1], screen[2], pixel_center) / area;
    b.y = edge(screen[2], screen[0], pixel_center) / area;
    b.z = 1.0 - b.x - b.y;
    b = clamp(b, 0.0, 1.0);
    b /= max(b.x + b.y + b.z, 1e-6);
    const float3 bw = b / float3(clip[0].w, clip[1].w, clip[2].w);
    b = bw / max(bw.x + bw.y + bw.z, 1e-12);
  } else {
    b = float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
  }
  Surface s;
  s.position = b.x * p[0] + b.y * p[1] + b.z * p[2];
  // The plane of the triangle that was drawn, turned towards the camera so that offsetting a
  // shadow ray along it always leaves the surface on the side the picture sees.
  s.geometric = normalize(cross(p[1] - p[0], p[2] - p[0]));
  if (dot(s.geometric, params->camera.xyz - s.position) < 0.0) s.geometric = -s.geometric;
  if (params->attributes != 0) {
    VertexAttributes* attributes = (VertexAttributes*)params->attributes;
    const VertexAttributes a0 = attributes[local[0]];
    const VertexAttributes a1 = attributes[local[1]];
    const VertexAttributes a2 = attributes[local[2]];
    const float3 n = b.x * decode_normal_oct(a0.normal_oct) + b.y * decode_normal_oct(a1.normal_oct) +
                     b.z * decode_normal_oct(a2.normal_oct);
    s.normal = transform_normal(instance.world, n);
    s.uv = b.x * decode_half2(a0.uv_half2) + b.y * decode_half2(a1.uv_half2) + b.z * decode_half2(a2.uv_half2);
  } else {
    s.normal = s.geometric;  // two-sided flat shading
    s.uv = float2(0.0, 0.0);
  }
  return s;
}

// The hit triangle's tangent frame, from its own positions and UVs: with dp1 = T du1 + B dv1 and
// dp2 = T du2 + B dv2, the two edge deltas and the two UV deltas solve for T and B, which are
// then Gram-Schmidt orthonormalized against the shading normal so the frame is orthonormal and
// keeps the UV parameterization's handedness (a mirrored UV island gives a mirrored bitangent,
// which is what a normal map authored on it expects). The positions go through the instance's
// world matrix, the same ones reconstruct() interpolated, so the frame is in world space like
// the normal it perturbs and an instance's scale or rotation carries into it. Returns false when
// there is no frame to build — no vertex attributes, or a triangle whose UV area is degenerate,
// seam-collapsed or simply absent — and the caller then leaves the normal alone rather than
// perturbing it by a frame it invented. Reads the triangle a second time instead of reaching into
// reconstruct(), so the surface reconstruction stays one self-contained block.
bool tangent_frame(ResolveParams* params, InstanceDesc instance, uint2 entry, uint id,
                   float3 normal, out float3 tangent, out float3 bitangent) {
  tangent = float3(1.0, 0.0, 0.0);
  bitangent = float3(0.0, 0.0, 1.0);
  if (params->attributes == 0) return false;
  ClusterDesc* clusters = (ClusterDesc*)params->clusters;
  uint* triangles = (uint*)params->triangles;
  MeshDesc* meshes = (MeshDesc*)params->mesh;
  VertexAttributes* attributes = (VertexAttributes*)params->attributes;
  const MeshDesc mesh = meshes[instance.mesh];
  const ClusterDesc cluster = clusters[entry.y];
  const uint packed = triangles[cluster.triangle_offset + (id & 0xff)];
  float3 p[3];
  float2 uv[3];
  for (uint i = 0; i < 3; ++i) {
    const uint vertex = cluster.vertex_offset + ((packed >> (8 * i)) & 0xff);
    p[i] = mul(instance.world, float4(load_position(mesh, instance, vertex), 1.0)).xyz;
    uv[i] = decode_half2(attributes[vertex].uv_half2);
  }
  const float3 edge1 = p[1] - p[0];
  const float3 edge2 = p[2] - p[0];
  const float2 duv1 = uv[1] - uv[0];
  const float2 duv2 = uv[2] - uv[0];
  const float area = duv1.x * duv2.y - duv2.x * duv1.y;
  if (abs(area) < 1e-12) return false;  // degenerate UVs: no frame, no perturbation
  const float r = 1.0 / area;
  float3 t = (edge1 * duv2.y - edge2 * duv1.y) * r;
  float3 b = (edge2 * duv1.x - edge1 * duv2.x) * r;
  t -= normal * dot(normal, t);
  const float t_length = length(t);
  if (t_length < 1e-12) return false;
  tangent = t / t_length;
  b -= normal * dot(normal, b) + tangent * dot(tangent, b);
  const float b_length = length(b);
  if (b_length < 1e-12) return false;
  bitangent = b / b_length;
  return true;
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
  if (out_pixel.x >= out_width || out_pixel.y >= out_height) return params->sky;
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
    if (source.x < 0.0 || source.y < 0.0) return params->sky;
  }
  const uint2 pixel = uint2(source);
  if (pixel.x >= params->width || pixel.y >= params->height) return params->sky;
  uint64_t* visibility = (uint64_t*)params->visibility;
  const uint64_t value = visibility[pixel.y * params->width + pixel.x];
  if (value == 0) return params->sky;
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
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  const InstanceDesc instance = instances[entry.x];
  const Surface s = reconstruct(params, instance, entry, id, source);
  if (params->mode == 4) return float4(s.normal * 0.5 + 0.5, 1.0);
  if (params->mode == 5) return float4(frac(s.uv), 0.0, 1.0);

  ResolveMaterial* materials = (ResolveMaterial*)params->materials;
  uint* cluster_materials = (uint*)params->cluster_materials;
  const ResolveMaterial material = materials[cluster_materials[entry.y] + instance.material_base];
  const float2 uv = s.uv * material.uv_scale;
  float3 albedo = material.albedo.rgb;
  if (material.albedo_texture != k_no_texture) {
    albedo *= g_textures[NonUniformResourceIndex(material.albedo_texture)]
                  .SampleLevel(g_samplers[NonUniformResourceIndex(material.sampler)], uv, 0)
                  .rgb;
  }
  // glTF packs roughness in G and metallic in B of one texture, and both multiply their factor.
  float roughness = material.albedo.w;
  float metallic = material.emissive.w;
  if (material.metallic_roughness_texture != k_no_texture) {
    const float4 mr =
        g_textures[NonUniformResourceIndex(material.metallic_roughness_texture)]
            .SampleLevel(g_samplers[NonUniformResourceIndex(material.sampler)], uv, 0);
    roughness *= mr.g;
    metallic *= mr.b;
  }
  // The normal map: a UNORM texture whose xyz are a unit vector remapped to 0..1 in the
  // triangle's own tangent frame. `normal_scale` widens or flattens the tangential part before
  // the vector is renormalized, so 0 gives the interpolated normal back exactly.
  float3 shading_normal = s.normal;
  if (material.normal_texture != k_no_texture) {
    float3 tangent;
    float3 bitangent;
    if (tangent_frame(params, instance, entry, id, s.normal, tangent, bitangent)) {
      float3 tangent_normal =
          g_textures[NonUniformResourceIndex(material.normal_texture)]
              .SampleLevel(g_samplers[NonUniformResourceIndex(material.sampler)], uv, 0)
              .xyz * 2.0 - 1.0;
      tangent_normal.xy *= material.normal_scale;
      const float length_sq = dot(tangent_normal, tangent_normal);
      if (length_sq > 1e-12) {
        tangent_normal *= rsqrt(length_sq);
        shading_normal = normalize(tangent * tangent_normal.x + bitangent * tangent_normal.y +
                                   s.normal * tangent_normal.z);
      }
    }
  }
  BrdfSurface surface;
  surface.albedo = albedo;
  surface.normal = shading_normal;
  surface.view = normalize(params->camera.xyz - s.position);
  surface.roughness = roughness;
  surface.metallic = metallic;

  // Shadow rays leave from the surface offset along the geometric normal, so that the quantized
  // position the resolve reconstructs cannot start the ray inside the float positions the
  // acceleration structure was built from.
  const float3 shadow_origin = s.position + s.geometric * params->shadow_bias;

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
    const ResolveLight light = lights[i];
    const float3 to_light = light.position_radius.xyz - s.position;
    const float distance_sq = dot(to_light, to_light);
    float attenuation = brdf_distance_attenuation(distance_sq, light.position_radius.w);
    if (attenuation <= 0.0) continue;
    const float3 light_dir = normalize(to_light);
    if (light.direction_type.w >= 0.5) {
      attenuation *= brdf_spot_attenuation(-light_dir, normalize(light.direction_type.xyz),
                                           light.spot.x, light.spot.y);
    }
    if (dot(surface.normal, light_dir) > 0.0 &&
        shadow_ray_blocked(params, k_shadow_lights, shadow_origin, light_dir,
                           sqrt(distance_sq) - params->shadow_bias)) {
      continue;
    }
    color += brdf_direct(surface, light_dir,
                         light.color_intensity.rgb * (light.color_intensity.w * attenuation));
  }
  // The sky hemisphere is the whole indirect term until the lighting passes exist: the diffuse
  // albedo takes it by normal elevation, and a Fresnel-weighted sliver of it at the view angle
  // keeps metals, which have no diffuse lobe, from going black.
  const float3 ambient = params->sky.rgb * lerp(0.15, 0.35, surface.normal.y * 0.5 + 0.5);
  const float n_dot_v = max(dot(surface.normal, surface.view), 0.0);
  color += ambient * (albedo * (1.0 - surface.metallic) +
                      brdf_f_schlick(brdf_f0(albedo, surface.metallic), n_dot_v));
  color += material.emissive.rgb;
  return float4(pow(max(color, 0.0), 1.0 / 2.2), 1.0);  // linear to display on a UNORM swapchain
}

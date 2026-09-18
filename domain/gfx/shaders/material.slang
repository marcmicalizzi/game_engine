// The material layer the real-time resolve and the reference path tracer share (docs/plan/
// 04-renderer.md §4.3 "Materials", §4.8). `brdf.slang` is the BSDF and stays free of bindings;
// this file is everything *around* it that both integrators have to agree on or the comparison
// measures the disagreement instead of the transport: the GPU-mirrored material and light
// structs, the triangle fetch off the cluster geometry, the attribute interpolation and the
// tangent frame, the texture reads, the analytic light falloffs, the sky, and the display
// transform.
//
// It declares the two bindless arrays it reads from (sampled images at binding 0, samplers at
// binding 2), which `brdf.slang` deliberately does not: a shader that includes this file is one
// that binds the scene's bindless set, and both of its users do. The acceleration structures at
// binding 3 stay out, because a device without them has no such binding and the plain resolve
// must still compile there.
//
// **Everything here used to live in visibility_resolve.slang.** It was moved rather than copied
// so that the reference cannot drift from the picture it is the reference for; the resolve's
// pictures are byte-identical across the move (docs/subsystems/renderer.md, "Reference
// renderer"). Keep the arithmetic and the order of operations as it is for the same reason.

#ifndef ENGINE_MATERIAL_SLANG
#define ENGINE_MATERIAL_SLANG

#include "brdf.slang"
#include "scene.slang"

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[];
[[vk::binding(2, 0)]] SamplerState g_samplers[];

static const uint k_no_texture = 0xffffffffu;

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

// The addresses of the scene both integrators read a hit through. One struct instead of eight
// arguments, because the resolve pulls them out of `ResolveParams` and the path tracer out of
// `PathTraceParams` and neither should have to spell the list twice.
struct SceneGeometry {
  uint64_t clusters;           // ClusterDesc[]
  uint64_t mesh;               // MeshDesc[]
  uint64_t triangles;          // uint[]: packed local indices
  uint64_t instances;          // InstanceDesc[]
  uint64_t attributes;         // VertexAttributes[] parallel to vertices; 0 = flat shading
  uint64_t materials;          // ResolveMaterial[]
  uint64_t cluster_materials;  // uint[cluster_count]: material index per cluster
  uint64_t visible;            // uint2[]: the cull pass's visible list; 0 reads {0, index}
};

struct Surface {
  float3 position;
  float3 normal;     // shading normal: the interpolated vertex normals, or the triangle plane
  float3 geometric;  // the triangle's own plane normal, facing the camera: what a ray offsets by
  float2 uv;
};

// One triangle of the scene, fetched once: three world-space positions and the three scene-wide
// vertex indices they came from, which is what the attributes and the tangent frame index by.
struct TriangleFetch {
  float3 p[3];
  uint local[3];
};

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

float material_edge(float2 a, float2 b, float2 c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// The three vertices of triangle `triangle` of cluster `cluster` under `instance`, in world
// space: positions come off the mesh's 16-bit grid, or out of the frame's deformed-vertex pool
// for a deformed instance (load_position), and go through the instance's world matrix, exactly
// as the rasterizer and the acceleration structure builds moved them.
TriangleFetch fetch_triangle(SceneGeometry geo, InstanceDesc instance, uint cluster_index,
                            uint triangle) {
  ClusterDesc* clusters = (ClusterDesc*)geo.clusters;
  uint* triangles = (uint*)geo.triangles;
  MeshDesc* meshes = (MeshDesc*)geo.mesh;
  const MeshDesc mesh = meshes[instance.mesh];
  const ClusterDesc cluster = clusters[cluster_index];
  const uint packed = triangles[cluster.triangle_offset + triangle];
  TriangleFetch fetched;
  for (uint i = 0; i < 3; ++i) {
    fetched.local[i] = cluster.vertex_offset + ((packed >> (8 * i)) & 0xff);
    fetched.p[i] =
        mul(instance.world, float4(load_position(mesh, instance, fetched.local[i]), 1.0)).xyz;
  }
  return fetched;
}

// Position, shading normal, geometric normal, and UV at barycentrics `b` of a fetched triangle.
// `towards` is the point the geometric normal is turned towards — the camera in the resolve, the
// ray's origin in the path tracer — so that offsetting a ray along it always leaves the surface
// on the side the ray came from.
Surface surface_at(SceneGeometry geo, InstanceDesc instance, TriangleFetch tri, float3 b,
                   float3 towards) {
  Surface s;
  s.position = b.x * tri.p[0] + b.y * tri.p[1] + b.z * tri.p[2];
  s.geometric = normalize(cross(tri.p[1] - tri.p[0], tri.p[2] - tri.p[0]));
  if (dot(s.geometric, towards - s.position) < 0.0) s.geometric = -s.geometric;
  if (geo.attributes != 0) {
    VertexAttributes* attributes = (VertexAttributes*)geo.attributes;
    const VertexAttributes a0 = attributes[tri.local[0]];
    const VertexAttributes a1 = attributes[tri.local[1]];
    const VertexAttributes a2 = attributes[tri.local[2]];
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

// The resolve's reconstruction: screen-space barycentrics against `pixel_center`, made
// perspective-correct through the vertices' clip-space w. A rasterized pixel has no barycentrics
// of its own — the visibility buffer holds an id and a depth — so they are recovered by putting
// the triangle back through the same view-projection the rasterizer used.
Surface reconstruct_screen(SceneGeometry geo, InstanceDesc instance, uint cluster_index,
                          uint triangle, float2 pixel_center, column_major float4x4 view_proj,
                          float3 camera, uint width, uint height) {
  const TriangleFetch tri = fetch_triangle(geo, instance, cluster_index, triangle);
  float4 clip[3];
  float2 screen[3];
  const float2 size = float2(float(width), float(height));
  for (uint i = 0; i < 3; ++i) {
    clip[i] = mul(view_proj, float4(tri.p[i], 1.0));
    const float2 ndc = clip[i].xy / clip[i].w;
    screen[i] = float2((ndc.x * 0.5 + 0.5) * size.x, (0.5 - ndc.y * 0.5) * size.y);
  }
  const float area = material_edge(screen[0], screen[1], screen[2]);
  float3 b;
  if (abs(area) > 1e-12) {
    b.x = material_edge(screen[1], screen[2], pixel_center) / area;
    b.y = material_edge(screen[2], screen[0], pixel_center) / area;
    b.z = 1.0 - b.x - b.y;
    b = clamp(b, 0.0, 1.0);
    b /= max(b.x + b.y + b.z, 1e-6);
    const float3 bw = b / float3(clip[0].w, clip[1].w, clip[2].w);
    b = bw / max(bw.x + bw.y + bw.z, 1e-12);
  } else {
    b = float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
  }
  return surface_at(geo, instance, tri, b, camera);
}

// The hit triangle's tangent frame, from its own positions and UVs: with dp1 = T du1 + B dv1 and
// dp2 = T du2 + B dv2, the two edge deltas and the two UV deltas solve for T and B, which are
// then Gram-Schmidt orthonormalized against the shading normal so the frame is orthonormal and
// keeps the UV parameterization's handedness (a mirrored UV island gives a mirrored bitangent,
// which is what a normal map authored on it expects). The positions are the ones the surface was
// interpolated from, so the frame is in world space like the normal it perturbs and an instance's
// scale or rotation carries into it. Returns false when there is no frame to build — no vertex
// attributes, or a triangle whose UV area is degenerate, seam-collapsed or simply absent — and
// the caller then leaves the normal alone rather than perturbing it by a frame it invented.
bool tangent_frame(SceneGeometry geo, TriangleFetch tri, float3 normal, out float3 tangent,
                   out float3 bitangent) {
  tangent = float3(1.0, 0.0, 0.0);
  bitangent = float3(0.0, 0.0, 1.0);
  if (geo.attributes == 0) return false;
  VertexAttributes* attributes = (VertexAttributes*)geo.attributes;
  float2 uv[3];
  for (uint i = 0; i < 3; ++i)
    uv[i] = decode_half2(attributes[tri.local[i]].uv_half2);
  const float3 edge1 = tri.p[1] - tri.p[0];
  const float3 edge2 = tri.p[2] - tri.p[0];
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

// The material of a hit: the cluster's entry of the table plus the instance's base.
ResolveMaterial material_of(SceneGeometry geo, InstanceDesc instance, uint cluster_index) {
  ResolveMaterial* materials = (ResolveMaterial*)geo.materials;
  uint* cluster_materials = (uint*)geo.cluster_materials;
  return materials[cluster_materials[cluster_index] + instance.material_base];
}

// A material's factors after its textures: the albedo texture multiplies the base color, and a
// glTF metallic-roughness map multiplies both factors out of G and B. Sampled at level 0, which
// is what a compute stage with no derivatives can do and what the fragment stage already did.
struct MaterialSample {
  float3 albedo;
  float roughness;
  float metallic;
  float3 emissive;
};

MaterialSample sample_material(ResolveMaterial material, float2 uv) {
  MaterialSample sampled;
  sampled.albedo = material.albedo.rgb;
  if (material.albedo_texture != k_no_texture) {
    sampled.albedo *= g_textures[NonUniformResourceIndex(material.albedo_texture)]
                          .SampleLevel(g_samplers[NonUniformResourceIndex(material.sampler)], uv, 0)
                          .rgb;
  }
  sampled.roughness = material.albedo.w;
  sampled.metallic = material.emissive.w;
  if (material.metallic_roughness_texture != k_no_texture) {
    const float4 mr =
        g_textures[NonUniformResourceIndex(material.metallic_roughness_texture)]
            .SampleLevel(g_samplers[NonUniformResourceIndex(material.sampler)], uv, 0);
    sampled.roughness *= mr.g;
    sampled.metallic *= mr.b;
  }
  sampled.emissive = material.emissive.rgb;
  return sampled;
}

// The normal map: a UNORM texture whose xyz are a unit vector remapped to 0..1 in the triangle's
// own tangent frame. `normal_scale` widens or flattens the tangential part before the vector is
// renormalized, so 0 gives the interpolated normal back exactly.
float3 apply_normal_map(ResolveMaterial material, float2 uv, float3 normal, float3 tangent,
                        float3 bitangent) {
  float3 tangent_normal = g_textures[NonUniformResourceIndex(material.normal_texture)]
                              .SampleLevel(g_samplers[NonUniformResourceIndex(material.sampler)],
                                           uv, 0)
                              .xyz * 2.0 - 1.0;
  tangent_normal.xy *= material.normal_scale;
  const float length_sq = dot(tangent_normal, tangent_normal);
  if (length_sq <= 1e-12) return normal;
  tangent_normal *= rsqrt(length_sq);
  return normalize(tangent * tangent_normal.x + bitangent * tangent_normal.y +
                   normal * tangent_normal.z);
}

// One analytic light at a surface point: the direction towards it, the radiance that arrives,
// and how far away it is, with the windowed inverse square and the spot cone already in the
// radiance. False when the light contributes nothing at all, which is what lets both integrators
// skip it before they trace a shadow ray.
bool light_sample(ResolveLight light, float3 position, out float3 direction, out float3 radiance,
                  out float distance) {
  const float3 to_light = light.position_radius.xyz - position;
  const float distance_sq = dot(to_light, to_light);
  float attenuation = brdf_distance_attenuation(distance_sq, light.position_radius.w);
  direction = float3(0.0, 1.0, 0.0);
  radiance = float3(0.0);
  distance = 0.0;
  if (attenuation <= 0.0) return false;
  direction = normalize(to_light);
  if (light.direction_type.w >= 0.5) {
    attenuation *= brdf_spot_attenuation(-direction, normalize(light.direction_type.xyz),
                                         light.spot.x, light.spot.y);
  }
  radiance = light.color_intensity.rgb * (light.color_intensity.w * attenuation);
  distance = sqrt(distance_sq);
  return true;
}

// The sky as **illumination**: the hemisphere term the resolve adds as its whole indirect light,
// and the environment radiance the path tracer's escaped rays pick up, so that the two agree in
// the limit as far as the model allows (docs/subsystems/renderer.md, "Reference renderer"). The
// ramp by elevation is the resolve's own and is kept here so there is one of it.
float3 sky_radiance(float3 sky, float3 direction) {
  return sky * lerp(0.15, 0.35, direction.y * 0.5 + 0.5);
}

// Linear to what a UNORM target holds. Both integrators end on this, so a reference image and a
// real-time image are in the same space and FLIP compares pictures rather than transfer curves.
float3 display_encode(float3 linear_color) { return pow(max(linear_color, 0.0), 1.0 / 2.2); }

#endif  // ENGINE_MATERIAL_SLANG

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
// ResolveMaterial::flags (gfx::k_material_*). The first two are zero for textures decoded from
// their sources at one level, which read exactly as they always did.
static const uint k_material_mipped = 1u;        // built, mipmapped textures: sample with gradients
static const uint k_material_normal_rg = 2u;     // a two-channel (BC5) normal map: rebuild z
static const uint k_material_uv_transform = 4u;  // the UVs go through uv_transform and uv_offset
// A per-slot sampler half that borrows the material's `sampler` (gfx::k_same_sampler).
static const uint k_same_sampler = 0xffffu;

// Mirrors gfx::ResolveMaterial (112 bytes).
struct ResolveMaterial {
  float4 albedo;    // rgb linear, w roughness
  float4 emissive;  // rgb linear factor, w metallic
  uint albedo_texture;  // bindless sampled-image slot; 0xffffffff for none
  uint sampler;         // bindless sampler slot of the base colour
  float uv_scale;
  uint flags;           // k_material_mipped | k_material_normal_rg | k_material_uv_transform
  uint metallic_roughness_texture;  // glTF packing: G roughness, B metallic
  uint normal_texture;              // tangent space, UNORM, remapped to -1..1
  float normal_scale;               // scales the map's xy before it is normalized
  uint occlusion_texture;           // R: occlusion of the indirect term
  uint emissive_texture;            // multiplies the emissive factor
  float occlusion_strength;         // occlusion = 1 + strength * (texel.r - 1)
  uint samplers_mr_normal;          // low half metallic-roughness, high half normal
  uint samplers_occlusion_emissive; // low half occlusion, high half emissive
  float4 uv_transform;              // u' = x u + y v + offset.x, v' = z u + w v + offset.y
  float4 uv_offset;                 // xy; zw unused
};

// The bindless sampler a slot reads with: its half of a per-slot word, or the material's own
// `sampler` where the half says k_same_sampler.
uint slot_sampler(ResolveMaterial material, uint word, uint shift) {
  const uint slot = (word >> shift) & 0xffffu;
  return slot == k_same_sampler ? material.sampler : slot;
}

// The material's UV transform (KHR_texture_transform) on a UV, and its linear part on a UV
// derivative. Without k_material_uv_transform both are the identity and cost a branch.
float2 material_uv(ResolveMaterial material, float2 uv) {
  if ((material.flags & k_material_uv_transform) == 0) return uv;
  return float2(material.uv_transform.x * uv.x + material.uv_transform.y * uv.y,
                material.uv_transform.z * uv.x + material.uv_transform.w * uv.y) +
         material.uv_offset.xy;
}

float2 material_duv(ResolveMaterial material, float2 duv) {
  if ((material.flags & k_material_uv_transform) == 0) return duv;
  return float2(material.uv_transform.x * duv.x + material.uv_transform.y * duv.y,
                material.uv_transform.z * duv.x + material.uv_transform.w * duv.y);
}

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
  // The deformed **normal** pool, parallel to the position pool and written by the same pass;
  // 0 when the frame carries none, and every shading normal then comes off the rest attribute
  // stream as it always has. That is what keeps every picture of a scene with no morph channels
  // byte-identical (gfx.md, "What happens to the shading normal").
  uint64_t normal_pool;
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
  // The pool block this triangle's vertices came out of and where they sit in their cluster, so
  // that `surface_at` can read the deformed normals beside the positions. `k_no_pool_slot` for
  // a rigid instance or a cluster the pool had no room for, which is what makes the normal read
  // fall back to the rest attribute stream for exactly the cases the position read does.
  uint pool_base;
  uint pool_local[3];
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
//
// `entry` is the **visible index**: the one the pixel's pair was drawn as this frame (the resolve
// reads it out of the view's pair-to-entry table) or the ray's geometry index carried, which is
// how the pool block is found: the pool is suballocated per frame per visible entry, so the same
// (instance, cluster) pair drawn by two views has two blocks holding the same positions.
TriangleFetch fetch_triangle(SceneGeometry geo, InstanceDesc instance, uint entry,
                            uint cluster_index, uint triangle) {
  ClusterDesc* clusters = (ClusterDesc*)geo.clusters;
  uint* triangles = (uint*)geo.triangles;
  MeshDesc* meshes = (MeshDesc*)geo.mesh;
  const MeshDesc mesh = meshes[instance.mesh];
  const ClusterDesc cluster = clusters[cluster_index];
  const uint packed = triangles[cluster.triangle_offset + triangle];
  const uint base = deform_base(mesh, instance, entry);
  TriangleFetch fetched;
  fetched.pool_base = base;
  for (uint i = 0; i < 3; ++i) {
    const uint local = (packed >> (8 * i)) & 0xff;
    fetched.local[i] = cluster.vertex_offset + local;
    fetched.pool_local[i] = local;
    fetched.p[i] =
        mul(instance.world, float4(load_position(mesh, base, cluster.vertex_offset, local), 1.0))
            .xyz;
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
    // The three vertex normals: the **deformed** ones out of the frame's normal pool when this
    // triangle came out of a pool block and the frame carries one, and the rest ones off the
    // attribute stream otherwise. The branch is on an address that is zero for every scene with
    // no morph channels, so a picture that had no normal pool reads exactly the instructions it
    // always read.
    float3 n0 = decode_normal_oct(a0.normal_oct);
    float3 n1 = decode_normal_oct(a1.normal_oct);
    float3 n2 = decode_normal_oct(a2.normal_oct);
    if (geo.normal_pool != 0 && tri.pool_base != k_no_pool_slot) {
      n0 = load_pool_normal(geo.normal_pool, tri.pool_base, tri.pool_local[0], n0);
      n1 = load_pool_normal(geo.normal_pool, tri.pool_base, tri.pool_local[1], n1);
      n2 = load_pool_normal(geo.normal_pool, tri.pool_base, tri.pool_local[2], n2);
    }
    const float3 n = b.x * n0 + b.y * n1 + b.z * n2;
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
Surface reconstruct_screen(SceneGeometry geo, InstanceDesc instance, uint entry,
                          uint cluster_index, uint triangle, float2 pixel_center,
                          column_major float4x4 view_proj, float3 camera, uint width,
                          uint height) {
  const TriangleFetch tri = fetch_triangle(geo, instance, entry, cluster_index, triangle);
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

// The UV's derivatives across the screen at `pixel_center`, taken from the triangle itself rather
// than from neighbouring fragments (docs/subsystems/texture.md, "Mip selection without quads").
// A visibility-buffer resolve is a fullscreen pass: the 2x2 quad a derivative instruction would
// difference holds pixels of *other* triangles wherever an edge crosses it — often other meshes,
// other UV islands — so ddx(uv) there is garbage and a texture fetch picks a level from it. The
// triangle's own derivatives are exact and cost one more pass over its three vertices:
//
//   screen barycentrics are affine in the pixel, l_i = edge(s_j, s_k, p) / area, so
//     dl_i/dx = -(s_k.y - s_j.y) / area   and   dl_i/dy = (s_k.x - s_j.x) / area;
//   the perspective-correct ones are b_i = q_i / Q with q_i = l_i / w_i and Q = sum q_j, so
//     db_i = (dq_i - b_i dQ) / Q   with dq_i = dl_i / w_i;
//   and uv = sum b_i uv_i, so duv = sum db_i uv_i.
//
// Zero for a triangle with no attributes, a degenerate screen area, or w that has collapsed,
// which SampleGrad reads as level 0.
void screen_uv_gradients(SceneGeometry geo, TriangleFetch tri, float2 pixel_center,
                         column_major float4x4 view_proj, uint width, uint height,
                         out float2 duv_dx, out float2 duv_dy) {
  duv_dx = float2(0.0, 0.0);
  duv_dy = float2(0.0, 0.0);
  if (geo.attributes == 0) return;
  float4 clip[3];
  float2 screen[3];
  const float2 size = float2(float(width), float(height));
  for (uint i = 0; i < 3; ++i) {
    clip[i] = mul(view_proj, float4(tri.p[i], 1.0));
    const float2 ndc = clip[i].xy / clip[i].w;
    screen[i] = float2((ndc.x * 0.5 + 0.5) * size.x, (0.5 - ndc.y * 0.5) * size.y);
  }
  const float area = material_edge(screen[0], screen[1], screen[2]);
  if (abs(area) <= 1e-12) return;
  float3 l;
  l.x = material_edge(screen[1], screen[2], pixel_center) / area;
  l.y = material_edge(screen[2], screen[0], pixel_center) / area;
  l.z = 1.0 - l.x - l.y;
  const float3 dl_dx =
      float3(screen[1].y - screen[2].y, screen[2].y - screen[0].y, screen[0].y - screen[1].y) /
      area;
  const float3 dl_dy =
      float3(screen[2].x - screen[1].x, screen[0].x - screen[2].x, screen[1].x - screen[0].x) /
      area;
  const float3 inv_w = 1.0 / float3(clip[0].w, clip[1].w, clip[2].w);
  const float3 q = l * inv_w;
  const float q_sum = q.x + q.y + q.z;
  if (abs(q_sum) <= 1e-20) return;
  const float3 b = q / q_sum;
  const float3 dq_dx = dl_dx * inv_w;
  const float3 dq_dy = dl_dy * inv_w;
  const float3 db_dx = (dq_dx - b * (dq_dx.x + dq_dx.y + dq_dx.z)) / q_sum;
  const float3 db_dy = (dq_dy - b * (dq_dy.x + dq_dy.y + dq_dy.z)) / q_sum;
  VertexAttributes* attributes = (VertexAttributes*)geo.attributes;
  const float2 uv0 = decode_half2(attributes[tri.local[0]].uv_half2);
  const float2 uv1 = decode_half2(attributes[tri.local[1]].uv_half2);
  const float2 uv2 = decode_half2(attributes[tri.local[2]].uv_half2);
  duv_dx = db_dx.x * uv0 + db_dx.y * uv1 + db_dx.z * uv2;
  duv_dy = db_dy.x * uv0 + db_dy.y * uv1 + db_dy.z * uv2;
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
// is what a compute stage with no derivatives can do (the reference path tracer) and all a texture
// decoded at one level has; the resolve samples a mipmapped material through
// `sample_material_grad` below.
struct MaterialSample {
  float3 albedo;
  float roughness;
  float metallic;
  float3 emissive;
};

//
// The emissive factor is multiplied by the emissive texture where the material has one — sRGB
// colour, so it samples linear — and the sum is **radiance leaving the surface**, which both
// integrators add as it is: the resolve to the pixel, the path tracer wherever a ray lands on it
// (docs/subsystems/renderer.md, "Materials").
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
    const uint s = slot_sampler(material, material.samplers_mr_normal, 0);
    const float4 mr = g_textures[NonUniformResourceIndex(material.metallic_roughness_texture)]
                          .SampleLevel(g_samplers[NonUniformResourceIndex(s)], uv, 0);
    sampled.roughness *= mr.g;
    sampled.metallic *= mr.b;
  }
  sampled.emissive = material.emissive.rgb;
  if (material.emissive_texture != k_no_texture) {
    const uint s = slot_sampler(material, material.samplers_occlusion_emissive, 16);
    sampled.emissive *= g_textures[NonUniformResourceIndex(material.emissive_texture)]
                            .SampleLevel(g_samplers[NonUniformResourceIndex(s)], uv, 0)
                            .rgb;
  }
  return sampled;
}

// `sample_material` for the resolve, which knows the UV's derivatives (`screen_uv_gradients`): a
// material whose textures are the content build's mipmapped ones samples them at the level and
// with the anisotropy those derivatives select; every other material is exactly
// `sample_material`, level 0, the reads it always made.
MaterialSample sample_material_grad(ResolveMaterial material, float2 uv, float2 duv_dx,
                                    float2 duv_dy) {
  if ((material.flags & k_material_mipped) == 0) return sample_material(material, uv);
  MaterialSample sampled;
  sampled.albedo = material.albedo.rgb;
  if (material.albedo_texture != k_no_texture) {
    sampled.albedo *= g_textures[NonUniformResourceIndex(material.albedo_texture)]
                          .SampleGrad(g_samplers[NonUniformResourceIndex(material.sampler)], uv,
                                      duv_dx, duv_dy)
                          .rgb;
  }
  sampled.roughness = material.albedo.w;
  sampled.metallic = material.emissive.w;
  if (material.metallic_roughness_texture != k_no_texture) {
    const uint s = slot_sampler(material, material.samplers_mr_normal, 0);
    const float4 mr =
        g_textures[NonUniformResourceIndex(material.metallic_roughness_texture)].SampleGrad(
            g_samplers[NonUniformResourceIndex(s)], uv, duv_dx, duv_dy);
    sampled.roughness *= mr.g;
    sampled.metallic *= mr.b;
  }
  sampled.emissive = material.emissive.rgb;
  if (material.emissive_texture != k_no_texture) {
    const uint s = slot_sampler(material, material.samplers_occlusion_emissive, 16);
    sampled.emissive *= g_textures[NonUniformResourceIndex(material.emissive_texture)]
                            .SampleGrad(g_samplers[NonUniformResourceIndex(s)], uv, duv_dx, duv_dy)
                            .rgb;
  }
  return sampled;
}

// The material's ambient occlusion at `uv`: glTF's `1 + strength * (texel.r - 1)`, and exactly 1
// for a material with no occlusion map. **The resolve's alone**, and only ever a factor on its
// indirect term (the sky hemisphere today, Phase 2's indirect lighting when it exists): an
// occlusion map is a baked estimate of how much of the hemisphere a point sees, which is what
// direct light's shadows and a path tracer's bounces already compute, so applying it to either
// would count the same occlusion twice (docs/subsystems/texture.md, "Occlusion").
float sample_occlusion(ResolveMaterial material, float2 uv, float2 duv_dx, float2 duv_dy) {
  if (material.occlusion_texture == k_no_texture) return 1.0;
  const uint s = slot_sampler(material, material.samplers_occlusion_emissive, 0);
  float texel;
  if ((material.flags & k_material_mipped) != 0) {
    texel = g_textures[NonUniformResourceIndex(material.occlusion_texture)]
                .SampleGrad(g_samplers[NonUniformResourceIndex(s)], uv, duv_dx, duv_dy)
                .r;
  } else {
    texel = g_textures[NonUniformResourceIndex(material.occlusion_texture)]
                .SampleLevel(g_samplers[NonUniformResourceIndex(s)], uv, 0)
                .r;
  }
  return 1.0 + material.occlusion_strength * (texel - 1.0);
}

// A normal-map texel to the tangent-space vector, before `normal_scale`: xyz remapped from 0..1,
// or — for a two-channel map, which the content build stores as BC5 and which samples blue as
// zero — x and y remapped and z rebuilt as the unit vector's third component.
float3 normal_map_vector(ResolveMaterial material, float4 texel) {
  if ((material.flags & k_material_normal_rg) == 0) return texel.xyz * 2.0 - 1.0;
  const float2 xy = texel.xy * 2.0 - 1.0;
  return float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
}

// The normal map: a UNORM texture whose xyz are a unit vector remapped to 0..1 in the triangle's
// own tangent frame. `normal_scale` widens or flattens the tangential part before the vector is
// renormalized, so 0 gives the interpolated normal back exactly.
float3 apply_normal_map(ResolveMaterial material, float2 uv, float3 normal, float3 tangent,
                        float3 bitangent) {
  const uint s = slot_sampler(material, material.samplers_mr_normal, 16);
  float3 tangent_normal = normal_map_vector(
      material, g_textures[NonUniformResourceIndex(material.normal_texture)].SampleLevel(
                    g_samplers[NonUniformResourceIndex(s)], uv, 0));
  tangent_normal.xy *= material.normal_scale;
  const float length_sq = dot(tangent_normal, tangent_normal);
  if (length_sq <= 1e-12) return normal;
  tangent_normal *= rsqrt(length_sq);
  return normalize(tangent * tangent_normal.x + bitangent * tangent_normal.y +
                   normal * tangent_normal.z);
}

// `apply_normal_map` with the UV's derivatives, as `sample_material_grad` is to `sample_material`.
float3 apply_normal_map_grad(ResolveMaterial material, float2 uv, float2 duv_dx, float2 duv_dy,
                             float3 normal, float3 tangent, float3 bitangent) {
  if ((material.flags & k_material_mipped) == 0) {
    return apply_normal_map(material, uv, normal, tangent, bitangent);
  }
  const uint s = slot_sampler(material, material.samplers_mr_normal, 16);
  float3 tangent_normal = normal_map_vector(
      material, g_textures[NonUniformResourceIndex(material.normal_texture)].SampleGrad(
                    g_samplers[NonUniformResourceIndex(s)], uv, duv_dx, duv_dy));
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

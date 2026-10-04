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
#include "display.slang"
#include "scene.slang"

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[];
[[vk::binding(2, 0)]] SamplerState g_samplers[];

static const uint k_no_texture = 0xffffffffu;
// ResolveMaterial::flags (gfx::k_material_*). The first two are zero for textures decoded from
// their sources at one level, which read exactly as they always did.
static const uint k_material_mipped = 1u;        // built, mipmapped textures: sample with gradients
static const uint k_material_normal_rg = 2u;     // a two-channel (BC5) normal map: rebuild z
static const uint k_material_uv_transform = 4u;  // the UVs go through uv_transform and uv_offset
// The material carries the ground's detail (ground_detail.slang), its sand share in the
// metallic-roughness map's alpha: a terrain's, when its scene asks for the detail.
static const uint k_material_ground_detail = 8u;
// A per-slot sampler half that borrows the material's `sampler` (gfx::k_same_sampler).
static const uint k_same_sampler = 0xffffu;

// Mirrors gfx::ResolveMaterial (112 bytes).
struct ResolveMaterial {
  float4 albedo;    // rgb linear, w roughness
  float4 emissive;  // rgb linear factor, w metallic
  uint albedo_texture;  // bindless sampled-image slot; 0xffffffff for none
  uint sampler;         // bindless sampler slot of the base colour
  float uv_scale;
  uint flags;           // k_material_mipped | _normal_rg | _uv_transform | _ground_detail
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
  float4 direction_type;   // xyz spot direction (towards the lit surface), w 0 point, 1 spot,
                           // 2 directional (position_radius.xyz then points towards the light)
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

// **How far a ray leaves a surface**, along its geometric normal, before it looks for anything:
// `floor` world units plus `steps` steps of the surface's **own** 16-bit position grid, through its
// instance's largest scale (docs/subsystems/renderer.md, "Shadows").
//
// Both integrators rebuild a surface point off that grid (`fetch_triangle`), and the acceleration
// structures hold the float positions it was rounded from, half a step away on each axis at most,
// so at most √3/2 of a step along any normal: a ray started on the rebuilt surface can start behind
// the one it is traced against and hit its own triangle. That gap is a property of the receiver's
// mesh — 0.06 mm on a 4 m wall of the ashlar kit, 7.8 cm on the 5 km terrain it stands on — and
// the offset follows it, where it used to be a thousandth of the *scene's* radius: 4.1 m in that
// scene, which put every ray from the sand above every wall. `floor` is what is left, the float
// arithmetic of a position that far from the origin, which the renderer sizes from the scene's
// bounds (`renderer::FrameLighting`). A deformed instance's positions come out of the pool the
// structures were built from, so it needs no grid term; it gets its rest mesh's anyway, which only
// moves its rays a grid step further out, and covers a cluster the pool had no room for, which
// both read at rest.
float ray_offset(SceneGeometry geo, InstanceDesc instance, float floor, float steps) {
  MeshDesc* meshes = (MeshDesc*)geo.mesh;
  return floor + steps * meshes[instance.mesh].quant.w * instance.scale_max;
}

// ---- where a pixel meets its triangle ------------------------------------------------------------
//
// **The resolve's reconstruction** (docs/subsystems/gfx.md, "Where a pixel meets its triangle"). A
// rasterized pixel has no barycentrics of its own — the visibility buffer holds an id and a depth —
// so they are recovered by putting the triangle back through the view-projection the rasterizer
// drew it with and asking where the ray through the pixel's centre meets its plane.
//
// In clip space, and **without a divide by w** (2D homogeneous rasterization, Olano and Greer 1997).
// The corners' clip-space (x, y, w) are three vectors from the eye, and the pixel's centre is the
// direction p = (ndc.x, ndc.y, 1) in the same space; the point of the triangle's plane on that line
// is sum b_i c_i with
//
//   b_i = e_i / E,   e_i = dot(c_j x c_k, p)   (i, j, k in turn),   E = e_0 + e_1 + e_2,
//
// because sum e_i c_i = det(c_0, c_1, c_2) p. Clip space is an affine image of the world, so these
// are the barycentrics of the triangle's own corners: the ray against the plane, exactly, for every
// triangle the rasterizer can draw — one with a corner behind the camera, or on its plane, included.
//
// Until 2026-10-03 the corners were divided by w and the barycentrics taken in screen space, then
// **clamped into [0, 1] there** and only then made perspective-correct. The divide is not wrong by
// itself — a screen barycentric times 1/w_i is e_i up to a factor all three share, for any w that
// is not zero — but the clamp was: on a triangle whose corners have w of both signs, a pixel the
// triangle covers has screen barycentrics outside [0, 1], so the clamp moved its point onto an edge
// metres from where it looks. And the divide loses everything as w goes to zero, where a corner
// lands at infinity. The derivatives below never clamped, which is why they were right where the
// point was not.
//
// Their change per pixel is exact too: e and E are linear in p, so db_i/dp = (n_i - b_i N) / E with
// n_i = c_j x c_k and N = sum n_i, and p moves by (2 / width, -2 / height) a pixel — the viewport
// the rasterizer drew with, flipped in y.
//
// **Measured from the eye.** The corners go in as their differences from the eye, through the
// matrix's linear part alone (a w of zero drops its translation column): that column is the eye's
// world position through the matrix, a number the size of the camera's distance from the origin,
// and a corner put through the whole matrix carries its rounding — millimetres of the corner fifty
// kilometres out, and different at every step the camera takes. The difference of two floats that
// close together is exact, and everything after it is on numbers the size of the triangle's
// distance from the eye. The point comes out the same way, as `from_eye`, which the view vector and
// the air's distance read as it is; the world position is `eye + from_eye`, one rounding at the
// end, which only a reader that needs the world (the lights, the shadows, the ground's detail) pays.
// `eye` must be the point the matrix's translation was made from (`ResolveParams::camera`).

// What the resolve takes at a pixel of a fetched triangle.
struct ScreenBarycentrics {
  // Where the pixel centre's ray meets the triangle — **moved onto it** when the ray lands outside:
  // a negative barycentric is zeroed and the rest renormalized. That happens where the rasterizer
  // drew a centre within its sub-pixel snap of an edge from the triangle across it, and the point
  // is then on that edge, a snap's worth of the footprint from where the ray meets the plane (past
  // a corner, the corner). A ray parallel to the plane — a triangle exactly edge-on, which only the
  // snap draws — is the same rule on the edge functions themselves, and with nothing on either side
  // of zero the centroid.
  float3 b;
  // The unclamped barycentrics' change per pixel along the picture's x and y (y down). Zero where
  // the ray is parallel to the plane.
  float3 db_dx;
  float3 db_dy;
};

ScreenBarycentrics screen_barycentrics(TriangleFetch tri, float2 pixel_center,
                                       column_major float4x4 view_proj, float3 eye, uint width,
                                       uint height) {
  // x, y and w: a pixel's ray is fixed by x / w and y / w, and z says nothing about where it goes.
  // Each corner from the eye, through the linear part: the clip-space position less the eye's.
  float3 c[3];
  for (uint i = 0; i < 3; ++i)
    c[i] = mul(view_proj, float4(tri.p[i] - eye, 0.0)).xyw;
  const float2 ndc_per_pixel = float2(2.0 / float(width), -2.0 / float(height));
  const float3 p = float3(pixel_center * ndc_per_pixel + float2(-1.0, 1.0), 1.0);
  const float3 n0 = cross(c[1], c[2]);
  const float3 n1 = cross(c[2], c[0]);
  const float3 n2 = cross(c[0], c[1]);
  const float3 e = float3(dot(n0, p), dot(n1, p), dot(n2, p));
  const float plane = e.x + e.y + e.z;
  ScreenBarycentrics s;
  // e / E, clamped, without dividing by E: the edge functions on E's side of zero, the others
  // zeroed, over their sum.
  const float3 inside = max(plane < 0.0 ? -e : e, 0.0);
  const float total = inside.x + inside.y + inside.z;
  s.b = total > 0.0 ? inside / total : float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
  s.db_dx = float3(0.0, 0.0, 0.0);
  s.db_dy = float3(0.0, 0.0, 0.0);
  if (plane != 0.0) {
    const float3 b = e / plane;
    const float3 nx = float3(n0.x, n1.x, n2.x);
    const float3 ny = float3(n0.y, n1.y, n2.y);
    s.db_dx = (nx - b * (nx.x + nx.y + nx.z)) * (ndc_per_pixel.x / plane);
    s.db_dy = (ny - b * (ny.x + ny.y + ny.z)) * (ndc_per_pixel.y / plane);
  }
  return s;
}

// The surface a pixel shows: its triangle fetched, its barycentrics (`screen_barycentrics`), and
// everything interpolated at them (`surface_at`). `from_eye` is the point less `camera`, made from
// the corners less `camera`, which is what the view vector and the air's distance are taken from;
// the surface's world position is `camera + from_eye`.
Surface reconstruct_screen(SceneGeometry geo, InstanceDesc instance, uint entry,
                          uint cluster_index, uint triangle, float2 pixel_center,
                          column_major float4x4 view_proj, float3 camera, uint width,
                          uint height, out float3 from_eye) {
  const TriangleFetch tri = fetch_triangle(geo, instance, entry, cluster_index, triangle);
  const ScreenBarycentrics s =
      screen_barycentrics(tri, pixel_center, view_proj, camera, width, height);
  Surface surface = surface_at(geo, instance, tri, s.b, camera);
  from_eye = s.b.x * (tri.p[0] - camera) + s.b.y * (tri.p[1] - camera) +
             s.b.z * (tri.p[2] - camera);
  surface.position = camera + from_eye;
  return surface;
}

// The UV's derivatives across the screen at `pixel_center`, taken from the triangle itself rather
// than from neighbouring fragments (docs/subsystems/texture.md, "Mip selection without quads").
// A visibility-buffer resolve is a fullscreen pass: the 2x2 quad a derivative instruction would
// difference holds pixels of *other* triangles wherever an edge crosses it — often other meshes,
// other UV islands — so ddx(uv) there is garbage and a texture fetch picks a level from it. The
// triangle's own derivatives are exact: uv = sum b_i uv_i, so duv = sum db_i uv_i, with the
// barycentrics' derivatives `screen_barycentrics` gives. They sum to zero, so the UVs enter as
// edges from the first, which keeps a UV a thousand tiles out as sharp as one at the origin.
// Zero for a triangle with no attributes or one seen exactly edge-on, which SampleGrad reads as
// level 0.
void screen_uv_gradients(SceneGeometry geo, TriangleFetch tri, float2 pixel_center,
                         column_major float4x4 view_proj, float3 eye, uint width, uint height,
                         out float2 duv_dx, out float2 duv_dy) {
  duv_dx = float2(0.0, 0.0);
  duv_dy = float2(0.0, 0.0);
  if (geo.attributes == 0) return;
  const ScreenBarycentrics s =
      screen_barycentrics(tri, pixel_center, view_proj, eye, width, height);
  VertexAttributes* attributes = (VertexAttributes*)geo.attributes;
  const float2 uv0 = decode_half2(attributes[tri.local[0]].uv_half2);
  const float2 e1 = decode_half2(attributes[tri.local[1]].uv_half2) - uv0;
  const float2 e2 = decode_half2(attributes[tri.local[2]].uv_half2) - uv0;
  duv_dx = s.db_dx.y * e1 + s.db_dx.z * e2;
  duv_dy = s.db_dy.y * e1 + s.db_dy.z * e2;
}

// The surface point's world-space change per pixel along the screen's two axes at `pixel_center`:
// `screen_uv_gradients` with the triangle's positions in place of its UVs — the barycentrics'
// derivatives times the three corners. It is the footprint the ground's detail is filtered by
// (ground_detail.slang), from the triangle alone for the reason the UV derivatives are: the 2x2
// quad a derivative instruction would difference straddles other triangles in a fullscreen pass.
// Zero for a triangle seen exactly edge-on, which reads as a footprint of nothing: the unfiltered
// detail.
void screen_position_gradients(TriangleFetch tri, float2 pixel_center,
                               column_major float4x4 view_proj, float3 eye, uint width,
                               uint height, out float3 dp_dx, out float3 dp_dy) {
  const ScreenBarycentrics s =
      screen_barycentrics(tri, pixel_center, view_proj, eye, width, height);
  // The derivatives sum to zero, so the corners enter as edges from the first: a terrain's corners
  // are thousands of metres from the origin and their differences are what the answer is made of.
  const float3 e1 = tri.p[1] - tri.p[0];
  const float3 e2 = tri.p[2] - tri.p[0];
  dp_dx = s.db_dx.y * e1 + s.db_dx.z * e2;
  dp_dy = s.db_dy.y * e1 + s.db_dy.z * e2;
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
  // The metallic-roughness map's alpha, 1 without the map: a `k_material_ground_detail` material's
  // sand share, which its detail is weighted by. Every other material ignores it (glTF gives the
  // map's alpha no meaning, and an image without one reads 1).
  float detail_mask;
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
  sampled.detail_mask = 1.0;
  if (material.metallic_roughness_texture != k_no_texture) {
    const uint s = slot_sampler(material, material.samplers_mr_normal, 0);
    const float4 mr = g_textures[NonUniformResourceIndex(material.metallic_roughness_texture)]
                          .SampleLevel(g_samplers[NonUniformResourceIndex(s)], uv, 0);
    sampled.roughness *= mr.g;
    sampled.metallic *= mr.b;
    sampled.detail_mask = mr.a;
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
  sampled.detail_mask = 1.0;
  if (material.metallic_roughness_texture != k_no_texture) {
    const uint s = slot_sampler(material, material.samplers_mr_normal, 0);
    const float4 mr =
        g_textures[NonUniformResourceIndex(material.metallic_roughness_texture)].SampleGrad(
            g_samplers[NonUniformResourceIndex(s)], uv, duv_dx, duv_dy);
    sampled.roughness *= mr.g;
    sampled.metallic *= mr.b;
    sampled.detail_mask = mr.a;
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
// indirect term (the sky-and-ground hemisphere today, Phase 2's indirect lighting when it
// exists): an occlusion map is a baked estimate of how much of the hemisphere a point sees, which
// is what direct light's shadows and a path tracer's bounces already compute, so applying it to
// either would count the same occlusion twice (docs/subsystems/texture.md, "Occlusion").
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
  // A light at infinity (gfx::k_light_directional): its direction is `position_radius.xyz`, and it
  // arrives everywhere with its colour and no falloff; its shadow ray runs to infinity. What a sky
  // does to it on the way — the air's transmittance — is the caller's (sky.slang, `sky_light_at`).
  if (light.direction_type.w >= 1.5) {
    direction = normalize(light.position_radius.xyz);
    radiance = light.color_intensity.rgb * light.color_intensity.w;
    distance = 1e30;
    return true;
  }
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

// ---- the environment: the sky above the horizon, the ground below it ---------------------------
//
// What the resolve's indirect term stands for and what the path tracer's escaped rays pick up, so
// that the two agree in the limit as far as the model allows (docs/subsystems/renderer.md, "The sky
// above, the ground below"): two **uniform** halves. Uniform is what makes the resolve's blend
// exact — for two uniform halves the cosine-weighted mean a Lambertian surface of normal n sees is
// the lerp `brdf_hemisphere_ambient` computes, at every n — so in open space the two integrators'
// diffuse ambient is one number and what is left between them is occlusion, interreflection and the
// specular lobe's shape.
//
// Until 2026-09-25 the environment was one ramp over the whole sphere, `sky * lerp(0.15, 0.35,
// d.y / 2 + 1/2)`, which the resolve evaluated at the normal: 0.35 of the sky colour facing up,
// 0.15 facing down. Its cosine-weighted mean is `sky * (0.25 + 0.067 n.y)`, so the resolve was 10%
// bright facing up and 18% dark facing down against its own reference, exact only facing sideways.
// The upper half keeps the old top, 0.35, so every surface that faces up draws what it always drew.

// The sky's radiance, the same in every direction above the horizon: 0.35 of the sky colour, the
// ambient an up-facing surface takes and the sky's share of the light on the ground.
float3 sky_radiance(float3 sky) { return sky * 0.35; }

// The ground below the horizon (`brdf_ground_radiance`): the scene's ground albedo under this
// frame's sun and sky. Both integrators compute it from the same three numbers, so the resolve's
// lower half and the reference's are one radiance.
float3 ground_radiance(float3 sky, float3 ground_albedo, float4 sun) {
  return brdf_ground_radiance(ground_albedo, sun.xyz, sun.w, sky_radiance(sky));
}

// The environment an escaped ray sees in the reference: the sky above the horizon and the ground
// below it.
float3 environment_radiance(float3 sky, float3 ground, float3 direction) {
  return direction.y >= 0.0 ? sky_radiance(sky) : ground;
}

// The resolve's indirect light at a shading normal: that environment's cosine-weighted mean, the
// sky and the ground blended by the share of the normal's cosine lobe each half fills
// (`brdf_hemisphere_ambient`).
float3 ambient_radiance(float3 sky, float3 ground, float3 normal) {
  return brdf_hemisphere_ambient(sky_radiance(sky), ground, normal.y);
}

// Linear to what a UNORM target holds is `display_encode` in display.slang, included above: both
// integrators end on it (through `display_output`, with the dither), so a reference image and a
// real-time image are in the same space and FLIP compares pictures rather than transfer curves.

#endif  // ENGINE_MATERIAL_SLANG

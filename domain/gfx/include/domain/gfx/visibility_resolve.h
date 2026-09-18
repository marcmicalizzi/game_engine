#pragma once

// Parameters of the visibility resolve (shaders/visibility_resolve.slang): the first material
// pass. Read through a device address; the fragment pass pushes only the address. A pixel's id
// is `visible_index << 8 | triangle`, so the resolve reads `visible[visible_index]` to recover
// the (instance, cluster) pair the rasterizer drew, decodes the positions off that instance's
// mesh's 16-bit grid (`MeshDesc` in cluster_cull.h) and transforms them by the instance's world
// matrix: exactly the triangle that was drawn. Materials are a flat table indexed per cluster
// plus the instance's `material_base`, which is enough until the material graph arrives. Shading
// is the physically based BSDF of shaders/brdf.slang (GGX specular, Lambert diffuse) under a
// directional sun, a list of analytic lights, and a sky hemisphere. A light whose bit is set in
// `shadow_flags` is shadowed by a ray query against the top-level structure in bindless slot
// `scene` — the one the frame built from the same visible list the rasterizer drew from, so the
// shadows are cast by exactly the geometry in the picture.

#include <core/base/types.h>
#include <core/math/math.h>

namespace engine::gfx {

inline constexpr u32 k_no_texture = 0xFFFFFFFFu;

// ResolveParams::scene when the frame has no top-level structure: no shadow ray is traced.
inline constexpr u32 k_no_scene = 0xFFFFFFFFu;

// ResolveParams::shadow_flags.
inline constexpr u32 k_shadow_sun = 1u;     // the directional sun casts
inline constexpr u32 k_shadow_lights = 2u;  // the analytic point and spot lights cast

// Mirrors ResolveMaterial in visibility_resolve.slang. 64 bytes. Every texture slot is a
// bindless sampled image read with the one `sampler` at `uv * uv_scale`. The albedo texture is
// uploaded sRGB, so sampling returns linear color; the metallic-roughness and normal textures
// are data, not color, and are uploaded UNORM.
struct ResolveMaterial {
  Vec4 albedo{0.8f, 0.8f, 0.8f, 0.5f};  // rgb linear, w roughness
  Vec4 emissive{};                      // rgb linear, w metallic
  u32 albedo_texture = k_no_texture;    // multiplies albedo
  u32 sampler = 0;                      // bindless sampler slot
  f32 uv_scale = 1.0f;
  u32 flags = 0;
  u32 metallic_roughness_texture = k_no_texture;  // glTF packing: G roughness, B metallic
  u32 normal_texture = k_no_texture;              // tangent space, UNORM, remapped to -1..1
  f32 normal_scale = 1.0f;                        // scales the map's xy before it is normalized
  u32 pad = 0;
};
static_assert(sizeof(ResolveMaterial) == 64);

inline constexpr f32 k_light_point = 0.0f;  // ResolveLight::direction_type.w
inline constexpr f32 k_light_spot = 1.0f;

// One analytic light. Mirrors ResolveLight in visibility_resolve.slang. 64 bytes.
// The falloff is inverse square windowed to zero at the radius of influence (Karis), so a light
// contributes nothing beyond it and culling by radius cannot change the picture.
struct ResolveLight {
  Vec4 position_radius{};  // xyz world position, w radius of influence
  Vec4 color_intensity{};  // rgb linear color, w radiant intensity scale
  Vec4 direction_type{};   // xyz spot direction (towards the lit surface), w k_light_point/_spot
  Vec4 spot{};             // x cos inner angle, y cos outer angle, zw unused
};
static_assert(sizeof(ResolveLight) == 64);

enum class ResolveMode : u32 {
  ClusterColors = 0,
  TriangleShade = 1,
  Depth = 2,
  Shaded = 3,
  Normals = 4,
  Uvs = 5,
};

// Mirrors ResolveParams in visibility_resolve.slang. 224 bytes.
struct ResolveParams {
  Vec4 sky{};     // rgb shown for empty pixels and used as the hemisphere ambient
  Vec4 sun{};     // xyz normalized direction towards the light, w intensity
  Vec4 camera{};  // xyz position
  Mat4 view_proj;
  u64 visibility = 0;         // u64[width * height]
  u64 clusters = 0;           // geometry::ClusterDesc[]
  u64 mesh = 0;               // MeshDesc[] (cluster_cull.h): the positions and each mesh's grid
  u64 triangles = 0;          // u32[]: packed local indices
  u64 materials = 0;          // ResolveMaterial[]
  u64 cluster_materials = 0;  // u32[cluster_count]
  u64 attributes = 0;         // geometry::VertexAttributes[] parallel to vertices; 0 = flat shading
  u64 lights = 0;             // ResolveLight[light_count]
  u32 width = 0;
  u32 height = 0;
  u32 mode = static_cast<u32>(ResolveMode::Shaded);
  u32 light_count = 0;
  u64 instances = 0;       // InstanceDesc[] (cluster_cull.h)
  u64 visible = 0;         // u32x2[]: the cull pass's visible list; 0 reads {0, visible_index}
  u32 scene = k_no_scene;  // bindless slot of the top-level structure the shadow rays trace
  u32 shadow_flags = 0;    // k_shadow_sun | k_shadow_lights; 0 traces nothing
  f32 shadow_bias = 0.0f;  // world units along the geometric normal, off the surface
  u32 pad = 0;             // keeps the block 16-byte aligned
};
static_assert(sizeof(ResolveParams) == 224);
static_assert(sizeof(ResolveParams) % 16 == 0, "the block is read as float4 rows on the GPU");

}  // namespace engine::gfx

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
//
// **One resolve per view.** A `renderer::ViewSet` (docs/plan/04-renderer.md §4.6) puts N views in
// one color target, so the pass runs once per view with that view's viewport rectangle and that
// view's region of the visibility buffer. `view_x`/`view_y` are the rectangle's origin, which the
// shader subtracts from `SV_Position` to get back to a view-local pixel, `out_width`/`out_height`
// are the rectangle's extent, and `width`/`height` stay the extent of the visibility region the
// ids were rasterized into — the same number for every view that draws rectilinearly, and a wider
// one for a **Panini** view, whose rectilinear source is oversampled and resampled here.

#include <core/base/types.h>
#include <core/math/math.h>

#include <cmath>

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

// The Panini projection of parameter d (docs/plan/04-renderer.md §4.6, experiment E9), as the
// resolve resamples it. A direction at azimuth `theta` from the view axis lands at image
// abscissa `x = (d + 1) * sin(theta) / (d + cos(theta))`; d = 0 is `tan(theta)`, the ordinary
// rectilinear projection, and larger d compresses the periphery instead of stretching it.
inline f32 panini_abscissa(f32 d, f32 theta) noexcept {
  return (d + 1.0f) * std::sin(theta) / (d + std::cos(theta));
}

// How much wider the rectilinear source has to be than the Panini picture it is resampled into,
// for the same horizontal field of view and no magnification anywhere. Both projections have unit
// angular scale on the view axis (d(x)/d(theta) = 1 at theta = 0), and rectilinear stretches the
// periphery harder than Panini does, so the worst case is the *centre* and the factor is simply
// the ratio of the two half-widths: `tan(theta_max) / panini_abscissa(d, theta_max)`.
inline f32 panini_oversample(f32 d, f32 half_fov_x) noexcept {
  const f32 panini = panini_abscissa(d, half_fov_x);
  return panini > 0.0f ? std::tan(half_fov_x) / panini : 1.0f;
}

// Mirrors ResolveParams in visibility_resolve.slang. 256 bytes.
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
  // This view's rectangle of the color target: the origin the shader takes off SV_Position, and
  // the extent of the picture. A rectilinear view's picture is exactly as big as the visibility
  // region it resolves, so the shader reads `out_width`/`out_height` **only when the view
  // resamples** and a caller that draws one rectilinear view leaves all four zero, as every caller
  // before multi-view did.
  u32 view_x = 0;
  u32 view_y = 0;
  u32 out_width = 0;
  u32 out_height = 0;
  // Panini resampling, off when `panini_x` is 0. `panini_x` is the picture's half-width in image
  // units, `source_x` the rectilinear source's (`tan` of half the horizontal field of view), and
  // the two are equal only at d = 0. The vertical scale needs no parameter: it cancels.
  f32 panini_d = 0.0f;
  f32 panini_x = 0.0f;
  f32 source_x = 0.0f;
  // 1: the pass cleared the color target to exactly `sky` before it drew, so a pixel the scene
  // does not cover already holds the value the shader would write. The shader then **discards**
  // that fragment instead of writing it, which at 11520x2160 is a 100 MB color write the frame
  // no longer makes; the picture is identical because the bytes under it already are. Zero is
  // the safe default and means "write the sky", which is what a caller that clears to something
  // else — `domain/gfx`'s resolve tests clear to black and read the sky back out of the
  // picture — needs. Whoever sets it owns the clear value beside it.
  u32 sky_is_clear = 0;
  u32 pad = 0;
  // One u32 per 32 x 32 tile of this view's visibility region, non-zero when the tile holds any
  // surface; 0 reads the visibility word for every pixel, as the pass always did. It is the Hi-Z
  // build's by-product (`hiz_build.slang`), so it costs nothing to produce and exists only while
  // two-pass occlusion culling is on — which is also the only time it is *valid*, because that
  // is when the last write to the visibility buffer is the pass-2 draw and the last Hi-Z build
  // follows it. A tile the mask calls empty is not read at all, and reading a 64-bit word per
  // empty pixel was the resolve's whole fixed cost.
  u64 coverage = 0;
  u32 coverage_pitch = 0;  // tiles per row: gfx::hiz_coverage_pitch(width)
  u32 pad2 = 0;            // keeps the block 16-byte aligned
  // The **deformed normal pool**: one octahedral `u32` per vertex of the frame's deformed-vertex
  // pool, parallel to it and written by the same pass, in exactly the packing
  // `geometry::VertexAttributes::normal_oct` uses — so the shader decodes it with the function it
  // already had. Zero, and every shading normal comes off the rest attribute stream as it always
  // has; that is what a frame with no morph channels passes, which is why every existing picture
  // is byte-identical. Read only for a pixel whose visible entry actually has a pool block, so a
  // rigid instance in a morphed scene is unaffected too (gfx.md, "What happens to the shading
  // normal").
  u64 normal_pool = 0;
  u64 pad3 = 0;  // keeps the block 16-byte aligned
};
static_assert(sizeof(ResolveParams) == 288);
static_assert(sizeof(ResolveParams) % 16 == 0, "the block is read as float4 rows on the GPU");

}  // namespace engine::gfx

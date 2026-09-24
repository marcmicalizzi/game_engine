#pragma once

// Parameters of the visibility resolve (shaders/visibility_resolve.slang): the first material
// pass. Read through a device address; the fragment pass pushes only the address. A pixel's id
// is `pair << 8 | triangle`, the scene's pair, so the resolve reads the scene's pair table
// (`pairs`) to recover the (instance, cluster) the rasterizer drew — and, for a deformed instance,
// this view's `pair_entries` for the visible entry whose pool block holds its positions — decodes
// the positions off that instance's mesh's 16-bit grid (`MeshDesc` in cluster_cull.h) and
// transforms them by the instance's world matrix: exactly the triangle that was drawn. Materials
// are a flat table indexed per cluster plus the instance's `material_base`, which is enough until
// the material graph arrives. Shading is the physically based BSDF of shaders/brdf.slang (GGX
// specular, Lambert diffuse) under a directional sun, a list of analytic lights, and a sky
// hemisphere. A light whose bit is set in `shadow_flags` is shadowed by a ray query against the
// top-level structure in bindless slot `scene` — the one the frame built from the same visible list
// the rasterizer drew from, so the shadows are cast by exactly the geometry in the picture — or,
// with `k_shadow_cascades`, the sun is shadowed by the cascaded depth maps behind `shadow_maps`,
// filtered (the baseline tier's shadow, for a device without ray queries).
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

// ResolveParams::shadow_flags. The first two say **which** lights are shadowed, the third **how**:
// the resolve's shadow query is one function with two implementations, a ray query against the
// top-level structure in `scene`, and a filtered lookup in the cascaded shadow maps behind
// `shadow_maps` when `k_shadow_cascades` is set. The maps shadow the sun alone; with the bit set
// `k_shadow_lights` is ignored, so the point and spot lights are unshadowed under the maps.
inline constexpr u32 k_shadow_sun = 1u;       // the directional sun casts
inline constexpr u32 k_shadow_lights = 2u;    // the analytic point and spot lights cast
inline constexpr u32 k_shadow_cascades = 4u;  // the sun's shadow comes from the cascaded maps

// ---- cascaded shadow maps (docs/plan/04-renderer.md §4.4, docs/subsystems/renderer.md) --------
//
// The baseline tier's sun shadow: depth maps rendered from the light by the same cull pass and the
// same rasterizers the visibility buffer uses, one per cascade, side by side in one depth atlas
// (`resolution` texels square each, cascade c at x = c * resolution), and read by the resolve with
// a filter. The CPU fits the cascades (renderer::fit_shadow_cascades); this is the block the
// resolve reads them through and the arithmetic both sides share.
inline constexpr u32 k_max_shadow_cascades = 4;

// The light's frame, shared by every cascade: `direction` points **towards** the light (the sun
// vector of ResolveParams), `right` and `up` span the plane across it. A cascade's texels are laid
// along `right` (columns) and `-up` (rows, because raster passes flip the viewport), and its depth
// along `direction`.
struct ShadowLight {
  Vec3 direction{0.0f, 1.0f, 0.0f};
  Vec3 right{1.0f, 0.0f, 0.0f};
  Vec3 up{0.0f, 0.0f, -1.0f};
};

inline ShadowLight shadow_light(Vec3 towards_light) noexcept {
  ShadowLight light;
  light.direction = normalize(towards_light);
  // Any vector across the direction will do; world up unless the sun is overhead, where it is
  // parallel, and then world x. The frame only has to be the same every frame, which it is for a
  // fixed sun, so the texel grid below does not rotate under a still light.
  const Vec3 reference =
      std::fabs(light.direction.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
  light.right = normalize(cross(reference, light.direction));
  light.up = cross(light.direction, light.right);
  return light;
}

// Moves `center` across the light so that its coordinates along `right` and `up` are whole
// multiples of `texel`. A cascade that moves with the camera then moves by whole texels, and a
// static caster lands on the same texels frame after frame instead of shimmering along its edges.
inline Vec3 shadow_snap(const ShadowLight& light, Vec3 center, f32 texel) noexcept {
  if (!(texel > 0.0f)) return center;
  const f32 x = dot(center, light.right);
  const f32 y = dot(center, light.up);
  const f32 sx = std::round(x / texel) * texel;
  const f32 sy = std::round(y / texel) * texel;
  return center + light.right * (sx - x) + light.up * (sy - y);
}

// Mirrors ShadowCascade in visibility_resolve.slang. 80 bytes.
struct ShadowCascade {
  // World to this cascade's clip space: x and y in [-1, 1] across its tile of the atlas, depth in
  // [0, 1] with **1 towards the light** (reversed, like every depth in this engine), w = 1.
  Mat4 view_proj;
  f32 texel_world = 0.0f;  // world units a texel spans, across the light
  f32 depth_range = 0.0f;  // world units from depth 0 to depth 1, along the light
  // Depth units the receiver's reference is moved towards the light before it is compared: the
  // constant half of the bias (the receiver-plane half is computed per texel in the shader).
  f32 bias = 0.0f;
  f32 pad = 0.0f;
};
static_assert(sizeof(ShadowCascade) == 80);

// The cascade that covers the receivers inside the sphere (`center`, `radius`) across the light.
// Its depth runs from `radius` beyond `center`, away from the light, to `towards` world units on
// the light's side of it — far enough to hold every caster between the light and those receivers,
// which is why a caster outside the camera's frustum still casts. `bias_texels` is the constant
// bias in texels of this cascade's own size, converted to depth units here.
inline ShadowCascade make_shadow_cascade(const ShadowLight& light, Vec3 center, f32 radius,
                                         f32 towards, u32 resolution, f32 bias_texels) noexcept {
  ShadowCascade out;
  const f32 inv_r = 1.0f / radius;
  const f32 z_lo = dot(light.direction, center) - radius;
  const f32 range = towards + radius;
  const f32 inv_range = 1.0f / range;
  Mat4 m;
  m.at(0, 0) = light.right.x * inv_r;
  m.at(0, 1) = light.right.y * inv_r;
  m.at(0, 2) = light.right.z * inv_r;
  m.at(0, 3) = -dot(light.right, center) * inv_r;
  m.at(1, 0) = light.up.x * inv_r;
  m.at(1, 1) = light.up.y * inv_r;
  m.at(1, 2) = light.up.z * inv_r;
  m.at(1, 3) = -dot(light.up, center) * inv_r;
  m.at(2, 0) = light.direction.x * inv_range;
  m.at(2, 1) = light.direction.y * inv_range;
  m.at(2, 2) = light.direction.z * inv_range;
  m.at(2, 3) = -z_lo * inv_range;
  m.at(3, 0) = 0.0f;
  m.at(3, 1) = 0.0f;
  m.at(3, 2) = 0.0f;
  m.at(3, 3) = 1.0f;
  out.view_proj = m;
  out.texel_world = 2.0f * radius / static_cast<f32>(resolution);
  out.depth_range = range;
  out.bias = bias_texels * out.texel_world * inv_range;
  return out;
}

// Mirrors ShadowMapParams in visibility_resolve.slang, read through `ResolveParams::shadow_maps`.
// 400 bytes.
struct ShadowMapParams {
  ShadowCascade cascades[k_max_shadow_cascades];
  Vec4 light_right{};  // xyz: ShadowLight::right
  Vec4 light_up{};     // xyz: ShadowLight::up
  Vec4 light_dir{};    // xyz: ShadowLight::direction, towards the light
  u32 cascade_count = 0;
  u32 resolution = 0;          // texels per cascade side; the atlas is count x resolution wide
  u32 texture = k_no_texture;  // bindless sampled image: the depth atlas
  u32 sampler = 0;             // bindless sampler: nearest, clamped
  // The receiver plane's depth slope, in world units along the light per world unit across it, is
  // clamped to this: near grazing the plane's prediction a few texels away means nothing.
  f32 max_slope = 0.0f;
  // The atlas's width in tiles, which is the cascade count it was **allocated** for. A frame may
  // use fewer cascades than that (renderer::fit_shadow_cascades ends early when one holds the
  // whole scene), and the resolve's texel addresses are fractions of the atlas's real width.
  u32 tiles = 0;
  // How far the lookup point leaves the surface along its geometric normal, in texels of the
  // cascade it is read from, at a grazing or back-facing surface; `(1 - n.l)` of it elsewhere.
  f32 normal_offset = 0.0f;
  u32 pad = 0;
};
static_assert(sizeof(ShadowMapParams) == 400);
static_assert(sizeof(ShadowMapParams) % 16 == 0, "the block is read as float4 rows on the GPU");

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
  // The sun's shadow alone: white where it reaches the surface, black where it is blocked, mid
  // grey where the surface faces away and nothing is asked. What the shaded view's sun term would
  // have seen, as a picture that can be counted (renderer.md, "Shadows"). A ray answers black or
  // white; the cascaded maps answer the filtered fraction, grey levels across a penumbra.
  Shadow = 6,
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

// Mirrors ResolveParams in visibility_resolve.slang. 304 bytes.
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
  u64 visible = 0;         // u32x2[]: the cull pass's visible list; 0 reads {0, pair}
  u32 scene = k_no_scene;  // bindless slot of the top-level structure the shadow rays trace
  u32 shadow_flags = 0;    // k_shadow_sun | k_shadow_lights [| k_shadow_cascades]; 0: no shadows
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
  // **This view's pair-to-entry table** (`CullParams::pair_entries`). A pixel's id is the scene's
  // pair and triangle (docs/subsystems/gfx.md, "The tie rule"), and the entry the pair was drawn
  // as this frame is where a deformed instance's pool block is found, so the resolve reads this
  // only for a pixel of a deformed instance. 0 with a null `visible`, where the pair is the entry.
  u64 pair_entries = 0;
  // **The scene's pair table**: u32x2 {instance, cluster} per pair, the inverse of `pair_of`,
  // which is how a pixel's id is decoded in one load. 0 decodes through `pair_entries` and
  // `visible` instead, two dependent loads, which is what a caller without the table gets.
  u64 pairs = 0;
  // **The cascaded shadow maps** (`ShadowMapParams`), read when `shadow_flags` has
  // `k_shadow_cascades`: the sun's shadow query then filters the maps instead of tracing a ray.
  // It took the pad word that kept the block 16-byte aligned, so the size is unchanged, and it is
  // zero for every caller that does not draw maps.
  u64 shadow_maps = 0;
};
static_assert(sizeof(ResolveParams) == 304);
static_assert(sizeof(ResolveParams) % 16 == 0, "the block is read as float4 rows on the GPU");

}  // namespace engine::gfx

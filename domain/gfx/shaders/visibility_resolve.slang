// Fullscreen resolve of the 64-bit visibility buffer (docs/plan/04-renderer.md §4.3 step 4):
// the first material pass. A pixel's id is `pair << 8 | triangle`, the scene's pair (scene.slang,
// `pair_of`); the scene's pair table gives back its (instance, cluster) — and, for a deformed
// instance only, this view's pair-to-entry table the visible entry whose pool block holds its
// positions; the cluster and the triangle lead
// to the three vertices, whose positions come off the 16-bit grid of that instance's mesh — or,
// for a deformed instance, out of the frame's deformed-vertex pool (MeshDesc, load_position) —
// and go through the instance's world matrix, exactly as the
// rasterizer moved them. The barycentrics of the point where the ray through the pixel center
// meets that triangle (material.slang, `screen_barycentrics`: homogeneous, no divide by w, so a
// triangle reaching behind the camera is rebuilt as rightly as one in front of it) reconstruct
// the surface position and interpolate the per-vertex normal and UV (octahedral snorm16 normals,
// half-float UVs, from geometry::VertexAttributes; without attributes the triangle plane gives a
// flat normal), and
// normals go through the instance's cofactor matrix so a non-uniform scale stays perpendicular.
// The cluster's material index plus the instance's `material_base` selects albedo, roughness,
// metallic, and emissive, and up to three optional textures from the bindless set: albedo, a
// glTF metallic-roughness map (G roughness, B metallic) that multiplies both factors, and a
// tangent-space normal map that perturbs the shading normal in a frame solved from the hit
// triangle's own world-space positions and UVs. The shared BSDF (brdf.slang) then shades a
// directional sun, N analytic point and spot lights, and a hemisphere of sky above and lit
// ground below. Debug modes show cluster colors, triangle shading, depth, the interpolated normal
// before any map, or UVs.
// Empty pixels (value 0: depth 0 is the far plane and id 0 never occurs there) take the sky
// color.
//
// **The material layer is shared, not copied** (docs/plan/04-renderer.md §4.8): the structs, the
// triangle fetch and the attribute interpolation, the tangent frame, the texture reads, the light
// falloffs, the sky and the display transform all live in `material.slang`, which the reference
// path tracer includes too, and so does the reconstruction a rasterized pixel needs because it
// carries no barycentrics (`reconstruct_screen`), which the path tracer does not call. What is
// left in this file is the part that is the *resolve's*: the visibility-buffer decode, the calls
// into that reconstruction, the debug view modes, the Panini resample, and the shadow rays.
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
#include "ground_detail.slang"
#include "material.slang"
#include "scene.slang"
#include "sky.slang"

#ifdef RESOLVE_RAY_SHADOWS
[[vk::binding(3, 0)]] RaytracingAccelerationStructure g_scenes[];
#endif

// One resolve per view (docs/plan/04-renderer.md §4.6): the pass runs once for each view of a
// `renderer::ViewSet` with that view's viewport rectangle, so `SV_Position` is in the *target's*
// pixels and `view_x`/`view_y` take it back to a view-local pixel of that view's own region of
// the visibility buffer. A **Panini** view goes one step further: its region holds a wider
// rectilinear picture, and `panini_x`/`source_x`/`panini_d` map each output pixel back into it.
//
// Read through a device address; mirrors gfx::ResolveParams (336 bytes).
struct ResolveParams {
  float4 sky;     // rgb, shown for empty pixels and the hemisphere ambient's upper half
  float4 sun;     // xyz normalized direction towards the light, w intensity
  float4 ground;  // rgb the ground's albedo, the hemisphere ambient's lower half; w unused
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
              // 6 the sun's shadow alone, 7 the textured albedo unlit, 8 the ambient occlusion,
              // 9 the ground's detail as data
  uint light_count;
  uint64_t instances;  // InstanceDesc[]
  uint64_t visible;    // uint2[]: the cull pass's visible list; 0 reads {0, pair}
  uint scene;          // bindless slot of the top-level structure; 0xffffffff for none
  uint shadow_flags;   // bit 0 the sun casts, bit 1 the analytic lights cast
  float shadow_bias;   // world units along the geometric normal, off the surface (ray_offset)
  uint view_x;         // this view's rectangle of the color target
  uint view_y;
  uint out_width;
  uint out_height;
  float panini_d;      // Panini parameter; the resample is off when panini_x is 0
  float panini_x;      // the picture's half-width in image units
  float source_x;      // the rectilinear source's half-width, tan(half the horizontal fov)
  uint sky_is_clear;   // 1: the pass cleared the target to `sky`, so an empty pixel is discarded
  float shadow_bias_steps;  // plus this many steps of the receiver's own grid (ray_offset)
  uint64_t coverage;    // uint[] per 32x32 tile of this view's region; 0: no mask, read every word
  uint coverage_pitch;  // tiles per row of that mask
  uint sky_view;        // this view's index into SkyParams::views
  uint64_t normal_pool;  // deformed normals, parallel to the deformed-vertex pool; 0 for none
  uint64_t pair_entries;  // uint[pair_count]: this view's pair -> visible entry; 0: the pair is it
  uint64_t pairs;         // uint2[pair_count]: the scene's {instance, cluster} per pair; 0: none
  uint64_t shadow_maps;   // ShadowMapParams*, read with k_shadow_cascades; 0: none
  uint64_t ground_detail;  // GroundDetail*, read for a k_material_ground_detail material; 0: none
  uint64_t sky_params;     // SkyParams* (sky.slang): the physical sky; 0: the stand-in
};

// Mirrors gfx::ShadowCascade (80 bytes): one cascade of the sun's depth atlas.
struct ShadowCascade {
  column_major float4x4 view_proj;  // world -> x, y in [-1, 1] over the tile; depth 1 towards the light
  float texel_world;                // world units a texel spans across the light
  float depth_range;                // world units from depth 0 to depth 1
  float bias;                       // depth units the reference moves towards the light
  float pad;
};

// Mirrors gfx::ShadowMapParams (400 bytes).
struct ShadowMapParams {
  ShadowCascade cascades[4];  // gfx::k_max_shadow_cascades
  float4 light_right;
  float4 light_up;
  float4 light_dir;  // towards the light
  uint cascade_count;
  uint resolution;  // texels per cascade side; cascade c is the atlas's columns c * resolution on
  uint texture;     // bindless slot of the depth atlas
  uint sampler;     // bindless slot of a nearest, clamped sampler
  float max_slope;  // the receiver plane's |depth slope| is clamped to this
  uint tiles;       // the atlas's width in cascades, as allocated (>= cascade_count)
  float normal_offset;  // texels along the geometric normal at grazing; (1 - n.l) of it elsewhere
  uint light;           // whose shadow: 0 the sun, i + 1 the directional light i of the array
};

struct ResolvePush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<ResolvePush> g_push;

static const uint k_no_scene = 0xffffffffu;
static const uint k_shadow_sun = 1u;
static const uint k_shadow_lights = 2u;
static const uint k_shadow_cascades = 4u;  // the sun's shadow is read from the cascaded maps

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
//
// **With a physical sky** (`sky_params`, sky.slang) the pixel is written, since no clear value is
// the sky along every direction: the direction through the pixel's centre in the view's own source
// — `source`, which for a Panini view is where the resample sends the output pixel — is looked up in
// the frame's sky-view table, and the sun's disc, the moon's and the stars are added through the air
// (`sky_background`), exposed by the frame's rule. The ray path's uncovered pixels are this too, and
// the reference tracer's primary misses the same function.
float4 sky_pixel(ResolveParams* params, float2 source) {
  if (params->sky_params == 0) {
    if (params->sky_is_clear != 0) discard;
    return params->sky;
  }
  SkyParams* sky = (SkyParams*)params->sky_params;
  const float2 ndc = float2(source.x / float(params->width) * 2.0 - 1.0,
                            1.0 - source.y / float(params->height) * 2.0);
  const float3 dir = sky_pixel_direction(sky, params->sky_view, ndc);
  const float3 radiance = sky_background(sky, dir, sky->views[params->sky_view].pixel.x);
  return float4(display_encode(sky_tone(sky, radiance * sky_exposure((SkyFrame*)sky->frame))),
                1.0);
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

// ---- the cascaded shadow maps (docs/subsystems/renderer.md, "Shadows") --------------------------
//
// One 2x2 block of the filter: the four depths a gather returned, compared against the receiver's
// plane at each texel's centre. `z` is the reference at the block's first texel (lowest column,
// lowest row), and `dzdi`/`dzdj` are the plane's depth per texel along the columns and the rows.
// Vulkan's gather order is x = (i0, j1), y = (i1, j1), z = (i1, j0), w = (i0, j0). A texel is lit
// where its caster is no nearer the light than the plane (depth 1 is the light's side).
float shadow_block(float4 g, float z, float dzdi, float dzdj, float wx0, float wx1, float wy0,
                   float wy1) {
  return wx0 * wy0 * step(g.w, z) + wx1 * wy0 * step(g.z, z + dzdi) +
         wx0 * wy1 * step(g.x, z + dzdj) + wx1 * wy1 * step(g.y, z + dzdi + dzdj);
}

// The sun's visibility at `position` through the cascaded depth maps: 1 where it reaches the
// surface, 0 where casters nearer the light cover the whole filter, and the covered fraction in
// between. `geometric` is the triangle's own normal, turned towards the camera.
//
// **Which cascade:** the first whose tile holds the filter's whole footprint and whose depth range
// holds the point. The cascades are nested outwards from the camera, so the first that holds it is
// the finest; a point beyond the last is lit, because there is nothing known about it.
//
// **The filter** is 3x3 bilinear PCF: nine bilinear taps a texel apart, which is a 4x4 texel
// footprint with the separable weights (1 - f, 1, 1, f) / 3 along each axis, f the point's
// fraction of a texel. It is evaluated as four gathers rather than nine comparison samples so
// that every one of the sixteen texels can be compared against its own reference, which is what
// the bias needs.
//
// **The bias is the receiver's plane.** Each texel is compared against the depth the receiver's
// plane has at that texel's centre, read off the geometric normal in the light's frame — which is
// exactly the depth the rasterizer wrote there for the receiver's own triangle — so a flat surface
// never shadows itself at any angle and the kernel's width costs no extra bias. On top of it,
// `ShadowCascade::bias` (a constant in texels of the cascade) covers what is not planar: curvature
// across the footprint, a neighbouring triangle, and the float error between the reconstruction
// and the rasterizer. The slope is clamped at `max_slope`, which only a grazing surface reaches.
//
// **The normal offset is for what is not planar.** The point looked up leaves the surface along
// its geometric normal by `normal_offset` texels of the cascade times (1 - N.L): nothing on a
// surface facing the light, where the receiver's plane is exact, and the whole offset at grazing
// and beyond. Without it a low-poly curved surface shadows itself twice over: in every concave
// crease, where the neighbouring triangle rises above the receiver's plane inside the footprint,
// and along the terminator, where a triangle whose geometric normal just faces away from the sun
// still has a shading normal that faces it. A shadow ray lifted off such a triangle skims the
// convex surface and reaches the sun, and a lookup lifted clear of the footprint does too, so the
// two implementations answer alike there (docs/subsystems/renderer.md, "Shadows", has the counts
// on the Khronos samples, and the offset they chose). With no offset a back-facing surface is its
// own shadow.
float cascade_visibility(ShadowMapParams* maps, float3 position, float3 geometric) {
  const float n_d = dot(geometric, maps->light_dir.xyz);
  if (n_d <= 0.0 && maps->normal_offset <= 0.0) return 0.0;
  const float res = float(maps->resolution);
  const float2 atlas = float2(res * float(maps->tiles), res);
  const float inv_n = 1.0 / max(n_d, 1.0e-4);
  const float slope_r =
      clamp(-dot(geometric, maps->light_right.xyz) * inv_n, -maps->max_slope, maps->max_slope);
  const float slope_u =
      clamp(-dot(geometric, maps->light_up.xyz) * inv_n, -maps->max_slope, maps->max_slope);
  const float lift = maps->normal_offset * saturate(1.0 - n_d);
  for (uint c = 0; c < maps->cascade_count; ++c) {
    const ShadowCascade cascade = maps->cascades[c];
    const float3 lifted = position + geometric * (lift * cascade.texel_world);
    const float4 clip = mul(cascade.view_proj, float4(lifted, 1.0));
    if (clip.z < 0.0 || clip.z > 1.0) continue;
    // Texel-centre coordinates in the tile (texel k's centre is at k); rows run down the tile,
    // because the raster pass that drew it flips the viewport.
    const float2 t = float2((clip.x * 0.5 + 0.5) * res, (0.5 - clip.y * 0.5) * res) - 0.5;
    const float2 base = floor(t);
    if (base.x < 1.0 || base.y < 1.0 || base.x > res - 3.0 || base.y > res - 3.0) continue;
    const float2 f = t - base;
    // Depth per texel along the columns (the light's right) and the rows (its down).
    const float scale = cascade.texel_world / cascade.depth_range;
    const float dzdi = slope_r * scale;
    const float dzdj = -slope_u * scale;
    // The reference at the footprint's first texel, (base - 1, base - 1).
    const float z = clip.z + cascade.bias + dzdi * (base.x - 1.0 - t.x) +
                    dzdj * (base.y - 1.0 - t.y);
    // A gather at a texel corner returns the 2x2 texels around it: these four cover the 4x4.
    // The atlas and its sampler are the same for every pixel of the pass, so the indices are
    // uniform and need no NonUniformResourceIndex.
    const float x0 = float(c) * res + base.x;
    const uint tex = maps->texture;
    const uint smp = maps->sampler;
    const float4 g00 = g_textures[tex].Gather(g_samplers[smp], float2(x0, base.y) / atlas);
    const float4 g10 = g_textures[tex].Gather(g_samplers[smp], float2(x0 + 2.0, base.y) / atlas);
    const float4 g01 = g_textures[tex].Gather(g_samplers[smp], float2(x0, base.y + 2.0) / atlas);
    const float4 g11 =
        g_textures[tex].Gather(g_samplers[smp], float2(x0 + 2.0, base.y + 2.0) / atlas);
    float lit = shadow_block(g00, z, dzdi, dzdj, 1.0 - f.x, 1.0, 1.0 - f.y, 1.0);
    lit += shadow_block(g10, z + 2.0 * dzdi, dzdi, dzdj, 1.0, f.x, 1.0 - f.y, 1.0);
    lit += shadow_block(g01, z + 2.0 * dzdj, dzdi, dzdj, 1.0 - f.x, 1.0, 1.0, f.y);
    lit += shadow_block(g11, z + 2.0 * (dzdi + dzdj), dzdi, dzdj, 1.0, f.x, 1.0, f.y);
    return lit * (1.0 / 9.0);
  }
  return 1.0;
}

// **The resolve's shadow query: one function, two implementations** (04 §4.4). How much of a
// light reaches the surface, from 0 (blocked) to 1 (lit). `k_shadow_cascades` in `shadow_flags`
// picks the cascaded maps, which shadow the sun alone and answer a fraction at a penumbra;
// otherwise it is the ray query, which answers 0 or 1 and so leaves every ray-traced picture
// exactly what it was. `position` and `geometric` are the reconstructed surface: the ray leaves
// it offset along the geometric normal by `offset` — `ray_offset`'s, `shadow_bias` plus
// `shadow_bias_steps` of the receiver's own grid — and the map is looked up at it unmoved.
//
// The maps are drawn from one light, `ShadowMapParams::light`: the sun (0) — every map before a sky
// was, and the only one the lights array ever had — or, on a sky's night, the moon, a directional
// light of the array (`owner` is the light's index plus one, 0 for the sun).
float shadow_visibility(ResolveParams* params, uint bit, uint owner, float3 position,
                        float3 geometric, float offset, float3 direction, float t_max) {
  if ((params->shadow_flags & bit) == 0) return 1.0;
  if ((params->shadow_flags & k_shadow_cascades) != 0) {
    if (params->shadow_maps == 0) return 1.0;
    ShadowMapParams* maps = (ShadowMapParams*)params->shadow_maps;
    if (maps->light != owner) return 1.0;
    return cascade_visibility(maps, position, geometric);
  }
  const float3 origin = position + geometric * offset;
  return shadow_ray_blocked(params, bit, origin, direction, t_max) ? 0.0 : 1.0;
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
  if (out_pixel.x >= out_width || out_pixel.y >= out_height) {
    return sky_pixel(params, float2(out_pixel) + 0.5);
  }
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
    if (source.x < 0.0 || source.y < 0.0) return sky_pixel(params, source);
  }
  const uint2 pixel = uint2(source);
  if (pixel.x >= params->width || pixel.y >= params->height) return sky_pixel(params, source);
  // A whole 32x32 tile with nothing in it: the visibility word is not read at all, which is the
  // fixed cost the discard above could not touch. The mask is the Hi-Z build's by-product and is
  // exact — the pass that wrote it read every word of the tile — so a tile it calls empty holds
  // no surface and a tile it calls covered is resolved exactly as before. Zero means no mask.
  if (params->coverage != 0) {
    uint* tiles = (uint*)params->coverage;
    if (tiles[(pixel.y >> 5) * params->coverage_pitch + (pixel.x >> 5)] == 0) {
      return sky_pixel(params, source);
    }
  }
  uint64_t* visibility = (uint64_t*)params->visibility;
  const uint64_t value = visibility[pixel.y * params->width + pixel.x];
  if (value == 0) return sky_pixel(params, source);
  const uint id = uint(value & 0xffffffffu);
  const float depth = asfloat(uint(value >> 32));
  // The id's pair is the scene's, and the scene's pair table gives its {instance, cluster} in one
  // load, as `visible[entry]` did when the id was the entry. The entry the pair was drawn as this
  // frame is a second, dependent load, so it is read only where it is needed: for a deformed
  // instance's pool block. Without the table both come from the frame's lists.
  const uint pair = id >> 8;
  uint2 entry;
  if (params->pairs != 0) {
    entry = ((uint2*)params->pairs)[pair];
  } else {
    entry = visible_entry(params->visible, entry_of_pair(params->pair_entries, pair));
  }
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
  const uint visible_index =
      instance.deform == k_invalid_deform ? 0u : entry_of_pair(params->pair_entries, pair);
  const Surface s = reconstruct_screen(geo, instance, visible_index, entry.y, id & 0xff, source,
                                       params->view_proj, params->camera.xyz, params->width,
                                       params->height);
  if (params->mode == 4) return float4(s.normal * 0.5 + 0.5, 1.0);
  if (params->mode == 5) return float4(frac(s.uv), 0.0, 1.0);

  const ResolveMaterial material = material_of(geo, instance, entry.y);
  // The material's UV transform (KHR_texture_transform) after the scale, and its linear part on
  // the derivatives: a brick texture authored to tile eight times reads eight tiles.
  const float2 uv = material_uv(material, s.uv * material.uv_scale);
  // Mip selection (texture.md): a mipmapped material samples at the level its own triangle's UV
  // derivatives select, since the quad's derivatives straddle other triangles in this pass.
  float2 duv_dx = float2(0.0, 0.0);
  float2 duv_dy = float2(0.0, 0.0);
  if ((material.flags & k_material_mipped) != 0) {
    screen_uv_gradients(geo, fetch_triangle(geo, instance, visible_index, entry.y, id & 0xff),
                        source, params->view_proj, params->width, params->height, duv_dx, duv_dy);
    duv_dx = material_duv(material, duv_dx * material.uv_scale);
    duv_dy = material_duv(material, duv_dy * material.uv_scale);
  }
  // The occlusion view (ResolveMode::Occlusion): the map's value as data, unencoded.
  if (params->mode == 8) {
    const float o = sample_occlusion(material, uv, duv_dx, duv_dy);
    return float4(o, o, o, 1.0);
  }
  const MaterialSample sampled = sample_material_grad(material, uv, duv_dx, duv_dy);
  float3 albedo = sampled.albedo;
  if (params->mode == 7) return float4(display_encode(albedo), 1.0);
  float3 shading_normal = s.normal;
  if (material.normal_texture != k_no_texture) {
    float3 tangent;
    float3 bitangent;
    // The triangle a second time rather than one held across the reconstruction, which is what
    // this block always did: the frame is needed only where a normal map is.
    const TriangleFetch tri = fetch_triangle(geo, instance, visible_index, entry.y, id & 0xff);
    if (tangent_frame(geo, tri, s.normal, tangent, bitangent)) {
      shading_normal =
          apply_normal_map_grad(material, uv, duv_dx, duv_dy, s.normal, tangent, bitangent);
    }
  }
  // A shading normal that ended up behind the surface — the far side of thin two-sided geometry,
  // whose vertex normals are the near side's — is turned to the camera's side, as the reference
  // path tracer turns it to the side its ray came from. It matters more since `brdf_eval` clamps
  // n.v instead of rejecting it (2026-09-25): a normal behind the surface would otherwise take
  // the lights on the far side of it through the surface. A normal merely leaning away from the
  // eye on the camera's side of its triangle is left alone; that is what the clamp is for.
  if (dot(shading_normal, s.geometric) < 0.0) shading_normal = -shading_normal;
  // The ground's detail (ground_detail.slang): ripples in the shading normal, grain in the albedo
  // and the roughness, a function of the point's world (x, z) filtered by this pixel's footprint —
  // the reconstruction's own derivatives, from the triangle, as the UV's are. Only a material that
  // carries it, and only when the frame hands over its block; everything else reads what it read.
  float roughness = sampled.roughness;
  const bool detailed =
      (material.flags & k_material_ground_detail) != 0 && params->ground_detail != 0;
  if (detailed) {
    const TriangleFetch tri = fetch_triangle(geo, instance, visible_index, entry.y, id & 0xff);
    float3 dp_dx;
    float3 dp_dy;
    screen_position_gradients(tri, source, params->view_proj, params->width, params->height, dp_dx,
                              dp_dy);
    const GroundShading ground =
        ground_detail_shade(((GroundDetail*)params->ground_detail)[0], s.position, shading_normal,
                            dp_dx, dp_dy, sampled.detail_mask, albedo, roughness,
                            params->mode == 9);
    // The detail view (ResolveMode::GroundDetail): data, not colour — the ripples' height over
    // their amplitude and the grain's coarse octave, each a function of (x, z) alone and mapped
    // from [-1, 1], and the share of the ripples this pixel draws (sand, slope and footprint).
    if (params->mode == 9) {
      return float4(ground.ripple * 0.5 + 0.5, ground.weight * ground.fade,
                    ground.grain * 0.5 + 0.5, 1.0);
    }
    shading_normal = ground.normal;
    albedo = ground.albedo;
    roughness = ground.roughness;
  }
  if (params->mode == 9) return float4(0.0, 0.0, 0.0, 1.0);
  BrdfSurface surface;
  surface.albedo = albedo;
  surface.normal = shading_normal;
  surface.view = normalize(params->camera.xyz - s.position);
  surface.roughness = roughness;
  surface.metallic = sampled.metallic;

  // Shadows are `shadow_visibility`'s: a ray leaves the surface offset along the geometric normal
  // by `ray_offset` (so that the quantized position the resolve reconstructs cannot start it
  // inside the float positions the acceleration structure was built from — a gap the size of the
  // receiver's own grid, not of the scene), and the cascaded maps are read at the surface itself,
  // whose own triangle the rasterizer drew into them from the same 16-bit grid.
  const float ray_bias =
      ray_offset(geo, instance, params->shadow_bias, params->shadow_bias_steps);

  // The shadow view (ResolveMode::Shadow): the sun's shadow query and nothing else, in flat values
  // a test or a person can count. White where the sun reaches the surface, black where it is
  // blocked, and mid grey where the surface faces away from the sun, so nothing is asked. It asks
  // exactly the question the shaded view below asks, with the same normal and the same surface, so
  // a pixel that is black here is a pixel the sun term is missing from there. A ray answers 0 or 1;
  // the cascaded maps answer the filtered fraction, so their penumbra is grey between the two.
  if (params->mode == 6) {
    if (dot(surface.normal, params->sun.xyz) <= 0.0) return float4(0.5, 0.5, 0.5, 1.0);
    const float v = shadow_visibility(params, k_shadow_sun, 0u, s.position, s.geometric, ray_bias,
                                      params->sun.xyz, 1e30);
    return float4(v, v, v, 1.0);
  }

  // With a sky, the sun and every directional light arrive through the air above the point — its
  // transmittance towards them, nothing below the planet's horizon — and the sun with its own
  // illuminance rather than the stand-in's intensity (sky.slang, `sky_light_at`).
  SkyParams* sky = (SkyParams*)params->sky_params;
  const bool has_sky = params->sky_params != 0;

  // The sun: one direction, one intensity, no falloff; a hit anywhere along the ray removes it,
  // and a filtered map removes the fraction it covers. A visibility of 1 multiplies nothing, so a
  // ray-traced or unshadowed pixel is the bytes it always was.
  float3 color = float3(0.0);
  if (dot(surface.normal, params->sun.xyz) > 0.0) {
    const float v = shadow_visibility(params, k_shadow_sun, 0u, s.position, s.geometric, ray_bias,
                                      params->sun.xyz, 1e30);
    if (v > 0.0) {
      float3 sun = float3(params->sun.w);
      if (has_sky) sun = sky_light_at(sky, s.position, params->sun.xyz, sky->sun_illuminance.rgb);
      color = brdf_direct(surface, params->sun.xyz, sun * v);
    }
  }
  // The analytic lights: inverse square windowed to zero at the radius, spots by their cone, and
  // a shadow ray that stops short of the light itself; a directional light (the moon) has neither
  // falloff nor end. The cascaded maps shadow only the light they were drawn from.
  ResolveLight* lights = (ResolveLight*)params->lights;
  for (uint i = 0; i < params->light_count; ++i) {
    float3 light_dir;
    float3 radiance;
    float light_distance;
    if (!light_sample(lights[i], s.position, light_dir, radiance, light_distance)) continue;
    float v = 1.0;
    if (dot(surface.normal, light_dir) > 0.0) {
      v = shadow_visibility(params, k_shadow_lights, i + 1u, s.position, s.geometric, ray_bias,
                            light_dir, light_distance - ray_bias);
      if (v <= 0.0) continue;
    }
    if (has_sky && lights[i].direction_type.w >= 1.5) {
      radiance = sky_light_at(sky, s.position, light_dir, radiance);
    }
    // A filtered map answers a fraction; a ray, or no shadow at all, answers 1 or 0, and a 1
    // multiplies nothing, so a picture drawn before the maps could follow a light is the same.
    color += brdf_direct(surface, light_dir, v < 1.0 ? radiance * v : radiance);
  }
  // The hemisphere is the whole indirect term until the lighting passes exist: the sky above the
  // horizon and the ground below it — the scene's ground albedo under this frame's sun and sky,
  // `ground_radiance` — blended by how much of the normal's cosine lobe each half fills
  // (`ambient_radiance`, brdf.slang's `brdf_hemisphere_ambient`), so a face turned towards the
  // ground is lit by the ground: a dune's slip face in shade by the sand around it. The diffuse
  // albedo takes it, and the specular lobe takes the part of it `brdf_env_specular` says a lobe of
  // this roughness reflects at this view angle — which keeps metals, which have no diffuse lobe,
  // from going black, and keeps rough stone matte at grazing (it was bare Schlick at n.v until
  // 2026-09-25, a mirror's sheen at every roughness; brdf.slang has why). The path tracer picks
  // the same two halves up as the radiance an escaped ray carries (`environment_radiance`), which
  // is what makes the two comparable.
  // The material's ambient occlusion multiplies this term and nothing else — not the sun, not the
  // point lights, not the emission — because it stands in for the part of the hemisphere the
  // surface cannot see, which is exactly what this term assumes it sees all of. A material with no
  // map multiplies nothing.
  //
  // **With a sky** the hemisphere is the sky's own: the environment of the frame's sky-view table
  // above the horizon and the ground it lights below, projected on nine spherical harmonics once a
  // frame and convolved with the cosine lobe (`sky_ambient`), so a face towards the low sun's side
  // of the sky takes its glow and one towards the ground the sand the sun and the sky light.
  float3 ambient;
  if (has_sky) {
    ambient = sky_ambient((SkyFrame*)sky->frame, surface.normal);
  } else {
    const float3 ground = ground_radiance(params->sky.rgb, params->ground.rgb, params->sun);
    ambient = ambient_radiance(params->sky.rgb, ground, surface.normal);
  }
  const float n_dot_v = max(dot(surface.normal, surface.view), 0.0);
  float3 indirect =
      ambient * (albedo * (1.0 - surface.metallic) +
                 brdf_env_specular(brdf_f0(albedo, surface.metallic), surface.roughness, n_dot_v));
  if (material.occlusion_texture != k_no_texture) {
    indirect *= sample_occlusion(material, uv, duv_dx, duv_dy);
  }
  color += indirect;
  // Emission — the factor times the emissive texture — is radiance the surface gives off, added
  // after the lighting and touched by no shadow and no occlusion.
  color += sampled.emissive;
  if (has_sky) {
    // The air between the eye and the surface: what it takes out of the surface's light on the way
    // and what it scatters in, out of the frame's aerial table, which is the sky-view table's
    // parameterization with a distance, so a surface at the table's far end is the sky beyond it.
    // Then the frame's exposure.
    const float3 to_surface = s.position - params->camera.xyz;
    const float distance = length(to_surface);
    float3 inscatter = float3(0.0);
    float transmittance = 1.0;
    if (distance > 0.0) sky_aerial(sky, to_surface / distance, distance, inscatter, transmittance);
    color = color * transmittance + inscatter;
    color = sky_tone(sky, color * sky_exposure((SkyFrame*)sky->frame));
  }
  return float4(display_encode(color), 1.0);  // linear to display on a UNORM swapchain
}

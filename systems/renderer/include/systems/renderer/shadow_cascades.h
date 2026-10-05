#pragma once

// The sun's cascaded shadow maps, fit to the camera (docs/subsystems/renderer.md, "Shadows";
// docs/plan/04-renderer.md §4.4). Pure arithmetic on the CPU, once a frame: which spheres of the
// world the cascades cover, where their texel grids sit, and the matrices the cull pass, the
// rasterizers and the resolve all use. Nothing here touches a device, so the fit is tested on
// every machine.
//
// **The fit.** The camera's depth from its near plane to the shadow distance is cut into
// `cascades` slices, logarithmically: slice i ends at `near * (far / near)^((i + 1) / n)`, so each
// cascade covers the same ratio of depths and its texels stay about as large, on screen, as the
// next one's. `far` is the far side of the scene's bounds unless the caller gives a distance, and
// the first slice starts at the nearest the scene's bounds come to the camera — a camera 150 units
// from a 100-unit scene spends its cascades on the 50 to 250 units the scene can occupy and not on
// the empty 50 in front of it — with the logarithm's base never under a thousandth of `far`,
// which is what keeps a camera standing inside a 5 km terrain from spending its first cascade on
// its own feet. Each slice's cascade is the smallest
// sphere centred on the camera's forward axis that holds the slice's corners in every view of
// the layout: a sphere, not a box, because a sphere is the same size whichever way the camera
// turns, so a rotating camera keeps its texels the same size and a static shadow does not
// swim. Its radius is rounded up to a 64th of its power of two for the same reason, so that the
// float noise of a moving camera cannot change a texel's size between frames, and its centre is
// snapped to a whole texel across the light (`gfx::shadow_snap`), so that a camera that moves
// moves the grid by whole texels. A cascade whose sphere would hold the whole scene's bounds is
// replaced by them, and is the last: every receiver there is is inside it. That is the common
// case for a camera framing an object — engine-view's orbit frames a mesh with a frustum wider
// than the mesh, so its first slice already holds it and the frame draws **one** cascade, every
// texel on the scene — and it is why a frame may draw fewer cascades than the atlas holds.
//
// **The depth range.** A cascade holds every caster between the light and its receivers: its
// depth runs from the sphere's far side, away from the light, to the far side of the scene's
// bounds towards it. That is what lets a caster outside the camera's frustum cast into the
// picture — a tree behind the camera, a card above it — which the ray-traced shadows, built from
// the camera's cut, cannot do.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/gfx/visibility_resolve.h>

namespace engine::renderer {

class ViewSet;
struct Camera;

// The constant half of the maps' depth bias, in texels of each cascade's own size. The receiver
// plane (evaluated per texel in the resolve) is exact for a flat receiver at any angle; this covers
// what is not flat across the filter's 4 x 4 footprint — curvature, a neighbouring triangle — and
// the float error between the resolve's reconstruction and the rasterizer. Measured, not guessed:
// docs/subsystems/renderer.md, "Shadows", has the acne and light-leak counts it was chosen by.
inline constexpr f32 k_shadow_bias_texels = 0.5f;
// The receiver plane's depth slope is clamped here, tan(83 degrees): past it a surface is so near
// grazing that the plane's prediction two texels away means nothing, and the sun's term there is
// N.L < 0.12 of its peak anyway.
inline constexpr f32 k_shadow_max_slope = 8.0f;
// How far the lookup leaves the surface along its geometric normal at a grazing or back-facing
// surface, in texels of the cascade read; (1 - N.L) of it elsewhere (gfx::ShadowMapParams).
inline constexpr f32 k_shadow_normal_offset_texels = 3.0f;
// The first split is at least this fraction of the shadow distance from the camera.
inline constexpr f32 k_shadow_near_ratio = 1.0e-3f;

struct ShadowFit {
  u32 cascades = 4;       // 1 .. gfx::k_max_shadow_cascades
  u32 resolution = 2048;  // texels per cascade side
  f32 distance = 0.0f;    // camera depth the last cascade reaches; 0: the scene's far side
};

struct ShadowCascades {
  u32 count = 0;  // may be fewer than asked: see "A cascade whose sphere would hold the scene"
  u32 resolution = 0;
  gfx::ShadowLight light;
  gfx::ShadowCascade cascades[gfx::k_max_shadow_cascades];
  // The spheres the cascades cover (snapped), in the frame's space, and the camera depths each was
  // fit to.
  Vec3 centers[gfx::k_max_shadow_cascades] = {};
  f32 radii[gfx::k_max_shadow_cascades] = {};
  f32 splits[gfx::k_max_shadow_cascades + 1] = {};
  // Whether cascade i is the scene's own bounds rather than a slice's sphere.
  bool scene_bounds[gfx::k_max_shadow_cascades] = {};
};

// The frame's cascades, from the camera the frame is drawn with and the layout it is drawn
// through (`views` must have been updated with `camera`). `towards_sun` is the sun vector of the
// frame's lighting; the scene's bounds are its bounding sphere.
//
// **In the frame's space** (ADR-0053): the fit is done in f64 relative to the camera's eye, which
// is the frame's origin, and every matrix, centre and sphere out of it is in that space, as the
// resolve's surfaces are. A cascade's centre is snapped to whole texels across the light measured
// from the corner of the eye's 64 m cell, not from the world's origin: still whole texels while the
// eye moves inside a cell, so a static caster stays put, and a function of nothing but the scene,
// the camera and the cell, so a scene and camera moved by whole cells draw the same maps. Where the
// eye crosses into the next cell the lattice the centres snap to moves by a fraction of a texel
// once (renderer.md, "Shadows").
void fit_shadow_cascades(const ViewSet& views, const Camera& camera, Vec3 towards_sun,
                         WorldPos scene_center, f32 scene_radius, const ShadowFit& fit,
                         ShadowCascades& out) noexcept;

// The block the resolve reads the maps through: the cascades, the light's frame, the atlas's
// bindless slot and width in tiles (the cascades it was allocated for, which a frame may not
// all use), and a nearest-filtering sampler's slot.
void shadow_map_params(const ShadowCascades& cascades, u32 texture, u32 tiles, u32 sampler,
                       gfx::ShadowMapParams& out) noexcept;

}  // namespace engine::renderer

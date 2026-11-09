#pragma once

// **What is under the water is seen: the submerged caster volume** (docs/subsystems/water.md,
// "What is under the water"; docs/subsystems/renderer.md, "Shadows"; roadmap S86).
//
// The resolve's ray-traced water modules trace, in the volume tier, one ray per water pixel along
// the view refracted at the wave normal, out to the ground the depth-map march found
// (`water_refract.slang`). That ray meets only what the frame's structures hold. Where the frame
// traces there is no two-pass occlusion culling (settings.cpp: the structures are built from one
// visible list), so a submerged pair inside the view's frustum is already in the cut — drawn, its
// pixels behind the water's surface. What the frame does not hold is what the **refraction bends
// into from outside the frustum**: Snell bends the view towards the vertical, so under the frame's
// bottom edge a refracted view reaches the bed nearer and deeper than the straight one would — a
// log on the bed under the near shore, a tree's base below the frame. A pair there is made a
// **caster** of the view, by one more caster volume in its cull block behind the lights' and the
// mirrored one (water_reflect.h): built non-opaque, never drawn, counted in
// `Stats::shadow_casters`, dropped first when the chain's capacity runs short.
//
// **The volume.** A refracted view leaves the surface at a point P the frustum holds, keeps its
// azimuth, and runs down at most at the critical angle, asin(1 / 1.33) = 48.75° from the vertical,
// so a point it reaches at depth h lies within h tan 48.75° = 1.1404 h of P horizontally, and
// within the water's reach `path_m` (what Beer–Lambert leaves 1 of 255 of light from, ln 255
// extinction lengths of the clearest channel) of P, so within `path_m sin 48.75° = 0.7518 path_m`.
// So the volume is a vertical prism: the frustum's cross-section at the level, as a convex polygon
// in (x, z), each edge pushed out by `min(1.1404 (level - floor), 0.7518 path_m)`; cut by the level
// above and the floor below — the body's deepest bed less a margin, or the reach's depth where that
// is shallower — and held within `reach` of the eye. The prism holds the whole frustum's water too,
// whose pairs the frustum test keeps before the volume is read, so it adds only what the
// refraction reaches outside the frustum.
//
// One body a frame, the one the reflection's plane is (`GpuScene::water_refract_body`): a body at
// another level still traces against everything the structures hold.
//
// Everything is in the frame's space, the eye at the origin (ADR-0053).

#include <core/base/types.h>
#include <core/math/math.h>
#include <systems/renderer/caster_volume.h>

namespace engine::renderer {

// tan and sin of water's critical angle, asin(1 / 1.33): how far a refracted view runs sideways
// per metre of depth at most, and per metre of path.
inline constexpr f32 k_water_refract_spread = 1.1404f;
inline constexpr f32 k_water_refract_path_spread = 0.7519f;
// The margin under the deepest bed the volume reaches, metres: an object lying on the bed stands
// on it, and the bed's deepest texel is a bilinear sample.
inline constexpr f32 k_water_refract_bed_margin_m = 0.5f;

// The water's reach, metres: the path after which Beer–Lambert leaves 1 of 255 of the light in its
// clearest channel, `gfx::k_water_refract_extinctions` over the smallest extinction.
f32 water_refract_path_m(Vec3 extinction) noexcept;

// The submerged volume of `view_proj` (above): below `level`, above `floor`, the frustum's
// cross-section at the level pushed out by `spread_m`, within `reach` of the eye. False, and an
// empty volume, when the eye is not above the level, the floor is not below it, or the frustum
// does not cross the level.
bool fit_water_refract_volume(const Mat4& view_proj, f32 level, f32 floor, f32 spread_m, f32 reach,
                              const CasterBounds& bounds, CasterVolume& out) noexcept;

// `renderer.water.refract`: 0 off, 1 on (the default), 2 on in even frames and off in odd ones, so
// one flight measures both. True when frame `frame_index` traces what is under the water.
bool water_refract_tunable(u64 frame_index) noexcept;

// `renderer.water.refract_volume` (true): the submerged caster volume. False refracts into only
// what the frame's cut holds, which is the test's way to show what the volume adds.
bool water_refract_volume_tunable() noexcept;

}  // namespace engine::renderer

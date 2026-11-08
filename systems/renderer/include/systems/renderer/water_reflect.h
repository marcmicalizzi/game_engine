#pragma once

// **The water reflects the scene: the mirrored caster volume** (docs/subsystems/water.md,
// "Shading"; docs/subsystems/renderer.md, "Shadows"; roadmap R129).
//
// The resolve's ray-traced water modules trace one ray per water pixel along the view reflected off
// the wave normal against the frame's top-level structure (`water_reflect.slang`). A reflection's
// volume is the **mirrored frustum**, which the frame's cut does not hold: what the water shows is
// what a camera reflected in its plane sees, so a tree over the eye's shoulder or a mountain above
// the frame's top edge is in the water and in no view. A pair that stands there is therefore made a
// **caster** of the view — built into the structures non-opaque, never drawn, counted in
// `Stats::shadow_casters` and in the chain's capacity like any caster — by one more caster volume
// in the view's cull block, behind the shadows' own: the view's closed frustum reflected in the
// plane `y = level`, cut by the plane itself (only what stands above the water is reflected) and
// held inside a sphere of `gfx::k_water_reflect_m` round the eye (the reflection ray's reach).
//
// **Why a volume rather than a run of its own.** The cull pass already tests a pair its frustum
// rejects against the view's caster volumes and keeps one inside as a caster at the camera's own
// LOD, with every test a caster takes; a pair its frustum keeps is drawn or a cone caster already,
// so a separate mirrored run would put it in the structures twice. The volume costs the pairs the
// frustum rejects one more plane test each, needs no change to the cull shader, and leaves the
// drawn and caster entries a partition of the frame's pairs (renderer.md, "The ray tracing chain's
// index space"). What it gives up: the cull counts the mirrored casters with the others, so the
// frame cannot say how many of its casters the reflection asked for (a measurement takes the
// difference with `renderer.water.reflect` cycling).
//
// **One plane a frame** (`GpuScene::water_reflect_level`): the open body's level when the scene has
// one, else the level of the body nearest the eye. A body at another level still traces against
// everything the structures hold — its reflection loses only what stands outside every view and
// outside the chosen plane's mirrored frustum.
//
// Everything is in the frame's space, the eye at the origin (ADR-0053).

#include <core/base/types.h>
#include <core/math/math.h>
#include <systems/renderer/caster_volume.h>

namespace engine::renderer {

// The closed frustum of `view_proj` reflected in the plane `y = level`, above that plane and inside
// the sphere of `reach` round the eye. False, and an empty volume, when the eye is not above the
// plane (looking up from under the water reflects nothing this way) or the projection has no
// frustum.
bool fit_water_reflect_volume(const Mat4& view_proj, f32 level, f32 reach,
                              const CasterBounds& bounds, CasterVolume& out) noexcept;

// `renderer.water.reflect`: 0 off, 1 on (the default), 2 on in even frames and off in odd ones — a
// flight that measures both from one process. True when frame `frame_index` reflects.
bool water_reflect_tunable(u64 frame_index) noexcept;

// `renderer.water.reflect_volume` (true): the mirrored caster volume. False reflects only what the
// frame's cut holds, which is the test's way to show what the volume adds.
bool water_reflect_volume_tunable() noexcept;

// `renderer.water.reflect_shadow`: the hit's sun shadow — 0 none, 1 half rate (the default), 2
// every pixel — as the `gfx::ResolveParams::shadow_flags` bits beside `k_shadow_water_reflect`.
u32 water_reflect_shadow_flags() noexcept;

}  // namespace engine::renderer

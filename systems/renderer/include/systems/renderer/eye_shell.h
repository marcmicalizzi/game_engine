#pragma once

// **The geometry round the eye** (docs/subsystems/renderer.md, "Indirect light: the world-space
// cache", "Stage 4"; roadmap R69 stage 4). The frame's acceleration structures hold the frustum's
// cut and the casters its lights and its water ask for (caster_volume.h, water_reflect.h,
// water_refract.h); a tree three metres behind the eye, outside the sun's sweep, is in none of
// them, so a ray of the world-space cache that leaves the frustum sideways or backwards meets
// nothing where the forest stands and brings back sky, and nothing behind the eye can be sampled
// before the eye turns to it. The shell is one more caster volume in the view's block: everything
// whose cluster sphere comes within `radius` of the eye, whatever the frustum.
//
// It is the cube of six planes round the sphere, with the sphere as the volume's `reach`, so the
// cull pass's existing test (`in_caster_volume`: every plane, then the reach) is the sphere test
// and no shader learns a sixth kind of volume. Its pairs are casters like the others: the camera's
// cut, built non-opaque (primary rays pass through them, the cache's and the shadow rays stop),
// in the chain's capacity and dropped by its rule.
//
// Everything is in the frame's space, the eye at the origin (ADR-0053). Pure arithmetic; the
// caster volume tests hold it to the sphere.

#include <core/base/types.h>
#include <core/math/math.h>
#include <systems/renderer/caster_volume.h>

namespace engine::renderer {

// The volume of every sphere that comes within `radius` of `eye` (in the frame's space). False,
// and an empty volume, for a radius of 0 or less.
bool fit_eye_shell_volume(Vec3 eye, f32 radius, CasterVolume& out) noexcept;

}  // namespace engine::renderer

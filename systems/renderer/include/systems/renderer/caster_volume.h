#pragma once

// **The shadow casters outside the camera's frustum** (docs/subsystems/renderer.md, "Shadows";
// roadmap R54). The traced shadows are cast by the frame's own acceleration structures, which the
// ray tracing chain builds from the cull pass's output; a pair the camera's frustum test drops is
// in none of them, so an object that leaves the view stops casting into it, cluster by cluster, as
// its clusters cross the frustum's edge. The fix is a second volume per view: every point from
// which a ray towards the light can reach the view. A pair that fails the frustum test but whose
// sphere is inside it is kept as a **caster** — built into the structures, never drawn.
//
// **The directional light's volume** is the view's frustum swept towards the light: the points
// `f + t L` for `f` in the frustum and `t` in `[0, T]`, `L` the unit direction towards the light.
// It is convex (a frustum's Minkowski sum with a segment), and with the inward convention the cull
// uses (`dot(n, p) + d >= 0` inside) its planes are:
//
//   - every frustum plane with `dot(n, L) >= 0`, because moving along `L` never leaves it;
//   - one plane through every **silhouette edge** — an edge between a kept plane and a dropped one
//     — containing `L`, with the normal `cross(edge, L)` turned inward;
//   - one **cap** perpendicular to `L`: `dot(L, p) <= h`.
//
// At most 6 + 6 + 1 = 13 (`gfx::k_caster_volume_planes`); a perspective frustum's silhouette is a
// cycle of at most six edges. Every plane's offset is the support of the hull's own points (the
// frustum's eight corners and the same eight moved by `T L`), so whatever the rounding of a
// normal, every point of the sweep is inside every plane — the volume is conservative by
// construction, and tight up to the cap.
//
// **The cap** says how far along the light a caster can stand from what it shadows.
// `RenderSettings::shadow_distance` where it is set (the maps' knob, which the owner already
// has): `h` is that far past the frustum's farthest corner along `L`. Otherwise, and never past
// it, the far side of the scene's bounds towards the light — exactly where a cascade's depth
// ends, since a caster beyond the scene's bounds does not exist. Never short of the frustum
// itself, which the cap must not cut.
//
// **The frustum is closed at the scene's far side.** The camera's projection has no far plane
// (`perspective_reversed_z` with `far = 0`), so its frustum is a cone with no corners at its far
// end. Nothing the scene holds is farther from the eye than `|centre| + radius`, and no point is
// deeper along the view's axis than it is far, so a far plane at that depth leaves out no
// receiver; a projection with a nearer far plane of its own keeps it.
//
// **A point light's volume** is the convex hull of the frustum and the light's position: kept
// planes are those with the light inside, a silhouette edge's plane goes through the light, and
// the light's own sphere of influence bounds it (a caster farther from the light than its reach
// shadows nothing the light reaches). A light inside the frustum needs no volume: the segment
// from it to any receiver in the frustum is inside the frustum too.
//
// Everything is in the frame's space, the eye at the origin (ADR-0053): the view-projection a view
// culls with, the scene's centre measured from the eye, the light's position as the resolve has
// it. Pure arithmetic, on the CPU once per view per frame; `caster_volume_tests.cpp` holds it to a
// brute-force membership.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/gfx/cluster_cull.h>

#include <span>

namespace engine::renderer {

struct CasterVolume {
  Plane planes[gfx::k_caster_volume_planes];
  u32 plane_count = 0;  // 0: no volume, and the volume test is never run
  // A sphere the volume is also inside (a point light's reach): xyz the centre, w the radius;
  // w < 0 is none. Also in the frame's space.
  Vec4 reach{0.0f, 0.0f, 0.0f, -1.0f};
};

// What bounds a volume: the scene, in the frame's space (its centre measured from the eye).
struct CasterBounds {
  Vec3 scene_center{};
  f32 scene_radius = 0.0f;
};

// The frustum of `view_proj` swept towards `towards_light` (normalized here). `distance` is
// `RenderSettings::shadow_distance`, 0 for the scene's bounds. False, and an empty volume, when
// there is nothing to sweep: no light, or a projection with no frustum.
bool fit_directional_caster_volume(const Mat4& view_proj, Vec3 towards_light, f32 distance,
                                   const CasterBounds& bounds, CasterVolume& out) noexcept;

// The hull of the frustum of `view_proj` and the point light at `light` with `reach` (its radius
// of influence; 0 or less is unbounded). False, and an empty volume, when the light is inside the
// frustum and so needs none.
bool fit_point_caster_volume(const Mat4& view_proj, Vec3 light, f32 reach,
                             const CasterBounds& bounds, CasterVolume& out) noexcept;

// The cull pass's test (`cluster_cull.slang`, `in_caster_volume`), on the CPU: false only when the
// sphere is wholly outside one plane, or wholly outside the reach.
bool caster_volume_contains_sphere(const CasterVolume& volume, Vec3 center, f32 radius) noexcept;

// The closed frustum the volumes are built from, which is `view_proj`'s own planes with the far one
// at the scene's far side when the projection has none (above): left, right, bottom, top, near,
// far. False when `view_proj` has no near or side planes.
bool closed_frustum(const Mat4& view_proj, const CasterBounds& bounds, Plane out[6]) noexcept;

}  // namespace engine::renderer

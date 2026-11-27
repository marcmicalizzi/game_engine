#pragma once

// **A smooth surface reflects the scene** (docs/subsystems/renderer.md, "Reflections on smooth
// surfaces"; roadmap R71).
//
// The resolve's ray-traced modules trace one ray along the mirror direction from every pixel of a
// material under `gfx::k_reflect_roughness_end` (surface_reflect.slang), wherever the frame traces
// and its scene has such a material: glass, polished stone and metal alike, by the material's own
// roughness and its reflected share at the view, never by a name.
//
// **What the ray meets: the frame's structures, and the eye's shell.** The water's reflection has
// one plane a frame and its mirrored frustum (water_reflect.h); panes face every way, and what a
// facade reflects is mostly behind the eye and beside it, which no plane's mirror holds. So where
// the frame keeps casters and the scene has a smooth surface, every view's cull carries the **eye's
// shell** (`fit_eye_shell_volume`, eye_shell.h): everything whose cluster sphere comes within
// `renderer.reflect.shell_m` of the eye, whatever the frustum, built non-opaque as casters at the
// camera's LOD, counted in `Stats::shadow_casters` and in the chain's capacity, and dropped first
// when it runs short — the world-space cache's volume of the same kind, so where the cache's shell
// is on as well the one volume is the larger of the two radii. Past the shell a reflection holds
// what the frame's cut and its other casters hold, and the sky beyond them.
//
// Everything is in the frame's space, the eye at the origin (ADR-0053).

#include <core/base/types.h>

namespace engine::renderer {

// `renderer.reflect.surfaces`: 0 off, 1 on (the default wherever the resolve traces), 2 on in even
// frames and off in odd ones, so one flight measures both, 3 the diagnostic (each traced pixel red
// where its ray met nothing, green where it met a surface). True when frame `frame_index` traces.
bool surface_reflect_tunable(u64 frame_index) noexcept;

// The `gfx::ResolveParams::shadow_flags` bits frame `frame_index` sets where the scene has a smooth
// surface and the resolve traces: `k_shadow_surface_reflect`, with `_diagnostic` under 3; 0 off.
u32 surface_reflect_flags(u64 frame_index) noexcept;

// `renderer.reflect.shell_m` (32): the eye's shell's radius for the reflections' rays; 0 none, and
// a reflection holds what the frame's cut and its other casters hold.
f32 surface_reflect_shell_m() noexcept;

}  // namespace engine::renderer

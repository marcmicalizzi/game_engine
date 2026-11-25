#pragma once

// **Lights by reach** (roadmap R87; docs/subsystems/renderer.md, "Lights by reach";
// docs/experiments/lights-by-reach.md): which of the frame's lights reach a point, for every reader
// of the light array at once.
//
// A light at a point (material.slang, `light_sample`) is the windowed inverse square of
// `brdf_distance_attenuation`, `(1 - (d/r)^4)^2 / d^2` (Karis 2013), which is **exactly zero** at
// and past its reach `r` (`ResolveLight::position_radius.w`, taken as at least 1e-4 m, since the
// falloff divides by `max(r^2, 1e-8)`). So a point that no light's sphere
// covers is lit by none of them, and a walk that skips the lights whose sphere misses the point
// adds the same terms in the same order: the culled picture is the unculled one's bytes.
//
// The structure is a **uniform grid in the frame's space** (the eye at the origin, ADR-0053) over
// the union of the finite lights' spheres, one bit a light in each cell: a cell's bit is set when
// the light's sphere, grown by `light_grid_margin`, touches the cell's box. It is a grid in space
// and not a list per screen tile because a screen tile serves the camera's pixels and nothing else:
// a reflection's hit, a bounce's hit or a path tracer's vertex is off the screen, and asks the same
// grid with its own point. A point outside the grid's box is reached by no finite light, which is
// exact, since the box holds every grown sphere.
//
// The array's leading directional lights (a sky's moon) have no reach: every reader walks lights
// `[0, first)` at every point, then the set bits of its cell in increasing order, so the order of
// the sum is the array's. A directional light after a finite one (none today) builds no grid, and
// the readers walk the whole array as before.
//
// **Its size is bounded, never its lights**: at most `k_light_grid_words` words of cells, so a
// frame with more lights has fewer, larger cells — coarser culling, never a light left out.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/gfx/visibility_resolve.h>

#include <span>

namespace engine::gfx {

// The words of a frame's cells at most (256 KiB): cells times the words a cell (one per 32 finite
// lights) never pass it. A named constant, not a tunable: it sizes a buffer made at init.
inline constexpr u32 k_light_grid_words = 65536;
// A cell's edge is never under this share of the smallest reach: a cell much smaller than the
// spheres it is tested against buys little culling and costs words (a cell a quarter of a reach
// across holds the lights within about 1.4 reaches of any point in it, where 1 would be exact).
inline constexpr f32 k_light_grid_cell_floor = 0.25f;

// Mirrors LightGrid in material.slang. 48 bytes; the cells follow it, `u32[cells * words]`, the
// cell (x, y, z) at `((z * dims_y + y) * dims_x + x) * words`.
struct LightGrid {
  Vec4 origin_inv_cell{};  // xyz the grid's low corner in the frame's space, w 1 / the cell's edge
  u32 dims_x = 0;
  u32 dims_y = 0;
  u32 dims_z = 0;
  u32 words = 0;  // u32 words a cell: (count - first + 31) / 32
  u32 first = 0;  // lights [0, first) are walked at every point; [first, count) are in the cells
  u32 count = 0;  // the light count the grid was built for
  u32 refs = 0;   // statistics: the set bits over every cell (a light counted once a cell)
  u32 cells = 0;  // dims_x * dims_y * dims_z
};

// Bytes of a built grid: its header and its cells.
inline u64 light_grid_bytes(const LightGrid& grid) noexcept {
  return sizeof(LightGrid) + u64{grid.cells} * grid.words * sizeof(u32);
}

// How much a light's reach is grown for its cells: a 256th of the reach and a centimetre, for the
// float arithmetic of the reader's distance (`brdf_distance_attenuation` compares d^2 with r^2 in
// f32), and a 64th of the cell's edge, for the f32 arithmetic that finds the reader's cell (its
// error is at most dims * 2^-23 of a cell, and a grid has at most 2^16 cells along an axis). A
// grown sphere only ever sets more bits, and a bit for a light that does not reach costs a distance
// test and changes nothing in the picture.
inline f64 light_grid_margin(f64 reach, f64 edge) noexcept {
  return reach / 256.0 + 0.01 + edge / 64.0;
}

// Builds the grid over `lights` (the frame's array, in its order) into `header` and `cells`, using
// at most `cells.size()` words. False when there is no grid to build: no light past the leading
// directional ones, a directional light behind a finite one, a position or a reach that is not a
// finite number, or room for no cell. The readers then walk the whole array, which is the same
// picture. A light's reach is never taken under 1e-4 m, the radius `brdf_distance_attenuation`'s
// `max(r^2, 1e-8)` gives a light of radius zero. Allocates nothing.
bool build_light_grid(std::span<const ResolveLight> lights, std::span<u32> cells,
                      LightGrid& header) noexcept;

// The cell a reader finds for `position`, by the shader's own f32 arithmetic
// (material.slang, `light_walk_begin`); false outside the grid's box.
bool light_grid_cell(const LightGrid& grid, Vec3 position, u32& cell) noexcept;

// Whether the grid leaves any light out of the walk at `position`: false when `position`'s cell
// holds every light past `first`, so a reader there walks what the whole array walks and pays the
// lookup on top. The renderer asks it at the eye and walks the whole array when it is false (R87's
// mend): lantern-street's four fires, whose 71 to 82 m reaches cover the street, cost the resolve
// 0.04 ms more through the grid than through main's loop. Either walk is the same picture, so the
// choice moves a cost and never a byte.
bool light_grid_culls_at(const LightGrid& grid, std::span<const u32> cells, Vec3 position) noexcept;

// Whether every light past the leading directional ones reaches `position` by its own reach,
// before any margin: then the cell holding `position` holds every one of them in any grid
// `build_light_grid` makes, so `light_grid_culls_at` there is false and the grid would be thrown
// away. The renderer asks it at the eye before building (R217): the stand-in's two point lights,
// whose reach is four times the scene's radius, reach every eye, and building their grid only to
// drop it was 8 microseconds of every frame on the desert overlook. False when there is no light
// with a reach, or a position or a reach that is not a finite number (the build decides those).
bool light_grid_reaches_all(std::span<const ResolveLight> lights, Vec3 position) noexcept;

}  // namespace engine::gfx

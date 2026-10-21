#pragma once

// The kit's shapes for a companion solver (docs/subsystems/flora.md, "The kit for companions";
// docs/subsystems/cover.md): what grows **over** another mesh rather than standing on the ground —
// a vine's stems and leaves, a cover's cushions, brackets and caps — built with the same emitter
// the species are, so a companion's mesh carries the motion contract and the structural table the
// renderer's flora stage and the walker's collision read, with nothing about them it has to
// re-derive.
//
// Added after everything else in the module and changing none of it: every species' bytes stay
// pinned. Everything here is IEEE `+ - * /` and `sqrt` in f64 with the deterministic sines, as the
// kit is, so a companion's mesh is a function of its inputs' bits on every toolchain.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/assets/gltf.h>
#include <domain/flora/motion.h>

#include <span>

namespace engine::flora {

struct KitPoint {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};

// The mesh being written and its motion contract. Every vertex is emitted with `part` and with
// `weight` (clamped to [0, 1]) unless a shape takes a weight per station.
struct KitWriter {
  assets::MeshData& mesh;
  Motion& motion;
  u32 part = k_trunk_part;
  f64 weight = 0.0;
};

// Opens a part: its motion record (pivot, rest direction, length, parent) and its structural row
// (radius at the base and the tip, flags), and makes it the part every vertex until the next one
// belongs to. Returns its id.
u32 kit_begin_part(KitWriter& out, KitPoint pivot, KitPoint direction, f64 length, u32 parent,
                   f64 base_radius, f64 tip_radius, u32 flags);

// A tube along a polyline: one ring of `sides` vertices (plus a seam column) at every point, of
// that point's radius, framed by parallel transport so it never twists, the base open (it starts
// inside its parent or the host) and the tip closed by a cone to the last point. UVs in metres —
// u round the circumference, v the arc — so a tiled bark set lies on it unstretched. Every ring's
// vertices carry that point's weight. `points`, `radii` and `weights` are parallel, at least two.
inline u32 kit_tube_triangles(u32 points, u32 sides) {
  return points < 2 ? 0u : (points - 1u) * sides * 2u + sides;
}
void kit_polyline_tube(KitWriter& out, std::span<const KitPoint> points, std::span<const f64> radii,
                       std::span<const f64> weights, u32 sides);

// A leaf card (the understory's): a diamond from `base` out along `direction` for `length`,
// widest (`width`) at 0.35 of it, its tip sunk by `droop` of the length, its plane across `side`.
// Two triangles, the UVs the full card, every vertex at the writer's weight.
inline constexpr u32 k_kit_leaf_triangles = 2;
void kit_leaf_card(KitWriter& out, KitPoint base, KitPoint direction, KitPoint side, f64 length,
                   f64 width, f64 droop);

// A dome over a disc: `rings` rings of `sides` vertices from the disc's rim at `centre` up to a
// pole `height` along `up`, the profile a quarter ellipse — a moss cushion, a mushroom's cap, a
// bracket's shelf (with `squash` < 1 the disc an ellipse, `squash` its minor over its major axis,
// the major along `along`). The rim sits on the surface and is open: the dome is closed by what it
// stands on. UVs the full card (u round, v up), every vertex at the writer's weight.
inline u32 kit_dome_triangles(u32 sides, u32 rings) {
  return rings < 1 ? 0u : sides * (2u * rings - 1u);
}
void kit_dome(KitWriter& out, KitPoint centre, KitPoint up, KitPoint along, f64 radius, f64 height,
              f64 squash, u32 sides, u32 rings);

}  // namespace engine::flora

#pragma once

// The structural table (docs/subsystems/flora.md, "The structural table"): what a structure solver
// — platforms, bridges, ramps and huts built in and between trees — needs to know about a tree's
// parts beyond the motion contract's pivot, direction, length and parent: how thick each part is,
// and which ones may carry a structure.
//
// **The rule a structure solver may rely on.** Records are one per motion part, in id order, so a
// parent's record comes before its children's (`Motion::structure`, section 37 of the container).
// A part's axis is the segment from its `pivot` along its `direction` for its `length`
// (`structural_point`), and its radius at arc `s` along it is linear from `base_radius` at the
// pivot to `tip_radius` at the far end (`structural_radius`). That holds **exactly** — to the
// millimetre — for a part flagged `k_structural_straight`; a part without the flag curves (it
// turns towards the light, wanders, hangs) and the segment is its rest tangent, so a structure
// anchors to straight parts only. A part flagged `k_structural_load_bearing` (a trunk, a stem, a
// major limb's run) has a base radius of at least its species' `load_bearing_min_radius`; a
// structure hangs only from those. A radius is the wood's: a part with no wood (a spray of leaf
// cards, a willow's curtain) records zero. The megaflora's trunk is the core: its buttresses
// stand outside the core radius in the bottom `buttress_height_m`, and its hollows cut into it
// where `ancient_hollows` says.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/flora/motion.h>

#include <schemas/flora.h>
#include <string_view>

namespace engine::flora {

using StructuralPart = geometry::ClusterFileStructuralPart;
inline constexpr u32 k_load_bearing = geometry::k_structural_load_bearing;
inline constexpr u32 k_straight = geometry::k_structural_straight;

// The point at arc `s` along a part's axis by the rule above.
inline Vec3 structural_point(const MotionPart& part, f32 s) noexcept {
  return part.pivot + part.direction * s;
}
// The radius at arc `s` along a part by the rule above, `s` clamped to the part.
inline f32 structural_radius(const StructuralPart& record, const MotionPart& part, f32 s) noexcept {
  const f32 t =
      part.length > 0.0f ? (s < 0.0f ? 0.0f : (s > part.length ? 1.0f : s / part.length)) : 0.0f;
  return record.base_radius + (record.tip_radius - record.base_radius) * t;
}

// The smallest base radius a load-bearing part of the species has, in metres, whatever its
// parameters inside their ranges; 0 for a species that emits no structural table. The species are
// `FloraFile::species`'s names.
f32 load_bearing_min_radius(std::string_view species) noexcept;

// A doorway-sized hollow in the megaflora's trunk (flora.md, "Megaflora"): the centre of its
// floor on the trunk's surface, the unit direction into the trunk, the unit direction up the
// trunk, and the doorway's width and height and the cavity's depth along `inward`. The cavity is
// a box-like pocket — a flat floor, walls parallel to `inward`, a back wall following the trunk's
// curve `depth` in.
struct Hollow {
  Vec3 sill;
  Vec3 inward;
  Vec3 up;
  f32 width = 0.0f;
  f32 height = 0.0f;
  f32 depth = 0.0f;
};

// The hollows `solve_ancient` cuts for these parameters, computed from them alone (no mesh), in
// mesh space. False, with nothing written, for parameters out of range.
bool ancient_hollows(const Ancient& params, Vector<Hollow>& out);

}  // namespace engine::flora

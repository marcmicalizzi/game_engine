#pragma once

#include "detail.h"

#include <core/json/json_value.h>
#include <domain/gfx/formation.h>

#include <string>

namespace engine::rocks::detail {

// Authored process mixtures grounded in docs/experiments/rock-lithology.md. These are not
// measured constitutive constants. Prepared once per recipe, never stored on runtime instances.
struct Lithology {
  f64 bed;
  f64 fold;
  f64 flaking;
  f64 dissolution;
  f64 granular;
  f64 plate;  // fragment thickness relative to its width
  f64 scar;   // broad shell-like fracture bowls (volcanic glass)
};
Lithology lithology(RockType type);

struct Fabric {
  Lithology model;
  D3 normal;
  D3 tangent;
  f64 exposure;
  f64 irregularity;
  u64 seed;
  // W85: the bedding is the world formation's (rocks.md, "Geology"): `normal` and `tangent` are
  // the field's, the type's own fold is off, and the coordinate is the field's at `origin` plus
  // the recipe's point (`gfx::formation_coordinate`, the shader's twin).
  bool world = false;
  gfx::FormationFrame field{};
  D3 origin{};
};
Fabric make_fabric(RockType type, const RockGeology& geology, u64 seed);
// The same with the scene's formation, when the recipe carries one and its geology does not opt
// out (`from_formation`): `origin` is where the recipe's origin stands in the world.
Fabric make_fabric(RockType type, const RockGeology& geology, u64 seed,
                   const scene::Formation* formation, const WorldPos& origin);
struct BedSample {
  f64 coordinate;  // canonical bed coordinate in metres, shared with the PBR baker
  f64 thickness;   // actual local distance between the two enclosing stations
};
BedSample sample_bed(const Fabric& f, D3 p);
f64 fabric_coordinate(const Fabric& f, D3 p);
// Metres removed from a surface. All narrow features fade over 2..4 mesh cells.
f64 fabric_recess(const Fabric& f, D3 p, f64 cell);
bool geology_in_range(const RockGeology& g, std::string& error);
bool valid_rock_type(RockType type);
bool valid_quality(RockQuality quality);
u32 quality_budget(u32 budget, RockQuality quality, u32 floor);
void material_factors(const RockMaterial& source, Vec4& colour, f32& roughness);
bool material_factors_in_range(const RockMaterial& m, std::string& error);
// A recipe's canonical JSON without the material blocks' absent `sampling` (R131, RockMaterial
// version 5): the reflection writes an absent optional as null, and leaving it in would move every
// rock's and deposit's source hash for a field that says nothing. A `sampling` that is set stays.
void drop_absent_sampling(JsonValue& json);
// The same for the formation (W85, RockGeology version 2, Cliff version 5): `from_formation` while
// it is the default true, and a cliff's `formation` and `world_origin` while it carries none, so
// every recipe without a field keeps the key it had.
void drop_default_formation(JsonValue& json);

}  // namespace engine::rocks::detail

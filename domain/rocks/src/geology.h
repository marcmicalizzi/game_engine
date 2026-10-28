#pragma once

#include "detail.h"

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
};
Fabric make_fabric(RockType type, const RockGeology& geology, u64 seed);
f64 fabric_coordinate(const Fabric& f, D3 p);
// Metres removed from a surface. All narrow features fade over 2..4 mesh cells.
f64 fabric_recess(const Fabric& f, D3 p, f64 cell);
bool geology_in_range(const RockGeology& g, std::string& error);
bool valid_rock_type(RockType type);
bool valid_quality(RockQuality quality);
u32 quality_budget(u32 budget, RockQuality quality, u32 floor);
void material_factors(const RockMaterial& source, Vec4& colour, f32& roughness);
bool material_factors_in_range(const RockMaterial& m, std::string& error);

}  // namespace engine::rocks::detail

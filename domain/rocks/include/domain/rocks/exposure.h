#pragma once

// Where bedrock shows through a soil mantle (roadmap G142; docs/subsystems/rocks.md, "Exposed
// bedrock (G142)"; the grounding, docs/experiments/exposed-bedrock.md).
//
// **The rule.** A point of the ground is described by three things: its slope and its convexity,
// both by central differences an arm `ExposureRule.scale_m` either side of it, from heights in
// whole millimetres; and the bed the world's formation (`engine.scene.Formation`, W85) puts at the
// surface there. Rock shows where the slope is past the one a soil mantle is whole under (30
// degrees) and the bed is resistant past a threshold; how much of the bed's thickness stands is
// the **exposure fraction**, which rises with the slope to one at the slope no soil is held on (45
// degrees), is raised on a convex nose, where soil thins, and lowered in a hollow, where it
// gathers. Every ledge the solver stands, and every face the cliffs generator sinks into a slope,
// is sized by this one number, so the two agree on where rock is.
//
// **Why this shape.** The thresholds are measured ones and the sum is not: the published models
// give soil depth against curvature on gentle convex ground (Heimsath et al. 1997) and rock
// exposure against slope on steep ground (DiBiase et al. 2012), and no one law for both. The rule
// adds the two, normalized by their own thresholds, and says so on the grounding page.
//
// **Determinism.** Heights enter as integers, and the rest is IEEE `+ - * /` and one square root
// in f64 with contraction off (ADR-0035), so a point's exposure is the same bits on every
// toolchain, whichever tile asks. Nothing here allocates or keeps state.

#include <core/base/types.h>
#include <domain/gfx/formation.h>
#include <domain/scene_gen/scene_gen.h>

#include <schemas/rocks.h>
#include <string>

namespace engine::rocks {

// The rule with its angles as tangents and its arm in whole millimetres: prepared once.
struct ExposureThresholds {
  f64 soil_slope = 0.0;          // tan(soil_slope_deg)
  f64 rock_slope = 0.0;          // tan(rock_slope_deg)
  f64 critical_curvature = 0.0;  // 1/m
  f64 min_resistance = 0.0;
  i64 arm_mm = 0;
  i64 patch_mm = 0;  // the soil's patches, whole millimetres; 0: none
  // What the soil's patches are drawn from: 0 from `prepare_exposure`, and the caller's (a
  // layer's seed, never a tile's) after it.
  u64 seed = 0;
};

// The ranges `engine.rocks.ExposureRule` documents: false, with a sentence naming the first field
// out of them, otherwise.
bool prepare_exposure(const ExposureRule& rule, ExposureThresholds& out, std::string& error);

// The ground's shape at a point, at the rule's scale. `convexity` is minus the Laplacian of the
// height, 1/m: positive on a nose or a ridge, negative in a hollow.
struct GroundShape {
  f64 slope_x = 0.0;  // dh/dx
  f64 slope_z = 0.0;  // dh/dz
  f64 slope = 0.0;    // the gradient's length: the tangent of the slope angle
  f64 convexity = 0.0;
};

// From five heights in whole millimetres: the point's, and the four an arm away along x and z.
GroundShape ground_shape(i64 centre_mm, i64 west_mm, i64 east_mm, i64 north_mm, i64 south_mm,
                         i64 arm_mm) noexcept;

// **The exposure fraction**, 0 to 1: the share of a resistant bed's thickness that stands out of
// the soil at a point of this shape. Exactly 0 under the soil slope whatever the curvature; on
// planar ground it rises linearly to 1 at the rock slope; a convexity of the critical curvature
// adds one and a hollow of the same depth takes one off, but the rock slope ends every mantle.
f64 exposure_fraction(const ExposureThresholds& rule, const GroundShape& shape) noexcept;

// What a bed's resistance makes of that fraction: 0 under the rule's threshold (the bed weathers
// back under the soil), and from 0.35 at the threshold to 1 for a bed of resistance 1.
f64 resistance_factor(const ExposureThresholds& rule, f64 resistance) noexcept;

// **The soil's cover** at a point of the world, [0, 1): a seeded field of the rule's patch size
// (two octaves of value noise on a lattice of the world's millimetres, so it is the same field
// from every tile), 0 where the rule has no patches. Rock shows where the exposure fraction is
// above it, so the fraction is also the share of such ground that is rock: DiBiase's catchments
// at 38 degrees are a sixth rock, not all of it (docs/experiments/exposed-bedrock.md).
f64 soil_cover(const ExposureThresholds& rule, i64 x_mm, i64 z_mm) noexcept;

// The share of a resistant bed's thickness that stands where the exposure fraction is `fraction`
// under a cover `cover`: nothing where the cover is past the fraction, rising to all of it over a
// quarter of the fraction's range. With no patches (a cover of exactly 0 everywhere, the rule's
// `patch_m` 0) it is the fraction itself.
f64 standing_share(const ExposureThresholds& rule, f64 fraction, f64 cover) noexcept;

// A point's exposure: its shape, the bed at the surface, and `stand`, the share of that bed's
// thickness that stands there (`standing_share * resistance_factor`); `exposed` is `stand > 0`.
struct Exposure {
  GroundShape shape;
  f64 height_m = 0.0;    // the ground at the point
  f64 coordinate = 0.0;  // the formation's bed coordinate at the surface, metres
  u32 bed = 0;           // the bed's index in the sequence, bottom first
  f32 resistance = 0.0f;
  f64 fraction = 0.0;
  f64 cover = 0.0;
  f64 stand = 0.0;
  bool exposed = false;
};

// The exposure at (x_mm, z_mm) of `ground`: five heights asked in whole millimetres. `formation`
// is the scene's, packed (`gfx::pack_formation`).
Exposure exposure_at(const ExposureThresholds& rule, const gfx::FormationParams& formation,
                     const scene_gen::Ground& ground, i64 x_mm, i64 z_mm) noexcept;

}  // namespace engine::rocks

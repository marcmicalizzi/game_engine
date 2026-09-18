#pragma once

// The deformable-volume *policy* numbers of ADR-0029: the per-tick budgets, the cage sizing
// limits the content validator will read, and the conversion from an authored dimensionless
// volume preservation to the compliance a tetrahedron's volume constraint takes.
//
// A separate header from soft_body.h because none of this is backend surface. The content
// pipeline (plan 07 §7.10) reads the sizing limits and the conversion at build time and does not
// want a `World`; the deformation system's LOD tiers (plan 05 §5.14) read the budget report and
// do not want Jolt. Nothing here allocates, touches the backend, or depends on more than
// core/base.
//
// The numbers come from E19 (docs/experiments/e19-lattice-cage.md), which measured them, and the
// reasoning is in ADR-0029 and docs/subsystems/physics.md. Change one and the ADR is what has to
// be revisited, not this file on its own.

#include <core/base/types.h>

namespace engine::physics {

// --- the per-tick budget (ADR-0029 decision 1) -----------------------------------------------
//
// Wall clock on the performance pool per 60 Hz tick, *not* a sum of per-worker CPU time: E19
// measured eight cages running 6.45x faster together than one after another, so a CPU-time sum
// would price the arrangement the engine actually wants as if it were the worst one.
//
// Ambient covers every non-hero T0 volume plus all of T1 and keeps ADR-0026's original figure.
// The hero allowance is for at most one volume flagged `SoftBodyDesc::hero` — the cavity
// interaction of plan 13 §13.7 is the case it exists for — and is on top, so the worst case is
// 4 ms of a 16.7 ms tick. Over budget, the tiers demote ambient volumes farthest-first; the hero
// is never demoted by the budget, because a hero that degrades under its own cost is the one
// failure the allowance exists to prevent.
inline constexpr f32 k_deformable_ambient_budget_ms = 1.5f;
inline constexpr f32 k_deformable_hero_budget_ms = 2.5f;

// --- cage sizing (ADR-0029 decision 2) -------------------------------------------------------
//
// One backend solve group is 256 vertices (`k_soft_body_constraint_batch`), and E19 measured
// that a cage wider than one group does not scale — it is *slower* on eight workers than on one,
// by 1.3x to 1.8x, because the solve jobs that find no group to claim spin rather than idle.
// So the default T0 cage is one group wide, and a wider one is an authoring decision that has to
// be made deliberately rather than by raising a number until the picture looks better.
inline constexpr u32 k_cage_elements_default = 256;
// Above this the content validator refuses outright: 800 is plan 07 §7.10's upper nominal and
// three solve groups, and past it the parallel penalty and the inversion risk both grow while
// the picture does not.
inline constexpr u32 k_cage_elements_max = 800;
// Sub-steps are the stability knob and iterations are not (E19): at 512 elements one sub-step
// took a permanent set at 8 and at 16 iterations while two sub-steps recovered at every
// iteration count. Both cost the same per unit of work, so a budget is better spent on the
// second sub-step than on doubling iterations.
inline constexpr u32 k_cage_iterations_default = 8;
inline constexpr u32 k_cage_sub_steps_default = 2;

// What the content validator does with a cage of `elements` particles. The rule is one line:
// wider than one solve group is a warning on an ambient volume and allowed on a hero, and wider
// than `k_cage_elements_max` is refused for either.
enum class CageSizeVerdict : u8 { Ok, Wide, Refused };

constexpr CageSizeVerdict cage_size_verdict(u32 elements, bool hero) noexcept {
  if (elements > k_cage_elements_max) return CageSizeVerdict::Refused;
  if (!hero && elements > k_cage_elements_default) return CageSizeVerdict::Wide;
  return CageSizeVerdict::Ok;
}

// --- volume preservation -> compliance (ADR-0029 decision 3) ---------------------------------
//
// **A volume constraint's compliance is not an edge constraint's, and it is not scale-free.**
// Giving both the same number is what made E19's first cages take a permanent set at two thirds
// of their volume: XPBD weighs a constraint's compliance against the sum of inverse masses times
// the squared constraint gradient, the gradient of an edge is a unit vector and the gradient of
// a tetrahedron's volume is an *area*, and at a 6 cm cell those are four orders of magnitude
// apart. The same 1e-4 that makes a stiff edge switches the volume constraint off.
//
// The conversion below comes from the energy rather than from that ratio, which is what makes it
// a material property instead of a tuning constant. The backend's constraint is on six times the
// tetrahedron's volume (C = 6V - 6V0) and XPBD's energy is C^2 / 2a, while a material of bulk
// modulus K storing the same volumetric strain holds K (dV)^2 / 2V0. Equating them:
//
//     a = 36 * V0 / K,  and a Kuhn tetrahedron of a cubic cell of side h has V0 = h^3 / 6
//     => a = 6 * h^3 / K
//
// so **compliance goes as the cube of the cell size** at a fixed material. (E19's own rule of
// thumb — "half the element size wants half the compliance" — came from the conditioning ratio
// above rather than from the energy, and is the wrong law: it is h, not h^3. The energy argument
// is the one to trust, because it is the one that makes the same authored number mean the same
// material at any cage resolution, which is the whole point of generating cages.)
//
// The authored knob is dimensionless. `volume_preservation` (plan 07 §7.10, default 0.8) scales
// the material's own bulk modulus: K = (E / 3) * p / (1 - p), where E is the layer's stiffness.
// p = 1/2 is K = E/3, the bulk modulus of a material with Poisson's ratio 0 (a cork: it resists
// volume change but does not bulge); p -> 1 is incompressible and a hard constraint; p -> 0 has
// no volume preservation at all. That gives the 0..1 weight the plan promises with endpoints
// that mean what they say.
//
// Sanity check against E19, which found 1e-8 by hand: at its 5.71 cm cell, 1e-8 is p = 0.63 at
// the plan's default 200 kPa stiffness, and its edge compliance of 1e-4 is a Young's modulus of
// 175 kPa at the same cell. The hand-tuned numbers and the plan's defaults were already the same
// material; nothing knew it because nothing had written the conversion down.

// plan 07 §7.10 `material.stiffness`, in pascals. Muscle; fat is about 20 kPa and cartilage
// about 5 MPa.
inline constexpr f32 k_default_stiffness_pa = 200.0e3f;

// What a volume constraint that preserves nothing gets. Large enough that the backend's own
// `denom` is dominated by it and the correction is zero to a float, so leaving such a constraint
// in the cage is harmless; a generator that can drop it should drop it instead.
inline constexpr f32 k_volume_compliance_none = 1.0e6f;

// The compliance for one tetrahedron of a cubic cell of side `cell_size_m`. `preservation` is
// plan 07 §7.10's `material.volume_preservation` in [0, 1]; `stiffness_pa` is the layer's
// `material.stiffness`. Returns 0 (a hard constraint) at preservation 1.
constexpr f32 volume_compliance_for(f32 preservation, f32 cell_size_m,
                                    f32 stiffness_pa = k_default_stiffness_pa) noexcept {
  if (!(cell_size_m > 0.0f) || !(stiffness_pa > 0.0f)) return k_volume_compliance_none;
  if (!(preservation > 0.0f)) return k_volume_compliance_none;
  if (preservation >= 1.0f) return 0.0f;
  // K = (E / 3) * p / (1 - p); a = 6 * h^3 / K = 18 * h^3 * (1 - p) / (E * p).
  const f32 cell_cubed = cell_size_m * cell_size_m * cell_size_m;
  const f32 compliance = 18.0f * cell_cubed * (1.0f - preservation) / (stiffness_pa * preservation);
  return compliance > k_volume_compliance_none ? k_volume_compliance_none : compliance;
}

// --- the budget report (ADR-0029 decision 1) -------------------------------------------------
//
// What `World::step` measured and what the tier logic above this module acts on. The ambient
// overrun is the actionable number: the tiers demote ambient volumes farthest-first until it is
// zero. A hero overrun is a content problem — the hero is never demoted by the budget — so it is
// reported separately rather than folded into one figure that would invite the wrong response.
struct SoftBodyBudget {
  f32 ambient_ms = 0.0f;  // wall clock in the backend's soft-body phase, ambient volumes' share
  f32 hero_ms = 0.0f;     // the same for the volume flagged `hero`, if there is one
  f32 ambient_over_ms = 0.0f;  // how far past k_deformable_ambient_budget_ms; 0 when inside
  f32 hero_over_ms = 0.0f;     // how far past k_deformable_hero_budget_ms; 0 when inside
  u32 hero_count = 0;          // volumes flagged hero. More than one is an authoring error
  // The most sweeps any cage's strain clamp needed last step, and whether one of them ran out of
  // them. The clamp is a projection that sweeps until nothing is outside the limit, so this is
  // how hard the limit is being fought: 1 is a cage inside its material, and a cage that keeps
  // hitting the cap is one whose press is driving it past what the limit permits and whose cost
  // is a multiple of its edge count every tick. It is in the budget report rather than in a log
  // because it is a cost, and the tick that pays it is the one the budget is about.
  u32 strain_clamp_sweeps = 0;
  bool strain_clamp_saturated = false;
  bool over_budget = false;  // either overrun is non-zero
};

}  // namespace engine::physics

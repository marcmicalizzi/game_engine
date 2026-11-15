#pragma once

// **A swaying mesh's form by ring** (G204; renderer.md, "A swaying mesh's form by ring"). The
// ring-form rule (ring_forms.h) left every mesh that sways as itself in every ring, because a form
// that is not posed is not the mesh. But a mesh's motion has a size, and past a distance that size
// is under a pixel. This is the measure of it and the rule that follows, both over what any
// swaying mesh has, never over who made or placed it.
//
// **The measure** (`measure_sway`): the farthest any vertex of the mesh leaves its rest position
// under the wind the session draws it in, in metres at placement scale 1 — the bound the cull
// already pads a swaying instance's spheres by (`flora_bounds_padding`, linear in the wind's
// strength), taken at the rule's wind instead of the strongest. It is read from the mesh's motion
// contract (cluster sections 35 and 37) as the flora stage reads it, so an imported plant whose
// container carries the contract is measured like a generated one; a mesh with no contract, or one
// the stage draws rigid, does not sway and is formed as a rigid mesh is.
//
// **The rule** (`still_scale`): seen from a ring's floor or farther, a placement of the mesh
// stands still when
//   - its reach there is under `renderer.forms.still_px` pixels at the host's projection: the whole
//     offset a hand-over from its swaying pose to its rest pose can show, and twice the amplitude
//     of the sway about its mean lean; or
//   - it is a field (the sward's kind) and the ring's floor lies past the far field's rest
//     distance by its roots' reach (renderer.md, "The far field at rest"): there the mesh is drawn
//     at rest already, to the bit, so a form with no pose is the same picture.
// The answer is the largest placement scale that stands still, so a ring's still form serves the
// placements small enough for it and a larger one keeps swaying (`pick_ring_form` walks to the
// ring before's entry). Between the mesh itself and a still form a ring draws a **swaying cut**: a
// cut of the mesh's DAG for the ring's floor that keeps its motion contract, as an authored tree's
// swaying distance form does (renderer.md, "Streamed trees sway").
//
// Everything here is a pure function of its arguments but `sway_form_rules`, which reads the
// tunables once, at the load.

#include <core/base/types.h>
#include <core/math/math.h>
#include <domain/geometry/cluster_file.h>
#include <systems/renderer/flora_motion.h>

#include <span>

namespace engine::renderer {

// The rule's knobs, read at the load.
struct SwayFormRules {
  // `renderer.forms.sway`: a swaying mesh takes forms by ring at all. False: it draws itself in
  // every ring, as before G204.
  bool enabled = true;
  // `renderer.forms.still_px`: the reach, in pixels at the host's projection, under which a
  // placement stands still. 0: never by its size (a field past its rest distance still does).
  f32 still_px = 1.0f;
  // The wind's strength the reach is measured under, over a record's mean:
  // `renderer.forms.still_wind`, or below zero (the default) the session's own
  // (`renderer.flora.wind_strength`). Held to [0, k_flora_max_wind].
  f32 wind = 1.0f;
  // The session draws its plants in motion (`renderer.flora.motion`). False: nothing sways, so
  // every mesh's reach is zero and its forms stand still from the first ring past the mesh.
  bool motion = true;
  // The far field's rest distance, metres, when that rule is on (`renderer.flora.field_rest_m`
  // over its margin, with motion); 0: off.
  f32 field_rest_m = 0.0f;
};
SwayFormRules sway_form_rules() noexcept;

// What a swaying mesh measures, at placement scale 1 (its fit applied).
struct SwayMeasure {
  // The cull's padding of a swaying instance of the mesh, mesh units: the bound under the
  // strongest wind. A still form's instances keep it, so the cull chooses over the form exactly
  // what it chose over the mesh. Zero when the session draws no motion.
  f32 padding = 0.0f;
  // Metres: the farthest a vertex leaves its rest position under `SwayFormRules::wind`.
  f32 reach_m = 0.0f;
  // The mesh is posed as a field (`FloraMesh::field`), which the far field's rest rule reaches.
  bool field = false;
};

// Measures `mesh` (`flora_mesh`'s answer for it): `vertices` and `motion` are its own, parallel
// (`flora_bounds_padding`'s arguments), `fit_scale` the scale its fit places it by.
SwayMeasure measure_sway(const FloraMesh& mesh, std::span<const Vec3> vertices,
                         std::span<const geometry::ClusterFileMotionVertex> motion, f32 fit_scale,
                         const SwayFormRules& rules) noexcept;

// What `still_scale` answers for a mesh that stands still at every scale.
inline constexpr f32 k_still_every_scale = 3.0e38f;

// The least still scale a ring's still form is made for: a ring's form stands still when a
// placement half the mesh's own size does, seen from its floor. A cost rule, not a visibility
// one — the form serves exactly the scales `still_scale` says, and below this the form would be a
// mesh of its own that few placements could take.
inline constexpr f32 k_still_form_scale = 0.5f;

// The largest placement scale of a mesh measured `sway` that stands still seen from `floor_m`
// metres or farther; 0 for none. `proj_scale` is the host's projection (`LodView::proj_scale`) and
// `field_root_m` the scene's largest field root offset times the mesh's fit scale, metres — what
// the rest rule takes an instance's roots to lie within of its origin (`gfx::field_rests`).
f32 still_scale(const SwayMeasure& sway, f32 floor_m, f32 proj_scale, f32 field_root_m,
                const SwayFormRules& rules) noexcept;

}  // namespace engine::renderer

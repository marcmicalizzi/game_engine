// A swaying mesh's form by ring: the measure and the rule (sway_forms.h; renderer.md, "A swaying
// mesh's form by ring").
#include <domain/gfx/flora_motion.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/sway_forms.h>

namespace engine::renderer {

namespace {

tunables::Bool forms_sway{
    "renderer.forms.sway", true,
    "A swaying mesh a streamed entry places takes a form by ring: a swaying cut where its motion "
    "is seen, a still one where it is not (G204; renderer.md, \"A swaying mesh's form by "
    "ring\"); false draws it as itself in every ring; read at the load"};
tunables::Float forms_still_px{
    "renderer.forms.still_px", 1.0, 0.0, 16.0,
    "Pixels at the host's projection: a swaying placement whose farthest travel from rest, seen "
    "from its ring's floor, is under this stands still there (G204; 0: never by its size); read "
    "at the load"};
tunables::Float forms_still_wind{
    "renderer.forms.still_wind", -1.0, -1.0, static_cast<f64>(k_flora_max_wind),
    "The wind's strength, over a record's mean, a swaying mesh's travel is measured under for "
    "renderer.forms.still_px (below 0: the session's renderer.flora.wind_strength; 8.4, 42 m/s, "
    "is every wind the sway takes); read at the load"};

}  // namespace

SwayFormRules sway_form_rules() noexcept {
  SwayFormRules rules;
  rules.enabled = forms_sway.get();
  rules.still_px = static_cast<f32>(forms_still_px.get());
  // The subjects are a streamed world's placements: they sway when its tail does.
  rules.motion = flora_motion_tunable() && flora_tail_tunable();
  const f32 asked = static_cast<f32>(forms_still_wind.get());
  f32 wind = asked < 0.0f ? flora_wind_default().strength : asked;
  wind = wind < 0.0f ? 0.0f : (wind > k_flora_max_wind ? k_flora_max_wind : wind);
  rules.wind = rules.motion ? wind : 0.0f;
  const f32 rest_m = flora_field_rest_m_tunable();
  rules.field_rest_m = rules.motion && rest_m > gfx::k_field_rest_margin_m ? rest_m : 0.0f;
  return rules;
}

SwayMeasure measure_sway(const FloraMesh& mesh, std::span<const Vec3> vertices,
                         std::span<const geometry::ClusterFileMotionVertex> motion, f32 fit_scale,
                         const SwayFormRules& rules) noexcept {
  SwayMeasure out;
  out.field = mesh.field;
  if (!rules.motion) return out;  // nothing is posed: no padding, no reach
  // The reach is the sway's bound at the rule's wind. Since W154 the bound follows the storm's law
  // and the bend's cap, no longer linear in the strength, so it is measured at the rule's wind
  // rather than scaled. The padding a still form keeps is the calm rung's (W154-2): it is rigid, so
  // the padding only makes its cut the mesh's at calm, which is main's; a storm's rung pads the
  // swaying mesh's records, never a still form's.
  out.padding = flora_padding_at_load(mesh, vertices, motion, k_flora_calm_load);
  const f32 scale = fit_scale > 0.0f ? fit_scale : 1.0f;
  out.reach_m = flora_bounds_padding(mesh, vertices, motion, rules.wind) * scale;
  return out;
}

f32 still_scale(const SwayMeasure& sway, f32 floor_m, f32 proj_scale, f32 field_root_m,
                const SwayFormRules& rules) noexcept {
  if (!rules.enabled || !(floor_m > 0.0f)) return 0.0f;
  if (!(sway.reach_m > 0.0f)) return k_still_every_scale;  // it does not move at all
  f32 best = 0.0f;
  // By its size: reach * scale * proj / floor < still_px.
  if (rules.still_px > 0.0f && proj_scale > 0.0f)
    best = rules.still_px * floor_m / (sway.reach_m * proj_scale);
  // By the far field's rest: every root of a placement scaled s lies within field_root_m * s of
  // its origin, and the origin is at least the floor away.
  if (sway.field && rules.field_rest_m > 0.0f && floor_m > rules.field_rest_m) {
    const f32 by_rest =
        field_root_m > 0.0f ? (floor_m - rules.field_rest_m) / field_root_m : k_still_every_scale;
    best = by_rest > best ? by_rest : best;
  }
  return best;
}

}  // namespace engine::renderer

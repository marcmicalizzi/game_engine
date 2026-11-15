#include <foundation/tunables/tunables.h>
#include <systems/renderer/ring_forms.h>

#include <cmath>

namespace engine::renderer {

namespace {

tunables::Bool forms_enabled{
    "renderer.forms.enabled", true,
    "A streamed placement's mesh takes a cheaper form in a farther ring "
    "(G197; renderer.md, \"A placement's form by ring\"); read at the load"};
tunables::Float forms_cut_scale{
    "renderer.forms.cut_scale", 1.5, 1.0, 16.0,
    "The largest placement scale a ring's cut serves; a placement scaled more draws the ring "
    "before's form (G197); read at the load"};
tunables::Float forms_min_px{
    "renderer.forms.min_px", 1.0, 0.0, 16.0,
    "A placement whose projected diameter at its ring's floor is under this many pixels at the "
    "host's projection is drawn as nothing there (G197; 0: never); read at the load"};

constexpr f32 k_every_scale = 3.0e38f;

f32 largest_axis(Vec3 s) noexcept {
  const f32 x = std::fabs(s.x);
  const f32 y = std::fabs(s.y);
  const f32 z = std::fabs(s.z);
  const f32 xy = x > y ? x : y;
  return xy > z ? xy : z;
}

bool same_entry(const RingForm& a, const RingForm& b) noexcept {
  return a.mesh == b.mesh && a.max_scale == b.max_scale && a.none_below_scale == b.none_below_scale;
}

}  // namespace

RingFormRules ring_form_rules(f32 proj_scale) noexcept {
  RingFormRules rules;
  rules.enabled = forms_enabled.get();
  rules.proj_scale = proj_scale;
  rules.cut_scale = static_cast<f32>(forms_cut_scale.get());
  rules.min_px = static_cast<f32>(forms_min_px.get());
  return rules;
}

u32 pick_ring_form(const RingFormTable& table, u32 mesh, u32 ring, Vec3 scale) noexcept {
  if (ring == 0 || table.rings == 0 || mesh >= table.meshes.size()) return mesh;
  const MeshRingForms& forms = table.meshes[mesh];
  if (!forms.active) return mesh;
  // A ring past the table's is at least as far as its last: that ring's entry serves it.
  const u32 r = ring < table.rings ? ring : table.rings - 1;
  const f32 s = largest_axis(scale);
  if (s < forms.ring[r].none_below_scale) return k_no_ring_form;
  // The coarsest entry at or inside the ring that serves the scale: each one serves a camera at its
  // own ring's floor, and this ring's floor is farther.
  for (u32 k = r; k >= 1; --k) {
    if (s <= forms.ring[k].max_scale) return forms.ring[k].mesh;
  }
  return forms.ring[0].mesh;
}

bool same_ring_forms(const RingFormTable& table, std::span<const u32> meshes, u32 a,
                     u32 b) noexcept {
  if (table.rings == 0) return true;
  u32 lo = a < table.rings ? a : table.rings - 1;
  u32 hi = b < table.rings ? b : table.rings - 1;
  if (lo > hi) {
    const u32 t = lo;
    lo = hi;
    hi = t;
  }
  if (lo == hi) return true;
  for (const u32 mesh : meshes) {
    if (mesh >= table.meshes.size() || !table.meshes[mesh].active) continue;
    const MeshRingForms& forms = table.meshes[mesh];
    // `pick` at `hi` walks hi, hi - 1, ... and at `lo` from lo down: the two agree for every scale
    // when every entry above `lo` up to `hi` is `lo`'s.
    for (u32 k = lo + 1; k <= hi; ++k) {
      if (!same_entry(forms.ring[k], forms.ring[lo])) return false;
    }
  }
  return true;
}

MeshRingForms choose_ring_forms(u32 mesh, u32 pairs, f32 diameter_m, std::span<const f32> floor_m,
                                std::span<const RingCut> cuts, std::span<const OfferedForm> offered,
                                const RingFormRules& rules) noexcept {
  MeshRingForms out;
  const u32 rings =
      static_cast<u32>(floor_m.size() < k_ring_forms_max ? floor_m.size() : k_ring_forms_max);
  for (u32 r = 0; r < k_ring_forms_max; ++r)
    out.ring[r] = RingForm{mesh, k_every_scale, 0.0f, RingFormKind::mesh, {}};
  if (!rules.enabled || rings < 2 || !(diameter_m > 0.0f) || !(rules.proj_scale > 0.0f)) return out;
  out.active = true;
  for (u32 r = 1; r < rings; ++r) {
    const f32 d = floor_m[r];
    if (!(d > 0.0f)) {
      out.ring[r] = out.ring[r - 1];
      continue;
    }
    // The placement's projected diameter at the ring's floor, at scale 1.
    const f32 px = diameter_m * rules.proj_scale / d;
    RingForm best = out.ring[r - 1];
    u32 best_pairs = pairs;
    if (r < cuts.size() && cuts[r].mesh != k_no_ring_form) {
      best.mesh = cuts[r].mesh;
      best.kind = cuts[r].mesh == mesh ? RingFormKind::mesh : RingFormKind::cut;
      best.max_scale = cuts[r].mesh == mesh ? k_every_scale : rules.cut_scale;
      best_pairs = cuts[r].pairs;
    }
    // An offered form serves the scales whose projected diameter it covers; the cheapest that
    // serves the mesh as placed (scale 1) wins, a tie the cut's, which draws what the mesh does.
    for (const OfferedForm& o : offered) {
      if (o.source != mesh || !(o.max_px > 0.0f)) continue;
      const f32 serves = o.max_px / px;
      if (serves >= 1.0f && o.pairs < best_pairs) {
        best.mesh = o.mesh;
        best.kind = RingFormKind::offered;
        best.max_scale = serves;
        best_pairs = o.pairs;
      }
    }
    best.none_below_scale = rules.min_px > 0.0f ? rules.min_px / px : 0.0f;
    out.ring[r] = best;
  }
  return out;
}

}  // namespace engine::renderer

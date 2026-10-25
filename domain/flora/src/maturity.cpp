// Species scales are authored envelopes grounded in docs/experiments/tree-maturity.md.
#include "maturity.h"

#include <algorithm>

namespace engine::flora::detail {
namespace {
struct Life {
  std::string_view species;
  f32 height_years;  // about 95% of potential height; not first reproduction
  f32 old_start;
  f32 old_scale;  // completes the architectural old-age blend; never a death date
  f32 height;
  f32 radius;  // basal radius, not DBH
};
constexpr Life k_life[] = {
    {"oak", 130, 180, 600, 28, .72f},       {"maple", 145, 180, 400, 30, .60f},
    {"elm", 100, 140, 300, 30, .85f},       {"walnut", 100, 150, 250, 30, .65f},
    {"beech", 180, 200, 400, 27, .55f},     {"ash", 90, 120, 200, 30, .55f},
    {"pine", 110, 180, 450, 40, .70f},      {"spruce", 120, 180, 400, 40, .65f},
    {"cedar", 200, 300, 1000, 50, 1.30f},   {"juniper", 100, 150, 300, 16, .45f},
    {"aspen", 55, 80, 200, 25, .28f},       {"fir", 70, 100, 200, 20, .35f},
    {"hemlock", 250, 300, 800, 38, .70f},   {"alder", 40, 65, 100, 32, .48f},
    {"broadleaf", 130, 180, 600, 28, .72f},  // white-oak-like generic contract
    {"conifer", 120, 180, 400, 40, .65f},    // Norway-spruce-like generic contract
    {"birch", 55, 70, 120, 25, .38f},        // silver birch, not paper birch
    {"willow", 25, 30, 75, 18, .65f},        // weeping willow; an old survivor is possible
    {"orchard", 20, 40, 80, 8, .35f},        // managed standard-rootstock apple-like form
    {"date_palm", 100, 80, 150, 24, .35f},
};
const Life* life_of(std::string_view species) {
  for (const Life& life : k_life)
    if (life.species == species) return &life;
  return nullptr;
}
f32 blend(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
u32 count(u32 adult, f32 mature, u32 minimum) {
  return std::max(
      minimum,
      static_cast<u32>(blend(static_cast<f32>(minimum), static_cast<f32>(adult), mature) + .5f));
}
f32 crown_base(f32 adult, const Tree& control, const MaturityState& age) {
  if (control.crown_base >= 0) return control.crown_base;
  return std::clamp(blend(.15f, adult + .22f * (.5f - age.space), age.mature), .1f, .8f);
}
void age_wood(GrowthRules& rules, const MaturityState& age) {
  const f64 relief = static_cast<f64>(.15f + .85f * age.mature + .35f * age.old);
  rules.wood.collar *= relief;
  rules.wood.flare *= relief;
  rules.wood.fluting *= relief;
  rules.wood.oval *= relief;
  rules.wood.knots *= relief;
}
}  // namespace

bool maturity_in_range(std::string_view species, const Tree& tree, std::string& error) {
  const auto bad = [&](const char* name) {
    error = std::string("engine.flora.Tree.") + name + " is out of range";
    return false;
  };
  if (!(tree.age_years == 0 || (tree.age_years >= 1 && tree.age_years <= 2000)))
    return bad("age_years");
  if (!(tree.site_quality >= .05f && tree.site_quality <= 1.5f)) return bad("site_quality");
  if (!(tree.growing_space >= 0 && tree.growing_space <= 1)) return bad("growing_space");
  if (!(tree.vitality >= 0 && tree.vitality <= 1)) return bad("vitality");
  if (tree.age_years > 0 && life_of(species) == nullptr) {
    error = "tree maturity is unavailable for " + std::string(species);
    return false;
  }
  if (!(tree.height_m == 0 || (tree.height_m >= .35f && tree.height_m <= 60)))
    return bad("height_m");
  if (!(tree.trunk_radius_m == 0 || (tree.trunk_radius_m >= .003f && tree.trunk_radius_m <= 2)))
    return bad("trunk_radius_m");
  if (!(tree.crown_base == -1 || (tree.crown_base >= .1f && tree.crown_base <= .8f)))
    return bad("crown_base");
  if (!(tree.crown_scale >= .5f && tree.crown_scale <= 1.5f)) return bad("crown_scale");
  return true;
}

MaturityState maturity_state(std::string_view species, const Tree& tree, f32 reference_height,
                             f32 reference_radius) {
  MaturityState out;
  const Life* life = life_of(species);
  if (life == nullptr || tree.age_years <= 0) return out;
  const f64 years = static_cast<f64>(tree.age_years);
  const f64 effective = years * static_cast<f64>(tree.site_quality);
  const f64 potential = .5 + .5 * static_cast<f64>(tree.site_quality);
  const f64 max_height = static_cast<f64>(reference_height > 0 ? reference_height : life->height);
  const f64 max_radius = static_cast<f64>(reference_radius > 0 ? reference_radius : life->radius);
  const f64 h = 1.0 - det::exp(-3.0 * effective / static_cast<f64>(life->height_years));
  const f64 r = 1.0 - det::exp(-effective / static_cast<f64>(life->height_years));
  out.height =
      tree.height_m > 0
          ? tree.height_m
          : static_cast<f32>(.35 + (max_height * potential - .35) * det::exp(1.3 * det::log(h)));
  out.radius =
      tree.trunk_radius_m > 0
          ? tree.trunk_radius_m
          : static_cast<f32>(.003 + (max_radius * potential - .003) * det::exp(1.15 * det::log(r)) *
                                        (.6 + .4 * static_cast<f64>(tree.growing_space)));
  out.height = std::clamp(out.height, .35f, 60.0f);
  out.radius = std::clamp(out.radius, .003f, 2.0f);
  // Development and chronological weathering are distinct: suppression delays the scaffold,
  // but does not make a two-century-old small stem chronologically young.
  out.mature = static_cast<f32>(
      std::clamp(effective / (.55 * static_cast<f64>(life->height_years)), 0.0, 1.0));
  out.old = static_cast<f32>(std::clamp((years - static_cast<f64>(life->old_start)) /
                                            static_cast<f64>(life->old_scale - life->old_start),
                                        0.0, 1.0));
  out.space = tree.growing_space;
  out.vitality = tree.vitality;
  return out;
}

void mature_broadleaf(Broadleaf& b, GrowthRules& rules, const Tree& control,
                      const MaturityState& age) {
  b.height_m = age.height;
  b.trunk_radius_m = age.radius;
  b.crown_base = crown_base(b.crown_base, control, age);
  b.limbs = count(b.limbs, age.mature, 2);
  b.branches = count(b.branches, age.mature, 1);
  b.limb_angle_deg = std::min(85.0f, blend(30, b.limb_angle_deg, age.mature) + 8 * age.old);
  b.limb_length = std::clamp(b.limb_length * blend(.6f, 1, age.mature) *
                                 blend(.65f, 1.3f, age.space) * (1 + .12f * age.old),
                             .08f, .8f);
  b.leader_share = blend(.65f, b.leader_share, age.mature) * (1 - .2f * age.old);
  b.upturn *= 1 - .25f * age.old;
  b.wander_deg = std::min(20.0f, b.wander_deg * (.3f + .7f * age.mature + .3f * age.old));
  b.twig_length_m = std::max(.1f, std::min(b.twig_length_m, age.height * .3f));
  b.leaf_area_index = std::max(.5f, b.leaf_area_index * (.15f + .85f * age.vitality));
  rules.trunk_fraction =
      static_cast<f64>(blend(1, static_cast<f32>(rules.trunk_fraction), age.mature)) *
      (1 - .08 * static_cast<f64>(age.old));
  rules.droop *= .25 + .75 * static_cast<f64>(age.mature) + .25 * static_cast<f64>(age.old);
  rules.renewal = static_cast<f64>(age.old);
  age_wood(rules, age);
}

void mature_conifer(Conifer& c, GrowthRules& rules, const Tree& control, const MaturityState& age) {
  const f32 reach = c.crown_radius_m / c.height_m;
  c.height_m = age.height;
  c.trunk_radius_m = age.radius;
  c.crown_base = crown_base(c.crown_base, control, age);
  c.crown_radius_m = std::clamp(age.height * reach * blend(.6f, 1, age.mature) *
                                    blend(.65f, 1.25f, age.space) * (1 + .15f * age.old),
                                .08f, 16.0f);
  c.whorls = count(c.whorls, age.mature, 2);
  c.branches_per_whorl = count(c.branches_per_whorl, age.mature, 3);
  c.interwhorl = count(c.interwhorl, age.mature, 0);
  c.bottom_angle_deg = std::min(130.0f, blend(65, c.bottom_angle_deg, age.mature) + 8 * age.old);
  c.top_angle_deg = std::min(100.0f, c.top_angle_deg + 10 * age.old);
  c.pendulous *= .2f + .8f * age.mature;
  c.dead_share *= age.mature * (1.25f - .5f * age.space);
  c.dead_share = std::min(.9f, c.dead_share + .4f * (1 - age.vitality));
  c.missing_branches *= age.mature;
  c.sparse_whorls *= age.mature;
  c.needle_cards = std::max(2u, static_cast<u32>(c.needle_cards * (.15f + .85f * age.vitality)));
  // Cards represent foliated shoots; a young tree has shorter shoots, not giant juvenile needles.
  c.needle_length_m = std::max(.03f, std::min(c.needle_length_m, age.height * .2f));
  c.lateral_length_m = std::max(.05f, std::min(c.lateral_length_m, age.height * .25f));
  rules.crown_power *= 1 + .45 * static_cast<f64>(age.old);
  age_wood(rules, age);
}

bool resolve_legacy_maturity(const FloraFile& source, FloraFile& out, std::string& error,
                             GrowthRules* resolved_rules) {
  out = source;
  const Tree& control = source.tree;
  if (!maturity_in_range(source.species, control, error)) return false;
  if (control.age_years == 0) return true;
  // Validate authored reference dimensions before any curve/clamp can sanitize them.
  const bool valid = source.species == "broadleaf"   ? broadleaf_in_range(source.broadleaf, error)
                     : source.species == "conifer"   ? conifer_in_range(source.conifer, error)
                     : source.species == "birch"     ? birch_in_range(source.birch, error)
                     : source.species == "willow"    ? willow_in_range(source.willow, error)
                     : source.species == "orchard"   ? orchard_in_range(source.orchard, error)
                     : source.species == "date_palm" ? palm_in_range(source.palm, error)
                                                     : true;
  if (!valid) return false;
  const auto state = [&](f32 h, f32 r) {
    auto age = maturity_state(source.species, control, h, r);
    const f32 max_height = source.species == "birch" || source.species == "date_palm" ? 30.0f
                           : source.species == "willow"                               ? 25.0f
                           : source.species == "orchard"                              ? 12.0f
                                                                                      : 60.0f;
    const f32 max_radius = source.species == "birch" || source.species == "date_palm" ? 1.0f
                           : source.species == "orchard"                              ? 1.5f
                                                                                      : 2.0f;
    // Automatic site growth stays in the native envelope. Explicit dimensions still receive
    // the native validator's species-specific error instead of being silently changed.
    if (control.height_m == 0) age.height = std::min(age.height, max_height);
    if (control.trunk_radius_m == 0) age.radius = std::min(age.radius, max_radius);
    return age;
  };
  if (source.species == "broadleaf") {
    auto& b = out.broadleaf;
    GrowthRules local_rules;
    GrowthRules& rules = resolved_rules != nullptr ? *resolved_rules : local_rules;
    const f32 radius =
        b.trunk_radius_m > 0
            ? b.trunk_radius_m
            : static_cast<f32>(.35 * det::exp(1.5 * det::log(static_cast<f64>(b.height_m) / 12.0)));
    mature_broadleaf(b, rules, control, state(b.height_m, radius));
  } else if (source.species == "conifer") {
    auto& c = out.conifer;
    GrowthRules local_rules;
    GrowthRules& rules = resolved_rules != nullptr ? *resolved_rules : local_rules;
    mature_conifer(c, rules, control, state(c.height_m, c.trunk_radius_m));
  } else if (source.species == "birch") {
    auto& b = out.birch;
    const auto age = state(b.height_m, b.stem_radius_m);
    b.height_m = age.height;
    b.stem_radius_m = age.radius;
    b.crown_base = std::max(.15f, crown_base(b.crown_base, control, age));
    b.limbs = count(b.limbs, age.mature, 2);
    b.branches = count(b.branches, age.mature, 1);
    b.limb_angle_deg = std::min(80.0f, blend(20, b.limb_angle_deg, age.mature) + 8 * age.old);
    b.limb_length = std::clamp(b.limb_length * blend(.55f, 1, age.mature) *
                                   blend(.65f, 1.3f, age.space) * control.crown_scale,
                               .08f, .5f);
    b.leader_share = blend(.65f, b.leader_share, age.mature) * (1 - .15f * age.old);
    b.droop *= .15f + .85f * age.mature;
    b.wander_deg = std::min(20.0f, b.wander_deg * (.3f + .7f * age.mature + .3f * age.old));
    b.twig_length_m = std::max(.1f, std::min(b.twig_length_m, age.height * .3f));
    b.leaf_area_index = std::max(.5f, b.leaf_area_index * (.15f + .85f * age.vitality));
  } else if (source.species == "willow") {
    auto& w = out.willow;
    const auto age = state(w.height_m, w.trunk_radius_m);
    w.height_m = age.height;
    w.trunk_radius_m = age.radius;
    w.trunk_height = blend(.6f, w.trunk_height, age.mature);
    w.limbs = count(w.limbs, age.mature, 2);
    w.branches = count(w.branches, age.mature, 1);
    w.curtains = count(w.curtains, age.mature, 1);
    w.limb_angle_deg = std::min(80.0f, blend(25, w.limb_angle_deg, age.mature) + 8 * age.old);
    w.limb_length = std::clamp(w.limb_length * blend(.6f, 1, age.mature) *
                                   blend(.7f, 1.2f, age.space) * control.crown_scale,
                               .15f, 1.0f);
    w.curtain_length_m = std::max(.1f, std::min(w.curtain_length_m, age.height * .45f));
    w.wander_deg = std::min(20.0f, w.wander_deg * (.3f + .7f * age.mature + .3f * age.old));
    w.leaves = std::max(2u, static_cast<u32>(w.leaves * (.15f + .85f * age.vitality)));
  } else if (source.species == "orchard") {
    auto& o = out.orchard;
    const auto age = state(o.height_m, o.trunk_radius_m);
    o.height_m = age.height;
    o.trunk_radius_m = age.radius;
    o.trunk_height = blend(.5f, o.trunk_height, age.mature);
    o.limbs = count(o.limbs, age.mature, 2);
    o.branches = count(o.branches, age.mature, 1);
    o.twigs = count(o.twigs, age.mature, 1);
    o.limb_angle_deg = std::min(80.0f, blend(25, o.limb_angle_deg, age.mature) + 5 * age.old);
    o.limb_length = std::clamp(o.limb_length * blend(.65f, 1, age.mature) *
                                   blend(.7f, 1.2f, age.space) * control.crown_scale,
                               .15f, 1.2f);
    o.twig_length_m = std::max(.1f, std::min(o.twig_length_m, age.height * .25f));
    o.wander_deg = std::min(20.0f, o.wander_deg * (.3f + .7f * age.mature + .3f * age.old));
    o.leaves = std::max(1u, static_cast<u32>(o.leaves * (.15f + .85f * age.vitality)));
    if (control.age_years < 5) o.fruit = 0;
  } else if (source.species == "date_palm") {
    auto& p = out.palm;
    const auto age = state(p.height_m, p.trunk_base_radius_m);
    const f32 juvenile = std::min(1.0f, control.age_years * control.site_quality / 12);
    const f32 girth = blend(.15f, 1, juvenile);
    const f32 ratio = p.trunk_top_radius_m / p.trunk_base_radius_m;
    p.height_m = age.height;
    p.trunk_base_radius_m =
        control.trunk_radius_m > 0 ? control.trunk_radius_m : p.trunk_base_radius_m * girth;
    p.trunk_top_radius_m = p.trunk_base_radius_m * ratio;
    p.ring_depth_m = std::min(p.ring_depth_m * (1 - .5f * age.old), p.trunk_top_radius_m * .25f);
    p.frond_count = count(p.frond_count, juvenile, 5);
    p.frond_count = std::max(1u, static_cast<u32>(p.frond_count * (.25f + .75f * age.vitality)));
    p.frond_length_m = std::max(.5f, p.frond_length_m * blend(.2f, 1, juvenile));
    p.leaflet_count = count(p.leaflet_count, juvenile, 10);
    p.dead_skirt_fronds = count(p.dead_skirt_fronds, juvenile, 0);
  }
  // The resolved copy dispatches through the native solver only once.
  if (source.species == "broadleaf")
    out.broadleaf.limb_length =
        std::clamp(out.broadleaf.limb_length * control.crown_scale, .08f, .9f);
  if (source.species == "conifer")
    out.conifer.crown_radius_m =
        std::clamp(out.conifer.crown_radius_m * control.crown_scale, .08f, 16.0f);
  out.tree.age_years = 0;
  return true;
}
}  // namespace engine::flora::detail

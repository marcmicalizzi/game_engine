// Species are rules over the existing growth structures, not independent mesh solvers.
// Botanical references and the distinction between observations and fitted parameters:
// docs/experiments/tree-growth-rules.md.
#include "species.h"

#include "kit.h"

namespace engine::flora::detail {
namespace {
enum class Family { branching, whorled, scale_spray };
struct Profile {
  std::string_view name;
  Family family;
  f32 height;
  f32 radius;
  f32 crown_base;
  f32 reach;
  f32 limb_angle;
  f32 leaf;  // individual leaf/leaflet for branching; foliated shoot or scale fan for evergreen
  f32 twig;
  f32 leader;
  f32 upturn;
  f32 droop;
  bool opposite;
  bool layered;
  WoodProfile wood;
};

// Reference dimensions are representative authored trees. They are not genus-wide limits.
constexpr Profile k_profiles[] = {
    {"oak",
     Family::branching,
     16,
     .46f,
     .28f,
     .48f,
     65,
     .12f,
     .65f,
     .22f,
     .55f,
     .1f,
     false,
     false,
     {.45, .4, .09, .06, .09, 1}},
    {"maple",
     Family::branching,
     18,
     .34f,
     .32f,
     .38f,
     48,
     .10f,
     .55f,
     .4f,
     .65f,
     .1f,
     true,
     true,
     {.3, .3, .05, .04, .05, 1}},
    {"elm",
     Family::branching,
     20,
     .45f,
     .30f,
     .48f,
     28,
     .09f,
     .65f,
     .22f,
     .1f,
     .65f,
     false,
     true,
     {.4, .35, .08, .06, .08, 1}},
    {"walnut",
     Family::branching,
     18,
     .38f,
     .38f,
     .42f,
     52,
     .08f,
     .45f,
     .3f,
     .5f,
     .12f,
     false,
     false,
     {.35, .3, .08, .05, .07, 1}},
    {"beech",
     Family::branching,
     18,
     .4f,
     .22f,
     .42f,
     76,
     .09f,
     .5f,
     .4f,
     .25f,
     .2f,
     false,
     true,
     {.25, .35, .025, .04, .035, 1}},
    {"ash",
     Family::branching,
     20,
     .36f,
     .4f,
     .38f,
     42,
     .085f,
     .30f,
     .45f,
     .65f,
     .1f,
     true,
     false,
     {.3, .3, .06, .05, .06, 1}},
    {"spruce",
     Family::whorled,
     20,
     .34f,
     .18f,
     .19f,
     95,
     .10f,
     .5f,
     .5f,
     .35f,
     .5f,
     false,
     false,
     {.3, .35, .05, .04, .04, 1}},
    {"pine",
     Family::whorled,
     24,
     .40f,
     .42f,
     .26f,
     80,
     .12f,
     .7f,
     .5f,
     .9f,
     .1f,
     false,
     false,
     {.4, .35, .07, .05, .07, 1}},
    {"cedar",
     Family::scale_spray,
     24,
     .55f,
     .16f,
     .22f,
     105,
     .12f,
     .65f,
     .5f,
     .8f,
     .35f,
     false,
     true,
     {.45, .7, .22, .10, .10, 1}},
    {"juniper",
     Family::scale_spray,
     10,
     .22f,
     .12f,
     .25f,
     65,
     .08f,
     .4f,
     .5f,
     .65f,
     .1f,
     false,
     false,
     {.35, .45, .16, .10, .08, 1}},
};

const Profile* find(std::string_view name) {
  for (const Profile& p : k_profiles)
    if (p.name == name) return &p;
  return nullptr;
}

struct Resolved {
  Broadleaf broadleaf;
  Conifer conifer;
  GrowthRules rules;
};

Resolved resolve(const Profile& p, const Tree& tree) {
  Resolved out;
  const f32 height = tree.height_m == 0 ? p.height : tree.height_m;
  const f64 scale = static_cast<f64>(height) / static_cast<f64>(p.height);
  const f32 radius =
      tree.trunk_radius_m > 0
          ? tree.trunk_radius_m
          : static_cast<f32>(static_cast<f64>(p.radius) * det::exp(det::log(scale) * 1.5));
  const f32 base = tree.crown_base >= 0 ? tree.crown_base : p.crown_base;
  out.rules.opposite = p.opposite;
  out.rules.terminal_leaf = p.name == "ash";
  out.rules.trunk_fraction = p.name == "elm"                          ? 0.6
                             : (p.name == "oak" || p.name == "maple") ? 0.7
                             : p.name == "walnut"                     ? 0.8
                                                                      : 1.0;
  out.rules.layered = p.layered;
  out.rules.droop = static_cast<f64>(p.droop);
  out.rules.wood = p.wood;
  Broadleaf& b = out.broadleaf;
  b.seed = tree.seed;
  b.height_m = height;
  b.trunk_radius_m = radius;
  b.crown_base = base;
  b.limbs = p.name == "elm" ? 6u : 10u;
  b.limb_angle_deg = p.limb_angle;
  b.limb_length = p.reach * tree.crown_scale;
  b.leaf_length_m = p.leaf;
  b.twig_length_m = p.twig;
  b.twig_radius_m = .0025f;
  b.leader_share = p.leader;
  b.upturn = p.upturn;
  b.leaf_area_index = 4.5f;
  b.leaves = 24;
  b.branching_ratio = p.opposite ? 5u : 4u;
  b.branches = p.opposite ? 6u : 5u;
  b.branch_angle_deg = p.name == "elm" ? 65.0f : 50.0f;
  b.wander_deg = p.name == "oak" ? 9.0f : 5.0f;
  b.leaf_width = p.name == "maple" ? .8f : .45f;
  // A terminal unit is a compound rachis with leaflet cards in the two compound profiles.
  // No giant single-leaf texture is needed; the same allometry carries the finer units.
  if (p.name == "walnut" || p.name == "ash") {
    b.leaves = p.name == "walnut" ? 8u : 3u;
    b.leaf_width = .3f;
    b.twig_radius_m = .002f;
    b.leaf_angle_deg = 65;
  }
  b.triangle_budget = tree.triangle_budget;
  b.card_bake_px = tree.card_bake_px;
  b.impostor_px = tree.impostor_px;

  Conifer& c = out.conifer;
  c.seed = tree.seed;
  c.height_m = height;
  c.trunk_radius_m = radius;
  c.crown_base = base;
  c.crown_radius_m = height * p.reach * tree.crown_scale;
  c.bottom_angle_deg = p.limb_angle;
  c.top_angle_deg = 35;
  c.upturn = p.upturn;
  c.pendulous = p.droop;
  c.needle_length_m = p.leaf;
  c.lateral_length_m = p.twig;
  c.triangle_budget = tree.triangle_budget;
  if (p.name == "pine") {
    c.whorls = 16;
    c.branches_per_whorl = 6;
    c.needle_years = 2;
    c.shoot_growth_m = .5f;
    c.laterals_per_m = 12;
    c.lateral_length_m = 1.4f;
    out.rules.crown_power = 1.8;
    out.rules.spray_lift = 1.4;
  }
  if (p.family == Family::scale_spray) {
    c.whorls = p.name == "cedar" ? 40u : 28u;
    c.branches_per_whorl = 7;
    c.interwhorl = 2;
    c.missing_branches = .06f;
    c.sparse_whorls = .04f;
    c.dead_share = .03f;
    c.needle_years = 8;
    c.shoot_growth_m = .25f;
    c.needle_cards = 10;
    c.laterals_per_m = 10;
    out.rules.irregular_stations = true;
    out.rules.spray_lift = p.name == "cedar" ? .12 : .65;
    out.rules.crown_power = p.name == "cedar" ? .9 : 1.4;
  }
  return out;
}
}  // namespace

bool is_tree_species(std::string_view species) { return find(species) != nullptr; }
bool is_evergreen_species(std::string_view species) {
  const Profile* p = find(species);
  return p != nullptr && p->family != Family::branching;
}

bool tree_in_range(std::string_view species, const Tree& tree, std::string& error) {
  const Profile* p = find(species);
  if (p == nullptr) {
    error = "unknown tree species: " + std::string(species);
    return false;
  }
  const auto bad = [&](const char* field) {
    error = std::string("engine.flora.Tree.") + field + " is out of range";
    return false;
  };
  if (!(tree.height_m == 0 || (tree.height_m >= 3 && tree.height_m <= 40))) return bad("height_m");
  if (!(tree.trunk_radius_m == 0 || (tree.trunk_radius_m >= .05f && tree.trunk_radius_m <= 1.5f)))
    return bad("trunk_radius_m");
  if (!(tree.crown_base == -1 || (tree.crown_base >= .1f && tree.crown_base <= .8f)))
    return bad("crown_base");
  if (!(tree.crown_scale >= .5f && tree.crown_scale <= 1.5f)) return bad("crown_scale");
  if (!(tree.triangle_budget >= 8000 && tree.triangle_budget <= 2000000))
    return bad("triangle_budget");
  if (tree.card_bake_px != 0 && !(tree.card_bake_px >= 16 && tree.card_bake_px <= 256))
    return bad("card_bake_px");
  if (tree.impostor_px != 0 && !(tree.impostor_px >= 32 && tree.impostor_px <= 1024))
    return bad("impostor_px");
  const Resolved r = resolve(*p, tree);
  return p->family == Family::branching ? broadleaf_in_range(r.broadleaf, error)
                                        : conifer_in_range(r.conifer, error);
}

bool solve_tree(std::string_view species, const Tree& tree, assets::MeshData& mesh,
                TreeReport* report, std::string& error, Motion& motion, TreeLevels* levels) {
  if (!tree_in_range(species, tree, error)) return false;
  const Profile& p = *find(species);
  const Resolved r = resolve(p, tree);
  return p.family == Family::branching
             ? solve_broadleaf_rules(r.broadleaf, r.rules, mesh, report, &error, &motion, levels)
             : solve_conifer_rules(r.conifer, r.rules, mesh, report, &error, &motion);
}
}  // namespace engine::flora::detail

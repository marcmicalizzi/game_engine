// The broadleaf's solver (trees.h; docs/subsystems/flora.md, "The broadleaf", "Allometry").
#include "kit.h"

#include <domain/flora/trees.h>
#include <domain/geometry/cluster.h>

#include <algorithm>
#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

// Independent streams per order, so a draw added to the crown never moves a limb.
enum class Stream : u64 { trunk = 11, limbs = 12, crown = 100000 };

constexpr u32 k_fine_rows = 16;  // the named shoots' resolution, whatever the budget
constexpr u32 k_part_cap = 400;  // the recursion's motion parts: a few hundred a tree
// The trunk radius the allometry is calibrated to: the old default oak, open-grown, 0.35 m at 12 m.
constexpr f64 k_reference_height = 12.0;
constexpr f64 k_reference_radius = 0.35;

f64 trunk_radius_of(const Broadleaf& p) {
  if (p.trunk_radius_m > 0.0f) return static_cast<f64>(p.trunk_radius_m);
  // H ~ D^b, so D ~ H^(1/b) (McMahon 1973; Niklas 1994).
  return k_reference_radius * det::exp(det::log(static_cast<f64>(p.height_m) / k_reference_height) /
                                       static_cast<f64>(p.allometry_exponent));
}

CrownSpec spec_of(const Broadleaf& p, const GrowthRules& rules = {}) {
  CrownSpec s;
  s.opposite = rules.opposite;
  s.droop = rules.droop;
  s.droop_class = 1;
  s.area_exponent = static_cast<f64>(p.area_exponent);
  s.leader_share = static_cast<f64>(p.leader_share);
  u32 laterals = p.branching_ratio - 1;
  if (laterals > p.twigs) laterals = p.twigs;
  s.laterals = laterals;
  s.length_ratio = static_cast<f64>(p.length_ratio);
  s.twig_radius = static_cast<f64>(p.twig_radius_m);
  s.twig_length = static_cast<f64>(p.twig_length_m);
  s.branch_angle = static_cast<f64>(p.branch_angle_deg) * k_degree;
  s.leader_angle = 0.25 * s.branch_angle;
  s.upturn = static_cast<f64>(p.upturn) * 40.0 * k_degree;
  s.wander = static_cast<f64>(p.wander_deg) * k_degree;
  s.from = 0.3;
  s.part_cap = k_part_cap;
  s.seed = p.seed;
  s.stream = static_cast<u64>(Stream::crown);
  return s;
}

// The structure: the trunk and the scaffold limbs (named, as before), and from every limb and the
// trunk's top the recursion to the species' twig. Grown once; it never depends on the budget.
Crown grow(const Broadleaf& p, const CrownSpec& spec, const GrowthRules& rules = {}) {
  Crown crown;
  crown.terminal_leaf = rules.terminal_leaf;
  const f64 height = static_cast<f64>(p.height_m);
  const f64 r_trunk = trunk_radius_of(p);
  const f64 wander = static_cast<f64>(p.wander_deg) * k_degree;
  const f64 upturn = static_cast<f64>(p.upturn) * 40.0 * k_degree;
  const f64 inv = 1.0 / spec.area_exponent;
  // The pipe rule at the trunk: its leader top keeps `leader_share`, the limbs share the rest.
  const f64 r_top = crown_leader_radius(spec, r_trunk);
  const f64 r_limb =
      r_trunk * det::exp(det::log((1.0 - spec.leader_share) / static_cast<f64>(p.limbs)) * inv);

  Rng trunk_rng = stream_of(p.seed, static_cast<u64>(Stream::trunk));
  const f64 lean = static_cast<f64>(p.lean_deg) * k_degree;
  const f64 azimuth = trunk_rng.unit() * 2.0 * k_pi;
  const D3 heading{det::cos(azimuth), 0.0, det::sin(azimuth)};
  const Axis trunk = grow_axis(D3{}, k_up * det::cos(lean) + heading * det::sin(lean),
                               height * rules.trunk_fraction / det::cos(lean), k_fine_rows * 2,
                               k_up, 0.3 / height, wander * 0.25, trunk_rng);
  crown_add_named(crown, trunk, r_trunk, r_top, k_crown_none);

  // The limbs, up the crown on the golden angle: the lowest longest, the upper ones shorter and a
  // third more upright.
  Rng limb_rng = stream_of(p.seed, static_cast<u64>(Stream::limbs));
  const f64 turn = limb_rng.unit() * 2.0 * k_pi;
  const f64 base = std::min(0.8, static_cast<f64>(p.crown_base) / rules.trunk_fraction);
  Vector<CrownRoot> roots;
  for (u32 i = 0; i < p.limbs; ++i) {
    const f64 f = rules.opposite
                      ? (static_cast<f64>(i / 2u) + 0.5) / static_cast<f64>((p.limbs + 1u) / 2u)
                      : (static_cast<f64>(i) + 0.5) / static_cast<f64>(p.limbs);
    const Axis& t_axis = crown.axes[0];
    const f64 s = t_axis.length * (base + (0.9 - base) * f);
    const f64 jitter_angle = limb_rng.symmetric();
    const f64 angle = static_cast<f64>(p.limb_angle_deg) * k_degree * (1.0 - f / 3.0) *
                      (1.0 + 0.1 * jitter_angle);
    const f64 jitter_around = limb_rng.symmetric();
    D3 radial;
    const D3 dir = branch_direction(t_axis, s,
                                    turn +
                                        (rules.opposite ? static_cast<f64>(i / 2u) * 0.5 * k_pi +
                                                              static_cast<f64>(i % 2u) * k_pi
                                                        : static_cast<f64>(i) * k_golden_angle) +
                                        0.15 * jitter_around,
                                    angle, &radial);
    const f64 jitter_length = limb_rng.symmetric();
    const f64 requested = static_cast<f64>(p.limb_length) * height *
                          (rules.layered ? (0.65 + 0.35 * det::sin(k_pi * f)) : (1.0 - 0.5 * f)) *
                          (1.0 + 0.1 * jitter_length);
    const f64 length = supported_length(requested, r_limb, rules);
    const f64 r_here = lerp(r_trunk, r_top, s / t_axis.length);
    const Axis limb = grow_axis(axis_point(t_axis, s) + radial * (0.5 * r_here), dir, length,
                                k_fine_rows, k_up, upturn / length, wander, limb_rng);
    CrownRoot root;
    root.node = crown_add_named(crown, limb, r_limb, crown_leader_radius(spec, r_limb), 0);
    root.radius = r_limb;
    root.laterals = p.branches;
    root.lateral_length = static_cast<f64>(p.branch_length) * length;
    root.from = 0.25;
    root.to = 0.95;
    roots.push_back(root);
  }
  // The leader's top: a crown of the recursion where the limbs stop.
  CrownRoot top;
  top.node = 0;
  top.radius = r_top;
  top.laterals = spec.laterals;
  top.lateral_length =
      static_cast<f64>(p.branch_length) * static_cast<f64>(p.limb_length) * height * 0.5;
  top.from = 0.86;
  top.to = 1.0;
  top.spread_scale = 0.8;
  roots.push_back(top);
  crown_grow(crown, spec, roots, 2);
  return crown;
}

// ---- the plan ----------------------------------------------------------------------------------

struct Plan {
  u32 sides[2] = {12, 8};  // trunk, limbs
  u32 rows[2] = {24, 14};
  CrownPlan crown;
};

u32 count_triangles(const Broadleaf& p, const Crown& crown, const Plan& plan) {
  return tube_triangles(plan.rows[0], plan.sides[0]) +
         p.limbs * tube_triangles(plan.rows[1], plan.sides[1]) + crown_triangles(crown, plan.crown);
}

// The ladder (flora.md, "The budget"): the finest wood first (a twig's few millimetres are under a
// pixel past a few metres), then rows and cross-sections, then the finer orders' wood folded into
// their parts, then — only where the budget forces it — leaf cards merged up the hierarchy, finest
// first, and the last of the rows. No rung moves a shoot or drops a part: the structure, the part
// list and the motion contract's parts are the same on every rung.
bool plan_broadleaf(const Broadleaf& p, const Crown& crown, Plan& plan, u32& steps) {
  plan = Plan{};
  const f64 height = static_cast<f64>(p.height_m);
  u32 trunk_rows = static_cast<u32>(height / 0.4);
  plan.rows[0] = trunk_rows < 6 ? 6 : (trunk_rows > 32 ? 32 : trunk_rows);
  plan.crown.edge_m = 0.06;
  plan.crown.max_sides = 8;
  plan.crown.row_m = 0.5;
  plan.crown.wood_class = 1;
  steps = 0;
  const auto fits = [&] { return count_triangles(p, crown, plan) <= p.triangle_budget; };
  if (fits()) return true;
  const auto step = [&](auto&& change) {
    change();
    ++steps;
    return fits();
  };
  if (step([&] { plan.crown.wood_class = 2; })) return true;
  if (step([&] {
        plan.crown.row_m = 1.0;
        plan.rows[1] = 8;
      })) {
    return true;
  }
  if (step([&] {
        plan.crown.edge_m = 0.1;
        plan.crown.max_sides = 6;
        plan.sides[1] = 6;
        plan.sides[0] = 10;
      })) {
    return true;
  }
  while (plan.crown.wood_class < crown.part_class) {
    if (step([&] { ++plan.crown.wood_class; })) return true;
  }
  if (step([&] {
        plan.crown.edge_m = 0.2;
        plan.crown.max_sides = 4;
        plan.sides[1] = 5;
        plan.sides[0] = 8;
        plan.rows[0] = plan.rows[0] / 2 < 4 ? 4 : plan.rows[0] / 2;
      })) {
    return true;
  }
  while (plan.crown.merge < crown.orders) {
    if (step([&] { ++plan.crown.merge; })) return true;
  }
  for (u32 k = 0; k < 6; ++k) {
    if (step([&] { ++plan.crown.halvings; })) return true;
  }
  return step([&] {
    plan.rows[0] = 3;
    plan.rows[1] = 2;
    plan.sides[1] = 4;
    plan.sides[0] = 6;
    plan.crown.row_m = 8.0;
    plan.crown.max_sides = 3;
  });
}

// The leaf count by the leaf area index: one-sided leaf area over the crown's footprint, each card
// one leaf of the species' size (its area `leaf_width` times its length squared).
void place_leaves(const Broadleaf& p, Crown& crown) {
  const f64 length = static_cast<f64>(p.leaf_length_m);
  const f64 width = static_cast<f64>(p.leaf_width) * length;
  const f64 angle = static_cast<f64>(p.leaf_angle_deg) * k_degree;
  crown.card_step = width / det::sin(angle);
  const f64 cards = static_cast<f64>(p.leaf_area_index) * crown.footprint / (width * length);
  crown_distribute_leaves(crown, cards, p.leaves);
}

}  // namespace

bool broadleaf_in_range(const Broadleaf& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.Broadleaf.") + field + " is out of range (" + range + ")";
    return false;
  };
  if (!(p.height_m >= 3.0f && p.height_m <= 40.0f)) return bad("height_m", "3 to 40 m");
  if (!(p.trunk_radius_m == 0.0f || (p.trunk_radius_m >= 0.05f && p.trunk_radius_m <= 2.0f)))
    return bad("trunk_radius_m", "0, or 0.05 to 2 m");
  if (!(p.crown_base >= 0.1f && p.crown_base <= 0.8f)) return bad("crown_base", "0.1 to 0.8");
  if (!(p.lean_deg >= 0.0f && p.lean_deg <= 30.0f)) return bad("lean_deg", "0 to 30 degrees");
  if (!(p.limbs >= 1u && p.limbs <= 12u)) return bad("limbs", "1 to 12");
  if (!(p.limb_angle_deg >= 15.0f && p.limb_angle_deg <= 85.0f))
    return bad("limb_angle_deg", "15 to 85 degrees");
  if (!(p.limb_length >= 0.15f && p.limb_length <= 0.9f)) return bad("limb_length", "0.15 to 0.9");
  if (!(p.branches >= 1u && p.branches <= 8u)) return bad("branches", "1 to 8");
  if (!(p.twigs >= 1u && p.twigs <= 10u)) return bad("twigs", "1 to 10");
  if (!(p.branch_angle_deg >= 15.0f && p.branch_angle_deg <= 85.0f))
    return bad("branch_angle_deg", "15 to 85 degrees");
  if (!(p.branch_length >= 0.2f && p.branch_length <= 0.9f))
    return bad("branch_length", "0.2 to 0.9");
  if (!(p.twig_length_m >= 0.1f && p.twig_length_m <= 3.0f))
    return bad("twig_length_m", "0.1 to 3 m");
  if (!(p.upturn >= -1.0f && p.upturn <= 1.0f)) return bad("upturn", "-1 to 1");
  if (!(p.wander_deg >= 0.0f && p.wander_deg <= 25.0f)) return bad("wander_deg", "0 to 25 degrees");
  if (!(p.leaves >= 1u && p.leaves <= 40u)) return bad("leaves", "1 to 40");
  if (!(p.leaf_length_m >= 0.03f && p.leaf_length_m <= 0.6f))
    return bad("leaf_length_m", "0.03 to 0.6 m");
  if (!(p.leaf_angle_deg >= 10.0f && p.leaf_angle_deg <= 85.0f))
    return bad("leaf_angle_deg", "10 to 85 degrees");
  if (!(p.triangle_budget >= 8000u && p.triangle_budget <= 2000000u))
    return bad("triangle_budget", "8,000 to 2,000,000");
  if (!(p.twig_radius_m >= 0.001f && p.twig_radius_m <= 0.012f))
    return bad("twig_radius_m", "0.001 to 0.012 m");
  if (!(p.area_exponent >= 1.8f && p.area_exponent <= 3.0f))
    return bad("area_exponent", "1.8 to 3");
  if (!(p.branching_ratio >= 2u && p.branching_ratio <= 6u))
    return bad("branching_ratio", "2 to 6");
  if (!(p.length_ratio >= 0.4f && p.length_ratio <= 0.9f)) return bad("length_ratio", "0.4 to 0.9");
  if (!(p.leader_share >= 0.1f && p.leader_share <= 0.7f)) return bad("leader_share", "0.1 to 0.7");
  if (!(p.leaf_area_index >= 0.5f && p.leaf_area_index <= 10.0f))
    return bad("leaf_area_index", "0.5 to 10");
  if (!(p.leaf_width >= 0.1f && p.leaf_width <= 1.0f)) return bad("leaf_width", "0.1 to 1");
  if (p.card_bake_px != 0u && !(p.card_bake_px >= 16u && p.card_bake_px <= 256u))
    return bad("card_bake_px", "0, or 16 to 256");
  if (p.impostor_px != 0u && !(p.impostor_px >= 32u && p.impostor_px <= 1024u))
    return bad("impostor_px", "0, or 32 to 1024");
  if (!(p.allometry_exponent >= 0.4f && p.allometry_exponent <= 1.0f))
    return bad("allometry_exponent", "0.4 to 1");
  return true;
}

bool broadleaf_crown(const Broadleaf& p, Vector<CrownShoot>& out) {
  std::string error;
  if (!broadleaf_in_range(p, error)) return false;
  crown_table(grow(p, spec_of(p)), out);
  return true;
}

bool solve_broadleaf(const Broadleaf& p, assets::MeshData& out, TreeReport* report,
                     std::string* error, Motion* motion) {
  return solve_broadleaf_chain(p, out, report, error, motion, nullptr);
}

bool solve_broadleaf_chain(const Broadleaf& p, assets::MeshData& out, TreeReport* report,
                           std::string* error, Motion* motion, TreeLevels* levels) {
  return detail::solve_broadleaf_rules(p, GrowthRules{}, out, report, error, motion, levels);
}

bool detail::solve_broadleaf_rules(const Broadleaf& p, const GrowthRules& rules,
                                   assets::MeshData& out, TreeReport* report, std::string* error,
                                   Motion* motion, TreeLevels* levels) {
  out = assets::MeshData{};
  std::string message;
  if (!broadleaf_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  const CrownSpec spec = spec_of(p, rules);
  Crown crown = grow(p, spec, rules);
  place_leaves(p, crown);
  Plan plan;
  u32 steps = 0;
  if (!plan_broadleaf(p, crown, plan, steps)) {
    if (error != nullptr) {
      *error = "engine.flora.Broadleaf.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this tree: its coarsest form needs " +
               std::to_string(count_triangles(p, crown, plan)) + " triangles";
    }
    return false;
  }

  out.materials.push_back(material("bark", Vec4{0.30f, 0.22f, 0.15f, 1.0f}, 0.9f, false));
  out.materials.push_back(material("leaf", Vec4{0.10f, 0.24f, 0.05f, 1.0f}, 0.6f, true));
  Motion parts;
  Emitter emit{out, parts};
  emit.wood = rules.wood;
  emit.wood.seed = p.seed;

  // The bark: the trunk and the limbs, then the recursion parents first, each part a tube.
  for (u32 i = 0; i < crown.first_recursion; ++i) {
    CrownNode& n = crown.nodes[i];
    const u32 order = i == 0 ? 0u : 1u;
    const Axis& axis = crown.axes[n.axis];
    n.part = emit.begin_part(axis.point[0], axis.tangent[0], axis.length,
                             i == 0 ? geometry::k_motion_no_parent : crown.nodes[0].part);
    build_tube(resample_axis(axis, plan.rows[order]), n.r0, n.r1, plan.sides[order],
               Vec2{0.0f, 0.0f}, emit);
  }
  crown_emit_wood(crown, plan.crown, emit, false);
  assets::Primitive bark;
  bark.first_index = 0;
  bark.index_count = out.indices.size();
  bark.material = 0;
  out.primitives.push_back(bark);

  // The leaves: one spray a leafy twig, its plane laid level so the cards face the sky.
  const u32 leaf_vertex = out.positions.size();
  const u32 leaf_first = out.indices.size();
  Vector<SprayRange> spray_ranges;
  const f64 card_length =
      crown_emit_leaves(crown, plan.crown, emit, static_cast<f64>(p.leaf_length_m),
                        static_cast<f64>(p.leaf_angle_deg) * k_degree, 15.0 * k_degree, 0.15,
                        levels != nullptr ? &spray_ranges : nullptr);
  assets::Primitive leaves;
  leaves.first_index = leaf_first;
  leaves.index_count = out.indices.size() - leaf_first;
  leaves.material = 1;
  out.primitives.push_back(leaves);
  smooth_normals(out, leaf_vertex, leaf_first);

  if (report != nullptr) {
    *report = TreeReport{};
    report->triangles = out.indices.size() / 3;
    report->wood_triangles = bark.index_count / 3;
    report->leaf_triangles = leaves.index_count / 3;
    report->vertices = out.positions.size();
    report->parts = parts.parts.size();
    report->shoots = crown.nodes.size();
    report->cards_per_side = 0;
    u32 sprays = 0;
    for (const u32 t : crown.twigs) {
      if (crown.nodes[t].cards > 0u) ++sprays;
      if (crown.nodes[t].cards > report->cards_per_side)
        report->cards_per_side = crown.nodes[t].cards;
    }
    report->sprays = sprays;
    report->trunk_sides = plan.sides[0];
    report->shoot_sides = plan.crown.max_sides;
    report->steps = steps;
    report->twigs = crown.twigs.size();
    report->orders = crown.orders;
    report->leaf_cards = report->leaf_triangles / 2;
    report->merge_level = plan.crown.merge;
    report->wood_class = static_cast<u32>(plan.crown.wood_class);
    report->part_class = static_cast<u32>(crown.part_class);
    report->card_length = static_cast<f32>(card_length);
    report->footprint_m2 = static_cast<f32>(crown.footprint);
    report->leaf_area_m2 = static_cast<f32>(triangles_area(out, leaf_first));
    report->leaf_area_index =
        crown.footprint > 0.0 ? report->leaf_area_m2 / report->footprint_m2 : 0.0f;
  }
  // The impostor chain above the finest leaves (impostor.h): appended past everything, so the
  // mesh above is the bytes it is without it.
  if (levels != nullptr) {
    *levels = TreeLevels{};
    levels->leaf_first_index = leaf_first;
    levels->leaf_index_end = out.indices.size();
    ImpostorSpec chain_spec;
    chain_spec.card_px = p.card_bake_px;
    chain_spec.tree_px = p.impostor_px;
    crown_levels(crown, plan.crown, spray_ranges, chain_spec, emit, *levels);
    tree_star(chain_spec, crown.nodes[0].part, emit, *levels);
  }
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

}  // namespace engine::flora

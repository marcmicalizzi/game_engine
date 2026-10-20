// The shrub's solver (understory.h; docs/subsystems/flora.md, "The shrub").
#include "kit.h"

#include <domain/flora/structure.h>
#include <domain/flora/understory.h>

#include <cmath>
#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

// One stream per stem and the recursion's after them, so a draw added to one stem moves no other.
constexpr u64 k_stem_stream = 11;
constexpr u64 k_crown_stream = 100000;
constexpr u32 k_fine_rows = 16;  // the stems' resolution, whatever the budget
constexpr u32 k_part_cap = 160;  // the recursion's motion parts: a shrub's few hundred shoots

// The stem radius the allometry is calibrated to: a 4 m coppiced hazel's stoutest stem, 4.4 cm
// across at the ground.
constexpr f64 k_reference_height = 4.0;
constexpr f64 k_reference_radius = 0.022;

f64 stem_radius_of(const Shrub& p) {
  if (p.stem_radius_m > 0.0f) return static_cast<f64>(p.stem_radius_m);
  // Elastic similarity, H ~ D^(2/3), as the broadleaf's trunk (McMahon 1973; Niklas 1994).
  return k_reference_radius *
         det::exp(det::log(static_cast<f64>(p.height_m) / k_reference_height) * 1.5);
}

CrownSpec spec_of(const Shrub& p) {
  CrownSpec s;
  s.area_exponent = static_cast<f64>(p.area_exponent);
  s.leader_share = static_cast<f64>(p.leader_share);
  s.laterals = p.branching_ratio - 1;
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
  s.stream = k_crown_stream;
  return s;
}

// The structure: the stems from the stool, and from every stem the recursion to the species'
// twig. Grown once; it never depends on the budget.
Crown grow(const Shrub& p, const CrownSpec& spec) {
  Crown crown;
  const f64 height = static_cast<f64>(p.height_m);
  const f64 r_max = stem_radius_of(p);
  const f64 stool = static_cast<f64>(p.stool_radius_m);
  const f64 spread = static_cast<f64>(p.stem_spread_deg) * k_degree;
  const f64 wander = static_cast<f64>(p.wander_deg) * k_degree;
  const f64 upturn = static_cast<f64>(p.upturn) * 40.0 * k_degree;
  const f64 n = static_cast<f64>(p.stems);
  Rng turn_rng = stream_of(p.seed, k_stem_stream - 1);
  const f64 turn = turn_rng.unit() * 2.0 * k_pi;
  Vector<CrownRoot> roots;
  for (u32 i = 0; i < p.stems; ++i) {
    Rng rng = stream_of(p.seed, k_stem_stream + i);
    // The stems' feet fill the stool as a sunflower does (the first at its middle), the stoutest
    // first: a stool's oldest stems stand at its centre and its young shoots round the rim.
    const f64 size = i == 0 ? 1.0 : 0.55 + 0.45 * rng.unit();
    const f64 out = std::sqrt(n > 1.0 ? static_cast<f64>(i) / (n - 1.0) : 0.0);
    const f64 around = turn + static_cast<f64>(i) * k_golden_angle;
    const D3 radial{det::cos(around), 0.0, det::sin(around)};
    const D3 foot = radial * (stool * out);
    const f64 jitter_lean = rng.unit();
    const f64 lean = spread * (0.25 + 0.75 * out) * (0.8 + 0.4 * jitter_lean);
    const f64 jitter_azimuth = rng.symmetric();
    const f64 azimuth = around + 0.3 * jitter_azimuth;
    const D3 heading{det::cos(azimuth), 0.0, det::sin(azimuth)};
    const D3 dir = k_up * det::cos(lean) + heading * det::sin(lean);
    const f64 r0 = r_max * size;
    // A thinner stem is a younger one and shorter: its height by elastic similarity, H ~ r^(2/3).
    const f64 tall = height * det::exp(det::log(size) * (2.0 / 3.0));
    const f64 length = tall / (0.5 + 0.5 * det::cos(lean));
    const Axis axis =
        grow_axis(foot, dir, length, k_fine_rows, k_up, upturn / length, wander * 0.5, rng);
    CrownRoot root;
    root.node = crown_add_named(crown, axis, r0, crown_leader_radius(spec, r0), k_crown_none);
    root.radius = r0;
    root.laterals = p.branches;
    root.lateral_length = static_cast<f64>(p.branch_length) * length;
    root.from = 0.3;
    root.to = 0.95;
    roots.push_back(root);
  }
  crown_grow(crown, spec, roots, 1);
  return crown;
}

// ---- the plan ----------------------------------------------------------------------------------

struct Plan {
  u32 stem_sides = 8;
  u32 stem_rows = 12;
  CrownPlan crown;
};

// A stem's sides by its girth, as the recursion's tubes are (`crown_tube_sides`), at most the
// plan's: a bramble's 6 mm cane is three sides round, a hazel's 2 cm stem six.
u32 stem_sides_of(const Plan& plan, f64 radius) {
  CrownPlan stem = plan.crown;
  stem.max_sides = plan.stem_sides;
  return crown_tube_sides(stem, radius);
}

u32 count_triangles(const Shrub& p, const Crown& crown, const Plan& plan) {
  (void)p;
  u32 n = crown_triangles(crown, plan.crown);
  for (u32 i = 0; i < crown.first_recursion; ++i)
    n += tube_triangles(plan.stem_rows, stem_sides_of(plan, crown.nodes[i].r0));
  return n;
}

// The broadleaf's ladder at a shrub's scale: the finest wood first, rows and cross-sections, the
// finer orders' wood folded into their parts, then the cards merged up the hierarchy and halved,
// and the last rows. No rung moves a shoot or drops a part.
bool plan_shrub(const Shrub& p, const Crown& crown, Plan& plan, u32& steps) {
  plan = Plan{};
  plan.crown.edge_m = 0.025;
  plan.crown.max_sides = 6;
  plan.crown.row_m = 0.3;
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
        plan.crown.row_m = 0.6;
        plan.stem_rows = 8;
      })) {
    return true;
  }
  if (step([&] {
        plan.crown.edge_m = 0.05;
        plan.crown.max_sides = 4;
        plan.stem_sides = 6;
      })) {
    return true;
  }
  while (plan.crown.wood_class < crown.part_class) {
    if (step([&] { ++plan.crown.wood_class; })) return true;
  }
  if (step([&] {
        plan.crown.edge_m = 0.12;
        plan.crown.max_sides = 3;
        plan.stem_sides = 4;
        plan.stem_rows = 4;
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
    plan.stem_rows = 2;
    plan.stem_sides = 3;
    plan.crown.row_m = 8.0;
  });
}

void place_leaves(const Shrub& p, Crown& crown) {
  const f64 length = static_cast<f64>(p.leaf_length_m);
  const f64 width = static_cast<f64>(p.leaf_width) * length;
  const f64 angle = static_cast<f64>(p.leaf_angle_deg) * k_degree;
  crown.card_step = width / det::sin(angle);
  const f64 cards = static_cast<f64>(p.leaf_area_index) * crown.footprint / (width * length);
  crown_distribute_leaves(crown, cards, p.leaves);
}

const char* tier_name(DetailTier t) {
  switch (t) {
    case DetailTier::essential: return "essential";
    case DetailTier::standard: return "standard";
    case DetailTier::rich: break;
  }
  return "rich";
}

}  // namespace

bool shrub_collides(const Shrub& p) noexcept {
  return stem_radius_of(p) >= static_cast<f64>(k_shrub_collider_radius_m);
}

bool shrub_in_range(const Shrub& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.Shrub.") + field + " is out of range (" + range + ")";
    return false;
  };
  if (!(p.height_m >= 0.5f && p.height_m <= 6.0f)) return bad("height_m", "0.5 to 6 m");
  if (!(p.stems >= 1u && p.stems <= 40u)) return bad("stems", "1 to 40");
  if (!(p.stem_radius_m == 0.0f || (p.stem_radius_m >= 0.003f && p.stem_radius_m <= 0.08f)))
    return bad("stem_radius_m", "0, or 0.003 to 0.08 m");
  if (!(p.stool_radius_m >= 0.02f && p.stool_radius_m <= 1.5f))
    return bad("stool_radius_m", "0.02 to 1.5 m");
  if (!(p.stem_spread_deg >= 0.0f && p.stem_spread_deg <= 70.0f))
    return bad("stem_spread_deg", "0 to 70 degrees");
  if (!(p.upturn >= -1.0f && p.upturn <= 1.0f)) return bad("upturn", "-1 to 1");
  if (!(p.wander_deg >= 0.0f && p.wander_deg <= 25.0f)) return bad("wander_deg", "0 to 25 degrees");
  if (!(p.branches >= 1u && p.branches <= 12u)) return bad("branches", "1 to 12");
  if (!(p.branch_length >= 0.1f && p.branch_length <= 0.8f))
    return bad("branch_length", "0.1 to 0.8");
  if (!(p.branch_angle_deg >= 15.0f && p.branch_angle_deg <= 85.0f))
    return bad("branch_angle_deg", "15 to 85 degrees");
  if (!(p.twig_radius_m >= 0.0008f && p.twig_radius_m <= 0.008f))
    return bad("twig_radius_m", "0.0008 to 0.008 m");
  if (!(p.twig_length_m >= 0.05f && p.twig_length_m <= 1.0f))
    return bad("twig_length_m", "0.05 to 1 m");
  if (!(p.area_exponent >= 1.8f && p.area_exponent <= 3.0f))
    return bad("area_exponent", "1.8 to 3");
  if (!(p.branching_ratio >= 2u && p.branching_ratio <= 6u))
    return bad("branching_ratio", "2 to 6");
  if (!(p.length_ratio >= 0.4f && p.length_ratio <= 0.9f)) return bad("length_ratio", "0.4 to 0.9");
  if (!(p.leader_share >= 0.1f && p.leader_share <= 0.7f)) return bad("leader_share", "0.1 to 0.7");
  if (!(p.leaf_area_index >= 0.5f && p.leaf_area_index <= 8.0f))
    return bad("leaf_area_index", "0.5 to 8");
  if (!(p.leaves >= 1u && p.leaves <= 30u)) return bad("leaves", "1 to 30");
  if (!(p.leaf_length_m >= 0.02f && p.leaf_length_m <= 0.3f))
    return bad("leaf_length_m", "0.02 to 0.3 m");
  if (!(p.leaf_width >= 0.1f && p.leaf_width <= 1.0f)) return bad("leaf_width", "0.1 to 1");
  if (!(p.leaf_angle_deg >= 10.0f && p.leaf_angle_deg <= 85.0f))
    return bad("leaf_angle_deg", "10 to 85 degrees");
  if (!(p.triangle_budget >= 4000u && p.triangle_budget <= 400000u))
    return bad("triangle_budget", "4,000 to 400,000");
  if (p.detail_tier == DetailTier::rich && shrub_collides(p)) {
    error = std::string("engine.flora.Shrub.detail_tier is ") + tier_name(p.detail_tier) +
            ", but the shrub's stems collide (its stoutest is " +
            std::to_string(stem_radius_of(p)) +
            " m in radius, at least 0.0125 m): a kind whose instances carry a collision body is "
            "never rich, "
            "so a preset that drops it never changes what a walker meets";
    return false;
  }
  return true;
}

bool solve_shrub(const Shrub& p, assets::MeshData& out, TreeReport* report, std::string* error,
                 Motion* motion) {
  out = assets::MeshData{};
  std::string message;
  if (!shrub_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  const CrownSpec spec = spec_of(p);
  Crown crown = grow(p, spec);
  place_leaves(p, crown);
  Plan plan;
  u32 steps = 0;
  if (!plan_shrub(p, crown, plan, steps)) {
    if (error != nullptr) {
      *error = "engine.flora.Shrub.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this shrub: its coarsest form needs " +
               std::to_string(count_triangles(p, crown, plan)) + " triangles";
    }
    return false;
  }

  out.materials.push_back(material("bark", Vec4{0.33f, 0.25f, 0.18f, 1.0f}, 0.85f, false));
  out.materials.push_back(material("leaf", Vec4{0.13f, 0.28f, 0.06f, 1.0f}, 0.6f, true));
  Motion parts;
  Emitter emit{out, parts};

  // Part 0 is the stool: a pivot at the ground and no geometry, which every stem hangs from.
  begin_structural_part(emit, D3{}, k_up, 0.1, geometry::k_motion_no_parent,
                        static_cast<f64>(p.stool_radius_m), static_cast<f64>(p.stool_radius_m), 0u);
  u32 colliders = 0;
  for (u32 i = 0; i < crown.first_recursion; ++i) {
    CrownNode& n = crown.nodes[i];
    const Axis& axis = crown.axes[n.axis];
    const bool collides = n.r0 >= static_cast<f64>(k_shrub_collider_radius_m);
    if (collides) ++colliders;
    n.part = begin_structural_part(emit, axis.point[0], axis.tangent[0], axis.length, 0, n.r0, n.r1,
                                   collides ? k_collider : 0u);
    build_tube(resample_axis(axis, plan.stem_rows), n.r0, n.r1, stem_sides_of(plan, n.r0),
               Vec2{0.0f, 0.0f}, emit);
  }
  crown_emit_wood(crown, plan.crown, emit, true);
  assets::Primitive bark;
  bark.first_index = 0;
  bark.index_count = out.indices.size();
  bark.material = 0;
  out.primitives.push_back(bark);

  const u32 leaf_vertex = out.positions.size();
  const u32 leaf_first = out.indices.size();
  const f64 card_length =
      crown_emit_leaves(crown, plan.crown, emit, static_cast<f64>(p.leaf_length_m),
                        static_cast<f64>(p.leaf_angle_deg) * k_degree, 15.0 * k_degree, 0.12);
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
    u32 sprays = 0;
    for (const u32 t : crown.twigs) {
      if (crown.nodes[t].cards > 0u) ++sprays;
      if (crown.nodes[t].cards > report->cards_per_side)
        report->cards_per_side = crown.nodes[t].cards;
    }
    report->sprays = sprays;
    report->trunk_sides = plan.stem_sides;
    report->shoot_sides = plan.crown.max_sides;
    report->steps = steps;
    report->colliders = colliders;
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
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

}  // namespace engine::flora

// The birch's solver (trees.h; docs/subsystems/flora.md, "The birch"; the grounding is
// docs/experiments/birch.md).
#include "kit.h"

#include <domain/flora/structure.h>
#include <domain/flora/trees.h>
#include <domain/geometry/cluster.h>

#include <algorithm>
#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

// Independent streams, so a draw added to one never moves another: the clump's stem count has a
// stream of its own, which is why a seeded clump of n stems is the explicit `stems = n` birch.
enum class Stream : u64 { stems = 21, limbs = 22, clump = 24, crown = 100000 };

constexpr u32 k_fine_rows = 16;  // the named shoots' resolution, whatever the budget
constexpr u32 k_part_cap = 160;  // the recursion's motion parts: a forest has hundreds of birches
// A clump's second and later stems are this share of the first's radius and more (flora.md, "The
// structural table": the thinnest load-bearing part is the thinnest birch's 0.03 m times this).
constexpr f64 k_least_stem = 0.6;

// The stem count: `stems` when the recipe names it, else drawn from the clump stream — one draw
// for whether the birch is a clump (`clump_chance`), and for a clump one more stem with even odds
// at each count up to `max_stems` (half the clumps two stems, a quarter three, ...).
u32 stem_count_of(const Birch& p) {
  if (p.stems != 0u) return p.stems;
  Rng rng = stream_of(p.seed, static_cast<u64>(Stream::clump));
  const f64 clump = rng.unit();
  if (!(clump < static_cast<f64>(p.clump_chance))) return 1;
  u32 n = 2;
  while (n < p.max_stems) {
    const f64 more = rng.unit();
    if (!(more < 0.5)) break;
    ++n;
  }
  return n;
}

CrownSpec spec_of(const Birch& p) {
  CrownSpec s;
  s.area_exponent = static_cast<f64>(p.area_exponent);
  s.leader_share = static_cast<f64>(p.leader_share);
  u32 laterals = p.branching_ratio - 1;
  if (laterals > p.twigs) laterals = p.twigs;
  s.laterals = laterals;
  s.length_ratio = static_cast<f64>(p.length_ratio);
  s.twig_radius = static_cast<f64>(p.twig_radius_m);
  s.twig_length = static_cast<f64>(p.twig_length_m);
  s.branch_angle = static_cast<f64>(p.limb_angle_deg) * k_degree;
  s.leader_angle = 0.25 * s.branch_angle;
  s.upturn = 0.1 * 40.0 * k_degree;  // the finer orders spread rather than climb
  s.wander = static_cast<f64>(p.wander_deg) * k_degree;
  s.from = 0.25;
  s.part_cap = k_part_cap;
  s.seed = p.seed;
  s.stream = static_cast<u64>(Stream::crown);
  // The pendulous habit (Betula pendula): the branchlets hang, their parents arch.
  s.droop = static_cast<f64>(p.droop) * 80.0 * k_degree;
  s.droop_class = 1;
  return s;
}

// The ovoid crown's reach at `f` up it (0 its base, 1 its top), as a share of its widest: an
// ellipse widest two fifths of the way up, a birch's crown being widest below its middle.
f64 ovoid(f64 f) {
  const f64 d = (f - 0.4) / 0.65;
  const f64 e = 1.0 - d * d;
  return e < 0.12 ? 0.35 : std::sqrt(e);
}

// The structure: the stems from one root collar, the scaffold limbs up every stem's crown, and
// from every limb and every stem's top the recursion to the species' branchlet. Grown once; it
// never depends on the budget.
Crown grow(const Birch& p, const CrownSpec& spec) {
  Crown crown;
  crown.leaf_placement = LeafPlacement::alternate_simple;
  crown.leaf_seed = p.seed;
  crown.leaf_width = static_cast<f64>(p.leaf_width);
  crown.leaf_length = static_cast<f64>(p.leaf_length_m);
  const u32 stems = stem_count_of(p);
  const f64 height = static_cast<f64>(p.height_m);
  const f64 r_stem = static_cast<f64>(p.stem_radius_m);
  const f64 wander = static_cast<f64>(p.wander_deg) * k_degree;
  const f64 spread = static_cast<f64>(p.stem_spread_deg) * k_degree;
  const f64 inv = 1.0 / spec.area_exponent;

  // The stems. A single birch stands at the origin, nearly upright. A clump's stems are sprouts
  // from one stump's root collar (Zasada et al. 1978; Hytönen 2019: over half within 2 cm of the
  // ground): their feet on a ring nine tenths the stoutest stem's radius out, each leaning out by
  // about `stem_spread_deg` and straightening towards the light, the first the stoutest and
  // tallest and the others thinner and, by elastic similarity (H ~ D^(2/3)), shorter.
  Rng stem_rng = stream_of(p.seed, static_cast<u64>(Stream::stems));
  const f64 turn = stem_rng.unit() * 2.0 * k_pi;
  Vector<f64> size(stems, 1.0);
  for (u32 k = 0; k < stems; ++k) {
    const f64 jitter_size = stem_rng.unit();
    const f64 jitter_around = stem_rng.symmetric();
    const f64 jitter_lean = stem_rng.unit();
    if (k > 0) size[k] = k_least_stem + (0.95 - k_least_stem) * jitter_size;
    const f64 around = turn + 2.0 * k_pi * static_cast<f64>(k) / static_cast<f64>(stems) +
                       (stems > 1 ? 0.35 * jitter_around : 0.0);
    const D3 heading{det::cos(around), 0.0, det::sin(around)};
    const f64 lean =
        stems == 1 ? 2.0 * k_degree : spread * (k == 0 ? 0.5 : 1.0) * (0.75 + 0.5 * jitter_lean);
    const f64 r0 = r_stem * size[k];
    const f64 tall = height * det::exp(det::log(size[k]) * (2.0 / 3.0));
    const D3 foot = stems == 1 ? D3{} : heading * (0.9 * r_stem);
    const Axis axis = grow_axis(foot, k_up * det::cos(lean) + heading * det::sin(lean),
                                tall / (0.5 + 0.5 * det::cos(lean)), k_fine_rows * 2, k_up,
                                1.2 / tall, wander * 0.25, stem_rng);
    crown_add_named(crown, axis, r0, crown_leader_radius(spec, r0), k_crown_none);
  }

  // The limbs, up every stem's crown on the golden angle from `crown_base`: many, slender and
  // steep (`limb_angle_deg` at the lowest, the upper ones a half more upright), their lengths
  // following the ovoid, their ends arching down by the droop. By the pipe rule the stem's top
  // keeps `leader_share` of its cross-section and the limbs share the rest.
  Rng limb_rng = stream_of(p.seed, static_cast<u64>(Stream::limbs));
  const f64 base = static_cast<f64>(p.crown_base);
  const f64 droop = static_cast<f64>(p.droop);
  Vector<CrownRoot> roots;
  for (u32 k = 0; k < stems; ++k) {
    const f64 lturn = limb_rng.unit() * 2.0 * k_pi;
    const f64 r0 = crown.nodes[k].r0;
    const f64 tall = crown.axes[k].length;
    const f64 count_f = static_cast<f64>(p.limbs) * size[k] + 0.5;
    u32 limbs = static_cast<u32>(count_f);
    if (limbs < 2) limbs = 2;
    const f64 r_top = crown.nodes[k].r1;
    const f64 r_limb =
        r0 * det::exp(det::log((1.0 - spec.leader_share) / static_cast<f64>(limbs)) * inv);
    for (u32 i = 0; i < limbs; ++i) {
      const Axis& stem = crown.axes[k];
      const f64 f = (static_cast<f64>(i) + 0.5) / static_cast<f64>(limbs);
      const f64 s = stem.length * (base + (0.92 - base) * f);
      const f64 jitter_angle = limb_rng.symmetric();
      const f64 angle = static_cast<f64>(p.limb_angle_deg) * k_degree * (1.0 - 0.5 * f) *
                        (1.0 + 0.1 * jitter_angle);
      const f64 jitter_around = limb_rng.symmetric();
      const f64 around = lturn + static_cast<f64>(i) * k_golden_angle + 0.2 * jitter_around;
      D3 radial;
      const D3 dir = branch_direction(stem, s, around, angle, &radial);
      const f64 jitter_length = limb_rng.unit();
      const f64 length =
          static_cast<f64>(p.limb_length) * tall * ovoid(f) * (0.85 + 0.3 * jitter_length);
      const f64 r_here = lerp(r0, r_top, s / stem.length);
      const Axis limb =
          grow_axis(axis_point(stem, s) + radial * (0.5 * r_here), dir, length, k_fine_rows,
                    D3{0.0, -1.0, 0.0}, droop * 0.25 * (1.0 - f) / length, wander, limb_rng);
      CrownRoot root;
      root.node = crown_add_named(crown, limb, r_limb, crown_leader_radius(spec, r_limb), k);
      root.radius = r_limb;
      root.laterals = p.branches;
      root.lateral_length = 0.4 * length;
      root.from = 0.2;
      root.to = 0.95;
      roots.push_back(root);
    }
  }
  // Every stem's top: a crown of the recursion where its limbs stop.
  for (u32 k = 0; k < stems; ++k) {
    CrownRoot top;
    top.node = k;
    top.radius = crown.nodes[k].r1;
    top.laterals = spec.laterals;
    top.lateral_length = static_cast<f64>(p.limb_length) * crown.axes[k].length * 0.3;
    top.from = 0.9;
    top.to = 1.0;
    top.spread_scale = 0.7;
    roots.push_back(top);
  }
  crown_grow(crown, spec, roots, 2);
  return crown;
}

// ---- the plan ----------------------------------------------------------------------------------

struct Plan {
  u32 sides[2] = {10, 6};  // stems, limbs
  u32 rows[2] = {24, 10};
  f64 card_length = 0.0;  // a card's length as placed (`place_leaves`)
  CrownPlan crown;
};

u32 count_triangles(const Crown& crown, u32 stems, const Plan& plan) {
  return stems * tube_triangles(plan.rows[0], plan.sides[0]) +
         (crown.first_recursion - stems) * tube_triangles(plan.rows[1], plan.sides[1]) +
         crown_triangles(crown, plan.crown);
}

// The smallest card the branchlets hold with the whole leaf area on them: one leaf of the species'
// size, unless they cannot take that many — at most `leaves` a side, and no more than their length
// takes, a card's base along its branchlet being its width over the sine of its angle. One side of
// a branchlet holds c cards of length L where c L <= reach and c <= `leaves`, and the leaf area
// asks c L^2 = need of each side.
f64 finest_card(const Birch& p, const Crown& crown) {
  const f64 leaf = static_cast<f64>(p.leaf_length_m);
  const f64 shape = static_cast<f64>(p.leaf_width);
  const f64 twigs = static_cast<f64>(crown.twigs.size());
  if (twigs == 0.0) return leaf;
  const f64 sine = det::sin(static_cast<f64>(p.leaf_angle_deg) * k_degree);
  const f64 area = static_cast<f64>(p.leaf_area_index) * crown.footprint;
  const f64 reach = 0.8 * static_cast<f64>(p.twig_length_m) * sine / shape;
  const f64 need = area / (2.0 * twigs * shape);
  const f64 by_reach = need / reach;
  const f64 by_cap = std::sqrt(need / static_cast<f64>(p.leaves));
  f64 length = leaf;
  if (by_reach > length) length = by_reach;
  if (by_cap > length) length = by_cap;
  return length;
}

// The leaf count by the leaf area index: one-sided leaf area over the crown's footprint in cards
// of the finest length the branchlets hold — or, where at most `cards` fit the budget, fewer cards
// each standing for more leaves (a short shoot's pair and more), grown to keep the leaf area: the
// crown keeps its fullness while the count falls. Returns a card's length.
f64 place_leaves(const Birch& p, Crown& crown, f64 cards) {
  const f64 shape = static_cast<f64>(p.leaf_width);
  const f64 area = static_cast<f64>(p.leaf_area_index) * crown.footprint;
  f64 length = finest_card(p, crown);
  if (cards < 1.0) cards = 1.0;
  const f64 fitted = std::sqrt(area / (shape * cards));
  if (fitted > length) length = fitted;
  crown_distribute_leaves(crown, area / (shape * length * length), p.leaves);
  crown.card_step = shape * length / det::sin(static_cast<f64>(p.leaf_angle_deg) * k_degree);
  return length;
}

// The ladder (flora.md, "The budget of a crown"). The leaves fill what the wood leaves of the
// budget — two triangles a card — with cards up to twice the finest, and while they would have to
// be larger the wood gives way first: the finest wood, rows and cross-sections, the finer orders'
// wood folded into their parts, the limbs' and the crown's tubes at their coarsest. Then cards up
// to four times the finest, and only past that the cards merged up the hierarchy, halved, and the
// last rows. No rung moves a shoot or drops a part.
bool plan_birch_aggregate(const Birch& p, Crown& crown, u32 stems, Plan& plan, u32& steps) {
  plan = Plan{};
  const u32 stem_rows = static_cast<u32>(static_cast<f64>(p.height_m) / 0.5);
  plan.rows[0] = stem_rows < 6 ? 6 : (stem_rows > 28 ? 28 : stem_rows);
  plan.crown.edge_m = 0.05;
  plan.crown.max_sides = 6;
  plan.crown.row_m = 0.6;
  plan.crown.wood_class = 1;
  const f64 finest = finest_card(p, crown);
  f64 grow = 2.0;  // the most a card may grow past the finest on this stretch of the ladder
  // Fills the leaves into what the wood leaves; true when they fit with cards no larger than `grow`
  // times the finest. With merged cards (past the leaf rungs) the count is the merge's.
  const auto fits = [&] {
    if (plan.crown.merge == 0) {
      crown_distribute_leaves(crown, 0.0, p.leaves);
      const u32 wood = count_triangles(crown, stems, plan);
      const f64 room =
          wood < p.triangle_budget ? static_cast<f64>((p.triangle_budget - wood) / 2u) : 0.0;
      plan.card_length = place_leaves(p, crown, room);
      if (plan.card_length > grow * finest * (1.0 + 1e-9)) return false;
    }
    return count_triangles(crown, stems, plan) <= p.triangle_budget;
  };
  steps = 0;
  if (fits()) return true;
  const auto step = [&](auto&& change) {
    change();
    ++steps;
    return fits();
  };
  if (step([&] { plan.crown.wood_class = 2; })) return true;
  if (step([&] {
        plan.crown.row_m = 1.2;
        plan.rows[1] = 6;
      })) {
    return true;
  }
  if (step([&] {
        plan.crown.edge_m = 0.09;
        plan.crown.max_sides = 5;
        plan.sides[1] = 5;
        plan.sides[0] = 8;
      })) {
    return true;
  }
  while (plan.crown.wood_class < crown.part_class) {
    if (step([&] { ++plan.crown.wood_class; })) return true;
  }
  if (step([&] {
        plan.crown.edge_m = 0.2;
        plan.crown.max_sides = 4;
        plan.crown.row_m = 2.5;
        plan.sides[1] = 4;
        plan.rows[1] = 4;
        plan.rows[0] = plan.rows[0] / 2 < 6 ? 6 : plan.rows[0] / 2;
      })) {
    return true;
  }
  if (step([&] { grow = 4.0; })) return true;
  if (step([&] { plan.sides[0] = 6; })) return true;
  // Past here the cards are the largest the ladder allows, then merged.
  const f64 largest = grow * finest;
  plan.card_length = place_leaves(p, crown,
                                  static_cast<f64>(p.leaf_area_index) * crown.footprint /
                                      (static_cast<f64>(p.leaf_width) * largest * largest));
  while (plan.crown.merge < crown.orders) {
    if (step([&] { ++plan.crown.merge; })) return true;
  }
  for (u32 k = 0; k < 6; ++k) {
    if (step([&] { ++plan.crown.halvings; })) return true;
  }
  return step([&] {
    plan.rows[0] = 3;
    plan.rows[1] = 2;
    plan.sides[1] = 3;
    plan.sides[0] = 5;
    plan.crown.row_m = 8.0;
    plan.crown.max_sides = 3;
  });
}

// Full geometry uses independent leaves on their supporting twigs. A budget that cannot hold
// the requested leaf area uses the established aggregate-card representation instead of making
// the crown bare. Callers requesting close inspection must budget for the physical leaves.
bool plan_birch(const Birch& p, Crown& crown, u32 stems, Plan& plan, u32& steps) {
  plan = Plan{};
  plan.rows[0] = std::clamp(static_cast<u32>(p.height_m / .5f), 6u, 28u);
  plan.crown.edge_m = .05;
  plan.crown.max_sides = 6;
  plan.crown.row_m = .6;
  plan.crown.wood_class = 0;
  plan.card_length = static_cast<f64>(p.leaf_length_m);
  const f64 width = static_cast<f64>(p.leaf_length_m * p.leaf_width);
  crown.card_step = width / det::sin(static_cast<f64>(p.leaf_angle_deg) * k_degree);
  crown_distribute_leaves(crown,
                          static_cast<f64>(p.leaf_area_index) * crown.footprint /
                              (width * static_cast<f64>(p.leaf_length_m)),
                          p.leaves);
  for (const u32 t : crown.twigs) {
    CrownNode& n = crown.nodes[t];
    const u32 capacity = std::max(1u, static_cast<u32>(.8 * n.length / (.8 * width)));
    n.cards = std::min(n.cards, capacity);
  }
  steps = 0;
  if (count_triangles(crown, stems, plan) <= p.triangle_budget) return true;
  crown.leaf_placement = LeafPlacement::spray;
  return plan_birch_aggregate(p, crown, stems, plan, steps);
}
}  // namespace

bool birch_in_range(const Birch& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.Birch.") + field + " is out of range (" + range + ")";
    return false;
  };
  if (!(p.height_m >= 3.0f && p.height_m <= 30.0f)) return bad("height_m", "3 to 30 m");
  if (!(p.stem_radius_m >= 0.03f && p.stem_radius_m <= 0.5f))
    return bad("stem_radius_m", "0.03 to 0.5 m");
  if (!(p.stems <= 5u)) return bad("stems", "0 for the seed's choice, or 1 to 5");
  if (!(p.stem_spread_deg >= 0.0f && p.stem_spread_deg <= 35.0f))
    return bad("stem_spread_deg", "0 to 35 degrees");
  if (!(p.crown_base >= 0.15f && p.crown_base <= 0.8f)) return bad("crown_base", "0.15 to 0.8");
  if (!(p.limbs >= 2u && p.limbs <= 24u)) return bad("limbs", "2 to 24");
  if (!(p.limb_angle_deg >= 15.0f && p.limb_angle_deg <= 80.0f))
    return bad("limb_angle_deg", "15 to 80 degrees");
  if (!(p.limb_length >= 0.08f && p.limb_length <= 0.5f)) return bad("limb_length", "0.08 to 0.5");
  if (!(p.twigs >= 1u && p.twigs <= 16u)) return bad("twigs", "1 to 16");
  if (!(p.twig_length_m >= 0.1f && p.twig_length_m <= 3.0f))
    return bad("twig_length_m", "0.1 to 3 m");
  if (!(p.droop >= 0.0f && p.droop <= 1.0f)) return bad("droop", "0 to 1");
  if (!(p.wander_deg >= 0.0f && p.wander_deg <= 20.0f)) return bad("wander_deg", "0 to 20 degrees");
  if (!(p.leaves >= 1u && p.leaves <= 30u)) return bad("leaves", "1 to 30");
  if (!(p.leaf_length_m >= 0.02f && p.leaf_length_m <= 0.25f))
    return bad("leaf_length_m", "0.02 to 0.25 m");
  if (!(p.leaf_angle_deg >= 10.0f && p.leaf_angle_deg <= 85.0f))
    return bad("leaf_angle_deg", "10 to 85 degrees");
  if (!(p.triangle_budget >= 8000u && p.triangle_budget <= 2000000u))
    return bad("triangle_budget", "8,000 to 2,000,000");
  if (!(p.clump_chance >= 0.0f && p.clump_chance <= 1.0f)) return bad("clump_chance", "0 to 1");
  if (!(p.max_stems >= 2u && p.max_stems <= 5u)) return bad("max_stems", "2 to 5");
  if (!(p.branches >= 1u && p.branches <= 8u)) return bad("branches", "1 to 8");
  if (!(p.twig_radius_m >= 0.001f && p.twig_radius_m <= 0.008f))
    return bad("twig_radius_m", "0.001 to 0.008 m");
  if (!(p.area_exponent >= 1.8f && p.area_exponent <= 3.0f))
    return bad("area_exponent", "1.8 to 3");
  if (!(p.branching_ratio >= 2u && p.branching_ratio <= 6u))
    return bad("branching_ratio", "2 to 6");
  if (!(p.length_ratio >= 0.4f && p.length_ratio <= 0.9f)) return bad("length_ratio", "0.4 to 0.9");
  if (!(p.leader_share >= 0.1f && p.leader_share <= 0.7f)) return bad("leader_share", "0.1 to 0.7");
  if (!(p.leaf_area_index >= 0.5f && p.leaf_area_index <= 8.0f))
    return bad("leaf_area_index", "0.5 to 8");
  if (!(p.leaf_width >= 0.1f && p.leaf_width <= 1.0f)) return bad("leaf_width", "0.1 to 1");
  if (p.card_bake_px != 0u && !(p.card_bake_px >= 16u && p.card_bake_px <= 256u))
    return bad("card_bake_px", "0, or 16 to 256");
  if (p.impostor_px != 0u && !(p.impostor_px >= 32u && p.impostor_px <= 1024u))
    return bad("impostor_px", "0, or 32 to 1024");
  return true;
}

u32 birch_stems(const Birch& p) { return stem_count_of(p); }

bool birch_crown(const Birch& p, Vector<CrownShoot>& out) {
  std::string error;
  if (!birch_in_range(p, error)) return false;
  crown_table(grow(p, spec_of(p)), out);
  return true;
}

bool solve_birch(const Birch& p, assets::MeshData& out, TreeReport* report, std::string* error,
                 Motion* motion) {
  return solve_birch_chain(p, out, report, error, motion, nullptr);
}

bool solve_birch_chain(const Birch& p, assets::MeshData& out, TreeReport* report,
                       std::string* error, Motion* motion, TreeLevels* levels) {
  out = assets::MeshData{};
  std::string message;
  if (!birch_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  const CrownSpec spec = spec_of(p);
  Crown crown = grow(p, spec);
  const u32 stems = stem_count_of(p);
  Plan plan;
  u32 steps = 0;
  if (!plan_birch(p, crown, stems, plan, steps)) {
    if (error != nullptr) {
      *error = "engine.flora.Birch.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this tree: its coarsest form needs " +
               std::to_string(count_triangles(crown, stems, plan)) + " triangles";
    }
    return false;
  }

  // White bark, small bright leaves.
  out.materials.push_back(material("bark", Vec4{0.78f, 0.76f, 0.70f, 1.0f}, 0.75f, false));
  out.materials.push_back(material("leaf", Vec4{0.20f, 0.36f, 0.08f, 1.0f}, 0.6f, true));
  Motion parts;
  Emitter emit{out, parts};
  emit.wood = WoodProfile{0.25, 0.25, 0.035, 0.04, 0.04, p.seed};

  // The bark: every stem (load-bearing: a structure may lean a ladder on one; the first is part 0
  // and the others hang from it at their feet), every limb, then the recursion parents first.
  u32 load_bearing = 0;
  for (u32 i = 0; i < crown.first_recursion; ++i) {
    CrownNode& n = crown.nodes[i];
    const bool stem = i < stems;
    const u32 parent = i == 0 ? geometry::k_motion_no_parent
                              : (stem ? crown.nodes[0].part : crown.nodes[n.parent].part);
    const u32 order = stem ? 0u : 1u;
    n.part = emit_tube_part(emit, crown.axes[n.axis], n.r0, n.r1, plan.rows[order],
                            plan.sides[order], parent, stem ? k_load_bearing : 0u);
    if (stem) ++load_bearing;
  }
  crown_emit_wood(crown, plan.crown, emit, true);
  assets::Primitive bark;
  bark.first_index = 0;
  bark.index_count = out.indices.size();
  bark.material = 0;
  out.primitives.push_back(bark);

  // The leaves: a spray along every branchlet, its plane laid level where it can be.
  const u32 leaf_vertex = out.positions.size();
  const u32 leaf_first = out.indices.size();
  Vector<SprayRange> spray_ranges;
  const f64 card_length = crown_emit_leaves(
      crown, plan.crown, emit, plan.card_length, static_cast<f64>(p.leaf_angle_deg) * k_degree,
      30.0 * k_degree, 0.2, levels != nullptr ? &spray_ranges : nullptr);
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
    f64 hang = 0.0;
    for (const u32 t : crown.twigs) {
      const CrownNode& n = crown.nodes[t];
      if (n.cards > 0u) ++sprays;
      if (n.cards > report->cards_per_side) report->cards_per_side = n.cards;
      const D3 d = normalized(n.tip - n.base);
      hang += d.y;
    }
    report->sprays = sprays;
    report->trunk_sides = plan.sides[0];
    report->shoot_sides = plan.crown.max_sides;
    report->steps = steps;
    report->load_bearing = load_bearing;
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
    report->twig_slope =
        crown.twigs.empty() ? 0.0f : static_cast<f32>(hang / static_cast<f64>(crown.twigs.size()));
    report->stems = stems;
  }
  // The impostor chain above the finest leaves (impostor.h), appended past everything, so the
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

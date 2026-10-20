// The birch's solver (trees.h; docs/subsystems/flora.md, "The birch").
#include "kit.h"

#include <domain/flora/structure.h>
#include <domain/flora/trees.h>

#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

// Independent streams per order, so a draw added to the twigs never moves a stem.
enum class Stream : u64 { stems = 21, limbs = 22, twigs = 23 };

constexpr u32 k_limb_rows = 12;  // the structure's resolution, whatever the budget
constexpr u32 k_twig_rows = 8;

struct Shoot {
  Axis axis;
  f64 r0 = 0.0;
  f64 r1 = 0.0;
  u32 order = 0;   // 0 a stem, 1 a limb, 2 a twig
  u32 parent = 0;  // index into the shoots, parents first
};

Vector<Shoot> grow(const Birch& p) {
  Vector<Shoot> shoots;
  const f64 height = static_cast<f64>(p.height_m);
  const f64 wander = static_cast<f64>(p.wander_deg) * k_degree;
  const f64 total = static_cast<f64>(p.limbs * p.twigs + p.twigs);

  // The stems: the first the tallest and nearly upright, the others leaning out from the same
  // foot on the circle's even turns, a little shorter and thinner.
  Rng stem_rng = stream_of(p.seed, static_cast<u64>(Stream::stems));
  const f64 turn = stem_rng.unit() * 2.0 * k_pi;
  for (u32 k = 0; k < p.stems; ++k) {
    const f64 around = turn + 2.0 * k_pi * static_cast<f64>(k) / static_cast<f64>(p.stems);
    const D3 heading{det::cos(around), 0.0, det::sin(around)};
    const f64 spread =
        k == 0 ? 2.0 * k_degree
               : static_cast<f64>(p.stem_spread_deg) * k_degree * (0.8 + 0.4 * stem_rng.unit());
    const f64 stem_height = k == 0 ? height : height * (0.82 + 0.1 * stem_rng.unit());
    const f64 r0 = static_cast<f64>(p.stem_radius_m) * (k == 0 ? 1.0 : 0.85 - 0.1 * k);
    const D3 base = k == 0 ? D3{} : heading * (0.5 * r0);
    Shoot stem;
    // Straightening towards the light along its length, so a leaning stem bows back up.
    stem.axis = grow_axis(base, k_up * det::cos(spread) + heading * det::sin(spread),
                          stem_height / det::cos(spread), 24, k_up, 0.25 / stem_height,
                          wander * 0.25, stem_rng);
    stem.r0 = r0;
    stem.r1 = pipe_radius_of(r0, 0.5 * static_cast<f64>(p.twigs), total);
    stem.order = 0;
    stem.parent = 0;
    shoots.push_back(std::move(stem));
  }

  // The limbs: up every stem's crown on the golden angle, short and steep — a birch's crown is
  // narrow — the upper ones shorter and a half more upright.
  Rng limb_rng = stream_of(p.seed, static_cast<u64>(Stream::limbs));
  const f64 base_share = static_cast<f64>(p.crown_base);
  for (u32 k = 0; k < p.stems; ++k) {
    const f64 lturn = limb_rng.unit() * 2.0 * k_pi;
    for (u32 i = 0; i < p.limbs; ++i) {
      const Axis& stem = shoots[k].axis;
      const f64 f = (static_cast<f64>(i) + 0.5) / static_cast<f64>(p.limbs);
      const f64 s = stem.length * (base_share + (0.95 - base_share) * f);
      const f64 jitter = limb_rng.symmetric();
      const f64 angle =
          static_cast<f64>(p.limb_angle_deg) * k_degree * (1.0 - 0.5 * f) * (1.0 + 0.1 * jitter);
      const f64 around = lturn + static_cast<f64>(i) * k_golden_angle + 0.2 * limb_rng.symmetric();
      D3 radial;
      const D3 dir = branch_direction(stem, s, around, angle, &radial);
      const f64 length = static_cast<f64>(p.limb_length) * height * (1.0 - 0.6 * f) *
                         (0.85 + 0.3 * limb_rng.unit());
      const f64 r_here = lerp(shoots[k].r0, shoots[k].r1, s / stem.length);
      Shoot limb;
      limb.axis = grow_axis(axis_point(stem, s) + radial * (0.5 * r_here), dir, length, k_limb_rows,
                            k_up, 0.35 * 40.0 * k_degree / length, wander, limb_rng);
      limb.r0 = pipe_radius_of(shoots[k].r0, static_cast<f64>(p.twigs), total);
      limb.r1 = pipe_radius_of(shoots[k].r0, 0.5, total);
      if (limb.r0 > 0.8 * r_here) limb.r0 = 0.8 * r_here;
      if (limb.r1 > limb.r0) limb.r1 = limb.r0;
      limb.order = 1;
      limb.parent = k;
      shoots.push_back(std::move(limb));
    }
  }

  // The twigs: on every limb from a fifth of the way out, and a tuft on every stem's top. They
  // hang: each turns towards the ground along its length, the further out on its limb the more.
  Rng twig_rng = stream_of(p.seed, static_cast<u64>(Stream::twigs));
  const u32 limbs_end = shoots.size();
  const f64 droop = static_cast<f64>(p.droop) * 80.0 * k_degree;
  const auto add_twigs = [&](u32 parent_index, f64 from, f64 to, f64 hang) {
    const f64 tturn = twig_rng.unit() * 2.0 * k_pi;
    for (u32 k = 0; k < p.twigs; ++k) {
      const Axis& parent = shoots[parent_index].axis;
      const f64 f = (static_cast<f64>(k) + 0.5) / static_cast<f64>(p.twigs);
      const f64 s = parent.length * (from + (to - from) * f);
      const f64 around = tturn + static_cast<f64>(k) * k_golden_angle + 0.2 * twig_rng.symmetric();
      const f64 spread = 40.0 * k_degree * (1.0 + 0.15 * twig_rng.symmetric());
      D3 radial;
      const D3 dir = branch_direction(parent, s, around, spread, &radial);
      const f64 length = static_cast<f64>(p.twig_length_m) * (0.85 + 0.3 * twig_rng.unit());
      Shoot twig;
      twig.axis = grow_axis(axis_point(parent, s), dir, length, k_twig_rows, D3{0.0, -1.0, 0.0},
                            droop * hang * (0.4 + 0.6 * f) / length, wander, twig_rng);
      twig.order = 2;
      twig.parent = parent_index;
      shoots.push_back(std::move(twig));
    }
  };
  for (u32 l = p.stems; l < limbs_end; ++l)
    add_twigs(l, 0.2, 1.0, 1.0);
  for (u32 k = 0; k < p.stems; ++k)
    add_twigs(k, 0.9, 1.0, 0.5);
  return shoots;
}

// ---- the plan ----------------------------------------------------------------------------------

struct Plan {
  u32 sides[2] = {10, 5};  // stems, limbs
  u32 rows[2] = {24, 10};
  u32 cards = 0;  // per side of a twig
};

u32 count_triangles(const Birch& p, const Plan& plan) {
  const u32 limbs = p.stems * p.limbs;
  const u32 twigs = p.stems * (p.limbs * p.twigs + p.twigs);
  return p.stems * tube_triangles(plan.rows[0], plan.sides[0]) +
         limbs * tube_triangles(plan.rows[1], plan.sides[1]) + twigs * spray_triangles(plan.cards);
}

// The ladder, the broadleaf's: detail along the shoots, then their cross-sections, then fewer
// and wider leaf cards, then the last rows. No rung drops a part.
bool plan_birch(const Birch& p, Plan& plan, u32& steps) {
  plan = Plan{};
  const u32 stem_rows = static_cast<u32>(static_cast<f64>(p.height_m) / 0.4);
  plan.rows[0] = stem_rows < 6 ? 6 : (stem_rows > 32 ? 32 : stem_rows);
  plan.cards = p.leaves;
  steps = 0;
  const auto fits = [&] { return count_triangles(p, plan) <= p.triangle_budget; };
  if (fits()) return true;
  const auto step = [&](auto&& change) {
    change();
    ++steps;
    return fits();
  };
  if (step([&] { plan.rows[1] = 6; })) return true;
  if (step([&] {
        plan.rows[1] = 4;
        plan.sides[1] = 4;
      })) {
    return true;
  }
  if (step([&] {
        plan.sides[0] = 8;
        plan.sides[1] = 3;
      })) {
    return true;
  }
  if (step([&] { plan.rows[0] = plan.rows[0] / 2 < 4 ? 4 : plan.rows[0] / 2; })) return true;
  while (plan.cards > 1) {
    if (step([&] { plan.cards = (plan.cards + 1) / 2; })) return true;
  }
  if (step([&] {
        plan.rows[1] = 2;
        plan.rows[0] = 3;
      })) {
    return true;
  }
  if (step([&] { plan.sides[0] = 6; })) return true;
  return step([&] { plan.rows[1] = 1; });
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
  if (!(p.stems >= 1u && p.stems <= 3u)) return bad("stems", "1 to 3");
  if (!(p.stem_spread_deg >= 0.0f && p.stem_spread_deg <= 30.0f))
    return bad("stem_spread_deg", "0 to 30 degrees");
  if (!(p.crown_base >= 0.15f && p.crown_base <= 0.8f)) return bad("crown_base", "0.15 to 0.8");
  if (!(p.limbs >= 2u && p.limbs <= 16u)) return bad("limbs", "2 to 16");
  if (!(p.limb_angle_deg >= 15.0f && p.limb_angle_deg <= 80.0f))
    return bad("limb_angle_deg", "15 to 80 degrees");
  if (!(p.limb_length >= 0.08f && p.limb_length <= 0.5f)) return bad("limb_length", "0.08 to 0.5");
  if (!(p.twigs >= 2u && p.twigs <= 16u)) return bad("twigs", "2 to 16");
  if (!(p.twig_length_m >= 0.2f && p.twig_length_m <= 3.0f))
    return bad("twig_length_m", "0.2 to 3 m");
  if (!(p.droop >= 0.0f && p.droop <= 1.0f)) return bad("droop", "0 to 1");
  if (!(p.wander_deg >= 0.0f && p.wander_deg <= 20.0f)) return bad("wander_deg", "0 to 20 degrees");
  if (!(p.leaves >= 1u && p.leaves <= 30u)) return bad("leaves", "1 to 30");
  if (!(p.leaf_length_m >= 0.02f && p.leaf_length_m <= 0.25f))
    return bad("leaf_length_m", "0.02 to 0.25 m");
  if (!(p.leaf_angle_deg >= 10.0f && p.leaf_angle_deg <= 85.0f))
    return bad("leaf_angle_deg", "10 to 85 degrees");
  if (!(p.triangle_budget >= 8000u && p.triangle_budget <= 2000000u))
    return bad("triangle_budget", "8,000 to 2,000,000");
  return true;
}

bool solve_birch(const Birch& p, assets::MeshData& out, TreeReport* report, std::string* error,
                 Motion* motion) {
  out = assets::MeshData{};
  std::string message;
  if (!birch_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  Plan plan;
  u32 steps = 0;
  if (!plan_birch(p, plan, steps)) {
    if (error != nullptr) {
      *error = "engine.flora.Birch.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this tree: its coarsest form needs " +
               std::to_string(count_triangles(p, plan)) + " triangles";
    }
    return false;
  }
  const Vector<Shoot> shoots = grow(p);

  // White bark, small bright leaves.
  out.materials.push_back(material("bark", Vec4{0.78f, 0.76f, 0.70f, 1.0f}, 0.75f, false));
  out.materials.push_back(material("leaf", Vec4{0.20f, 0.36f, 0.08f, 1.0f}, 0.6f, true));
  Motion parts;
  Emitter emit{out, parts};
  Vector<u32> part_of(shoots.size(), 0u);

  // The bark: every stem (load-bearing: a structure may lean a ladder on one) and every limb.
  u32 wood = 0;
  u32 load_bearing = 0;
  for (u32 i = 0; i < shoots.size(); ++i) {
    const Shoot& s = shoots[i];
    if (s.order > 1) continue;
    const u32 parent = i == 0 ? geometry::k_motion_no_parent : part_of[s.parent];
    const u32 flags = s.order == 0 ? k_load_bearing : 0u;
    part_of[i] = emit_tube_part(emit, s.axis, s.r0, s.r1, plan.rows[s.order], plan.sides[s.order],
                                parent, flags);
    load_bearing += flags != 0u ? 1u : 0u;
    ++wood;
  }
  assets::Primitive bark;
  bark.first_index = 0;
  bark.index_count = out.indices.size();
  bark.material = 0;
  out.primitives.push_back(bark);

  const u32 leaf_vertex = out.positions.size();
  const u32 leaf_first = out.indices.size();
  u32 sprays = 0;
  for (u32 i = 0; i < shoots.size(); ++i) {
    const Shoot& s = shoots[i];
    if (s.order != 2) continue;
    part_of[i] = emit_spray_part(
        emit, s.axis, part_of[s.parent], plan.cards, static_cast<f64>(p.leaf_length_m),
        static_cast<f64>(p.leaf_angle_deg) * k_degree, 10.0 * k_degree, 0.25);
    ++sprays;
  }
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
    report->shoots = wood;
    report->sprays = sprays;
    report->cards_per_side = plan.cards;
    report->trunk_sides = plan.sides[0];
    report->shoot_sides = plan.sides[1];
    report->steps = steps;
    report->load_bearing = load_bearing;
  }
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

}  // namespace engine::flora

// The weeping willow's solver (trees.h; docs/subsystems/flora.md, "The willow").
#include "kit.h"

#include <domain/flora/structure.h>
#include <domain/flora/trees.h>

#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

enum class Stream : u64 { trunk = 31, limbs = 32, branches = 33, curtains = 34 };

constexpr u32 k_limb_rows = 16;  // the structure's resolution, whatever the budget
constexpr u32 k_branch_rows = 10;
constexpr u32 k_curtain_rows = 24;
constexpr f64 k_clearance_m = 0.15;  // a curtain stops this far above the ground
constexpr f64 k_hang_per_m = 1.4;    // how fast a curtain turns down, radians a metre (times sine)

struct Shoot {
  Axis axis;
  f64 r0 = 0.0;
  f64 r1 = 0.0;
  u32 order = 0;   // 0 trunk, 1 limb, 2 branch, 3 curtain
  u32 parent = 0;  // index into the shoots, parents first
};

Vector<Shoot> grow(const Willow& p) {
  Vector<Shoot> shoots;
  const f64 height = static_cast<f64>(p.height_m);
  const f64 r_trunk = static_cast<f64>(p.trunk_radius_m);
  const f64 wander = static_cast<f64>(p.wander_deg) * k_degree;
  const f64 total = static_cast<f64>(p.limbs * p.branches * p.curtains);

  // A short thick trunk, nearly upright, that keeps most of its girth to where it divides.
  Rng trunk_rng = stream_of(p.seed, static_cast<u64>(Stream::trunk));
  const f64 azimuth = trunk_rng.unit() * 2.0 * k_pi;
  const D3 heading{det::cos(azimuth), 0.0, det::sin(azimuth)};
  const f64 lean = 4.0 * k_degree;
  Shoot trunk;
  trunk.axis =
      grow_axis(D3{}, k_up * det::cos(lean) + heading * det::sin(lean),
                static_cast<f64>(p.trunk_height) * height, 16, k_up, 0.0, wander * 0.3, trunk_rng);
  trunk.r0 = r_trunk;
  trunk.r1 = 0.75 * r_trunk;
  shoots.push_back(std::move(trunk));

  // The limbs: from the trunk's top, arching out and up — a turn towards the light along their
  // length — no longer than reaches the crown's top.
  Rng limb_rng = stream_of(p.seed, static_cast<u64>(Stream::limbs));
  const f64 lturn = limb_rng.unit() * 2.0 * k_pi;
  const Axis t_axis = shoots[0].axis;  // a copy: the limbs pushed below move the array
  for (u32 i = 0; i < p.limbs; ++i) {
    const f64 f = (static_cast<f64>(i) + 0.5) / static_cast<f64>(p.limbs);
    const f64 s = t_axis.length * (0.72 + 0.28 * f);
    const f64 around = lturn + static_cast<f64>(i) * k_golden_angle + 0.2 * limb_rng.symmetric();
    const f64 angle =
        static_cast<f64>(p.limb_angle_deg) * k_degree * (1.0 + 0.12 * limb_rng.symmetric());
    D3 radial;
    // From vertical rather than from the trunk's tangent: the trunk is nearly upright.
    branch_direction(t_axis, s, around, 0.0, &radial);
    const D3 dir = normalized(k_up * det::cos(angle) + radial * det::sin(angle));
    const D3 base = axis_point(t_axis, s);
    const f64 room = (height - base.y) / (det::cos(angle) > 0.35 ? det::cos(angle) : 0.35);
    f64 length = static_cast<f64>(p.limb_length) * height * (0.9 + 0.2 * limb_rng.unit());
    if (length > room) length = room;
    if (length < std::min(.5, .3 * height)) length = std::min(.5, .3 * height);
    Shoot limb;
    limb.axis = grow_axis(base + radial * (0.4 * shoots[0].r1), dir, length, k_limb_rows, k_up,
                          0.4 * 40.0 * k_degree / length, wander, limb_rng);
    limb.r0 = pipe_radius_of(r_trunk, static_cast<f64>(p.branches * p.curtains), total);
    if (limb.r0 > 0.7 * shoots[0].r1) limb.r0 = 0.7 * shoots[0].r1;
    limb.r1 = pipe_radius_of(r_trunk, static_cast<f64>(p.curtains), total) * 0.6;
    limb.order = 1;
    limb.parent = 0;
    shoots.push_back(std::move(limb));
  }

  // The branches: along each limb's outer half, out at 45 degrees, straight for their first
  // stretch and then arching over towards the ground.
  Rng branch_rng = stream_of(p.seed, static_cast<u64>(Stream::branches));
  const u32 limbs_end = shoots.size();
  for (u32 l = 1; l < limbs_end; ++l) {
    const f64 bturn = branch_rng.unit() * 2.0 * k_pi;
    for (u32 j = 0; j < p.branches; ++j) {
      const Axis& parent = shoots[l].axis;
      const f64 f = (static_cast<f64>(j) + 0.5) / static_cast<f64>(p.branches);
      const f64 s = parent.length * (0.45 + 0.55 * f);
      const f64 around =
          bturn + static_cast<f64>(j) * k_golden_angle + 0.2 * branch_rng.symmetric();
      const f64 spread = 45.0 * k_degree * (1.0 + 0.1 * branch_rng.symmetric());
      D3 radial;
      const D3 dir = branch_direction(parent, s, around, spread, &radial);
      const f64 length = static_cast<f64>(p.branch_length) * parent.length * (1.0 - 0.3 * f) *
                         (0.9 + 0.2 * branch_rng.unit());
      const f64 r_here = lerp(shoots[l].r0, shoots[l].r1, s / parent.length);
      Shoot branch;
      branch.axis = grow_axis_after(axis_point(parent, s) + radial * (0.4 * r_here), dir, length,
                                    k_branch_rows, D3{0.0, -1.0, 0.0}, 70.0 * k_degree / length,
                                    0.35 * length, wander, branch_rng);
      branch.r0 = pipe_radius_of(r_trunk, static_cast<f64>(p.curtains), total);
      if (branch.r0 > 0.8 * r_here) branch.r0 = 0.8 * r_here;
      branch.r1 = 0.3 * branch.r0;
      branch.order = 2;
      branch.parent = l;
      shoots.push_back(std::move(branch));
    }
  }

  // The curtains: along every branch, each leaving it out and down and then hanging — a turn
  // towards the ground that is fastest across the fall and slows as it hangs plumb, the
  // catenary-like droop of a long thin twig — cut where it would reach the ground.
  Rng curtain_rng = stream_of(p.seed, static_cast<u64>(Stream::curtains));
  const u32 branches_end = shoots.size();
  for (u32 b = limbs_end; b < branches_end; ++b) {
    for (u32 k = 0; k < p.curtains; ++k) {
      const Axis& parent = shoots[b].axis;
      const f64 f = (static_cast<f64>(k) + 0.5) / static_cast<f64>(p.curtains);
      const f64 s = parent.length * (0.1 + 0.9 * f);
      const f64 around = curtain_rng.unit() * 2.0 * k_pi;
      D3 radial;
      branch_direction(parent, s, around, 0.5 * k_pi, &radial);
      const D3 along = axis_tangent(parent, s);
      const D3 out = normalized(along + radial * 0.6);
      const D3 dir = normalized(out * det::cos(30.0 * k_degree) - k_up * det::sin(30.0 * k_degree));
      const f64 length = static_cast<f64>(p.curtain_length_m) * (0.7 + 0.6 * curtain_rng.unit());
      Axis axis = grow_axis(axis_point(parent, s), dir, length, k_curtain_rows, D3{0.0, -1.0, 0.0},
                            k_hang_per_m, wander, curtain_rng);
      u32 keep = k_curtain_rows;
      while (keep > 2 && axis.point[keep].y < k_clearance_m)
        --keep;
      if (keep < k_curtain_rows) axis = truncate_axis(axis, keep);
      Shoot curtain;
      curtain.axis = std::move(axis);
      curtain.order = 3;
      curtain.parent = b;
      shoots.push_back(std::move(curtain));
    }
  }
  return shoots;
}

// ---- the plan ----------------------------------------------------------------------------------

struct Plan {
  u32 sides[3] = {14, 8, 5};  // trunk, limbs, branches
  u32 rows[3] = {12, 14, 8};
  u32 cards = 0;  // per side of a curtain
  u32 curtain_rows = 6;
};

u32 count_triangles(const Willow& p, const Plan& plan) {
  const u32 limbs = p.limbs;
  const u32 branches = p.limbs * p.branches;
  const u32 curtains = branches * p.curtains;
  return tube_triangles(plan.rows[0], plan.sides[0]) +
         limbs * tube_triangles(plan.rows[1], plan.sides[1]) +
         branches * tube_triangles(plan.rows[2], plan.sides[2]) +
         curtains * (spray_triangles(plan.cards) +
                     (plan.curtain_rows > 0 ? tube_triangles(plan.curtain_rows, 3) : 0u));
}

// The ladder: the shoots' rows, then their cross-sections, then fewer and longer leaf cards on
// every curtain (a curtain of half the cards keeps its length; its cards are twice as far apart),
// then the last rows. No rung drops a curtain: each is a part a wind stage moves on its own.
bool plan_willow(const Willow& p, Plan& plan, u32& steps) {
  plan = Plan{};
  const u32 trunk_rows =
      static_cast<u32>(static_cast<f64>(p.trunk_height) * static_cast<f64>(p.height_m) / 0.3);
  plan.rows[0] = trunk_rows < 6 ? 6 : (trunk_rows > 24 ? 24 : trunk_rows);
  plan.cards = p.leaves;
  steps = 0;
  const auto fits = [&] { return count_triangles(p, plan) <= p.triangle_budget; };
  if (fits()) return true;
  const auto step = [&](auto&& change) {
    change();
    ++steps;
    return fits();
  };
  if (step([&] {
        plan.rows[2] = 4;
        plan.rows[1] = 8;
      })) {
    return true;
  }
  if (step([&] {
        plan.sides[2] = 3;
        plan.sides[1] = 5;
        plan.sides[0] = 10;
      })) {
    return true;
  }
  if (step([&] { plan.rows[0] = plan.rows[0] / 2 < 4 ? 4 : plan.rows[0] / 2; })) return true;
  if (step([&] { plan.curtain_rows = 1; })) return true;
  while (plan.cards > 1) {
    if (step([&] { plan.cards = (plan.cards + 1) / 2; })) return true;
  }
  if (step([&] { plan.curtain_rows = 0; })) return true;
  if (step([&] {
        plan.rows[2] = 2;
        plan.rows[1] = 4;
        plan.rows[0] = 3;
        plan.sides[1] = 4;
        plan.sides[0] = 6;
      })) {
    return true;
  }
  return step([&] {
    plan.rows[2] = 1;
    plan.rows[1] = 2;
  });
}

}  // namespace

bool willow_in_range(const Willow& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.Willow.") + field + " is out of range (" + range + ")";
    return false;
  };
  if (!(p.height_m >= .35f && p.height_m <= 25.0f)) return bad("height_m", "0.35 to 25 m");
  if (!(p.trunk_radius_m >= .003f && p.trunk_radius_m <= 2.0f))
    return bad("trunk_radius_m", "0.003 to 2 m");
  if (!(p.trunk_height >= 0.15f && p.trunk_height <= 0.6f))
    return bad("trunk_height", "0.15 to 0.6");
  if (!(p.limbs >= 2u && p.limbs <= 12u)) return bad("limbs", "2 to 12");
  if (!(p.limb_angle_deg >= 20.0f && p.limb_angle_deg <= 80.0f))
    return bad("limb_angle_deg", "20 to 80 degrees");
  if (!(p.limb_length >= .15f && p.limb_length <= 1.0f)) return bad("limb_length", "0.15 to 1");
  if (!(p.branches >= 1u && p.branches <= 10u)) return bad("branches", "1 to 10");
  if (!(p.branch_length >= 0.15f && p.branch_length <= 0.8f))
    return bad("branch_length", "0.15 to 0.8");
  if (!(p.curtains >= 1u && p.curtains <= 16u)) return bad("curtains", "1 to 16");
  if (!(p.curtain_length_m >= .1f && p.curtain_length_m <= 15.0f))
    return bad("curtain_length_m", "0.1 to 15 m");
  if (!(p.wander_deg >= 0.0f && p.wander_deg <= 20.0f)) return bad("wander_deg", "0 to 20 degrees");
  if (!(p.leaves >= 2u && p.leaves <= 40u)) return bad("leaves", "2 to 40");
  if (!(p.leaf_length_m >= 0.03f && p.leaf_length_m <= 0.4f))
    return bad("leaf_length_m", "0.03 to 0.4 m");
  if (!(p.leaf_angle_deg >= 5.0f && p.leaf_angle_deg <= 60.0f))
    return bad("leaf_angle_deg", "5 to 60 degrees");
  if (!(p.triangle_budget >= 12000u && p.triangle_budget <= 2000000u))
    return bad("triangle_budget", "12,000 to 2,000,000");
  return true;
}

bool solve_willow(const Willow& p, assets::MeshData& out, TreeReport* report, std::string* error,
                  Motion* motion) {
  out = assets::MeshData{};
  std::string message;
  if (!willow_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  Plan plan;
  u32 steps = 0;
  if (!plan_willow(p, plan, steps)) {
    if (error != nullptr) {
      *error = "engine.flora.Willow.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this tree: its coarsest form needs " +
               std::to_string(count_triangles(p, plan)) + " triangles";
    }
    return false;
  }
  const Vector<Shoot> shoots = grow(p);

  out.materials.push_back(material("bark", Vec4{0.30f, 0.25f, 0.18f, 1.0f}, 0.9f, false));
  out.materials.push_back(material("leaf", Vec4{0.24f, 0.36f, 0.10f, 1.0f}, 0.6f, true));
  Motion parts;
  Emitter emit{out, parts};
  emit.wood = WoodProfile{0.4, 0.4, 0.08, 0.07, 0.08, p.seed};
  Vector<u32> part_of(shoots.size(), 0u);

  u32 wood = 0;
  for (u32 i = 0; i < shoots.size(); ++i) {
    const Shoot& s = shoots[i];
    const u32 parent = i == 0 ? geometry::k_motion_no_parent : part_of[s.parent];
    if (s.order == 3) {
      part_of[i] = begin_structural_part(emit, s.axis.point[0], s.axis.tangent[0], s.axis.length,
                                         parent, 0, 0, 0);
      if (plan.curtain_rows > 0)
        build_tube(resample_axis(s.axis, plan.curtain_rows), .0015, .0004, 3, {}, emit);
      continue;
    }
    part_of[i] = emit_tube_part(
        emit, s.axis, s.r0, s.r1, plan.rows[s.order], plan.sides[s.order], parent,
        s.order == 0 && s.r0 >= static_cast<f64>(load_bearing_min_radius("willow")) ? k_load_bearing
                                                                                    : 0u);
    ++wood;
  }
  assets::Primitive bark;
  bark.first_index = 0;
  bark.index_count = out.indices.size();
  bark.material = 0;
  out.primitives.push_back(bark);

  // The curtains: each a spray hanging from its branch, its cards close along the twig — a
  // willow's narrow leaves — and its plane facing out from the trunk.
  const u32 leaf_vertex = out.positions.size();
  const u32 leaf_first = out.indices.size();
  u32 sprays = 0;
  for (u32 i = 0; i < shoots.size(); ++i) {
    const Shoot& s = shoots[i];
    if (s.order != 3) continue;
    emit.part = part_of[i];
    Rng rng = stream_of(p.seed, 700000u + i);
    build_simple_leaves(s.axis, plan.cards, static_cast<f64>(p.leaf_length_m),
                        static_cast<f64>(p.leaf_length_m) * .16,
                        static_cast<f64>(p.leaf_angle_deg) * k_degree,
                        LeafPlacement::alternate_simple, rng, emit);
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
    report->shoot_sides = plan.sides[2];
    report->steps = steps;
    report->load_bearing = p.trunk_radius_m >= load_bearing_min_radius("willow") ? 1u : 0u;
  }
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

}  // namespace engine::flora

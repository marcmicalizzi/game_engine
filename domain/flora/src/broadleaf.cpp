// The broadleaf's solver (trees.h; docs/subsystems/flora.md, "The broadleaf").
#include "kit.h"

#include <domain/flora/trees.h>
#include <domain/geometry/cluster.h>

#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

// Independent streams per order, so a draw added to the twigs never moves a limb.
enum class Stream : u64 { trunk = 11, limbs = 12, branches = 13, twigs = 14 };

constexpr f64 k_pipe_exponent = 2.5;  // da Vinci's rule, a parent's area the sum of its children's
constexpr u32 k_fine_rows = 16;       // the structure's resolution, whatever the budget

// One woody shoot or twig of the structure: its axis at the structure's resolution, its radii and
// its parent part. The structure is grown once and never depends on the budget.
struct Shoot {
  Axis axis;
  f64 r0 = 0.0;
  f64 r1 = 0.0;
  u32 order = 0;   // 0 trunk, 1 limb, 2 branch, 3 twig
  u32 parent = 0;  // index into the shoots, parents first
};

f64 pipe_radius(f64 trunk_radius, f64 tips, f64 total) {
  return trunk_radius * det::exp(det::log(tips / total) / k_pipe_exponent);
}

// A child's direction: `angle` from the parent's tangent, towards `azimuth` round it.
D3 child_direction(const Axis& parent, f64 arc, f64 azimuth, f64 angle, D3* radial_out) {
  const u32 rows = parent.point.size() - 1;
  const f64 f = arc / parent.length * static_cast<f64>(rows);
  u32 i = static_cast<u32>(f);
  if (i > rows) i = rows;
  const D3 t = axis_tangent(parent, arc);
  const D3 n = perpendicular(t, parent.normal[i]);
  const D3 b = cross(t, n);
  const D3 radial = n * det::cos(azimuth) + b * det::sin(azimuth);
  if (radial_out != nullptr) *radial_out = radial;
  return normalized(t * det::cos(angle) + radial * det::sin(angle));
}

Vector<Shoot> grow(const Broadleaf& p) {
  Vector<Shoot> shoots;
  const f64 height = static_cast<f64>(p.height_m);
  const f64 r_trunk = static_cast<f64>(p.trunk_radius_m);
  const f64 wander = static_cast<f64>(p.wander_deg) * k_degree;
  const f64 upturn = static_cast<f64>(p.upturn) * 40.0 * k_degree;
  const f64 total = static_cast<f64>(p.limbs * p.branches * p.twigs + p.twigs);

  Rng trunk_rng = stream_of(p.seed, static_cast<u64>(Stream::trunk));
  const f64 lean = static_cast<f64>(p.lean_deg) * k_degree;
  const f64 azimuth = trunk_rng.unit() * 2.0 * k_pi;
  const D3 heading{det::cos(azimuth), 0.0, det::sin(azimuth)};
  Shoot trunk;
  trunk.axis =
      grow_axis(D3{}, k_up * det::cos(lean) + heading * det::sin(lean), height / det::cos(lean),
                k_fine_rows * 2, k_up, 0.3 / height, wander * 0.25, trunk_rng);
  trunk.r0 = r_trunk;
  trunk.r1 = pipe_radius(r_trunk, static_cast<f64>(p.twigs), total);
  shoots.push_back(std::move(trunk));

  // The limbs, up the crown on the golden angle: the lowest longest and widest, the upper ones
  // shorter and a third more upright.
  Rng limb_rng = stream_of(p.seed, static_cast<u64>(Stream::limbs));
  const f64 turn = limb_rng.unit() * 2.0 * k_pi;
  const f64 base = static_cast<f64>(p.crown_base);
  for (u32 i = 0; i < p.limbs; ++i) {
    const f64 f = (static_cast<f64>(i) + 0.5) / static_cast<f64>(p.limbs);
    const Axis& t_axis = shoots[0].axis;
    const f64 s = t_axis.length * (base + (0.9 - base) * f);
    const f64 angle = static_cast<f64>(p.limb_angle_deg) * k_degree * (1.0 - f / 3.0) *
                      (1.0 + 0.1 * limb_rng.symmetric());
    D3 radial;
    const D3 dir = child_direction(
        t_axis, s, turn + static_cast<f64>(i) * k_golden_angle + 0.15 * limb_rng.symmetric(), angle,
        &radial);
    const f64 length = static_cast<f64>(p.limb_length) * height * (1.0 - 0.5 * f) *
                       (1.0 + 0.1 * limb_rng.symmetric());
    const f64 r0 = pipe_radius(r_trunk, static_cast<f64>(p.branches * p.twigs), total);
    const f64 r_here = lerp(shoots[0].r0, shoots[0].r1, s / t_axis.length);
    Shoot limb;
    limb.axis = grow_axis(axis_point(t_axis, s) + radial * (0.5 * r_here), dir, length, k_fine_rows,
                          k_up, upturn / length, wander, limb_rng);
    limb.r0 = r0;
    limb.r1 = pipe_radius(r_trunk, static_cast<f64>(p.twigs), total);
    limb.order = 1;
    limb.parent = 0;
    shoots.push_back(std::move(limb));
  }

  // The branches on every limb, alternating round it on the golden angle.
  Rng branch_rng = stream_of(p.seed, static_cast<u64>(Stream::branches));
  const u32 limbs_end = shoots.size();
  for (u32 l = 1; l < limbs_end; ++l) {
    const f64 bturn = branch_rng.unit() * 2.0 * k_pi;
    for (u32 j = 0; j < p.branches; ++j) {
      const Axis& parent = shoots[l].axis;
      const f64 f = (static_cast<f64>(j) + 0.5) / static_cast<f64>(p.branches);
      const f64 s = parent.length * (0.25 + 0.7 * f);
      D3 radial;
      const D3 dir = child_direction(
          parent, s, bturn + static_cast<f64>(j) * k_golden_angle + 0.2 * branch_rng.symmetric(),
          static_cast<f64>(p.branch_angle_deg) * k_degree * (1.0 + 0.1 * branch_rng.symmetric()),
          &radial);
      const f64 length = static_cast<f64>(p.branch_length) * parent.length * (1.0 - 0.4 * f) *
                         (1.0 + 0.1 * branch_rng.symmetric());
      const f64 r_here = lerp(shoots[l].r0, shoots[l].r1, s / parent.length);
      Shoot branch;
      branch.axis = grow_axis(axis_point(parent, s) + radial * (0.5 * r_here), dir, length,
                              k_fine_rows / 2, k_up, upturn / length, wander, branch_rng);
      branch.r0 = pipe_radius(r_trunk, static_cast<f64>(p.twigs), total);
      branch.r1 = pipe_radius(r_trunk, 0.35, total);
      branch.order = 2;
      branch.parent = l;
      shoots.push_back(std::move(branch));
    }
  }

  // The twigs: on every branch, and a tuft of them on the leader's top.
  Rng twig_rng = stream_of(p.seed, static_cast<u64>(Stream::twigs));
  const u32 branches_end = shoots.size();
  const auto add_twigs = [&](u32 parent_index, f64 from, f64 to) {
    const f64 tturn = twig_rng.unit() * 2.0 * k_pi;
    for (u32 k = 0; k < p.twigs; ++k) {
      const Axis& parent = shoots[parent_index].axis;
      const f64 f = (static_cast<f64>(k) + 0.5) / static_cast<f64>(p.twigs);
      const f64 s = parent.length * (from + (to - from) * f);
      D3 radial;
      const D3 dir = child_direction(
          parent, s, tturn + static_cast<f64>(k) * k_golden_angle + 0.2 * twig_rng.symmetric(),
          static_cast<f64>(p.branch_angle_deg) * k_degree * (1.0 + 0.15 * twig_rng.symmetric()),
          &radial);
      const f64 length = static_cast<f64>(p.twig_length_m) * (0.85 + 0.3 * twig_rng.unit());
      Shoot twig;
      twig.axis =
          grow_axis(axis_point(parent, s), dir, length, 8, k_up, upturn / length, wander, twig_rng);
      twig.order = 3;
      twig.parent = parent_index;
      shoots.push_back(std::move(twig));
    }
  };
  for (u32 b = limbs_end; b < branches_end; ++b)
    add_twigs(b, 0.2, 1.0);
  add_twigs(0, 0.85, 1.0);
  return shoots;
}

// ---- the plan ----------------------------------------------------------------------------------

struct Plan {
  u32 sides[3] = {12, 8, 5};  // trunk, limbs, branches
  u32 rows[3] = {24, 14, 7};
  u32 cards = 0;  // per side of a twig
};

u32 count_triangles(const Broadleaf& p, const Plan& plan) {
  const u32 limbs = p.limbs;
  const u32 branches = p.limbs * p.branches;
  const u32 twigs = branches * p.twigs + p.twigs;
  return tube_triangles(plan.rows[0], plan.sides[0]) +
         limbs * tube_triangles(plan.rows[1], plan.sides[1]) +
         branches * tube_triangles(plan.rows[2], plan.sides[2]) +
         twigs * spray_triangles(plan.cards);
}

// The ladder: detail along the shoots first, then their cross-sections, then fewer and wider leaf
// cards (a twig of half the cards keeps its leaf area), then the last of the shoots' rows and
// sides. No rung drops a shoot or a twig: the motion parts are the same at every budget.
bool plan_broadleaf(const Broadleaf& p, Plan& plan, u32& steps) {
  plan = Plan{};
  const f64 height = static_cast<f64>(p.height_m);
  u32 trunk_rows = static_cast<u32>(height / 0.4);
  plan.rows[0] = trunk_rows < 6 ? 6 : (trunk_rows > 32 ? 32 : trunk_rows);
  plan.cards = p.leaves;
  steps = 0;
  const auto fits = [&] { return count_triangles(p, plan) <= p.triangle_budget; };
  if (fits()) return true;
  const auto step = [&](auto&& change) {
    change();
    ++steps;
    return fits();
  };
  if (step([&] { plan.rows[2] = 4; })) return true;
  if (step([&] { plan.rows[1] = 8; })) return true;
  if (step([&] {
        plan.sides[2] = 4;
        plan.sides[1] = 6;
        plan.sides[0] = 10;
      })) {
    return true;
  }
  if (step([&] {
        plan.sides[2] = 3;
        plan.sides[1] = 5;
        plan.sides[0] = 8;
      })) {
    return true;
  }
  if (step([&] { plan.rows[0] = plan.rows[0] / 2 < 4 ? 4 : plan.rows[0] / 2; })) return true;
  while (plan.cards > 1) {
    if (step([&] { plan.cards = (plan.cards + 1) / 2; })) return true;
  }
  if (step([&] {
        plan.rows[2] = 2;
        plan.rows[1] = 4;
        plan.rows[0] = 3;
      })) {
    return true;
  }
  if (step([&] {
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

bool broadleaf_in_range(const Broadleaf& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.Broadleaf.") + field + " is out of range (" + range + ")";
    return false;
  };
  if (!(p.height_m >= 3.0f && p.height_m <= 40.0f)) return bad("height_m", "3 to 40 m");
  if (!(p.trunk_radius_m >= 0.05f && p.trunk_radius_m <= 2.0f))
    return bad("trunk_radius_m", "0.05 to 2 m");
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
  return true;
}

bool solve_broadleaf(const Broadleaf& p, assets::MeshData& out, TreeReport* report,
                     std::string* error, Motion* motion) {
  out = assets::MeshData{};
  std::string message;
  if (!broadleaf_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  Plan plan;
  u32 steps = 0;
  if (!plan_broadleaf(p, plan, steps)) {
    if (error != nullptr) {
      *error = "engine.flora.Broadleaf.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this tree: its coarsest form needs " +
               std::to_string(count_triangles(p, plan)) + " triangles";
    }
    return false;
  }
  const Vector<Shoot> shoots = grow(p);

  out.materials.push_back(material("bark", Vec4{0.30f, 0.22f, 0.15f, 1.0f}, 0.9f, false));
  out.materials.push_back(material("leaf", Vec4{0.10f, 0.24f, 0.05f, 1.0f}, 0.6f, true));
  Motion parts;
  Emitter emit{out, parts};
  Vector<u32> part_of(shoots.size(), 0u);

  // The bark: the trunk, the limbs and the branches, parents first, each a part and a tube. Each
  // order's tubes take their UVs from one origin: a tiled bark repeats on all of them alike.
  u32 wood_shoots = 0;
  for (u32 i = 0; i < shoots.size(); ++i) {
    const Shoot& s = shoots[i];
    if (s.order > 2) continue;
    part_of[i] = emit.begin_part(s.axis.point[0], s.axis.tangent[0], s.axis.length,
                                 s.order == 0 ? geometry::k_motion_no_parent : part_of[s.parent]);
    const Axis mesh_axis = resample_axis(s.axis, plan.rows[s.order]);
    build_tube(mesh_axis, s.r0, s.r1, plan.sides[s.order], Vec2{0.0f, 0.0f}, emit);
    ++wood_shoots;
  }
  assets::Primitive bark;
  bark.first_index = 0;
  bark.index_count = out.indices.size();
  bark.material = 0;
  out.primitives.push_back(bark);

  // The leaves: one spray a twig, its plane laid level so the cards face the sky.
  const u32 leaf_vertex = out.positions.size();
  const u32 leaf_first = out.indices.size();
  u32 sprays = 0;
  for (u32 i = 0; i < shoots.size(); ++i) {
    const Shoot& s = shoots[i];
    if (s.order != 3) continue;
    part_of[i] =
        emit.begin_part(s.axis.point[0], s.axis.tangent[0], s.axis.length, part_of[s.parent]);
    D3 side = cross(s.axis.tangent[0], k_up);
    if (dot(side, side) < 1e-6) side = D3{1.0, 0.0, 0.0};
    build_spray(s.axis, 0.0, plan.cards, static_cast<f64>(p.leaf_length_m),
                static_cast<f64>(p.leaf_angle_deg) * k_degree, 15.0 * k_degree, 0.15,
                normalized(side), emit);
    ++sprays;
  }
  assets::Primitive leaves;
  leaves.first_index = leaf_first;
  leaves.index_count = out.indices.size() - leaf_first;
  leaves.material = 1;
  out.primitives.push_back(leaves);
  {
    Vector<Vec3> normals;
    const std::span<const u32> range(out.indices.data() + leaf_first, leaves.index_count);
    geometry::compute_vertex_normals(out.positions, range, normals);
    for (u32 v = leaf_vertex; v < out.positions.size(); ++v)
      out.normals[v] = normals[v];
  }
  if (report != nullptr) {
    *report = TreeReport{};
    report->triangles = out.indices.size() / 3;
    report->wood_triangles = bark.index_count / 3;
    report->leaf_triangles = leaves.index_count / 3;
    report->vertices = out.positions.size();
    report->parts = parts.parts.size();
    report->shoots = wood_shoots;
    report->sprays = sprays;
    report->cards_per_side = plan.cards;
    report->trunk_sides = plan.sides[0];
    report->shoot_sides = plan.sides[2];
    report->steps = steps;
  }
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

}  // namespace engine::flora

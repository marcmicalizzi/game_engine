// The orchard tree's solver (trees.h; docs/subsystems/flora.md, "The orchard tree").
#include "kit.h"

#include <domain/flora/structure.h>
#include <domain/flora/trees.h>

#include <string>
#include <utility>

namespace engine::flora {

namespace {

using namespace detail;

enum class Stream : u64 { trunk = 41, limbs = 42, branches = 43, twigs = 44 };

constexpr u32 k_limb_rows = 12;  // the structure's resolution, whatever the budget
constexpr u32 k_branch_rows = 8;
constexpr u32 k_twig_rows = 6;
constexpr u32 k_fruit_triangles = 8;  // an octahedron: round enough at a fruit's size

struct Shoot {
  Axis axis;
  f64 r0 = 0.0;
  f64 r1 = 0.0;
  u32 order = 0;   // 0 trunk, 1 scaffold limb, 2 branch, 3 twig
  u32 parent = 0;  // index into the shoots, parents first
};

Vector<Shoot> grow(const Orchard& p) {
  Vector<Shoot> shoots;
  const f64 height = static_cast<f64>(p.height_m);
  const f64 r_trunk = static_cast<f64>(p.trunk_radius_m);
  const f64 wander = static_cast<f64>(p.wander_deg) * k_degree;
  const f64 total = static_cast<f64>(p.limbs * p.branches * p.twigs);

  Rng trunk_rng = stream_of(p.seed, static_cast<u64>(Stream::trunk));
  const f64 azimuth = trunk_rng.unit() * 2.0 * k_pi;
  const D3 heading{det::cos(azimuth), 0.0, det::sin(azimuth)};
  const f64 lean = 3.0 * k_degree;
  Shoot trunk;
  trunk.axis =
      grow_axis(D3{}, k_up * det::cos(lean) + heading * det::sin(lean),
                static_cast<f64>(p.trunk_height) * height, 12, k_up, 0.0, wander * 0.3, trunk_rng);
  trunk.r0 = r_trunk;
  trunk.r1 = 0.8 * r_trunk;
  shoots.push_back(std::move(trunk));

  // The scaffold limbs: an open vase from the trunk's top, evenly round it. Pruned, they run
  // straight and stop under the crown's top; free, they turn up towards the light.
  Rng limb_rng = stream_of(p.seed, static_cast<u64>(Stream::limbs));
  const f64 lturn = limb_rng.unit() * 2.0 * k_pi;
  const Axis t_axis = shoots[0].axis;  // a copy: the limbs pushed below move the array
  const f64 top = height * 0.95;
  for (u32 i = 0; i < p.limbs; ++i) {
    const f64 f = (static_cast<f64>(i) + 0.5) / static_cast<f64>(p.limbs);
    const f64 s = t_axis.length * (0.8 + 0.2 * f);
    const f64 around = lturn + 2.0 * k_pi * static_cast<f64>(i) / static_cast<f64>(p.limbs) +
                       0.25 * limb_rng.symmetric();
    const f64 angle =
        static_cast<f64>(p.limb_angle_deg) * k_degree * (1.0 + 0.1 * limb_rng.symmetric());
    D3 radial;
    branch_direction(t_axis, s, around, 0.0, &radial);
    const D3 dir = normalized(k_up * det::cos(angle) + radial * det::sin(angle));
    const D3 base = axis_point(t_axis, s);
    f64 length = static_cast<f64>(p.limb_length) * height * (0.9 + 0.2 * limb_rng.unit());
    const f64 rise = det::cos(angle) > 0.2 ? det::cos(angle) : 0.2;
    if (p.pruned && base.y + length * rise > top) length = (top - base.y) / rise;
    if (length < 0.3) length = 0.3;
    Shoot limb;
    limb.axis =
        grow_axis(base + radial * (0.4 * shoots[0].r1), dir, length, k_limb_rows, k_up,
                  (p.pruned ? 0.0 : 0.6 * 40.0 * k_degree) / length, wander * 0.7, limb_rng);
    limb.r0 = pipe_radius_of(r_trunk, static_cast<f64>(p.branches * p.twigs), total);
    if (limb.r0 > 0.75 * shoots[0].r1) limb.r0 = 0.75 * shoots[0].r1;
    limb.r1 = pipe_radius_of(r_trunk, static_cast<f64>(p.twigs), total) * 0.7;
    limb.order = 1;
    limb.parent = 0;
    shoots.push_back(std::move(limb));
  }

  // The branches: along every limb, out at 40 degrees. Pruned, each turns level — out from the
  // trunk — so the crown's top is flat; free, each turns up.
  Rng branch_rng = stream_of(p.seed, static_cast<u64>(Stream::branches));
  const u32 limbs_end = shoots.size();
  for (u32 l = 1; l < limbs_end; ++l) {
    const f64 bturn = branch_rng.unit() * 2.0 * k_pi;
    for (u32 j = 0; j < p.branches; ++j) {
      const Axis& parent = shoots[l].axis;
      const f64 f = (static_cast<f64>(j) + 0.5) / static_cast<f64>(p.branches);
      const f64 s = parent.length * (0.3 + 0.7 * f);
      const f64 around =
          bturn + static_cast<f64>(j) * k_golden_angle + 0.2 * branch_rng.symmetric();
      const f64 spread = 40.0 * k_degree * (1.0 + 0.12 * branch_rng.symmetric());
      D3 radial;
      const D3 dir = branch_direction(parent, s, around, spread, &radial);
      const f64 length = 0.55 * parent.length * (1.0 - 0.35 * f) * (0.9 + 0.2 * branch_rng.unit());
      const D3 at = axis_point(parent, s);
      D3 level{at.x, 0.0, at.z};
      level = dot(level, level) > 1e-6 ? normalized(level) : D3{1.0, 0.0, 0.0};
      const f64 r_here = lerp(shoots[l].r0, shoots[l].r1, s / parent.length);
      Shoot branch;
      branch.axis =
          grow_axis(at + radial * (0.4 * r_here), dir, length, k_branch_rows,
                    p.pruned ? level : k_up, 50.0 * k_degree / length, wander, branch_rng);
      branch.r0 = pipe_radius_of(r_trunk, static_cast<f64>(p.twigs), total);
      if (branch.r0 > 0.8 * r_here) branch.r0 = 0.8 * r_here;
      branch.r1 = 0.35 * branch.r0;
      branch.order = 2;
      branch.parent = l;
      shoots.push_back(std::move(branch));
    }
  }

  // The twigs: short leafy spurs along every branch, a little upturned.
  Rng twig_rng = stream_of(p.seed, static_cast<u64>(Stream::twigs));
  const u32 branches_end = shoots.size();
  for (u32 b = limbs_end; b < branches_end; ++b) {
    const f64 tturn = twig_rng.unit() * 2.0 * k_pi;
    for (u32 k = 0; k < p.twigs; ++k) {
      const Axis& parent = shoots[b].axis;
      const f64 f = (static_cast<f64>(k) + 0.5) / static_cast<f64>(p.twigs);
      const f64 s = parent.length * (0.15 + 0.85 * f);
      const f64 around = tturn + static_cast<f64>(k) * k_golden_angle + 0.2 * twig_rng.symmetric();
      const f64 spread = 50.0 * k_degree * (1.0 + 0.15 * twig_rng.symmetric());
      D3 radial;
      const D3 dir = branch_direction(parent, s, around, spread, &radial);
      const f64 length = static_cast<f64>(p.twig_length_m) * (0.8 + 0.4 * twig_rng.unit());
      Shoot twig;
      twig.axis = grow_axis(axis_point(parent, s), dir, length, k_twig_rows, k_up,
                            0.3 * 40.0 * k_degree / length, wander, twig_rng);
      twig.order = 3;
      twig.parent = b;
      shoots.push_back(std::move(twig));
    }
  }
  return shoots;
}

// ---- the plan ----------------------------------------------------------------------------------

struct Plan {
  u32 sides[3] = {12, 7, 5};  // trunk, limbs, branches
  u32 rows[3] = {8, 12, 7};
  u32 cards = 0;  // per side of a twig
};

u32 count_triangles(const Orchard& p, const Plan& plan) {
  const u32 limbs = p.limbs;
  const u32 branches = p.limbs * p.branches;
  const u32 twigs = branches * p.twigs;
  return tube_triangles(plan.rows[0], plan.sides[0]) +
         limbs * tube_triangles(plan.rows[1], plan.sides[1]) +
         branches * tube_triangles(plan.rows[2], plan.sides[2]) +
         twigs * (spray_triangles(plan.cards) + p.fruit * k_fruit_triangles);
}

// The broadleaf's ladder. The fruit is never dropped or coarsened: a tree whose fruit the budget
// cannot hold is refused with the count, rather than drawn as a different tree.
bool plan_orchard(const Orchard& p, Plan& plan, u32& steps) {
  plan = Plan{};
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
        plan.rows[1] = 7;
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
  while (plan.cards > 1) {
    if (step([&] { plan.cards = (plan.cards + 1) / 2; })) return true;
  }
  if (step([&] {
        plan.rows[2] = 2;
        plan.rows[1] = 3;
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

bool orchard_in_range(const Orchard& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.Orchard.") + field + " is out of range (" + range + ")";
    return false;
  };
  const auto unit = [](f32 v) { return v >= 0.0f && v <= 1.0f; };
  if (!(p.height_m >= 2.0f && p.height_m <= 12.0f)) return bad("height_m", "2 to 12 m");
  if (!(p.trunk_radius_m >= 0.04f && p.trunk_radius_m <= 0.6f))
    return bad("trunk_radius_m", "0.04 to 0.6 m");
  if (!(p.trunk_height >= 0.12f && p.trunk_height <= 0.5f))
    return bad("trunk_height", "0.12 to 0.5");
  if (!(p.limbs >= 2u && p.limbs <= 8u)) return bad("limbs", "2 to 8");
  if (!(p.limb_angle_deg >= 25.0f && p.limb_angle_deg <= 80.0f))
    return bad("limb_angle_deg", "25 to 80 degrees");
  if (!(p.limb_length >= 0.3f && p.limb_length <= 1.2f)) return bad("limb_length", "0.3 to 1.2");
  if (!(p.branches >= 1u && p.branches <= 8u)) return bad("branches", "1 to 8");
  if (!(p.twigs >= 1u && p.twigs <= 12u)) return bad("twigs", "1 to 12");
  if (!(p.twig_length_m >= 0.1f && p.twig_length_m <= 1.5f))
    return bad("twig_length_m", "0.1 to 1.5 m");
  if (!(p.wander_deg >= 0.0f && p.wander_deg <= 20.0f)) return bad("wander_deg", "0 to 20 degrees");
  if (!(p.leaves >= 1u && p.leaves <= 30u)) return bad("leaves", "1 to 30");
  if (!(p.leaf_length_m >= 0.03f && p.leaf_length_m <= 0.25f))
    return bad("leaf_length_m", "0.03 to 0.25 m");
  if (!(p.leaf_angle_deg >= 10.0f && p.leaf_angle_deg <= 85.0f))
    return bad("leaf_angle_deg", "10 to 85 degrees");
  if (!(p.fruit <= 6u)) return bad("fruit", "0 to 6");
  if (!(p.fruit_radius_m >= 0.02f && p.fruit_radius_m <= 0.08f))
    return bad("fruit_radius_m", "0.02 to 0.08 m");
  if (!unit(p.fruit_red)) return bad("fruit_red", "0 to 1");
  if (!unit(p.fruit_green)) return bad("fruit_green", "0 to 1");
  if (!unit(p.fruit_blue)) return bad("fruit_blue", "0 to 1");
  if (!(p.triangle_budget >= 8000u && p.triangle_budget <= 2000000u))
    return bad("triangle_budget", "8,000 to 2,000,000");
  return true;
}

bool solve_orchard(const Orchard& p, assets::MeshData& out, TreeReport* report, std::string* error,
                   Motion* motion) {
  out = assets::MeshData{};
  std::string message;
  if (!orchard_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  Plan plan;
  u32 steps = 0;
  if (!plan_orchard(p, plan, steps)) {
    if (error != nullptr) {
      *error = "engine.flora.Orchard.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this tree: its coarsest form needs " +
               std::to_string(count_triangles(p, plan)) + " triangles";
    }
    return false;
  }
  const Vector<Shoot> shoots = grow(p);

  out.materials.push_back(material("bark", Vec4{0.33f, 0.26f, 0.19f, 1.0f}, 0.85f, false));
  out.materials.push_back(material("leaf", Vec4{0.14f, 0.30f, 0.06f, 1.0f}, 0.55f, true));
  if (p.fruit > 0) {
    out.materials.push_back(
        material("fruit", Vec4{p.fruit_red, p.fruit_green, p.fruit_blue, 1.0f}, 0.45f, false));
  }
  Motion parts;
  Emitter emit{out, parts};
  emit.wood = WoodProfile{0.4, 0.35, 0.08, 0.06, 0.1, p.seed};
  Vector<u32> part_of(shoots.size(), 0u);

  u32 wood = 0;
  for (u32 i = 0; i < shoots.size(); ++i) {
    const Shoot& s = shoots[i];
    if (s.order > 2) continue;
    const u32 parent = i == 0 ? geometry::k_motion_no_parent : part_of[s.parent];
    part_of[i] = emit_tube_part(emit, s.axis, s.r0, s.r1, plan.rows[s.order], plan.sides[s.order],
                                parent, s.order == 0 ? k_load_bearing : 0u);
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
    if (s.order != 3) continue;
    part_of[i] = emit_spray_part(
        emit, s.axis, part_of[s.parent], plan.cards, static_cast<f64>(p.leaf_length_m),
        static_cast<f64>(p.leaf_angle_deg) * k_degree, 15.0 * k_degree, 0.15);
    ++sprays;
  }
  assets::Primitive leaves;
  leaves.first_index = leaf_first;
  leaves.index_count = out.indices.size() - leaf_first;
  leaves.material = 1;
  out.primitives.push_back(leaves);
  smooth_normals(out, leaf_vertex, leaf_first);

  // The fruit: on every twig, at even stations along it, each hanging on a short stalk below the
  // twig — an octahedron, one-sided and closed, in its twig's part with its station's weight.
  u32 fruit_first = out.indices.size();
  if (p.fruit > 0) {
    const f64 radius = static_cast<f64>(p.fruit_radius_m);
    const D3 axes[6] = {{1.0, 0.0, 0.0},  {-1.0, 0.0, 0.0}, {0.0, 1.0, 0.0},
                        {0.0, -1.0, 0.0}, {0.0, 0.0, 1.0},  {0.0, 0.0, -1.0}};
    const u32 faces[8][3] = {{0, 2, 4}, {4, 2, 1}, {1, 2, 5}, {5, 2, 0},
                             {4, 3, 0}, {1, 3, 4}, {5, 3, 1}, {0, 3, 5}};
    for (u32 i = 0; i < shoots.size(); ++i) {
      const Shoot& s = shoots[i];
      if (s.order != 3) continue;
      emit.part = part_of[i];
      for (u32 j = 0; j < p.fruit; ++j) {
        const f64 at = s.axis.length * (static_cast<f64>(j) + 0.5) / static_cast<f64>(p.fruit);
        emit.weight = at / s.axis.length;
        const D3 centre = axis_point(s.axis, at) - k_up * (0.02 + radius);
        u32 v[6];
        for (u32 k = 0; k < 6; ++k) {
          v[k] = emit.vertex(centre + axes[k] * radius, axes[k], 0.5 + 0.5 * axes[k].x,
                             0.5 + 0.5 * axes[k].z);
        }
        for (const auto& f : faces) {
          const D3 mid = (axes[f[0]] + axes[f[1]] + axes[f[2]]) * (1.0 / 3.0);
          emit.face(v[f[0]], v[f[1]], v[f[2]], mid);
        }
      }
    }
    assets::Primitive fruit;
    fruit.first_index = fruit_first;
    fruit.index_count = out.indices.size() - fruit_first;
    fruit.material = 2;
    out.primitives.push_back(fruit);
  }

  if (report != nullptr) {
    *report = TreeReport{};
    report->triangles = out.indices.size() / 3;
    report->wood_triangles = bark.index_count / 3;
    report->leaf_triangles = leaves.index_count / 3;
    report->fruit_triangles = (out.indices.size() - fruit_first) / 3;
    report->vertices = out.positions.size();
    report->parts = parts.parts.size();
    report->shoots = wood;
    report->sprays = sprays;
    report->cards_per_side = plan.cards;
    report->trunk_sides = plan.sides[0];
    report->shoot_sides = plan.sides[2];
    report->steps = steps;
    report->load_bearing = 1;
  }
  if (motion != nullptr) *motion = std::move(parts);
  return true;
}

}  // namespace engine::flora

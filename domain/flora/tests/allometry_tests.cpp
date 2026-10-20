// The allometric crown (roadmap G72; docs/subsystems/flora.md, "Allometry";
// docs/experiments/tree-allometry.md): the broadleaf's and the ancient tree's crowns are a
// recursion of self-similar orders down to a terminal twig of the species' own size, their radii
// by the pipe rule, their leaf count by the leaf area index, and a budget that changes a tree's
// detail and never its structure.
#include <domain/flora/flora.h>
#include <domain/flora/structure.h>
#include <domain/flora/trees.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace engine;

namespace {

struct Solved {
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
};

template <typename Params, typename Solve>
Solved solve(const Params& p, Solve fn) {
  Solved s;
  std::string error;
  REQUIRE_MESSAGE(fn(p, s.mesh, &s.report, &error, &s.motion), error);
  return s;
}

// The convex hull's area of the leaf primitive's vertices seen from above: the footprint measured
// from the mesh, not from the solver's own tips.
f64 leaf_hull_area(const assets::MeshData& mesh) {
  const assets::Primitive& leaves = mesh.primitives[1];
  std::vector<std::pair<f64, f64>> points;
  for (u32 i = leaves.first_index; i < leaves.first_index + leaves.index_count; ++i) {
    const Vec3 v = mesh.positions[mesh.indices[i]];
    points.emplace_back(static_cast<f64>(v.x), static_cast<f64>(v.z));
  }
  std::sort(points.begin(), points.end());
  points.erase(std::unique(points.begin(), points.end()), points.end());
  if (points.size() < 3) return 0.0;
  const auto turn = [](const std::pair<f64, f64>& o, const std::pair<f64, f64>& a,
                       const std::pair<f64, f64>& b) {
    return (a.first - o.first) * (b.second - o.second) -
           (a.second - o.second) * (b.first - o.first);
  };
  std::vector<std::pair<f64, f64>> hull(2 * points.size());
  usize k = 0;
  for (usize i = 0; i < points.size(); ++i) {
    while (k >= 2 && turn(hull[k - 2], hull[k - 1], points[i]) <= 0.0)
      --k;
    hull[k++] = points[i];
  }
  for (usize i = points.size() - 1, t = k + 1; i > 0; --i) {
    while (k >= t && turn(hull[k - 2], hull[k - 1], points[i - 1]) <= 0.0)
      --k;
    hull[k++] = points[i - 1];
  }
  hull.resize(k - 1);
  f64 twice = 0.0;
  for (usize i = 0; i < hull.size(); ++i) {
    const auto& a = hull[i];
    const auto& b = hull[(i + 1) % hull.size()];
    twice += a.first * b.second - b.first * a.second;
  }
  return 0.5 * std::abs(twice);
}

f64 leaf_area(const assets::MeshData& mesh) {
  const assets::Primitive& leaves = mesh.primitives[1];
  f64 area = 0.0;
  for (u32 i = leaves.first_index; i < leaves.first_index + leaves.index_count; i += 3) {
    const Vec3 a = mesh.positions[mesh.indices[i]];
    const Vec3 b = mesh.positions[mesh.indices[i + 1]];
    const Vec3 c = mesh.positions[mesh.indices[i + 2]];
    const Vec3 n = cross(b - a, c - a);
    area += 0.5 * std::sqrt(static_cast<f64>(dot(n, n)));
  }
  return area;
}

// The pipe rule at every node of the recursion, and every twig at the species' radius: the
// children's base radii to the power D sum to the parent's (a named root's: the radius its
// children share, which is its base radius for a limb), and a terminal twig is no thinner than
// `twig_radius` and less than one pipe step thicker.
void check_crown(const Vector<flora::CrownShoot>& crown, f64 exponent, f64 twig_radius, f64 step,
                 u32& nodes_checked, f64& worst) {
  std::vector<f64> sums(crown.size(), 0.0);
  std::vector<u32> named_children(crown.size(), 0u);
  for (u32 i = 0; i < crown.size(); ++i) {
    const flora::CrownShoot& s = crown[i];
    if (s.parent == 0xFFFFFFFFu) continue;
    REQUIRE(s.parent < i);  // parents first
    if ((s.flags & flora::k_crown_shoot_named) != 0u) {
      ++named_children[s.parent];
    } else {
      sums[s.parent] += std::pow(static_cast<f64>(s.base_radius), exponent);
    }
  }
  nodes_checked = 0;
  worst = 0.0;
  u32 twigs = 0;
  for (u32 i = 0; i < crown.size(); ++i) {
    const flora::CrownShoot& s = crown[i];
    if ((s.flags & flora::k_crown_shoot_terminal) != 0u) {
      ++twigs;
      CAPTURE(i);
      CHECK(static_cast<f64>(s.base_radius) >= twig_radius * (1.0 - 1e-5));
      CHECK(static_cast<f64>(s.base_radius) < twig_radius * step * (1.0 + 1e-5));
      continue;
    }
    if (sums[i] == 0.0 || named_children[i] != 0u) continue;
    const f64 own = std::pow(static_cast<f64>(s.base_radius), exponent);
    const f64 error = std::abs(sums[i] / own - 1.0);
    worst = std::max(worst, error);
    ++nodes_checked;
  }
  CHECK(twigs > 0);
}

f64 smallest_step(f64 leader_share, u32 laterals, f64 exponent) {
  const f64 lateral = (1.0 - leader_share) / static_cast<f64>(laterals);
  return std::pow(std::min(lateral, leader_share), -1.0 / exponent);
}

}  // namespace

TEST_CASE("flora allometry: a leaf is the species' size whatever the tree's height") {
  // The broadleaf at 8 m and at 32 m, the ancient tree at 45 m and at 100 m: cards of one length.
  for (const f32 height : {8.0f, 32.0f}) {
    flora::Broadleaf p;
    p.height_m = height;
    p.triangle_budget = 2000000;
    const Solved s = solve(p, flora::solve_broadleaf);
    CAPTURE(height);
    CHECK(s.report.merge_level == 0u);
    CHECK(std::abs(s.report.card_length - p.leaf_length_m) < 0.001f);
    MESSAGE("broadleaf " << height << " m: " << s.report.twigs << " twigs over " << s.report.orders
                         << " orders, " << s.report.leaf_cards << " cards of "
                         << s.report.card_length << " m, LAI " << s.report.leaf_area_index);
  }
  for (const f32 height : {45.0f, 100.0f}) {
    flora::Ancient p;
    p.height_m = height;
    if (height < 60.0f) {
      p.trunk_radius_m = 2.4f;
      p.limb_low_m = 14.0f;
      p.limb_high_m = 30.0f;
      p.limb_radius_m = 0.6f;
      p.limb_run_m = 8.0f;
      p.limb_reach_m = 14.0f;
      p.buttress_height_m = 4.0f;
      p.hollow_width_m = 1.2f;
      p.hollow_height_m = 2.2f;
    }
    const Solved s = solve(p, flora::solve_ancient);
    CAPTURE(height);
    CHECK(s.report.merge_level == 0u);
    CHECK(std::abs(s.report.card_length - p.leaf_length_m) < 0.001f);
    MESSAGE("ancient " << height << " m: " << s.report.twigs << " twigs over " << s.report.orders
                       << " orders, " << s.report.leaf_cards << " cards of " << s.report.card_length
                       << " m, LAI " << s.report.leaf_area_index << ", " << s.report.triangles
                       << " triangles");
  }
}

TEST_CASE("flora allometry: the order count grows with the log of trunk over twig radius") {
  u32 last = 0;
  for (const f32 height : {6.0f, 12.0f, 24.0f}) {
    flora::Broadleaf p;
    p.height_m = height;
    Vector<flora::CrownShoot> crown;
    REQUIRE(flora::broadleaf_crown(p, crown));
    const f64 r_trunk = static_cast<f64>(crown[0].base_radius);
    const f64 twigs_expected =
        std::pow(r_trunk / static_cast<f64>(p.twig_radius_m), static_cast<f64>(p.area_exponent));
    u32 twigs = 0;
    u32 orders = 0;
    for (const flora::CrownShoot& s : crown) {
      if ((s.flags & flora::k_crown_shoot_terminal) == 0u) continue;
      ++twigs;
      orders = std::max(orders, s.generation);
    }
    // The trunk and the limbs, and the recursion's generations to the deepest twig.
    orders += 2;
    const f64 formula =
        1.0 + std::log(twigs_expected) / std::log(static_cast<f64>(p.branching_ratio));
    MESSAGE(height << " m: trunk radius " << r_trunk << " m, " << twigs << " twigs (the pipe rule "
                   << twigs_expected << "), " << orders << " orders (log formula " << formula
                   << ")");
    CHECK(orders >= last);
    CHECK(std::abs(static_cast<f64>(orders) - formula) <= 1.0 + 1e-9);
    last = orders;
  }
}

TEST_CASE("flora allometry: the pipe rule holds at every node and every twig is the species'") {
  for (const f32 exponent : {2.0f, 2.3f}) {
    CAPTURE(exponent);
    flora::Broadleaf b;
    b.area_exponent = exponent;
    Vector<flora::CrownShoot> crown;
    REQUIRE(flora::broadleaf_crown(b, crown));
    u32 nodes = 0;
    f64 worst = 0.0;
    check_crown(crown, exponent, static_cast<f64>(b.twig_radius_m),
                smallest_step(b.leader_share, std::min(b.branching_ratio - 1, b.twigs), exponent),
                nodes, worst);
    MESSAGE("broadleaf, exponent " << exponent << ": " << crown.size() << " shoots, " << nodes
                                   << " nodes, the worst within " << worst);
    CHECK(nodes > 100);
    CHECK(worst < 0.02);
    // The ancient tree at 2.3 is a million and a half shoots: the broadleaf says the same thing at
    // that exponent for a fraction of the time.
    if (exponent != 2.0f) continue;

    flora::Ancient a;
    a.area_exponent = exponent;
    REQUIRE(flora::ancient_crown(a, crown));
    check_crown(crown, exponent, static_cast<f64>(a.twig_radius_m),
                smallest_step(a.leader_share,
                              std::min({a.branching_ratio - 1, a.sub_branches, a.twigs}), exponent),
                nodes, worst);
    MESSAGE("ancient, exponent " << exponent << ": " << crown.size() << " shoots, " << nodes
                                 << " nodes, the worst within " << worst);
    CHECK(nodes > 1000);
    CHECK(worst < 0.02);
  }
}

TEST_CASE("flora allometry: the realised leaf area index is the field's") {
  const flora::Broadleaf b;
  const Solved oak = solve(b, flora::solve_broadleaf);
  const f64 oak_lai = leaf_area(oak.mesh) / leaf_hull_area(oak.mesh);
  MESSAGE("broadleaf: LAI " << oak_lai << " measured from the mesh (" << oak.report.leaf_area_index
                            << " the solver's, " << oak.report.footprint_m2 << " m2 footprint, "
                            << oak.report.leaf_cards << " cards)");
  CHECK(std::abs(oak_lai / static_cast<f64>(b.leaf_area_index) - 1.0) < 0.2);
  const flora::Ancient a;
  const Solved ancient = solve(a, flora::solve_ancient);
  const f64 ancient_lai = leaf_area(ancient.mesh) / leaf_hull_area(ancient.mesh);
  MESSAGE("ancient: LAI " << ancient_lai << " measured from the mesh ("
                          << ancient.report.leaf_area_index << " the solver's, "
                          << ancient.report.footprint_m2 << " m2 footprint, "
                          << ancient.report.leaf_cards << " cards, " << ancient.report.twigs
                          << " twigs, " << ancient.report.orders << " orders, "
                          << ancient.report.parts << " parts, " << ancient.report.triangles
                          << " triangles after " << ancient.report.steps << " steps)");
  CHECK(ancient.report.merge_level == 0u);
  CHECK(std::abs(ancient_lai / static_cast<f64>(a.leaf_area_index) - 1.0) < 0.2);
}

TEST_CASE("flora allometry: the old fields still validate and build, as caps") {
  flora::FloraFile file;
  std::string error;
  REQUIRE_MESSAGE(flora::read_flora_file("{\"species\":\"ancient\",\"ancient\":{\"sub_branches\":2,"
                                         "\"twigs\":3,\"triangle_budget\":200000}}",
                                         file, error),
                  error);
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  REQUIRE_MESSAGE(flora::solve_flora(file, mesh, motion, nullptr, &report, error), error);
  CHECK(report.triangles <= 200000u);
  // A shoot of the recursion bears at most `sub_branches` laterals and its leader.
  Vector<flora::CrownShoot> crown;
  REQUIRE(flora::ancient_crown(file.ancient, crown));
  std::vector<u32> children(crown.size(), 0u);
  for (const flora::CrownShoot& s : crown)
    if (s.parent != 0xFFFFFFFFu && (crown[s.parent].flags & flora::k_crown_shoot_named) == 0u)
      ++children[s.parent];
  CHECK(*std::max_element(children.begin(), children.end()) <= 3u);
  REQUIRE_MESSAGE(flora::read_flora_file("{\"species\":\"broadleaf\",\"broadleaf\":{\"twigs\":2,"
                                         "\"leaves\":1}}",
                                         file, error),
                  error);
  REQUIRE_MESSAGE(flora::solve_flora(file, mesh, motion, nullptr, &report, error), error);
  CHECK(report.cards_per_side <= 1u);
}

namespace {

// Every rung of the ladder: the mesh in its budget, cards merged finest first (the merge level
// never falls as the budget does), and the skeleton the same — the part list, and where the
// species emits it the structural table, byte for byte.
template <typename Params, typename Solve>
void check_ladder(const std::string& name, const Params& params, Solve fn,
                  std::initializer_list<u32> budgets) {
  const Solved full = solve(params, fn);
  u32 last_merge = full.report.merge_level;
  for (const u32 budget : budgets) {
    CAPTURE(budget);
    Params q = params;
    q.triangle_budget = budget;
    const Solved s = solve(q, fn);
    MESSAGE(name << " on " << budget << ": " << s.report.triangles << " triangles after "
                 << s.report.steps << " steps, wood from class " << s.report.wood_class
                 << ", merge " << s.report.merge_level << " (cards to " << s.report.card_length
                 << " m), " << s.report.leaf_cards << " cards");
    CHECK(s.report.triangles <= budget);
    CHECK(s.report.merge_level >= last_merge);
    last_merge = s.report.merge_level;
    REQUIRE(s.motion.parts.size() == full.motion.parts.size());
    CHECK(std::memcmp(s.motion.parts.data(), full.motion.parts.data(),
                      full.motion.parts.size() * sizeof(flora::MotionPart)) == 0);
    REQUIRE(s.motion.structure.size() == full.motion.structure.size());
    if (!full.motion.structure.empty()) {
      CHECK(std::memcmp(s.motion.structure.data(), full.motion.structure.data(),
                        full.motion.structure.size() * sizeof(flora::StructuralPart)) == 0);
    }
    CHECK(s.report.parts == full.report.parts);
    CHECK(s.report.load_bearing == full.report.load_bearing);
  }
}

}  // namespace

TEST_CASE("flora allometry: the ladder changes detail, never the structure") {
  check_ladder("broadleaf", flora::Broadleaf{}, flora::solve_broadleaf,
               {40000u, 25000u, 15000u, 8000u});
  check_ladder("ancient", flora::Ancient{}, flora::solve_ancient,
               {1000000u, 500000u, 250000u, 100000u, 40000u, 20000u});
}

// The collision proxy is the mesh's triangles by part (scene_collision): how far the load-bearing
// runs' and limbs' surfaces move between the finest and the coarsest rung is how far a walker's
// feet would, at a different preset. Measured here, not changed (another brief owns the proxy).
TEST_CASE("flora allometry: how far the load-bearing surfaces move down the ladder") {
  const flora::Ancient fine_p;
  flora::Ancient coarse_p;
  coarse_p.triangle_budget = 20000;  // the bottom rung
  const Solved fine = solve(fine_p, flora::solve_ancient);
  const Solved coarse = solve(coarse_p, flora::solve_ancient);
  const auto triangles_of = [](const Solved& s, u32 part) {
    std::vector<std::array<Vec3, 3>> out;
    const assets::Primitive& bark = s.mesh.primitives[0];
    for (u32 i = bark.first_index; i < bark.first_index + bark.index_count; i += 3) {
      const u32 a = s.mesh.indices[i];
      if (s.motion.vertex_part[a] != part) continue;
      out.push_back({s.mesh.positions[a], s.mesh.positions[s.mesh.indices[i + 1]],
                     s.mesh.positions[s.mesh.indices[i + 2]]});
    }
    return out;
  };
  // The distance from a point to a triangle (Ericson, Real-Time Collision Detection 5.1.5).
  const auto distance = [](Vec3 p, const std::array<Vec3, 3>& t) {
    const Vec3 a = t[0], b = t[1], c = t[2];
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const f32 d1 = dot(ab, ap), d2 = dot(ac, ap);
    Vec3 q;
    if (d1 <= 0.0f && d2 <= 0.0f) {
      q = a;
    } else {
      const Vec3 bp = p - b;
      const f32 d3 = dot(ab, bp), d4 = dot(ac, bp);
      const Vec3 cp = p - c;
      const f32 d5 = dot(ab, cp), d6 = dot(ac, cp);
      const f32 vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
      if (d3 >= 0.0f && d4 <= d3) {
        q = b;
      } else if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        q = a + ab * (d1 / (d1 - d3));
      } else if (d6 >= 0.0f && d5 <= d6) {
        q = c;
      } else if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        q = a + ac * (d2 / (d2 - d6));
      } else if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        q = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
      } else {
        const f32 denom = 1.0f / (va + vb + vc);
        q = a + ab * (vb * denom) + ac * (vc * denom);
      }
    }
    const Vec3 d = p - q;
    return std::sqrt(dot(d, d));
  };
  // One way: the farthest sample of `from` (vertices, edge midpoints, centroids) from `to`.
  const auto one_way = [&](const std::vector<std::array<Vec3, 3>>& from,
                           const std::vector<std::array<Vec3, 3>>& to) {
    f32 worst = 0.0f;
    for (const auto& t : from) {
      const Vec3 samples[7] = {t[0],
                               t[1],
                               t[2],
                               (t[0] + t[1]) * 0.5f,
                               (t[1] + t[2]) * 0.5f,
                               (t[2] + t[0]) * 0.5f,
                               (t[0] + t[1] + t[2]) * (1.0f / 3.0f)};
      for (const Vec3& p : samples) {
        f32 best = 1e30f;
        for (const auto& u : to)
          best = std::min(best, distance(p, u));
        worst = std::max(worst, best);
      }
    }
    return worst;
  };
  f32 run_worst = 0.0f;
  f32 limb_worst = 0.0f;
  for (u32 i = 0; i < fine_p.limbs; ++i) {
    for (u32 k = 0; k < 2; ++k) {
      const u32 part = 1 + 2 * i + k;
      REQUIRE((fine.motion.structure[part].flags & flora::k_load_bearing) != 0u);
      const auto f = triangles_of(fine, part);
      const auto c = triangles_of(coarse, part);
      REQUIRE(!f.empty());
      REQUIRE(!c.empty());
      const f32 d = std::max(one_way(f, c), one_way(c, f));
      (k == 0 ? run_worst : limb_worst) = std::max(k == 0 ? run_worst : limb_worst, d);
    }
  }
  MESSAGE("between the finest rung ("
          << fine.report.triangles << " triangles) and a coarse one (" << coarse.report.triangles
          << " after " << coarse.report.steps << " steps) the runs' surfaces move " << run_worst
          << " m at most and the limbs' past them " << limb_worst << " m");
  CHECK(run_worst > 0.0f);
  CHECK(limb_worst > 0.0f);
}

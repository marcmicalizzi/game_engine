#include "../src/kit.h"

#include <core/hash/hash.h>
#include <domain/flora/flora.h>

#include <doctest/doctest.h>

#include <cmath>

using namespace engine;
using namespace engine::flora::detail;

namespace {
u32 shoot(Crown& crown, u32 parent, f64 station, f64 radius, D3 direction = k_up) {
  Rng rng(1);
  const D3 base = parent == k_crown_none ? D3{} : D3{.1, station, 0};
  const u32 index = crown_add_named(crown, grow_axis(base, direction, 10, 16, k_up, 0, 0, rng),
                                    radius, .5 * radius, parent);
  crown.nodes[index].parent_arc = station;
  return index;
}
}  // namespace

TEST_CASE("flora taper: coincident laterals debit area together and the leader remains slender") {
  Crown crown;
  shoot(crown, k_crown_none, 0, 1);
  shoot(crown, 0, 4, .4, {1, 0, 0});
  shoot(crown, 0, 4, .4, {-1, 0, 0});
  shoot(crown, 0, 10, .01);
  crown_taper(crown, 2);
  const Axis& axis = crown.axes[0];
  const auto support = [&](f64 arc) {
    const f64 radius = axis_radius(axis, 1, crown.nodes[0].r1, arc) / (1 - .05 * arc / 10);
    return radius * radius;
  };
  // Account separately for the continuously tapering reserve and the two side branches.
  const f64 reserve = 1 - .4 * .4 * 2 - .01 * .01;
  const f64 before = support(4.0) - reserve * .6 * .6;
  const f64 after = support(5.6) - reserve * .44 * .44;
  CHECK(before - after == doctest::Approx(.32).epsilon(1e-10));
  CHECK(crown.nodes[1].r0 == doctest::Approx(.4 * .98));
  CHECK(crown.nodes[1].r0 == crown.nodes[2].r0);
  CHECK(crown.nodes[0].r1 == doctest::Approx(.0095));
  CHECK(crown.nodes[0].r1 == doctest::Approx(crown.nodes[3].r0));
  CHECK(length_of(crown.nodes[3].base - axis.point.back()) < 1e-12);
  f64 previous = 1;
  for (u32 sample = 0; sample <= 1000; ++sample) {
    const f64 radius = axis_radius(axis, 1, crown.nodes[0].r1, .01 * sample);
    CHECK(radius > 0);
    CHECK(radius <= previous + 1e-12);
    previous = radius;
  }
  // A mesh budget samples the same profile without changing it or adding ring intervals.
  for (u32 rows : {3u, 12u, 32u}) {
    const Axis sampled = resample_axis(axis, rows);
    CHECK(sampled.point.size() == rows + 1);
    for (f64 arc : {0., 4., 4.8, 5.6, 10.})
      CHECK(axis_radius(sampled, 1, .0095, arc) == axis_radius(axis, 1, .0095, arc));
  }
}

TEST_CASE("flora taper: one lateral cannot redirect the trunk or masquerade as a terminal fork") {
  for (f64 radius : {.003, .35, 2.}) {
    Crown crown;
    shoot(crown, k_crown_none, 0, radius);
    shoot(crown, 0, 8.5, .6 * radius, {1, 0, 0});
    const Axis original = crown.axes[0];
    crown.first_recursion = crown.nodes.size();
    CrownSpec spec;
    spec.twig_radius = .002;
    spec.twig_length = .3;
    crown_add_apex(crown, 0, spec);
    crown_taper(crown, 2);
    REQUIRE(crown.nodes.size() == 3);
    REQUIRE(crown.twigs.size() == 1);
    CHECK(crown.nodes[2].parent == 0);
    CHECK(crown.nodes[2].carrier == 0);
    CHECK(crown.nodes[2].weight == 1);
    CHECK(crown.nodes[0].r1 <= spec.twig_radius);
    CHECK(crown.nodes[0].r1 == doctest::Approx(crown.nodes[2].r0));
    CHECK(length_of(crown.nodes[2].base - original.point.back()) < 1e-12);
    CHECK(dot(crown.nodes[2].dir, original.tangent.back()) == doctest::Approx(1));
    for (u32 point = 0; point < original.point.size(); ++point)
      CHECK(length_of(original.point[point] - crown.axes[0].point[point]) == 0);
  }
}

TEST_CASE("flora taper: terminal forks support both outgoing axes without a phantom leader") {
  for (f64 exponent : {1.8, 2., 3.}) {
    Crown crown;
    shoot(crown, k_crown_none, 0, 1);
    shoot(crown, 0, 10, .6, {.6, .8, 0});
    shoot(crown, 0, 10, .8, {-.6, .8, 0});
    crown_taper(crown, exponent);
    CHECK(std::pow(crown.nodes[0].r1, exponent) ==
          doctest::Approx(std::pow(crown.nodes[1].r0, exponent) +
                          std::pow(crown.nodes[2].r0, exponent))
              .epsilon(1e-10));
    CHECK(crown.nodes.size() == 3);
    CHECK(crown.twigs.empty());
  }
}

TEST_CASE("flora taper: keys invalidate affected shapes and keep unrelated tree architectures") {
  for (const char* species :
       {"maple", "oak", "birch", "broadleaf", "spruce", "willow", "ancient"}) {
    flora::FloraFile file;
    file.species = species;
    const std::string canonical = flora::canonical_flora_json(file);
    u64 old = hash_combine(hash_bytes(canonical.data(), canonical.size()),
                           hash_bytes(file.species.data(), file.species.size()));
    old = hash_combine(old, flora::k_flora_version);
    const bool changed = file.species == "maple" || file.species == "oak" ||
                         file.species == "birch" || file.species == "broadleaf";
    CHECK(flora::flora_source_hash(file) ==
          (changed ? hash_combine(old, flora::k_branch_taper_version) : old));
  }
}

TEST_CASE("flora taper: heavy off-takes keep their open rim buried at every named mesh budget") {
  for (f64 angle : {15., 45., 80.}) {
    for (u32 rows : {2u, 4u, 14u}) {
      Crown crown;
      shoot(crown, k_crown_none, 0, 1);
      const D3 direction{det::sin(angle * k_degree), det::cos(angle * k_degree), 0};
      shoot(crown, 0, 8.5, .99, direction);
      crown.first_recursion = 2;
      CrownSpec spec;
      spec.twig_radius = .002;
      spec.twig_length = .3;
      crown_add_apex(crown, 0, spec);
      crown_taper(crown, 2);
      assets::MeshData mesh;
      flora::Motion motion;
      Emitter emit{mesh, motion};
      emit.wood = WoodProfile{.4, .3, .1, .05, .1, 17};
      const CrownNode& branch = crown.nodes[1];
      emit.begin_part(branch.base, branch.dir, branch.length, 0);
      crown_seat(crown, 1, emit);
      REQUIRE(emit.seated);
      const Axis sampled = resample_axis(crown.axes[branch.axis], rows, emit.seat_length);
      CHECK(sampled.point.size() == rows + 1);
      CHECK(sampled.arc[1] <= emit.seat_length);
      build_tube(sampled, branch.r0, branch.r1, 8, {}, emit);
      CHECK(mesh.indices.size() / 3 == tube_triangles(rows, 8));
      // Opening must be safely inside the trunk even with a near-total area off-take.
      for (u32 vertex = 0; vertex <= 8; ++vertex) {
        const D3 point = from_f32(mesh.positions[vertex]);
        const f64 parent_radius = axis_radius(crown.axes[0], 1, crown.nodes[0].r1, point.y);
        const f64 radial = std::sqrt(point.x * point.x + point.z * point.z);
        CHECK(radial < .6 * parent_radius);
        CHECK(point.y < 8.5);
      }
      for (const Vec3 n : mesh.normals) {
        CHECK(std::isfinite(n.x));
        CHECK(std::isfinite(n.y));
        CHECK(std::isfinite(n.z));
      }
    }
  }
}

TEST_CASE("flora taper: explicit upper leaders share support without promoting a lateral") {
  for (f64 exponent : {1.8, 2., 3.}) {
    Crown crown;
    shoot(crown, k_crown_none, 0, 1);
    Vector<CrownRoot> roots;
    for (u32 i = 0; i < 2; ++i) {
      const u32 index = shoot(crown, 0, 8, .4, {i == 0 ? .6 : -.6, .8, 0});
      CrownNode& node = crown.nodes[index];
      Axis& axis = crown.axes[node.axis];
      const f64 scale = i == 0 ? .6 : .3;
      for (u32 sample = 0; sample < axis.point.size(); ++sample) {
        axis.point[sample] = node.base + (axis.point[sample] - node.base) * scale;
        axis.arc[sample] *= scale;
      }
      axis.length *= scale;
      node.length = axis.length;
      node.tip = axis.point.back();
      CrownRoot root;
      root.node = index;
      root.radius = .4;
      root.laterals = 3;
      root.lateral_length = .4 * node.length;
      roots.push_back(root);
    }
    CrownSpec spec;
    spec.area_exponent = exponent;
    spec.twig_radius = .002;
    const Axis lateral = crown.axes[crown.nodes[2].axis];
    // Merely being last must not redirect a mature stem.
    crown_finish_stem(crown, 0, exponent);
    CHECK(crown.nodes[0].length == 10);
    REQUIRE(crown_allocate_upper(crown, roots, 0, spec));
    REQUIRE(roots.size() == 3);
    const u32 leader = roots.back().node;
    REQUIRE((crown.nodes[leader].flags & k_crown_continuation) != 0u);
    CHECK(crown.nodes[leader].length == 2);
    CHECK(dot(crown.nodes[leader].dir, k_up) == doctest::Approx(1));
    CHECK(length_of(crown.nodes[leader].tip - D3{0, 10, 0}) < 1e-12);
    f64 budget = 0;
    for (const CrownRoot& root : roots)
      budget += std::pow(root.radius, exponent);
    CHECK(budget == doctest::Approx(2 * std::pow(.4, exponent)).epsilon(1e-10));
    CHECK(std::pow(roots[0].radius / roots[1].radius, exponent) == doctest::Approx(4));
    CHECK(roots[0].radius > roots[2].radius);  // the chosen leader need not be the thickest
    const f64 leader_area = std::pow(roots[2].radius, exponent);
    CHECK(leader_area / budget >= spec.leader_share - 1e-10);
    crown.first_recursion = crown.nodes.size();
    crown_finish_stem(crown, 0, exponent);
    CHECK(crown.nodes[0].length == 8);
    // The unequal final laterals pull the leader away from the larger off-take,
    // rather than shrinking it concentrically into a bottle-shaped shoulder.
    CHECK(crown.nodes[leader].tip.x < 0);
    const Axis& unchanged_lateral = crown.axes[crown.nodes[2].axis];
    for (u32 i = 0; i < lateral.point.size(); ++i)
      CHECK(length_of(unchanged_lateral.point[i] - lateral.point[i]) == 0);
    const u32 before_apex = crown.nodes.size();
    crown_add_apex(crown, 0, spec);
    CHECK(crown.nodes.size() == before_apex);  // no phantom twig between the real axes
    crown_taper(crown, exponent);
    CHECK(length_of(crown.nodes[1].base - crown.axes[0].point.back()) > 0);
    CHECK(length_of(crown.nodes[leader].base - crown.axes[0].point.back()) < 1e-10);
    const Axis& stem = crown.axes[0];
    CHECK(axis_radius(stem, 1, crown.nodes[0].r1, 4) == doctest::Approx(.975));
    CHECK(crown.nodes[0].r1 == doctest::Approx(.95));
    f64 outgoing = 0;
    for (u32 i = 1; i < crown.nodes.size(); ++i)
      outgoing += std::pow(crown.nodes[i].r0, exponent);
    CHECK(outgoing == doctest::Approx(std::pow(.95, exponent)));
    crown_join_relays(crown);
    REQUIRE(crown.relays.size() == 1);
    CHECK(crown.relays[0].child == leader);
    for (u32 rows : {4u, 12u, 24u}) {
      const Axis mesh = crown_sample_stem(crown, 0, rows);
      CHECK(mesh.point.size() == rows + 1);
      CHECK(length_of(mesh.point.back() - axis_point(crown.axes[crown.nodes[leader].axis],
                                                     crown.relays[0].child_arc)) < 1e-10);
    }
  }
  // Small shoots can share a motion part; test branching on growth nodes themselves.
  Crown growing;
  shoot(growing, k_crown_none, 0, 1);
  const u32 lateral = shoot(growing, 0, 8, .4, {.6, .8, 0});
  Vector<CrownRoot> growing_roots;
  CrownRoot growing_root;
  growing_root.node = lateral;
  growing_root.radius = .4;
  growing_roots.push_back(growing_root);
  CrownSpec growing_spec;
  growing_spec.twig_radius = .02;
  REQUIRE(crown_allocate_upper(growing, growing_roots, 0, growing_spec));
  const u32 upper = growing_roots.back().node;
  crown_finish_stem(growing, 0);
  crown_grow(growing, growing_spec, growing_roots, 2);
  u32 children = 0;
  for (const CrownNode& node : growing.nodes)
    children += node.parent == upper ? 1u : 0u;
  CHECK(children >= 2);

  Crown sapling;
  shoot(sapling, k_crown_none, 0, .01);
  shoot(sapling, 0, 8, .003);
  Vector<CrownRoot> roots;
  CrownRoot root;
  root.node = 1;
  root.radius = .003;
  roots.push_back(root);
  CHECK_FALSE(crown_allocate_upper(sapling, roots, 0, CrownSpec{}));
  crown_finish_stem(sapling, 0);
  CHECK(sapling.nodes.size() == 2);
  CHECK(sapling.nodes[0].length == 10);
  CHECK((sapling.nodes[0].flags & k_crown_stem_relay) == 0u);
}

TEST_CASE("flora taper: Forest beech 10408 keeps an upright branching upper leader") {
  flora::FloraFile file;
  file.species = "beech";
  file.tree.seed = 10408;
  file.tree.age_years = 260;
  file.tree.height_m = 26;
  file.tree.trunk_radius_m = .66f;
  file.tree.crown_base = .34f;
  file.tree.crown_scale = 1.15f;
  file.tree.site_quality = .9f;
  file.tree.growing_space = .65f;
  file.tree.vitality = .95f;
  file.tree.card_bake_px = file.tree.impostor_px = 0;
  for (u32 budget : {8000u, 1000000u}) {
    file.tree.triangle_budget = budget;
    assets::MeshData mesh;
    flora::Motion motion;
    flora::TreeReport report;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_flora(file, mesh, motion, nullptr, &report, error), error);
    CHECK(report.triangles <= budget);
    u32 highest = 0;
    for (u32 i = 1; i < motion.parts.size(); ++i)
      if (motion.parts[i].parent == 0 && motion.parts[i].pivot.y > motion.parts[highest].pivot.y)
        highest = i;
    REQUIRE(highest != 0);
    CHECK(motion.parts[highest].direction.y > .95f);
    CHECK(motion.parts[highest].length < .2f * file.tree.height_m);
    u32 children = 0;
    for (const auto& part : motion.parts)
      children += part.parent == highest ? 1u : 0u;
    CHECK(children >= 2);  // this specimen has multiple substantial upper shoots
    MESSAGE("beech 10408 budget=" << budget << " triangles=" << report.triangles << " upper length="
                                  << motion.parts[highest].length << " children=" << children);
  }
}

#include "../src/maturity.h"

#include <core/hash/hash.h>
#include <domain/flora/structure.h>
#include <domain/geometry/flora_species.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace engine;
namespace {
u64 mesh_hash(const assets::MeshData& mesh) {
  return hash_combine(hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3)),
                      hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
}
void valid_mesh(const assets::MeshData& mesh, const flora::Motion& motion) {
  REQUIRE(!mesh.indices.empty());
  REQUIRE(motion.vertex_part.size() == mesh.positions.size());
  REQUIRE(std::all_of(mesh.positions.begin(), mesh.positions.end(), [](const Vec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
  }));
  REQUIRE(std::all_of(mesh.indices.begin(), mesh.indices.end(),
                      [&](u32 i) { return i < mesh.positions.size(); }));
  for (u32 i = 1; i < motion.parts.size(); ++i)
    REQUIRE(motion.parts[i].parent < i);
  for (const auto& s : motion.structure) {
    CHECK(s.base_radius >= 0);
    CHECK(s.tip_radius >= 0);
    CHECK(s.tip_radius <= s.base_radius);
  }
}
}  // namespace

TEST_CASE("flora maturity: species scales separate age size space and vigor") {
  using flora::detail::maturity_state;
  flora::Tree t;
  t.age_years = 50;
  const auto alder = maturity_state("alder", t);
  const auto hemlock = maturity_state("hemlock", t);
  CHECK(alder.mature == 1);
  CHECK(hemlock.mature < .5f);
  CHECK(alder.height > hemlock.height);
  t.age_years = 400;
  const auto old = maturity_state("hemlock", t);
  t.site_quality = .05f;
  const auto suppressed = maturity_state("hemlock", t);
  CHECK(suppressed.height < old.height * .3f);
  CHECK(suppressed.radius < old.radius * .15f);
  CHECK(suppressed.old == old.old);
  t.site_quality = 1;
  t.vitality = 0;
  const auto stressed = maturity_state("hemlock", t);
  CHECK(stressed.height == old.height);
  CHECK(stressed.radius == old.radius);
  t.age_years = 150;
  const auto maple150 = maturity_state("maple", t);
  t.age_years = 400;
  const auto maple400 = maturity_state("maple", t);
  CHECK(maple400.height < maple150.height * 1.08f);
  CHECK(maple400.radius > maple150.radius * 1.3f);
  t.height_m = 2;
  t.trunk_radius_m = .05f;
  CHECK(maturity_state("oak", t).height == 2);
  CHECK(maturity_state("oak", t).radius == .05f);
}

TEST_CASE("flora maturity: every tree solves sapling mature and old within its budget") {
  for (const auto& info : geometry::k_flora_species) {
    if (!info.profile && info.name != "broadleaf" && info.name != "conifer" &&
        info.name != "birch" && info.name != "willow" && info.name != "orchard" &&
        info.name != "date_palm")
      continue;
    for (const f32 years : {1.0f, 30.0f, 200.0f, 800.0f}) {
      CAPTURE(info.name);
      CAPTURE(years);
      flora::FloraFile f;
      f.species = info.name;
      f.tree.age_years = years;
      f.tree.seed = 17;
      f.tree.card_bake_px = 0;
      f.tree.impostor_px = 0;
      assets::MeshData mesh, again;
      flora::Motion motion, repeated;
      flora::TreeReport report;
      flora::PalmReport palm;
      std::string error;
      REQUIRE_MESSAGE(flora::solve_flora(f, mesh, motion, &palm, &report, error), error);
      valid_mesh(mesh, motion);
      CHECK(mesh.indices.size() / 3 <= (info.profile ? info.triangle_budget : 2000000u));
      if (info.name != "date_palm") CHECK(report.leaf_triangles > 0);
      if (years == 1) {
        CHECK(motion.parts[0].length < 3);
        REQUIRE_MESSAGE(flora::solve_flora(f, again, repeated, nullptr, nullptr, error), error);
        CHECK(mesh_hash(mesh) == mesh_hash(again));
      }
      for (const auto& s : motion.structure)
        if ((s.flags & flora::k_load_bearing) != 0)
          CHECK(s.base_radius >= flora::load_bearing_min_radius(std::string(info.name)));
    }
  }
}

TEST_CASE("flora maturity: fixed size still changes architecture and vigor keeps wood") {
  for (const char* species : {"oak", "hemlock", "birch", "willow", "orchard"}) {
    CAPTURE(std::string_view(species));
    flora::FloraFile f;
    f.species = species;
    f.tree.height_m = 3;
    f.tree.trunk_radius_m = .08f;
    f.tree.age_years = 3;
    assets::MeshData young, old, sparse;
    flora::Motion a, b, c;
    flora::TreeReport ry, ro, rs;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_flora(f, young, a, nullptr, &ry, error), error);
    f.tree.age_years = 500;
    REQUIRE_MESSAGE(flora::solve_flora(f, old, b, nullptr, &ro, error), error);
    CHECK(mesh_hash(young) != mesh_hash(old));
    const auto scaffold = [](const flora::Motion& m) {
      return std::count_if(m.parts.begin(), m.parts.end(),
                           [](const auto& p) { return p.parent == 0; });
    };
    CHECK(scaffold(a) < scaffold(b));
    f.tree.vitality = .1f;
    REQUIRE_MESSAGE(flora::solve_flora(f, sparse, c, nullptr, &rs, error), error);
    CHECK(rs.leaf_triangles < ro.leaf_triangles);
    if (std::string_view(species) != "hemlock") {
      CHECK(b.parts.size() == c.parts.size());
      CHECK(b.parts[0].length == c.parts[0].length);
    }
  }
}

TEST_CASE("flora maturity: input identity and legacy opt-in are explicit") {
  flora::FloraFile f, parsed;
  std::string error;
  for (const char* species : {"birch", "willow", "orchard", "date_palm", "broadleaf", "conifer"}) {
    f = {};
    f.species = species;
    const auto baseline = flora::canonical_flora_json(f);
    f.tree.age_years = 10;
    const auto aged = flora::canonical_flora_json(f);
    CHECK(aged != baseline);
    REQUIRE_MESSAGE(flora::read_flora_file(aged, parsed, error), error);
    CHECK(parsed.tree.age_years == 10);
    CHECK(flora::canonical_flora_json(parsed) == aged);
    f.tree.age_years = 0;
    CHECK(flora::canonical_flora_json(f) == baseline);
  }
  for (const char* input : {R"({"species":"oak","tree":{"age_years":-1}})",
                            R"({"species":"oak","tree":{"age_years":0.5}})",
                            R"({"species":"oak","tree":{"site_quality":0}})",
                            R"({"species":"oak","tree":{"growing_space":2}})",
                            R"({"species":"oak","tree":{"vitality":-1}})",
                            R"({"species":"ancient","tree":{"age_years":100}})"})
    CHECK_FALSE(flora::read_flora_file(input, parsed, error));
  f = {};
  f.species = "birch";
  f.tree.age_years = std::numeric_limits<f32>::quiet_NaN();
  assets::MeshData mesh;
  flora::Motion motion;
  CHECK_FALSE(flora::solve_flora(f, mesh, motion, nullptr, nullptr, error));
}

TEST_CASE("flora maturity: stem-tip crowns are local shoots rather than a second whole crown") {
  for (bool birch : {false, true}) {
    Vector<flora::CrownShoot> shoots;
    if (birch) {
      flora::Birch p;
      p.stems = 1;
      REQUIRE(flora::birch_crown(p, shoots));
    } else {
      REQUIRE(flora::broadleaf_crown(flora::Broadleaf{}, shoots));
    }
    u32 top = 0, lateral = 0;
    for (u32 i = 1; i < shoots.size(); ++i) {
      if ((shoots[i].flags & flora::k_crown_shoot_terminal) == 0) continue;
      u32 root = i;
      while (shoots[root].parent != 0 && shoots[root].parent < shoots.size())
        root = shoots[root].parent;
      if ((shoots[root].flags & flora::k_crown_shoot_named) == 0)
        ++top;
      else
        ++lateral;
    }
    MESSAGE(std::string_view(birch ? "birch" : "broadleaf")
            << ": " << top << " tip twigs, " << lateral << " lateral twigs, " << shoots.size()
            << " shoots");
    CHECK(top == 0);
    CHECK(top < lateral / 10);
    // Every terminal twig belongs to a named scaffold; there is no duplicate stem-tip crown.
    CHECK(shoots[0].tip_radius <= shoots[0].base_radius);
  }
}

TEST_CASE("flora maturity: native boundaries reject invalid sources and clamp derived controls") {
  std::string error;
  flora::FloraFile parsed;
  for (const char* input :
       {R"({"species":"conifer","tree":{"age_years":100},"conifer":{"height_m":-1}})",
        R"({"species":"conifer","tree":{"age_years":100},"conifer":{"height_m":9999}})"})
    CHECK_FALSE(flora::read_flora_file(input, parsed, error));
  for (const char* species : {"broadleaf", "conifer", "birch", "willow", "orchard", "date_palm"}) {
    CAPTURE(std::string_view(species));
    flora::FloraFile source, resolved;
    source.species = species;
    source.tree.age_years = 2000;
    source.tree.site_quality = 1.5f;
    source.tree.growing_space = 1;
    source.tree.crown_scale = 1.5f;
    source.broadleaf.height_m = 60;
    source.broadleaf.limb_length = .9f;
    source.broadleaf.limb_angle_deg = 85;
    source.conifer.height_m = 60;
    source.conifer.crown_radius_m = 16;
    source.conifer.bottom_angle_deg = 130;
    source.conifer.top_angle_deg = 100;
    source.birch.height_m = 30;
    source.birch.limb_angle_deg = 80;
    source.willow.height_m = 25;
    source.willow.limb_angle_deg = 80;
    source.orchard.height_m = 12;
    source.orchard.limb_angle_deg = 80;
    source.palm.height_m = 30;
    REQUIRE_MESSAGE(flora::detail::resolve_legacy_maturity(source, resolved, error), error);
    REQUIRE_MESSAGE(flora::read_flora_file(flora::canonical_flora_json(resolved), parsed, error),
                    error);
  }
  flora::FloraFile bad;
  bad.species = "conifer";
  bad.tree.age_years = 100;
  bad.conifer.height_m = -1;
  assets::MeshData mesh;
  flora::Motion motion;
  CHECK_FALSE(flora::solve_flora(bad, mesh, motion, nullptr, nullptr, error));
}

TEST_CASE("flora maturity: old best-site and explicit large low-budget outputs stay bounded") {
  for (bool explicit_size : {false, true}) {
    CAPTURE(explicit_size);
    flora::FloraFile source;
    source.species = "oak";
    source.tree.age_years = 600;
    source.tree.site_quality = 1.5f;
    source.tree.growing_space = 1;
    source.tree.card_bake_px = 0;
    source.tree.impostor_px = 0;
    if (explicit_size) {
      source.tree.height_m = 12;
      source.tree.trunk_radius_m = .6f;
      source.tree.triangle_budget = 24000;
    }
    assets::MeshData mesh;
    flora::Motion motion;
    flora::TreeReport report;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_flora(source, mesh, motion, nullptr, &report, error), error);
    valid_mesh(mesh, motion);
    CHECK(mesh.indices.size() / 3 <= (explicit_size ? 24000u : 500000u));
    CHECK(motion.parts.size() <= 4096);
    CHECK(report.twigs < 1000000);
    MESSAGE("old oak explicit=" << explicit_size << ": " << report.triangles << " triangles, "
                                << report.twigs << " twigs, " << motion.parts.size() << " parts");
  }
}
TEST_CASE("flora maturity: upper continuation preserves attachments and joins a living scaffold") {
  using namespace flora::detail;
  for (f64 radius : {.003, .35, 2.0}) {
    for (f64 child_length : {.1, 3.0}) {
      for (f64 angle : {15.0, 45.0, 75.0}) {
        CAPTURE(radius);
        CAPTURE(child_length);
        CAPTURE(angle);
        Rng rng = stream_of(1, 1);
        const Axis original = grow_axis({}, k_up, 10, 20, k_up, 0, 0, rng);
        Crown crown;
        const u32 stem = crown_add_named(crown, original, radius, .5 * radius, k_crown_none);
        constexpr f64 station = 8.5;
        const f64 parent_radius = lerp(radius, .5 * radius, station / original.length);
        const D3 direction{det::sin(angle * k_degree), det::cos(angle * k_degree), 0};
        const Axis child =
            grow_axis(axis_point(original, station) + D3{.5 * parent_radius, 0, 0}, direction,
                      child_length, 16, k_up, .2 / child_length, .03, rng);
        const u32 branch = crown_add_named(crown, child, .4 * radius, .2 * radius, stem);
        crown_continue_stem(crown, stem, branch, station);
        const Axis& axis = crown.axes[crown.nodes[stem].axis];
        const auto& join = crown.continuations[0];
        CHECK(crown.nodes[stem].length == axis.length);
        CHECK(length_of(crown.nodes[stem].tip - axis.point.back()) < 1e-12);
        CHECK(length_of(crown.axes[branch].point.back() - child.point.back()) < 1e-12);
        CHECK(dot(axis.tangent.back(), axis_tangent(child, join.child_arc)) ==
              doctest::Approx(1).epsilon(1e-12));
        CHECK(length_of(axis_point(axis, station) - axis_point(original, station)) < 1e-12);
        CHECK(dot(axis_tangent(axis, station), k_up) == doctest::Approx(1).epsilon(1e-12));
        for (f64 s : {1.0, 5.0, 8.0, station}) {
          CHECK(length_of(axis_point(axis, s) - axis_point(original, s)) < 1e-12);
          CHECK(continuation_radius(radius, crown.nodes[stem].r1, axis.length, join, s) ==
                lerp(radius, .5 * radius, s / original.length));
        }
        f64 previous = parent_radius;
        for (u32 i = 1; i <= 100; ++i) {
          const f64 s = station + (axis.length - station) * static_cast<f64>(i) / 100;
          const f64 r = continuation_radius(radius, crown.nodes[stem].r1, axis.length, join, s);
          CHECK(r > 0);
          CHECK(r <= previous + 1e-12);
          previous = r;
          CHECK(std::isfinite(length_of(axis_point(axis, s))));
          CHECK(dot(axis_tangent(axis, s), direction + k_up) > 0);
        }
        for (u32 rows : {3u, 8u, 32u}) {
          assets::MeshData mesh;
          flora::Motion motion;
          Emitter emit{mesh, motion};
          emit.begin_part({}, k_up, axis.length, geometry::k_motion_no_parent);
          const Axis sampled = resample_continuation(axis, rows, join);
          build_tube(sampled, radius, crown.nodes[stem].r1, 8, {}, emit, true, &join);
          CHECK(mesh.indices.size() / 3 == tube_triangles(rows + k_continuation_rows, 8) - 2);
          CHECK(sampled.arc[rows] == station);
          const u32 last = rows + k_continuation_rows;
          for (u32 ring = 0; ring <= last; ++ring) {
            const D3 v = from_f32(mesh.positions[ring * 9]);
            const f64 expected = continuation_radius(radius, crown.nodes[stem].r1, axis.length,
                                                     join, sampled.arc[ring]);
            CHECK(length_of(v - sampled.point[ring]) == doctest::Approx(expected).epsilon(1e-3));
            const Vec3 n = mesh.normals[ring * 9];
            CHECK(std::isfinite(n.x + n.y + n.z));
            CHECK(dot(n, n) == doctest::Approx(1).epsilon(1e-5));
          }
          // A flat buried cap adds no point beyond the continuing branch's junction.
          CHECK(mesh.positions.size() == (last + 1) * 9);
          // The continuing child's reserved ring passes through exactly the same centre even
          // at its coarsest rung. Check actual polygons, including independently phased relief.
          for (u32 child_sides : {3u, 4u, 8u}) {
            assets::MeshData wood;
            flora::Motion wood_motion;
            Emitter bark{wood, wood_motion};
            bark.wood = WoodProfile{.35, .3, .12, .08, .12, 17};
            bark.begin_part({}, k_up, axis.length, geometry::k_motion_no_parent);
            build_tube(sampled, radius, crown.nodes[stem].r1, 8, {}, bark, true, &join);
            const u32 child_first = wood.positions.size();
            const Axis joined_child = resample_continuing_child(child, rows, join.child_arc);
            u32 reserved = 0;
            for (u32 r = 1; r < joined_child.arc.size(); ++r) {
              if (joined_child.arc[r] == join.child_arc) reserved = r;
              CHECK(joined_child.arc[r] - joined_child.arc[r - 1] <=
                    2.0 * child.length / (rows + 1));
              CHECK(length_of(joined_child.point[r] - axis_point(child, joined_child.arc[r])) <
                    1e-12);
            }
            REQUIRE(reserved != 0);
            CHECK(length_of(joined_child.point[reserved] - axis.point.back()) < 1e-12);
            bark.begin_part(child.point[0], child.tangent[0], child.length, 0);
            build_tube(joined_child, .4 * radius, .2 * radius, child_sides, {}, bark);
            CHECK((wood.indices.size() / 3) == tube_triangles(rows + k_continuation_rows, 8) - 2 +
                                                   tube_triangles(rows + 1, child_sides));
            const u32 child_ring = child_first + reserved * (child_sides + 1);
            bool inside = true;
            for (u32 j = 0; j < child_sides; ++j) {
              const D3 a = from_f32(wood.positions[child_ring + j]);
              const D3 b = from_f32(wood.positions[child_ring + (j + 1) % child_sides]);
              for (u32 v = 0; v < 8; ++v) {
                const D3 p = from_f32(wood.positions[last * 9 + v]);
                inside &= dot(cross(b - a, p - a), axis.tangent.back()) >= -1e-8;
              }
            }
            CHECK(inside);
            bind_continuation_cap(sampled, radius, crown.nodes[stem].r1, 8, 0, join, 0, 1,
                                  child.length, wood_motion);
            CHECK(wood_motion.parts[0].length == static_cast<f32>(sampled.arc[last - 1]));
            CHECK(wood_motion.vertex_weight[(last - 1) * 9] == 1.0f);
            CHECK(wood_motion.vertex_part[last * 9] == 1);
            CHECK(flora::encode_bend_weight(wood_motion.vertex_weight[last * 9]) ==
                  flora::encode_bend_weight(wood_motion.vertex_weight[child_ring]));
          }
        }
      }
    }
  }
}

TEST_CASE("flora maturity: continuation shoulder remains affordable at minimum budgets") {
  for (const char* species : {"broadleaf", "birch", "oak", "aspen", "alder"}) {
    flora::FloraFile source;
    source.species = species;
    source.tree.age_years = 5;
    source.tree.triangle_budget = 8000;
    source.tree.card_bake_px = 0;
    source.tree.impostor_px = 0;
    source.broadleaf.triangle_budget = 8000;
    source.birch.triangle_budget = 8000;
    source.broadleaf.card_bake_px = source.birch.card_bake_px = 0;
    source.broadleaf.impostor_px = source.birch.impostor_px = 0;
    assets::MeshData mesh;
    flora::Motion motion;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_flora(source, mesh, motion, nullptr, nullptr, error), error);
    valid_mesh(mesh, motion);
    CHECK(mesh.indices.size() / 3 <= 8000);
  }
}

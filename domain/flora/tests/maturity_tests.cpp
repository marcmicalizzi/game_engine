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

TEST_CASE("flora maturity: upper crowns share named support without a duplicate trunk-tip crown") {
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
    CHECK(top == 0);  // the upper crown is a funded named axis, not an extra trunk-tip crown
    u32 upper = 0;
    for (u32 i = 1; i < shoots.size(); ++i)
      if (shoots[i].parent == 0 && (shoots[i].flags & flora::k_crown_shoot_named) != 0) upper = i;
    REQUIRE(upper != 0);
    CHECK(shoots[upper].length < .25f * shoots[0].length);
    CHECK(shoots[upper].base_radius < shoots[0].base_radius);
    u32 upper_children = 0;
    for (const auto& shoot : shoots)
      upper_children += shoot.parent == upper ? 1u : 0u;
    CHECK(upper_children >= 2);
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
TEST_CASE("flora maturity: tapered terminal leader remains affordable at minimum budgets") {
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

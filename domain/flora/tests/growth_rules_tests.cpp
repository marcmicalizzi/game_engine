#include "../src/kit.h"

#include <core/hash/hash.h>
#include <domain/flora/flora.h>
#include <domain/flora/structure.h>
#include <domain/geometry/flora_species.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace engine;

namespace {
u64 shape_hash(const assets::MeshData& mesh) {
  return hash_combine(hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3)),
                      hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
}
}  // namespace

TEST_CASE("flora: maple blades have independent UV rectangles attached at opposite nodes") {
  using namespace flora::detail;
  Rng axis_rng(1);
  const Axis axis = grow_axis({}, {0, 0, 1}, .55, 1, k_up, 0, 0, axis_rng);
  assets::MeshData mesh;
  flora::Motion motion;
  Emitter emit{mesh, motion};
  Rng leaves_rng(13);
  build_opposite_leaves(axis, 5, .1, .08, 60 * k_degree, leaves_rng, emit);
  REQUIRE(mesh.positions.size() == 40);
  REQUIRE(mesh.indices.size() == 60);
  for (u32 leaf = 0; leaf < 10; ++leaf) {
    const u32 v = leaf * 4;
    for (u32 i = leaf * 6; i < leaf * 6 + 6; ++i) {
      CHECK(mesh.indices[i] >= v);
      CHECK(mesh.indices[i] < v + 4);  // no strip can join adjacent leaves
    }
    CHECK(mesh.uvs[v].x == 0);
    CHECK(mesh.uvs[v].y == 1);  // image bottom (stalk) attaches to the twig
    CHECK(mesh.uvs[v + 2].x == 1);
    CHECK(mesh.uvs[v + 2].y == 0);
    const Vec3 base = (mesh.positions[v] + mesh.positions[v + 1]) * .5f;
    const Vec3 tip = (mesh.positions[v + 2] + mesh.positions[v + 3]) * .5f;
    CHECK(std::abs(base.x) < 1e-6f);
    CHECK(std::abs(base.y) < 1e-6f);
    const u32 other = (leaf ^ 1u) * 4;
    const Vec3 paired = (mesh.positions[other] + mesh.positions[other + 1]) * .5f;
    CHECK(dot(base - paired, base - paired) < 1e-12f);
    const f32 length = std::sqrt(dot(tip - base, tip - base));
    const Vec3 width = mesh.positions[v + 1] - mesh.positions[v];
    CHECK(std::sqrt(dot(width, width)) / length == doctest::Approx(.8));
    CHECK(length >= .0899f);
    CHECK(length <= .1001f);
    CHECK(motion.vertex_weight[v] == motion.vertex_weight[v + 3]);
  }
  CHECK(std::abs(dot(mesh.normals[0], mesh.normals[8])) < .999f);
}

TEST_CASE("flora: reference maple retains terminal wood without squeezing leaf nodes") {
  flora::FloraFile file;
  file.species = "maple";
  file.tree.seed = 17;
  file.tree.card_bake_px = 0;
  file.tree.impostor_px = 0;
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  std::string error;
  REQUIRE_MESSAGE(flora::solve_flora(file, mesh, motion, nullptr, &report, error), error);
  CHECK(report.triangles <= 200000);
  CHECK(report.triangles == mesh.indices.size() / 3);
  CHECK(report.merge_level == 0);
  CHECK(report.wood_class == 0);
  CHECK(report.cards_per_side <= 6);
  const auto& leaves = mesh.primitives[1];
  for (u32 i = leaves.first_index; i < leaves.first_index + leaves.index_count; i += 6) {
    const u32 first = *std::min_element(mesh.indices.begin() + i, mesh.indices.begin() + i + 6);
    for (u32 k = i; k < i + 6; ++k)
      REQUIRE((mesh.indices[k] >= first && mesh.indices[k] < first + 4));
  }
}

TEST_CASE("flora: alternate blades occupy distinct twig nodes and compound leaves have rachises") {
  using namespace flora::detail;
  Rng rng(1);
  const Axis axis = grow_axis({}, {0, 0, 1}, .65, 1, k_up, 0, 0, rng);
  assets::MeshData mesh;
  flora::Motion motion;
  Emitter emit{mesh, motion};
  build_simple_leaves(axis, 4, .12, .06, 60 * k_degree, LeafPlacement::alternate_simple, rng, emit);
  REQUIRE(mesh.positions.size() == 32);
  f32 previous = -1;
  for (u32 leaf = 0; leaf < 8; ++leaf) {
    const u32 v = leaf * 4;
    const Vec3 base = (mesh.positions[v] + mesh.positions[v + 1]) * .5f;
    CHECK(base.z > previous);
    CHECK(std::abs(base.x) + std::abs(base.y) < 1e-6f);
    previous = base.z;
  }
  for (const LeafPlacement placement :
       {LeafPlacement::opposite_compound, LeafPlacement::alternate_compound}) {
    assets::MeshData compound;
    flora::Motion cm;
    Emitter ce{compound, cm};
    const bool ash = placement == LeafPlacement::opposite_compound;
    const u32 pairs = ash ? 3u : 8u;
    Rng stems(31), blades(31);
    build_compound_leaves(axis, pairs, .08, .025, 65 * k_degree, placement, stems, ce, true);
    const u32 first = compound.positions.size();
    const u32 stem_indices = compound.indices.size();
    build_compound_leaves(axis, pairs, .08, .025, 65 * k_degree, placement, blades, ce);
    CHECK(compound.indices.size() / 3 == compound_triangles(pairs, ash));
    CHECK((compound.indices.size() - stem_indices) / 6 == (ash ? 7u : 16u));
    CHECK(compound.positions.size() - first == (ash ? 28u : 64u));
    // One terminal unit is one whole leaf, with no extra woody branching tier.
    CHECK(cm.parts.empty());
    for (const f32 w : cm.vertex_weight)
      CHECK((w >= 0 && w <= 1));
  }
}

TEST_CASE("flora: green wood limits a named scaffold while legacy growth remains unchanged") {
  using namespace flora::detail;
  GrowthRules rules;
  CHECK(supported_length(100, .1, rules) == 100);
  rules.wood_rupture_mpa = 34;
  rules.wood_density = 561;
  const f64 length = supported_length(100, .1, rules);
  CHECK(2 * 561 * 9.81 * length * length / .1 == doctest::Approx(34e6 * .3));
  CHECK(supported_length(1, .1, rules) == 1);
  rules.wood_density *= 2;
  CHECK(supported_length(100, .1, rules) == doctest::Approx(length / std::sqrt(2.0)));
}

TEST_CASE("flora: named species share growth and preserve their skeleton across budgets") {
  const char* names[] = {"maple",  "oak",   "elm",     "walnut", "beech", "ash",     "pine",
                         "spruce", "cedar", "juniper", "aspen",  "fir",   "hemlock", "alder"};
  Vector<u64> shapes;
  for (const char* name : names) {
    CAPTURE(std::string_view(name));
    flora::FloraFile file;
    std::string error;
    REQUIRE_MESSAGE(flora::read_flora_file(
                        std::string("{\"species\":\"") + name +
                            "\",\"tree\":{\"height_m\":10,\"card_bake_px\":0,\"impostor_px\":0}}",
                        file, error),
                    error);
    assets::MeshData mesh;
    flora::Motion motion;
    flora::TreeReport report;
    REQUIRE_MESSAGE(flora::solve_flora(file, mesh, motion, nullptr, &report, error), error);
    CHECK(report.triangles <= geometry::flora_species_info(name)->triangle_budget);
    CHECK(report.triangles == mesh.indices.size() / 3u);
    CHECK(report.wood_triangles > 0);
    CHECK(report.leaf_triangles > 0);
    REQUIRE(motion.vertex_part.size() == mesh.positions.size());
    for (u32 v = 0; v < mesh.positions.size(); ++v) {
      const Vec3 p = mesh.positions[v];
      REQUIRE(std::isfinite(p.x + p.y + p.z));
      REQUIRE(motion.vertex_part[v] < motion.parts.size());
    }
    for (u32 p = 1; p < motion.parts.size(); ++p)
      REQUIRE(motion.parts[p].parent < p);
    for (u32 i = 0; i < mesh.indices.size(); i += 3) {
      const Vec3 a = mesh.positions[mesh.indices[i]];
      const Vec3 b = mesh.positions[mesh.indices[i + 1]];
      const Vec3 c = mesh.positions[mesh.indices[i + 2]];
      const Vec3 n = cross(b - a, c - a);
      REQUIRE(dot(n, n) > 0.0f);
    }
    const u64 hash = shape_hash(mesh);
    for (const u64 other : shapes)
      CHECK(hash != other);
    shapes.push_back(hash);
    assets::MeshData again;
    flora::Motion repeated;
    REQUIRE(flora::solve_flora(file, again, repeated, nullptr, nullptr, error));
    CHECK(shape_hash(again) == hash);
    file.tree.triangle_budget = 8000;
    REQUIRE_MESSAGE(flora::solve_flora(file, again, repeated, nullptr, &report, error), error);
    CHECK(report.triangles <= 8000);
    REQUIRE(repeated.parts.size() == motion.parts.size());
    CHECK(std::memcmp(repeated.parts.data(), motion.parts.data(),
                      motion.parts.size() * sizeof(geometry::ClusterFileMotionPart)) == 0);
    const std::string canonical = flora::canonical_flora_json(file);
    CHECK(canonical.find("\"tree\"") != std::string::npos);
    flora::FloraFile roundtrip;
    REQUIRE(flora::read_flora_file(canonical, roundtrip, error));
    CHECK(flora::flora_source_hash(file) == flora::flora_source_hash(roundtrip));
    const u64 low_hash = shape_hash(again);
    file.tree.seed += 1;
    REQUIRE(flora::solve_flora(file, again, repeated, nullptr, nullptr, error));
    CHECK(shape_hash(again) != low_hash);
  }
  const u64 combined = hash_bytes(shapes.data(), shapes.size() * sizeof(u64));
  // G86: 10800499079816480223 until maple gained terminal wood and independent leaf quads.
  // G87: alternate simple blades and compound leaves; maple's previous aggregate was
  // 16620326556857323731. G88 adds four profiles and bounds the local tip crown;
  // the prior ten-profile aggregate was 7127305501285355394.
  // G91 changes blade geometry only; the G88 aggregate was 6585964004099755467.
  CHECK_MESSAGE(combined == 6929553177682108527ull, "named profile geometry hash: " << combined);
}

TEST_CASE("flora: named species reject invalid input and keep distinct material roles") {
  flora::FloraFile file;
  std::string error;
  CHECK_FALSE(flora::read_flora_file("{\"species\":\"unknown_tree\"}", file, error));
  CHECK_FALSE(
      flora::read_flora_file("{\"species\":\"maple\",\"tree\":{\"crown_scale\":0}}", file, error));
  CHECK(error.find("crown_scale") != std::string::npos);
  file.species = "cedar";
  file.tree.height_m = std::numeric_limits<f32>::quiet_NaN();
  assets::MeshData mesh;
  flora::Motion motion;
  CHECK_FALSE(flora::solve_flora(file, mesh, motion, nullptr, nullptr, error));
  CHECK(flora::flora_role("cedar", 1) == "cedar/needles");
  CHECK(flora::flora_role("maple", 1) == "maple/leaf");
}

TEST_CASE("flora: every named species reference tree fits the default mesh budget") {
  for (const char* name : {"maple", "oak", "elm", "walnut", "beech", "ash", "pine", "spruce",
                           "cedar", "juniper", "aspen", "fir", "hemlock", "alder"}) {
    CAPTURE(std::string_view(name));
    flora::FloraFile file;
    file.species = name;
    assets::MeshData mesh;
    flora::Motion motion;
    flora::TreeReport report;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_flora(file, mesh, motion, nullptr, &report, error), error);
    CHECK(report.triangles <= geometry::flora_species_info(name)->triangle_budget);
    CHECK(report.leaf_triangles > 0);
    CHECK(report.merge_level == 0);  // reference trees retain their botanical leaf dimensions
    // Coverage floors for these authored reference crowns, not botanical LAI requirements.
    // G88 removes the redundant trunk-tip knot but preserves named scaffold pipes/lengths.
    // Matched alpha-textured whole/top views validate these retained-crown floors; do not
    // restore the old total LAI by crowding leaves onto the remaining shoots.
    const std::string_view species(name);
    const f32 minimum_lai = species == "ash"      ? .59f
                            : species == "walnut" ? 2.2f
                            : species == "oak"    ? 3.0f
                            : species == "elm"    ? 3.4f
                            : species == "beech"  ? 1.6f
                                                  : 0.0f;
    CHECK(report.leaf_area_index >= minimum_lai);
  }
}

TEST_CASE("flora: ancient scaffold expansion leaves tertiary and fine shoot lengths alone") {
  flora::Ancient p;
  p.habit = flora::AncientHabit::spreading;
  p.height_m = 40;
  p.trunk_radius_m = 3;
  p.buttress_height_m = 4;
  p.limbs = 3;
  p.limb_low_m = 12;
  p.limb_high_m = 18;
  p.limb_run_m = 6;
  p.limb_reach_m = 8;
  p.branch_length = .2f;
  // Short, strong limbs isolate the length controls from the separate mechanical cap.
  p.wood_density = 300;
  p.wood_rupture_mpa = 150;
  Vector<flora::CrownShoot> before, after;
  std::string error;
  REQUIRE_MESSAGE(flora::ancient_in_range(p, error), error);
  REQUIRE(flora::ancient_crown(p, before));
  p.primary_length_scale = 2;
  p.secondary_length_scale = 1.5f;
  REQUIRE(flora::ancient_crown(p, after));
  REQUIRE(before.size() == after.size());
  Vector<flora::AncientLimb> limbs;
  REQUIRE(flora::ancient_limbs(p, limbs));
  for (const auto& limb : limbs) {
    REQUIRE(limb.scale == 1.0f);
    CHECK(limb.peak_stress_mpa <= limb.allowable_mpa);
  }
  u32 primaries = 0, secondaries = 0;
  for (u32 i = 0; i < before.size(); ++i) {
    CAPTURE(i);
    const auto& a = before[i];
    const auto& b = after[i];
    CHECK(a.parent == b.parent);
    f32 expected = 1;
    if ((a.flags & flora::k_crown_shoot_named) != 0 && i > 2 && a.parent != 0) {
      // Named order: trunk, then each run, its renewal upright, and its curved fork system.
      const u32 parent = a.parent;
      if (before[parent].parent == 0 && i != parent + 1) {
        expected = 2;
        ++primaries;
      } else if (before[parent].parent != 0 && before[before[parent].parent].parent == 0 &&
                 parent != before[parent].parent + 1) {
        expected = 1.5f;
        ++secondaries;
      }
    }
    CHECK(b.length == doctest::Approx(a.length * expected).epsilon(1e-5));
  }
  CHECK(primaries == p.limbs);
  CHECK(secondaries >= 2 * p.limbs);
  p.limb_reach_m = 40;
  p.limb_radius_m = .5f;
  p.wood_density = 1300;
  p.wood_rupture_mpa = 20;
  REQUIRE(flora::ancient_limbs(p, limbs));
  for (const auto& limb : limbs) {
    CHECK(limb.scale < 1);
    CHECK(limb.peak_stress_mpa <= limb.allowable_mpa * 1.001f);
  }
  p.primary_length_scale = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(flora::ancient_in_range(p, error));
}

TEST_CASE("flora: supported wood has a collar and irregular sections without moving its axis") {
  using namespace flora::detail;
  Rng rng(1);
  const Axis axis = grow_axis({}, k_up, 6.0, 24, k_up, 0.0, 0.0, rng);
  assets::MeshData mesh;
  flora::Motion motion;
  Emitter emit{mesh, motion};
  emit.wood = WoodProfile{.4, .6, .2, .08, .1, 19};
  begin_structural_part(emit, {}, k_up, 6.0, geometry::k_motion_no_parent, .5, .2, 0u);
  build_tube(axis, .5, .2, 24, {}, emit);
  f32 smallest = 10.0f;
  f32 largest = 0.0f;
  for (u32 i = 0; i < 24; ++i) {
    const Vec3 p = mesh.positions[i];
    const f32 r = std::sqrt(p.x * p.x + p.z * p.z);
    smallest = std::min(smallest, r);
    largest = std::max(largest, r);
  }
  CHECK(smallest > .5f);            // the base flares
  CHECK(largest - smallest > .1f);  // fluting, not a scaled round tube
  CHECK(mesh.indices.size() / 3u == tube_triangles(24, 24));
  const u32 begin = mesh.positions.size();
  begin_structural_part(emit, {}, k_up, 6, 0, .5, .2, flora::k_straight);
  build_tube(axis, .5, .2, 24, {}, emit);
  for (u32 r = 0; r <= 24; ++r) {
    const f32 expected = .5f - .3f * static_cast<f32>(r) / 24.0f;
    for (u32 i = 0; i <= 24; ++i) {
      const Vec3 p = mesh.positions[begin + r * 25 + i];
      CHECK(std::sqrt(p.x * p.x + p.z * p.z) == doctest::Approx(expected).epsilon(1e-5));
    }
  }
}

TEST_CASE("flora: renewal stays beside the trunk on an explicitly reserved ancient run") {
  flora::Ancient p;
  p.limb_run_m = 24;
  p.limbs = 9;
  p.limb_low_m = 30;
  p.limb_high_m = 62;
  p.limb_radius_m = 1.8f;
  p.limb_reach_m = 36;
  p.triangle_budget = 100000;
  assets::MeshData mesh;
  flora::Motion motion;
  std::string error;
  REQUIRE_MESSAGE(flora::solve_ancient(p, mesh, nullptr, &error, &motion), error);
  for (u32 i = 0; i < p.limbs; ++i) {
    const u32 run_id = 1 + 2 * i;
    const auto& run = motion.parts[run_id];
    const auto& trunk = motion.parts[0];
    const f32 core = flora::structural_radius(motion.structure[0], trunk,
                                              dot(run.pivot - trunk.pivot, trunk.direction));
    if (i == 0) {
      CAPTURE(run.length);
      CHECK(run.length - (.5f * core + 2.5f) - 1.0f >= 8.0f);
    }
    u32 renewals = 0;
    for (u32 part = run_id + 1; part < motion.parts.size(); ++part) {
      const auto& child = motion.parts[part];
      if (child.parent != run_id) continue;
      const f32 along = dot(child.pivot - run.pivot, run.direction);
      if (along >= run.length - .01f) continue;  // the limb beyond the reserved run
      CHECK(along <= .5f * core + .251f);
      ++renewals;
    }
    CHECK(renewals == 1u);
  }
}

TEST_CASE("flora: a seated continuation buries its open rim without moving its structural axis") {
  using namespace flora::detail;
  for (const f64 angle : {0.0, 30.0, 65.0}) {
    Rng rng(31);
    const D3 direction{det::sin(angle * k_degree), det::cos(angle * k_degree), 0};
    const Axis axis = grow_axis({0, 6, 0}, direction, 4, 24, direction, 0, 0, rng);
    assets::MeshData mesh;
    flora::Motion motion;
    Emitter emit{mesh, motion};
    begin_structural_part(emit, axis.point[0], direction, 4, 0, .8, .3, 0);
    emit.seated = true;
    emit.seat_origin = {0, 5.25, 0};
    emit.seat_radius = .36;
    build_tube(axis, .8, .3, 24, {}, emit);
    for (u32 i = 0; i <= 24; ++i) {
      const Vec3 p = mesh.positions[i];
      CHECK(p.x * p.x + p.z * p.z < 1.0f);  // inside the parent of radius 1
      CHECK(p.y < 6.0f);                    // below its end plane
    }
    CHECK(motion.parts[0].pivot.y == 6.0f);
    CHECK(motion.parts[0].length == 4.0f);
    CHECK(mesh.indices.size() / 3 == tube_triangles(24, 24));
    for (const Vec3 normal : mesh.normals) {
      CHECK(std::isfinite(normal.x));
      CHECK(std::isfinite(normal.y));
      CHECK(std::isfinite(normal.z));
      CHECK(length(normal) == doctest::Approx(1.0f).epsilon(.001));
    }
  }
}

TEST_CASE("flora: blade dimensions preserve allocation wood and attachment stations") {
  using namespace flora::detail;
  for (const u32 budget : {200000u, 8000u}) {
    flora::Broadleaf params;
    params.seed = 17;
    params.leaf_length_m = .1f;
    params.leaf_width = .8f;
    params.triangle_budget = budget;
    params.card_bake_px = 0;
    params.impostor_px = 0;
    GrowthRules rules;
    rules.leaf_placement = LeafPlacement::opposite_simple;
    assets::MeshData before, after;
    flora::Motion bm, am;
    flora::TreeReport br, ar;
    std::string error;
    REQUIRE_MESSAGE(solve_broadleaf_rules(params, rules, before, &br, &error, &bm, nullptr), error);
    rules.leaf_length_scale = 1.5;
    rules.leaf_width_scale = 1.875;
    REQUIRE_MESSAGE(solve_broadleaf_rules(params, rules, after, &ar, &error, &am, nullptr), error);
    CHECK(br.leaf_cards == ar.leaf_cards);
    CHECK(br.triangles == ar.triangles);
    CHECK(br.merge_level == ar.merge_level);
    CHECK(br.steps == ar.steps);
    REQUIRE(before.positions.size() == after.positions.size());
    REQUIRE(before.indices == after.indices);
    REQUIRE(bm.vertex_part == am.vertex_part);
    REQUIRE(bm.vertex_weight == am.vertex_weight);
    const auto& bark = before.primitives[0];
    for (u32 i = bark.first_index; i < bark.first_index + bark.index_count; ++i) {
      const u32 v = before.indices[i];
      REQUIRE(std::memcmp(&before.positions[v], &after.positions[v], sizeof(Vec3)) == 0);
    }
    const auto& leaves = before.primitives[1];
    for (u32 i = leaves.first_index; i < leaves.first_index + leaves.index_count; i += 6) {
      const u32 v = *std::min_element(before.indices.begin() + i, before.indices.begin() + i + 6);
      const Vec3 b0 = (before.positions[v] + before.positions[v + 1]) * .5f;
      const Vec3 a0 = (after.positions[v] + after.positions[v + 1]) * .5f;
      const Vec3 bt = (before.positions[v + 2] + before.positions[v + 3]) * .5f;
      const Vec3 at = (after.positions[v + 2] + after.positions[v + 3]) * .5f;
      CHECK(dot(a0 - b0, a0 - b0) < 1e-11f);
      CHECK(std::sqrt(dot(at - a0, at - a0) / dot(bt - b0, bt - b0)) ==
            doctest::Approx(1.5).epsilon(.001));
      const Vec3 bw = before.positions[v + 1] - before.positions[v];
      const Vec3 aw = after.positions[v + 1] - after.positions[v];
      CHECK(std::sqrt(dot(aw, aw) / dot(bw, bw)) == doctest::Approx(1.875).epsilon(.001));
    }
    CHECK(ar.leaf_area_m2 / br.leaf_area_m2 == doctest::Approx(2.8125).epsilon(.001));
  }
}

TEST_CASE("flora: named broadleaf blades retain small trees and reach botanical envelopes") {
  struct Expected {
    const char* name;
    float sapling_length, sapling_width, adult_length, adult_width;
  };
  const Expected species[] = {
      {"maple", .10f, .08f, .15f, .15f},       {"oak", .12f, .054f, .20f, .10f},
      {"elm", .09f, .0405f, .13f, .065f},      {"beech", .09f, .0405f, .12f, .06f},
      {"walnut", .08f, .024f, .12f, .05f},     {"ash", .085f, .0255f, .12f, .05f},
      {"aspen", .065f, .05525f, .075f, .065f}, {"alder", .10f, .065f, .13f, .085f}};
  for (const auto& s : species) {
    for (const float height : {2.0f, 8.0f, 16.0f}) {
      CAPTURE(std::string(s.name));
      CAPTURE(height);
      flora::FloraFile f;
      f.species = s.name;
      f.tree.height_m = height;
      f.tree.seed = 17;
      f.tree.card_bake_px = 0;
      f.tree.impostor_px = 0;
      assets::MeshData mesh;
      flora::Motion motion;
      flora::TreeReport report;
      std::string error;
      REQUIRE_MESSAGE(flora::solve_flora(f, mesh, motion, nullptr, &report, error), error);
      REQUIRE(report.merge_level == 0);
      const float expected_length = height <= 3 ? s.sapling_length : s.adult_length;
      const float expected_width = height <= 3 ? s.sapling_width : s.adult_width;
      const auto found = std::find_if(mesh.primitives.begin(), mesh.primitives.end(),
                                      [](const assets::Primitive& p) { return p.material == 1; });
      REQUIRE(found != mesh.primitives.end());
      const auto& leaves = *found;
      REQUIRE(leaves.index_count > 0);
      for (u32 i = leaves.first_index; i < leaves.first_index + leaves.index_count; i += 6) {
        const u32 v = *std::min_element(mesh.indices.begin() + i, mesh.indices.begin() + i + 6);
        const Vec3 base = (mesh.positions[v] + mesh.positions[v + 1]) * .5f;
        const Vec3 tip = (mesh.positions[v + 2] + mesh.positions[v + 3]) * .5f;
        const Vec3 across = mesh.positions[v + 1] - mesh.positions[v];
        const float length = std::sqrt(dot(tip - base, tip - base));
        const float width = std::sqrt(dot(across, across));
        REQUIRE(length >= .899f * expected_length);
        REQUIRE(length <= 1.001f * expected_length);
        REQUIRE(width / length == doctest::Approx(expected_width / expected_length).epsilon(.001));
      }
    }
  }
}

TEST_CASE("flora: added temperate profiles retain maturity and deterministic LOD skeletons") {
  Vector<u64> hashes;
  for (const char* species : {"basswood", "black_cherry", "sycamore"}) {
    CAPTURE(species);
    flora::FloraFile f;
    f.species = species;
    f.tree.seed = 73;
    f.tree.card_bake_px = 0;
    f.tree.impostor_px = 0;
    std::string error;
    f.tree.age_years = 2;
    assets::MeshData young, adult, repeated;
    flora::Motion ym, am, rm;
    flora::TreeReport report;
    REQUIRE_MESSAGE(flora::solve_flora(f, young, ym, nullptr, &report, error), error);
    f.tree.age_years = 200;
    REQUIRE_MESSAGE(flora::solve_flora(f, adult, am, nullptr, &report, error), error);
    CHECK(report.triangles <= geometry::flora_species_info(species)->triangle_budget);
    CHECK(report.leaf_triangles > 0);
    CHECK(adult.indices.size() > young.indices.size());
    REQUIRE(flora::solve_flora(f, repeated, rm, nullptr, nullptr, error));
    const u64 hash = shape_hash(adult);
    CHECK(shape_hash(repeated) == hash);
    for (const u64 other : hashes)
      CHECK(hash != other);
    hashes.push_back(hash);
    flora::FloraFile read;
    REQUIRE(flora::read_flora_file(flora::canonical_flora_json(f), read, error));
    CHECK(flora::flora_source_hash(read) == flora::flora_source_hash(f));
    f.tree.triangle_budget = 8000;
    REQUIRE_MESSAGE(flora::solve_flora(f, repeated, rm, nullptr, &report, error), error);
    CHECK(report.triangles <= 8000);
    CHECK(rm.parts.size() == am.parts.size());
  }
}

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
  const char* names[] = {"maple", "oak",  "elm",    "walnut", "beech",
                         "ash",   "pine", "spruce", "cedar",  "juniper"};
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
  CHECK_MESSAGE(combined == 16620326556857323731ull, "named profile geometry hash: " << combined);
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
  for (const char* name :
       {"maple", "oak", "elm", "walnut", "beech", "ash", "pine", "spruce", "cedar", "juniper"}) {
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
  }
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

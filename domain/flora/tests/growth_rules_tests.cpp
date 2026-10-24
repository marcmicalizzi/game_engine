#include "../src/kit.h"

#include <core/hash/hash.h>
#include <domain/flora/flora.h>
#include <domain/flora/structure.h>

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
    CHECK(report.triangles <= file.tree.triangle_budget);
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
  CHECK_MESSAGE(combined == 7516634195967486933ull, "named profile geometry hash: " << combined);
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
    CHECK(report.triangles <= file.tree.triangle_budget);
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

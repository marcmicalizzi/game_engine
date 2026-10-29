#include <core/hash/hash.h>
#include <domain/flora/flora.h>
#include <domain/flora/shrubs.h>
#include <domain/flora/structure.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace engine;
namespace {
u64 shrub_hash(const assets::MeshData& mesh, const flora::Motion& motion) {
  u64 hash = hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
  hash = hash_combine(hash, hash_bytes(mesh.normals.data(), mesh.normals.size() * sizeof(Vec3)));
  hash = hash_combine(hash, hash_bytes(mesh.uvs.data(), mesh.uvs.size() * sizeof(Vec2)));
  hash = hash_combine(hash, hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
  hash = hash_combine(
      hash, hash_bytes(motion.parts.data(), motion.parts.size() * sizeof(flora::MotionPart)));
  hash = hash_combine(hash, hash_bytes(motion.structure.data(),
                                       motion.structure.size() * sizeof(flora::StructuralPart)));
  hash = hash_combine(
      hash, hash_bytes(motion.vertex_part.data(), motion.vertex_part.size() * sizeof(u32)));
  return hash_combine(
      hash, hash_bytes(motion.vertex_weight.data(), motion.vertex_weight.size() * sizeof(f32)));
}
}  // namespace

TEST_CASE("shrubs: researched dimensions, distinct forms, budgets and deterministic streams") {
  u64 combined = 0;
  for (const auto& profile : flora::shrub_profiles()) {
    const auto& catalogue = geometry::flora_species_info(profile.species);
    CAPTURE(catalogue.name);
    flora::ShrubGrowth p;
    assets::MeshData mesh;
    flora::Motion motion;
    flora::TreeReport report;
    std::string error;
    REQUIRE_MESSAGE(flora::solve_shrub_growth(catalogue.name, p, mesh, motion, &report, error),
                    error);
    CHECK(report.triangles <= catalogue.triangle_budget);
    CHECK(report.triangles == mesh.indices.size() / 3);
    CHECK(report.leaf_cards * 2 == report.leaf_triangles);
    REQUIRE(mesh.primitives.size() == 2);
    REQUIRE(motion.parts.size() == motion.structure.size());
    CHECK(motion.parts[0].parent == geometry::k_motion_no_parent);
    u32 stems = 0;
    for (u32 i = 1; i < motion.parts.size(); ++i) {
      REQUIRE(motion.parts[i].parent < i);
      const bool stem = motion.parts[i].parent == 0;
      stems += stem ? 1u : 0u;
      CHECK(((motion.structure[i].flags & flora::k_collider) != 0) ==
            (stem && motion.structure[i].base_radius >= flora::k_shrub_collider_radius_m));
    }
    CHECK(stems == profile.stems * profile.colonies);
    CHECK((report.colliders != 0) == flora::shrub_growth_collides(catalogue.name, p));
    for (u32 index : mesh.indices)
      REQUIRE(index < mesh.positions.size());
    for (const Vec3 v : mesh.positions) {
      REQUIRE(std::isfinite(v.x));
      REQUIRE(std::isfinite(v.y));
      REQUIRE(std::isfinite(v.z));
    }
    // The actual diamond's base-to-tip length, not just a reported metadata number.
    for (u32 i = mesh.primitives[1].first_index; i < mesh.indices.size(); i += 6) {
      f32 diameter = 0;
      for (u32 a = 0; a < 6; ++a)
        for (u32 b = 0; b < 6; ++b) {
          const Vec3 d = mesh.positions[mesh.indices[i + a]] - mesh.positions[mesh.indices[i + b]];
          diameter = std::max(diameter, std::sqrt(dot(d, d)));
        }
      CHECK(diameter >= profile.leaf_min_m - 1e-6f);
      CHECK(diameter <= profile.leaf_max_m + 1e-6f);
    }
    assets::MeshData repeat;
    flora::Motion repeated;
    REQUIRE(flora::solve_shrub_growth(catalogue.name, p, repeat, repeated, nullptr, error));
    CHECK(shrub_hash(mesh, motion) == shrub_hash(repeat, repeated));
    combined = hash_combine(combined, shrub_hash(mesh, motion));
    p.seed = 2;
    REQUIRE(flora::solve_shrub_growth(catalogue.name, p, repeat, repeated, nullptr, error));
    CHECK(shrub_hash(mesh, motion) != shrub_hash(repeat, repeated));
    MESSAGE(catalogue.name << ": triangles=" << report.triangles
                           << " wood=" << report.wood_triangles
                           << " leaves=" << report.leaf_triangles << " parts=" << report.parts);
  }
  // First MSVC pin for G107; Linux must reproduce it before the branch is handed over.
  CHECK_MESSAGE(combined == 0x7bb6204d0b5e55c4ull, "shrub section hash=" << combined);
}

TEST_CASE("shrubs: budget changes tessellation without moving structure or enlarging leaves") {
  for (const auto& profile : flora::shrub_profiles()) {
    const auto name = geometry::flora_species_info(profile.species).name;
    CAPTURE(name);
    flora::ShrubGrowth p;
    assets::MeshData mesh, coarse;
    flora::Motion motion, reduced;
    flora::TreeReport report, low;
    std::string error;
    REQUIRE(flora::solve_shrub_growth(name, p, mesh, motion, &report, error));
    p.triangle_budget = std::max(4000u, report.triangles * 4 / 5);
    REQUIRE_MESSAGE(flora::solve_shrub_growth(name, p, coarse, reduced, &low, error), error);
    CHECK(low.steps > 0);
    CHECK(low.triangles <= p.triangle_budget);
    CHECK(low.leaf_cards == report.leaf_cards);
    REQUIRE(reduced.parts.size() == motion.parts.size());
    CHECK(std::memcmp(reduced.parts.data(), motion.parts.data(),
                      motion.parts.size() * sizeof(flora::MotionPart)) == 0);
    CHECK(std::memcmp(reduced.structure.data(), motion.structure.data(),
                      motion.structure.size() * sizeof(flora::StructuralPart)) == 0);
    const auto& a = mesh.primitives[1];
    const auto& b = coarse.primitives[1];
    REQUIRE(a.index_count == b.index_count);
    for (u32 i = 0; i < a.index_count; ++i) {
      const Vec3 x = mesh.positions[mesh.indices[a.first_index + i]];
      const Vec3 y = coarse.positions[coarse.indices[b.first_index + i]];
      CHECK(std::memcmp(&x, &y, sizeof(Vec3)) == 0);
    }
  }
}

TEST_CASE("shrubs: live and dead stems coexist; all-dead output stays woody") {
  flora::ShrubGrowth p;
  p.stems = 10;
  p.dead_fraction = .3f;
  p.triangle_budget = 100000;
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  std::string error;
  REQUIRE(flora::solve_shrub_growth("blackberry", p, mesh, motion, &report, error));
  Vector<u8> leafy(motion.parts.size(), 0);
  for (u32 i = mesh.primitives[1].first_index; i < mesh.indices.size(); ++i) {
    u32 part = motion.vertex_part[mesh.indices[i]];
    while (motion.parts[part].parent != 0)
      part = motion.parts[part].parent;
    leafy[part] = 1;
  }
  u32 living = 0;
  for (u8 yes : leafy)
    living += yes;
  CHECK(living == 7);
  p.dead_fraction = 1;
  flora::TreeLevels levels;
  REQUIRE(flora::solve_shrub_growth("blackberry", p, mesh, motion, &report, error, &levels));
  CHECK(report.leaf_triangles == 0);
  CHECK(report.wood_triangles > 0);
  CHECK(mesh.primitives.size() == 1);
  CHECK(levels.levels.empty());
}

TEST_CASE("shrubs: height overrides keep blades inside the researched size envelope") {
  for (const auto& profile : flora::shrub_profiles()) {
    const auto name = geometry::flora_species_info(profile.species).name;
    for (const f32 height : {profile.height_min_m, profile.height_max_m}) {
      CAPTURE(name);
      CAPTURE(height);
      flora::ShrubGrowth p;
      p.height_m = height;
      assets::MeshData mesh;
      flora::Motion motion;
      std::string error;
      REQUIRE_MESSAGE(flora::solve_shrub_growth(name, p, mesh, motion, nullptr, error), error);
      REQUIRE(mesh.primitives.size() == 2);
      for (u32 i = mesh.primitives[1].first_index; i < mesh.indices.size(); i += 6) {
        f32 diameter = 0;
        for (u32 a = 0; a < 6; ++a)
          for (u32 b = 0; b < 6; ++b) {
            const Vec3 d =
                mesh.positions[mesh.indices[i + a]] - mesh.positions[mesh.indices[i + b]];
            diameter = std::max(diameter, std::sqrt(dot(d, d)));
          }
        CHECK(diameter >= profile.leaf_min_m - 1e-6f);
        CHECK(diameter <= profile.leaf_max_m + 1e-6f);
      }
    }
  }
}

TEST_CASE("shrubs: planting plane and botanical stature are represented in geometry") {
  for (const auto& profile : flora::shrub_profiles()) {
    const auto name = geometry::flora_species_info(profile.species).name;
    CAPTURE(name);
    flora::ShrubGrowth p;
    p.slope_x = .3f;
    p.slope_z = -.2f;
    assets::MeshData mesh;
    flora::Motion motion;
    std::string error;
    REQUIRE(flora::solve_shrub_growth(name, p, mesh, motion, nullptr, error));
    f32 max_height = 0;
    for (const auto v : mesh.positions)
      max_height = std::max(max_height, v.y - p.slope_x * v.x - p.slope_z * v.z);
    if (profile.form == flora::ShrubForm::spreading)
      CHECK(max_height < .30f);
    else
      CHECK(max_height < profile.height_m * 1.5f);
    for (u32 i = 1; i < motion.parts.size(); ++i) {
      if (motion.parts[i].parent != 0) continue;
      const auto v = motion.parts[i].pivot;
      CHECK(std::abs(v.y - p.slope_x * v.x - p.slope_z * v.z) < 1e-6f);
    }
  }
}

TEST_CASE("shrubs: schema canonical identity, refusals and baked distance container") {
  std::string error;
  flora::FloraFile file, read;
  REQUIRE(
      flora::read_flora_file(R"({"species":"bearberry","shrub_growth":{"seed":17}})", file, error));
  const auto hash = flora::flora_source_hash(file);
  REQUIRE(flora::read_flora_file(flora::canonical_flora_json(file), read, error));
  CHECK(flora::flora_source_hash(read) == hash);
  file.shrub.height_m = 6;
  CHECK(flora::flora_source_hash(file) == hash);
  file.shrub_growth.seed++;
  CHECK(flora::flora_source_hash(file) != hash);
  for (f32 invalid : {-1.f, 7.f, std::numeric_limits<f32>::quiet_NaN()}) {
    file.shrub_growth.height_m = invalid;
    CHECK_FALSE(flora::shrub_growth_in_range(file.species, file.shrub_growth, error));
    CHECK(error.find("height_m") != std::string::npos);
  }
  flora::ShrubGrowth p;
  p.dead_fraction = 1.1f;
  CHECK_FALSE(flora::shrub_growth_in_range("blackberry", p, error));
  p = {};
  p.detail_tier = flora::DetailTier::rich;
  CHECK_FALSE(flora::shrub_growth_in_range("beaked_hazel", p, error));
  CHECK(flora::shrub_growth_in_range("bearberry", p, error));
  geometry::ClusterFileData container;
  geometry::ClusterLodOptions lod;
  REQUIRE_MESSAGE(flora::build_flora_container(read, lod, 0, container, nullptr, error), error);
  CHECK(container.generator_kind == "bearberry");
  CHECK(!container.impostors.empty());
  CHECK(!container.motion_parts.empty());
  CHECK(!container.motion_vertices.empty());
}

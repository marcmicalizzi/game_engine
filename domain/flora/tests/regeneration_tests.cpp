#include <core/hash/hash.h>
#include <domain/assets/mesh_generator.h>
#include <domain/flora/flora.h>
#include <domain/flora/regeneration.h>
#include <domain/flora/structure.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace engine;

namespace {
u64 shape(const assets::MeshData& m) {
  return hash_combine(hash_bytes(m.positions.data(), m.positions.size() * sizeof(Vec3)),
                      hash_bytes(m.indices.data(), m.indices.size() * sizeof(u32)));
}
void valid(const assets::MeshData& mesh, const flora::Motion& motion) {
  REQUIRE(mesh.positions.size() == motion.vertex_part.size());
  REQUIRE(motion.parts.size() == motion.structure.size());
  CHECK(motion.parts[0].parent == geometry::k_motion_no_parent);
  for (u32 i = 1; i < motion.parts.size(); ++i) {
    CHECK(motion.parts[i].parent < i);
    CHECK(motion.parts[i].length > 0);
  }
  for (u32 i = 0; i < mesh.positions.size(); ++i) {
    CHECK(std::isfinite(mesh.positions[i].x));
    CHECK(std::isfinite(mesh.positions[i].y));
    CHECK(std::isfinite(mesh.positions[i].z));
    CHECK(motion.vertex_part[i] < motion.parts.size());
    CHECK(motion.vertex_weight[i] >= 0);
    CHECK(motion.vertex_weight[i] <= 1);
  }
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    REQUIRE(mesh.indices[i] < mesh.positions.size());
    REQUIRE(mesh.indices[i + 1] < mesh.positions.size());
    REQUIRE(mesh.indices[i + 2] < mesh.positions.size());
    const Vec3 area = cross(mesh.positions[mesh.indices[i + 1]] - mesh.positions[mesh.indices[i]],
                            mesh.positions[mesh.indices[i + 2]] - mesh.positions[mesh.indices[i]]);
    CHECK(dot(area, area) > 0);
  }
}
}  // namespace

TEST_CASE("regeneration: species, groups and damage build deterministic valid juveniles") {
  std::string error;
  for (const auto& profile : flora::regeneration_profiles()) {
    const auto species = geometry::flora_species_info(profile.species).name;
    for (const f32 height : {.15f, .6f, 1.25f, 2.0f, 4.0f}) {
      flora::Regeneration p;
      p.height_m = height;
      p.triangle_budget = 200000;
      p.impostor_px = 0;
      p.plants = 3;
      p.group_radius_m = 1;
      p.leader_loss = .3f;
      p.recovery = .7f;
      assets::MeshData mesh, again;
      flora::Motion motion, other;
      flora::TreeReport report;
      REQUIRE_MESSAGE(flora::solve_regeneration(species, p, mesh, motion, &report, error), error);
      valid(mesh, motion);
      CHECK(report.triangles == mesh.indices.size() / 3);
      CHECK(report.triangles <= p.triangle_budget);
      CHECK(report.stems == p.plants * profile.basal_stems);
      CHECK(flora::regeneration_collides(species, p) == (report.colliders > 0));
      REQUIRE(flora::solve_regeneration(species, p, again, other, nullptr, error));
      CHECK(shape(mesh) == shape(again));
      CHECK(std::memcmp(motion.parts.data(), other.parts.data(),
                        motion.parts.size() * sizeof(flora::MotionPart)) == 0);
      ++p.seed;
      REQUIRE(flora::solve_regeneration(species, p, again, other, nullptr, error));
      CHECK(shape(mesh) != shape(again));
    }
  }
}

TEST_CASE("regeneration: budget changes tube detail and preserves every structural part") {
  flora::Regeneration p;
  p.height_m = 2;
  p.impostor_px = 0;
  p.triangle_budget = 200000;
  assets::MeshData full, low;
  flora::Motion a, b;
  flora::TreeReport first, last;
  std::string error;
  REQUIRE(flora::solve_regeneration("maple", p, full, a, &first, error));
  p.triangle_budget = first.triangles * 3 / 4;
  REQUIRE_MESSAGE(flora::solve_regeneration("maple", p, low, b, &last, error), error);
  CHECK(last.steps > 0);
  CHECK(last.leaf_cards == first.leaf_cards);
  REQUIRE(a.parts.size() == b.parts.size());
  CHECK(std::memcmp(a.parts.data(), b.parts.data(), a.parts.size() * sizeof(flora::MotionPart)) ==
        0);
  CHECK(std::memcmp(a.structure.data(), b.structure.data(),
                    a.structure.size() * sizeof(flora::StructuralPart)) == 0);
  p.triangle_budget = 256;
  CHECK_FALSE(flora::solve_regeneration("maple", p, low, b, &last, error));
  CHECK(error.find("needs") != std::string::npos);
}

TEST_CASE("regeneration: juvenile blades retain species dimensions across height and suppression") {
  std::string error;
  for (const auto& profile : flora::regeneration_profiles()) {
    for (const f32 height : {.2f, 3.0f}) {
      flora::Regeneration p;
      p.height_m = height;
      p.suppression = .6f;
      p.impostor_px = 0;
      p.triangle_budget = 200000;
      assets::MeshData mesh;
      flora::Motion motion;
      flora::TreeReport report;
      REQUIRE(flora::solve_regeneration(geometry::flora_species_info(profile.species).name, p, mesh,
                                        motion, &report, error));
      REQUIRE(report.leaf_cards > 0);
      const u32 first = mesh.positions.size() - report.leaf_cards * 4;
      for (u32 v = first; v < mesh.positions.size(); v += 4) {
        // Undo the prescribed 4% tip droop before measuring a blade's reference length.
        const Vec3 width = mesh.positions[v + 2] - mesh.positions[v + 1];
        const f32 w = std::sqrt(dot(width, width));
        CHECK(w >= profile.leaf_width_m * .899f);
        CHECK(w <= profile.leaf_width_m * 1.001f);
        const f32 expected = w * profile.leaf_length_m / profile.leaf_width_m;
        Vec3 along = mesh.positions[v + 3] - mesh.positions[v];
        along.y += expected * .04f;
        // Width-derived length magnifies coordinate roundoff on sub-millimetre needles.
        CHECK(std::sqrt(dot(along, along)) ==
              doctest::Approx(expected).epsilon(profile.evergreen ? .003 : .0002));
      }
    }
  }
}

TEST_CASE("regeneration: suppression, loss and browsing have distinct structural effects") {
  flora::Regeneration p;
  p.height_m = 2;
  p.impostor_px = 0;
  assets::MeshData mesh;
  flora::Motion healthy, suppressed, broken, recovered, browsed;
  flora::TreeReport before, after;
  std::string error;
  REQUIRE(flora::solve_regeneration("maple", p, mesh, healthy, &before, error));
  p.suppression = 1;
  REQUIRE(flora::solve_regeneration("maple", p, mesh, suppressed, nullptr, error));
  CHECK(suppressed.parts[1].length < healthy.parts[1].length * .5f);
  p.suppression = 0;
  p.leader_loss = .4f;
  REQUIRE(flora::solve_regeneration("maple", p, mesh, broken, nullptr, error));
  CHECK(broken.parts[1].length == doctest::Approx(healthy.parts[1].length * .6f));
  p.recovery = 1;
  REQUIRE(flora::solve_regeneration("maple", p, mesh, recovered, nullptr, error));
  CHECK(recovered.parts.size() == broken.parts.size() + 2);
  p.leader_loss = p.recovery = 0;
  p.browse_fraction = 1;
  p.browse_height_m = 4;
  REQUIRE(flora::solve_regeneration("maple", p, mesh, browsed, &after, error));
  CHECK(after.leaf_cards == 0);
  CHECK(browsed.parts[1].length == healthy.parts[1].length);
  REQUIRE(browsed.parts.size() == healthy.parts.size());
  for (u32 i = 2; i < browsed.parts.size(); ++i)
    CHECK(browsed.parts[i].length < healthy.parts[i].length * .5f);
}

TEST_CASE("regeneration: obstacles bend a stem and exclude foliage without moving its root") {
  flora::Regeneration p;
  p.height_m = .4f;  // a seedling, leaves directly on the main shoot
  p.impostor_px = 0;
  p.obstacles.push_back({0, .23f, 0, .08f});
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  std::string error;
  REQUIRE_MESSAGE(flora::solve_regeneration("oak", p, mesh, motion, &report, error), error);
  CHECK(report.leaf_cards > 0);
  CHECK(motion.parts[1].pivot.x == 0);
  CHECK(motion.parts[1].pivot.z == 0);
  const Vec3 centre{0, .23f, 0};
  f32 reach = 0;
  for (const Vec3 v : mesh.positions) {
    const Vec3 d = v - centre;
    CHECK(dot(d, d) > .08f * .08f);
    reach = std::max(reach, std::sqrt(v.x * v.x + v.z * v.z));
  }
  CHECK(reach > .08f);
  p.obstacles[0].y_m = 0;
  CHECK_FALSE(flora::solve_regeneration("oak", p, mesh, motion, nullptr, error));
  CHECK(error.find("attachment") != std::string::npos);
}

TEST_CASE("regeneration: recipe identity is opt-in, validates inputs and cooks a far form") {
  flora::FloraFile file;
  std::string error;
  REQUIRE(flora::read_flora_file(R"({"species":"maple"})", file, error));
  CHECK(flora::canonical_flora_json(file).find("regeneration") == std::string::npos);
  REQUIRE(flora::read_flora_file(
      R"({"species":"maple","regeneration":{"height_m":0.4,"plants":3,"group_radius_m":0.4}})",
      file, error));
  const std::string canonical = flora::canonical_flora_json(file);
  CHECK(canonical.find("\"tree\"") == std::string::npos);
  flora::FloraFile back;
  REQUIRE(flora::read_flora_file(canonical, back, error));
  CHECK(flora::flora_source_hash(back) == flora::flora_source_hash(file));
  geometry::ClusterFileData container;
  REQUIRE_MESSAGE(flora::build_flora_container(file, {}, 65536, container, nullptr, error), error);
  CHECK(container.impostors.size() == 1);
  CHECK(container.motion_parts.size() == container.structural_parts.size());
  CHECK_FALSE(flora::flora_collides(file));
  for (
      const char* text :
      {R"({"species":"date_palm","regeneration":{}})",
       R"({"species":"maple","regeneration":{"plants":0}})",
       R"({"species":"maple","regeneration":{"plants":2}})",
       R"({"species":"maple","regeneration":{"height_m":0.01}})",
       R"({"species":"maple","regeneration":{"leader_loss":1}})",
       R"({"species":"maple","regeneration":{"recovery":1}})",
       R"({"species":"pine","regeneration":{"recovery":1,"browse_fraction":1,"browse_height_m":1}})",
       R"({"species":"maple","regeneration":{"browse_fraction":1}})",
       R"({"species":"maple","regeneration":{"obstacles":[{"radius_m":-1}]}})"})
    CHECK_FALSE(flora::read_flora_file(text, back, error));
  auto p = *file.regeneration;
  p.suppression = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(flora::regeneration_in_range("maple", p, error));
}

TEST_CASE("regeneration: far bakes key external leaf bytes for trees and shrubs") {
  flora::FloraFile file;
  std::string error;
  REQUIRE(flora::read_flora_file(
      R"({"species":"maple","regeneration":{},"materials":{"leaf":{"base_color":"leaf.png","tile_m":1}}})",
      file, error));
  u8 byte = 1;
  assets::MeshGeneratorHosts hosts;
  hosts.context = &byte;
  hosts.read_image = [](void* context, std::string_view, Vector<u8>& bytes, std::string&) {
    bytes.push_back(*static_cast<u8*>(context));
    return true;
  };
  for (const auto& profile : flora::regeneration_profiles()) {
    file.species = geometry::flora_species_info(profile.species).name;
    file.regeneration->impostor_px = 128;
    u64 a = 0, b = 0;
    byte = 1;
    REQUIRE(flora::flora_source_hash_with(file, {nullptr, &hosts}, a, error));
    byte = 2;
    REQUIRE(flora::flora_source_hash_with(file, {nullptr, &hosts}, b, error));
    CHECK(a != b);
    file.regeneration->impostor_px = 0;
    REQUIRE(flora::flora_source_hash_with(file, {nullptr, &hosts}, a, error));
    byte = 3;
    REQUIRE(flora::flora_source_hash_with(file, {nullptr, &hosts}, b, error));
    CHECK(a == b);
  }
}

TEST_CASE("regeneration: submillimetre recovery is refused before an axis can collapse") {
  flora::Regeneration p;
  p.leader_loss = .5f;
  p.recovery = std::numeric_limits<f32>::denorm_min();
  assets::MeshData mesh;
  flora::Motion motion;
  std::string error;
  CHECK_FALSE(flora::solve_regeneration("maple", p, mesh, motion, nullptr, error));
  CHECK(error.find("1 mm") != std::string::npos);
  CHECK(mesh.positions.empty());
  CHECK(motion.parts.empty());
}

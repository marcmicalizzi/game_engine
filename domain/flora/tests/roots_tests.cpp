#include <core/hash/hash.h>
#include <domain/flora/roots.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>

using namespace engine;
namespace {
u64 root_hash(const assets::MeshData& mesh) {
  u64 h = hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
  return hash_combine(h, hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
}
flora::RootGround patch(f32 slope, bool cliff = false) {
  flora::RootGround out;
  out.min_x_m = out.min_z_m = -8;
  out.spacing_m = .25f;
  out.columns = out.rows = 65;
  for (u32 z = 0; z < out.rows; ++z)
    for (u32 x = 0; x < out.columns; ++x) {
      const f32 xx = out.min_x_m + x * out.spacing_m;
      out.heights_m.push_back(cliff && xx > 2 ? -5 : slope * xx);
    }
  return out;
}
}  // namespace
TEST_CASE("roots: deterministic tapered rigid wood, terrain-safe authored LOD and budget") {
  flora::Roots p;
  p.seed = 17;
  assets::MeshData mesh, again;
  flora::Motion motion;
  flora::RootReport report;
  std::string error;
  REQUIRE_MESSAGE(flora::solve_roots(p, mesh, motion, report, error), error);
  REQUIRE(report.primary >= 5);
  REQUIRE(report.secondary > 0);
  CHECK(report.triangles <= p.triangle_budget);
  CHECK(report.triangles == mesh.indices.size() / 3);
  CHECK(report.coarse_indices.size() < mesh.indices.size());
  CHECK(report.fine_element.size() == mesh.indices.size() / 3);
  CHECK(report.coarse_element.size() == report.coarse_indices.size() / 3);
  CHECK(report.elements == report.primary + report.secondary);
  REQUIRE(mesh.positions.size() == mesh.normals.size());
  REQUIRE(mesh.positions.size() == mesh.uvs.size());
  CHECK(motion.parts.size() == 1);
  for (const auto& v : mesh.positions) {
    CHECK(std::isfinite(v.x));
    CHECK(std::isfinite(v.y));
    CHECK(std::isfinite(v.z));
    CHECK(std::sqrt(v.x * v.x + v.z * v.z) <= p.crown_radius_m + .0001f);
  }
  for (const auto& normal : mesh.normals)
    CHECK(length(normal) == doctest::Approx(1).epsilon(.001));
  for (f32 weight : motion.vertex_weight)
    CHECK(weight == 0);
  for (const auto& tip : report.buried_tips)
    CHECK(tip.y < -.02f);
  for (u32 index : report.coarse_indices)
    REQUIRE(index < mesh.positions.size());
  REQUIRE(!report.exposed.empty());
  const auto& visible = report.exposed.front();
  CHECK(flora::root_covers(report, visible.from.x, visible.from.z));
  CHECK_FALSE(flora::root_covers(report, 100, 100));
  const u64 hash = root_hash(mesh);
  REQUIRE(flora::solve_roots(p, again, motion, report, error));
  CHECK(root_hash(again) == hash);
  p.seed++;
  REQUIRE(flora::solve_roots(p, again, motion, report, error));
  CHECK(root_hash(again) != hash);
  p.triangle_budget = 128;
  CHECK_FALSE(flora::solve_roots(p, again, motion, report, error));
}
TEST_CASE(
    "roots: switch off and small stems produce no geometry; constraints contain the full width") {
  flora::Roots p;
  assets::MeshData mesh;
  flora::Motion motion;
  flora::RootReport report;
  std::string error;
  p.enabled = false;
  p.ground = flora::RootGround{};  // intentionally unavailable; disabled does not read it
  REQUIRE(flora::solve_roots(p, mesh, motion, report, error));
  CHECK(mesh.positions.empty());
  CHECK(motion.parts.empty());
  p.enabled = true;
  p.ground.reset();
  p.trunk_radius_m = .02f;
  REQUIRE(flora::solve_roots(p, mesh, motion, report, error));
  CHECK(mesh.positions.empty());
  p.trunk_radius_m = .35f;
  p.bounds = flora::RootBounds{};
  p.max_radius_m = 1;
  p.exclusions.push_back({.6f, 0, .22f});
  REQUIRE(flora::solve_roots(p, mesh, motion, report, error));
  REQUIRE(!mesh.positions.empty());
  for (const auto& v : mesh.positions) {
    CHECK(v.x >= -1.0001f);
    CHECK(v.x <= 1.0001f);
    CHECK(v.z >= -1.0001f);
    CHECK(v.z <= 1.0001f);
    CHECK(v.x * v.x + v.z * v.z <= 1.0001f);
    CHECK((v.x - .6f) * (v.x - .6f) + v.z * v.z >= .22f * .22f - .0001f);
  }
  p.exclusions.push_back({0, 0, 2});
  REQUIRE(flora::solve_roots(p, mesh, motion, report, error));
  CHECK(mesh.indices.empty());
}
TEST_CASE("roots: sloped ground buries every endpoint and a drop stops roots before the edge") {
  flora::Roots p;
  p.seed = 19;
  assets::MeshData mesh;
  flora::Motion motion;
  flora::RootReport report;
  std::string error;
  p.ground = patch(.4f);
  REQUIRE(flora::solve_roots(p, mesh, motion, report, error));
  REQUIRE(report.primary > 0);
  for (const auto& tip : report.buried_tips)
    CHECK(tip.y < .4f * tip.x - .019f);
  p.ground = patch(0, true);
  REQUIRE(flora::solve_roots(p, mesh, motion, report, error));
  REQUIRE(report.primary > 0);
  for (const auto& v : mesh.positions)
    CHECK(v.x < 2.251f);
  for (const auto& tip : report.buried_tips) {
    // The height grid interpolates the drop across [2, 2.25], not at a vertical step.
    // A path may enter that cell slightly before its slope cutoff trips, but must bury.
    CHECK(tip.x < 2.25f);
    const f32 floor = tip.x <= 2 ? 0.0f : -5.0f * (tip.x - 2.0f) / .25f;
    CHECK(tip.y < floor - .019f);
  }
}
TEST_CASE("roots: ground samples keep far-world precision, yaw and uniform scale") {
  struct Site {
    f64 x, z;
    u32 calls = 0;
  } site{420000.125, -10000000.25};
  scene_gen::Ground ground;
  ground.context = &site;
  ground.surface_fn = [](const void* state, i64 x, i64 z) noexcept {
    const auto& s = *static_cast<const Site*>(state);
    return 20 + (static_cast<f64>(x) * .001 - s.x) * .2 + (static_cast<f64>(z) * .001 - s.z) * .3;
  };
  flora::RootGround sampled;
  std::string error;
  REQUIRE(flora::sample_root_ground(ground, {site.x, 20, site.z}, 0, 2, 2, .25f, sampled, error));
  CHECK(sampled.columns == 17);
  CHECK(sampled.heights_m.front() == doctest::Approx(-1));
  CHECK(sampled.heights_m.back() == doctest::Approx(1));
  REQUIRE(flora::sample_root_ground(ground, {site.x, 20, site.z}, 1.570796327f, 2, 2, .25f, sampled,
                                    error));
  CHECK(sampled.heights_m.front() == doctest::Approx(.2).epsilon(.002));
  CHECK_FALSE(
      flora::sample_root_ground(ground, {site.x, 20, site.z}, 0, 0, 2, .25f, sampled, error));
}
TEST_CASE("roots: recipe identity includes terrain and constraints without changing tree keys") {
  flora::FloraFile f;
  f.species = "roots";
  f.roots.seed = 17;
  flora::FloraFile read;
  std::string error;
  REQUIRE(flora::read_flora_file(flora::canonical_flora_json(f), read, error));
  CHECK(read.roots.seed == 17);
  const u64 key = flora::flora_source_hash(f);
  f.roots.bounds = flora::RootBounds{};
  CHECK(flora::flora_source_hash(f) != key);
  f.roots.bounds.reset();
  f.roots.ground = patch(0);
  const u64 terrain_key = flora::flora_source_hash(f);
  f.roots.ground->heights_m[100] = .25f;
  CHECK(flora::flora_source_hash(f) != terrain_key);
  f.roots.ground->heights_m[0] = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(flora::roots_in_range(f.roots, error));
  f = {};
  f.species = "maple";
  const u64 tree_key = flora::flora_source_hash(f);
  f.roots.enabled = false;
  CHECK(flora::flora_source_hash(f) == tree_key);
}
TEST_CASE("roots: maturity dimensions come from the same tree profile; site controls survive") {
  flora::FloraFile f;
  f.species = "oak";
  flora::Roots p;
  p.max_radius_m = 2;
  f.tree.age_years = 1;
  std::string error;
  REQUIRE(flora::roots_for_tree(f, p, error));
  const f32 young_radius = p.trunk_radius_m;
  f.tree.age_years = 150;
  REQUIRE(flora::roots_for_tree(f, p, error));
  CHECK(p.trunk_radius_m > young_radius);
  CHECK(p.max_radius_m == 2);
  CHECK(p.seed == f.tree.seed);
  CHECK(p.species == f.species);
}
TEST_CASE(
    "roots: cooked hierarchy has only the fitted fine and coarse rungs, rigid and bark-only") {
  flora::FloraFile f;
  f.species = "roots";
  f.roots.ground = patch(.3f);
  geometry::ClusterFileData container;
  geometry::ClusterLodOptions options;
  std::string error;
  REQUIRE_MESSAGE(flora::build_flora_container(f, options, 65536, container, nullptr, error),
                  error);
  CHECK(container.generator_kind == "roots");
  CHECK(container.materials.size() == 1);
  CHECK(container.impostors.empty());
  CHECK(container.mesh.level_cluster_counts.size() == 2);
  CHECK(container.motion_parts.size() == 1);
  for (const auto& v : container.motion_vertices)
    CHECK(v.weight == 0);
  for (const auto& lod : container.mesh.lod)
    CHECK(lod.level <= 1);
  f.roots.enabled = false;
  CHECK_FALSE(flora::build_flora_container(f, options, 65536, container, nullptr, error));
  CHECK(error.find("omit") != std::string::npos);
}

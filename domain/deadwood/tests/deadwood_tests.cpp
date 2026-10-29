#include <core/hash/hash.h>
#include <domain/assets/gltf.h>
#include <domain/deadwood/deadwood.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace engine;
namespace {
deadwood::DeadwoodFile recipe() {
  deadwood::DeadwoodFile p;
  p.tree.species = "oak";
  p.tree.tree.height_m = 6;
  p.tree.tree.triangle_budget = 60000;
  p.seed = 17;
  return p;
}
u64 mesh_hash(const assets::MeshData& m) {
  u64 h = hash_bytes(m.positions.data(), m.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(m.normals.data(), m.normals.size() * sizeof(Vec3)));
  h = hash_combine(h, hash_bytes(m.uvs.data(), m.uvs.size() * sizeof(Vec2)));
  return hash_combine(h, hash_bytes(m.indices.data(), m.indices.size() * sizeof(u32)));
}
void valid(const assets::MeshData& mesh) {
  REQUIRE(!mesh.positions.empty());
  REQUIRE(mesh.positions.size() == mesh.normals.size());
  REQUIRE(mesh.positions.size() == mesh.uvs.size());
  for (const auto& v : mesh.positions)
    CHECK(std::isfinite(v.x + v.y + v.z));
  for (const auto& n : mesh.normals)
    CHECK(length(n) == doctest::Approx(1).epsilon(.001));
  for (const u32 i : mesh.indices)
    REQUIRE(i < mesh.positions.size());
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const Vec3 a = mesh.positions[mesh.indices[i]], b = mesh.positions[mesh.indices[i + 1]],
               c = mesh.positions[mesh.indices[i + 2]];
    CHECK(length(cross(b - a, c - a)) > 0);
  }
}
}  // namespace
TEST_CASE("deadwood: variants preserve species geometry with bounded posed contact") {
  for (const auto kind :
       {deadwood::RemainKind::fallen, deadwood::RemainKind::snag, deadwood::RemainKind::snapped,
        deadwood::RemainKind::stump, deadwood::RemainKind::log, deadwood::RemainKind::limb}) {
    auto p = recipe();
    p.kind = kind;
    assets::MeshData mesh, again;
    flora::Motion motion;
    deadwood::Report report;
    std::string error;
    CAPTURE(static_cast<u32>(kind));
    REQUIRE_MESSAGE(deadwood::solve(p, mesh, motion, report, error), error);
    valid(mesh);
    CHECK(report.triangles <= report.budget);
    CHECK(report.minimum_clearance_m >= -p.contact_tolerance_m);
    if (kind == deadwood::RemainKind::fallen) {
      CHECK(report.foliage_triangles > 0);
      CHECK(report.bent_parts + report.broken_parts > 0);
    }
    if (kind == deadwood::RemainKind::snag) CHECK(report.foliage_triangles == 0);
    REQUIRE(motion.parts.size() == 2);
    for (const auto weight : motion.vertex_weight)
      CHECK(weight == 0);
    const u64 hash = mesh_hash(mesh);
    REQUIRE_MESSAGE(deadwood::solve(p, again, motion, report, error), error);
    CHECK(mesh_hash(again) == hash);
  }
}
TEST_CASE("deadwood: leafless and branchless are independent, source is unchanged") {
  auto p = recipe();
  p.pitch_deg = 0;
  p.kind = deadwood::RemainKind::snag;
  p.branches = 1;
  p.foliage = 1;
  assets::MeshData a, b, source_a, source_b;
  flora::Motion motion;
  deadwood::Report ra, rb;
  std::string error;
  REQUIRE(flora::solve_flora(p.tree, source_a, motion, nullptr, nullptr, error));
  REQUIRE_MESSAGE(deadwood::solve(p, a, motion, ra, error), error);
  p.foliage = 0;
  REQUIRE_MESSAGE(deadwood::solve(p, b, motion, rb, error), error);
  CHECK(rb.foliage_triangles == 0);
  CHECK(rb.wood_triangles == ra.wood_triangles);
  p.branches = 0;
  REQUIRE_MESSAGE(deadwood::solve(p, b, motion, rb, error), error);
  CHECK(rb.wood_triangles < ra.wood_triangles);
  REQUIRE(flora::solve_flora(p.tree, source_b, motion, nullptr, nullptr, error));
  CHECK(mesh_hash(source_a) == mesh_hash(source_b));
}
TEST_CASE("deadwood: hollow shell and collapsed log have inner walls within the cap") {
  auto p = recipe();
  p.kind = deadwood::RemainKind::log;
  p.start_m = .3f;
  p.end_m = 3;
  p.hollow = .7f;
  p.bark = .25f;
  assets::MeshData mesh;
  flora::Motion motion;
  deadwood::Report report;
  std::string error;
  REQUIRE_MESSAGE(deadwood::solve(p, mesh, motion, report, error), error);
  valid(mesh);
  const u64 intact = mesh_hash(mesh);
  p.collapse = .65f;
  REQUIRE_MESSAGE(deadwood::solve(p, mesh, motion, report, error), error);
  valid(mesh);
  CHECK(mesh_hash(mesh) != intact);
  p.triangle_budget = 128;
  CHECK_FALSE(deadwood::solve(p, mesh, motion, report, error));
  CHECK(error.find("triangle_budget") != std::string::npos);
  CHECK(mesh.positions.empty());
}
TEST_CASE("deadwood: schema validation, canonical identity and registry") {
  auto p = recipe();
  deadwood::DeadwoodFile read;
  std::string error;
  const auto text = deadwood::canonical_json(p);
  REQUIRE_MESSAGE(deadwood::read_file(text, read, error), error);
  CHECK(deadwood::source_hash(read) == deadwood::source_hash(p));
  read.yaw_deg = 35;
  CHECK(deadwood::source_hash(read) != deadwood::source_hash(p));
  CHECK_FALSE(deadwood::read_file(R"({"unexpected":1})", read, error));
  p.bark = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(deadwood::in_range(p, error));
  p = recipe();
  p.require_support = true;
  CHECK_FALSE(deadwood::in_range(p, error));
  const auto* generator = assets::MeshGeneratorRegistry::global().find("deadwood");
  REQUIRE(generator != nullptr);
  u64 hash = 0;
  REQUIRE(
      generator->source_hash({reinterpret_cast<const u8*>(text.data()), text.size()}, hash, error));
  CHECK(hash == deadwood::source_hash(recipe()));
}
TEST_CASE("deadwood: leaning support contact is checked and hashed") {
  auto p = recipe();
  p.pitch_deg = 55;
  p.branches = 0;
  p.foliage = 0;
  assets::MeshData mesh;
  flora::Motion motion;
  deadwood::Report report;
  std::string error;
  REQUIRE_MESSAGE(deadwood::solve(p, mesh, motion, report, error), error);
  const Vec3 tip = *std::max_element(mesh.positions.begin(), mesh.positions.end(),
                                     [](Vec3 a, Vec3 b) { return a.z < b.z; });
  deadwood::Support support;
  support.from = {tip.x, -10, tip.z + .5f};
  support.to = {tip.x, 10, tip.z + .5f};
  support.radius_m = .5f;
  p.supports.push_back(support);
  p.require_support = true;
  REQUIRE_MESSAGE(deadwood::solve(p, mesh, motion, report, error), error);
  const u64 key = deadwood::source_hash(p);
  p.supports[0].from.z += 1;
  p.supports[0].to.z += 1;
  CHECK(deadwood::source_hash(p) != key);
  CHECK_FALSE(deadwood::solve(p, mesh, motion, report, error));
  CHECK(error.find("does not touch") != std::string::npos);
  p.supports[0] = support;
  p.supports[0].radius_m = 1;
  CHECK_FALSE(deadwood::solve(p, mesh, motion, report, error));
  CHECK(error.find("intersects") != std::string::npos);
}
TEST_CASE("deadwood: sampled terrain and intervals shorter than a source segment") {
  auto p = recipe();
  p.kind = deadwood::RemainKind::log;
  p.start_m = .21f;
  p.end_m = .24f;
  p.splinter_m = 0;
  assets::MeshData mesh;
  flora::Motion motion;
  deadwood::Report report;
  std::string error;
  REQUIRE_MESSAGE(deadwood::solve(p, mesh, motion, report, error), error);
  valid(mesh);
  p = recipe();
  p.ground.emplace();
  auto& g = *p.ground;
  g.columns = g.rows = 17;
  g.spacing_m = 2;
  g.min_x_m = g.min_z_m = -16;
  g.heights_m.resize(17 * 17);
  for (u32 z = 0; z < 17; ++z)
    for (u32 x = 0; x < 17; ++x)
      g.heights_m[z * 17 + x] = .03f * (static_cast<f32>(x) * 2 - 16);
  REQUIRE_MESSAGE(deadwood::solve(p, mesh, motion, report, error), error);
  for (Vec3 v : mesh.positions)
    CHECK(v.y - .03f * v.x >= -p.contact_tolerance_m);
  g.min_x_m = 100;
  CHECK_FALSE(deadwood::solve(p, mesh, motion, report, error));
  CHECK(error.find("ground") != std::string::npos);
}
TEST_CASE("deadwood: broadleaf and conifer profiles retain their own posed geometry") {
  for (const char* species : {"oak", "maple", "pine", "spruce", "alder"}) {
    auto p = recipe();
    p.tree.species = species;
    p.roll_deg = 37;
    p.yaw_deg = 71;
    assets::MeshData mesh;
    flora::Motion motion;
    deadwood::Report report;
    std::string error;
    CAPTURE(species);
    REQUIRE_MESSAGE(deadwood::solve(p, mesh, motion, report, error), error);
    valid(mesh);
    CHECK(report.foliage_triangles > 0);
  }
}
TEST_CASE("deadwood: container keeps wood collision eligibility and texture roles") {
  auto p = recipe();
  p.kind = deadwood::RemainKind::stump;
  geometry::ClusterFileData container;
  deadwood::Report report;
  std::string error;
  REQUIRE_MESSAGE(deadwood::build_container(p, {}, {}, 65536, container, report, error), error);
  CHECK(container.generator == "deadwood");
  CHECK(container.generator_kind == "oak");
  CHECK(container.source_hash == deadwood::source_hash(p));
  REQUIRE(container.motion_vertices.size() == container.mesh.mesh.vertices.size());
  for (const auto& v : container.motion_vertices) {
    CHECK(v.part == 0);
    CHECK(v.weight == 0);
  }
  assets::GeneratorHostMesh host;
  assets::host_mesh_of(container, host);
  CHECK(!host.indices.empty());
  for (const auto h : host.holds)
    CHECK(h == 1);
}

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <domain/assets/gltf.h>
#include <domain/deadwood/deadwood.h>
#include <domain/materials/materials.h>
#include <foundation/image/decode.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
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
TEST_CASE("deadwood: authored lean contacts the visible supporting trunk") {
  const auto path = test::data_path(ENGINE_DEADWOOD_CONTENT, "content/test-scenes/deadwood");
  if (!test::path_exists(path)) {
    MESSAGE("bundle omits deadwood study");
    return;
  }
  std::ifstream input(path + "/leaning-oak.deadwood.json");
  const std::string json{std::istreambuf_iterator<char>(input), {}};
  deadwood::DeadwoodFile p;
  assets::MeshData mesh, standing;
  flora::Motion motion, growth;
  deadwood::Report report;
  std::string error;
  REQUIRE_MESSAGE(deadwood::read_file(json, p, error), error);
  REQUIRE_MESSAGE(deadwood::solve(p, mesh, motion, report, error), error);
  std::ifstream tree_input(path + "/support-oak.flora.json");
  const std::string tree_json{std::istreambuf_iterator<char>(tree_input), {}};
  flora::FloraFile tree;
  REQUIRE_MESSAGE(flora::read_flora_file(tree_json, tree, error), error);
  REQUIRE_MESSAGE(flora::solve_flora(tree, standing, growth, nullptr, nullptr, error), error);
  // Match lean.scene.json's placement and check the actual trunk cross-sections, not just
  // the capsule the solver used. Edge midpoints catch between-vertex penetration too.
  std::ifstream scene_input(path + "/lean.scene.json");
  const std::string scene_json{std::istreambuf_iterator<char>(scene_input), {}};
  JsonValue scene;
  REQUIRE(parse_json(scene_json, scene).ok);
  REQUIRE(scene.find("instances") != nullptr);
  const auto& instances = scene.find("instances")->as_array();
  REQUIRE(instances.size() == 2);
  REQUIRE(instances[1].find("translation") != nullptr);
  const auto& translation = instances[1].find("translation")->as_array();
  REQUIRE(translation.size() == 3);
  f64 xyz[3]{};
  for (u32 i = 0; i < 3; ++i)
    REQUIRE(translation[i].get_f64(xyz[i]));
  const Vec3 placement{static_cast<f32>(xyz[0]), static_cast<f32>(xyz[1]),
                       static_cast<f32>(xyz[2])};
  Vector<std::array<Vec3, 3>> trunk;
  for (const auto& prim : standing.primitives) {
    if (prim.material != 0) continue;
    for (u32 i = prim.first_index; i < prim.first_index + prim.index_count; i += 3)
      if (growth.vertex_part[standing.indices[i]] == 0)
        trunk.push_back({standing.positions[standing.indices[i]],
                         standing.positions[standing.indices[i + 1]],
                         standing.positions[standing.indices[i + 2]]});
  }
  f32 minimum = 1000;
  auto check_sample = [&](Vec3 point) {
    const Vec3 q = point - placement;
    u32 crossings = 0;
    f32 nearest = 1000;
    for (const auto& triangle : trunk) {
      Vec3 hit[3];
      u32 count = 0;
      for (u32 k = 0; k < 3; ++k) {
        const Vec3 a = triangle[k];
        const Vec3 b = triangle[(k + 1) % 3];
        if ((a.y < q.y) != (b.y < q.y)) hit[count++] = a + (b - a) * ((q.y - a.y) / (b.y - a.y));
      }
      if (count != 2) continue;
      const Vec3 a = hit[0], b = hit[1], d = b - a;
      const f32 squared = dot(d, d);
      if (squared < 1e-12f) continue;
      const Vec3 closest = a + d * clamp(dot(q - a, d) / squared, 0, 1);
      nearest = std::min(nearest, length(q - closest));
      if ((a.z > q.z) != (b.z > q.z) && q.x < a.x + (b.x - a.x) * ((q.z - a.z) / (b.z - a.z)))
        ++crossings;
    }
    const f32 gap = (crossings & 1) ? -nearest : nearest;
    CHECK(gap >= -p.contact_tolerance_m);
    minimum = std::min(minimum, gap);
  };
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    for (u32 k = 0; k < 3; ++k) {
      const Vec3 a = mesh.positions[mesh.indices[i + k]];
      const Vec3 b = mesh.positions[mesh.indices[i + (k + 1) % 3]];
      check_sample(a);
      check_sample((a + b) * .5f);
    }
  }
  CHECK(minimum <= p.contact_tolerance_m);
  CHECK(report.minimum_clearance_m >= -p.contact_tolerance_m);
  const u64 key = deadwood::source_hash(p);
  p.supports[0].from.z += 2;
  p.supports[0].to.z += 2;
  CHECK(deadwood::source_hash(p) != key);
  CHECK_FALSE(deadwood::solve(p, mesh, motion, report, error));
}
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
TEST_CASE("deadwood: native maps and authored material precedence") {
  auto p = recipe();
  p.kind = deadwood::RemainKind::log;
  p.bark = .5f;
  p.start_m = .3f;
  p.end_m = 3;
  p.decay = .7f;
  p.surface_resolution = 128;
  geometry::ClusterFileData data;
  deadwood::Report report;
  std::string error;
  REQUIRE_MESSAGE(deadwood::build_container(p, {}, {}, 65536, data, report, error), error);
  REQUIRE(data.materials.size() >= 4);
  const u32 first = data.materials.size() - 3;
  REQUIRE(data.images.size() == 9);
  CHECK(data.textures.size() == data.images.size());
  for (u32 role = 0; role < 3; ++role) {
    const auto& m = data.materials[first + role];
    REQUIRE(m.base_color_image >= 0);
    REQUIRE(m.normal_image >= 0);
    CHECK(m.metallic_roughness_image != 0);
    image::Image colour, normal;
    REQUIRE(
        image::decode_image(data.images[static_cast<u32>(m.base_color_image)].bytes, colour, 3));
    REQUIRE(image::decode_image(data.images[static_cast<u32>(m.normal_image)].bytes, normal, 3));
    CHECK(colour.width == 128);
    CHECK(colour.height == 128);
    const auto [lo, hi] = std::minmax_element(colour.pixels.begin(), colour.pixels.end());
    CHECK(*hi - *lo > 25);
    bool relief = false;
    for (u32 i = 0; i < normal.pixels.size(); i += 3)
      relief |= normal.pixels[i] < 124 || normal.pixels[i] > 132 || normal.pixels[i + 1] < 124 ||
                normal.pixels[i + 1] > 132;
    CHECK(relief);
  }
  for (const auto& v : data.motion_vertices)
    CHECK(v.part == 0);
  const u64 fibrous = geometry::cluster_file_hash(data);
  p.decay_pattern = deadwood::DecayPattern::cubical;
  REQUIRE(deadwood::build_container(p, {}, {}, 65536, data, report, error));
  CHECK(geometry::cluster_file_hash(data) != fibrous);
  p.tree.materials.emplace();
  p.tree.materials->wood.emplace();
  p.tree.materials->wood->base_color = "authored-wood.png";
  REQUIRE(deadwood::build_container(p, {}, {}, 65536, data, report, error));
  CHECK(data.image_paths.size() == 1);
  CHECK(data.image_paths[0] == "authored-wood.png");
  for (u32 role = 0; role < 3; ++role)
    CHECK(data.materials[first + role].base_color_image == 0);
  p.tree.materials.reset();
  p.surface_resolution = 0;
  REQUIRE(deadwood::build_container(p, {}, {}, 65536, data, report, error));
  CHECK(data.image_paths.empty());
  for (u32 bad : {1u, 127u, 255u, 2048u}) {
    p.surface_resolution = bad;
    CHECK_FALSE(deadwood::in_range(p, error));
  }
}
TEST_CASE("deadwood: bark partition preserves the solid and hollow surface area") {
  for (f32 hollow : {0.0f, .7f}) {
    auto p = recipe();
    p.kind = deadwood::RemainKind::log;
    p.start_m = .3f;
    p.end_m = 3;
    p.hollow = hollow;
    assets::MeshData intact, patches;
    flora::Motion motion;
    deadwood::Report report;
    std::string error;
    REQUIRE(deadwood::solve(p, intact, motion, report, error));
    p.bark = .5f;
    REQUIRE(deadwood::solve(p, patches, motion, report, error));
    const auto area = [](const assets::MeshData& m) {
      f64 sum = 0;
      for (u32 i = 0; i < m.indices.size(); i += 3) {
        const Vec3 a = m.positions[m.indices[i]], b = m.positions[m.indices[i + 1]],
                   c = m.positions[m.indices[i + 2]];
        sum += static_cast<f64>(length(cross(b - a, c - a))) * .5;
      }
      return sum;
    };
    CHECK(area(patches) == doctest::Approx(area(intact)).epsilon(.0001));
  }
}

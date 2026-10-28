// The cliff solver (docs/subsystems/rocks.md, "Cliffs"): closedness at every rung and type, the
// ladder's numbers, the undercut's overhang, the file and its key, the pack's roles, determinism.
#include "rock_checks.h"

#include <core/hash/hash.h>
#include <core/time/time.h>
#include <domain/geometry/cluster.h>
#include <domain/materials/materials.h>
#include <domain/rocks/rocks.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::rocks;
using namespace engine::rocks::checks;

namespace {

// A face `length` metres along a gentle bend in the ground's plane: the line runs along +x and
// swings 8% of its length toward +z and back, so the frames turn and the face looks to +z.
Cliff bent(f32 length, f32 height, u32 budget, RockType type = RockType::Sandstone) {
  Cliff c;
  c.type = type;
  c.height = height;
  c.triangle_budget = budget;
  const u32 n = 9;
  for (u32 i = 0; i < n; ++i) {
    const f32 t = static_cast<f32>(i) / static_cast<f32>(n - 1);
    CliffPoint p;
    p.x = length * t;
    p.z = 0.08f * length * 4.0f * t * (1.0f - t);
    p.y = 0.02f * length * t;  // the ground rises along it a little
    c.points.push_back(p);
  }
  return c;
}

Cliff straight(f32 length, f32 height, u32 budget) {
  Cliff c;
  c.height = height;
  c.triangle_budget = budget;
  c.points.push_back(CliffPoint{});
  CliffPoint end;
  end.x = length;
  c.points.push_back(end);
  return c;
}

assets::MeshData solve(const Cliff& c, CliffReport* report = nullptr) {
  assets::MeshData mesh;
  std::string error;
  const bool ok = solve_cliff(c, mesh, report, &error);
  REQUIRE_MESSAGE(ok, error);
  return mesh;
}

RockFile cliff_file(const Cliff& c) {
  RockFile f;
  f.shape = RockShape::Cliff;
  f.cliff = c;
  return f;
}

// Where a vertical line through (x, z) meets the mesh's triangles, and the sign of each one's
// normal's y there.
struct Hit {
  f64 y;
  bool down;
};
std::vector<Hit> vertical_hits(const assets::MeshData& mesh, f64 x, f64 z) {
  std::vector<Hit> hits;
  for (u32 i = 0; i < mesh.indices.size(); i += 3) {
    const Vec3 a = mesh.positions[mesh.indices[i]];
    const Vec3 b = mesh.positions[mesh.indices[i + 1]];
    const Vec3 c = mesh.positions[mesh.indices[i + 2]];
    const f64 ax = static_cast<f64>(a.x), ay = static_cast<f64>(a.y), az = static_cast<f64>(a.z);
    const f64 bx = static_cast<f64>(b.x) - ax, by = static_cast<f64>(b.y) - ay;
    const f64 bz = static_cast<f64>(b.z) - az;
    const f64 cx = static_cast<f64>(c.x) - ax, cy = static_cast<f64>(c.y) - ay;
    const f64 cz = static_cast<f64>(c.z) - az;
    const f64 d = bx * cz - cx * bz;
    if (std::fabs(d) < 1e-12) continue;
    const f64 u = ((x - ax) * cz - cx * (z - az)) / d;
    const f64 v = (bx * (z - az) - (x - ax) * bz) / d;
    if (u < 0.0 || v < 0.0 || u + v > 1.0) continue;
    const f64 y = ay + u * by + v * cy;
    const Vec3 n = cross(b - a, c - a);
    hits.push_back(Hit{y, n.y < 0.0f});
  }
  return hits;
}

}  // namespace

TEST_CASE("cliffs: every type is closed, two-manifold and wound outwards at every rung") {
  for (const RockType type :
       {RockType::Granite, RockType::Sandstone, RockType::Basalt, RockType::Limestone}) {
    for (const u32 budget : {2000u, 20000u, 120000u}) {
      for (const f32 undercut : {0.0f, 0.7f}) {
        CAPTURE(std::string(rock_type_name(type)));
        CAPTURE(budget);
        CAPTURE(undercut);
        CliffReport report;
        Cliff c = bent(120.0f, 30.0f, budget, type);
        c.undercut = undercut;
        const assets::MeshData mesh = solve(c, &report);
        CHECK(report.triangles <= budget);
        check_closed(mesh, report.parts);
      }
    }
  }
}

TEST_CASE("cliffs: a 300 m by 40 m face down the ladder") {
  const i64 start = time::monotonic_ns();
  CliffReport fine;
  const assets::MeshData a = solve(straight(300.0f, 40.0f, 250000), &fine);
  const f64 ms = static_cast<f64>(time::monotonic_ns() - start) / 1.0e6;
  MESSAGE("300 x 40 at 250,000: cell "
          << fine.cell_m << " m, " << fine.stations << " x " << fine.rows << " x "
          << fine.depth_rows << ", face " << fine.face_triangles << ", skirt "
          << fine.skirt_triangles << ", blocks " << fine.block_triangles << " (n "
          << fine.block_grid << "), " << fine.triangles << " triangles, depth " << fine.depth_m
          << " m, " << ms << " ms");
  CliffReport coarse;
  const assets::MeshData b = solve(straight(300.0f, 40.0f, 2000), &coarse);
  MESSAGE("300 x 40 at 2,000: cell " << coarse.cell_m << " m, " << coarse.stations << " x "
                                     << coarse.rows << " x " << coarse.depth_rows << ", "
                                     << coarse.triangles << " triangles");
  check_closed(a, fine.parts);
  check_closed(b, coarse.parts);
}

TEST_CASE("cliffs: the undercut makes an overhang, and without it nothing overhangs") {
  // A straight sandstone face along +x looking at +z, no skirt and no blocks, so every triangle
  // is the face's own. An overhang is a triangle facing down well above the foot: the underside
  // of a hard bed standing out over the notched weak bed under it. A vertical line through it
  // meets the face twice — into the bed from above and out of its underside — before the ground.
  const auto overhangs = [](f32 undercut, std::vector<Vec3>* centres) {
    Cliff c = straight(120.0f, 30.0f, 200000);
    c.undercut = undercut;
    c.talus = 0.0f;
    c.talus_blocks = 0;
    const assets::MeshData mesh = solve(c);
    u32 count = 0;
    for (u32 i = 0; i < mesh.indices.size(); i += 3) {
      const Vec3 a = mesh.positions[mesh.indices[i]];
      const Vec3 b = mesh.positions[mesh.indices[i + 1]];
      const Vec3 d = mesh.positions[mesh.indices[i + 2]];
      const Vec3 n = cross(b - a, d - a);
      const f32 area2 = std::sqrt(dot(n, n));
      if (area2 < 0.02f) continue;
      const Vec3 centre = (a + b + d) * (1.0f / 3.0f);
      // On the front, above the foot and under the crest, with no part of the box's top or back.
      if (centre.y < 1.0f || centre.y > 26.0f || centre.z < -12.0f) continue;
      if (n.y / area2 < -0.5f) {
        ++count;
        if (centres != nullptr) centres->push_back(centre);
      }
    }
    return std::pair<u32, assets::MeshData>(count, mesh);
  };
  std::vector<Vec3> centres;
  const auto [with, mesh] = overhangs(1.0f, &centres);
  const auto [without, plain] = overhangs(0.0f, nullptr);
  MESSAGE("downward-facing triangles above the foot: " << with << " with the undercut, " << without
                                                       << " without");
  CHECK(with > 200);
  CHECK(without < with / 20);
  REQUIRE(!centres.empty());
  // The vertical line through the first: at least two hits above the foot, the lower one facing
  // down (out of the overhang's underside) and one above it facing up (into the bed from above).
  const Vec3 at = centres[centres.size() / 2];
  const std::vector<Hit> hits = vertical_hits(mesh, static_cast<f64>(at.x), static_cast<f64>(at.z));
  u32 down = 0;
  u32 up_above = 0;
  for (const Hit& h : hits) {
    if (h.down && h.y > 1.0 && std::fabs(h.y - static_cast<f64>(at.y)) < 0.5) ++down;
    if (!h.down && h.y > static_cast<f64>(at.y)) ++up_above;
  }
  CHECK(down >= 1);
  CHECK(up_above >= 1);
  CHECK(hits.size() >= 2);
}

TEST_CASE("cliffs: the file is read, canonicalized and refused with the field's name") {
  const std::string text =
      R"({"shape": "Cliff", "cliff": {"points": [{"x": 0, "z": 0}, {"x": 50, "z": 0}]}})";
  RockFile file;
  std::string error;
  REQUIRE_MESSAGE(read_rock_file(text, file, error), error);
  REQUIRE(file.cliff.has_value());
  CHECK(file.cliff->points.size() == 2);
  // Defaults are spelled out, and a boulder's canonical bytes name no cliff.
  const std::string canonical = canonical_rock_json(file);
  CHECK(canonical.find("\"talus_angle_deg\":35") != std::string::npos);
  RockFile boulder;
  boulder.shape = RockShape::Boulder;
  CHECK(canonical_rock_json(boulder).find("cliff") == std::string::npos);
  // Refusals, each naming its field.
  const auto refused = [](const std::string& json, const char* field) {
    RockFile f;
    std::string why;
    CHECK_FALSE(read_rock_file(json, f, why));
    CHECK_MESSAGE(why.find(field) != std::string::npos, why);
  };
  refused(R"({"shape": "Cliff"})", "engine.rocks.RockFile.cliff");
  refused(R"({"shape": "Cliff", "cliff": {"points": [{"x": 0}]}})", "engine.rocks.Cliff.points");
  refused(R"({"shape": "Cliff", "cliff": {"points": [{"x": 0}, {"x": 1200}]}})",
          "engine.rocks.Cliff.points");
  refused(R"({"shape": "Cliff", "cliff": {"points": [{"x": 0}, {"x": 50}], "height": 400}})",
          "engine.rocks.Cliff.height");
  refused(R"({"shape": "Cliff", "cliff": {"points": [{"x": 0}, {"x": 50}], "undercut": 2}})",
          "engine.rocks.Cliff.undercut");
  refused(R"({"shape": "Cliff", "cliff": {"points": [{"x": 0}, {"x": 50}], "bed_m": 0.1}})",
          "engine.rocks.Cliff.bed_m");
  refused(R"({"shape": "Cliff", "cliff": {"points": [{"x": 0}, {"x": 50, "height": 0.5}]}})",
          "engine.rocks.Cliff.points.height");
  refused(
      R"({"shape": "Cliff", "cliff": {"points": [{"x": 0}, {"x": 50}], "triangle_budget": 10}})",
      "engine.rocks.Cliff.triangle_budget");
  refused(R"({"shape": "Boulder", "cliff": {"points": [{"x": 0}, {"x": 50}]}})",
          "engine.rocks.RockFile.cliff");
  refused(R"({"shape": "Cliff", "boulder": {}, "cliff": {"points": [{"x": 0}, {"x": 50}]}})",
          "engine.rocks.RockFile.boulder");
}

TEST_CASE("cliffs: a cliff's container is built like any rock's, and samples its type's role") {
  Cliff c = bent(80.0f, 20.0f, 40000);
  c.type = RockType::Limestone;
  const RockFile file = cliff_file(c);
  const geometry::ClusterLodOptions lod{};
  geometry::ClusterFileData plain;
  std::string error;
  RockReport report;
  REQUIRE_MESSAGE(build_rock_container(file, lod, 0, plain, &report, error), error);
  CHECK(plain.generator == "rock");
  CHECK(plain.generator_kind == "Cliff");
  CHECK(plain.source_hash == rock_source_hash(file));
  CHECK(report.triangles <= 40000);
  CHECK(plain.image_paths.empty());
  u32 coneless = 0;
  for (const geometry::ClusterDesc& cluster : plain.mesh.mesh.clusters)
    coneless += (cluster.cone >> 24) == 0x7fu ? 1u : 0u;
  CHECK(coneless < plain.mesh.mesh.clusters.size() / 4);  // closed and one-sided: cones on
  // A pack with `rock/limestone` gives the face the pack's set, at the pack's strength.
  const char* k_pack =
      R"({"format":"engine.materials.pack/1","name":"stand-in","version":"1","roles":{)"
      R"("rock/limestone":{"tile_m":3,"normal_scale":0.7,"normal_convention":"opengl","maps":{)"
      R"("base_color":{"file":"l/c.png"},"normal":{"file":"l/n.png"},"roughness":{"file":"l/r.png"},)"
      R"("metallic":{"file":"l/m.png"},"height":{"file":"l/h.png"}}}}})";
  materials::Pack pack;
  REQUIRE_MESSAGE(materials::parse_pack(k_pack, "C:/stand-in", pack, error), error);
  geometry::ClusterFileData packed;
  REQUIRE_MESSAGE(build_rock_container(file, lod, 0, packed, nullptr, error, &pack), error);
  REQUIRE(packed.materials.size() == 1);
  REQUIRE(packed.materials[0].base_color_image >= 0);
  CHECK(packed.image_paths[static_cast<u32>(packed.materials[0].base_color_image)] ==
        "C:/stand-in/l/c.png");
  CHECK(packed.materials[0].normal_scale == doctest::Approx(0.7f));
  CHECK(packed.build_key != plain.build_key);
  CHECK(packed.source_hash == plain.source_hash);
}
TEST_CASE("cliffs: one seed is one face, and the solve is the same bytes twice") {
  const Cliff c = bent(100.0f, 25.0f, 30000);
  const u64 a = mesh_hash(solve(c));
  CHECK(mesh_hash(solve(c)) == a);
  Cliff other = c;
  other.seed = 2;
  CHECK(mesh_hash(solve(other)) != a);
}

TEST_CASE("cliffs: a kilometre by 200 m face is one mesh, closed, at a million triangles") {
  Cliff c = bent(980.0f, 200.0f, 1000000, RockType::Granite);  // 997 m along its bend
  c.talus_blocks = 64;
  const i64 start = time::monotonic_ns();
  CliffReport report;
  const assets::MeshData mesh = solve(c, &report);
  const f64 ms = static_cast<f64>(time::monotonic_ns() - start) / 1.0e6;
  MESSAGE("1000 x 200 at 1,000,000: cell "
          << report.cell_m << " m, " << report.stations << " x " << report.rows << " x "
          << report.depth_rows << ", face " << report.face_triangles << ", skirt "
          << report.skirt_triangles << ", blocks " << report.block_triangles << ", "
          << report.triangles << " triangles, depth " << report.depth_m << " m, " << ms
          << " ms (this build)");
  CHECK(report.triangles <= 1000000);
  check_closed(mesh, report.parts);
}

TEST_CASE("cliffs: the committed escarpment is closed, holds its budget, and is its bytes") {
  // content/rocks/sandstone-escarpment.rock.json: what docs/subsystems/rocks.md's numbers and E10
  // were taken on. Its mesh is pinned like the boulders'.
  constexpr u64 k_mesh = 0xd60ee81d81f86032ull;
  const std::string dir = test::data_path(ENGINE_SOURCE_DIR "/content/rocks", "content/rocks");
  std::ifstream in(dir + "/sandstone-escarpment.rock.json", std::ios::binary);
  if (!in) {
    MESSAGE("content/rocks is not here (a bundle without it): skipped");
    return;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  RockFile file;
  std::string error;
  REQUIRE_MESSAGE(read_rock_file(text, file, error), error);
  REQUIRE(file.shape == RockShape::Cliff);
  CliffReport report;
  const assets::MeshData mesh = solve(*file.cliff, &report);
  check_closed(mesh, report.parts);
  CHECK(report.triangles <= file.cliff->triangle_budget);
  f32 extreme = 0.0f;
  for (const Vec2& uv : mesh.uvs)
    extreme = std::max(extreme, std::max(std::fabs(uv.x), std::fabs(uv.y)));
  CHECK(extreme <= 16.0f);
  const u64 h = mesh_hash(mesh);
  MESSAGE("sandstone-escarpment: "
          << report.triangles << " triangles, " << report.parts << " parts, cell " << report.cell_m
          << " m, " << report.stations << " x " << report.rows << " x " << report.depth_rows
          << ", face " << report.face_triangles << ", skirt " << report.skirt_triangles
          << ", blocks " << report.block_triangles << " (n " << report.block_grid << "), uv to "
          << extreme << ", mesh " << h);
  CHECK(h == k_mesh);
}

// The cliff solver (docs/subsystems/rocks.md, "Cliffs"): closedness at every rung and type, the
// ladder's numbers, the undercut's overhang, the file and its key, the pack's roles, determinism.
#include "../src/cliff_benches.h"
#include "rock_checks.h"

#include <core/hash/hash.h>
#include <core/time/time.h>
#include <domain/geometry/cluster.h>
#include <domain/materials/materials.h>
#include <domain/rocks/cliffs.h>
#include <domain/rocks/rocks.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::rocks;
using namespace engine::rocks::checks;

TEST_CASE("cliff: sampled ground buries the apron base and toe on falling terrain") {
  for (RockQuality quality : {RockQuality::Low, RockQuality::High}) {
    Cliff c;
    c.height = 32;
    c.talus = 0.2f;  // A broad apron must expose the old reference-plane defect.
    c.quality = quality;
    c.triangle_budget = 30000;
    c.talus_blocks = 3;
    c.points.push_back(CliffPoint{});
    CliffPoint end;
    end.x = 60;
    c.points.push_back(end);
    assets::MeshData before, after;
    CliffReport a, b;
    std::string error;
    REQUIRE_MESSAGE(solve_cliff(c, before, &a, &error), error);
    scene_gen::Ground ground;
    ground.surface_fn = [](const void*, i64 x, i64 z) noexcept {
      return 0.05 * (static_cast<f64>(x) / 1000.0) - 0.65 * (static_cast<f64>(z) / 1000.0);
    };
    REQUIRE_MESSAGE(fit_cliff_ground(c, ground, WorldPos{}, 0.5f, &error), error);
    REQUIRE(c.ground.has_value());
    REQUIRE_MESSAGE(solve_cliff(c, after, &b, &error), error);
    CHECK(a.triangles == b.triangles);
    CHECK(a.parts == b.parts);
    REQUIRE(before.indices.size() == after.indices.size());
    check_closed(after, b.parts);
    // Ground-dependent UV chart sorting may reorder triangles. Match vertical columns by
    // position instead of assuming equal index order in two differently draped meshes.
    std::map<std::pair<f32, f32>, std::vector<f32>> columns;
    for (u32 i = b.face_triangles * 3; i < (b.face_triangles + b.skirt_triangles) * 3; ++i) {
      const auto p = after.positions[after.indices[i]];
      columns[{p.x, p.z}].push_back(p.y);
    }
    u32 base = 0, toe = 0, exposed_before = 0;
    for (u32 i = a.face_triangles * 3; i < (a.face_triangles + a.skirt_triangles) * 3; ++i) {
      const u32 v = before.indices[i];
      const auto old = before.positions[v];
      REQUIRE(columns.contains({old.x, old.z}));
      const auto& heights = columns.at({old.x, old.z});
      const f64 h = 0.05 * static_cast<f64>(old.x) - 0.65 * static_cast<f64>(old.z);
      if (std::fabs(old.y + 2.3f) < 1e-4f) {
        CHECK(static_cast<f64>(*std::min_element(heights.begin(), heights.end())) <= h - 0.999);
        if (static_cast<f64>(old.y) > h + 1.0) ++exposed_before;
        ++base;
      }
      if (std::fabs(old.y + 1.3f) < 1e-4f) {
        CHECK(std::any_of(heights.begin(), heights.end(),
                          [&](f32 y) { return y == doctest::Approx(h - 1.3).epsilon(0.0001); }));
        ++toe;
      }
    }
    CHECK(base > 0);
    CHECK(toe > 0);
    CHECK(exposed_before > 0);
    RockFile file;
    file.shape = RockShape::Cliff;
    file.cliff = c;
    const auto hash = rock_source_hash(file);
    file.cliff->ground->heights[0] += 1.0f;
    CHECK(rock_source_hash(file) != hash);
    c.ground->min_x_m += 10000;
    CHECK_FALSE(solve_cliff(c, after, nullptr, &error));
    CHECK(after.positions.empty());
    CHECK(error.find("cover") != std::string::npos);
    c.ground->heights.pop_back();
    CHECK_FALSE(cliff_in_range(c, error));
  }
}

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
  for (const RockType type : {RockType::Granite, RockType::Sandstone, RockType::Basalt,
                              RockType::Limestone, RockType::Quartzite, RockType::Dolostone}) {
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
  // Joint recesses and disconnected visible talus increase mixed-facing clusters; keep at
  // least two thirds cullable rather than pinning the old flat panels' three-quarter share.
  CHECK(coneless < plain.mesh.mesh.clusters.size() / 3);
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
  constexpr u64 k_mesh = 5793705113405651943ull;
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

TEST_CASE("cliffs: local talus leaves buried gaps and occupied deposits within the budget") {
  Cliff c = straight(80.0f, 24.0f, 120000);
  c.talus_blocks = 0;
  c.talus = 0.12f;
  for (RockQuality quality : {RockQuality::Medium, RockQuality::High}) {
    c.quality = quality;
    CliffReport r;
    const auto mesh = solve(c, &r);
    check_closed(mesh, r.parts);
    CHECK(r.triangles <= c.triangle_budget);
    std::vector<f32> tops(81, -1e6f);
    for (u32 i = 3 * r.face_triangles; i < 3 * (r.face_triangles + r.skirt_triangles); ++i) {
      const auto p = mesh.positions[mesh.indices[i]];
      if (p.z <= 0.0f) continue;
      const auto station = static_cast<u32>(std::clamp(p.x, 0.0f, 80.0f));
      tops[station] = std::max(tops[station], p.y);
    }
    u32 gaps = 0, deposits = 0;
    for (u32 i = 3; i < 78; ++i) {
      if (tops[i] > -1e5f && tops[i] < 0.0f) ++gaps;
      if (tops[i] > 0.3f) ++deposits;
    }
    CHECK(gaps >= 10);
    CHECK(deposits >= 10);
  }
  c.talus_coverage = 0.0f;
  std::string error;
  CHECK_FALSE(cliff_in_range(c, error));
  c.talus_coverage = 0.6f;
  c.talus_patch_m = std::numeric_limits<f32>::infinity();
  CHECK_FALSE(cliff_in_range(c, error));
}

TEST_CASE("cliffs: coarse carbonate faces retain broad relief after close joints filter out") {
  Cliff c = straight(60.0f, 24.0f, 250000);
  c.type = RockType::Limestone;
  c.seed = 47;
  c.quality = RockQuality::Low;
  c.batter = 0.0f;
  c.undercut = 0.0f;
  c.talus = 0.0f;
  c.talus_blocks = 0;
  c.geology.weathering = 0.0f;
  c.geology.irregularity = 0.0f;
  const auto spread = [](const assets::MeshData& mesh) {
    f32 lo = 1e6f, hi = -1e6f;
    for (u32 i = 0; i < mesh.positions.size(); ++i) {
      const auto p = mesh.positions[i];
      if (p.x < 5.0f || p.x > 55.0f || p.y < 4.0f || p.y > 18.0f || mesh.normals[i].z < 0.5f)
        continue;
      lo = std::min(lo, p.z);
      hi = std::max(hi, p.z);
    }
    REQUIRE(hi >= lo);
    return hi - lo;
  };
  c.relief = 0.0f;
  const f32 unjointed = spread(solve(c));
  c.relief = 8.0f;
  const auto coarse = solve(c);
  CHECK(spread(coarse) > unjointed + 0.1f);
  CHECK(coarse.indices.size() / 3 <= 25000);
}

TEST_CASE("cliffs: a bedded face's members step as risers and benches in their grounded ranges") {
  // Level beds, a cliff-former of 6 m under a weak member of `weak` metres, repeated; a face 30 m
  // high on ground at 50 degrees (the tracer's threshold) and at 63 (the escarpment's steepest
  // tiers). The smallest weak member the default steps (k_member_min_m, 1 m) and a large one
  // (20 m, a shale slope taller than its tier).
  for (const f32 weak : {1.0f, 20.0f}) {
    scene::Formation f;
    f.dip_deg = 0.0f;
    scene::FormationBed hard;
    hard.name = "sandstone";
    hard.thickness_m = 6.0f;
    hard.resistance = 0.9f;
    scene::FormationBed soft;
    soft.name = "shale";
    soft.thickness_m = weak;
    soft.resistance = 0.1f;
    f.beds.push_back(hard);
    f.beds.push_back(soft);
    detail::BenchBeds beds;
    REQUIRE(detail::bench_beds(f, beds));
    for (const f64 slope_deg : {50.0, 63.0}) {
      CAPTURE(weak);
      CAPTURE(slope_deg);
      const f64 batter = 1.0 / std::tan(slope_deg * 3.14159265358979323846 / 180.0);
      detail::BenchProfile p;
      detail::bench_profile(beds, 0.0, 0.0, 1.0, 0.0, 0.0, 30.0, batter, p);
      // The chord is the plane's: tiers still meet as they did.
      CHECK(p.setback(30.0) == doctest::Approx(batter * 30.0).epsilon(1e-9));
      const f64 bench_deg = std::atan(1.0 / p.bench_run) * 180.0 / 3.14159265358979323846;
      const f64 riser_deg =
          std::atan(1.0 / std::max(p.riser_run, 1e-12)) * 180.0 / 3.14159265358979323846;
      u32 benches = 0, risers = 0;
      for (u32 k = 0; k < p.count; ++k)
        (p.weak[k] != 0 ? benches : risers) += 1;
      MESSAGE("weak member " << weak << " m on " << slope_deg << " degrees: bench at " << bench_deg
                             << " degrees, riser at " << riser_deg << ", widest tread " << p.tread_m
                             << " m, " << benches << " bench and " << risers << " riser segments, "
                             << p.behind() << " m behind the plane at worst");
      CHECK(benches > 0);
      CHECK(risers > 0);
      // A bench near the angle of repose where the ground leaves the room (35 degrees), and never
      // outside 30 to 60; a riser at 80 or steeper.
      CHECK(bench_deg >= 30.0 - 1e-9);
      CHECK(bench_deg <= 60.0 + 1e-9);
      CHECK(riser_deg >= 80.0 - 1e-6);
      if (weak <= 1.0f) CHECK(bench_deg == doctest::Approx(35.0).epsilon(1e-6));
      CHECK(p.tread_m >= 0.0);
      // The profile only goes back: no bench or riser leans out over the one below.
      for (u32 k = 0; k < p.count; ++k)
        CHECK(p.s[k + 1] >= p.s[k] - 1e-12);
    }
  }
  // Without beds, or with beds that dip past 20 degrees, it is the plane.
  scene::Formation steep;
  steep.dip_deg = 35.0f;
  scene::FormationBed one;
  one.name = "sandstone";
  one.thickness_m = 3.0f;
  steep.beds.push_back(one);
  detail::BenchBeds none;
  CHECK_FALSE(detail::bench_beds(steep, none));
  detail::BenchProfile plane;
  detail::bench_profile(none, 0.0, 0.0, 1.0, 0.0, 0.0, 30.0, 0.5, plane);
  CHECK(plane.count == 1);
  CHECK(plane.behind() == 0.0);
}

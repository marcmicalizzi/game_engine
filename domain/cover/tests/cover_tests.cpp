// Tests for the cover capability (docs/subsystems/cover.md, "How to test"): the host query against
// brute force, the vine's walk (it keeps to the surface, it climbs, it attaches more on a rough
// host than a smooth one, it is the same bytes for the same seed), the cover's rules (a shaded
// north crevice by the ground over a sunlit smooth top) and its shaped pieces following the mask,
// and the two generators' containers.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <domain/assets/mesh_generator.h>
#include <domain/cover/cover.h>
#include <domain/cover/habitat.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace engine;

namespace {

// A box from `lo` to `hi`, each face an n x n grid wound counter-clockwise from outside.
void add_box(assets::GeneratorHostMesh& mesh, Vec3 lo, Vec3 hi, u32 n, f32 roughness) {
  struct Face {
    Vec3 origin, u, v;
  };
  const Vec3 d{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
  const Face faces[6] = {
      {{hi.x, lo.y, lo.z}, {0, 0, d.z}, {0, d.y, 0}},   // +x
      {{lo.x, lo.y, hi.z}, {0, 0, -d.z}, {0, d.y, 0}},  // -x
      {{lo.x, hi.y, lo.z}, {0, 0, d.z}, {d.x, 0, 0}},   // +y
      {{lo.x, lo.y, lo.z}, {d.x, 0, 0}, {0, 0, d.z}},   // -y
      {{hi.x, lo.y, hi.z}, {-d.x, 0, 0}, {0, d.y, 0}},  // +z
      {{lo.x, lo.y, lo.z}, {d.x, 0, 0}, {0, d.y, 0}},   // -z
  };
  for (const Face& f : faces) {
    const u32 base = mesh.positions.size();
    for (u32 j = 0; j <= n; ++j) {
      for (u32 i = 0; i <= n; ++i) {
        const f32 a = static_cast<f32>(i) / static_cast<f32>(n);
        const f32 b = static_cast<f32>(j) / static_cast<f32>(n);
        mesh.positions.push_back(Vec3{f.origin.x + f.u.x * a + f.v.x * b,
                                      f.origin.y + f.u.y * a + f.v.y * b,
                                      f.origin.z + f.u.z * a + f.v.z * b});
      }
    }
    // Which winding faces out: the cross of u and v points out for +x, -x? Decided per face by
    // the face's centre against the box's.
    const Vec3 c{lo.x + 0.5f * d.x, lo.y + 0.5f * d.y, lo.z + 0.5f * d.z};
    const Vec3 mid{f.origin.x + 0.5f * (f.u.x + f.v.x), f.origin.y + 0.5f * (f.u.y + f.v.y),
                   f.origin.z + 0.5f * (f.u.z + f.v.z)};
    const Vec3 nrm = cross(f.u, f.v);
    const bool flip = dot(nrm, Vec3{mid.x - c.x, mid.y - c.y, mid.z - c.z}) < 0.0f;
    for (u32 j = 0; j < n; ++j) {
      for (u32 i = 0; i < n; ++i) {
        const u32 a = base + j * (n + 1) + i;
        const u32 b = a + 1;
        const u32 cc = a + (n + 1);
        const u32 dd = cc + 1;
        const u32 tri[6] = {a, b, dd, a, dd, cc};
        for (u32 k = 0; k < 6; k += 3) {
          mesh.indices.push_back(tri[k]);
          mesh.indices.push_back(flip ? tri[k + 2] : tri[k + 1]);
          mesh.indices.push_back(flip ? tri[k + 1] : tri[k + 2]);
          mesh.roughness.push_back(roughness);
          mesh.holds.push_back(1u);
        }
      }
    }
  }
}

assets::GeneratorHostMesh cube(f32 roughness, u32 n = 8) {
  assets::GeneratorHostMesh mesh;
  add_box(mesh, Vec3{-1.0f, 0.0f, -1.0f}, Vec3{1.0f, 2.0f, 1.0f}, n, roughness);
  mesh.identity = 0x1234u;
  return mesh;
}

assets::GeneratorHostMesh sphere(u32 rings, u32 segments) {
  assets::GeneratorHostMesh mesh;
  for (u32 r = 0; r <= rings; ++r) {
    const f32 theta = 3.14159265f * static_cast<f32>(r) / static_cast<f32>(rings);
    for (u32 s = 0; s <= segments; ++s) {
      const f32 phi = 6.2831853f * static_cast<f32>(s) / static_cast<f32>(segments);
      mesh.positions.push_back(Vec3{std::sin(theta) * std::cos(phi), 1.0f + std::cos(theta),
                                    std::sin(theta) * std::sin(phi)});
    }
  }
  for (u32 r = 0; r < rings; ++r) {
    for (u32 s = 0; s < segments; ++s) {
      const u32 a = r * (segments + 1) + s;
      const u32 b = a + segments + 1;
      const u32 tri[6] = {a, b, a + 1, a + 1, b, b + 1};
      for (u32 k = 0; k < 6; ++k)
        mesh.indices.push_back(tri[k]);
      mesh.roughness.push_back(0.8f);
      mesh.roughness.push_back(0.8f);
      mesh.holds.push_back(1u);
      mesh.holds.push_back(1u);
    }
  }
  return mesh;
}

// The brute-force closest point: every triangle, by the same exact routine's result distance.
f64 brute_distance(const cover::HostSurface& surface, cover::P3 p) {
  f64 best = 1e300;
  for (u32 t = 0; t < surface.triangle_count(); ++t) {
    // Sample the triangle densely enough to bound the true distance from above, and use the
    // plane distance where the projection falls inside.
    const cover::P3 a = surface.corner(t, 0);
    const cover::P3 b = surface.corner(t, 1);
    const cover::P3 c = surface.corner(t, 2);
    for (u32 i = 0; i <= 40; ++i) {
      for (u32 j = 0; i + j <= 40; ++j) {
        const f64 u = static_cast<f64>(i) / 40.0;
        const f64 v = static_cast<f64>(j) / 40.0;
        const cover::P3 q = a + (b - a) * u + (c - a) * v;
        const f64 d = cover::length_of(p - q);
        best = d < best ? d : best;
      }
    }
  }
  return best;
}

u64 mesh_hash(const assets::MeshData& mesh) {
  u64 h = hash_bytes(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
  h = hash_combine(h, hash_bytes(mesh.indices.data(), mesh.indices.size() * sizeof(u32)));
  return h;
}

cover::HostSite site() {
  cover::HostSite s;  // sun from the south at 45 degrees, ground at y = 0
  return s;
}

}  // namespace

TEST_CASE("the host query's closest point agrees with brute force on a sphere and a cube") {
  for (const assets::GeneratorHostMesh& mesh : {sphere(12, 16), cube(0.5f, 3)}) {
    cover::Host host;
    std::string error;
    REQUIRE(cover::prepare_host(mesh, host, error));
    u64 state = 7;
    for (u32 i = 0; i < 40; ++i) {
      const auto draw = [&]() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<f64>(state >> 11) * (1.0 / 9007199254740992.0) * 4.0 - 2.0;
      };
      const cover::P3 p{draw(), draw() + 1.0, draw()};
      cover::HostHit hit;
      REQUIRE(host.bvh.closest(p, 100.0, false, hit));
      const f64 brute = brute_distance(host.surface, p);
      // The sampled brute force is an upper bound within the sampling's spacing of the truth.
      CHECK(hit.distance <= brute + 1e-9);
      CHECK(hit.distance >= brute - 0.06);
      CHECK(cover::length_of(p - hit.point) == doctest::Approx(hit.distance).epsilon(1e-9));
    }
  }
}

TEST_CASE("the host query sees occlusion and the weld joins a cube's faces") {
  cover::Host host;
  std::string error;
  REQUIRE(cover::prepare_host(cube(0.5f, 2), host, error));
  // 6 faces of 3 x 3 vertices share their edges: 26 distinct positions.
  CHECK(host.surface.welded == 26u);
  CHECK(host.bvh.occluded(cover::P3{0.0, 1.0, -5.0}, cover::P3{0.0, 0.0, 1.0}, 10.0));
  CHECK_FALSE(host.bvh.occluded(cover::P3{0.0, 3.0, -5.0}, cover::P3{0.0, 0.0, 1.0}, 10.0));
}

TEST_CASE("a vine grown on a cube keeps to its surface and climbs") {
  cover::Host host;
  std::string error;
  REQUIRE(cover::prepare_host(cube(0.9f), host, error));
  cover::Vine vine;
  vine.seeds = 4;
  Vector<cover::VineStem> stems;
  cover::VineReport report;
  REQUIRE(cover::grow_vine(host.surface, host.bvh, site(), vine, stems, report, error));
  CHECK(report.seeds == 4u);
  CHECK(report.stems >= 4u);
  f64 worst = 0.0;
  for (const cover::VineStem& stem : stems) {
    for (u32 i = 0; i < stem.points.size(); ++i) {
      if (stem.held[i] == 0) continue;
      cover::HostHit hit;
      REQUIRE(host.bvh.closest(stem.points[i], 1.0, false, hit));
      worst = hit.distance > worst ? hit.distance : worst;
    }
  }
  MESSAGE("held points stand at most " << worst << " m off the cube; seeds at "
                                       << report.seed_height << " m, tips at " << report.tip_height
                                       << " m");
  CHECK(worst < 0.002);
  CHECK(report.tip_height > report.seed_height + 0.5);
  CHECK(report.held_steps > report.steps / 2u);
}

TEST_CASE("a vine attaches more on a rough host than on a smooth one") {
  std::string error;
  cover::Vine vine;
  vine.seeds = 4;
  cover::VineReport rough_report;
  cover::VineReport smooth_report;
  {
    cover::Host host;
    REQUIRE(cover::prepare_host(cube(0.9f), host, error));
    Vector<cover::VineStem> stems;
    REQUIRE(cover::grow_vine(host.surface, host.bvh, site(), vine, stems, rough_report, error));
  }
  {
    cover::Host host;
    REQUIRE(cover::prepare_host(cube(0.1f), host, error));
    Vector<cover::VineStem> stems;
    REQUIRE(cover::grow_vine(host.surface, host.bvh, site(), vine, stems, smooth_report, error));
  }
  MESSAGE("held steps: rough " << rough_report.held_steps << ", smooth " << smooth_report.held_steps
                               << "; leaning on the smooth " << smooth_report.leaning_steps);
  CHECK(rough_report.held_steps > 10u * (smooth_report.held_steps + 1u));
  CHECK(rough_report.tip_height > smooth_report.tip_height);
}

TEST_CASE("the same seed grows the same vine, and the mesh's bytes are pinned") {
  cover::Host host;
  std::string error;
  REQUIRE(cover::prepare_host(cube(0.9f), host, error));
  cover::Vine vine;
  assets::MeshData a;
  assets::MeshData b;
  flora::Motion ma;
  flora::Motion mb;
  cover::VineReport report;
  REQUIRE(cover::solve_vine(host.surface, host.bvh, site(), vine, a, ma, &report, &error));
  REQUIRE(cover::solve_vine(host.surface, host.bvh, site(), vine, b, mb, nullptr, &error));
  CHECK(mesh_hash(a) == mesh_hash(b));
  vine.seed = 2;
  assets::MeshData c;
  flora::Motion mc;
  REQUIRE(cover::solve_vine(host.surface, host.bvh, site(), vine, c, mc, nullptr, &error));
  CHECK(mesh_hash(a) != mesh_hash(c));
  // Part 0 is a root with no geometry, every stem a part of its own, no part load-bearing.
  REQUIRE(ma.parts.size() == report.stems + 1u);
  for (const u32 part : ma.vertex_part)
    CHECK(part != 0u);
  for (const geometry::ClusterFileStructuralPart& record : ma.structure)
    CHECK(record.flags == 0u);
  char text[24];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(mesh_hash(a)));
  MESSAGE("vine on the cube: " << text << ", " << report.triangles << " triangles, "
                               << report.leaves << " leaves, " << report.stems << " stems");
  // Pinned on MSVC; GCC and Clang at v3 and v2 reproduce it (cover.md, "Determinism").
  CHECK(std::string(text) == "c6e1fe3ff8177894");
}

TEST_CASE("moss favours a shaded north crevice by the ground over a sunlit smooth top") {
  // A block with a second one 0.15 m to its north: the gap between them is a north-facing crevice
  // at the ground, under a sun from the south at 45 degrees. The top is open to the sun.
  assets::GeneratorHostMesh mesh;
  add_box(mesh, Vec3{-1.0f, 0.0f, -1.0f}, Vec3{1.0f, 2.0f, 1.0f}, 8, 0.6f);
  add_box(mesh, Vec3{-1.0f, 0.0f, -2.65f}, Vec3{1.0f, 2.0f, -1.15f}, 8, 0.6f);
  cover::Host host;
  std::string error;
  REQUIRE(cover::prepare_host(mesh, host, error));
  cover::CoverGrowth growth;
  Vector<cover::Exposure> exposure;
  cover::host_exposure(host.surface, host.bvh, site(), 0.6, 32, exposure);
  // The triangles to compare: the first block's north face (z = -1, facing -z) low down, and its
  // top's middle.
  f64 crevice = 0.0;
  f64 top = 0.0;
  u32 crevice_n = 0;
  u32 top_n = 0;
  for (u32 t = 0; t < host.surface.triangle_count(); ++t) {
    const cover::P3 c = host.surface.centroid(t);
    const cover::P3 n = host.surface.face_normal[t];
    const cover::Suitability s = cover::cover_suitability(exposure[t], 0.6, site(), growth);
    if (n.z < -0.9 && c.z > -1.01 && c.z < -0.99 && c.y < 0.5) {
      crevice += s.kind[cover::k_moss];
      ++crevice_n;
    }
    if (n.y > 0.9 && c.y > 1.99 && c.z > -0.5 && c.z < 0.5 && c.x > -0.5 && c.x < 0.5) {
      top += s.kind[cover::k_moss];
      ++top_n;
    }
  }
  REQUIRE(crevice_n > 0u);
  REQUIRE(top_n > 0u);
  crevice /= crevice_n;
  top /= top_n;
  MESSAGE("moss suitability: crevice " << crevice << ", sunlit top " << top);
  CHECK(crevice > top + 0.2);
  // And the lichen goes the other way: the sunlit top is its.
  f64 lichen_top = 0.0;
  f64 lichen_crevice = 0.0;
  for (u32 t = 0; t < host.surface.triangle_count(); ++t) {
    const cover::P3 c = host.surface.centroid(t);
    const cover::P3 n = host.surface.face_normal[t];
    const cover::Suitability s = cover::cover_suitability(exposure[t], 0.6, site(), growth);
    if (n.z < -0.9 && c.z > -1.01 && c.z < -0.99 && c.y < 0.5)
      lichen_crevice += s.kind[cover::k_lichen];
    if (n.y > 0.9 && c.y > 1.99 && c.z > -0.5 && c.z < 0.5 && c.x > -0.5 && c.x < 0.5)
      lichen_top += s.kind[cover::k_lichen];
  }
  CHECK(lichen_top / top_n > lichen_crevice / crevice_n);
}

TEST_CASE("the shaped cover's count follows the mask") {
  cover::Host host;
  std::string error;
  REQUIRE(cover::prepare_host(cube(0.9f), host, error));
  cover::HostSite shaded = site();
  shaded.canopy_shade = 0.8f;
  shaded.wetness = 0.9f;
  cover::CoverGrowth growth;
  growth.cushions_per_m2 = 60.0f;
  const auto run = [&](f32 age, f32 moss, cover::CoverReport& report) {
    growth.age_years = age;
    growth.moss = moss;
    assets::MeshData mesh;
    flora::Motion motion;
    Vector<u32> mask;
    REQUIRE(cover::solve_cover(host.surface, host.bvh, shaded, growth, mesh, motion, &mask, &report,
                               &error));
    CHECK(mask.size() == host.surface.positions.size());
  };
  cover::CoverReport young;
  cover::CoverReport old;
  cover::CoverReport sparse;
  run(2.0f, 1.0f, young);
  run(40.0f, 1.0f, old);
  run(40.0f, 0.3f, sparse);
  MESSAGE("moss mean " << young.mean[cover::k_moss] << " -> " << old.mean[cover::k_moss]
                       << "; cushions " << young.cushions << " -> " << old.cushions << " ("
                       << sparse.cushions << " at a third of the moss)");
  CHECK(old.mean[cover::k_moss] > young.mean[cover::k_moss]);
  CHECK(old.cushions > young.cushions);
  CHECK(sparse.cushions < old.cushions);
  // The count is the mask's integral times the density, within a few standard deviations.
  const f64 expected = old.mean[cover::k_moss] * old.holding_area_m2 * 60.0;
  CHECK(static_cast<f64>(old.cushions) > expected - 4.0 * std::sqrt(expected) - 2.0);
  CHECK(static_cast<f64>(old.cushions) < expected + 4.0 * std::sqrt(expected) + 2.0);
}

TEST_CASE("surface colonization separates substrate from roughness and surface age") {
  cover::CoverGrowth g;
  g.dead_wood = true;  // explicit stone overrides this compatibility flag
  g.habitat.substrate = cover::Substrate::siliceous_rock;
  g.age_years = 500.0f;
  cover::Exposure e{1.0f, 0.0f, 0.7f, 1.0f, 1.0f, 1000.0f};
  const auto suitable = [&] { return cover::cover_suitability(e, 0.9, site(), g); };
  CHECK(suitable().kind[cover::k_fungus] == 0.0);
  g.lichen_substrate = cover::LichenSubstrate::calcareous;
  CHECK(suitable().kind[cover::k_lichen] == 0.0);
  g.habitat.substrate = cover::Substrate::calcareous_rock;
  CHECK(suitable().kind[cover::k_lichen] > 0.0);
  g.habitat.substrate = cover::Substrate::dead_wood;
  CHECK(suitable().kind[cover::k_fungus] == 0.0);  // old surface, fresh wood
  g.habitat.decay = 0.5f;
  CHECK(suitable().kind[cover::k_fungus] > 0.0);
  g.habitat.substrate = cover::Substrate::living_bark;
  CHECK(suitable().kind[cover::k_fungus] == 0.0);
  g.habitat.substrate = cover::Substrate::sealed;
  for (const f64 s : suitable().kind)
    CHECK(s == 0.0);
  g.habitat.substrate = cover::Substrate::siliceous_rock;
  g.moss_crevice_bias = 1.0f;
  CHECK(suitable().kind[cover::k_moss] == 0.0);
  e.sky = 0.1f;
  CHECK(suitable().kind[cover::k_moss] > 0.0);
}

TEST_CASE("nurse-log establishment needs upward decayed penetrable wood and a shallow mat") {
  cover::HostHabitat h;
  cover::Exposure e{1, 0, 0, 0.5f, 1, 1000};
  h.substrate = cover::Substrate::dead_wood;
  h.decay = 0.5f;
  h.rooting_depth_m = 0.04f;
  const f64 good = cover::nurse_log_suitability(h, e, 0.8);
  CHECK(good > 0.0);
  h.bryophyte_depth_m = 0.1f;
  CHECK(cover::nurse_log_suitability(h, e, 0.8) == 0.0);
  h.bryophyte_depth_m = 0.0f;
  h.rooting_depth_m = 0.0f;
  CHECK(cover::nurse_log_suitability(h, e, 0.8) == 0.0);
  h.rooting_depth_m = 0.04f;
  h.decay = 0.0f;
  CHECK(cover::nurse_log_suitability(h, e, 0.8) == 0.0);
  h.decay = 0.5f;
  e.up = 0.0f;
  CHECK(cover::nurse_log_suitability(h, e, 0.8) == 0.0);
  e.up = -1.0f;
  CHECK(cover::nurse_log_suitability(h, e, 0.8) == 0.0);
  e.up = 1.0f;
  h.substrate = cover::Substrate::soil;
  CHECK(cover::nurse_log_suitability(h, e, 0.8) == 0.0);
}

TEST_CASE("surface habitat validation and canonical identity include every ecological input") {
  cover::CoverFile file;
  std::string error;
  REQUIRE(cover::read_cover_file(R"({"host":"log.deadwood.json","cover":{
    "habitat":{"substrate":"dead_wood","decay":0.5,"rooting_depth_m":0.04},
    "plants_per_m2":20,"moss_crevice_bias":0.5}})",
                                 file, error));
  const u64 key = cover::cover_source_hash(file, 42);
  cover::CoverFile copy;
  REQUIRE(cover::read_cover_file(cover::canonical_cover_json(file), copy, error));
  CHECK(cover::cover_source_hash(copy, 42) == key);
  auto endpoint = copy.cover;
  endpoint.plant_leaf_m = 0.02f;
  CHECK(cover::cover_in_range(endpoint, error));
  endpoint.plant_leaf_m = 0.019f;
  CHECK_FALSE(cover::cover_in_range(endpoint, error));
  file.cover.habitat.decay = 0.6f;
  CHECK(cover::cover_source_hash(file, 42) != key);
  file.cover.habitat.decay = std::numeric_limits<f32>::quiet_NaN();
  CHECK_FALSE(cover::cover_in_range(file.cover, error));
  file.cover = copy.cover;
  cover::HabitatRegion r;
  r.min = {-1, -1, -1};
  r.max = {1, 1, 1};
  r.habitat.substrate = cover::Substrate::sealed;
  file.cover.regions.push_back(r);
  CHECK(cover::cover_source_hash(file, 42) != key);
  CHECK(cover::habitat_at(file.cover, {0, 0, 0}).substrate == cover::Substrate::sealed);
  CHECK(cover::habitat_at(file.cover, {2, 0, 0}).substrate == cover::Substrate::dead_wood);
  r.habitat.substrate = cover::Substrate::living_bark;
  file.cover.regions.push_back(r);
  CHECK(cover::habitat_at(file.cover, {0, 0, 0}).substrate == cover::Substrate::living_bark);
  file.cover.regions[0].max.x = file.cover.regions[0].min.x;
  CHECK_FALSE(cover::cover_in_range(file.cover, error));
  CHECK(error.find("regions[0]") != std::string::npos);
  CHECK_FALSE(cover::read_cover_file(R"({"host":"x","cover":{"habitat":{"substrate":"bogus"}}})",
                                     copy, error));
}

TEST_CASE("surface masks do not diffuse fungi onto stone or any cover through sealed regions") {
  cover::Host host;
  std::string error;
  REQUIRE(cover::prepare_host(cube(0.9f, 4), host, error));
  cover::CoverGrowth g;
  g.habitat.substrate = cover::Substrate::dead_wood;
  g.habitat.decay = 0.7f;
  g.patchiness = 0.0f;
  g.smoothing = 32;
  cover::HabitatRegion r;
  r.min = {0, -1, -2};
  r.max = {2, 3, 2};
  r.habitat.substrate = cover::Substrate::siliceous_rock;
  g.regions.push_back(r);
  Vector<cover::Exposure> exposure;
  exposure.resize(host.surface.triangle_count(), cover::Exposure{1, 1, 0, 0.2f, 0, 0});
  Vector<f32> mask;
  cover::solve_cover_mask(host.surface, exposure, site(), g, mask);
  u32 fungi = 0;
  for (u32 v = 0; v < host.surface.positions.size(); ++v) {
    const f32 value = mask[host.surface.weld[v] * 4 + cover::k_fungus];
    if (host.surface.positions[v].x >= 0)
      CHECK(value == 0.0f);
    else if (value > 0.0f)
      ++fungi;
  }
  CHECK(fungi > 0);
  g.regions[0].habitat.substrate = cover::Substrate::sealed;
  cover::solve_cover_mask(host.surface, exposure, site(), g, mask);
  for (u32 v = 0; v < host.surface.positions.size(); ++v)
    if (host.surface.positions[v].x >= 0)
      for (u32 k = 0; k < 4; ++k)
        CHECK(mask[host.surface.weld[v] * 4 + k] == 0.0f);
}

TEST_CASE("nurse-log plants share the cover budget and are deterministic noncolliding companions") {
  cover::Host host;
  std::string error;
  REQUIRE(cover::prepare_host(cube(0.9f, 2), host, error));
  cover::CoverGrowth g;
  g.habitat.substrate = cover::Substrate::dead_wood;
  g.habitat.decay = 0.7f;
  g.habitat.rooting_depth_m = 0.04f;
  g.plants_per_m2 = 200;
  g.moss = g.lichen = g.algae = g.fungus = 0;
  g.triangle_budget = 500;
  auto s = site();
  s.wetness = 1;
  s.canopy_shade = 0.8f;
  assets::MeshData mesh, again;
  flora::Motion motion, again_motion;
  cover::CoverReport report;
  Vector<u32> mask;
  REQUIRE(cover::solve_cover(host.surface, host.bvh, s, g, mesh, motion, &mask, &report, &error));
  CHECK(report.plants > 0);
  CHECK(report.triangles == report.plants * 12);
  CHECK(report.triangles <= g.triangle_budget);
  CHECK(report.plant_area_m2 > 0);
  CHECK(report.density_scale < 1.0);
  for (const u32 word : mask)
    CHECK(word == 0);
  for (const auto& part : motion.structure)
    CHECK(part.flags == 0);
  for (const auto weight : motion.vertex_weight)
    CHECK(weight == 0.0f);
  REQUIRE(cover::solve_cover(host.surface, host.bvh, s, g, again, again_motion, nullptr, nullptr,
                             &error));
  CHECK(mesh_hash(mesh) == mesh_hash(again));
  char pin[32];
  std::snprintf(pin, sizeof(pin), "%016llx", static_cast<unsigned long long>(mesh_hash(mesh)));
  MESSAGE("nurse-log mesh: " << pin << "; plants " << report.plants);
  CHECK(std::string(pin) ==
        "25201b473dd0580a");  // G124, first MSVC pin; 25 plants / 300 triangles.

  cover::CoverFile recipe;
  recipe.host = "nurse-log.clusters";
  recipe.site = s;
  recipe.cover = g;
  geometry::ClusterFileData container;
  REQUIRE(cover::build_cover_container(recipe, host, 123, {}, 65536, nullptr, container, nullptr,
                                       error));
  CHECK(container.cover_mask.size() == host.surface.positions.size());
  CHECK(container.materials.size() == 5);
  std::snprintf(pin, sizeof(pin), "%016llx",
                static_cast<unsigned long long>(geometry::cluster_file_hash(container)));
  MESSAGE("nurse-log container: " << pin);
  CHECK(std::string(pin) == "4fa3b4dddeb82baf");  // G124, section-40/motion/LOD/container pin.

  // Nonholding host parts must not inflate the reported suitable seedbed area.
  u32 top = 0;
  for (u32 t = 0; t < host.surface.triangle_count(); ++t)
    if (host.surface.face_normal[t].y > 0.5 && (++top % 2u) == 0) host.surface.holds[t] = 0;
  cover::CoverReport restricted;
  REQUIRE(cover::solve_cover(host.surface, host.bvh, s, g, again, again_motion, nullptr,
                             &restricted, &error));
  CHECK(restricted.plant_area_m2 > 0.0);
  CHECK(restricted.plant_area_m2 < report.plant_area_m2);
}

TEST_CASE("the generators build companions through the host seam, the mask in section 40") {
  const assets::MeshGeneratorRegistry& registry = assets::MeshGeneratorRegistry::global();
  const assets::MeshGeneratorDesc* vine = registry.find("vine");
  const assets::MeshGeneratorDesc* cover = registry.find("cover");
  REQUIRE(vine != nullptr);
  REQUIRE(cover != nullptr);
  struct Context {
    assets::GeneratorHostMesh mesh;
  } context{cube(0.9f)};
  assets::MeshGeneratorHosts hosts;
  hosts.context = &context;
  hosts.identify = [](void* c, std::string_view, u64& out, std::string&) {
    out = static_cast<Context*>(c)->mesh.identity;
    return true;
  };
  hosts.load = [](void* c, std::string_view, assets::GeneratorHostMesh& out, std::string&) {
    out = static_cast<Context*>(c)->mesh;
    return true;
  };
  const assets::MeshGeneratorInputs inputs{nullptr, &hosts};
  const std::string vine_text = R"({"host": "cube.clusters", "vine": {"seeds": 2}})";
  const std::string cover_text =
      R"({"host": "cube.clusters", "site": {"canopy_shade": 0.7, "wetness": 0.9}})";
  const auto bytes = [](const std::string& s) {
    return std::span<const u8>(reinterpret_cast<const u8*>(s.data()), s.size());
  };
  u64 hash = 0;
  std::string error;
  // Without a host reader a companion has no key.
  CHECK_FALSE(vine->source_hash(bytes(vine_text), hash, error));
  REQUIRE(assets::generated_source_hash(*vine, bytes(vine_text), inputs, hash, error));
  const u64 first = hash;
  context.mesh.identity = 0x5678u;
  REQUIRE(assets::generated_source_hash(*vine, bytes(vine_text), inputs, hash, error));
  CHECK(hash != first);  // a changed host is a new key
  geometry::ClusterLodOptions lod;
  geometry::ClusterFileData vine_data;
  INFO(error);
  REQUIRE(vine->build(bytes(vine_text), inputs, lod, 65536u, vine_data, error));
  CHECK(vine_data.source_hash == hash);
  CHECK(vine_data.generator == "vine");
  CHECK(vine_data.motion_parts.size() > 1u);
  CHECK(vine_data.cover_mask.empty());
  geometry::ClusterFileData cover_data;
  REQUIRE(cover->build(bytes(cover_text), inputs, lod, 65536u, cover_data, error));
  CHECK(cover_data.cover_mask.size() == context.mesh.positions.size());
  // The section round-trips.
  const test::TempDir dir("cover");
  const std::string file = dir.file("cover.clusters");
  geometry::ClusterFileData back;
  REQUIRE(geometry::write_cluster_file(file, cover_data, &error));
  REQUIRE(geometry::read_cluster_file(file, back, &error));
  CHECK(back.cover_mask.size() == cover_data.cover_mask.size());
  CHECK(geometry::cluster_file_hash(back) == geometry::cluster_file_hash(cover_data));
  char text[48];
  std::snprintf(text, sizeof(text), "%016llx %016llx",
                static_cast<unsigned long long>(geometry::cluster_file_hash(vine_data)),
                static_cast<unsigned long long>(geometry::cluster_file_hash(cover_data)));
  MESSAGE("containers: " << text);
  // G124: cover version 2 records habitat inputs; the vine stays at its version-1 pin.
  // Before G124 (base 86d76acd): cover 09e1eaf9313d8022. Only canonical metadata moved.
  CHECK(std::string(text) == "9cb906a303a4611a e3e7eb494653eab9");
  // Restore only version-1 metadata, and the entire legacy container must match its old pin.
  // This proves geometry, materials, mask, motion and LOD are unchanged, not merely repeatable.
  JsonValue legacy;
  REQUIRE(parse_json(cover_data.generator_params, legacy).ok);
  auto& old_growth = legacy.find("cover")->as_object();
  for (const char* field : {"habitat", "regions", "lichen_substrate", "moss_crevice_bias",
                            "plants_per_m2", "plant_leaf_m"})
    old_growth.erase(std::string(field));
  cover_data.generator_params = write_json(legacy, JsonWriteOptions{.pretty = false});
  u64 old_hash = hash_bytes(cover_data.generator_params.data(), cover_data.generator_params.size());
  old_hash = hash_combine(old_hash, hash_bytes("cover", 5));
  old_hash = hash_combine(old_hash, 1);
  old_hash = hash_combine(old_hash, context.mesh.identity);
  cover_data.source_hash = old_hash;
  cover_data.build_key = assets::generated_mesh_build_key(old_hash, inputs, lod, 65536u);
  CHECK(geometry::cluster_file_hash(cover_data) == 0x09e1eaf9313d8022ull);
}

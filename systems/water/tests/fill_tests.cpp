// A water body that fills (docs/subsystems/water.md, "A body that fills"; roadmap S97), no device:
// the flood's outline over a paraboloid is the circle where the level meets it, within a cell; the
// mesh covers the region and nothing above the level; a hollow the flood does not reach stays dry;
// a region with an island (not star-shaped) is meshed round it; the refusals; the same bytes twice
// and a pinned hash; and the block's new fields read and refused.
#include <core/hash/hash.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/water/water.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>
#include <string>

using namespace engine;

namespace {

// The paraboloid of water_tests.cpp: round (100, 50), 2 m deep at its centre, rising 2 cm per
// square metre of radius, so a level at 0 meets it ten metres out.
f64 paraboloid(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0 - 100.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0 - 50.0;
  return -2.0 + 0.02 * (x * x + z * z);
}

// Two such bowls 40 m apart along x, the second shallower (1 m), with a ridge between them.
f64 two_bowls(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  const f64 a = -2.0 + 0.02 * ((x - 100.0) * (x - 100.0) + (z - 50.0) * (z - 50.0));
  const f64 b = -1.0 + 0.02 * ((x - 140.0) * (x - 140.0) + (z - 50.0) * (z - 50.0));
  return std::min(a, b);
}

// A moat: the paraboloid with an island rising in its middle, its top 1 m above the level, so the
// water is a ring between r = 3 and r = 10 (the island's flank meets 0 at r = 3).
f64 moat(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0 - 100.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0 - 50.0;
  const f64 r2 = x * x + z * z;
  const f64 bowl = -2.0 + 0.02 * r2;
  const f64 island = 1.0 - r2 / 9.0;  // 0 at r = 3
  return std::max(bowl, island);
}

scene_gen::Ground ground_of(f64 (*fn)(const void*, i64, i64) noexcept) {
  scene_gen::Ground g;
  g.surface_fn = fn;
  return g;
}

scene::WaterBody fill_block(f64 sx, f64 sz, f64 level, f32 extent = 60.0f) {
  scene::WaterBody b;
  b.name = "lake";
  b.level_m = level;
  scene::WaterFill f;
  f.seed = WorldPos{sx, 0.0, sz};
  f.max_extent_m = extent;
  b.shape.fill = f;
  return b;
}

water::Body filled(const scene::WaterBody& block, const scene_gen::Ground& ground) {
  water::Body body;
  std::string error;
  REQUIRE_MESSAGE(water::read_body(block, body, &error), error);
  REQUIRE_MESSAGE(water::fill_body(body, ground, &error), error);
  return body;
}

f64 mesh_area(const water::Mesh& mesh) {
  f64 area = 0.0;
  for (u32 t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const Vec3 a = mesh.positions[mesh.indices[t]];
    const Vec3 b = mesh.positions[mesh.indices[t + 1]];
    const Vec3 c = mesh.positions[mesh.indices[t + 2]];
    area += 0.5 *
            (static_cast<f64>(c.x - a.x) * static_cast<f64>(b.z - a.z) -
                   static_cast<f64>(b.x - a.x) * static_cast<f64>(c.z - a.z));
  }
  return area;
}

}  // namespace

TEST_CASE("water fill: over a paraboloid the outline is the level's circle, within a cell") {
  const scene_gen::Ground ground = ground_of(&paraboloid);
  const auto start = std::chrono::steady_clock::now();
  const water::Body body = filled(fill_block(103.0, 48.0, 0.0), ground);
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  const f64 ms =
      std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
  const water::FillRegion& r = body.region;
  MESSAGE("paraboloid fill: spacing " << r.spacing << " m, " << r.inside << " nodes inside, "
                                      << r.crossings << " edge points, " << mesh.positions.size()
                                      << " vertices, " << mesh.indices.size() / 3
                                      << " triangles, flood and mesh " << ms << " ms");
  CHECK(body.filled());
  CHECK(r.spacing == doctest::Approx(2.0f / 3.0f));
  // One loop, every point of it on the circle of radius 10 about (100, 50), the body's centre at
  // the seed (103, 48): within a cell, and in fact within the trace's simplification and the
  // bilinear zero's offset from the paraboloid's, a few centimetres.
  REQUIRE(r.loop_starts.size() == 1u);
  REQUIRE(r.loop_points.size() > 20u);
  f64 worst = 0.0;
  for (const Vec2& p : r.loop_points) {
    const f64 x = static_cast<f64>(p.x) + 3.0, z = static_cast<f64>(p.y) - 2.0;
    worst = std::max(worst, std::fabs(std::sqrt(x * x + z * z) - 10.0));
  }
  MESSAGE("outline: " << r.loop_points.size() << " points, worst " << worst << " m off the circle");
  CHECK(worst < static_cast<f64>(r.spacing));
  CHECK(worst < 0.1);
  // The mesh covers the disc: its area within 1% of pi 10^2, every triangle facing +y, every
  // vertex's ground at or below the level (the edge's points to the bilinear zero's few
  // millimetres) and its depth the level less the ground, and no triangle's middle above it.
  const f64 area = mesh_area(mesh);
  CHECK(area == doctest::Approx(3.14159265 * 100.0).epsilon(0.01));
  for (u32 v = 0; v < mesh.positions.size(); ++v) {
    const Vec3 p = mesh.positions[v];
    CHECK(p.y == 0.0f);
    const WorldPos at{body.center.x + static_cast<f64>(p.x), 0.0,
                      body.center.z + static_cast<f64>(p.z)};
    const f64 ground_y = ground.surface(at);
    CHECK(ground_y <= 0.005);
    CHECK(mesh.uvs[v].y == static_cast<f32>(std::max(0.0, -ground_y)));
    CHECK(mesh.uvs[v].x >= 0.0f);
  }
  u32 above = 0;
  for (u32 t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const Vec3 a = mesh.positions[mesh.indices[t]];
    const Vec3 b = mesh.positions[mesh.indices[t + 1]];
    const Vec3 c = mesh.positions[mesh.indices[t + 2]];
    const f32 ny = (c.x - a.x) * (b.z - a.z) - (b.x - a.x) * (c.z - a.z);
    CHECK(ny >= 0.0f);
    const WorldPos mid{body.center.x + static_cast<f64>(a.x + b.x + c.x) / 3.0, 0.0,
                       body.center.z + static_cast<f64>(a.z + b.z + c.z) / 3.0};
    if (ground.surface(mid) > 0.005) ++above;
  }
  CHECK(above == 0u);
  // The depth map spans the region, as a circle's does its ring.
  water::DepthMap map;
  water::build_depth_map(body, mesh, ground, map);
  CHECK(map.size >= 64u);
  CHECK(map.depth_max == doctest::Approx(2.0f).epsilon(0.01));
  // The region's inside answers depth_at, and the bank outside it does not.
  const water::Body bodies[] = {body};
  CHECK(water::depth_at(bodies, ground, WorldPos{100.0, 0.0, 50.0}) == doctest::Approx(2.0f));
  CHECK(water::depth_at(bodies, ground, WorldPos{109.0, 0.0, 50.0}) > 0.3f);
  CHECK(water::depth_at(bodies, ground, WorldPos{111.0, 0.0, 50.0}) == 0.0f);
}

TEST_CASE("water fill: the same bytes twice, a pinned hash, and the GLB's own reading") {
  const scene_gen::Ground ground = ground_of(&paraboloid);
  const water::Body body = filled(fill_block(103.0, 48.0, 0.0), ground);
  water::Mesh a, b;
  water::build_mesh(body, ground, a);
  const water::Body again = filled(fill_block(103.0, 48.0, 0.0), ground);
  water::build_mesh(again, ground, b);
  const Vector<u8> ga = water::mesh_glb(a, Vec3{0.3f, 0.55f, 0.5f}, 0.08f);
  const Vector<u8> gb = water::mesh_glb(b, Vec3{0.3f, 0.55f, 0.5f}, 0.08f);
  REQUIRE(ga.size() == gb.size());
  CHECK(std::equal(ga.begin(), ga.end(), gb.begin()));
  const u64 h = hash_bytes(ga.data(), ga.size(), 0);
  MESSAGE("fill GLB: " << ga.size() << " bytes, hash " << std::hex << h);
  // Taken on MSVC (2026-11-07); GCC and Clang at v3 must reproduce it (water.md, "Determinism").
  CHECK(h == 0xa520edff3e19d9baull);
}

TEST_CASE("water fill: a hollow the flood does not reach stays dry") {
  const scene_gen::Ground ground = ground_of(&two_bowls);
  const water::Body body = filled(fill_block(100.0, 50.0, 0.0), ground);
  const water::Body bodies[] = {body};
  CHECK(water::depth_at(bodies, ground, WorldPos{100.0, 0.0, 50.0}) == doctest::Approx(2.0f));
  // The second bowl is 1 m below the level at its centre and the flood never reached it.
  CHECK(water::depth_at(bodies, ground, WorldPos{140.0, 0.0, 50.0}) == 0.0f);
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  for (const Vec3& p : mesh.positions)
    CHECK(body.center.x + static_cast<f64>(p.x) < 111.0);
  // Without its flood (a holder that read the block but never had the ground) the body answers
  // the ground below the level within its bound, the other hollow included: said on the page.
  water::Body unflooded;
  REQUIRE(water::read_body(fill_block(100.0, 50.0, 0.0), unflooded, nullptr));
  const water::Body loose[] = {unflooded};
  CHECK(water::depth_at(loose, ground, WorldPos{140.0, 0.0, 50.0}) == doctest::Approx(1.0f));
}

TEST_CASE("water fill: a ring round an island, which no star-shaped polygon could draw") {
  const scene_gen::Ground ground = ground_of(&moat);
  const water::Body body = filled(fill_block(106.0, 50.0, 0.0), ground);
  CHECK(body.region.loop_starts.size() == 2u);
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  const f64 ring = 3.14159265 * (100.0 - 9.0);
  CHECK(mesh_area(mesh) == doctest::Approx(ring).epsilon(0.015));
  const water::Body bodies[] = {body};
  CHECK(water::depth_at(bodies, ground, WorldPos{100.0, 0.0, 50.0}) == 0.0f);
  CHECK(water::depth_at(bodies, ground, WorldPos{106.0, 0.0, 50.0}) > 0.0f);
  CHECK(water::depth_at(bodies, ground, WorldPos{94.0, 0.0, 50.0}) > 0.0f);
}

TEST_CASE("water fill: refused with a sentence when it cannot fill, and the new fields read") {
  const scene_gen::Ground ground = ground_of(&paraboloid);
  water::Body body;
  std::string error;
  // A seed on the bank.
  REQUIRE(water::read_body(fill_block(115.0, 50.0, 0.0), body, &error));
  CHECK_FALSE(water::fill_body(body, ground, &error));
  CHECK(error.find("seed stands on ground at or above its level") != std::string::npos);
  // A level over the rim of a bound too small: it spills.
  REQUIRE(water::read_body(fill_block(100.0, 50.0, 0.0, 6.0f), body, &error));
  CHECK_FALSE(water::fill_body(body, ground, &error));
  CHECK(error.find("spills out of the hollow") != std::string::npos);
  CHECK(error.find("max_extent_m (6 m)") != std::string::npos);
  // Two shapes, and a bound that is not positive.
  scene::WaterBody two = fill_block(100.0, 50.0, 0.0);
  two.shape.polygon.push_back(WorldPos{0.0, 0.0, 0.0});
  CHECK_FALSE(water::read_body(two, body, &error));
  CHECK(error.find("exactly one of a circle, a polygon and a fill") != std::string::npos);
  CHECK_FALSE(water::read_body(fill_block(100.0, 50.0, 0.0, 0.0f), body, &error));
  CHECK(error.find("max_extent_m must be positive") != std::string::npos);
  // The shore's fields: defaults from the literature, and refused negative.
  scene::WaterBody block = fill_block(100.0, 50.0, 0.0);
  REQUIRE(water::read_body(block, body, &error));
  CHECK(body.wet_m == 0.3f);
  CHECK(body.foam_m == 0.6f);
  block.wet_m = -0.1f;
  CHECK_FALSE(water::read_body(block, body, &error));
  CHECK(error.find("wet_m must not be negative") != std::string::npos);
  block.wet_m = 0.2f;
  block.foam_m = -1.0f;
  CHECK_FALSE(water::read_body(block, body, &error));
  CHECK(error.find("foam_m must not be negative") != std::string::npos);
}

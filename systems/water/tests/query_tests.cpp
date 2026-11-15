// The water at a point (query.h; docs/subsystems/water.md, "The water at a point"; roadmap A43): a
// body answered from its depth map — five points of a pond agreeing with the map's own texels and
// with the ground under it — a channel answered from its description, dry where neither is, and
// the answers pinned so every toolchain gives the same bits. No device.
#include <core/hash/hash.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/water/channel.h>
#include <systems/water/query.h>
#include <systems/water/water.h>

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace engine;

namespace {

// The bowl of water_tests.cpp: round (100, 50), 2 m deep at its centre, 2 cm per square metre.
f64 bowl(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0 - 100.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0 - 50.0;
  return -2.0 + 0.02 * (x * x + z * z);
}

scene_gen::Ground bowl_ground() {
  scene_gen::Ground g;
  g.surface_fn = &bowl;
  return g;
}

water::Body pond() {
  scene::WaterBody b;
  b.name = "pond";
  b.level_m = 0.0;
  scene::WaterCircle c;
  c.center = WorldPos{100.0, 0.0, 50.0};
  c.radius_m = 12.0f;
  b.shape.circle = c;
  water::Body body;
  std::string error;
  REQUIRE_MESSAGE(water::read_body(b, body, &error), error);
  return body;
}

// A straight stream along +x from (0, 1, 0) to (40, 0, 0): 3 m wide and 0.6 m deep at its head,
// 4 m and 0.8 m at its foot, a slope of 0.025.
water::Channel stream() {
  scene::WaterChannel c;
  c.name = "stream";
  c.points = {WorldPos{0.0, 1.0, 0.0}, WorldPos{40.0, 0.0, 0.0}};
  c.width_m = {3.0f, 4.0f};
  c.depth_m = {0.6f, 0.8f};
  c.slope = {0.025f, 0.025f};
  water::Channel out;
  std::string error;
  REQUIRE_MESSAGE(water::read_channel(c, out, &error), error);
  return out;
}

u64 bits(u64 h, f64 v) {
  u64 b = 0;
  std::memcpy(&b, &v, sizeof b);
  return hash_combine(h, b);
}
u64 bits(u64 h, f32 v) {
  u32 b = 0;
  std::memcpy(&b, &v, sizeof b);
  return hash_combine(h, b);
}

}  // namespace

TEST_CASE("water: a pond's water at five points is its depth map's, and the ground's under it") {
  const scene_gen::Ground ground = bowl_ground();
  const water::Body body = pond();
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  water::DepthMap map;
  water::build_depth_map(body, mesh, ground, map);
  REQUIRE(map.size > 0u);
  water::WaterQuery query;
  query.set_ground(&ground);
  query.add_body(body, map);
  // Five points: the centre, four more on and between the map's texel centres, all under water.
  const f64 points[5][2] = {
      {100.0, 50.0}, {104.0, 50.0}, {97.0, 46.0}, {100.3, 57.1}, {108.2, 52.6}};
  const f64 unit = (static_cast<f64>(map.depth_max) + static_cast<f64>(map.bank_max)) / 65535.0;
  for (const auto& p : points) {
    water::WaterPoint w;
    query.sample(WorldPos{p[0], 0.0, p[1]}, w);
    REQUIRE(w.wet);
    CHECK(w.source == 1u);
    CHECK(w.index == 0u);
    CHECK(w.level == 0.0);
    CHECK(w.velocity.x == 0.0f);
    CHECK(w.velocity.y == 0.0f);
    // The map's own bilinear depth, from its texels as the file stores them.
    const f32 u = (static_cast<f32>(p[0] - 100.0) - map.origin.x) / map.texel_m;
    const f32 v = (static_cast<f32>(p[1] - 50.0) - map.origin.y) / map.texel_m;
    const u32 i = static_cast<u32>(u);
    const u32 j = static_cast<u32>(v);
    const f32 fu = u - static_cast<f32>(i);
    const f32 fv = v - static_cast<f32>(j);
    const f32 a = water::depth_map_depth(map, i, j), b = water::depth_map_depth(map, i + 1, j);
    const f32 c = water::depth_map_depth(map, i, j + 1),
              d = water::depth_map_depth(map, i + 1, j + 1);
    const f32 top = a + (b - a) * fu;
    const f32 want = top + ((c + (d - c) * fu) - top) * fv;
    CHECK(w.depth == want);
    // And the ground under the point, within three things the map is built with: the bowl's
    // curvature over a texel (0.02 m^-1 x 2, its second derivative, x texel^2 / 8 for each axis of
    // the bilinear), half a 16-bit step, and the ground's own millimetre: a texel's centre is asked
    // at the nearest millimetre in x and in z, which on the bowl's slope (0.04 r, at most 0.48 at
    // the 12 m outline) is up to 0.48 x 0.5 mm x sqrt 2 of height.
    const f64 truth = -ground.surface(WorldPos{p[0], 0.0, p[1]});
    const f64 texel = static_cast<f64>(map.texel_m);
    const f64 bound =
        0.04 * texel * texel / 8.0 * 2.0 + 0.5 * unit + 0.48 * 0.0005 * std::sqrt(2.0) + 1.0e-6;
    MESSAGE("(" << p[0] << ", " << p[1] << "): the map says " << w.depth << " m, the ground "
                << truth << " m");
    CHECK(std::fabs(static_cast<f64>(w.depth) - truth) <= bound);
  }
  // Outside the outline, and on the shore inside it where the ground stands above the level: dry.
  water::WaterPoint dry;
  query.sample(WorldPos{113.0, 0.0, 50.0}, dry);
  CHECK_FALSE(dry.wet);
  query.sample(WorldPos{111.0, 0.0, 50.0}, dry);
  CHECK_FALSE(dry.wet);
  CHECK(dry.depth == 0.0f);
}

TEST_CASE("water: a channel's water is its description's, flowing down its line") {
  const water::Channel c = stream();
  water::WaterQuery query;
  query.add_channel(c);
  // Mid-stream, on the line: the surface half way down, the centre's depth, Manning's speed along
  // +x times the profile's centre share (C = 1.045718 at r = 1).
  water::WaterPoint w;
  query.sample(WorldPos{20.0, 0.0, 0.0}, w);
  REQUIRE(w.wet);
  CHECK(w.source == 2u);
  CHECK(w.index == 0u);
  CHECK(w.level == doctest::Approx(0.5).epsilon(1.0e-6));
  CHECK(w.depth == doctest::Approx(0.7f).epsilon(1.0e-5));
  const f64 mean = static_cast<f64>(water::manning_speed(0.04f, 0.7f, 0.025f));
  CHECK(w.velocity.x == doctest::Approx(mean * water::k_channel_profile_mean).epsilon(1.0e-5));
  CHECK(w.velocity.y == 0.0f);
  // A metre off the line, of 3.5 m: the parabola's share 1 - (2/3.5)^2 of the depth, slower.
  water::WaterPoint side;
  query.sample(WorldPos{20.0, 0.0, 1.0}, side);
  REQUIRE(side.wet);
  const f64 r = 1.0 - (2.0 / 3.5) * (2.0 / 3.5);
  CHECK(side.depth == doctest::Approx(0.7 * r).epsilon(1.0e-4));
  CHECK(side.velocity.x < w.velocity.x);
  CHECK(side.velocity.x == doctest::Approx(mean * water::lateral_speed_share(r)).epsilon(1.0e-4));
  // Past the edge, and past the foot: dry.
  water::WaterPoint dry;
  query.sample(WorldPos{20.0, 0.0, 2.0}, dry);
  CHECK_FALSE(dry.wet);
  query.sample(WorldPos{45.0, 0.0, 0.0}, dry);
  CHECK_FALSE(dry.wet);
}

TEST_CASE("water: a module's water answers are pinned, the same from every toolchain") {
  const scene_gen::Ground ground = bowl_ground();
  const water::Body body = pond();
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  water::DepthMap map;
  water::build_depth_map(body, mesh, ground, map);
  water::WaterQuery query;
  query.set_ground(&ground);
  query.add_body(body, map);
  query.add_channel(stream());
  u64 h = 0;
  for (u32 k = 0; k < 64; ++k) {
    // A walk across the pond and down the stream.
    const f64 x = k < 32 ? 88.0 + 0.77 * static_cast<f64>(k) : 1.3 * static_cast<f64>(k - 32);
    const f64 z =
        k < 32 ? 50.0 + 0.31 * static_cast<f64>(k) : 0.05 * static_cast<f64>(k - 32) - 0.8;
    water::WaterPoint w;
    query.sample(WorldPos{x, 0.0, z}, w);
    h = hash_combine(h, w.wet ? 1u : 0u);
    h = bits(h, w.level);
    h = bits(h, w.depth);
    h = bits(h, w.velocity.x);
    h = bits(h, w.velocity.y);
    h = hash_combine(h, w.source);
  }
  char text[32];
  std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(h));
  MESSAGE("the answers' hash: " << text);
  // Taken on MSVC at x86-64-v3 (2026-11-14); GCC and Clang in the Linux containers reproduce it.
  CHECK(std::string(text) == "dbad2ab66666308d");

  // The cost, for the page: a thousand questions in one step.
  const auto t0 = std::chrono::steady_clock::now();
  u32 wet = 0;
  for (u32 k = 0; k < 1000; ++k) {
    water::WaterPoint w;
    query.sample(WorldPos{88.0 + 0.024 * static_cast<f64>(k), 0.0, 50.0}, w);
    wet += w.wet ? 1u : 0u;
  }
  const f64 us =
      std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - t0).count();
  MESSAGE("1000 water questions (a pond and a stream): " << us << " us, " << wet << " wet");
  CHECK(wet > 0u);
}

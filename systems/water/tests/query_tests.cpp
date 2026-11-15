// The water at a point (query.h; docs/subsystems/water.md, "The water at a point"; roadmap A43): a
// body answered from its depth map — five points of a pond agreeing with the map's own texels and
// with the ground under it — a channel answered from its solved stations (the flow fixture's), the
// channels' index answering what every stretch answers over 300 channels, dry where neither is,
// and the answers pinned so every toolchain gives the same bits. No device.
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

#include "channel_fixture.h"

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

// A solved channel as the stream row's tests hold one (channel_fixture.h: the three channels
// `engine-content flow fixture` writes, solved by `flow solve`, and their ground).
water::Channel fixture_channel(const scene::WaterChannel& block) {
  water::Channel out;
  std::string error;
  REQUIRE_MESSAGE(water::read_channel(block, out, &error), error);
  return out;
}

// The pond's bowl north of z = 30, the flow fixture's trenches south of it.
f64 bowl_and_trenches(const void* context, i64 x_mm, i64 z_mm) noexcept {
  return z_mm > 30000 ? bowl(context, x_mm, z_mm)
                      : channel_fixture::ground_fn(context, x_mm, z_mm);
}

scene_gen::Ground both_grounds() {
  scene_gen::Ground g;
  g.surface_fn = &bowl_and_trenches;
  return g;
}

// `count` copies of the fixture's three channels, laid out on a grid 120 m apart in x and 80 m in
// z, each named for its place.
void many_channels(u32 count, water::WaterQuery& query) {
  const scene::WaterChannel kinds[3] = {channel_fixture::uniform(), channel_fixture::step(),
                                        channel_fixture::mouth()};
  for (u32 k = 0; k < count; ++k) {
    scene::WaterChannel block = kinds[k % 3];
    const f64 dx = 120.0 * static_cast<f64>((k / 3) % 10);
    const f64 dz = 80.0 * static_cast<f64>((k / 3) / 10);
    for (WorldPos& p : block.points) {
      p.x += dx;
      p.z += dz;
    }
    block.name += "-" + std::to_string(k);
    query.add_channel(fixture_channel(block));
  }
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
  query.add_body(body, map);
  query.prepare(&ground);
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

TEST_CASE("water: a channel's water is its solve's, flowing down its line") {
  const scene_gen::Ground ground = channel_fixture::ground();
  const water::Channel c = fixture_channel(channel_fixture::uniform());
  const water::ChannelLine line = water::channel_line(c);
  water::WaterQuery query;
  query.add_channel(c);
  query.prepare(&ground);
  // On the line at its third station (24, -20): the solved level, the level less the ground
  // (0.5 m on the trench's flat bed), and the surface's speed along +x: the section's solved mean
  // times 7/6 (the vertical's surface over its mean) and the vertical's share for its depth, along
  // the stretch's fall of 0.01.
  const WorldPos on{24.0, 0.0, -20.0};
  water::WaterPoint w;
  query.sample(on, w);
  REQUIRE(w.wet);
  CHECK(w.source == 2u);
  CHECK(w.index == 0u);
  CHECK(w.level == doctest::Approx(10.34).epsilon(1.0e-6));
  const f64 under = channel_fixture::ground_m(24.0, -20.0);
  CHECK(w.depth == doctest::Approx(10.34 - under).epsilon(1.0e-4));
  const f64 hydraulic = static_cast<f64>(water::station_hydraulic_depth(c, 2));
  const f64 surface = 1.2794 * water::k_channel_surface_factor *
                      water::lateral_speed_share(static_cast<f64>(w.depth), hydraulic) /
                      std::sqrt(1.0 + 0.01 * 0.01);
  CHECK(w.velocity.x == doctest::Approx(surface).epsilon(1.0e-4));
  CHECK(std::fabs(w.velocity.y) < 1.0e-6f);
  // It is `channel_sample`'s answer, bit for bit.
  water::ChannelSample s;
  REQUIRE(water::channel_sample(c, line, ground, on, s));
  CHECK(w.level == s.level_m);
  CHECK(w.depth == s.depth_m);
  CHECK(w.velocity.x == s.velocity.x);
  CHECK(w.velocity.y == s.velocity.z);
  // 1.2 m off the line the bank has risen 0.2 m: shallower, and slower by the vertical's share.
  water::WaterPoint side;
  query.sample(WorldPos{24.0, 0.0, -18.8}, side);
  REQUIRE(side.wet);
  CHECK(side.level == w.level);
  CHECK(side.depth == doctest::Approx(0.3).epsilon(1.0e-3));
  CHECK(side.velocity.x < w.velocity.x);
  // Past the wet reach (1.5 m), and past the last station: dry.
  water::WaterPoint dry;
  query.sample(WorldPos{24.0, 0.0, -18.4}, dry);
  CHECK_FALSE(dry.wet);
  query.sample(WorldPos{55.0, 0.0, -20.0}, dry);
  CHECK_FALSE(dry.wet);
  // A described ribbon left from before the solve (a level, widths, depths, a slope) is refused
  // at load: there is nothing for a module's question to answer from.
  scene::WaterChannel laid = channel_fixture::uniform();
  laid.discharge_m3s.clear();
  laid.speed_mps.clear();
  laid.froude.clear();
  laid.wet_left_m.clear();
  laid.wet_right_m.clear();
  laid.flags.clear();
  water::Channel refused;
  CHECK_FALSE(water::read_channel(laid, refused, nullptr));
}

TEST_CASE("water: the channels' index answers what every stretch answers, over 300 channels") {
  // Prepared, a point asks its cell's spans; never prepared, every channel's every stretch. The
  // two must agree to the bit everywhere: on the lines, at the banks, past the ends, between.
  water::WaterQuery indexed;
  water::WaterQuery linear;
  many_channels(300, indexed);
  many_channels(300, linear);
  indexed.prepare(nullptr);
  u32 wet = 0;
  for (u32 k = 0; k < 20000; ++k) {
    // A walk that crosses every slot's three lines at many offsets.
    const f64 x = 4.0 + 0.0613 * static_cast<f64>(k);
    const f64 z = -23.0 + 0.0417 * static_cast<f64>((k * 7919u) % 19000u);
    water::WaterPoint a;
    water::WaterPoint b;
    indexed.sample(WorldPos{x, 0.0, z}, a);
    linear.sample(WorldPos{x, 0.0, z}, b);
    REQUIRE(a.wet == b.wet);
    CHECK(a.level == b.level);
    CHECK(a.depth == b.depth);
    CHECK(a.velocity.x == b.velocity.x);
    CHECK(a.velocity.y == b.velocity.y);
    CHECK(a.index == b.index);
    wet += a.wet ? 1u : 0u;
  }
  MESSAGE(wet << " of 20000 points wet");
  CHECK(wet > 500u);
  // The cost, for the page: a thousand questions over the 300 channels, along the slots' lines.
  for (water::WaterQuery* q : {&indexed, &linear}) {
    const auto t0 = std::chrono::steady_clock::now();
    u32 hits = 0;
    for (u32 k = 0; k < 1000; ++k) {
      water::WaterPoint w;
      const f64 x = 6.0 + 1.19 * static_cast<f64>(k);
      const f64 z = -20.0 + 0.04 * static_cast<f64>(k % 9);
      q->sample(WorldPos{x, 0.0, z}, w);
      hits += w.wet ? 1u : 0u;
    }
    const f64 us =
        std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("1000 water questions over 300 channels, "
            << std::string(q == &indexed ? "indexed" : "every stretch") << ": " << us << " us, " << hits
            << " wet");
  }
}

TEST_CASE("water: a module's water answers are pinned, the same from every toolchain") {
  const scene_gen::Ground ground = both_grounds();
  const water::Body body = pond();
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  water::DepthMap map;
  water::build_depth_map(body, mesh, ground, map);
  water::WaterQuery query;
  query.add_body(body, map);
  query.add_channel(fixture_channel(channel_fixture::uniform()));
  query.add_channel(fixture_channel(channel_fixture::step()));
  query.add_channel(fixture_channel(channel_fixture::mouth()));
  query.prepare(&ground);
  u64 h = 0;
  for (u32 k = 0; k < 64; ++k) {
    // A walk across the pond, and down the step's channel over its fall and its jump.
    const f64 x =
        k < 32 ? 88.0 + 0.77 * static_cast<f64>(k) : 8.0 + 1.6 * static_cast<f64>(k - 32);
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
  // Re-pinned at the join of A43 with S106 (2026-11-14): a channel now answers from its solved
  // stations through `channel_sample`, and the walk goes down the flow fixture's step instead of
  // a laid stream. Taken on MSVC at x86-64-v3; MSVC release and the Linux container reproduce it.
  CHECK(std::string(text) == "588dd6b257de7005");

  // The cost, for the page: the same thousand questions A43 measured, along the pond's line.
  const auto t0 = std::chrono::steady_clock::now();
  u32 wet = 0;
  for (u32 k = 0; k < 1000; ++k) {
    water::WaterPoint w;
    query.sample(WorldPos{88.0 + 0.024 * static_cast<f64>(k), 0.0, 50.0}, w);
    wet += w.wet ? 1u : 0u;
  }
  const f64 us =
      std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - t0).count();
  MESSAGE("1000 water questions (a pond and three channels): " << us << " us, " << wet << " wet");
  CHECK(wet > 0u);
}

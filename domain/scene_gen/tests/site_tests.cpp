// The site a placement reads (site.h; docs/subsystems/scene_gen.md, "The site"; roadmap G144): a
// sample on a plane, a ridge and a paraboloid against the analytic value; the heat load and the
// sunward tilt against the reference sun; the distances to a channel's bank and a body's outline;
// the same bits whatever order points are asked in and whatever order the scene lists its water;
// the fallback a context with none answers; and a pin of the bytes the Linux containers reproduce.
#include <core/hash/hash.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>

using namespace engine;
using namespace engine::scene_gen;

namespace {

// A quadratic ground z = c0 + cx x + cz z + cxx x² + czz z² (metres), whose central differences are
// exact: what the analytic tests read.
struct Quadratic {
  f64 c0 = 0.0, cx = 0.0, cz = 0.0, cxx = 0.0, czz = 0.0;
};
f64 quadratic_height(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const Quadratic& q = *static_cast<const Quadratic*>(context);
  const f64 x = static_cast<f64>(x_mm) / 1000.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  return q.c0 + q.cx * x + q.cz * z + q.cxx * x * x + q.czz * z * z;
}
Ground ground_of(const Quadratic& q) {
  Ground g;
  g.surface_fn = &quadratic_height;
  g.context = &q;
  return g;
}

// A rough ground of products and sums alone (no C library), so the pin is the ground's own bits on
// every toolchain: a ridge along z with a ripple.
f64 rough_height(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  const f64 u = x / 100.0;
  const f64 v = z / 37.0;
  return 300.0 - 120.0 * u * u + 3.0 * u * v - 0.8 * v * v * v + 0.02 * x * z / (1.0 + u * u);
}

i64 mm(f64 metres) { return nearest_mm(metres); }

constexpr f64 k_eps = 1e-9;

scene::WaterChannel straight_channel(f64 x0, f64 x1, f32 width) {
  scene::WaterChannel c;
  c.points.push_back(WorldPos{x0, 0.0, 0.0});
  c.points.push_back(WorldPos{x1, 0.0, 0.0});
  c.width_m.push_back(width);
  c.width_m.push_back(width);
  c.depth_m.push_back(0.5f);
  c.depth_m.push_back(0.5f);
  c.slope.push_back(0.01f);
  c.slope.push_back(0.01f);
  return c;
}
scene::WaterBody circle_body(f64 x, f64 z, f32 radius, bool open = false) {
  scene::WaterBody b;
  b.level_m = 0.0;
  b.open = open;
  scene::WaterCircle c;
  c.center = WorldPos{x, 0.0, z};
  c.radius_m = radius;
  b.shape.circle = c;
  return b;
}
scene::WaterBody square_body(f64 x0, f64 z0, f64 side) {
  scene::WaterBody b;
  b.level_m = 0.0;
  b.shape.polygon.push_back(WorldPos{x0, 0.0, z0});
  b.shape.polygon.push_back(WorldPos{x0 + side, 0.0, z0});
  b.shape.polygon.push_back(WorldPos{x0 + side, 0.0, z0 + side});
  b.shape.polygon.push_back(WorldPos{x0, 0.0, z0 + side});
  return b;
}

// The reference sun the page states, computed here with the C library: what the site's own
// deterministic sine and cosine must agree with.
void reference_sun(f64 latitude, f64& sx, f64& sy, f64& sz, f64& bx, f64& bz) {
  const f64 phi = std::fabs(latitude) * 3.14159265358979323846 / 180.0;
  const f64 bearing = (latitude < 0.0 ? 315.0 : 225.0) * 3.14159265358979323846 / 180.0;
  bx = std::sin(bearing);
  bz = -std::cos(bearing);
  sy = std::cos(phi);
  sx = std::sin(phi) * bx;
  sz = std::sin(phi) * bz;
}

}  // namespace

TEST_CASE("a site on a plane is the plane's slope and direction, and no curvature") {
  const Quadratic plane{12.0, 0.3, -0.2, 0.0, 0.0};
  const Ground ground = ground_of(plane);
  Site site;
  REQUIRE(site.build(46.5, {}, {}));
  for (const f64 at : {-1234.5, 0.0, 77.7}) {
    const SiteSample s = site.sample_mm(ground, mm(at), mm(-at * 0.5));
    const f64 slope = std::sqrt(0.3 * 0.3 + 0.2 * 0.2);
    CHECK(std::fabs(s.slope - slope) < k_eps);
    CHECK(std::fabs(s.down_x - (-0.3 / slope)) < k_eps);
    CHECK(std::fabs(s.down_z - (0.2 / slope)) < k_eps);
    CHECK(std::fabs(s.curvature) < k_eps);
    CHECK(std::fabs(s.elevation - quadratic_height(&plane, mm(at), mm(-at * 0.5))) == 0.0);
    // The heat load and the sunward tilt from the unit normal and the page's sun.
    f64 sx = 0, sy = 0, sz = 0, bx = 0, bz = 0;
    reference_sun(46.5, sx, sy, sz, bx, bz);
    const f64 len = std::sqrt(1.0 + slope * slope);
    const f64 nx = -0.3 / len, ny = 1.0 / len, nz = 0.2 / len;
    CHECK(std::fabs(s.heat_load - (nx * sx + ny * sy + nz * sz) / sy) < 1e-12);
    CHECK(std::fabs(s.sunward - (nx * bx + nz * bz)) < 1e-12);
  }
}

TEST_CASE("a site on a ridge and in a bowl is their analytic slope and curvature") {
  // A ridge along z: z = 100 - 0.01 x², convex across it.
  const Quadratic ridge{100.0, 0.0, 0.0, -0.01, 0.0};
  const Ground rg = ground_of(ridge);
  const Site& site = Site::fallback();
  const SiteSample flank = site.sample_mm(rg, mm(20.0), mm(-3.0));
  CHECK(std::fabs(flank.slope - 0.4) < k_eps);
  CHECK(std::fabs(flank.down_x - 1.0) < k_eps);
  CHECK(std::fabs(flank.down_z) < k_eps);
  CHECK(std::fabs(flank.curvature - 0.02) < k_eps);
  const SiteSample crest = site.sample_mm(rg, 0, mm(5.0));
  CHECK(crest.slope == 0.0);
  CHECK(crest.down_x == 0.0);
  CHECK(crest.down_z == 0.0);
  CHECK(crest.heat_load == 1.0);
  CHECK(crest.sunward == 0.0);
  // A paraboloid bowl: z = 0.005 (x² + z²), a hollow everywhere, down towards its centre.
  const Quadratic bowl{0.0, 0.0, 0.0, 0.005, 0.005};
  const Ground bg = ground_of(bowl);
  const SiteSample side = site.sample_mm(bg, mm(30.0), mm(40.0));
  CHECK(std::fabs(side.slope - 0.5) < k_eps);
  CHECK(std::fabs(side.down_x - (-0.6)) < k_eps);
  CHECK(std::fabs(side.down_z - (-0.8)) < k_eps);
  CHECK(std::fabs(side.curvature - (-0.02)) < k_eps);
}

TEST_CASE("the heat load ranks the folded aspects, by hemisphere") {
  // A 30° plane (rise over run tan 30°) facing a bearing A: downslope (sin A, -cos A).
  const f64 t30 = 0.57735026918962576451;
  const auto facing = [&](f64 bearing_deg, Quadratic& q) {
    const f64 a = bearing_deg * 3.14159265358979323846 / 180.0;
    // Downslope along (sin A, -cos A): the gradient points the other way.
    q = Quadratic{0.0, -t30 * std::sin(a), t30 * std::cos(a), 0.0, 0.0};
    return ground_of(q);
  };
  Site north;
  REQUIRE(north.build(46.5, {}, {}));
  Quadratic q;
  const SiteSample warm = north.sample_mm(facing(225.0, q), 0, 0);
  const SiteSample cool = north.sample_mm(facing(45.0, q), 0, 0);
  const SiteSample south = north.sample_mm(facing(180.0, q), 0, 0);
  // The grounding page's 1.39 and 0.34 at 46.5° N.
  CHECK(std::fabs(warm.heat_load - 1.393) < 0.002);
  CHECK(std::fabs(cool.heat_load - 0.339) < 0.002);
  CHECK(south.heat_load < warm.heat_load);
  CHECK(south.heat_load > 1.0);
  CHECK(std::fabs(warm.sunward - 0.5) < 1e-9);
  CHECK(std::fabs(cool.sunward + 0.5) < 1e-9);
  Site southern;
  REQUIRE(southern.build(-46.5, {}, {}));
  CHECK(southern.sample_mm(facing(315.0, q), 0, 0).heat_load > 1.39);
  CHECK(southern.sample_mm(facing(135.0, q), 0, 0).heat_load < 0.35);
  // The deterministic sun against the C library's.
  f64 sx = 0, sy = 0, sz = 0, bx = 0, bz = 0;
  reference_sun(46.5, sx, sy, sz, bx, bz);
  CHECK(std::fabs(north.sun_x() - sx) < 1e-15);
  CHECK(std::fabs(north.sun_y() - sy) < 1e-15);
  CHECK(std::fabs(north.sun_z() - sz) < 1e-15);
  // A latitude past the cap is held to it; one that is not a number is refused.
  Site pole;
  REQUIRE(pole.build(90.0, {}, {}));
  CHECK(pole.latitude_deg() == k_site_max_latitude_deg);
  std::string why;
  CHECK_FALSE(pole.build(std::nan(""), {}, {}, &why));
  CHECK(!why.empty());
}

TEST_CASE("the site measures the distances to a channel's bank and a body's outline") {
  const Vector<scene::WaterChannel> channels{straight_channel(-500.0, 500.0, 3.0f)};
  Vector<scene::WaterBody> bodies;
  bodies.push_back(circle_body(300.0, 300.0, 20.0f));
  bodies.push_back(square_body(-300.0, 200.0, 40.0));
  bodies.push_back(circle_body(-300.0, -300.0, 50.0f, true));  // open: its shore is not an outline
  Site site;
  REQUIRE(site.build(46.5, {bodies.data(), bodies.size()}, {channels.data(), channels.size()}));
  CHECK(site.segment_count() == 1);
  CHECK(site.body_count() == 2);
  const Ground flat;
  // The bank edge of a 3 m stream is 2.65 m from its line (scene_gen.md, "Exclusion").
  CHECK(std::fabs(site.sample_mm(flat, 0, mm(10.0), k_site_water).channel_m - 7.35) < k_eps);
  CHECK(site.sample_mm(flat, 0, mm(1.0), k_site_water).channel_m == 0.0);
  CHECK(site.sample_mm(flat, 0, mm(-200.0), k_site_water).channel_m == k_site_water_reach_m);
  // Past the channel's end the distance is to its round cap.
  CHECK(std::fabs(site.sample_mm(flat, mm(530.0), mm(40.0), k_site_water).channel_m - 47.35) <
        k_eps);
  CHECK(std::fabs(site.sample_mm(flat, mm(350.0), mm(300.0), k_site_water).body_m - 30.0) < k_eps);
  CHECK(site.sample_mm(flat, mm(-280.0), mm(220.0), k_site_water).body_m == 0.0);
  CHECK(std::fabs(site.sample_mm(flat, mm(-305.0), mm(220.0), k_site_water).body_m - 5.0) < k_eps);
  CHECK(site.sample_mm(flat, mm(-300.0), mm(-300.0), k_site_water).body_m == k_site_water_reach_m);
  const SiteSample both = site.sample_mm(flat, mm(300.0), mm(270.0), k_site_water);
  CHECK(both.water_m() == std::min(both.channel_m, both.body_m));
  // Fields not asked for keep their defaults.
  const SiteSample only_water = site.sample_mm(flat, 0, mm(10.0), k_site_water);
  CHECK(only_water.slope == 0.0);
  CHECK(only_water.heat_load == 1.0);
}

TEST_CASE("the site's distance to a channel is the exclusion's bank edge, a flood bed's included") {
  // T90: the site asks the exclusion's one definition of a bank edge (exclusion.h,
  // `channel_edge_distance_mm`) rather than keeping a copy of it. The copy left out the flood bed
  // (W93), which reaches further than the bank on its side: here 20 m on the stream's left (+z).
  Vector<scene::WaterChannel> channels{straight_channel(-500.0, 500.0, 3.0f),
                                       straight_channel(-100.0, 40.0, 2.0f)};
  channels[0].flood_left_m.push_back(20.0f);
  channels[0].flood_left_m.push_back(20.0f);
  channels[0].flood_right_m.push_back(0.0f);
  channels[0].flood_right_m.push_back(0.0f);
  for (WorldPos& p : channels[1].points)
    p.z = 90.0;
  Site site;
  REQUIRE(site.build(46.5, {}, {channels.data(), channels.size()}));
  Exclusion exclusion;
  REQUIRE(exclusion.build({}, {}, {channels.data(), channels.size()}));
  const Ground flat;
  // 30 m on the flood side is 10 m past the bed's edge; 30 m on the other, 27.35 m past the bank's.
  CHECK(site.sample_mm(flat, 0, mm(30.0), k_site_water).channel_m == doctest::Approx(10.0));
  CHECK(site.sample_mm(flat, 0, mm(-30.0), k_site_water).channel_m == doctest::Approx(27.35));
  // And to the bit everywhere, out to the reach and past it.
  u32 differ = 0;
  for (i64 j = -40; j <= 40; ++j)
    for (i64 i = -40; i <= 40; ++i) {
      const i64 x = i * 7919 - 3000, z = j * 4001 + 17;
      const f64 edge = exclusion.channel_edge_distance_mm(x, z, k_site_water_reach_m);
      const f64 want = edge < k_site_water_reach_m ? edge : k_site_water_reach_m;
      const f64 got = site.sample_mm(flat, x, z, k_site_water).channel_m;
      differ += std::memcmp(&want, &got, sizeof(f64)) == 0 ? 0u : 1u;
    }
  CHECK(differ == 0);
}

TEST_CASE("a site sample is a function of the point, whatever asks for it") {
  // Two tiles' windows that share a border, sampled in opposite orders, and a second site built
  // from the scene's water listed the other way round: the shared points are the same bits.
  Vector<scene::WaterChannel> channels{straight_channel(-100.0, 40.0, 2.0f),
                                       straight_channel(40.0, 300.0, 5.0f)};
  Vector<scene::WaterBody> bodies{circle_body(60.0, 90.0, 15.0f), square_body(-80.0, 30.0, 25.0)};
  Site a;
  REQUIRE(a.build(46.5, {bodies.data(), bodies.size()}, {channels.data(), channels.size()}));
  Vector<scene::WaterChannel> channels_r{channels[1], channels[0]};
  Vector<scene::WaterBody> bodies_r{bodies[1], bodies[0]};
  Site b;
  REQUIRE(
      b.build(46.5, {bodies_r.data(), bodies_r.size()}, {channels_r.data(), channels_r.size()}));
  Ground rough;
  rough.surface_fn = &rough_height;
  // The border x = 32 m, asked by the tile west of it (south to north, after its own points) and by
  // the tile east of it (north to south, first).
  const i64 border = 32000;
  Vector<SiteSample> west;
  for (i64 x = 0; x < border; x += 4000)
    (void)a.sample_mm(rough, x, 1234);
  for (i64 j = 0; j <= 32; ++j)
    west.push_back(a.sample_mm(rough, border, j * 1000 - 7));
  for (i64 j = 32; j >= 0; --j) {
    const SiteSample east = b.sample_mm(rough, border, j * 1000 - 7);
    CHECK(std::memcmp(&west[static_cast<u32>(j)], &east, sizeof(SiteSample)) == 0);
  }
}

TEST_CASE("a context with no site reads the fallback, and one with a site reads it") {
  const Quadratic plane{0.0, 0.1, 0.25, 0.0, 0.0};
  Context context;
  context.ground = ground_of(plane);
  const SiteSample none = context.site_mm(mm(5.0), mm(6.0));
  const SiteSample fallback = Site::fallback().sample_mm(context.ground, mm(5.0), mm(6.0));
  CHECK(std::memcmp(&none, &fallback, sizeof(SiteSample)) == 0);
  CHECK(Site::fallback().latitude_deg() == k_site_default_latitude_deg);
  Site site;
  REQUIRE(site.build(-20.0, {}, {}));
  context.site = &site;
  const SiteSample mine = context.site_mm(mm(5.0), mm(6.0));
  const SiteSample direct = site.sample_mm(context.ground, mm(5.0), mm(6.0));
  CHECK(std::memcmp(&mine, &direct, sizeof(SiteSample)) == 0);
  CHECK(mine.heat_load != none.heat_load);
}

TEST_CASE("the site's bytes are pinned, for the containers to reproduce") {
  Vector<scene::WaterChannel> channels{straight_channel(-100.0, 40.0, 2.0f),
                                       straight_channel(40.0, 300.0, 5.0f)};
  Vector<scene::WaterBody> bodies{circle_body(60.0, 90.0, 15.0f), square_body(-80.0, 30.0, 25.0)};
  Site site;
  REQUIRE(site.build(46.5, {bodies.data(), bodies.size()}, {channels.data(), channels.size()}));
  Ground rough;
  rough.surface_fn = &rough_height;
  u64 h = hash_bytes("site.pin", 8);
  for (i64 j = 0; j < 24; ++j) {
    for (i64 i = 0; i < 24; ++i) {
      const SiteSample s = site.sample_mm(rough, -200000 + i * 17321, -150000 + j * 13579);
      h = hash_bytes(&s, sizeof(s), h);
    }
  }
  // Taken on MSVC (2026-11-10); GCC and Clang at v3 and GCC at v2 must reproduce it.
  CHECK(h == 0x3db1ef5e21f98a71ull);
}

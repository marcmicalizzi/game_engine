// Where on the Earth a scene is (georeference.h; W186; scene_gen.md, "Where on the Earth a scene
// is"): the block read, written and read back, refused in each malformed way, the round trip from
// the map exact to the bit at a national grid's magnitudes, the axes in both of the engine's common
// mappings and for a system of either axis order, and a scene with no block unchanged.
#include <core/schema/json_text.h>
#include <domain/scene_gen/georeference.h>

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

using namespace engine;
using namespace engine::scene_gen;

namespace {

bool read_scene(std::string_view text, scene::Scene& out, std::string* error = nullptr) {
  schema::ReadContext ctx;
  JsonParseResult parsed;
  const bool read = schema::read_json_text(out, text, ctx, parsed);
  if (error != nullptr && (!parsed.ok || !read || !ctx.ok()))
    *error = parsed.ok ? std::string("schema") : parsed.message;
  return parsed.ok && read && ctx.ok();
}

// The Toronto proof of concept's (G-T6): NAD83(CSRS) / MTM zone 10, CGVD2013 heights.
scene::Georeference toronto() {
  scene::Georeference g;
  g.horizontal_crs = "EPSG:2952";
  g.vertical_crs = "EPSG:6647";
  g.origin_easting = 314400.0;
  g.origin_northing = 4833900.0;
  g.origin_height = 76.0;
  g.east = scene::EngineAxis::PlusX;
  g.north = scene::EngineAxis::MinusZ;
  return g;
}

// A deterministic draw in [0, 1).
struct Draw {
  u64 state = 0x9e3779b97f4a7c15ull;
  f64 next() {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<f64>(state >> 11) * (1.0 / 9007199254740992.0);
  }
};

}  // namespace

TEST_CASE("georeference: the block is read, written and read back the same") {
  const std::string_view text =
      R"({"format":"engine.scene.v1","name":"block","terrain":{"size":17,"extent":16,"seed":1},
        "georeference":{"horizontal_crs":"EPSG:2952","vertical_crs":"EPSG:6647",
          "origin_easting":314400.25,"origin_northing":4833900.5,"origin_height":76.125,
          "east":"PlusX","north":"MinusZ"}})";
  scene::Scene file;
  std::string error;
  REQUIRE_MESSAGE(read_scene(text, file, &error), error);
  REQUIRE(file.georeference.has_value());
  const scene::Georeference& g = *file.georeference;
  CHECK(g.horizontal_crs == "EPSG:2952");
  CHECK(g.vertical_crs == "EPSG:6647");
  CHECK(g.origin_easting == 314400.25);
  CHECK(g.origin_northing == 4833900.5);
  CHECK(g.origin_height == 76.125);
  CHECK(g.east == scene::EngineAxis::PlusX);
  CHECK(g.north == scene::EngineAxis::MinusZ);
  CHECK(g.unit_m == 1.0);  // the default: a metre grid
  CHECK(validate_georeference(g, nullptr));

  std::string written;
  REQUIRE(schema::write_json_text(schema::type_of<scene::Scene>(), &file, written));
  scene::Scene back;
  REQUIRE_MESSAGE(read_scene(written, back, &error), error);
  CHECK(back == file);
  std::string again;
  REQUIRE(schema::write_json_text(schema::type_of<scene::Scene>(), &back, again));
  CHECK(again == written);

  // A well-known text for a system with no code is carried as it was given, unread.
  scene::Georeference wkt = g;
  wkt.horizontal_crs = R"wkt(PROJCRS["local grid",BASEGEOGCRS["NAD83(CSRS)"]])wkt";
  CHECK(validate_georeference(wkt, nullptr));
}

TEST_CASE("georeference: each malformed block is refused with a sentence") {
  std::string why;
  auto refused = [&](const scene::Georeference& g, std::string_view says) {
    why.clear();
    const bool ok = validate_georeference(g, &why);
    CHECK_FALSE(ok);
    CHECK_MESSAGE(why.find(says) != std::string::npos, why);
  };
  scene::Georeference g = toronto();
  REQUIRE(validate_georeference(g, &why));

  g = toronto();
  g.horizontal_crs.clear();
  refused(g, "horizontal_crs");
  g = toronto();
  g.horizontal_crs = "  \t";
  refused(g, "horizontal_crs");
  g = toronto();
  g.vertical_crs.clear();
  refused(g, "vertical_crs");
  g = toronto();
  g.origin_easting = std::numeric_limits<f64>::quiet_NaN();
  refused(g, "finite");
  g = toronto();
  g.origin_northing = std::numeric_limits<f64>::infinity();
  refused(g, "finite");
  g = toronto();
  g.origin_height = -std::numeric_limits<f64>::infinity();
  refused(g, "finite");
  g = toronto();
  g.unit_m = 0.0;
  refused(g, "unit_m");
  g = toronto();
  g.unit_m = -0.3048;
  refused(g, "unit_m");
  g = toronto();
  g.unit_m = std::numeric_limits<f64>::infinity();
  refused(g, "unit_m");
  g = toronto();
  g.east = scene::EngineAxis::Unset;
  refused(g, "both name");
  g = toronto();
  g.north = scene::EngineAxis::Unset;
  refused(g, "both name");
  g = toronto();
  g.north = scene::EngineAxis::MinusX;  // east is +x
  refused(g, "same engine axis");
  g = toronto();
  g.east = scene::EngineAxis::PlusZ;  // north is -z
  refused(g, "same engine axis");

  // The schema's own refusals: an axis that is not one of the names, a field it does not have.
  scene::Scene file;
  CHECK_FALSE(read_scene(R"({"georeference":{"horizontal_crs":"EPSG:2952","east":"East"}})", file));
  CHECK_FALSE(read_scene(R"({"georeference":{"horizontal_crs":"EPSG:2952","crs":"x"}})", file));
  CHECK_FALSE(read_scene(R"({"georeference":{"origin_easting":"314400"}})", file));
}

TEST_CASE("georeference: the round trip from the map returns its bits at a national grid's size") {
  // Toronto on MTM 10, and a UTM zone's far north (5.9 million metres) with every axis reversed.
  scene::Georeference grids[2] = {toronto(), toronto()};
  grids[1].horizontal_crs = "EPSG:32617";
  grids[1].origin_easting = 499872.0;
  grids[1].origin_northing = 5912345.5;
  grids[1].east = scene::EngineAxis::MinusZ;
  grids[1].north = scene::EngineAxis::MinusX;
  for (const scene::Georeference& g : grids) {
    REQUIRE(validate_georeference(g, nullptr));
    Draw draw;
    u32 exact = 0;
    f64 worst_world = 0.0;
    for (u32 i = 0; i < 20000; ++i) {
      // Anywhere within 50 km of the origin, at full f64 precision; heights within a factor of two of
      // the origin's (38 to 114 m about 76 m), where the claim of bits holds (the next case is outside).
      const MapCoord m{g.origin_easting + (draw.next() * 2.0 - 1.0) * 50000.0,
                       g.origin_northing + (draw.next() * 2.0 - 1.0) * 50000.0,
                       g.origin_height + (draw.next() * 2.0 - 1.0) * 38.0};
      const MapCoord back = map_from_world(g, world_from_map(g, m));
      const bool same = std::bit_cast<u64>(back.easting) == std::bit_cast<u64>(m.easting) &&
                        std::bit_cast<u64>(back.northing) == std::bit_cast<u64>(m.northing) &&
                        std::bit_cast<u64>(back.height) == std::bit_cast<u64>(m.height);
      exact += same ? 1u : 0u;
      // And from the world: within half an ulp of the map coordinate where it is within a factor of two,
      // its heights (-424 to 576 m) outside the band and held to an ulp of |y| + |origin| instead.
      const WorldPos p{(draw.next() * 2.0 - 1.0) * 50000.0, (draw.next() * 2.0 - 1.0) * 500.0,
                       (draw.next() * 2.0 - 1.0) * 50000.0};
      const WorldPos q = world_from_map(g, map_from_world(g, p));
      worst_world = std::fmax(worst_world, std::fabs(q.x - p.x));
      worst_world = std::fmax(worst_world, std::fabs(q.z - p.z));
      worst_world = std::fmax(worst_world, std::fabs(q.y - p.y));
    }
    CHECK(exact == 20000u);
    // An ulp at 4-8 million metres is 2^-30 m (0.93 nm); half of it is the bound.
    CHECK(worst_world <= std::ldexp(1.0, -31));
  }
}

TEST_CASE("georeference: the axes in both common mappings, for a system of either axis order") {
  scene::Georeference g = toronto();
  // Right-handed, y up: +x east and -z north, a rotation of the map.
  const MapCoord a = map_from_world(g, WorldPos{10.0, 2.0, -20.0});
  CHECK(a.easting == 314410.0);
  CHECK(a.northing == 4833920.0);
  CHECK(a.height == 78.0);
  CHECK(world_from_map(g, a) == WorldPos{10.0, 2.0, -20.0});
  // The mirror: (easting, height, northing) written straight into (x, y, z).
  g.north = scene::EngineAxis::PlusZ;
  const MapCoord b = map_from_world(g, WorldPos{10.0, 2.0, 20.0});
  CHECK(b == a);
  CHECK(world_from_map(g, b) == WorldPos{10.0, 2.0, 20.0});
  CHECK(georeference_mirrored(g));
  // A system whose own order is northing first (DHDN / 3-degree Gauss-Kruger zone 3) names the
  // same two numbers by the same names: the block never says first or second.
  scene::Georeference gk = toronto();
  gk.horizontal_crs = "EPSG:31467";
  gk.origin_easting = 3500000.0;
  gk.origin_northing = 5400000.0;
  const MapCoord c = map_from_world(gk, WorldPos{10.0, 0.0, -20.0});
  CHECK(c.easting == 3500010.0);
  CHECK(c.northing == 5400020.0);
  // East along -z and north along -x: a quarter turn of the first, still a rotation. The point 10 m
  // east and 20 m north is at z = -10 and x = -20: -x is north, so a step along +x is south.
  gk.east = scene::EngineAxis::MinusZ;
  gk.north = scene::EngineAxis::MinusX;
  CHECK_FALSE(georeference_mirrored(gk));
  const MapCoord d = map_from_world(gk, WorldPos{-20.0, 0.0, -10.0});
  CHECK(d == c);
  CHECK(map_from_world(gk, WorldPos{1.0, 0.0, 0.0}).northing == 5399999.0);
  CHECK(world_from_map(gk, MapCoord{3500000.0, 5400001.0, 76.0}) == WorldPos{-1.0, 0.0, 0.0});
  // East along -z and north along +x is not: east x north is -y, the map's mirror image.
  gk.north = scene::EngineAxis::PlusX;
  CHECK(georeference_mirrored(gk));
  CHECK(map_from_world(gk, WorldPos{20.0, 0.0, -10.0}) == c);
  // A foot grid: a world metre is 1 / 0.3048006096012192 survey feet of easting.
  scene::Georeference ft = toronto();
  ft.unit_m = 0.3048006096012192;
  ft.origin_easting = 1000000.0;
  const MapCoord e = map_from_world(ft, WorldPos{0.3048006096012192 * 100.0, 0.0, 0.0});
  CHECK(e.easting == doctest::Approx(1000100.0).epsilon(1e-15));
  CHECK(world_from_map(ft, e).x == doctest::Approx(30.48006096012192).epsilon(1e-11));
}

TEST_CASE("georeference: a scene with no block reads none and writes it as nothing") {
  scene::Scene file;
  REQUIRE(read_scene(R"({"format":"engine.scene.v1","name":"plain"})", file));
  CHECK_FALSE(file.georeference.has_value());
  std::string written;
  REQUIRE(schema::write_json_text(schema::type_of<scene::Scene>(), &file, written));
  scene::Scene back;
  REQUIRE(read_scene(written, back));
  CHECK_FALSE(back.georeference.has_value());
  CHECK(back == file);
}

TEST_CASE("georeference: a height outside the origin's band comes back within an ulp") {
  // The origin at 76 m; ground at 0.1 m, 10 m, 200 m, 2 km, below sea level and at 0. The difference
  // and the sum each round there, so these come back within an ulp of |h| + |origin|, not to bits.
  const scene::Georeference g = toronto();
  const f64 heights[] = {0.1, 10.0, 200.0, 2000.0, -12.3, -400.7, 0.0, 1e-9, 37.9, 152.1};
  for (const f64 h : heights) {
    const MapCoord m{g.origin_easting + 12.5, g.origin_northing - 7.25, h};
    const MapCoord back = map_from_world(g, world_from_map(g, m));
    const f64 sum = std::fabs(h) + std::fabs(g.origin_height);  // the bound's magnitude
    const f64 ulp = std::nextafter(sum, std::numeric_limits<f64>::infinity()) - sum;
    CHECK_MESSAGE(std::fabs(back.height - h) <= ulp, h);
    CHECK(back.easting == m.easting);
    CHECK(back.northing == m.northing);
  }
  // With the origin's height 0 every height comes back to its bits.
  scene::Georeference sea = g;
  sea.origin_height = 0.0;
  for (const f64 h : heights) {
    const f64 back = map_from_world(sea, world_from_map(sea, MapCoord{0.0, 0.0, h})).height;
    CHECK(std::bit_cast<u64>(back) == std::bit_cast<u64>(h));
  }
}

TEST_CASE("georeference: of the eight pairs of axes four are rotations and four mirrors") {
  using A = scene::EngineAxis;
  struct Pair {
    A east, north;
    bool mirrored;
  };
  const Pair pairs[] = {
      {A::PlusX, A::MinusZ, false}, {A::MinusZ, A::MinusX, false}, {A::MinusX, A::PlusZ, false},
      {A::PlusZ, A::PlusX, false},  {A::PlusX, A::PlusZ, true},    {A::MinusZ, A::PlusX, true},
      {A::MinusX, A::MinusZ, true}, {A::PlusZ, A::MinusX, true},
  };
  u32 mirrors = 0;
  for (const Pair& p : pairs) {
    scene::Georeference g = toronto();
    g.east = p.east;
    g.north = p.north;
    REQUIRE(validate_georeference(g, nullptr));
    CHECK(georeference_mirrored(g) == p.mirrored);
    mirrors += georeference_mirrored(g) ? 1u : 0u;
    // Mirror or not, the point one metre east and one north is measured back as that point.
    const WorldPos p1 = world_from_map(g, MapCoord{g.origin_easting + 1.0, g.origin_northing + 1.0,
                                                   g.origin_height});
    const MapCoord m = map_from_world(g, p1);
    CHECK(m.easting == g.origin_easting + 1.0);
    CHECK(m.northing == g.origin_northing + 1.0);
  }
  CHECK(mirrors == 4u);
}
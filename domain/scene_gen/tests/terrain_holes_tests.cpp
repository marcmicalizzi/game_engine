// A terrain's holes (terrain_holes.h; docs/subsystems/terrain.md, "Holes"; roadmap W87): the cell
// rule, the refusals, and the hash a terrain without holes keeps.
#include <domain/scene_gen/terrain_holes.h>

#include <doctest/doctest.h>

using namespace engine;
using namespace engine::scene_gen;

namespace {

scene::TerrainHole circle(f64 x, f64 z, f32 radius) {
  scene::TerrainHole h;
  h.center = WorldPos{x, 0.0, z};
  h.radius_m = radius;
  return h;
}

}  // namespace

TEST_CASE("terrain holes: a 6 m circle is the cells wholly inside it, at the levels that fit one") {
  TerrainHoles holes;
  const scene::TerrainHole hole = circle(0.0, 0.0, 3.0f);
  REQUIRE(holes.build({&hole, 1}));
  CHECK(holes.contains_mm(0.0, 0.0));
  CHECK(holes.contains_mm(2900.0, 0.0));
  CHECK_FALSE(holes.contains_mm(3000.0, 0.0));  // on the rim: ground
  CHECK_FALSE(holes.contains_mm(3100.0, 0.0));
  // At 50 cm: a cell is a hole exactly when its four corners are inside, counted by brute force.
  u32 expected = 0;
  for (i64 j = -8; j < 8; ++j) {
    for (i64 i = -8; i < 8; ++i) {
      bool all = true;
      for (i64 dj = 0; dj < 2; ++dj) {
        for (i64 di = 0; di < 2; ++di) {
          const f64 x = static_cast<f64>((i + di) * 500);
          const f64 z = static_cast<f64>((j + dj) * 500);
          all = all && x * x + z * z < 9.0e6;
        }
      }
      expected += all ? 1u : 0u;
      CHECK(holes.cell(500, i, j) == all);
    }
  }
  CHECK(expected > 80u);  // about pi 3^2 / 0.25 = 113 less the rim's
  u32 counted = 0;
  CHECK(holes.window_hash(500, -8, -8, 16, 16, &counted) != 0);
  CHECK(counted == expected);
  // At 4 m a cell is wider than the hole's inscribed square (4.24 m): none fits, the level closes
  // it.
  u32 coarse = 0;
  CHECK(holes.window_hash(4000, -4, -4, 8, 8, &coarse) == 0);
  CHECK(coarse == 0);
  // At 2 m: the four cells round the centre fit (their far corners 2.83 m out).
  u32 middle = 0;
  holes.window_hash(2000, -4, -4, 8, 8, &middle);
  CHECK(middle == 4u);
  // A window no hole reaches hashes to 0: a tile's key does not move.
  CHECK(holes.window_hash(500, 100, 100, 16, 16) == 0);
}

TEST_CASE("terrain holes: a polygon, either winding, and the refusals") {
  scene::TerrainHole square;
  square.polygon = {WorldPos{-2.0, 0.0, -2.0}, WorldPos{2.0, 0.0, -2.0}, WorldPos{2.0, 0.0, 2.0},
                    WorldPos{-2.0, 0.0, 2.0}};
  scene::TerrainHole reversed = square;
  std::swap(reversed.polygon[1], reversed.polygon[3]);
  for (const scene::TerrainHole* h : {&square, &reversed}) {
    TerrainHoles holes;
    REQUIRE(holes.build({h, 1}));
    CHECK(holes.cell(1000, -2, -2) == false);  // its corner (-2, -2) is on the edge
    CHECK(holes.cell(1000, -1, -1));
    CHECK(holes.cell(1000, 0, 0));
    CHECK_FALSE(holes.cell(1000, 1, 1));
    CHECK(holes.contains_mm(1999.0, -1999.0));
    CHECK_FALSE(holes.contains_mm(2001.0, 0.0));
  }
  TerrainHoles a, b;
  REQUIRE(a.build({&square, 1}));
  REQUIRE(b.build({&reversed, 1}));
  CHECK(a.hash() != 0);
  CHECK(a.hash() != b.hash());  // the numbers differ, so the key does

  TerrainHoles none;
  REQUIRE(none.build({}));
  CHECK(none.empty());
  CHECK(none.hash() == 0);
  CHECK_FALSE(none.cell(1000, 0, 0));

  std::string error;
  scene::TerrainHole both = square;
  both.radius_m = 1.0f;
  CHECK_FALSE(none.build({&both, 1}, &error));
  CHECK(error.find("both") != std::string::npos);
  scene::TerrainHole neither;
  neither.name = "mouth";
  CHECK_FALSE(none.build({&neither, 1}, &error));
  CHECK(error.find("\"mouth\"") != std::string::npos);
  scene::TerrainHole two;
  two.polygon = {WorldPos{0.0, 0.0, 0.0}, WorldPos{1.0, 0.0, 0.0}};
  CHECK_FALSE(none.build({&two, 1}, &error));
  CHECK(error.find("fewer than three") != std::string::npos);
}

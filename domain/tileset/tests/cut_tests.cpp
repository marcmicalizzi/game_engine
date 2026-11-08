// Channels cut into the ground (cuts.h; docs/subsystems/tileset.md, "Channels cut into the
// ground"; roadmap W89): the section against its formula, the finest level cut and a level whose
// spacing is not finer than the width keeping its heights, the tiles no cut reaches unmoved, a set
// without cuts keyed as it was, and the provider standing on the cut ground.
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/cuts.h>
#include <domain/tileset/tileset.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <schemas/scene.h>
#include <string>

using namespace engine;
using namespace engine::tileset;

namespace {

scene::TerrainCut cut_of(std::string name, WorldPos a, WorldPos b, f32 width, f32 depth,
                         f32 slope = 1.0f) {
  scene::TerrainCut c;
  c.name = std::move(name);
  c.points = {a, b};
  c.width_m = {width, width};
  c.depth_m = {depth, depth};
  c.bank_slope = slope;
  return c;
}

// The distance from (x, z) to the segment, computed here apart from the build's own.
f64 segment_distance(f64 x, f64 z, f64 ax, f64 az, f64 bx, f64 bz) {
  const f64 ex = bx - ax, ez = bz - az;
  const f64 t = std::clamp(((x - ax) * ex + (z - az) * ez) / (ex * ex + ez * ez), 0.0, 1.0);
  const f64 px = ax + t * ex - x, pz = az + t * ez - z;
  return std::sqrt(px * px + pz * pz);
}

// A flat 40 m square at 25 cm, 16-cell tiles (4 m), its chain 1, 2, 4 and 8 m.
struct Flat {
  test::TempDir tmp{"engine_tileset_cuts"};
  Heightfield field = synth_heightfield(SynthKind::flat, 161, 250);
  BuildOptions options;
  Flat() {
    options.cells = 16;
    options.ring_spacing_mm = 1000;
    options.far_ratio = 1;
    options.far_levels = 4;
  }
  BuildResult build(const BuildOptions& o, const std::string& name) {
    BuildResult out;
    std::string error;
    REQUIRE_MESSAGE(build_tileset(field, 0x1234, "flat", o, tmp.file(name), out, error), error);
    return out;
  }
};

}  // namespace

TEST_CASE("tileset: a cut's section is the trapezoid its formula gives") {
  f64 y = 0;
  // 3 m wide, 0.6 m deep, banks at 45 degrees: a flat bed 1.8 m wide, the top at 1.5 m.
  REQUIRE(cut_section(10.0, 3.0, 0.6, 1.0, 0.0, y));
  CHECK(y == doctest::Approx(9.4).epsilon(1e-12));
  REQUIRE(cut_section(10.0, 3.0, 0.6, 1.0, 0.9, y));
  CHECK(y == doctest::Approx(9.4).epsilon(1e-12));
  REQUIRE(cut_section(10.0, 3.0, 0.6, 1.0, -1.2, y));
  CHECK(y == doctest::Approx(9.7).epsilon(1e-12));
  REQUIRE(cut_section(10.0, 3.0, 0.6, 1.0, 1.5, y));
  CHECK(y == doctest::Approx(10.0).epsilon(1e-12));
  // Past the top the face goes on up for at most a depth: to 2.1 m, then nothing.
  REQUIRE(cut_section(10.0, 3.0, 0.6, 1.0, 2.0, y));
  CHECK(y == doctest::Approx(10.5).epsilon(1e-12));
  CHECK_FALSE(cut_section(10.0, 3.0, 0.6, 1.0, 2.2, y));
  // A bed narrower than the banks allow is a V: 1 m wide and 1 m deep steepens the banks to 2.
  REQUIRE(cut_section(10.0, 1.0, 1.0, 1.0, 0.0, y));
  CHECK(y == doctest::Approx(9.0).epsilon(1e-12));
  REQUIRE(cut_section(10.0, 1.0, 1.0, 1.0, 0.25, y));
  CHECK(y == doctest::Approx(9.5).epsilon(1e-12));
}

TEST_CASE("tileset: a cut lowers the finest level to its section and leaves the rest as it was") {
  Flat s;
  const BuildResult plain = s.build(s.options, "plain");
  BuildOptions o = s.options;
  // A 3 m channel along z = 20 m from x = 5 to 35 m, 0.6 m deep, its top on the flat ground.
  const scene::TerrainCut cut = cut_of("run", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0},
                                       3.0f, 0.6f);
  std::string error;
  REQUIRE_MESSAGE(o.cuts.build({&cut, 1}, &error), error);
  const BuildResult cut_set = s.build(o, "cut");
  CHECK(cut_set.key != plain.key);
  CHECK(cut_set.cut_points > 0);

  TileSet a, b;
  REQUIRE_MESSAGE(a.open(plain.manifest, error), error);
  REQUIRE_MESSAGE(b.open(cut_set.manifest, error), error);
  // Every finest point: the section where the cut reaches, rounded once, else the ground; and a
  // finest tile no cut reaches holds the same millimetres it held without the cut.
  const i64 sp = 250;
  const f64 reach = 1.5 + 0.6;  // half the width, and the face's run past it
  u32 cut_points = 0, untouched_tiles = 0, touched_tiles = 0;
  for (i64 tz = 0; tz < 10; ++tz) {
    for (i64 tx = 0; tx < 10; ++tx) {
      bool tile_touched = false;
      for (i64 j = tz * 16; j <= tz * 16 + 16; ++j) {
        for (i64 i = tx * 16; i <= tx * 16 + 16; ++i) {
          const f64 x = static_cast<f64>(i * sp), z = static_cast<f64>(j * sp);
          const f64 ground = a.point_mm(i * sp, j * sp);
          // Millimetres, as the build computes them.
          const f64 d_mm = segment_distance(x, z, 5000.0, 20000.0, 35000.0, 20000.0);
          const f64 d = d_mm / 1000.0;
          f64 expected = ground;
          f64 section = 0;
          if (cut_section(0.0, 3000.0, 600.0, 1.0, d_mm, section))
            expected = std::min(ground, std::floor(section + 0.5));
          CHECK(b.point_mm(i * sp, j * sp) == expected);
          if (expected != ground) {
            ++cut_points;
            tile_touched = true;
          }
          if (d > reach + 0.25) CHECK(b.point_mm(i * sp, j * sp) == ground);
        }
      }
      ++(tile_touched ? touched_tiles : untouched_tiles);
    }
  }
  CHECK(cut_points > 1000);
  CHECK(touched_tiles >= 8);
  CHECK(untouched_tiles >= 60);
  // The bed under the line and the top at the bank's edge.
  CHECK(b.point_mm(20000, 20000) == -600.0);
  CHECK(b.point_mm(20000, 21500) == 0.0);

  // The chain: a level finer than the width (1 and 2 m) holds the cut; one not finer (4, 8 m)
  // keeps its heights to the byte, its hash the plain set's.
  const TileSetManifest& pm = a.manifest();
  const TileSetManifest& cm = b.manifest();
  REQUIRE(pm.levels.size() == 5);
  REQUIRE(cm.levels.size() == 5);
  CHECK(cm.levels[0].hash != pm.levels[0].hash);
  CHECK(cm.levels[1].spacing_mm == 1000);
  CHECK(cm.levels[1].hash != pm.levels[1].hash);
  CHECK(cm.levels[2].spacing_mm == 2000);
  CHECK(cm.levels[2].hash != pm.levels[2].hash);
  CHECK(cm.levels[3].spacing_mm == 4000);
  CHECK(cm.levels[3].hash == pm.levels[3].hash);
  CHECK(cm.levels[4].hash == pm.levels[4].hash);
}

TEST_CASE("tileset: a cut no wider than a level's spacing closes on it") {
  Flat s;
  const BuildResult plain = s.build(s.options, "plain");
  BuildOptions o = s.options;
  // 0.2 m wide: not wider than the finest's 25 cm, so no level holds it.
  const scene::TerrainCut rill = cut_of("rill", WorldPos{5.0, 0.0, 20.0},
                                        WorldPos{35.0, 0.0, 20.0}, 0.2f, 0.1f);
  std::string error;
  REQUIRE_MESSAGE(o.cuts.build({&rill, 1}, &error), error);
  const BuildResult closed = s.build(o, "rill");
  CHECK(closed.cut_points == 0);
  CHECK(closed.key != plain.key);
  TileSet a, b;
  REQUIRE_MESSAGE(a.open(plain.manifest, error), error);
  REQUIRE_MESSAGE(b.open(closed.manifest, error), error);
  for (u32 l = 0; l < a.manifest().levels.size(); ++l)
    CHECK(b.manifest().levels[l].hash == a.manifest().levels[l].hash);
}

TEST_CASE("tileset: cuts key a set only when there are some, and their order is not part of it") {
  Flat s;
  const BuildOptions none = s.options;
  BuildOptions empty = s.options;
  std::string error;
  REQUIRE(empty.cuts.build({}, &error));
  CHECK(empty.cuts.empty());
  CHECK(tileset_key(7, none) == tileset_key(7, empty));
  // Two crossing channels, built in either order: the same heights to the byte (the lowest of the
  // sections, whichever came first), though the key, which names the order, differs.
  const scene::TerrainCut a =
      cut_of("a", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0}, 3.0f, 0.6f);
  const scene::TerrainCut b =
      cut_of("b", WorldPos{20.0, 0.2, 5.0}, WorldPos{20.0, -0.1, 35.0}, 2.0f, 0.9f, 2.0f);
  BuildOptions ab = s.options, ba = s.options;
  const scene::TerrainCut first[2] = {a, b};
  const scene::TerrainCut second[2] = {b, a};
  REQUIRE(ab.cuts.build(first, &error));
  REQUIRE(ba.cuts.build(second, &error));
  const BuildResult x = s.build(ab, "ab");
  const BuildResult y = s.build(ba, "ba");
  TileSet tx, ty;
  REQUIRE_MESSAGE(tx.open(x.manifest, error), error);
  REQUIRE_MESSAGE(ty.open(y.manifest, error), error);
  for (u32 l = 0; l < tx.manifest().levels.size(); ++l)
    CHECK(tx.manifest().levels[l].hash == ty.manifest().levels[l].hash);
  // Building again is the same bytes.
  const BuildResult again = s.build(ab, "ab-again");
  CHECK(again.hash == x.hash);
}

TEST_CASE("tileset: a cut that is not one is refused with a sentence") {
  std::string error;
  TerrainCuts cuts;
  scene::TerrainCut one = cut_of("lone", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f);
  one.points.resize(1);
  one.width_m.resize(1);
  one.depth_m.resize(1);
  CHECK_FALSE(cuts.build({&one, 1}, &error));
  CHECK(error.find("\"lone\" has fewer than two points") != std::string::npos);
  scene::TerrainCut uneven = cut_of("uneven", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f);
  uneven.depth_m.push_back(0.3f);
  CHECK_FALSE(cuts.build({&uneven, 1}, &error));
  CHECK(error.find("length") != std::string::npos);
  scene::TerrainCut flat = cut_of("flat", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.0f);
  CHECK_FALSE(cuts.build({&flat, 1}, &error));
  CHECK(error.find("positive") != std::string::npos);
  scene::TerrainCut sheer = cut_of("sheer", WorldPos{0, 0, 0}, WorldPos{1, 0, 0}, 1.0f, 0.2f, 9.f);
  CHECK_FALSE(cuts.build({&sheer, 1}, &error));
  CHECK(error.find("bank slope") != std::string::npos);
  CHECK(cuts.empty());
}

TEST_CASE("tileset: the provider stands on the cut ground, and refuses cuts beside a built set") {
  test::TempDir tmp{"engine_tileset_cut_provider"};
  const Heightfield field = synth_heightfield(SynthKind::flat, 161, 250);
  std::string error;
  REQUIRE_MESSAGE(write_heightfield(tmp.file("flat.json"), field, error), error);
  scene::Terrain entry;
  entry.provider = "tiles";
  entry.tiles = scene::TerrainTileSet{};
  entry.tiles->heightfield = "flat.json";
  entry.tiles->ddc = "ddc";
  entry.tiles->cells = 16;
  entry.tiles->ring_spacing_mm = 1000;
  entry.tiles->far_ratio = 1;
  entry.tiles->far_levels = 2;
  entry.tiles->cuts.push_back(
      cut_of("run", WorldPos{5.0, 0.0, 20.0}, WorldPos{35.0, 0.0, 20.0}, 3.0f, 0.6f));
  scene_gen::Context context;
  const std::string dir = tmp.path();
  context.dir = dir;
  const scene_gen::GroundProviderDesc* desc =
      scene_gen::GeneratorRegistry::global().find_ground("tiles");
  REQUIRE(desc != nullptr);
  scene_gen::GroundProvider ground;
  REQUIRE_MESSAGE(desc->make(entry, context, ground, &error), error);
  CHECK(ground.view().surface_mm(20000, 20000) == static_cast<f64>(-0.6f));  // metres, as f32
  CHECK(ground.view().surface_mm(20000, 25000) == 0.0);
  // A set built ahead is not built here, so it cannot be cut.
  entry.tiles->heightfield.clear();
  entry.tiles->manifest = "set/manifest.json";
  CHECK_FALSE(desc->make(entry, context, ground, &error));
  CHECK(error.find("cuts besides") != std::string::npos);
  // A cut that is not one is refused before anything is built.
  entry.tiles->manifest.clear();
  entry.tiles->heightfield = "flat.json";
  entry.tiles->cuts[0].bank_slope = 0.f;
  CHECK_FALSE(desc->make(entry, context, ground, &error));
  CHECK(error.find("cannot cut its ground") != std::string::npos);
}

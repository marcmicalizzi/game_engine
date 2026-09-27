// The terrain consumer (world.md, "The consumers"; terrain.md; ADR-0043), compiled where the
// terrain capability is: a held tile is the generator's tile at the ring's resolution; footprints
// go to every held tile they touch; a tile's overlay goes to the store when the tile goes and comes
// back caught up — the same bytes as an overlay that never left — and a buried tile stores nothing;
// a pit's lag rebuilds the tiles round it and keeps them seamless. A building on the generator's
// ground is ruins_on_dunes_tests.cpp's, where both capabilities are.
#include <domain/terrain/terrain.h>
#include <systems/world/terrain_tiles.h>
#include <systems/world/tile_store.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::world;

namespace {

constexpr i64 k_hour = terrain::k_us_per_day / 24;

terrain::FieldDesc field_desc() {
  terrain::FieldDesc d;
  d.seed = 2026;
  d.wind.seed = 2026;
  return d;
}

sim::ObserverSet at(f32 x, f32 z) {
  sim::ObserverSet set;
  set.add(Vec3{x, 0.0f, z}, 1.0f);
  return set;
}

RingParams two_rings() {
  RingParams params;
  params.ring_count = 2;
  params.radius[0] = 1.5f;
  params.radius[1] = 3.0f;
  params.max_activations = 0;
  params.max_deactivations = 0;
  return params;
}

terrain::Stamp step(i64 time_us, i32 x_mm, i32 z_mm) {
  terrain::Stamp s;
  s.time_us = time_us;
  s.x_mm = x_mm;
  s.z_mm = z_mm;
  s.depth_mm = 40;
  s.rim_mm = 8;
  s.kind = static_cast<u8>(terrain::StampKind::footprint);
  return s;
}

u64 overlay_rows(WorldStore& store) {
  u64 rows = 0;
  store.log().visit_projections(
      [](const store::ProjectionRecord& r, void* user) {
        if (r.kind == TerrainTiles::overlay_kind()) ++*static_cast<u64*>(user);
      },
      &rows);
  return rows;
}

}  // namespace

TEST_CASE("world terrain: a held tile is the generator's tile at its ring's resolution") {
  const terrain::DuneField field(field_desc());
  TerrainTilesConfig config;
  config.field = &field;
  TerrainTiles tiles(config);
  World world(two_rings());
  world.add_consumer(tiles.consumer());
  tiles.set_time(400 * terrain::k_us_per_day);
  world.update(at(10.0f, 10.0f), 0);
  REQUIRE(tiles.stats().held > 0);
  CHECK(tiles.stats().held == world.ring().active_count());
  for (const TileCoord t : {TileCoord{0, 0}, TileCoord{2, 1}}) {
    const terrain::TileOutput* held = tiles.tile(t);
    REQUIRE(held != nullptr);
    terrain::TileOptions options;
    options.cells = world.ring().ring_of(t) == 0 ? 128 : 64;
    terrain::TileOutput direct;
    terrain::evaluate_tile(field, terrain::TileCoord{t.x, t.z}, 400 * terrain::k_us_per_day,
                           options, &tiles.lag(), nullptr, direct);
    CHECK(held->cells == options.cells);
    CHECK(held->hash() == direct.hash());
  }
}

TEST_CASE("world terrain: an overlay goes to the store with its tile and comes back caught up") {
  const test::TempDir tmp("world_terrain_store");
  const terrain::DuneField field(field_desc());
  WorldStore store;
  REQUIRE(store.open(tmp.file("world.db")) == store::Status::Ok);
  TerrainTilesConfig config;
  config.field = &field;
  config.store = &store;
  TerrainTiles tiles(config);
  World world(two_rings());
  world.add_consumer(tiles.consumer());

  i64 now = 10 * terrain::k_us_per_day;
  tiles.set_time(now);
  world.update(at(16.0f, 16.0f), 0);
  // A walk across tile (0, 0) and over its east edge into (1, 0).
  for (i32 k = 0; k < 40; ++k) {
    now += 20 * terrain::k_us_per_second;
    CHECK(tiles.deform(step(now, 4'000 + k * 800, 16'000 + (k % 2) * 250)));
  }
  CHECK_FALSE(tiles.deform(step(now, 900'000, 900'000)));  // nobody holds that tile
  CHECK(tiles.overlay(TileCoord{1, 0})->pending() > 0);    // the walk crossed the edge
  // The same walk, into an overlay that never leaves: the reference.
  terrain::Overlay stayed;
  stayed.reset(terrain::TileCoord{0, 0}, 32'000, 10 * terrain::k_us_per_day);
  stayed.set_fill_rates(field);
  i64 t = 10 * terrain::k_us_per_day;
  for (i32 k = 0; k < 40; ++k) {
    t += 20 * terrain::k_us_per_second;
    REQUIRE(
        stayed.push(step(t, 4'000 + k * 800, 16'000 + (k % 2) * 250), field.wind(), config.rules));
  }

  // Away for three hours: the tiles go, and their overlays are written.
  now += 3 * k_hour;
  tiles.set_time(now);
  world.update(at(5000.0f, 5000.0f), 1);
  CHECK_FALSE(tiles.holds(TileCoord{0, 0}));
  CHECK(tiles.stats().written >= 2);
  CHECK(overlay_rows(store) >= 2);
  CHECK(tiles.stats().largest_record <= terrain::k_overlay_record_max_bytes);

  // Back five hours later: the overlay is read and caught up, the same bytes as the one that
  // stayed.
  now += 5 * k_hour;
  tiles.set_time(now);
  world.update(at(16.0f, 16.0f), 2);
  REQUIRE(tiles.holds(TileCoord{0, 0}));
  CHECK(tiles.stats().loaded >= 2);
  stayed.advance(now, field.wind(), config.rules);
  Vector<u8> a, b;
  tiles.overlay(TileCoord{0, 0})->write(a, config.rules);
  stayed.write(b, config.rules);
  CHECK(a == b);

  // Gone for a month, which buries every footprint: the tiles store nothing any more.
  now += 30 * terrain::k_us_per_day;
  tiles.set_time(now);
  world.update(at(5000.0f, 5000.0f), 3);
  world.update(at(16.0f, 16.0f), 4);
  CHECK(tiles.overlay(TileCoord{0, 0})->empty());
  world.update(at(5000.0f, 5000.0f), 5);
  CHECK(overlay_rows(store) == 0);
  CHECK(tiles.stats().erased > 0);
}

TEST_CASE("world terrain: a pit's lag rebuilds the tiles round it, and they stay seamless") {
  const terrain::DuneField field(field_desc());
  TerrainTilesConfig config;
  config.field = &field;
  config.cells[1] = 128;  // one resolution, so shared edges can be compared vertex for vertex
  TerrainTiles tiles(config);
  World world(two_rings());
  world.add_consumer(tiles.consumer());
  i64 now = 50 * terrain::k_us_per_day;
  tiles.set_time(now);
  world.update(at(16.0f, 16.0f), 0);
  const u64 built = tiles.stats().built;
  terrain::Stamp pit;
  pit.time_us = now + k_hour;
  pit.x_mm = 16'000;
  pit.z_mm = 16'000;
  pit.radius_mm = 1'500;
  pit.radius2_mm = 1'500;
  pit.depth_mm = 1'500;
  pit.kind = static_cast<u8>(terrain::StampKind::dig);
  REQUIRE(tiles.deform(pit));
  tiles.advance(now + 2 * k_hour);
  CHECK(tiles.overlay(TileCoord{0, 0})->lag().units[0] == 1);
  CHECK(tiles.lag().get(terrain::TileCoord{0, 0}).units[0] == 1);
  CHECK(tiles.stats().built >= built + 9);  // the tile and its eight neighbours
  const terrain::TileOutput* here = tiles.tile(TileCoord{0, 0});
  const terrain::TileOutput* east = tiles.tile(TileCoord{1, 0});
  REQUIRE(here != nullptr);
  REQUIRE(east != nullptr);
  const u32 v = here->cells + 1;
  u32 bad = 0;
  for (u32 k = 0; k < v; ++k) {
    // Heights of the base plus the overlays: the east tile reads (0, 0)'s pit at its west edge too,
    // but the pit is in the tile's middle, so the shared edge is the base's, lagged, on both.
    bad += here->height_um[k * v + (v - 1)] != east->height_um[k * v];
  }
  CHECK(bad == 0);
}

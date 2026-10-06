// The ground consumer over the dunes (ground_tiles.h; world.md, "The consumers"; terrain.md;
// scene_gen.md; ADR-0043, ADR-0046), compiled where the terrain capability is and linked with it as
// a host is, since the world reaches the dunes through the scene-generator registry: a held tile is
// the generator's tile at the ring's resolution; footprints go to every held tile they touch; a
// tile's overlay goes to the store when the tile goes and comes back caught up — the same bytes as
// an overlay that never left — and a buried tile stores nothing; a pit's lag rebuilds the tiles
// round it and keeps them seamless; the record's key in the store is the one the terrain consumer
// wrote before it was generic, so a save made then reads now; and a ground with no tiles is
// refused. A building on the generator's ground is ruins_on_dunes_tests.cpp's, where both
// capabilities are.
#include <core/hash/hash.h>
#include <core/schema/materialize.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/terrain/terrain.h>
#include <systems/world/ground_tiles.h>
#include <systems/world/tile_store.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <schemas/scene.h>
#include <string>

using namespace engine;
using namespace engine::world;

namespace {

constexpr i64 k_hour = terrain::k_us_per_day / 24;

// The dunes at seed 2026, made through the registry from a scene's terrain entry, as a scene reader
// would make them.
scene_gen::GroundProvider make_ground(std::string_view name) {
  scene::Terrain entry;
  entry.seed = 2026;
  entry.provider = std::string(name);
  const scene_gen::GroundProviderDesc* desc =
      scene_gen::GeneratorRegistry::global().find_ground(name);
  REQUIRE(desc != nullptr);
  scene_gen::GroundProvider ground;
  std::string error;
  REQUIRE_MESSAGE(desc->make(entry, scene_gen::Context{}, ground, &error), error);
  return ground;
}

// The ground, its consumer, and the dunes' own half of its tiles.
struct Dunes {
  scene_gen::GroundProvider ground = make_ground("dunes");
  GroundTiles consumer;
  terrain::DuneTiles* tiles = nullptr;

  explicit Dunes(WorldStore* store = nullptr) {
    GroundTilesConfig config;
    config.store = store;
    std::string error;
    REQUIRE_MESSAGE(consumer.create(ground, config, &error), error);
    tiles = terrain::dune_tiles(consumer.tiles());
    REQUIRE(tiles != nullptr);
  }
  const terrain::DuneField& field() const { return *terrain::dune_field(ground); }
};

sim::ObserverSet at(f32 x, f32 z) {
  sim::ObserverSet set;
  set.add(WorldPos{static_cast<f64>(x), 0.0, static_cast<f64>(z)}, 1.0f);
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
        if (r.kind == GroundTiles::record_kind(terrain::k_overlay_record))
          ++*static_cast<u64*>(user);
      },
      &rows);
  return rows;
}

}  // namespace

TEST_CASE("world ground: a held tile is the generator's tile at its ring's resolution") {
  Dunes d;
  World world(two_rings());
  world.add_consumer(d.consumer.consumer());
  d.consumer.set_time(400 * terrain::k_us_per_day);
  world.update(at(10.0f, 10.0f), 0);
  REQUIRE(d.tiles->stats().held > 0);
  CHECK(d.tiles->stats().held == world.ring().active_count());
  for (const TileCoord t : {TileCoord{0, 0}, TileCoord{2, 1}}) {
    const terrain::TileOutput* held = d.tiles->tile(terrain::TileCoord{t.x, t.z});
    REQUIRE(held != nullptr);
    terrain::TileOptions options;
    options.cells = world.ring().ring_of(t) == 0 ? 128 : 64;
    terrain::TileOutput direct;
    terrain::evaluate_tile(d.field(), terrain::TileCoord{t.x, t.z}, 400 * terrain::k_us_per_day,
                           options, &d.tiles->lag(), nullptr, direct);
    CHECK(held->cells == options.cells);
    CHECK(held->hash() == direct.hash());
  }
}

TEST_CASE("world ground: an overlay goes to the store with its tile and comes back caught up") {
  const test::TempDir tmp("world_ground_store");
  WorldStore store;
  REQUIRE(store.open(tmp.file("world.db")) == store::Status::Ok);
  Dunes d(&store);
  terrain::DuneTiles& tiles = *d.tiles;
  World world(two_rings());
  world.add_consumer(d.consumer.consumer());

  i64 now = 10 * terrain::k_us_per_day;
  d.consumer.set_time(now);
  world.update(at(16.0f, 16.0f), 0);
  // A walk across tile (0, 0) and over its east edge into (1, 0).
  for (i32 k = 0; k < 40; ++k) {
    now += 20 * terrain::k_us_per_second;
    CHECK(tiles.deform(step(now, 4'000 + k * 800, 16'000 + (k % 2) * 250)));
  }
  CHECK_FALSE(tiles.deform(step(now, 900'000, 900'000)));         // nobody holds that tile
  CHECK(tiles.overlay(terrain::TileCoord{1, 0})->pending() > 0);  // the walk crossed the edge
  // The same walk, into an overlay that never leaves: the reference.
  terrain::Overlay stayed;
  stayed.reset(terrain::TileCoord{0, 0}, 32'000, 10 * terrain::k_us_per_day);
  stayed.set_fill_rates(d.field());
  i64 t = 10 * terrain::k_us_per_day;
  for (i32 k = 0; k < 40; ++k) {
    t += 20 * terrain::k_us_per_second;
    REQUIRE(stayed.push(step(t, 4'000 + k * 800, 16'000 + (k % 2) * 250), d.field().wind(),
                        tiles.rules()));
  }

  // Away for three hours: the tiles go, and their overlays are written.
  now += 3 * k_hour;
  d.consumer.set_time(now);
  world.update(at(5000.0f, 5000.0f), 1);
  CHECK_FALSE(tiles.holds(terrain::TileCoord{0, 0}));
  CHECK(tiles.stats().written >= 2);
  CHECK(overlay_rows(store) >= 2);
  CHECK(tiles.stats().largest_record <= terrain::k_overlay_record_max_bytes);

  // Back five hours later: the overlay is read and caught up, the same bytes as the one that
  // stayed.
  now += 5 * k_hour;
  d.consumer.set_time(now);
  world.update(at(16.0f, 16.0f), 2);
  REQUIRE(tiles.holds(terrain::TileCoord{0, 0}));
  CHECK(tiles.stats().loaded >= 2);
  stayed.advance(now, d.field().wind(), tiles.rules());
  Vector<u8> a, b;
  tiles.overlay(terrain::TileCoord{0, 0})->write(a, tiles.rules());
  stayed.write(b, tiles.rules());
  CHECK(a == b);

  // Gone for a month, which buries every footprint: the tiles store nothing any more.
  now += 30 * terrain::k_us_per_day;
  d.consumer.set_time(now);
  world.update(at(5000.0f, 5000.0f), 3);
  world.update(at(16.0f, 16.0f), 4);
  CHECK(tiles.overlay(terrain::TileCoord{0, 0})->empty());
  world.update(at(5000.0f, 5000.0f), 5);
  CHECK(overlay_rows(store) == 0);
  CHECK(tiles.stats().erased > 0);
}

TEST_CASE("world ground: a pit's lag rebuilds the tiles round it, and they stay seamless") {
  Dunes d;
  terrain::DuneTiles& tiles = *d.tiles;
  tiles.set_cells(1, 128);  // one resolution, so shared edges can be compared vertex for vertex
  World world(two_rings());
  world.add_consumer(d.consumer.consumer());
  i64 now = 50 * terrain::k_us_per_day;
  d.consumer.set_time(now);
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
  CHECK(tiles.overlay(terrain::TileCoord{0, 0})->lag().units[0] == 1);
  CHECK(tiles.lag().get(terrain::TileCoord{0, 0}).units[0] == 1);
  CHECK(tiles.stats().built >= built + 9);  // the tile and its eight neighbours
  const terrain::TileOutput* here = tiles.tile(terrain::TileCoord{0, 0});
  const terrain::TileOutput* east = tiles.tile(terrain::TileCoord{1, 0});
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

TEST_CASE("world ground: a record's key is the terrain consumer's, and the waves hold no tiles") {
  // The store keys a tile's record by the provider's record type and the tile, exactly as the
  // terrain consumer did before it was generic, so an overlay a save holds from then reads now.
  const TileCoord tile{-3, 7};
  CHECK(GroundTiles::record_entity(terrain::k_overlay_record, tile) ==
        Id128{hash_bytes("engine.terrain.OverlayTile", 26), store_tile(tile)});
  CHECK(GroundTiles::record_kind(terrain::k_overlay_record) ==
        schema::stable_type_id("engine.terrain.OverlayTile"));
  // The renderer's waves have no tiles a world could hold, and say so.
  const scene_gen::GroundProvider waves = make_ground("waves");
  GroundTiles consumer;
  std::string error;
  CHECK_FALSE(consumer.create(waves, GroundTilesConfig{}, &error));
  CHECK(error.find("no tiles") != std::string::npos);
  CHECK_FALSE(consumer.valid());
}

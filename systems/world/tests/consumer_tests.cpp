// The document and store consumers (systems/world/document_tiles.h, store_tiles.h;
// docs/subsystems/world.md, "The consumers"): the loop a tile makes — materialized from the
// document when it activates, written to the store and dematerialized when it goes, reconciled from
// what it wrote when it comes back — and the refusals. The world here is a fake that keeps plain
// structs (the entity store's own hook is `domain/ecs`'s to test); the records are the engine's own
// `engine.world.Node`, whose mapping is in schemas/world.schema.
#include <core/hash/hash.h>
#include <core/json/json_value.h>
#include <domain/doc/document.h>
#include <domain/sim/materialize.h>
#include <domain/sim/scheduler.h>
#include <domain/sim/timing_wheel.h>
#include <systems/world/document_tiles.h>
#include <systems/world/store_tiles.h>
#include <systems/world/world.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <map>
#include <string>

using namespace engine;
using namespace engine::world;

namespace {

const char* const k_node = "engine.world.Node";

Id128 id_of(u64 n) { return Id128::from_parts(0x7717, n); }

JsonValue vec3(f64 x, f64 y, f64 z) {
  JsonValue v = JsonValue::array();
  v.push_back(JsonValue(x));
  v.push_back(JsonValue(y));
  v.push_back(JsonValue(z));
  return v;
}

// One entity per record, keyed by the record's id as the entity store keys its entities; a record
// from the store (no source) resolves the entity that exists and creates none, as the real hook
// does.
struct FakeWorld {
  std::map<Id128, u64> entities;
  std::map<u64, Id128> by_handle;
  u64 next = 1;
  u32 promotions = 0;
  u32 resolved_from_store = 0;

  static sim::EntityHandle materialize(void* c, const sim::EntityRecord& record, u8) {
    auto* self = static_cast<FakeWorld*>(c);
    auto it = self->entities.find(record.entity);
    if (record.source == nullptr) {
      if (it == self->entities.end()) return {};
      ++self->resolved_from_store;
      return sim::EntityHandle{it->second};
    }
    if (it == self->entities.end()) {
      it = self->entities.emplace(record.entity, self->next).first;
      self->by_handle[self->next++] = record.entity;
    }
    return sim::EntityHandle{it->second};
  }
  static void promote(void* c, sim::EntityHandle, u8, u8) {
    ++static_cast<FakeWorld*>(c)->promotions;
  }
  static void dematerialize(void* c, sim::EntityHandle handle) {
    auto* self = static_cast<FakeWorld*>(c);
    const auto it = self->by_handle.find(handle.value);
    if (it == self->by_handle.end()) return;
    self->entities.erase(it->second);
    self->by_handle.erase(it);
  }
  static bool has_component(void*, const schema::TypeInfo&) { return true; }
  static sim::EntityHandle resolve(void* c, const Id128& id) {
    auto* self = static_cast<FakeWorld*>(c);
    const auto it = self->entities.find(id);
    return it != self->entities.end() ? sim::EntityHandle{it->second} : sim::EntityHandle{};
  }
  // What a system moved since the last flush, handed to the write-back as the entity store would.
  Vector<sim::WriteBackChange> pending;
  static void collect(void* c, Vector<sim::WriteBackChange>& out) {
    auto* self = static_cast<FakeWorld*>(c);
    for (sim::WriteBackChange& change : self->pending)
      out.push_back(std::move(change));
    self->pending.clear();
  }

  sim::MaterializationHooks hooks() {
    sim::MaterializationHooks row;
    row.name = "fake";
    row.context = this;
    row.materialize = &materialize;
    row.promote = &promote;
    row.dematerialize = &dematerialize;
    return row;
  }
  sim::MaterializeTarget target() {
    sim::MaterializeTarget t;
    t.context = this;
    t.name = "fake";
    t.has_component = &has_component;
    t.resolve = &resolve;
    t.collect_writeback = &collect;
    return t;
  }
};

// A summarizer that records every interval it is handed: reconciliation's step 3.
struct Summaries {
  Vector<sim::SummarizeInterval> seen;
  static void fn(void* c, const sim::SummarizeInterval& interval) {
    static_cast<Summaries*>(c)->seen.push_back(interval);
  }
};

doc::Document partitioned(f64 tile_size) {
  doc::Document d;
  d.add_layer("base", doc::LayerRole::Base);
  doc::LayerPartition partition;
  partition.tile_size = tile_size;
  d.set_layer_partition(0, partition);
  return d;
}

void node(doc::Document& d, u64 n, f64 x, f64 z) {
  JsonValue props = JsonValue::object();
  props.set("name", JsonValue("n" + std::to_string(n)));
  props.set("position", vec3(x, 1.0, z));
  Vector<doc::Diagnostic> diagnostics;
  const bool ok =
      d.apply(doc::cmd_create(id_of(n), k_node, Id128{}, std::move(props)), nullptr, &diagnostics);
  REQUIRE_MESSAGE(ok, (diagnostics.empty() ? std::string() : diagnostics[0].message));
}

sim::ObserverSet at(f32 x, f32 z) {
  sim::ObserverSet set;
  set.add(Vec3{x, 0.0f, z}, 1.0f);
  return set;
}

}  // namespace

TEST_CASE(
    "world consumers: a tile materializes, is written when it goes, and reconciles when it comes "
    "back") {
  const test::TempDir tmp("engine_world_store");
  const f32 tile_size = 32.0f;
  doc::Document d = partitioned(static_cast<f64>(tile_size));
  // Two records in the tile at the origin, one in the tile to its east, one far away.
  node(d, 1, 5.0, 5.0);
  node(d, 2, 20.0, 12.0);
  node(d, 3, 40.0, 10.0);
  node(d, 4, 3000.0, 0.0);

  sim::SimScheduler scheduler;
  FakeWorld fake;
  scheduler.add_hooks(fake.hooks());
  Summaries summaries;
  // A system that summarizes at LOD3, which reconciliation asks for over the gap (05 §5.5 step 3).
  sim::Summarizer summarizer;
  summarizer.name = "test.lod3";
  summarizer.fn = &Summaries::fn;
  summarizer.context = &summaries;
  summarizer.system = 7;
  summarizer.tiers = 0x08u;
  scheduler.wheel().add_summarizer(summarizer);
  sim::MaterializeConfig config;
  config.tier = 2;
  config.writeback_every = 0;
  sim::Materializer driver(scheduler, config);
  driver.set_target(fake.target());

  WorldStore store;
  REQUIRE(store.open(tmp.file("world.db")) == store::Status::Ok);
  store.set_world_seed(99);

  RingParams params;
  params.ring_count = 2;
  params.radius[0] = 1.5f;
  params.radius[1] = 3.0f;
  params.max_activations = 0;
  params.max_deactivations = 0;
  World world(params);
  DocumentTiles document_tiles(driver, scheduler, tile_size);
  document_tiles.bind(&d);
  StoreTilesBinding binding;
  binding.store = &store;
  binding.scheduler = &scheduler;
  binding.driver = &driver;
  binding.document = &d;
  binding.world = &world;
  StoreTiles store_tiles(binding);
  // The simulation runs in the inner ring only; registration order is activation order.
  world.add_consumer(document_tiles.consumer(0x1));
  world.add_consumer(store_tiles.consumer(0x1));

  // At the origin: tiles (-1..0, -1..0) are inner; the records of tile (0, 0) are entities, and the
  // store did not know the tile.
  world.update(at(0.0f, 0.0f), scheduler.tick().value);
  CHECK(fake.entities.count(id_of(1)) == 1);
  CHECK(fake.entities.count(id_of(2)) == 1);
  CHECK(fake.entities.count(id_of(3)) == 0);  // tile (1, 0): at 1.58 tiles, outside the inner ring
  CHECK(fake.entities.count(id_of(4)) == 0);
  CHECK(store_tiles.stats().reconciled == 4);
  CHECK(store_tiles.stats().known == 0);
  CHECK(summaries.seen.empty());

  // Time passes, then the observer walks away: the tile is written and its records go.
  for (u32 i = 0; i < 600; ++i)
    scheduler.step();
  const i64 left_at = scheduler.game_time().us;
  world.update(at(1000.0f, 1000.0f), scheduler.tick().value);
  CHECK(fake.entities.empty());
  CHECK(driver.live() == 0);
  Vector<TileRow> rows;
  bool has_snapshot = false;
  store::SnapshotInfo info;
  REQUIRE(store.read_tile(TileCoord{0, 0}, rows, has_snapshot, info) == store::Status::Ok);
  CHECK(has_snapshot);
  CHECK(info.game_time_us == left_at);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0].record == id_of(1));
  CHECK(rows[0].position.x == 5.0f);
  CHECK(rows[0].position.y == 1.0f);

  // More time, and back: reconciliation knows the tile, summarizes the gap from the tile's seed,
  // reads its two projections back, resolves the entities the document consumer made (registered
  // first, so they exist) and promotes them by distance.
  for (u32 i = 0; i < 1200; ++i)
    scheduler.step();
  const i64 back_at = scheduler.game_time().us;
  world.update(at(0.0f, 0.0f), scheduler.tick().value);
  CHECK(fake.entities.count(id_of(1)) == 1);
  // All four inner tiles were written when they went, the three with no records as well: a tile
  // with nothing in it still has a last-active time for the next reconciliation's gap.
  CHECK(store_tiles.stats().known == 4);
  bool found = false;
  for (const sim::SummarizeInterval& s : summaries.seen) {
    if (s.tile != store_tile(TileCoord{0, 0})) continue;
    found = true;
    CHECK(s.from.us == left_at);
    CHECK(s.to.us == back_at);
    CHECK(s.seed == store.tile_seed(store_tile(TileCoord{0, 0})));
  }
  CHECK(found);
  CHECK(fake.resolved_from_store == 2);
  CHECK(fake.promotions >= 2);  // from LOD2 toward the observer at the tile's corner

  // The store outlives the process: reopened, the tile is still there.
  store.close();
  WorldStore again;
  REQUIRE(again.open(tmp.file("world.db"), false) == store::Status::Ok);
  REQUIRE(again.read_tile(TileCoord{0, 0}, rows, has_snapshot, info) == store::Status::Ok);
  CHECK(has_snapshot);
  CHECK(rows.size() == 2);
}

namespace {

// A write-back committed as a session would: one transaction.
struct Sink {
  doc::Document* document = nullptr;
  static bool commit(void* c, const sim::WriteBackBatch& batch) {
    auto* self = static_cast<Sink*>(c);
    doc::Transaction transaction = self->document->begin(batch.attribution);
    for (const doc::Command& command : batch.commands) {
      if (!transaction.apply(command)) return false;
    }
    return transaction.commit();
  }
};

}  // namespace

// Records that move (world.md): a record a system moved into another tile follows it when the tile
// is live in the consumer's rings, and goes when it is not — so what is materialized is the records
// of the live tiles, and a deactivation writes and drops the records that are in its tile now.
TEST_CASE("world consumers: a record the world moved follows its tile, or goes with it not live") {
  const test::TempDir tmp("engine_world_moves");
  doc::Document d = partitioned(32.0);
  node(d, 1, 5.0, 5.0);    // tile (0, 0): will move to (-1, 0), which is live
  node(d, 2, 10.0, 5.0);   // tile (0, 0): will move 200 m east, where nothing is live
  node(d, 3, 20.0, 20.0);  // tile (0, 0): stays

  sim::SimScheduler scheduler;
  FakeWorld fake;
  scheduler.add_hooks(fake.hooks());
  sim::MaterializeConfig config;
  config.writeback_every = 0;
  sim::Materializer driver(scheduler, config);
  driver.set_target(fake.target());
  Sink sink{&d};
  driver.set_writeback_sink(sim::WriteBackSink{&sink, &Sink::commit});
  WorldStore store;
  REQUIRE(store.open(tmp.file("world.db")) == store::Status::Ok);

  RingParams params;
  params.ring_count = 2;
  params.radius[0] = 1.5f;
  params.radius[1] = 3.0f;
  params.max_activations = 0;
  params.max_deactivations = 0;
  World world(params);
  DocumentTiles document_tiles(driver, scheduler, 32.0f);
  document_tiles.bind(&d);
  StoreTilesBinding binding;
  binding.store = &store;
  binding.scheduler = &scheduler;
  binding.driver = &driver;
  binding.document = &d;
  binding.world = &world;
  binding.document_tiles = &document_tiles;
  StoreTiles store_tiles(binding);
  world.add_consumer(document_tiles.consumer(0x1, &world.ring()));
  world.add_consumer(store_tiles.consumer(0x1));
  world.update(at(0.0f, 0.0f), 0);
  REQUIRE(driver.live() == 3);

  const schema::MaterializeInfo* mapping = schema::MaterializeRegistry::global().find(k_node);
  REQUIRE(mapping != nullptr);
  u32 row = 0;
  while (std::string_view(mapping->fields[row].property) != "position")
    ++row;
  auto move = [&](u64 n, f64 x, f64 z) {
    sim::WriteBackChange change;
    change.record = id_of(n);
    change.mapping = mapping;
    change.row = row;
    change.value = vec3(x, 1.0, z);
    fake.pending.push_back(change);
  };
  move(1, -10.0, 5.0);
  move(2, 210.0, 5.0);
  REQUIRE(driver.flush_writeback(SimTick{1}, GameTime{16667}) == 2);
  // Settled between ticks: one follows into the live tile (-1, 0), the other goes.
  CHECK(document_tiles.settle(world.ring()) == 1);
  CHECK(document_tiles.stats().refiled == 1);
  CHECK(document_tiles.stats().left == 1);
  CHECK(driver.holds(id_of(1)));
  CHECK_FALSE(driver.holds(id_of(2)));
  CHECK(fake.entities.count(id_of(2)) == 0);
  Vector<Id128> held;
  driver.held(sim::MaterializeScope::of_tile({-1, 0}), held);
  REQUIRE(held.size() == 1);
  CHECK(held[0] == id_of(1));

  // A move the deactivation's own flush finds is settled before the tile is written and let go:
  // record 3 walks into (-1, 0) as (0, 0) goes, and is written under neither (0, 0) nor dropped.
  move(3, -20.0, 20.0);
  world.update(at(-40.0f, 0.0f), 1);  // (0, 0) leaves the inner ring; (-1, 0) and (-2, 0) stay
  CHECK(world.ring().ring_of(TileCoord{0, 0}) != 0);
  CHECK(driver.holds(id_of(3)));
  CHECK(driver.holds(id_of(1)));
  Vector<TileRow> rows;
  bool has_snapshot = false;
  store::SnapshotInfo info;
  REQUIRE(store.read_tile(TileCoord{0, 0}, rows, has_snapshot, info) == store::Status::Ok);
  CHECK(has_snapshot);
  CHECK(rows.empty());
}

TEST_CASE("world consumers: a document on another grid is refused, once, with a sentence") {
  doc::Document d = partitioned(64.0);
  node(d, 1, 5.0, 5.0);
  sim::SimScheduler scheduler;
  FakeWorld fake;
  scheduler.add_hooks(fake.hooks());
  sim::Materializer driver(scheduler);
  driver.set_target(fake.target());
  DocumentTiles tiles(driver, scheduler, 32.0f);
  tiles.bind(&d);
  const char* why = nullptr;
  CHECK_FALSE(tiles.grid_matches(&why));
  CHECK(why != nullptr);
  RingParams params;
  params.max_activations = 0;
  World world(params);
  world.add_consumer(tiles.consumer(0x1));
  const UpdateStats& s = world.update(at(0.0f, 0.0f), 0);
  CHECK(s.refused == s.per_ring[0]);
  CHECK(fake.entities.empty());
}

TEST_CASE("world consumers: where a record is, from its partition's property") {
  doc::Document d = partitioned(32.0);
  node(d, 1, -12.5, 44.0);
  Vec3 p;
  REQUIRE(record_position(d, id_of(1), p));
  CHECK(p.x == -12.5f);
  CHECK(p.y == 1.0f);
  CHECK(p.z == 44.0f);
  CHECK_FALSE(record_position(d, id_of(9), p));
}

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

#include <algorithm>
#include <map>
#include <span>
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
  set.add(WorldPos{static_cast<f64>(x), 0.0, static_cast<f64>(z)}, 1.0f);
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

TEST_CASE(
    "world consumers: a document names its world seed in a settings record, and the store takes "
    "it when it is bound") {
  // W11 (world.md, "The world seed"): the seed every tile's seed is hashed from was 1 for every
  // headless run, because a document could not name one. A settings record at the fixed id names
  // it; the store consumer gives it to the store before any tile is reconciled.
  char hex[33] = {};
  k_world_settings.to_hex(hex);
  CHECK(std::string(hex) == "776f726c642e73657474696e67730001");  // "world.settings", 1

  doc::Document d = partitioned(32.0);
  node(d, 1, 5.0, 5.0);
  u64 seed = 77;
  CHECK_FALSE(document_world_seed(d, seed));  // none: the store keeps its own
  CHECK(seed == 77);

  // A record of another type at the id names nothing.
  {
    doc::Document other = partitioned(32.0);
    JsonValue props = JsonValue::object();
    props.set("name", JsonValue("not the settings"));
    props.set("position", vec3(1.0, 0.0, 1.0));
    REQUIRE(other.apply(doc::cmd_create(k_world_settings, k_node, Id128{}, std::move(props)),
                        nullptr, nullptr));
    CHECK_FALSE(document_world_seed(other, seed));
  }

  // A seed past 2^53 and past 2^63, which a double could not hold: the document keeps it whole.
  const u64 big = 0xFEDCBA9876543210ull;
  JsonValue props = JsonValue::object();
  props.set("seed", JsonValue(big));
  Vector<doc::Diagnostic> diagnostics;
  const bool created =
      d.apply(doc::cmd_create(k_world_settings, k_world_settings_type, Id128{}, std::move(props)),
              nullptr, &diagnostics);
  REQUIRE_MESSAGE(created, (diagnostics.empty() ? std::string() : diagnostics[0].message));
  REQUIRE(document_world_seed(d, seed));
  CHECK(seed == big);

  // Bound to the document, the store takes it, and every tile's seed follows from it.
  WorldStore store;
  REQUIRE(store.open_memory() == store::Status::Ok);
  CHECK(store.world_seed() == 1);
  StoreTilesBinding binding;
  binding.store = &store;
  binding.document = &d;
  {
    const StoreTiles tiles(binding);
    CHECK(store.world_seed() == big);
    CHECK(store.tile_seed(store_tile(TileCoord{3, -4})) ==
          hash_combine(big, store_tile(TileCoord{3, -4})));
  }

  // The record without a seed is the schema's 1; deleted, it names none, and a store bound then
  // keeps the seed its host gave it.
  REQUIRE(d.apply(doc::cmd_clear(k_world_settings, "seed"), nullptr, nullptr));
  REQUIRE(document_world_seed(d, seed));
  CHECK(seed == 1);
  REQUIRE(d.apply(doc::cmd_delete(k_world_settings), nullptr, nullptr));
  CHECK_FALSE(document_world_seed(d, seed));
  store.set_world_seed(99);
  {
    const StoreTiles tiles(binding);
    CHECK(store.world_seed() == 99);
  }
  CHECK(d.validate_index());
}

TEST_CASE("world consumers: where a record is, from its partition's property") {
  doc::Document d = partitioned(32.0);
  node(d, 1, -12.5, 44.0);
  WorldPos p;
  REQUIRE(record_position(d, id_of(1), p));
  CHECK(p.x == -12.5);
  CHECK(p.y == 1.0);
  CHECK(p.z == 44.0);
  CHECK_FALSE(record_position(d, id_of(9), p));
}

namespace {

sim::WriteBackBatch batch_of(std::initializer_list<u64> records) {
  sim::WriteBackBatch batch;
  for (const u64 n : records) {
    sim::WriteBackChange change;
    change.record = id_of(n);
    batch.changes.push_back(std::move(change));
  }
  return batch;
}

Vector<TileRow> tile_rows(WorldStore& store, i32 x, i32 z) {
  Vector<TileRow> rows;
  bool has_snapshot = true;
  store::SnapshotInfo info;
  REQUIRE(store.read_tile(TileCoord{x, z}, rows, has_snapshot, info) == store::Status::Ok);
  CHECK_FALSE(has_snapshot);  // a flush writes projections, never a tile's snapshot
  return rows;
}

}  // namespace

// Roadmap S11: the projections of what a write-back flush wrote, kept current in the store as the
// document has them, under the tile it puts them in, a moved record's row following it.
TEST_CASE("world store: a flush's records' projections are kept current") {
  WorldStore store;
  REQUIRE(store.open_memory() == store::Status::Ok);
  doc::Document d = partitioned(64.0);
  node(d, 1, 10.0, 10.0);  // tile (0, 0)
  node(d, 2, 70.0, 10.0);  // tile (1, 0)
  node(d, 3, 80.0, 20.0);  // tile (1, 0), not written back

  ProjectWritebackStats stats;
  REQUIRE(project_writeback(store, d, batch_of({2, 1}), &stats) == store::Status::Ok);
  CHECK(stats.written == 2);
  Vector<TileRow> west = tile_rows(store, 0, 0);
  REQUIRE(west.size() == 1);
  CHECK(west[0].record == id_of(1));
  CHECK(west[0].type == k_node);
  CHECK(west[0].position.x == 10.0);
  REQUIRE(tile_rows(store, 1, 0).size() == 1);  // only what the flush wrote

  // The world moved 1 east: the document says so, and the flush's projection follows it.
  JsonValue moved = vec3(75.5, 1.0, 12.0);
  REQUIRE(d.apply(doc::cmd_set(id_of(1), "position", std::move(moved)), nullptr, nullptr));
  REQUIRE(project_writeback(store, d, batch_of({1}), &stats) == store::Status::Ok);
  CHECK(tile_rows(store, 0, 0).empty());
  const Vector<TileRow> east = tile_rows(store, 1, 0);
  REQUIRE(east.size() == 2);
  CHECK(east[0].record == id_of(1));
  CHECK(east[0].position.x == 75.5);
  CHECK(east[0].position.z == 12.0);

  // A record the document no longer has is not projected, and its last row goes in the same
  // transaction (D45): an upsert never removes a row, so nothing else would.
  REQUIRE(d.apply(doc::cmd_delete(id_of(2)), nullptr, nullptr));
  REQUIRE(project_writeback(store, d, batch_of({2}), &stats) == store::Status::Ok);
  CHECK(stats.gone == 1);
  CHECK(stats.erased == 1);
  CHECK(stats.written == 0);
  const Vector<TileRow> left = tile_rows(store, 1, 0);
  REQUIRE(left.size() == 1);
  CHECK(left[0].record == id_of(1));
}

// D45: a flush whose projections cannot be written leaves the store as it was and is not lost — the
// next call projects its records again, from the document as it is by then.
TEST_CASE("world store: a flush's projections that failed to write are written by the next") {
  WorldStore store;
  REQUIRE(store.open_memory() == store::Status::Ok);
  doc::Document d = partitioned(64.0);
  node(d, 1, 10.0, 10.0);  // tile (0, 0)
  node(d, 2, 20.0, 10.0);  // tile (0, 0)
  ProjectWritebackStats stats;
  REQUIRE(project_writeback(store, d, batch_of({1, 2}), &stats) == store::Status::Ok);
  REQUIRE(tile_rows(store, 0, 0).size() == 2);

  // Record 1 moves east and record 2 is deleted, and the write fails: a transaction already open on
  // the connection refuses the flush's own, as a busy or full file would refuse it.
  JsonValue moved = vec3(70.0, 1.0, 10.0);
  REQUIRE(d.apply(doc::cmd_set(id_of(1), "position", std::move(moved)), nullptr, nullptr));
  REQUIRE(d.apply(doc::cmd_delete(id_of(2)), nullptr, nullptr));
  {
    store::Transaction busy;
    REQUIRE(store.database().begin(busy, true) == store::Status::Ok);
    CHECK(project_writeback(store, d, batch_of({1, 2}), &stats) != store::Status::Ok);
    CHECK(stats.written == 0);
    CHECK(stats.erased == 0);
    busy.rollback();
  }
  CHECK(tile_rows(store, 0, 0).size() == 2);  // as it was
  CHECK(tile_rows(store, 1, 0).empty());
  CHECK(store.unprojected().size() == 2);

  // The next flush wrote nothing of either, and still puts both right.
  REQUIRE(project_writeback(store, d, batch_of({}), &stats) == store::Status::Ok);
  CHECK(stats.retried == 2);
  CHECK(stats.written == 1);
  CHECK(stats.erased == 1);
  CHECK(store.unprojected().empty());
  CHECK(tile_rows(store, 0, 0).empty());
  const Vector<TileRow> east = tile_rows(store, 1, 0);
  REQUIRE(east.size() == 1);
  CHECK(east[0].record == id_of(1));
  CHECK(east[0].position.x == 70.0);
}

namespace {

// engine-host's write-back sink as far as the store goes: each command applied alone (a record
// deleted since it was materialized refuses its own command and nothing else), then the flush's
// records' projections kept current.
struct ProjectingSink {
  doc::Document* document = nullptr;
  WorldStore* store = nullptr;
  ProjectWritebackStats last;
  static bool commit(void* c, const sim::WriteBackBatch& batch) {
    auto* self = static_cast<ProjectingSink*>(c);
    for (const doc::Command& command : batch.commands)
      (void)self->document->apply(command, nullptr, nullptr);
    return project_writeback(*self->store, *self->document, batch, &self->last) ==
           store::Status::Ok;
  }
};

// A streamed world over a store file: the document consumer, then the store consumer, on 32 m
// tiles, the simulation in the inner ring, writing back through `ProjectingSink`.
struct StreamedRig {
  static sim::MaterializeConfig config() {
    sim::MaterializeConfig c;
    c.writeback_every = 0;
    return c;
  }
  static RingParams ring() {
    RingParams p;
    p.ring_count = 2;
    p.radius[0] = 1.5f;
    p.radius[1] = 3.0f;
    p.max_activations = 0;
    p.max_deactivations = 0;
    return p;
  }
  StoreTilesBinding binding(doc::Document& d) {
    StoreTilesBinding b;
    b.store = &store;
    b.scheduler = &scheduler;
    b.driver = &driver;
    b.document = &d;
    b.world = &world;
    b.document_tiles = &document_tiles;
    return b;
  }

  StreamedRig(doc::Document& d, const std::string& path)
      : driver(scheduler, config()),
        sink{&d, &store, {}},
        world(ring()),
        document_tiles(driver, scheduler, 32.0f),
        store_tiles(binding(d)) {
    scheduler.add_hooks(fake.hooks());
    driver.set_target(fake.target());
    driver.set_writeback_sink(sim::WriteBackSink{&sink, &ProjectingSink::commit});
    REQUIRE(store.open(path) == store::Status::Ok);
    document_tiles.bind(&d);
    world.add_consumer(document_tiles.consumer(0x1, &world.ring()));
    world.add_consumer(store_tiles.consumer(0x1));
  }

  // The world moved record `n` to (x, z), for the next flush to write back.
  void move(u64 n, f64 x, f64 z) {
    const schema::MaterializeInfo* mapping = schema::MaterializeRegistry::global().find(k_node);
    REQUIRE(mapping != nullptr);
    u32 row = 0;
    while (std::string_view(mapping->fields[row].property) != "position")
      ++row;
    sim::WriteBackChange change;
    change.record = id_of(n);
    change.mapping = mapping;
    change.row = row;
    change.value = vec3(x, 1.0, z);
    fake.pending.push_back(change);
  }

  Vector<Id128> stored(i32 x, i32 z) {
    Vector<TileRow> rows;
    bool has_snapshot = false;
    store::SnapshotInfo info;
    REQUIRE(store.read_tile(TileCoord{x, z}, rows, has_snapshot, info) == store::Status::Ok);
    Vector<Id128> ids;
    for (const TileRow& r : rows)
      ids.push_back(r.record);
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  sim::SimScheduler scheduler;
  FakeWorld fake;
  sim::Materializer driver;
  WorldStore store;
  ProjectingSink sink;
  World world;
  DocumentTiles document_tiles;
  StoreTiles store_tiles;
};

}  // namespace

// D45: a record the document no longer has keeps no projection — erased by the flush that finds it
// gone, and by its tile's going when no flush did — so a tile that comes back reads only what the
// document still has.
TEST_CASE(
    "world store: a deleted record's projection goes with the flush that finds it gone and with "
    "its tile") {
  const test::TempDir tmp("engine_world_deleted");
  doc::Document d = partitioned(32.0);
  node(d, 1, 5.0, 5.0);    // tile (0, 0): stays
  node(d, 2, 10.0, 5.0);   // tile (0, 0): written back, deleted, and a flush finds it gone
  node(d, 3, 20.0, 20.0);  // tile (0, 0): deleted while live, which no flush sees
  StreamedRig rig(d, tmp.file("world.db"));
  rig.world.update(at(0.0f, 0.0f), 0);
  REQUIRE(rig.driver.live() == 3);

  // The tile goes and comes back: its three records projected, read back and resolved.
  rig.world.update(at(1000.0f, 1000.0f), 1);
  CHECK(rig.stored(0, 0).size() == 3);
  u64 read = rig.store_tiles.stats().records;
  rig.world.update(at(0.0f, 0.0f), 2);
  CHECK(rig.store_tiles.stats().records - read == 3);

  // Written and flushed.
  rig.move(2, 12.0, 6.0);
  REQUIRE(rig.driver.flush_writeback(SimTick{3}, GameTime{50000}) == 1);
  CHECK(rig.sink.last.written == 1);
  // Deleted, and the next flush still carries a change of it: the flush finds it gone, and its
  // projection goes before the tile does.
  REQUIRE(d.apply(doc::cmd_delete(id_of(2)), nullptr, nullptr));
  rig.move(2, 14.0, 6.0);
  (void)rig.driver.flush_writeback(SimTick{4}, GameTime{66667});
  CHECK(rig.sink.last.gone == 1);
  CHECK(rig.sink.last.erased == 1);
  Vector<Id128> rows = rig.stored(0, 0);
  REQUIRE(rows.size() == 2);
  CHECK(rows[0] == id_of(1));
  CHECK(rows[1] == id_of(3));

  // Record 3 is deleted with no flush to see it; the tile's going erases its projection.
  REQUIRE(d.apply(doc::cmd_delete(id_of(3)), nullptr, nullptr));
  rig.world.update(at(1000.0f, 1000.0f), 5);
  CHECK(rig.store_tiles.stats().erased == 1);
  CHECK(rig.store_tiles.stats().failures == 0);
  rows = rig.stored(0, 0);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0] == id_of(1));

  // Back again: the tile reads the one record the document still has.
  rig.fake.resolved_from_store = 0;
  read = rig.store_tiles.stats().records;
  rig.world.update(at(0.0f, 0.0f), 6);
  CHECK(rig.store_tiles.stats().records - read == 1);
  CHECK(rig.store_tiles.stats().skipped == 0);
  CHECK(rig.fake.resolved_from_store == 1);
}

// D45: a store written before the mend may hold the projection of a record its document has since
// deleted. A load leaves it out and writes nothing; the tile's next going erases it.
TEST_CASE(
    "world store: a stale projection from before is skipped by a load and erased by a going") {
  const test::TempDir tmp("engine_world_stale");
  doc::Document d = partitioned(32.0);
  node(d, 1, 5.0, 5.0);  // tile (0, 0)
  {
    // The store as an earlier build left it: record 9's projection outlived its record.
    WorldStore before;
    REQUIRE(before.open(tmp.file("world.db")) == store::Status::Ok);
    TileRow rows[2];
    rows[0].record = id_of(1);
    rows[0].type = k_node;
    rows[0].position = WorldPos{5.0, 1.0, 5.0};
    rows[1].record = id_of(9);
    rows[1].type = k_node;
    rows[1].position = WorldPos{9.0, 1.0, 9.0};
    REQUIRE(before.write_tile(TileCoord{0, 0}, std::span<const TileRow>(rows, 2), 1, 16667) ==
            store::Status::Ok);
  }
  StreamedRig rig(d, tmp.file("world.db"));
  rig.world.update(at(0.0f, 0.0f), 0);
  CHECK(rig.store_tiles.stats().known == 1);
  CHECK(rig.store_tiles.stats().records == 1);
  CHECK(rig.store_tiles.stats().skipped == 1);
  CHECK(rig.fake.resolved_from_store == 1);
  CHECK(rig.stored(0, 0).size() == 2);  // a load reads; it writes nothing

  rig.world.update(at(1000.0f, 1000.0f), 1);
  CHECK(rig.store_tiles.stats().erased == 1);
  const Vector<Id128> rows = rig.stored(0, 0);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0] == id_of(1));
}

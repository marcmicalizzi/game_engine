// The store-backed `sim::TileStore` (systems/world/tile_store.h; docs/subsystems/world.md, "The
// store"): a tile's projections and snapshot written and read back through `foundation/store`, and
// `SimScheduler::reconcile_tile` reading them — the gap from the snapshot, the summary from the
// tile's seed, the records from the projections — where until now only tests' fakes stood.
#include <core/hash/hash.h>
#include <core/schema/materialize.h>
#include <domain/sim/scheduler.h>
#include <domain/sim/timing_wheel.h>
#include <systems/world/tile_store.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <map>
#include <string>

using namespace engine;
using namespace engine::world;

namespace {

const char* const k_node = "engine.world.Node";

Id128 id_of(u64 n) { return Id128::from_parts(0x7717, n); }

// The entities a document pass would have made: reconciliation's records come from the store with
// no source, so the hook resolves one that exists and creates none, as the entity store's does.
struct Resolver {
  std::map<Id128, u64> entities;
  u32 resolved = 0;
  u32 promotions = 0;
  static sim::EntityHandle materialize(void* c, const sim::EntityRecord& record, u8) {
    auto* self = static_cast<Resolver*>(c);
    const auto it = self->entities.find(record.entity);
    if (record.source != nullptr || it == self->entities.end()) return {};
    ++self->resolved;
    return sim::EntityHandle{it->second};
  }
  static void promote(void* c, sim::EntityHandle, u8, u8) {
    ++static_cast<Resolver*>(c)->promotions;
  }
};

struct Summaries {
  Vector<sim::SummarizeInterval> seen;
  static void fn(void* c, const sim::SummarizeInterval& interval) {
    static_cast<Summaries*>(c)->seen.push_back(interval);
  }
};

}  // namespace

TEST_CASE("world store: a tile's projections and snapshot round-trip through the store") {
  WorldStore store;
  REQUIRE(store.open_memory() == store::Status::Ok);
  store.set_world_seed(2026);
  const TileCoord tile{-3, 7};
  TileRow rows[2];
  rows[0].record = id_of(2);
  rows[0].type = k_node;
  rows[0].position = WorldPos{-90.0, 1.0, 230.0};
  rows[1].record = id_of(1);
  rows[1].type = k_node;
  rows[1].position = WorldPos{-70.0, 2.0, 240.0};
  rows[1].importance = 4.0f;

  sim::TileStore seam = store.tile_store();
  sim::TileState state;
  CHECK_FALSE(seam.tile_state(seam.context, store_tile(tile), state));  // never written: unknown
  CHECK(state.seed == hash_combine(u64{2026}, store_tile(tile)));

  REQUIRE(store.write_tile(tile, std::span<const TileRow>(rows, 2), 120, 2'000'000) ==
          store::Status::Ok);
  REQUIRE(seam.tile_state(seam.context, store_tile(tile), state));
  CHECK(state.last_active.us == 2'000'000);

  Vector<sim::EntityRecord> records;
  seam.load_records(seam.context, store_tile(tile), records);
  REQUIRE(records.size() == 2);
  // The store's (entity, kind) order: by id.
  CHECK(records[0].entity == id_of(1));
  CHECK(records[0].importance == 4.0f);
  CHECK(records[0].position.z == 240.0);
  CHECK(records[0].source == nullptr);
  CHECK(records[1].entity == id_of(2));
  CHECK(records[0].kind == schema::stable_type_id(k_node));

  Vector<TileRow> back;
  bool has_snapshot = false;
  store::SnapshotInfo info;
  REQUIRE(store.read_tile(tile, back, has_snapshot, info) == store::Status::Ok);
  CHECK(has_snapshot);
  CHECK(info.sim_tick == 120);
  CHECK(back.size() == 2);
  // A second write replaces the snapshot and moves the tile's last-active time.
  REQUIRE(store.write_tile(tile, std::span<const TileRow>(rows, 1), 300, 5'000'000) ==
          store::Status::Ok);
  REQUIRE(seam.tile_state(seam.context, store_tile(tile), state));
  CHECK(state.last_active.us == 5'000'000);
  // Another tile is untouched.
  REQUIRE(store.read_tile(TileCoord{0, 0}, back, has_snapshot, info) == store::Status::Ok);
  CHECK(back.empty());
  CHECK_FALSE(has_snapshot);
}

TEST_CASE("world store: reconciliation reads the gap, the seed and the records from the store") {
  const test::TempDir tmp("engine_world_tile_store");
  const TileCoord tile{4, -2};
  {
    WorldStore store;
    REQUIRE(store.open(tmp.file("world.db")) == store::Status::Ok);
    TileRow rows[2];
    rows[0].record = id_of(1);
    rows[0].type = k_node;
    rows[0].position = WorldPos{140.0, 0.0, -50.0};
    rows[1].record = id_of(2);
    rows[1].type = k_node;
    rows[1].position = WorldPos{150.0, 0.0, -40.0};
    REQUIRE(store.write_tile(tile, std::span<const TileRow>(rows, 2), 60, 1'000'000) ==
            store::Status::Ok);
  }
  // Another process, later: the file is what carries the tile.
  WorldStore store;
  REQUIRE(store.open(tmp.file("world.db"), false) == store::Status::Ok);
  store.set_world_seed(7);

  sim::SimScheduler scheduler;
  Resolver world;
  world.entities[id_of(1)] = 11;
  world.entities[id_of(2)] = 12;
  sim::MaterializationHooks hooks;
  hooks.name = "resolver";
  hooks.context = &world;
  hooks.materialize = &Resolver::materialize;
  hooks.promote = &Resolver::promote;
  scheduler.add_hooks(hooks);
  Summaries summaries;
  sim::Summarizer summarizer;
  summarizer.name = "test.lod3";
  summarizer.fn = &Summaries::fn;
  summarizer.context = &summaries;
  summarizer.system = 3;
  summarizer.tiers = 0x08u;
  scheduler.wheel().add_summarizer(summarizer);

  sim::ObserverSet observers;
  observers.add(WorldPos{145.0, 0.0, -45.0}, 1.0f);
  sim::ReconcileParams params;
  params.tile = store_tile(tile);
  params.now = GameTime{9'000'000};
  const sim::ReconcileResult result =
      scheduler.reconcile_tile(params, store.tile_store(), observers, sim::TierParams{});
  CHECK(result.known);
  CHECK(result.gap.us == 8'000'000);
  CHECK(result.records == 2);
  CHECK(result.summarized == 1);
  REQUIRE(summaries.seen.size() == 1);
  CHECK(summaries.seen[0].seed == hash_combine(u64{7}, store_tile(tile)));
  CHECK(summaries.seen[0].from.us == 1'000'000);
  CHECK(summaries.seen[0].tile == store_tile(tile));
  CHECK(world.resolved == 2);
  // Both within 32 m of the observer: promoted from LOD2 to LOD0.
  CHECK(result.promoted == 2);
  CHECK(world.promotions == 2);

  // A tile the store never saw is new: nothing summarized, nothing read.
  params.tile = store_tile(TileCoord{0, 0});
  const sim::ReconcileResult fresh =
      scheduler.reconcile_tile(params, store.tile_store(), observers, sim::TierParams{});
  CHECK_FALSE(fresh.known);
  CHECK(fresh.records == 0);
  CHECK(summaries.seen.size() == 1);
}

TEST_CASE(
    "world store: a projection far out keeps the document's f64, and promotes as by the origin") {
  // ADR-0053's far sites, at positions no float32 holds: a projection is the document's position,
  // not the float it rounded to before version 2 (3.1 cm at 419 km, a metre at 10,000 km, 8 m at
  // 1e8 m), and it is what reconciliation promotes from.
  const WorldPos sites[] = {
      WorldPos{419070.2, 1.5, -419072.7},
      WorldPos{-10000000.4, 2.25, 10000000.3},
      WorldPos{100000000.25, -3.0, -100000000.75},
  };
  for (const WorldPos& site : sites) {
    CAPTURE(site.x);
    WorldStore store;
    REQUIRE(store.open_memory() == store::Status::Ok);
    const TileCoord tile = tile_at(site, 32.0f);
    TileRow rows[2];
    rows[0].record = id_of(1);
    rows[0].type = k_node;
    rows[0].position = site;
    rows[1].record = id_of(2);
    rows[1].type = k_node;
    rows[1].position = site + DVec3{0.001, 0.0, -0.3};
    REQUIRE(store.write_tile(tile, std::span<const TileRow>(rows, 2), 60, 1'000'000) ==
            store::Status::Ok);
    Vector<TileRow> back;
    bool has_snapshot = false;
    store::SnapshotInfo info;
    REQUIRE(store.read_tile(tile, back, has_snapshot, info) == store::Status::Ok);
    REQUIRE(back.size() == 2);
    CHECK(back[0].position == rows[0].position);
    CHECK(back[1].position == rows[1].position);

    // Reconciled with an observer 20 m off: both promoted from LOD2 to LOD0 (within 32 m), and the
    // records handed the hooks are at the bits the projections were written with.
    const sim::TileStore seam = store.tile_store();
    Vector<sim::EntityRecord> records;
    seam.load_records(seam.context, store_tile(tile), records);
    REQUIRE(records.size() == 2);
    CHECK(records[0].position == rows[0].position);
    CHECK(records[1].position == rows[1].position);
    sim::SimScheduler scheduler;
    Resolver world;
    world.entities[id_of(1)] = 11;
    world.entities[id_of(2)] = 12;
    sim::MaterializationHooks hooks;
    hooks.name = "resolver";
    hooks.context = &world;
    hooks.materialize = &Resolver::materialize;
    hooks.promote = &Resolver::promote;
    scheduler.add_hooks(hooks);
    sim::ObserverSet observers;
    observers.add(site + DVec3{12.0, 0.0, 16.0}, 1.0f);
    sim::ReconcileParams params;
    params.tile = store_tile(tile);
    params.now = GameTime{9'000'000};
    const sim::ReconcileResult result =
        scheduler.reconcile_tile(params, seam, observers, sim::TierParams{});
    CHECK(result.records == 2);
    CHECK(result.promoted == 2);
  }
}

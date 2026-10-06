#pragma once

// The persistent store's side of a tile (docs/subsystems/world.md, "The store";
// docs/plan/03-data-model.md §3.5, 05 §5.5): a `sim::TileStore` over `foundation/store`, which is
// what `SimScheduler::reconcile_tile` reads when a tile activates — step 1, "load the tile's
// projections from the persistent store", and step 2's "when was it last active" — and the writes
// a tile makes when it goes inactive: one projection per record it held and the tile's snapshot.
//
// **Why the seam is function pointers.** `domain/sim` must not depend on an optional capability
// (ADR-0027 decision 1), so reconciliation asks for "this tile's state" and "this tile's records"
// through `sim::TileStore`, and this is the first implementation of it that is not a test's fake
// (ADR-0028 chose that seam; sim.md said this was the gap). It lives in the world capability rather
// than in `foundation/store` because what a tile *is* — its id packing, what a projection of a
// document record carries, when a snapshot is taken — is the world's; the store stays the thin
// SQLite layer store.md says it is.
//
// **What is persisted, and what is not.** A projection is `engine.world.TileProjection` as
// canonical JSON: the record's id, its type, where it was and its importance — what reconciliation
// needs to place a record and promote it by distance. The record's authored state stays the
// document's, written back through `sim::Materializer`'s `@writeback` rows, which a deactivation
// flushes before it reads a position. The snapshot is the store's own (`EventLog::snapshot`): it
// records the game time the tile went inactive, which is the next activation's
// `TileState::last_active`, and the log sequence it covers. The tile's seed is not stored: it is
// `hash_combine(world seed, tile id)`, a function of the world and the tile like everything else a
// tile is.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/sim/scheduler.h>
#include <foundation/store/database.h>
#include <foundation/store/event_log.h>
#include <systems/world/tile_ring.h>

#include <span>
#include <string>

namespace engine::world {

// One record as a tile's projection writes it.
struct TileRow {
  Id128 record;
  std::string type;
  WorldPos position{};  // where the document had it, f64 (ADR-0053)
  f32 importance = 1.0f;
};

class WorldStore {
 public:
  WorldStore() noexcept = default;
  ENGINE_NON_COPYABLE(WorldStore);

  // Opens the database at `native_path` (creating it when `create`), and the event log's tables in
  // it: the world's projections and snapshots are the store's own tables, not a schema of the
  // world's.
  store::Status open(std::string_view native_path, bool create = true);
  store::Status open_memory();
  void close() noexcept;
  bool is_open() const noexcept { return db_.is_open(); }

  void set_world_seed(u64 seed) noexcept { world_seed_ = seed; }
  u64 world_seed() const noexcept { return world_seed_; }
  // The tile's seed for `SummarizeInterval` (05 §5.5 step 3).
  u64 tile_seed(u64 tile) const noexcept;

  // The reconciliation seam. `tile_state` is the tile's snapshot: known when there is one, last
  // active at its game time. `load_records` is the tile's projections, one `EntityRecord` each, in
  // the store's (entity, kind) order, with no `source` — a hook resolves the entity the document
  // consumer already made, and creates none (sim.md, "Which name each hook carries").
  sim::TileStore tile_store() noexcept;

  // A tile going inactive: its projections replaced by `rows` (one transaction), then its
  // snapshot at (tick, time). A record the tile held before and does not now keeps its old
  // projection until the tile is written again with it gone: `upsert` cannot tell a deletion from
  // a record that moved away, and the store's key leads with the tile (store.md).
  store::Status write_tile(TileCoord tile, std::span<const TileRow> rows, u64 sim_tick,
                           i64 game_time_us);
  // What is stored for `tile`: its projections (decoded) and whether it has a snapshot.
  store::Status read_tile(TileCoord tile, Vector<TileRow>& rows, bool& has_snapshot,
                          store::SnapshotInfo& info);

  store::Database& database() noexcept { return db_; }
  store::EventLog& log() noexcept { return log_; }

 private:
  static bool tile_state(void* context, u64 tile, sim::TileState& out);
  static void load_records(void* context, u64 tile, Vector<sim::EntityRecord>& out);

  store::Database db_;
  store::EventLog log_{db_};
  u64 world_seed_ = 1;
  Vector<u8> scratch_;
};

}  // namespace engine::world

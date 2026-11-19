#pragma once

// The store consumer (docs/subsystems/world.md, "The consumers"; 05 §5.5): a tile that activates
// is reconciled from what the store kept of it — `SimScheduler::reconcile_tile` over the
// `WorldStore`'s `sim::TileStore`, so the gap since the tile was last active is summarized from the
// tile's seed, its projections are read back and its entities promoted by distance — and a tile
// that goes inactive is written: one projection per record it held, and its snapshot.
//
// **It registers after the document consumer.** On activation the document's records are then
// already entities when reconciliation's step 4 resolves them (a record from the store has no
// source, so the entity store's hook resolves and creates nothing) and step 5 promotes them; on
// deactivation, which runs the consumers backwards, it writes the tile while the document consumer
// still holds its records — after flushing the write-back, so the positions it writes are where the
// world left them rather than where the document last heard.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <domain/doc/document.h>
#include <domain/sim/materialize.h>
#include <domain/sim/scheduler.h>
#include <domain/sim/tiers.h>
#include <systems/world/document_tiles.h>
#include <systems/world/tile_ring.h>
#include <systems/world/tile_store.h>
#include <systems/world/world.h>

namespace engine::world {

struct StoreTilesBinding {
  WorldStore* store = nullptr;
  sim::SimScheduler* scheduler = nullptr;
  sim::Materializer* driver = nullptr;      // what the tile holds, and the write-back flush
  const doc::Document* document = nullptr;  // where each record is, for its projection
  const World* world = nullptr;             // the observers reconciliation promotes by
  // The document consumer, when the tile's records follow their tile (document_tiles.h, `settle`):
  // a deactivation flushes through it, so a record the flush moves out of the tile is written
  // under the tile it is in now, not under this one.
  DocumentTiles* document_tiles = nullptr;
  sim::TierParams tiers;    // the entities' tiers (05 §5.4), not the ring's
  u8 materialize_tier = 2;  // 05 §5.5 step 4
};

struct StoreTilesStats {
  u64 reconciled = 0;
  u64 known = 0;  // tiles the store had a snapshot of
  u64 summarized = 0;
  u64 records = 0;
  u64 promoted = 0;
  u64 written = 0;  // tiles written on deactivation
  u64 rows = 0;
  u64 failures = 0;
  i64 reconcile_ns = 0;
  i64 write_ns = 0;
};

class StoreTiles {
 public:
  // **The store takes the document's world seed when it is bound** (W11; world.md, "The world
  // seed"): a document with a settings record gives the store its seed here, before any tile is
  // reconciled from it; one with none leaves the store's as it was (1 unless the host set another).
  // Read once: a later edit of the record moves no tile seed of the run, and `rebind_document`
  // reads nothing.
  explicit StoreTiles(const StoreTilesBinding& binding) noexcept;
  ENGINE_NON_COPYABLE(StoreTiles);

  // The row to register: `rings` is the rings the simulation runs in.
  TileConsumer consumer(u8 rings) noexcept;
  void rebind_document(const doc::Document* document) noexcept { binding_.document = document; }
  const StoreTilesStats& stats() const noexcept { return stats_; }
  const sim::ReconcileResult& last() const noexcept { return last_; }

 private:
  static bool activate(void* context, const TileEvent& event);
  static void deactivate(void* context, const TileEvent& event);

  StoreTilesBinding binding_;
  StoreTilesStats stats_;
  sim::ReconcileResult last_;
  Vector<Id128> held_;
  Vector<TileRow> rows_;
};

// Where a document record is: its composed position property under the partition of the layer that
// defines it (`doc::position_property`), or the property named `position` or `transform` when no
// layer is partitioned. False when it has none, or one no cell can name (`world_cell_valid`). The
// partition reads x and z; this reads all three, as the f64 the document holds (ADR-0053).
bool record_position(const doc::Document& document, const Id128& id, WorldPos& out);

// The document's world settings record (`engine.world.WorldSettings`, world_settings.schema): at
// this fixed id, "world.settings" and a one in hexadecimal, `776f726c642e73657474696e67730001`.
inline constexpr Id128 k_world_settings =
    Id128::from_parts(0x776F726C642E7365ull, 0x7474696E67730001ull);
inline constexpr const char* k_world_settings_type = "engine.world.WorldSettings";

// The world seed a document names: true with `out` when a live record of the settings type stands
// at `k_world_settings` (its `seed`, or the schema's 1 when the record leaves it out). False, with
// `out` untouched, for a document with none — or with a record of another type at that id, or a
// seed that is not a whole number from 0 to 2^64 - 1, which a validated document never holds.
bool document_world_seed(const doc::Document& document, u64& out);
// **The projections a write-back flush wrote, kept current** (roadmap S11; plan 03 §3.5,
// "Projections ... kept in memory for loaded tiles and in the persistent store for all tiles";
// store.md, "Tiles: projections and snapshots"). A tile's projections were written only when it
// went, so the store said where a live tile's records were when that tile last went, or nothing.
// Called with a flush's batch after the document took it, this replaces the projection of every
// record the flush wrote, as the document now has it, under the tile the document now puts it in —
// one transaction, in (tile, record) order, a moved record's row following it out of its old tile
// — so the store's projections of live tiles are as current as the write-back's cadence. It reads
// the document, never the world. No snapshot: that stays a tile's going. A host whose session
// keeps a store calls it from its write-back sink after the commit, as engine-host's does.
struct ProjectWritebackStats {
  u32 written = 0;  // projections replaced
  u32 untiled = 0;  // records in no tile of a partitioned layer: a tile's projection has no place
  u32 gone = 0;     // records the document no longer has, or whose position no cell can name
};
store::Status project_writeback(WorldStore& world_store, const doc::Document& document,
                                const sim::WriteBackBatch& batch,
                                ProjectWritebackStats* stats = nullptr);

}  // namespace engine::world

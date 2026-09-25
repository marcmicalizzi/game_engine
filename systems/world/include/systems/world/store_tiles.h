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
  explicit StoreTiles(const StoreTilesBinding& binding) noexcept : binding_(binding) {}
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
// layer is partitioned. False when it has none. The partition reads x and z; this reads all three.
bool record_position(const doc::Document& document, const Id128& id, Vec3& out);

}  // namespace engine::world

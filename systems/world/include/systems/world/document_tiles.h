#pragma once

// The document's side of a tile (docs/subsystems/world.md, "The document"; 03 §3.4, 05 §5.5 step
// 4): a tile that activates materializes the records a partitioned document files under it, through
// the scheduler's hooks, and a tile that goes inactive flushes what the world wrote back and
// dematerializes them again. It is `sim::Materializer`'s tile pass
// (`MaterializeScope::of_tile`) and its mirror (`Materializer::dematerialize(scope)`), and nothing
// else: which hooks run, what an entity is and what is written back are the driver's and the entity
// store's, exactly as they are for a whole-document pass.
//
// **One grid.** A ring tile and a document tile must be the same cell, so a partitioned layer whose
// tile size is not the ring's is refused — once, with a sentence — rather than materialized by a
// tile that means a different square. Records in no tile (an unpartitioned layer, a record with no
// position) are the host's to materialize once, whole (`MaterializeScope::untiled()`), because no
// tile ever activates for them.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <domain/doc/document.h>
#include <domain/sim/materialize.h>
#include <domain/sim/scheduler.h>
#include <systems/world/tile_ring.h>
#include <systems/world/world.h>

namespace engine::world {

struct DocumentTilesStats {
  u64 activated = 0;
  u64 deactivated = 0;
  u64 created = 0;
  u64 dematerialized = 0;
  // Records a system moved into another tile: filed under it, or let go with it not live.
  u64 refiled = 0;
  u64 left = 0;
  // Records a capability's own rule brought into a live tile between two settles — a resident its
  // routine walked home — materialized there (`sim::Materializer::materialize_arrivals`).
  u64 arrived = 0;
  u64 visited = 0;  // records the tile passes looked at: a tile pass is a full pass over the scope
  u64 refused = 0;
  i64 materialize_ns = 0;
  i64 dematerialize_ns = 0;
};

class DocumentTiles {
 public:
  DocumentTiles(sim::Materializer& driver, sim::SimScheduler& scheduler, f32 tile_size) noexcept
      : driver_(&driver), scheduler_(&scheduler), tile_size_(tile_size) {}
  ENGINE_NON_COPYABLE(DocumentTiles);

  // The document the tiles are materialized from. The host rebinds it on every call, because a
  // session's document is the host's (engine-host keeps the world by session id).
  void bind(const doc::Document* document) noexcept;
  // The row to register: `rings` is the rings the simulation runs in, and `ring` — the world's own,
  // `World::ring()` — is what a deactivation settles the records that moved against (`settle`).
  // Without one a deactivation only flushes, as it did before records followed their tile.
  TileConsumer consumer(u8 rings, const TileRing* ring = nullptr) noexcept;
  const DocumentTilesStats& stats() const noexcept { return stats_; }

  // **Records follow their tile** (world.md, "Records that move"). A record a system moved into
  // another tile, once a write-back has told the document, is filed under its new tile when the
  // ring holds that tile in this consumer's rings, and dematerialized when it does not — its state
  // is in the document, which is where the new tile's activation will find it. So what is
  // materialized is always the records of the live tiles, a function of the document and the ring
  // and not of the way the world came to be as it is, which is what lets a save be loaded into the
  // same world. Between ticks: the host after every update and at the end of a call; a deactivation
  // itself after its flush and before it lets its tile go (`flush`). Returns the records let go.
  //
  // It first brings in the records the driver's tile sources report as arrived in a live tile
  // (`sim::Materializer::materialize_arrivals`; sim.md, "Records the document has not caught up
  // with"): a resident whose routine walked it home to a live tile from one the ring does not
  // simulate, which its record's tile alone would never bring in. Settling is where it goes because
  // it is the one call every host already makes between ticks, before the ring updates.
  u32 settle(const TileRing& ring);
  // The write-back flushed, and the moves it made settled against `ring`: what a consumer calls
  // before it reads or drops what a tile holds.
  void flush(const TileRing& ring);
  // False, with the reason, when a partitioned layer of the bound document is on another grid.
  bool grid_matches(const char** why = nullptr) const noexcept;

 private:
  static bool activate(void* context, const TileEvent& event);
  static void deactivate(void* context, const TileEvent& event);

  sim::Materializer* driver_ = nullptr;
  sim::SimScheduler* scheduler_ = nullptr;
  const doc::Document* document_ = nullptr;
  // The ring this consumer's deactivations settle against: the world it is registered with.
  const TileRing* ring_ = nullptr;
  f32 tile_size_ = 0.0f;
  u8 rings_ = 0;
  bool grid_ok_ = true;
  bool warned_ = false;
  DocumentTilesStats stats_;
  Vector<sim::TileMove> moves_;
  Vector<Id128> gone_;
};

}  // namespace engine::world

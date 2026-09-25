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
  TileConsumer consumer(u8 rings) noexcept;
  const DocumentTilesStats& stats() const noexcept { return stats_; }
  // False, with the reason, when a partitioned layer of the bound document is on another grid.
  bool grid_matches(const char** why = nullptr) const noexcept;

 private:
  static bool activate(void* context, const TileEvent& event);
  static void deactivate(void* context, const TileEvent& event);

  sim::Materializer* driver_ = nullptr;
  sim::SimScheduler* scheduler_ = nullptr;
  const doc::Document* document_ = nullptr;
  f32 tile_size_ = 0.0f;
  bool grid_ok_ = true;
  bool warned_ = false;
  DocumentTilesStats stats_;
};

}  // namespace engine::world

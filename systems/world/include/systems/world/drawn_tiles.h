#pragma once

// **The drawn ground** (docs/subsystems/world.md, "The consumers"; renderer.md, "The ground from
// the world's tiles"; ADR-0050): the tiles the renderer draws the ground from, as the world's ring
// holds them. For every tile the ring holds it keeps the tile's ring, and at `commit` — once an
// update in which any changed — it hands **what changed** to the renderer's tile set
// (`renderer::TerrainTileSet::change_tiles`: tiles that entered or changed ring, and tiles let go),
// which rebuilds only the tiles whose key changed and swaps them in at once. Until 2026-10-03 it
// handed the whole set, 14,000 tiles a commit on the endless desert, which the renderer sorted and
// compared every frame of a flight. It builds nothing and holds nothing but the set: the heights
// come from the renderer's tile source, the same one the walker's collision reads.
//
// **Where in the order.** After the ground consumer where a world has one (a tile's record is read
// before the tile is drawn) and before the placements (a tile is drawn before anything is placed on
// it); it acts in every ring. The renderer's first layout is the ring's own first-fill rule
// (`renderer::terrain_tiles_round`), so the world's first update from nothing hands over the set
// the renderer already drew and nothing is rebuilt.
//
// **Nothing in steady state.** A tile coming or going touches one entry of a map that has grown to
// the most the ring has held; `commit` fills a list kept between updates. An update that changes no
// tile does not call it.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <systems/renderer/terrain_tiles.h>
#include <systems/world/tile_ring.h>
#include <systems/world/world.h>

namespace engine::world {

struct DrawnTilesStats {
  u32 held = 0;     // now
  u64 commits = 0;  // sets handed over
  u64 changed = 0;  // of which the renderer took as a change
  u64 activated = 0;
  u64 moved = 0;  // a tile's change of ring
  u64 deactivated = 0;
  i64 commit_ns = 0;
  i64 max_commit_ns = 0;
};

class DrawnTiles {
 public:
  DrawnTiles() noexcept = default;
  ENGINE_NON_COPYABLE(DrawnTiles);

  // The renderer's tile set it hands the held tiles to; it must outlive the consumer.
  void create(renderer::TerrainTileSet& tiles) noexcept;
  bool valid() const noexcept { return tiles_ != nullptr; }
  // The row to register with the world, acting in every ring.
  TileConsumer consumer() noexcept;
  // The world ring's parameters a tile set was built for: its tiles, rings and hysteresis, and the
  // budget from the world's tunables. What a host configures the ring that drives it with.
  static RingParams ring_params(const renderer::TerrainTilesDesc& tiles) noexcept;

  // The tiles held, in the order the map holds them, and what the consumer did.
  const HashMap<u64, u8>& held() const noexcept { return held_; }
  const DrawnTilesStats& stats() const noexcept { return stats_; }

 private:
  static bool on_activate(void* context, const TileEvent& event);
  static bool on_change(void* context, const TileEvent& event);
  static void on_deactivate(void* context, const TileEvent& event);
  static void on_commit(void* context);

  renderer::TerrainTileSet* tiles_ = nullptr;
  HashMap<u64, u8> held_;  // tile_key -> ring
  // The changes since the last commit, in the order the ring made them (`k_tile_gone` for a tile
  // let go); kept between updates.
  Vector<renderer::TerrainTile> handed_;
  DrawnTilesStats stats_;
};

}  // namespace engine::world

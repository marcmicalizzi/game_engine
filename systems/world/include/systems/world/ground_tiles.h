#pragma once

// The ground consumer (docs/subsystems/world.md, "The consumers"; scene_gen.md; ADR-0046): a tile
// of the scene's ground for every tile the ring holds, whatever ground provider the terrain names —
// the ground's own tiles (`scene_gen::GroundOps::open_tiles`; the terrain capability's dunes hold a
// tile of their field at the ring's resolution and a deformation overlay per tile) — with each
// tile's record kept in the persistent store between activations, under the provider's record type
// (`scene_gen::GroundOps::record`, the dunes' `engine.terrain.OverlayTile`). It names no ground and
// links none: the provider is the scene's (`renderer::TerrainSampler::provider()`), or one a caller
// made through the registry, and a host that links the capability reaches the ground's own half of
// a tile through the capability's typed accessor on `tiles()` (the dunes': `terrain::dune_tiles`).
//
// **Ground first.** Register it before the placements consumer, so a tile's ground is built before
// anything on it (ADR-0040's declared order, ADR-0046's rule), and after the store consumer:
// deactivation runs in reverse, so the ground writes a tile's record before the store consumer
// snapshots the tile, and the tile's snapshot carries it (store.md: a snapshot packs every
// projection of the tile).
//
// **Time is the host's.** The world's update carries a tick, not a game time; the host says what
// time it is (`set_time`) before an update, as it moves its observers.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/world/tile_ring.h>
#include <systems/world/tile_store.h>
#include <systems/world/world.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::world {

struct GroundTilesConfig {
  f32 tile_size = 32.0f;  // the ring's
  // Where a tile's record lives between activations. Null: in memory only, and dropped with its
  // tile — what a world with no store (engine-view today) gets.
  WorldStore* store = nullptr;
};

// Where the tiles a commit built go (a renderer's terrain tiles, when one takes them).
using GroundSink = void (*)(void* context, std::span<const TileCoord> built);

class GroundTiles {
 public:
  GroundTiles() noexcept = default;
  ~GroundTiles();
  ENGINE_NON_COPYABLE(GroundTiles);

  // Opens `ground`'s tiles on the ring's grid, their records in `config.store`. False, with a
  // sentence, for a ground that has no tiles a world could hold (the waves). `ground` must outlive
  // the consumer.
  bool create(const scene_gen::GroundProvider& ground, const GroundTilesConfig& config,
              std::string* error = nullptr);
  bool valid() const noexcept { return tiles_.valid(); }

  // The row to register with the world, acting in `rings`: before the placements consumer's.
  TileConsumer consumer(u8 rings = 0x7Fu) noexcept;
  void set_sink(GroundSink sink, void* context) noexcept {
    sink_ = sink;
    sink_context_ = context;
  }
  // The game time the next activations are built at. Never backwards.
  void set_time(i64 time_us);

  // The ground's own half of the held tiles, for its capability's typed accessor.
  scene_gen::GroundTiles& tiles() noexcept { return tiles_; }

  // The store's key for a tile's record of `record` type: its projection's entity and kind — one
  // row per tile, never a document record's id.
  static Id128 record_entity(std::string_view record, TileCoord tile) noexcept;
  static u32 record_kind(std::string_view record) noexcept;

 private:
  static bool on_activate(void* context, const TileEvent& event);
  static bool on_change(void* context, const TileEvent& event);
  static void on_deactivate(void* context, const TileEvent& event);
  static void on_commit(void* context);
  static bool load(void* context, scene_gen::TileCoord tile, Vector<u8>& out);
  static bool save(void* context, scene_gen::TileCoord tile, std::span<const u8> bytes);
  static void erase(void* context, scene_gen::TileCoord tile);

  scene_gen::GroundTiles tiles_;
  WorldStore* store_ = nullptr;
  std::string record_;
  Id128 record_hi_{};  // the entity's high half, hashed from the record type once
  u32 kind_ = 0;
  Vector<scene_gen::TileCoord> built_;
  Vector<TileCoord> handed_;
  GroundSink sink_ = nullptr;
  void* sink_context_ = nullptr;
};

}  // namespace engine::world

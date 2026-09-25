#pragma once

// The terrain consumer (docs/subsystems/world.md, "The consumers"; docs/subsystems/terrain.md;
// ADR-0043): a tile of the terrain capability's dune field for every tile the ring holds, and the
// tile's deformation overlay kept in the persistent store. Compiled where the terrain capability is
// (ENGINE_WORLD_TERRAIN), the way the ruins consumer is compiled where the ruins are.
//
//   activation    the tile's overlay read from the store (a projection of kind
//                 `engine.terrain.OverlayTile` filed under the tile) and caught up to now — exact
//                 whatever time passed, since decay is a closed form — or a fresh one; the tile's
//                 lag and its neighbours' put in the lag field; the tile's field evaluated at now,
//                 at the ring's cells a side (128 inner, 64 middle, 32 beyond: E36)
//   change_ring   the field evaluated again at the new ring's resolution; the overlay is kept
//   deactivation  the overlay advanced to now and written back — or its projection erased when it
//                 is empty — and the tile dropped
//   commit        the tiles built or rebuilt since the last commit handed to a sink
//
// **Register it after the store consumer.** Deactivation runs in reverse registration order, so
// this consumer writes the overlay's projection before the store consumer snapshots the tile, and
// the tile's snapshot carries it (store.md: a snapshot packs every projection of the tile).
//
// **Time is the host's.** The world's update carries a tick, not a game time; the host says what
// time it is (`set_time`) before an update, as it moves its observers. Between updates the host
// may stamp (`deform`) and advance (`advance`) the overlays of held tiles; both are exact at any
// cadence (overlay.h), so a host that advances once a tick and one that advances once a second
// write the same records.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <domain/terrain/terrain.h>
#include <systems/world/tile_ring.h>
#include <systems/world/tile_store.h>
#include <systems/world/world.h>

#include <memory>
#include <span>

namespace engine::world {

struct TerrainTilesConfig {
  // The world's field, which must outlive the consumer.
  const terrain::DuneField* field = nullptr;
  i64 tile_mm = 32'000;  // the ring's tile, which must be the field's tiles'
  // Cells a side of a tile built in each ring: the overlay's 25 cm grid where the player is,
  // coarser beyond (E36: 2.6, 0.83 and 0.28 ms a tile).
  u32 cells[k_max_rings] = {128, 64, 32, 32, 32, 32, 32};
  terrain::OverlayRules rules;
  // Where overlays live between activations. Null: in memory only, and dropped with their tile —
  // what a world with no store (engine-view today) gets.
  WorldStore* store = nullptr;
  // The drifts declared on a tile — the ruins', where a host has them — or null for none. Called
  // for a tile and its neighbours on activation; must be a function of the tile.
  void (*drifts)(void* context, TileCoord tile, Vector<terrain::DriftDecl>& out) = nullptr;
  void* drifts_context = nullptr;
};

// Where the tiles a commit built go (a renderer's terrain tiles, when one takes them).
using TerrainSink = void (*)(void* context, std::span<const TileCoord> built);

struct TerrainTilesStats {
  u64 built = 0;  // tile evaluations, activations and ring changes
  u64 dropped = 0;
  u64 loaded = 0;   // overlays read from the store
  u64 written = 0;  // overlays written to it
  u64 erased = 0;   // overlays found empty on the way out, their projection removed
  u64 refused = 0;  // records the store held that could not be read (other rules, damage)
  u64 stamps = 0;   // stamps taken by held tiles
  u64 largest_record = 0;
  i64 build_ns = 0;
  i64 max_build_ns = 0;
  i64 store_ns = 0;
  u32 held = 0;
};

class TerrainTiles {
 public:
  explicit TerrainTiles(const TerrainTilesConfig& config);
  ~TerrainTiles();
  ENGINE_NON_COPYABLE(TerrainTiles);

  // The row to register with the world, acting in `rings`.
  TileConsumer consumer(u8 rings = 0x7Fu) noexcept;
  void set_sink(TerrainSink sink, void* context) noexcept;

  // The game time the next activations are built at and overlays advanced to. Never backwards.
  void set_time(i64 time_us) noexcept;
  i64 time_us() const noexcept { return time_us_; }

  // A stamp at its own time (not before `time_us()`), into every held tile its footprint covers.
  // False when it touches none: a deformation far from anyone is not kept.
  bool deform(const terrain::Stamp& stamp);
  // Every held overlay baked and decayed to `time_us`; tiles whose lag or whose neighbours' lag
  // changed are rebuilt, so tiles stay seamless.
  void advance(i64 time_us);

  bool holds(TileCoord tile) const noexcept;
  const terrain::TileOutput* tile(TileCoord tile) const noexcept;
  const terrain::Overlay* overlay(TileCoord tile) const noexcept;
  const terrain::LagField& lag() const noexcept { return lag_; }
  const TerrainTilesStats& stats() const noexcept { return stats_; }

  // The store's key for a tile's overlay: its projection's entity and kind.
  static Id128 overlay_entity(TileCoord tile) noexcept;
  static u32 overlay_kind() noexcept;

 private:
  struct Held;
  static bool on_activate(void* context, const TileEvent& event);
  static bool on_change(void* context, const TileEvent& event);
  static void on_deactivate(void* context, const TileEvent& event);
  static void on_commit(void* context);

  void build(Held& held, u8 ring);
  void load(Held& held);
  void write(Held& held);
  // The tile's lag: its overlay's (held) or its stored record's, plus its drifts', clamped.
  terrain::TileLag total_lag(TileCoord tile);
  terrain::TileLag stored_lag(TileCoord tile);
  void refresh_lags(TileCoord centre);

  Held* find(TileCoord tile) const noexcept;

  TerrainTilesConfig config_;
  // The held tiles: owned by `slots_`, found through `index_` by `store_tile`.
  Vector<std::unique_ptr<Held>> slots_;
  HashMap<u64, u32> index_;
  terrain::LagField lag_;
  Vector<TileCoord> built_;
  TerrainSink sink_ = nullptr;
  void* sink_context_ = nullptr;
  i64 time_us_ = 0;
  TerrainTilesStats stats_;
  Vector<u8> scratch_;
  Vector<terrain::DriftDecl> drifts_;
};

}  // namespace engine::world

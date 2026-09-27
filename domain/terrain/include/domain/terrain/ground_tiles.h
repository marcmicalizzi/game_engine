#pragma once

// The dunes' tiles in a world (docs/subsystems/terrain.md, "Where it attaches"; world.md, "The
// consumers"; scene_gen.md; ADR-0043, ADR-0046): what the ground provider "dunes" answers a world's
// ground consumer with (`scene_gen::GroundOps::open_tiles`) — a tile of the field for every tile
// the ring holds, and the tile's deformation overlay kept wherever the world keeps a tile's record
// (`scene_gen::TileRecords`, the world's store). The world's consumer (`world::GroundTiles`) is
// generic and names no ground; this is the dunes' half, and a host that links the capability
// reaches it through `dune_tiles` to stamp and advance.
//
//   activation    the tile's overlay read from its record and caught up to now — exact whatever
//                 time passed, since decay is a closed form — or a fresh one; the tile's lag and
//                 its neighbours' put in the lag field; the tile's field evaluated at now, at the
//                 ring's cells a side (128 inner, 64 middle, 32 beyond: E36)
//   change_ring   the field evaluated again at the new ring's resolution; the overlay is kept
//   deactivation  the overlay advanced to now and written back — or its record erased when it is
//                 empty — and the tile dropped
//
// **Time is the host's.** The world's update carries a tick, not a game time; the host says what
// time it is (`set_time`) before an update, as it moves its observers. Between updates the host
// may stamp (`deform`) and advance (`advance`) the overlays of held tiles; both are exact at any
// cadence (overlay.h), so a host that advances once a tick and one that advances once a second
// write the same records. It was the world's `TerrainTiles`, compiled under `ENGINE_WORLD_TERRAIN`
// with a conditional link to this capability, until 2026-09-27; the rules did not change.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/terrain/dunes.h>
#include <domain/terrain/feedback.h>
#include <domain/terrain/overlay.h>
#include <domain/terrain/tile.h>

#include <memory>
#include <span>

namespace engine::terrain {

// The schema type a tile's overlay record is (`engine.terrain.OverlayTile`, terrain.schema): what
// the world's store files it under.
inline constexpr const char* k_overlay_record = "engine.terrain.OverlayTile";
// The rings a tile's resolution is set for; a ring past the last takes the last's.
inline constexpr u32 k_tile_rings = 8;

struct DuneTilesStats {
  u64 built = 0;  // tile evaluations, activations and ring changes
  u64 dropped = 0;
  u64 loaded = 0;   // overlays read from their records
  u64 written = 0;  // overlays written to them
  u64 erased = 0;   // overlays found empty on the way out, their record removed
  u64 refused = 0;  // records that could not be read (other rules, damage)
  u64 stamps = 0;   // stamps taken by held tiles
  u64 largest_record = 0;
  i64 build_ns = 0;
  i64 max_build_ns = 0;
  i64 store_ns = 0;  // reading and writing records
  u32 held = 0;
};

class DuneTiles {
 public:
  // `field` and whatever `records` points at must outlive the tiles.
  DuneTiles(const DuneField& field, const scene_gen::TileRecords& records, i64 tile_mm,
            const OverlayRules& rules);
  ~DuneTiles();
  ENGINE_NON_COPYABLE(DuneTiles);

  // Cells a side of a tile built in `ring`: the overlay's 25 cm grid where the player is, coarser
  // beyond (E36: 2.6, 0.83 and 0.28 ms a tile). Before the first activation.
  void set_cells(u32 ring, u32 cells) noexcept;
  u32 cells(u32 ring) const noexcept;
  // The drifts declared on a tile — the ruins', where a host has them — or none. Called for a tile
  // and its neighbours on activation; must be a function of the tile.
  using DriftsFn = void (*)(void* context, TileCoord tile, Vector<DriftDecl>& out);
  void set_drifts(DriftsFn fn, void* context) noexcept;

  // The world's ground consumer's calls (`scene_gen::GroundTilesOps`).
  bool activate(TileCoord tile, u8 ring);
  bool change_ring(TileCoord tile, u8 ring);
  void deactivate(TileCoord tile);
  // The tiles built or rebuilt since the last call.
  void take_built(Vector<TileCoord>& out);

  // The game time the next activations are built at and overlays advanced to. Never backwards.
  void set_time(i64 time_us) noexcept;
  i64 time_us() const noexcept { return time_us_; }

  // A stamp at its own time (not before `time_us()`), into every held tile its footprint covers.
  // False when it touches none: a deformation far from anyone is not kept.
  bool deform(const Stamp& stamp);
  // Every held overlay baked and decayed to `time_us`; tiles whose lag or whose neighbours' lag
  // changed are rebuilt, so tiles stay seamless.
  void advance(i64 time_us);

  bool holds(TileCoord tile) const noexcept;
  const TileOutput* tile(TileCoord tile) const noexcept;
  const Overlay* overlay(TileCoord tile) const noexcept;
  const LagField& lag() const noexcept { return lag_; }
  const OverlayRules& rules() const noexcept { return rules_; }
  const DuneField& field() const noexcept { return *field_; }
  const DuneTilesStats& stats() const noexcept { return stats_; }

 private:
  struct Held;
  void build(Held& held, u8 ring);
  void load(Held& held);
  void write(Held& held);
  // The tile's lag: its overlay's (held) or its stored record's, plus its drifts', clamped.
  TileLag total_lag(TileCoord tile);
  TileLag stored_lag(TileCoord tile);
  void refresh_lags(TileCoord centre);
  Held* find(TileCoord tile) const noexcept;

  const DuneField* field_;
  scene_gen::TileRecords records_;
  i64 tile_mm_;
  OverlayRules rules_;
  u32 cells_[k_tile_rings] = {128, 64, 32, 32, 32, 32, 32, 32};
  DriftsFn drifts_fn_ = nullptr;
  void* drifts_context_ = nullptr;
  // The held tiles: owned by `slots_`, found through `index_` by `tile_key`.
  Vector<std::unique_ptr<Held>> slots_;
  HashMap<u64, u32> index_;
  LagField lag_;
  Vector<TileCoord> built_;
  i64 time_us_ = 0;
  DuneTilesStats stats_;
  Vector<u8> scratch_;
  Vector<DriftDecl> drifts_;
};

// The dunes' tiles as a world's generic handle holds them (what `scene_gen::GroundOps::open_tiles`
// of the provider "dunes" makes), for the provider's own `open_tiles`.
scene_gen::GroundTiles make_dune_tiles(const DuneField& field,
                                       const scene_gen::TileRecords& records, i64 tile_mm);
// The dunes' tiles behind a world's handle, or null for another ground's: how a host that links the
// capability stamps, advances and reads them.
DuneTiles* dune_tiles(const scene_gen::GroundTiles& tiles) noexcept;

}  // namespace engine::terrain

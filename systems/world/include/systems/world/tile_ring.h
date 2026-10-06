#pragma once

// The tile ring (docs/subsystems/world.md, docs/plan/03-data-model.md §3.7, 05 §5.4–5.5): which
// tiles of the fixed grid are active, and at which ring, as a function of the observer set.
//
// **A ring is a tier, and the ring is `sim::TierAssignment`.** A tile's ring is the minimum over
// observers of the distance from the observer to the tile's centre on the ground plane, divided by
// the observer's weight — exactly the score an entity's simulation tier is (05 §5.4) — banded by
// one radius per ring, with the same hysteresis and the same per-update rate limits. So the ring
// is not a second LOD policy beside the simulation's: it is the simulation's reference function
// (`TierAssignment::score`/`tier_of`, which a capability's policy is meant to agree with) applied
// to tiles, with one more tier than there are rings to mean "inactive". Two players, a camera and
// a quest marker are all observers, and none of this knows which one is "the player".
//
// **What an update costs is the tiles within reach, not the world.** An update enumerates the
// tiles whose centres are within the outermost radius (widened by the hysteresis band and one
// tile) of some observer, merges them with the tiles already active, scores that set and bands
// it. Nothing ticks per entity, and nothing here knows what a tile holds: that is the consumers'
// (world.h).
//
// **Deterministic from the observer set.** The tracked tiles are kept sorted by `tile_key`, the
// tier code emits its changes in index order, and every decision is `TierAssignment`'s — which
// is itself a function of the inputs alone, its ties broken on the index — so the same sequence
// of observer sets produces the same events, byte for byte, on any machine and at any job count
// (05 §5.10). The events of an update come out in tile order; a tile changes at most once per
// update, so the ring each event names orders nothing further.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/sim/tiers.h>

#include <span>

namespace engine::world {

// A tile of the fixed grid: tile (x, z) covers [x, x + 1) * tile_size in world x and z (y up).
// It is the cell `ruins::TileCoord` names at the same size, and the one a partitioned document
// layer files a record under (`doc::TileCoord`, whose `y` is world z).
struct TileCoord {
  i32 x = 0;
  i32 z = 0;
  friend bool operator==(TileCoord, TileCoord) noexcept = default;
};

// **Tile order** is x, then z, both ascending as signed integers; the key is that order as one
// unsigned integer, so a sorted array of keys is the tiles in tile order.
constexpr u64 tile_key(TileCoord tile) noexcept {
  return (u64{static_cast<u32>(tile.x) ^ 0x80000000u} << 32) |
         u64{static_cast<u32>(tile.z) ^ 0x80000000u};
}
constexpr TileCoord tile_of_key(u64 key) noexcept {
  return TileCoord{static_cast<i32>(static_cast<u32>(key >> 32) ^ 0x80000000u),
                   static_cast<i32>(static_cast<u32>(key) ^ 0x80000000u)};
}

// The persistent store's id for a tile: x in the high 32 bits and z in the low, each as its 32-bit
// two's complement. It is the packing engine-host's write-back events already file a record under
// (docs/subsystems/store.md, "Write-back events"), so a tile's events and its snapshot share one
// id.
constexpr u64 store_tile(TileCoord tile) noexcept {
  return (u64{static_cast<u32>(tile.x)} << 32) | u64{static_cast<u32>(tile.z)};
}

// The tile a world position is in, and a tile's centre on the ground plane (y = 0), both in f64
// (ADR-0053): `floor(x / size)` of the f64 coordinate, never of a float32 metre, which at 10,000 km
// steps by a metre and at 1e8 m by eight, and so puts a point by a tile's edge in the wrong tile.
// The division is exact for a power-of-two size (32 m, 64 m); for another size it rounds once, at
// the size of the quotient, and a point within that rounding of an edge may land on either side —
// the same side on every machine, since IEEE division is.
TileCoord tile_at(WorldPos position, f32 tile_size) noexcept;
WorldPos tile_center(TileCoord tile, f32 tile_size) noexcept;

// Rings in use are at most one fewer than the simulation's tiers: the last tier is "inactive".
inline constexpr u32 k_max_rings = sim::k_max_tiers - 1;
// The ring of a tile that is not active.
inline constexpr u8 k_inactive = 0xFFu;

// What the ring is asked to be. The defaults are docs/subsystems/world.md's ("The ring's rules and
// defaults"), with the measurement each one rests on; `docs/adr/0039-...` records the decision.
struct RingParams {
  // Metres. The scene's own (its ruins' `tile_size`), 32 m today; the plan's 64–128 m is a game's
  // choice and this is a parameter for exactly that reason.
  f32 tile_size = 32.0f;
  u32 ring_count = 3;
  // Radius of each ring in tiles, from an observer to a tile's centre, innermost first and
  // increasing. 1.5 is the 3 x 3 tiles round the observer's own; 8 is where the ruins' own LOD
  // policy drops a tile's debris (`ruins::detail_for_distance`); 24 (770 m at 32 m tiles) is where
  // E34's far building had fallen to 73 visible pairs in sections.
  f32 radius[k_max_rings] = {1.5f, 8.0f, 24.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  // A tile leaves a ring only past its radius times (1 + hysteresis): `TierAssignment`'s band, so
  // an observer walking along a boundary crosses it once.
  f32 hysteresis = 0.15f;
  // Changes per update: a tile activated or moved inward is a promotion, nearest first; moved
  // outward or deactivated, a demotion, farthest first. The rest wait for the next update, so a
  // moving observer never activates a burst. Zero means unlimited.
  u32 max_activations = 8;
  u32 max_deactivations = 16;
};

// True when the parameters make a ring: a positive tile size, 1..k_max_rings rings of positive,
// strictly increasing radius, a non-negative hysteresis. `error` says which is wrong.
bool valid_ring_params(const RingParams& params, const char** error = nullptr) noexcept;

enum class TileEventKind : u8 {
  Activate = 0,    // inactive -> `to`
  ChangeRing = 1,  // `from` -> `to`, both rings
  Deactivate = 2,  // `from` -> inactive
};
const char* tile_event_name(TileEventKind kind) noexcept;

// One change of one tile. Its bytes are compared by the tests, so the padding is a named, zeroed
// member rather than whatever the allocator left (the rule sim.md states for `TierChange`).
struct TileEvent {
  TileCoord tile;
  TileEventKind kind = TileEventKind::Activate;
  u8 from = k_inactive;
  u8 to = k_inactive;
  u8 pad = 0;
};

// What the last update did and what the ring holds after it.
struct RingStats {
  u32 tracked = 0;    // tiles scored: those within reach plus those still active
  u32 active = 0;     // tiles in some ring after the update
  u32 activated = 0;  // events of this update, by kind
  u32 changed = 0;
  u32 deactivated = 0;
  u32 deferred_promotions = 0;  // wanted to activate or move in, and lost to the rate limit
  u32 deferred_demotions = 0;
  u32 per_ring[k_max_rings] = {};  // active tiles in each ring
};

class TileRing {
 public:
  TileRing() noexcept = default;
  explicit TileRing(const RingParams& params) { configure(params); }
  ENGINE_NON_COPYABLE(TileRing);

  // Sets the parameters. Only before the first update, or after `clear`: the rings of the tiles
  // already active were decided against the old radii.
  void configure(const RingParams& params);
  const RingParams& params() const noexcept { return params_; }

  // One update: appends this update's events to `events` in tile order and returns how many.
  // `unlimited` lifts the rate limits for this update — a first fill, or a host that would rather
  // wait for a teleport than draw a world that arrives over several frames.
  u32 update(const sim::ObserverSet& observers, Vector<TileEvent>& events, bool unlimited = false);
  // Deactivates every active tile, appending the events in tile order.
  u32 clear(Vector<TileEvent>& events);
  // Makes `keys` the active tiles at `rings`, as a save kept them (docs/subsystems/world.md, "Save
  // and load"), on a ring that holds none, with `observers` as the last update's: appends one
  // activation per tile, in tile order, and returns how many. The ring a tile is in depends on the
  // way the observers came, not only on where they are (hysteresis and the budget), so it is
  // restored rather than recomputed; the next update goes on from it exactly as it would have gone
  // on from the update that left it. Refused, with nothing changed, when the ring already holds
  // tiles, the keys are not strictly ascending, or a ring is not one of this ring's.
  bool restore(std::span<const u64> keys, std::span<const u8> rings,
               const sim::ObserverSet& observers, Vector<TileEvent>& events,
               const char** error = nullptr);

  u32 active_count() const noexcept { return active_keys_.size(); }
  // The ring of `tile`, or `k_inactive`.
  u8 ring_of(TileCoord tile) const noexcept;
  // The tile's score against the last update's observers — the ground distance from its centre to
  // the nearest, divided by that observer's weight — which is what its ring was banded from.
  f32 score_of(TileCoord tile) const noexcept;
  // The active tiles in tile order, and their rings.
  std::span<const u64> active_keys() const noexcept {
    return {active_keys_.data(), active_keys_.size()};
  }
  std::span<const u8> active_rings() const noexcept {
    return {active_rings_.data(), active_rings_.size()};
  }
  const RingStats& stats() const noexcept { return stats_; }

 private:
  void add_candidates(WorldPos observer, f32 weight);

  RingParams params_;
  sim::TierParams tiers_params_;
  sim::TierAssignment tiers_;
  sim::ObserverSet ground_;  // the observers on the ground plane: tiles are columns (03 §3.7)
  f32 reach_tiles_ = 0.0f;   // the enumeration window's radius, in tiles
  // What is active, sorted by key.
  Vector<u64> active_keys_;
  Vector<u8> active_rings_;
  // One update's scratch, reused.
  Vector<u64> candidates_;
  Vector<u64> tracked_;
  Vector<WorldPos> centers_;
  Vector<f32> importance_;
  Vector<u8> tier_;
  Vector<sim::TierChange> changes_;
  RingStats stats_;
};

}  // namespace engine::world

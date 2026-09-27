#pragma once

// The world capability (docs/subsystems/world.md; ADR-0027; docs/plan/03-data-model.md §3.7,
// 05 §5.4–5.5, 04 §4.9): **world tile streaming**. It owns the tile grid, the observer set and the
// tile ring (tile_ring.h), and turns the ring's changes into calls on the **consumers** registered
// with it — what a tile *is* to the renderer, the document and the persistent store is theirs:
//
//   ground_tiles.h    a tile of the scene's ground — the dunes' field and deformation overlay —
//                     with its record kept in the store, from the ground provider the terrain names
//   placement_tiles.h what a scene's placement generators put on a tile — its ruined building
//                     (blocks in the inner ring, sections beyond: E34), a district's proxies — as
//                     instances the renderer adds and removes between frames, each generator found
//                     in the scene-generator registry by name (ADR-0046)
//   document_tiles.h  the tile of a partitioned document, materialized through the scheduler's
//                     hooks (`sim::Materializer`) and dematerialized again
//   tile_store.h      the tile's projections and snapshot in `foundation/store`, which
//                     `SimScheduler::reconcile_tile` reads (05 §5.5 step 1) and deactivation writes
//
// and beside them what a run is saved, loaded and replayed through (world.md, "Save and load"):
//
//   input_observer.h  an observer an input log steers: the player of a headless world
//   state_hash.h      the persistent-state hash two runs of a world compare (05 §5.10)
//   save_game.h       a save: the document, the store's backup, the ring, and a manifest
//
// **Consumers are a table, not a base class**, for the reason `sim::MaterializationHooks` is: a
// row of function pointers and a context, walked in registration order, so a world with no
// placements consumer has no placements row and costs nothing for it, and nothing here names a
// consumer's type. Activation (and a move between rings) calls the consumers **in registration
// order**; deactivation calls them **in reverse**, the mirror of construction — whatever a later
// consumer built on a tile (the store's reconciliation resolves the document consumer's entities)
// lets go before what it was built on does. Each consumer names the rings it acts in; a tile moving
// out of them is a deactivation *to that consumer* even while it stays active in the ring.
//
// **Between ticks, never inside one.** An update materializes and dematerializes, and the driver
// (`sim::Materializer`) must be called by whoever owns the tick, outside it (sim.md, "Between
// ticks"): the entity store's hook refuses a call while the world is deferred. So the host calls
// `update` between two `SimScheduler::step()`s — engine-host's headless run once a tick,
// engine-view once a frame — and it is not a system in the scheduler's table.
//
// Scaffolded by tools/new-capability.ps1. ADR-0027 decision 2, and where each point stands:
//
//   [x] schema types      schemas/world_tiles.schema (the store's projection, the world log's line)
//   [-] scheduler entry   none: the update runs between ticks (above), not in a phase
//   [-] render passes     none: the placements consumer hands the renderer instances it already
//   draws
//   [-] derived data      none: a tile's building is assembled at activation, from its seed
//   [x] protocol methods  session.save_game, load_game, state_hash (engine-host); run_headless
//                         takes the ring (apps.md)
//   [x] tunables          world.ring.max_activations / max_deactivations (the per-update budget)
//   [x] LOD policy        the ring itself: sim::TierAssignment over tiles, with hysteresis
//   [x] determinism       k_determinism below
//   [x] zero cost unused  no linked code; a scene with no world runs no ring (world.md)
//   [x] docs, tests, size table, bench

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/sim/tiers.h>
#include <systems/world/tile_ring.h>

#include <span>

namespace engine::world {

// Determinism stance (ADR-0010): `hashed`. Which tiles are active decides what is materialized
// and reconciled, so it is simulation state, and it is a function of the sequence of observer sets
// alone (tile_ring.h). A replay that feeds the same observers gets the same events in the same
// order, and so the same consumers' calls. What the placements consumer builds from them is
// `derived`.
inline constexpr const char* k_determinism = "hashed";

// The per-update budget, from the tunables (`world.ring.max_activations`,
// `world.ring.max_deactivations`), read once by whoever builds a `RingParams`.
u32 max_activations_tunable() noexcept;
u32 max_deactivations_tunable() noexcept;

// One consumer's row. Every function pointer may be null. `activate` and `change_ring` return
// false to say the tile could not be taken (counted, and logged by the world); the tile stays
// active for every other consumer.
struct TileConsumer {
  const char* name = nullptr;
  void* context = nullptr;
  // The rings this consumer acts in, one bit per ring (bit 0 the innermost).
  u8 rings = 0x7Fu;
  bool (*activate)(void* context, const TileEvent& event) = nullptr;
  bool (*change_ring)(void* context, const TileEvent& event) = nullptr;
  void (*deactivate)(void* context, const TileEvent& event) = nullptr;
  // Once at the end of an update in which any of this consumer's tiles changed: where a consumer
  // hands its batch on (the placements consumer's instances, to the renderer, once a frame).
  void (*commit)(void* context) = nullptr;
};

// What one consumer's calls have cost, over the world's life (the E35 CPU table).
struct ConsumerStats {
  u64 activations = 0;
  u64 changes = 0;
  u64 deactivations = 0;
  u64 refusals = 0;
  u64 commits = 0;
  i64 activate_ns = 0;
  i64 change_ns = 0;
  i64 deactivate_ns = 0;
  i64 commit_ns = 0;
  i64 max_activate_ns = 0;
  i64 max_change_ns = 0;
  i64 max_deactivate_ns = 0;
  i64 max_commit_ns = 0;
};

// One update, as the world log (`engine.world.TileFrame`) records it.
struct UpdateStats {
  u64 tick = 0;
  u32 events = 0;
  u32 activated = 0;
  u32 changed = 0;
  u32 deactivated = 0;
  u32 refused = 0;
  u32 active = 0;
  u32 tracked = 0;
  u32 deferred_promotions = 0;
  u32 deferred_demotions = 0;
  u32 per_ring[k_max_rings] = {};
  i64 ring_ns = 0;      // the ring update alone
  i64 dispatch_ns = 0;  // every consumer's calls and commits
};

class World {
 public:
  World() = default;
  explicit World(const RingParams& params) { configure(params); }
  ENGINE_NON_COPYABLE(World);

  // False, with nothing changed, when `valid_ring_params` refuses them. Before the first update.
  bool configure(const RingParams& params, const char** error = nullptr);
  const RingParams& params() const noexcept { return ring_.params(); }

  // Returns the consumer's index. Registration order is activation order; before the first update.
  u16 add_consumer(const TileConsumer& consumer);
  u16 consumer_count() const noexcept { return static_cast<u16>(consumers_.size()); }
  const TileConsumer& consumer(u16 index) const noexcept { return consumers_[index]; }
  const ConsumerStats& consumer_stats(u16 index) const noexcept { return consumer_stats_[index]; }

  // One update: the ring over `observers` (an observer of weight zero or less observes nothing),
  // then every event to the consumers and a commit to each one whose tiles changed. Between ticks.
  const UpdateStats& update(const sim::ObserverSet& observers, u64 tick, bool unlimited = false);
  // Every active tile deactivated, in tile order, consumers in reverse; then the commits.
  const UpdateStats& clear(u64 tick);
  // The tiles a save kept, at their rings, with `observers` as the last update's
  // (`TileRing::restore`): one activation per tile, in tile order, to the consumers whose rings it
  // is in, as an update would make them, and the commits. Between ticks, on a world that holds no
  // tile; false, with nothing changed, when the ring refuses the tiles.
  bool restore(std::span<const TileCoord> tiles, std::span<const u8> rings,
               const sim::ObserverSet& observers, u64 tick, const char** error = nullptr);

  const TileRing& ring() const noexcept { return ring_; }
  const UpdateStats& last() const noexcept { return last_; }
  std::span<const TileEvent> last_events() const noexcept {
    return {events_.data(), events_.size()};
  }
  // The observers of the last update, as the consumers that need them (the store's promotion by
  // distance) read them during it.
  const sim::ObserverSet& observers() const noexcept { return observers_; }
  u64 tick() const noexcept { return tick_; }
  // Totals over the world's life.
  u64 updates() const noexcept { return updates_; }
  u64 total_events() const noexcept { return total_events_; }

 private:
  void dispatch(u64 tick);

  TileRing ring_;
  Vector<TileConsumer> consumers_;
  Vector<ConsumerStats> consumer_stats_;
  Vector<u8> touched_;  // per consumer, this update
  Vector<TileEvent> events_;
  sim::ObserverSet observers_;
  UpdateStats last_;
  u64 tick_ = 0;
  u64 updates_ = 0;
  u64 total_events_ = 0;
};

}  // namespace engine::world

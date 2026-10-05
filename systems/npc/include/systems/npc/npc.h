#pragma once

// npc capability (ADR-0027, ADR-0045; docs/plan/05-simulation.md §5.3–5.6,
// docs/plan/13-reference-consumer-games.md §13.2, docs/subsystems/npc.md).
//
// Scheduled residents: an `engine.npc.Resident` is a document record whose daily routine runs
// through the engine's timing wheel at the tier the observer set gives it. The routine is data (the
// tables in src/routine.cpp) and where a resident is in it is a closed-form function of the
// routine, a variation drawn once from the world seed and the record's id, and the game time
// (routine.h). That one fact is what everything here leans on:
//
// - **LOD2** is one `Timer(entity, kind, at)` per materialized resident: firing it moves the
//   resident to the next row — its state, the next row's place, the next timer — and nothing ticks
//   in between (05 §5.6: "scheduled transitions only").
// - **LOD1 and LOD0** add one thing: a resident on a trip is drawn on the straight segment between
//   the two places by elapsed fraction, every tick. Locomotion, perception and navigation are
//   Phase 6; this capability touches none of them.
// - **LOD3** is not an entity. It is the record in the document and the store, and the
//   `Summarizer` that brings a resident up to any time from the closed form without visiting the
//   transitions it skipped, which meets the five conditions of docs/subsystems/sim.md exactly.
//
// **Tiers change what runs, not what is true.** What is written back — `state`, `next_event` and
// the anchor `position` — is the closed form's at the time of the transition, whatever the tier;
// the transform a tier moves along a trip is not written back. So materializing a resident twice,
// in any order, at any tier, at any time, gives the same resident.
//
// ---- ADR-0027 decision 2, registration point by registration point ----------------------------
//
//   [x] capability graph  engine_capability_requires(npc ecs)
//   [x] schema types      schemas/npc.schema: Place, Resident, NpcPlace, NpcRoutine, NpcState and
//                         the two materialize declarations
//   [x] scheduler entry   two table systems (npc.lod at Lod, npc.motion at Systems), the wheel's
//                         summarizer table, the materialization hooks, and the wheel's event sink
//   [ ] render passes     none: a resident is a transform; whoever draws reads it
//   [x] derived data      the generator (`generate_residents`, `engine-content npcs`): a seeded
//                         document layer, derived and never committed
//   [ ] protocol methods  none: residents reach the world through materialization, and
//                         `session.run_headless` already runs it
//   [x] tunables          npc.lod.*, npc.motion.every_ticks, npc.fast_forward.budget
//   [x] LOD policy        LOD0–1 interpolate a trip, LOD2 transitions only, LOD3 the summary
//   [x] determinism       k_determinism = "hashed"; the LOD1 transform is "derived"
//   [x] zero cost unused  no linked code (ENGINE_WITH_NPC=OFF); no residents, no timers, and the
//                         two systems return at their first comparison
//   [x] docs, tests, size table, bench

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/hash_set.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <core/time/time.h>
#include <domain/doc/document.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/materialize.h>
#include <domain/sim/scheduler.h>
#include <domain/sim/tiers.h>
#include <domain/sim/timing_wheel.h>
#include <systems/npc/routine.h>

#include <schemas/npc.h>

namespace engine::npc {

inline constexpr const char* k_determinism = "hashed";
// The wheel payload's kind for a resident's next transition ("NPC1").
inline constexpr u32 k_timer_kind = 0x4E504331u;
// The wheel payload's kind for a resident that is not held but whose routine visits a live tile:
// its next transition, when the capability looks again whether the routine has brought it into one
// ("NPC2"; the schedule index, docs/subsystems/npc.md).
inline constexpr u32 k_watch_kind = 0x4E504332u;
// The tiers a resident is an entity at: LOD3 is a record (05 §5.6).
inline constexpr u8 k_entity_tiers = 0b0111u;
inline constexpr u32 k_no_place = 0xFFFF'FFFFu;

// The type names, as the document spells them.
inline constexpr const char* k_resident_type = "engine.npc.Resident";
inline constexpr const char* k_place_type = "engine.npc.Place";

// Run-time parameters, read once when a host installs the capability (foundation/tunables:
// npc.lod.near_m, npc.lod.mid_m, npc.lod.hysteresis, npc.lod.max_promotions,
// npc.lod.max_demotions, npc.lod.every_ticks, npc.motion.every_ticks, npc.fast_forward.budget).
struct NpcConfig {
  // Every resident's variation is drawn from this hashed with its id (ADR-0045).
  u64 world_seed = 1;
  // LOD0 within `near_m`, LOD1 within `mid_m`, LOD2 beyond — of an observer, over importance and
  // weight (05 §5.4). LOD3 is the tiles the ring does not simulate.
  f32 near_m = 16.0f;
  f32 mid_m = 64.0f;
  f32 hysteresis = 0.15f;
  u32 max_promotions = 64;
  u32 max_demotions = 256;
  // How often the tiers are reassigned and the trips of LOD0–1 residents moved, in ticks.
  u32 lod_every = 6;
  u32 motion_every = 1;
  // The events a fast-forward may execute before it summarizes instead (sim.md, "Fast-forward").
  u64 fast_forward_budget = 100'000;
};

// The config from the tunables, with `world_seed` left for the host.
NpcConfig config_from_tunables();

// Every `engine.npc.Place` of a document, by id: where a routine row's role sends a resident.
// Places in inactive tiles are included — a resident whose job is in a tile nobody simulates still
// goes there — which is why this is read from the document and not from what is materialized.
class PlaceIndex {
 public:
  void clear() noexcept;
  void add(const Id128& id, WorldPos position, PlaceRole role);
  // Every live Place record, through the document's composed index (never a scan of the layers).
  // A no-op when the document's revision is the one last read. Returns the places indexed.
  // `generation()` moves only when what the index holds changed — a place added, removed or moved —
  // which is when a resident's resolved places have to be looked up again.
  u32 refresh(const doc::Document& document);
  u64 generation() const noexcept { return generation_; }
  u32 find(const Id128& id) const noexcept;
  WorldPos position(u32 index) const noexcept { return positions_[index]; }
  PlaceRole role(u32 index) const noexcept { return roles_[index]; }
  u32 size() const noexcept { return positions_.size(); }

 private:
  HashMap<Id128, u32> by_id_;
  Vector<WorldPos> positions_;  // f64 world positions (ADR-0053)
  Vector<PlaceRole> roles_;
  Vector<Id128> ids_;
  u64 revision_ = ~u64{0};
  u64 generation_ = 0;
};

// What the capability did, cumulative.
struct NpcStats {
  u64 materialized = 0;  // materialize hook calls that made or refreshed a resident
  u64 dematerialized = 0;
  u64 transitions = 0;     // timers delivered and acted on
  u64 summaries = 0;       // summarizer calls
  u64 summary_visits = 0;  // residents a summary looked at
  u64 summarized = 0;      // residents a summary moved on
  u64 lod_runs = 0;
  u64 promotions = 0;
  u64 demotions = 0;
  u64 moved = 0;  // LOD0–1 transforms moved along a trip
};

// What the schedule index did, cumulative (docs/subsystems/npc.md, "The schedule index").
struct ScheduleStats {
  u64 builds = 0;       // the index built whole from the document
  u64 updates = 0;      // residents read again from the document's change feed
  u64 tile_passes = 0;  // tile passes it answered
  u64 visits = 0;       // residents those passes looked at: the ones whose routines visit the tile
  u64 placed = 0;       // residents a pass placed in its tile although their record is elsewhere
  u64 watches = 0;      // watch timers armed on residents that are not held
  u64 wakes = 0;        // watch timers delivered
  u64 arrivals = 0;     // residents reported arrived in a live tile, for the driver to bring in
  f64 build_ms = 0;     // the last whole build
};

// One resident of the document as the schedule index keeps it, held or not: what its closed form
// needs (the variation is drawn again from the routine, the world seed and the id when it is asked)
// and the tile of each of its four places. 72 bytes, once per resident of the document, pinned by
// the size table.
struct ScheduledResident {
  Id128 id;
  i64 clock_offset = 0;
  u64 tiles[4] = {};  // per PlaceRole: the tile of that place under the resident's grid (x, y)
  sim::TimerHandle watch;
  Routine routine = Routine::Idle;
  u8 tiled = 0;  // bit r: role r has a place, and `tiles[r]` is its tile
  bool held = false;
  bool live = false;  // false: a slot whose resident has left the document
};

// One resident as the capability holds it, for a test or a tool.
struct ResidentView {
  Id128 id;
  sim::EntityHandle entity;
  RoutinePoint point;
  WorldPos anchor;
  WorldPos drawn;
  u8 tier = 2;
  bool timer_live = false;
};

class NpcSystem {
 public:
  explicit NpcSystem(const NpcConfig& config = {});
  ~NpcSystem();
  ENGINE_NON_COPYABLE(NpcSystem);

  // Registers the world's components and this capability's (ADR-0028 seam 1), the two table
  // systems, the summarizer on the scheduler's wheel, and this capability as the wheel's event sink
  // — forwarding every event that is not a resident's to `next`, so a host that routes timers of
  // its own keeps them. Once per world, outside a tick. The hooks are separate (`hooks()`): they
  // must be added after the entity store's, which a host does once every capability is installed.
  void install(ecs::SimWorld& sim, sim::SimScheduler& scheduler, sim::EventSink next = {});
  sim::MaterializationHooks hooks() noexcept;

  // The observers tiers are assigned from, or null for none: residents then stay at the tier they
  // were materialized at. The set is the caller's and must outlive the ticks that read it.
  void set_observers(const sim::ObserverSet* observers) noexcept { observers_ = observers; }
  PlaceIndex& places() noexcept { return places_; }
  // The place index from `document`, and — when what it holds changed — every held resident's
  // places resolved again from its entity; then the schedule index (`refresh_schedule`). A host
  // calls it before it materializes residents; both indexes are read from the document and not from
  // what is materialized, because a resident's job may be in a tile nobody simulates.
  u32 refresh_places(const doc::Document& document);

  // ---- the schedule index (docs/subsystems/npc.md, "The schedule index") ------
  //
  // A resident's record is filed under the tile of the place its routine last wrote back, but its
  // routine moves it on whether it is held or not. The schedule index is, for every resident of the
  // document, what its closed form needs — routine, clock offset, places — and the tiles of its
  // four places; and per tile, the residents whose routines visit it. The driver consults it
  // through `tile_source()`:
  //
  // - a tile's pass is handed the residents whose closed form is in that tile now, wherever their
  //   records are, and the driver files a resident its routine has in a live tile under that tile;
  // - a resident that is not held but whose routine visits a live tile is *watched*: a timer on the
  //   wheel at its next transition (`k_watch_kind`), which looks again, and reports it as an
  //   arrival when its routine has brought it into a live tile — the driver brings it in between
  //   ticks
  //   (`sim::Materializer::materialize_arrivals`).
  //
  // So what is materialized is the residents whose routines have them in a live tile, plus — until
  // the write-back that moves their record — those whose record is in one; and it does not depend
  // on the order the tiles came in.

  // The driver's tile source; a host that streams tiles registers it once, beside `hooks()`.
  sim::TileSource tile_source() noexcept;
  // Built whole from `document` the first time, for another document, and whenever a place moved;
  // otherwise brought up to date from the document's change feed. Returns the residents indexed.
  u32 refresh_schedule(const doc::Document& document);
  // The residents whose routines visit `tile`, sorted.
  void visiting(doc::TileCoord tile, Vector<Id128>& out) const;
  // Residents in the index, and those of them watched (not held, a watch timer live).
  u32 scheduled() const noexcept { return schedule_by_id_.size(); }
  u32 watching() const noexcept;
  const ScheduleStats& schedule_stats() const noexcept { return schedule_stats_; }
  // What the schedule index has allocated: its entries, its two maps and the tiles' lists.
  u64 bytes_scheduled() const noexcept;
  const PlaceIndex& places() const noexcept { return places_; }
  const NpcConfig& config() const noexcept { return config_; }
  void set_world_seed(u64 seed) noexcept { config_.world_seed = seed; }

  // The routing sink the wheel delivers to; a test that drives the wheel directly passes it.
  sim::EventSink sink() noexcept;
  // The summary itself (what the registered summarizer calls): every held resident whose current
  // row ends at or before `to` is set to where its routine has it at `to`, its timer re-armed past
  // `to`. Returns the residents visited.
  u64 summarize(GameTime from, GameTime to);
  // A fast-forward of the scheduler's wheel to `to` under `budget` events (0: the tunable's):
  // executed when the gap holds no more, summarized when it does. The wheel only — the scheduler's
  // clock is a separate thing and a jump of it is not this capability's (docs/subsystems/npc.md).
  sim::FastForwardResult fast_forward(GameTime to, u64 budget = 0);

  u32 residents() const noexcept { return ids_.size(); }
  bool find(const Id128& id, ResidentView& out) const;
  // Residents per tier, 0..3 (3 is always 0: a resident at LOD3 is not held).
  void tier_counts(u32 (&out)[4]) const noexcept;
  const NpcStats& stats() const noexcept { return stats_; }
  // Bytes this capability holds per resident, and in all (its arrays and index, not flecs').
  static constexpr u32 bytes_per_resident() noexcept {
    return static_cast<u32>(sizeof(Id128) + sizeof(sim::EntityHandle) + sizeof(Variation) +
                            sizeof(i64) + 4 * sizeof(u32) + 2 * sizeof(WorldPos) +
                            sizeof(RoutinePoint) + sizeof(sim::TimerHandle) + sizeof(f32) +
                            sizeof(u8));
  }
  // Every array at its capacity, and the handle index's buckets: what the capability has allocated.
  u64 bytes_held() const noexcept;

 private:
  static sim::EntityHandle materialize_hook(void* context, const sim::EntityRecord& record,
                                            u8 tier);
  static void promote_hook(void* context, sim::EntityHandle entity, u8 from, u8 to);
  static void dematerialize_hook(void* context, sim::EntityHandle entity);
  static void deliver(void* context, const sim::TimerEvent& event);
  static void summarize_fn(void* context, const sim::SummarizeInterval& interval);
  static void lod_tick(sim::SystemContext& context, sim::Batch batch);
  static void motion_tick(sim::SystemContext& context, sim::Batch batch);

  sim::EntityHandle materialize(const sim::EntityRecord& record, u8 tier);
  void set_tier(sim::EntityHandle entity, u8 to);
  void dematerialize(sim::EntityHandle entity);
  // Puts resident `i` at `point` and arms its timer for the row's end.
  void move_to(u32 i, const RoutinePoint& point, i64 t_us);
  WorldPos place_position(u32 i, PlaceRole role) const noexcept;
  void resolve_places(u32 i, const NpcRoutine& routine) noexcept;
  // The closed form for resident `i` at game time `t`, in game time.
  RoutinePoint point_at(u32 i, i64 t_us) const noexcept;
  // Where a resident is: at its place, or on a trip's segment by elapsed fraction — whatever its
  // tier. `drawn_at` is that at LOD0–1 and the destination at LOD2.
  WorldPos position_at(u32 i, i64 t_us) const noexcept;
  WorldPos drawn_at(u32 i, i64 t_us) const noexcept;
  void write_components(u32 i);
  void remove(u32 i);

  // ---- the schedule index ----
  static bool where_fn(void* context, const doc::Document& document, const Id128& id,
                       doc::TileCoord& out);
  static void tile_in_fn(void* context, const doc::Document& document, doc::TileCoord tile,
                         Vector<Id128>& out);
  static void tile_out_fn(void* context, doc::TileCoord tile);
  static void arrivals_fn(void* context, Vector<Id128>& out);

  using Scheduled = ScheduledResident;
  // Reads resident `id` from the document into `out`; false when it is not a resident the index
  // keeps (not live, not a resident, or on no grid a routine moves it across).
  bool read_scheduled(const doc::Document& document, const Id128& id, Scheduled& out);
  void build_schedule(const doc::Document& document);
  void update_scheduled(const doc::Document& document, const Id128& id);
  void link_tiles(u32 slot);
  void unlink_tiles(u32 slot);
  // The tile of place `place` under a grid of `tile_size`, as the partition would file a resident
  // standing there — the anchor the write-back writes is the place's position. False when the
  // position has no tile (out of i32 range).
  bool place_tile(u32 place, f64 tile_size, u64& out);
  RoutinePoint scheduled_point(const Scheduled& s, i64 t_us) const noexcept;
  // The tile the resident's routine has it in at `point`: its place's; false for a role it has no
  // place for.
  static bool anchor_tile(const Scheduled& s, const RoutinePoint& point, u64& out) noexcept;
  bool visits_live(const Scheduled& s) const noexcept;
  // A timer at the end of the row `t` falls in, unless one is live already.
  void watch(u32 slot, i64 t_us);
  void unwatch(u32 slot);
  // A resident that is not held any more, or not held and looked at again at `t`: an arrival when
  // its routine has it in a live tile, watched while its routine visits one.
  void look_again(u32 slot, i64 t_us);
  void deliver_watch(const sim::TimerEvent& event);

  NpcConfig config_;
  ecs::SimWorld* sim_ = nullptr;
  sim::SimScheduler* scheduler_ = nullptr;
  const sim::ObserverSet* observers_ = nullptr;
  sim::EventSink next_;
  PlaceIndex places_;
  const void* resident_mapping_ = nullptr;
  sim::TierAssignment tiers_;
  sim::TierParams tier_params_;
  Vector<sim::TierChange> changes_;
  Vector<Vec3> scored_;  // scratch: the tier pass's positions

  // One entry per held resident, dense: swap-removed, so a resident's index is not stable and
  // nothing outside keeps one. The wheel's payload names the entity handle instead.
  Vector<Id128> ids_;
  Vector<sim::EntityHandle> handles_;
  Vector<Variation> variations_;
  Vector<i64> offsets_;        // the routine's clock at game time 0 (Resident.clock_offset)
  Vector<u32> places_of_;      // four per resident: home, job, service, leisure (PlaceRole order)
  Vector<WorldPos> fallback_;  // where the document had it, for a role with no known place
  Vector<RoutinePoint> points_;
  Vector<sim::TimerHandle> timers_;
  Vector<WorldPos> drawn_;  // the transform
  Vector<f32> importance_;
  Vector<u8> tier_;
  HashMap<u64, u32> by_handle_;
  u64 places_generation_ = 0;
  NpcStats stats_;

  // The schedule index: every resident of the document by slot (a slot is stable until the next
  // whole build, which is what a watch timer's payload names), by id, and per tile the slots whose
  // routines visit it, in id order.
  Vector<Scheduled> schedule_;
  HashMap<Id128, u32> schedule_by_id_;
  HashMap<u64, Vector<u32>> schedule_by_tile_;
  // Per place, its tile under `place_tiles_size_` (bit 0 of `place_tiled_`: it has one) and the
  // place index generation it was computed at; recomputed for another grid.
  Vector<u64> place_tiles_;
  Vector<u8> place_tiled_;
  f64 place_tiles_size_ = 0.0;
  u64 place_tiles_generation_ = ~u64{0};
  // The tiles the driver has passed and not let go of, as the tile source was told.
  HashSet<u64> live_tiles_;
  // Residents whose routines brought them into a live tile, since the driver last asked.
  Vector<Id128> arrivals_;
  const doc::Document* schedule_document_ = nullptr;
  u64 schedule_revision_ = 0;
  u64 schedule_places_generation_ = ~u64{0};
  bool schedule_built_ = false;
  ScheduleStats schedule_stats_;
};

}  // namespace engine::npc

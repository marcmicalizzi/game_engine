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
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/math/math.h>
#include <core/time/time.h>
#include <domain/doc/document.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/scheduler.h>
#include <domain/sim/tiers.h>
#include <domain/sim/timing_wheel.h>
#include <systems/npc/routine.h>

#include <schemas/npc.h>

namespace engine::npc {

inline constexpr const char* k_determinism = "hashed";
// The wheel payload's kind for a resident's next transition ("NPC1").
inline constexpr u32 k_timer_kind = 0x4E504331u;
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
  void add(const Id128& id, Vec3 position, PlaceRole role);
  // Every live Place record, through the document's composed index (never a scan of the layers).
  // A no-op when the document's revision is the one last read. Returns the places indexed.
  // `generation()` moves only when what the index holds changed — a place added, removed or moved —
  // which is when a resident's resolved places have to be looked up again.
  u32 refresh(const doc::Document& document);
  u64 generation() const noexcept { return generation_; }
  u32 find(const Id128& id) const noexcept;
  Vec3 position(u32 index) const noexcept { return positions_[index]; }
  PlaceRole role(u32 index) const noexcept { return roles_[index]; }
  u32 size() const noexcept { return positions_.size(); }

 private:
  HashMap<Id128, u32> by_id_;
  Vector<Vec3> positions_;
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

// One resident as the capability holds it, for a test or a tool.
struct ResidentView {
  Id128 id;
  sim::EntityHandle entity;
  RoutinePoint point;
  Vec3 anchor;
  Vec3 drawn;
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
  // places resolved again from its entity. A host calls it before it materializes residents; the
  // index is read from the document and not from what is materialized, because a resident's job may
  // be in a tile nobody simulates.
  u32 refresh_places(const doc::Document& document);
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
                            sizeof(i64) + 4 * sizeof(u32) + 2 * sizeof(Vec3) +
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
  Vec3 place_position(u32 i, PlaceRole role) const noexcept;
  void resolve_places(u32 i, const NpcRoutine& routine) noexcept;
  // The closed form for resident `i` at game time `t`, in game time.
  RoutinePoint point_at(u32 i, i64 t_us) const noexcept;
  // Where a resident is: at its place, or on a trip's segment by elapsed fraction — whatever its
  // tier. `drawn_at` is that at LOD0–1 and the destination at LOD2.
  Vec3 position_at(u32 i, i64 t_us) const noexcept;
  Vec3 drawn_at(u32 i, i64 t_us) const noexcept;
  void write_components(u32 i);
  void remove(u32 i);

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
  Vector<i64> offsets_;    // the routine's clock at game time 0 (Resident.clock_offset)
  Vector<u32> places_of_;  // four per resident: home, job, service, leisure (PlaceRole order)
  Vector<Vec3> fallback_;  // where the document had it, for a role with no known place
  Vector<RoutinePoint> points_;
  Vector<sim::TimerHandle> timers_;
  Vector<Vec3> drawn_;  // the transform
  Vector<f32> importance_;
  Vector<u8> tier_;
  HashMap<u64, u32> by_handle_;
  u64 places_generation_ = 0;
  NpcStats stats_;
};

}  // namespace engine::npc

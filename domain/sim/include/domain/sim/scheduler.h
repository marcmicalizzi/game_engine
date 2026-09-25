#pragma once

// The simulation tick scheduler (docs/plan/05-simulation.md §5.2, §5.5,
// docs/plan/03-data-model.md §3.4, ADR-0027).
//
// Three things live here, because they are the same mechanism seen from three sides:
//
//   1. **The tick.** Eight phases in the plan's order, a fixed step from `core/time`, and a
//      table of systems with declared read/write component sets. Within a phase, two systems
//      that touch a component in a way that conflicts keep declaration order; everything else
//      runs in parallel on `core/jobs`. The schedule is computed once from the registration and
//      is a pure function of it, so it is the same on one worker and on sixteen.
//   2. **The materialization contract** ([03
//   §3.4](../../../docs/plan/03-data-model.md#34-the-runtime-world)):
//      `materialize`, `promote`, `demote`, `dematerialize` as a hooks table, driven by the tier
//      changes `domain/sim/tiers.h` produces.
//   3. **Tile reconciliation** ([05
//   §5.5](../../../docs/plan/05-simulation.md#55-reconciliation-when-a-tile-activates)),
//      which is the two above plus the timing wheel's summarizers, run in the plan's five steps.
//
// **This module does not depend on `domain/ecs`.** `TickPhase` is declared here and
// `ecs::TickPhase` is an alias of it
// ([ADR-0028](../../../docs/adr/0028-ecs-and-persistent-store.md) seam 2), the dependency going ecs
// → sim and never back. That is what lets this scheduler own the tick with flecs' systems run
// inside its phases by a `TickExecutor` the entity store installs
// ([ADR-0038](../../../docs/adr/0038-the-scheduler-owns-the-tick.md), proposed): the executor is
// four function pointers, and nothing here knows what is behind them.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/jobs/job_system.h>
#include <core/math/math.h>
#include <core/time/time.h>
#include <domain/sim/tiers.h>
#include <domain/sim/timing_wheel.h>

#include <span>

namespace engine::sim {

// docs/plan/05-simulation.md §5.2, in order. Same enumerators as `ecs::TickPhase` and
// ADR-0027's `SystemDesc::phase`.
enum class TickPhase : u8 {
  Input,
  EventsIn,
  Lod,
  Systems,
  Physics,
  PostPhysics,
  EventsOut,
  Persist,
  Count,
};

inline constexpr u32 k_phase_count = static_cast<u32>(TickPhase::Count);

const char* phase_name(TickPhase phase) noexcept;

// 256 component types. A fixed mask rather than a growable set because the conflict test runs
// once per pair of systems in a phase at build time and four `and`s is the whole of it; the cap
// is stated, checked, and cheap to raise (ADR-0017: a limit that is documented and asserted is
// not a hidden one).
inline constexpr u32 k_component_mask_words = 4;
inline constexpr u32 k_max_components = k_component_mask_words * 64;

struct ComponentMask {
  u64 words[k_component_mask_words] = {};

  void set(u32 component) noexcept;
  bool test(u32 component) const noexcept;
  bool any() const noexcept;
  bool intersects(const ComponentMask& other) const noexcept;
  bool operator==(const ComponentMask&) const noexcept = default;
};

// ---- named resources ---------------------------------------------------------------------------
//
// A component mask names components, and [03
// §3.4](../../../docs/plan/03-data-model.md#34-the-runtime-world) tells a hot system to own its
// data instead: a pose pool, a physics island set, a GPU upload buffer. Two systems ordered by one
// of those had nothing to declare, so the dependency between them was invisible to the schedule —
// the gap `systems/animation` found first and worked around with a phase boundary.
//
// A **resource** is that data, named. A capability registers a name once (`resource_id`), lists the
// id in `SystemDesc::reads_resources`/`writes_resources`, and the wave layering treats a resource
// conflict exactly like a component conflict: a writer conflicts with every other toucher, two
// readers do not. Nothing else about a resource is the scheduler's business — it never sees the
// data, only the id — which is what keeps this a declaration and not an ownership mechanism.
//
// **64, one word, for the same reason components get 256**: the conflict test runs once per pair of
// systems in a phase when the schedule is built, and one `and` is the whole of it. Resources are
// per *capability* and not per type, so 64 is a different order of magnitude from the component
// count, and the cap is stated, asserted and cheap to raise
// ([ADR-0017](../../../docs/adr/0017-no-hidden-limits.md)).
inline constexpr u32 k_resource_mask_words = 1;
inline constexpr u32 k_max_resources = k_resource_mask_words * 64;
inline constexpr u32 k_invalid_resource = 0xFFFF'FFFFu;

struct ResourceMask {
  u64 words[k_resource_mask_words] = {};

  void set(u32 resource) noexcept;
  bool test(u32 resource) const noexcept;
  bool any() const noexcept;
  bool intersects(const ResourceMask& other) const noexcept;
  bool operator==(const ResourceMask&) const noexcept = default;
};

// Names to ids, in registration order, for the process.
//
// **Why one registry for the process and not one per world.** A component's index is a property of
// a world, because the component type is registered into that world; a resource is a property of a
// *capability* — "the animation pose pool" means the same thing in every world in the process, and
// a capability that installs into two worlds declares the same masks in both. A per-world registry
// would need a world to reach, and `domain/sim` has no world: the scheduler must stay free of
// `domain/ecs` ([ADR-0028](../../../docs/adr/0028-ecs-and-persistent-store.md) decision 7).
//
// Registration is cold — install time, from one thread — and is not synchronized, exactly like
// `ecs::ComponentRegistry`. `name` must outlive the registry, so it is a string literal, the same
// convention `SystemDesc::name` already has.
class ResourceRegistry {
 public:
  // The id for this name, registering it if it is new. `k_invalid_resource` past the cap.
  u32 id(const char* name);
  // The id for this name, or `k_invalid_resource`; registers nothing.
  u32 find(const char* name) const noexcept;
  // The name an id was registered under, or nullptr: what a diagnostic prints, and what an assert
  // uses to tell a real id from a stale one.
  const char* name_of(u32 resource) const noexcept;
  u32 size() const noexcept { return names_.size(); }
  // Names refused because the cap was reached. Non-zero means the schedule is under-declared.
  u32 overflowed() const noexcept { return overflowed_; }

  static ResourceRegistry& global() noexcept;

 private:
  Vector<const char*> names_;
  u32 overflowed_ = 0;
};

// `ResourceRegistry::global().id(name)`, which is how a capability spells it.
u32 resource_id(const char* name);

// The mask naming these resources, registering any that are new. One call per system at install
// time: `desc.writes_resources = sim::resource_mask("animation.pose_pool");`
template <class First, class... Rest>
ResourceMask resource_mask(First first, Rest... rest) {
  const char* const names[] = {first, rest...};
  ResourceMask mask;
  for (const char* name : names) {
    const u32 id = resource_id(name);
    if (id != k_invalid_resource) mask.set(id);
  }
  return mask;
}

// ADR-0010's two stances, declared per system and recorded on the docs page.
enum class Determinism : u8 { Hashed, Derived };

// The slice of a system's own work one invocation covers. The scheduler does not know what the
// indices mean; a system that declares `batches = 1` gets [0, 1).
struct Batch {
  u32 begin = 0;
  u32 end = 0;
};

struct SystemContext {
  SimTick tick;
  GameTime time;
  GameTime step;
  void* context = nullptr;
  TickPhase phase = TickPhase::Systems;
  u16 system = 0;
  u16 wave = 0;
};

// ADR-0027's registration table, field for field, plus `context` and `batches`: the ADR's
// sketch assumes a system reaches its module's globals, and a testable one needs to be handed
// its state instead.
struct SystemDesc {
  const char* name = nullptr;
  TickPhase phase = TickPhase::Systems;
  ComponentMask reads;
  ComponentMask writes;
  // Data outside the ECS this system touches, by registered name. Read-write against the same
  // resource conflicts; two readers do not. See "named resources" above.
  ResourceMask reads_resources;
  ResourceMask writes_resources;
  u8 tiers = 0x0Fu;  // LodMask: tiers this system runs at
  Determinism determinism = Determinism::Hashed;
  u16 batches = 1;
  void* context = nullptr;
  void (*begin_tick)(SystemContext&) = nullptr;
  void (*tick)(SystemContext&, Batch) = nullptr;
  void (*end_tick)(SystemContext&) = nullptr;
};

// One system's place in its phase's schedule. Systems in the same wave provably do not conflict
// and run in parallel; wave order is declaration order for everything that does.
struct ScheduleEntry {
  u16 system = 0;
  u16 wave = 0;
};

// ---- the two names an entity has, and which hook gets which -----------------------------------
//
// [ADR-0028](../../../docs/adr/0028-ecs-and-persistent-store.md) seam 3 settles this for the engine
// and the hooks below did not speak it: persistent identity is `Id128`, and **a runtime entity id
// must not outlive a tick's working set**. Both halves of the materialization contract have to
// appear here, because the contract is exactly where the two meet, and a `u64` that was sometimes
// one and sometimes the other meant two capabilities could choose differently and not interoperate.
//
// `EntityHandle` is the runtime half: opaque to this module, meaningful only to whoever produced
// it, and **valid only inside the call that carries it**. It is a struct rather than a `u64` so
// that the two cannot be swapped at a call site by a cast that compiles, and so that
// `static_cast<flecs::entity_t>` has exactly one legal home — `domain/ecs`, which is the only
// module allowed to know what is inside one (`ecs::handle_of`, `ecs::entity_of`,
// `ecs::handle_for`).
struct EntityHandle {
  u64 value = 0;

  constexpr bool is_null() const noexcept { return value == 0; }
  bool operator==(const EntityHandle&) const noexcept = default;
};

// Where a record came from when the authoring document is its source: the mapping row and the
// record's property values (domain/sim/materialize.h).
struct RecordSource;

// The persistent record a tile's store hands back for one entity ([03
// §3.5](../../../docs/plan/03-data-model.md#35-persistent-world-state)), or the document record the
// materialization driver hands the hooks. Either way it came from outside the world, so the only
// name it can carry is the one that survived the trip: `Id128`.
struct EntityRecord {
  Id128 entity;
  u64 seed = 0;
  Vec3 position;
  f32 importance = 1.0f;
  u32 kind = 0;
  u8 tier = 3;
  // Set by the driver for a document record: which components it becomes and with what values.
  // Null for a record from the store, whose projections are opaque bytes today; a hook then
  // resolves an entity that already exists and creates none.
  const RecordSource* source = nullptr;
};

// The materialization contract of [03
// §3.4](../../../docs/plan/03-data-model.md#34-the-runtime-world), as data rather than as a base
// class, for the same reason `SystemDesc` is: the scheduler holds a table it walks in registration
// order, and a capability that implements none of the four hooks contributes no row and costs
// nothing.
//
// **Which hook gets which name, and why it is not a matter of taste.**
//
// `materialize` is handed a record that came out of the store, so it gets the record's `Id128` —
// there is no runtime handle yet, which is the whole point of the call. It **returns** the handle
// the record now has: created by a hook that brings the entity into being, or resolved by a hook
// that found the one that already existed. A hook that gave the record no runtime existence
// returns a null handle, the first non-null answer in registration order wins, and a record no
// hook answered for is skipped by everything downstream rather than being promoted into nothing.
//
// `promote`, `demote` and `dematerialize` act on something that is already live *this tick*: they
// are driven by `TierAssignment`, whose input is an array the caller is already holding, and whose
// output indexes into it. So they get an `EntityHandle`, and it is valid for the call and no
// longer. Handing them an `Id128` instead would put a hash lookup per entity per tier change in
// front of information the caller already had, for a name nothing on this path needs.
struct MaterializationHooks {
  const char* name = nullptr;
  void* context = nullptr;
  u8 tiers = 0x0Fu;  // tiers this system materializes at; other tiers skip the hook entirely
  EntityHandle (*materialize)(void* context, const EntityRecord& record, u8 tier) = nullptr;
  void (*promote)(void* context, EntityHandle entity, u8 from, u8 to) = nullptr;
  void (*demote)(void* context, EntityHandle entity, u8 from, u8 to) = nullptr;
  void (*dematerialize)(void* context, EntityHandle entity) = nullptr;
};

struct TileState {
  GameTime last_active;
  u64 seed = 0;
};

// What reconciliation needs from `foundation/store`, as function pointers so that `domain/sim`
// does not depend on it (and so that a test can hand it a fake).
struct TileStore {
  void* context = nullptr;
  bool (*tile_state)(void* context, u64 tile, TileState& out) = nullptr;
  void (*load_records)(void* context, u64 tile, Vector<EntityRecord>& out) = nullptr;
};

struct ReconcileParams {
  u64 tile = 0;
  GameTime now;
  u8 materialize_tier = 2;  // plan 05 §5.5 step 4
  u8 summarize_tier = 3;    // step 3: the LOD3 systems
};

struct ReconcileResult {
  GameTime gap;
  u32 records = 0;
  u32 summarized = 0;
  u32 materialized = 0;
  u32 promoted = 0;
  bool known = false;  // the store had a state for this tile; false means it is new
};

// An executor for systems the table does not hold — flecs' systems, today
// ([ADR-0038](../../../docs/adr/0038-the-scheduler-owns-the-tick.md), proposed). The scheduler
// keeps the clock, the fixed step, the phase order, the timing wheel, the hooks and its own table,
// and calls the executor at three points of every tick:
//
//   begin_tick   once, after the tick and the game clock have advanced and before `Input`
//   run_phase    once per phase, after the phase's begin_tick hooks and before the table's waves
//   end_tick     once, after `Persist` and the persist hook
//
// So a phase runs: (at `EventsIn`, the wheel), begin_tick hooks, the executor's systems for the
// phase, the table's waves, end_tick hooks, (at `Persist`, the persist hook). The executor's
// systems go first because the table today holds engine systems that consume what capabilities'
// systems wrote in the same phase — the write-back flush at `Persist` is the first — and none that
// produces for them.
struct TickExecutor {
  void* context = nullptr;
  void (*begin_tick)(void* context, SimTick tick, GameTime time, GameTime step) = nullptr;
  void (*run_phase)(void* context, TickPhase phase, SimTick tick, GameTime time,
                    GameTime step) = nullptr;
  void (*end_tick)(void* context, SimTick tick, GameTime time) = nullptr;
};

struct SimSchedulerConfig {
  u32 hz = 60;
  u32 max_steps_per_advance = 8;
  f64 game_seconds_per_real_second = 1.0;
  // Where the clock starts: the game time the wheel's origin and the game clock begin at, and the
  // tick the scheduler's own counter begins at. A world loaded from a save starts at the tick and
  // game time it was saved at (docs/subsystems/world.md, "Save and load"), so the write-back's
  // cadence (`tick % writeback_every`), the events' ticks and every timer due after the save land
  // on the same ticks they would have in the run that never stopped.
  GameTime epoch;
  SimTick start_tick;
  jobs::JobSystem* job_system = nullptr;
};

class SimScheduler {
 public:
  explicit SimScheduler(const SimSchedulerConfig& config = {});
  ENGINE_NON_COPYABLE(SimScheduler);

  // --- registration --------------------------------------------------------------------------

  // Returns the system's index, which is also the `system` id a periodic or a summarizer uses:
  // one number identifies a system to the tick, to the wheel and to the fast-forward path.
  u16 add_system(const SystemDesc& desc);
  u16 system_count() const noexcept { return static_cast<u16>(systems_.size()); }
  const SystemDesc& system(u16 index) const noexcept { return systems_[index]; }

  u16 add_hooks(const MaterializationHooks& hooks);
  u16 hooks_count() const noexcept { return static_cast<u16>(hooks_.size()); }

  // Everything the tick will run, ordered by (wave, declaration index). Computed on first use
  // and after any registration; calling it explicitly is only useful for inspecting it.
  std::span<const ScheduleEntry> schedule(TickPhase phase) const;
  u16 wave_count(TickPhase phase) const;
  u16 wave_of(u16 index) const;
  // A 64-bit summary of the whole schedule: name, phase, wave, batches, per system. Two
  // registrations that hash the same produce the same tick order.
  u64 schedule_hash() const;

  // --- the tick ------------------------------------------------------------------------------

  // Events due in this tick are delivered during `TickPhase::EventsIn`, through this sink.
  void set_event_sink(const EventSink& sink) noexcept { event_sink_ = sink; }
  // Called once at the end of `TickPhase::Persist`, which is where a store flush belongs.
  void set_persist_hook(void (*hook)(void* hook_context, SimTick at_tick, GameTime at_time),
                        void* hook_context) noexcept;
  // The executor for systems outside the table (see `TickExecutor`). One at a time; a default one
  // detaches.
  void set_executor(const TickExecutor& executor) noexcept { executor_ = executor; }
  const TickExecutor& executor() const noexcept { return executor_; }

  // One fixed step: advances the tick and game time, then runs the eight phases in order.
  // Reads no clock, which is what lets a replay, a headless fast-forward and a live session
  // take the same path ([05
  // §5.10](../../../docs/plan/05-simulation.md#510-determinism-and-replay)).
  void step();
  // Feeds real nanoseconds to the fixed-step accumulator and runs whole steps. Returns how many.
  u32 advance(i64 real_ns);

  // The scheduler's own tick counter, not the accumulator's: `step()` advances the world
  // without touching `FixedStepClock`, so a replay that never feeds real time still counts.
  SimTick tick() const noexcept { return tick_; }
  GameTime game_time() const noexcept { return game_clock_.now(); }
  GameTime step_size() const noexcept { return GameTime{game_clock_.us_per_tick()}; }
  const FixedStepClock& clock() const noexcept { return clock_; }
  const GameClock& game_clock() const noexcept { return game_clock_; }
  TimingWheel& wheel() noexcept { return wheel_; }
  const TimingWheel& wheel() const noexcept { return wheel_; }
  TierAssignment& tiers() noexcept { return tiers_; }

  // --- the materialization contract ----------------------------------------------------------

  // Drives the hooks from the tier changes. `entities[change.index]` is the runtime handle, so the
  // tier code stays free of identity and this stays free of positions. A null handle is a row
  // nothing materialized and is skipped: there is nothing for a hook to act on.
  void apply_tier_changes(std::span<const TierChange> changes,
                          std::span<const EntityHandle> entities);
  // Returns the handle the record now has, or a null one when no hook gave it a runtime existence.
  // Hooks run in registration order, so the hook that brings the entity into being registers first.
  EntityHandle materialize(const EntityRecord& record, u8 tier);
  // Hooks run in **reverse** registration order: the mirror of construction, so every hook that
  // attached state to an entity lets go of it before the hook that created the entity destroys it.
  // Walked forwards, the entity store's hook would delete the entity first and every later hook
  // would be handed a handle that names nothing — and leak whatever it held for it.
  void dematerialize(std::span<const EntityHandle> entities);

  // --- tile reconciliation -------------------------------------------------------------------

  // The five steps of [05
  // §5.5](../../../docs/plan/05-simulation.md#55-reconciliation-when-a-tile-activates): load the
  // tile's projections, compute the elapsed game time, summarize the LOD3 systems over the gap from
  // the tile's stored seed, materialize at LOD2, then promote by observer distance. Step 5 of the
  // plan (derived visual state) belongs to the renderer and is not done here.
  ReconcileResult reconcile_tile(const ReconcileParams& params, const TileStore& store,
                                 const ObserverSet& observers, const TierParams& tier_params);

 private:
  struct Invocation {
    SystemContext context;
    const SystemDesc* desc = nullptr;
    Batch batch;
  };

  static void run_invocation(void* data);
  void rebuild_schedule() const;
  void run_phase(TickPhase phase);
  void run_wave(TickPhase phase, u16 wave, u32 first, u32 count);

  Vector<SystemDesc> systems_;
  Vector<MaterializationHooks> hooks_;

  // Mutable because the schedule is a cache of a pure function of `systems_`, and asking for it
  // from a const inspector must not be a different answer than asking for it from the tick.
  mutable Vector<ScheduleEntry> schedule_;
  mutable u32 phase_first_[k_phase_count] = {};
  mutable u32 phase_count_[k_phase_count] = {};
  mutable u16 phase_waves_[k_phase_count] = {};
  mutable Vector<u16> wave_of_;
  mutable bool schedule_dirty_ = true;

  mutable Vector<Invocation> invocations_;
  mutable Vector<jobs::Job> jobs_buffer_;
  Vector<EntityRecord> records_;
  Vector<Vec3> positions_;
  Vector<f32> importance_;
  Vector<u8> tiers_buffer_;
  Vector<EntityHandle> handles_;
  Vector<TierChange> changes_;

  SimTick tick_;
  FixedStepClock clock_;
  GameClock game_clock_;
  TimingWheel wheel_;
  TierAssignment tiers_;
  EventSink event_sink_;
  void (*persist_hook_)(void* hook_context, SimTick at_tick, GameTime at_time) = nullptr;
  void* persist_context_ = nullptr;
  TickExecutor executor_;
  jobs::JobSystem* jobs_ = nullptr;
};

}  // namespace engine::sim

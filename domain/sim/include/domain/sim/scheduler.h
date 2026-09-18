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
// **This module does not depend on `domain/ecs`.** `TickPhase` is declared here, with the same
// enumerators and the same order as `ecs::TickPhase`, because
// [ADR-0028](../../../docs/adr/0028-ecs-and-persistent-store.md) §7 deliberately leaves open
// whether flecs' pipeline stays the tick scheduler. The two should become one type —
// `ecs::TickPhase` aliasing this one — in the change that decides it; doing it the other way round
// would make the scheduler depend on flecs.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
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

// The persistent record a tile's store hands back for one entity ([03
// §3.5](../../../docs/plan/03-data-model.md#35-persistent-world-state)).
struct EntityRecord {
  u64 entity = 0;
  u64 seed = 0;
  Vec3 position;
  f32 importance = 1.0f;
  u32 kind = 0;
  u8 tier = 3;
};

// The materialization contract of [03
// §3.4](../../../docs/plan/03-data-model.md#34-the-runtime-world), as data rather than as a base
// class, for the same reason `SystemDesc` is: the scheduler holds a table it walks in registration
// order, and a capability that implements none of the four hooks contributes no row and costs
// nothing.
struct MaterializationHooks {
  const char* name = nullptr;
  void* context = nullptr;
  u8 tiers = 0x0Fu;  // tiers this system materializes at; other tiers skip the hook entirely
  void (*materialize)(void* context, const EntityRecord& record, u8 tier) = nullptr;
  void (*promote)(void* context, u64 entity, u8 from, u8 to) = nullptr;
  void (*demote)(void* context, u64 entity, u8 from, u8 to) = nullptr;
  void (*dematerialize)(void* context, u64 entity) = nullptr;
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

struct SimSchedulerConfig {
  u32 hz = 60;
  u32 max_steps_per_advance = 8;
  f64 game_seconds_per_real_second = 1.0;
  GameTime epoch;
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

  // Drives the hooks from the tier changes. `entities[change.index]` is the entity id, so the
  // tier code stays free of identity and this stays free of positions.
  void apply_tier_changes(std::span<const TierChange> changes, std::span<const u64> entities);
  void materialize(const EntityRecord& record, u8 tier);
  void dematerialize(std::span<const u64> entities);

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
  Vector<u64> entities_;
  Vector<TierChange> changes_;

  SimTick tick_;
  FixedStepClock clock_;
  GameClock game_clock_;
  TimingWheel wheel_;
  TierAssignment tiers_;
  EventSink event_sink_;
  void (*persist_hook_)(void* hook_context, SimTick at_tick, GameTime at_time) = nullptr;
  void* persist_context_ = nullptr;
  jobs::JobSystem* jobs_ = nullptr;
};

}  // namespace engine::sim

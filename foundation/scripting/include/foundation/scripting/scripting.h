#pragma once

// scripting capability (ADR-0027; docs/plan/02-architecture.md §2.8, docs/plan/08-toolchain.md
// §8.2, docs/plan/11-performance-principles.md §11.10; experiment E7).
//
// The Luau host for content logic: quest conditions, dialogue logic, NPC routines — the small,
// numerous, frequently edited functions the plan does not want in C++ and cannot express as pure
// data. ScriptContext (script_context.h) is the VM, its sandbox and budgets, the schema bindings
// and hot reload; emit_type_definitions() (type_definitions.h) is the typed API; this header is the
// capability's tick driver, ScriptingSystem, which calls `on_tick(self, dt)` for a set of scripted
// instances the way the scheduler will once it exists. What it does not do: mutate the document
// (a script returns data and the host issues commands), keep coroutines alive across ticks, or
// persist anything a script keeps in its own globals.
//
// The module is additive: ENGINE_WITH_SCRIPTING=OFF (or ENGINE_MINIMAL=ON) removes it, Luau and
// its tests from the build, and nothing else in the tree includes it. The checklist is ADR-0027
// decision 2; docs/subsystems/scripting.md says why each line is what it is.
//
//   [x] schema types      none of its own: it reads every registered type through the descriptors
//   [ ] scheduler entry   the SystemDesc fields below, registered when the scheduler lands
//   [x] render passes     none: scripts return data, they do not draw
//   [x] derived data      none yet: scripts compile at load (a bytecode cache is a later step)
//   [ ] protocol methods  none yet: `script.check` and `script.types` are the obvious first two
//   [x] tunables          scripting.memory_limit_mb, scripting.step_budget,
//   scripting.time_budget_us [x] LOD policy        ScriptingSystem::lod_tier(): near instances
//   tick, far ones are dormant [x] determinism       k_determinism below: outputs are a function of
//   script, inputs and tick [x] zero cost unused  no instances, no calls; the linker drops the
//   module when nothing links it [x] docs, tests, size table [x] bench bench/scripting_bench.cpp

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <foundation/scripting/script_context.h>

namespace engine::scripting {

// ---- the scheduler's registration table entry -------------------------------------------------

/// Tick phase (plan 05 §5.2). Scripted routines read the world the previous phase left and
/// return data the events phase turns into commands.
inline constexpr const char* k_phase = "systems";

/// Determinism stance (ADR-0010): "hashed". A script's output is a function of its bytes, its
/// arguments and the tick — the sandbox removes the wall clock and unseeded randomness, and the
/// step budget is counted, not timed — so a replay calls the same functions with the same inputs
/// and gets the same data back. What a script keeps in its own globals between calls is not part
/// of any save in v0, which is one reason the tick driver passes state in rather than leaving it
/// in the VM.
inline constexpr const char* k_determinism = "hashed";

/// LOD tiers this system runs at, one bit per tier, bit 0 is LOD0 (ADR-0010).
inline constexpr u32 k_lod_tiers = 0b0001u;

/// One scripted instance: the function to call and the object it is called with.
struct ScriptInstance {
  FunctionId on_tick;
  BindingId self;
  u32 tier = 0;
};

/// A non-nil value an instance's `on_tick` returned this tick.
struct TickResult {
  u32 instance = 0;
  JsonValue value;
};

/// Calls `on_tick(self, dt)` for every instance at tier 0, in instance order, each call under the
/// context's budgets. A failing call is counted, the first failure of a tick is logged, and the
/// tick goes on: one broken script must not stop ten thousand working ones.
class ScriptingSystem {
 public:
  explicit ScriptingSystem(ScriptContext& context) noexcept : context_(&context) {}

  /// Adds an instance and returns its index. Instances are never removed in v0.
  u32 add(FunctionId on_tick, BindingId self);
  void set_tier(u32 instance, u32 tier) noexcept;
  u32 instance_count() const noexcept { return instances_.size(); }

  /// Once per tick, before the parallel phase: the tick index and the fixed step every script
  /// sees through `engine.tick()` and `engine.fixed_step()`.
  void begin_tick(u64 tick, f64 fixed_step) noexcept;

  /// The per-tick work: one budgeted call per tier-0 instance.
  void tick(f32 dt);

  /// Once per tick, after the parallel phase. Results stay readable until the next begin_tick().
  void end_tick() noexcept {}

  /// The LOD tier for one instance given its observer score, the minimum over observers of
  /// f(distance, importance, observer weight) (plan 05 §5.4, ADR-0010). Lower is nearer. v0 has
  /// two tiers and no hysteresis: tier 0 is ticked, tier 1 is dormant, because a routine nobody
  /// can observe is summarized by its declarative state and not run.
  static u32 lod_tier(f32 observer_score) noexcept;

  u64 tick_index() const noexcept { return tick_; }
  u32 calls_last_tick() const noexcept { return calls_; }
  u32 failures_last_tick() const noexcept { return failures_; }
  const Vector<TickResult>& results() const noexcept { return results_; }

 private:
  ScriptContext* context_;
  Vector<ScriptInstance> instances_;
  Vector<TickResult> results_;
  u64 tick_ = 0;
  u32 calls_ = 0;
  u32 failures_ = 0;
  JsonValue scratch_;
};

}  // namespace engine::scripting

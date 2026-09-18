#pragma once

// animation capability (ADR-0027; docs/plan/02-architecture.md §2.8,
// docs/plan/05-simulation.md §5.15, docs/plan/11-performance-principles.md §11.10).
//
// TODO(animation): one paragraph — what this capability is, what it owns, what it does not do.
//
// Scaffolded by tools/new-capability.ps1. The module is additive by construction: nothing under
// core/ or foundation/, the render graph, the scheduler, or another capability is edited to add
// it, and ENGINE_WITH_ANIMATION=OFF (or ENGINE_MINIMAL=ON) removes it from the build
// entirely. Each registration point below is either wired up or deliberately not needed, and
// docs/subsystems/animation.md says which. The list is ADR-0027 decision 2.
//
//   [x] schema types      schemas/animation.schema, engine_schema_library
//   [ ] scheduler entry   the SystemDesc fields below, registered from src/animation.cpp
//   [ ] render passes     gfx::RenderGraph::add_pass() from this system, never a graph edit
//   [ ] derived data      a content-build step keyed by the asset property block it consumes
//   [ ] protocol methods  only if agents or the editor drive it; rerun with -WithProtocol
//   [ ] tunables          tunables::Int/Float/Bool/Enum at namespace scope, read outside hot loops
//   [x] LOD policy        AnimationSystem::lod_tier() (the observer function, plan 05 §5.4)
//   [x] determinism       k_determinism below
//   [ ] zero cost unused  name the mechanism (plan 11 §11.10) on the docs page
//   [x] docs, tests, size table
//   [x] bench             bench/animation_bench.cpp
//
// The hot-path rules are unchanged (plan 11 §11.4): the scheduler reaches this system through a
// constant-initialized table of function pointers, never a virtual interface, and no loop in the
// engine asks at run time whether this capability is present — the linker already answered.

#include <core/base/types.h>
#include <schemas/animation.h>

namespace engine::animation {

// ---- the scheduler's registration table entry -------------------------------------------------
//
// These are SystemDesc's fields (ADR-0027). The tick scheduler does not exist yet — Phase 1 is
// the renderer core — so they are documented constants here, and the commit that lands the
// scheduler turns them into table fields without changing their meaning.

/// Tick phase this system runs in (plan 05 §5.2): "input", "events_in", "lod", "systems",
/// "physics", "post_physics", "events_out", "persist".
inline constexpr const char* k_phase = "systems";

/// Determinism stance (ADR-0010). "hashed": the state lives in the fixed-step tick, enters the
/// sim hash, and replays exactly. "derived": output of the sim that is never read back into
/// gameplay (GPU work, visuals). TODO(animation): decide, and say why on the docs page.
inline constexpr const char* k_determinism = "hashed";

/// LOD tiers this system runs at, one bit per tier, bit 0 is LOD0 (ADR-0010). A system that runs
/// at every tier is usually a system that has not thought about it.
inline constexpr u32 k_lod_tiers = 0b0011u;

/// The capability's system. begin_tick() runs once before the parallel systems phase, tick()
/// inside it, end_tick() once after. They are plain member functions reached through the
/// registration table, so a tick costs no virtual dispatch, and a tick with no instances of this
/// capability does not call them at all: the scheduler drops a system whose instance set is
/// empty (plan 11 §11.10), which is what makes an unused capability free.
struct AnimationSystem {
  /// Once per tick, before the parallel phase: take the tick index, size this tick's pools.
  void begin_tick(u64 tick) noexcept;

  /// The per-tick work, over this capability's own instances only.
  /// TODO(animation): this is the capability.
  void tick(f32 dt) noexcept;

  /// Once per tick, after the parallel phase: publish results, emit events.
  void end_tick() noexcept;

  /// The LOD tier for one instance given its observer score: the minimum over observers of
  /// f(distance, importance, observer weight) (plan 05 §5.4, ADR-0010). Lower is nearer. The
  /// hysteresis band belongs here, in the capability, not in the caller.
  /// TODO(animation): the capability's real tiers and their bands.
  static u32 lod_tier(f32 observer_score) noexcept;

  u64 tick_index() const noexcept { return tick_; }
  f32 elapsed() const noexcept { return elapsed_; }

 private:
  u64 tick_ = 0;
  f32 elapsed_ = 0.0f;
};

}  // namespace engine::animation

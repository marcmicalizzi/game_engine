#pragma once

// The event scheduler's timing wheel (docs/plan/05-simulation.md §5.3).
//
// A hierarchical timing wheel over `GameTime` with five levels whose resolutions are the fixed
// step, then roughly a second, a minute, an hour and a day. Insert and cancel are O(1); advance
// is O(events delivered) plus a bounded walk of the level bitmasks, so skipping a game month
// with nothing scheduled costs the same as skipping a tick.
//
// Why a wheel and not a heap: the simulation schedules 10^5-10^6 timers whose due times are
// spread over game *years* and cancels most of them before they fire (an NPC's next meal is
// rescheduled every time it eats). A binary heap pays O(log n) on both, with a cache miss per
// level, and cannot cancel at all without a side index. The wheel pays a bucket push and a
// bucket unlink, both O(1) and both one cache line, and its cost per fired event is independent
// of how many timers are asleep. What the wheel gives up is a cheap "what is the next event" —
// it has to look — and this implementation buys that back with one occupancy bit per bucket.
//
// Determinism ([ADR-0010](../../../docs/adr/0010-deterministic-sim-and-lod-contract.md)):
// `hashed`. Delivery order is (due time, insertion sequence), both integers, so two runs with
// the same inputs deliver the same events in the same order on every machine, and the order
// does not depend on how many timers happen to share a bucket.

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/time/time.h>

#include <span>

namespace engine::sim {

// Ticks, seconds, minutes, hours, days. See `TimingWheel::resolution()` for why the upper four
// are "roughly": the fixed step does not divide a second, and delivery times stay exact anyway.
inline constexpr u32 k_wheel_levels = 5;

inline constexpr u32 k_invalid_slot = 0xFFFF'FFFFu;

// A system that never registered has no index; a plain `Timer(entity, kind, at)` has none either.
inline constexpr u16 k_no_system = 0xFFFFu;

// O(1) cancel with stale-handle detection, the same scheme `SlotMap` uses: a live slot's
// generation is odd, and freeing it makes it even, so a handle from a previous occupant of the
// slot fails the comparison rather than cancelling somebody else's timer.
struct TimerHandle {
  u32 index = 0;
  u32 generation = 0;

  constexpr bool valid() const noexcept { return (generation & 1u) != 0u; }
  constexpr bool operator==(const TimerHandle&) const noexcept = default;
};

// What the wheel carries for the game. The wheel never interprets any of it: `subject` is an
// entity or object, `kind` is the game's event kind, `user` is a spare word (a payload index,
// a reason code). Anything larger belongs in the game's own table, keyed by these.
struct TimerPayload {
  u64 subject = 0;
  u32 kind = 0;
  u32 user = 0;
};

// One delivered event. `at` is the due time, not the time the sink ran: a fast-forward delivers
// several events with different `at` inside one `advance`, and a system that stamps state with
// the wall position of the wheel instead of `at` is not replayable.
struct TimerEvent {
  GameTime at;
  GameTime interval;  // 0 for a one-shot
  TimerPayload payload;
  u64 sequence = 0;  // insertion order; the tie-break within one `at`
  TimerHandle handle;
  u16 system = k_no_system;
  bool periodic = false;
};

// Where events go. A function pointer and a context rather than `std::function`: no allocation,
// no indirect call through a type-erased wrapper, and nothing to own. Build one with
// `make_sink(callable)`.
struct EventSink {
  using Fn = void (*)(void* context, const TimerEvent& event);

  Fn fn = nullptr;
  void* context = nullptr;

  void operator()(const TimerEvent& event) const {
    if (fn != nullptr) fn(context, event);
  }
};

// Wraps a callable the caller keeps alive for the duration of the advance.
template <class F>
EventSink make_sink(F& callable) noexcept {
  EventSink sink;
  sink.fn = [](void* context, const TimerEvent& event) { (*static_cast<F*>(context))(event); };
  sink.context = &callable;
  return sink;
}

// The argument of `SummarizeInterval` (plan 05 §5.3, §5.5). `tile` and `seed` are zero for the
// fast-forward case and set for tile reconciliation, where the summary must be a deterministic
// function of the tile's stored seed and nothing else.
struct SummarizeInterval {
  GameTime from;
  GameTime to;
  u64 tile = 0;
  u64 seed = 0;
  u16 system = k_no_system;
  u8 tier = 3;
};

// A system's implementation of `SummarizeInterval`. Every LOD2/LOD3 system must register one;
// the contract it has to meet is in docs/subsystems/sim.md.
struct Summarizer {
  using Fn = void (*)(void* context, const SummarizeInterval& interval);

  const char* name = nullptr;
  Fn fn = nullptr;
  void* context = nullptr;
  u16 system = k_no_system;
  u8 tiers = 0x0Fu;  // bit t set: this summarizer covers tier t
};

struct TimingWheelConfig {
  GameTime step = GameTime::from_us(16'667);  // 60 Hz, matching GameClock's own rounding
  GameTime epoch;                             // the wheel's starting `now`
  u32 initial_capacity = 0;                   // timers to reserve up front
};

// What a budgeted advance did, so a caller can log it or assert on it.
struct FastForwardResult {
  GameTime from;
  GameTime to;
  u64 due_estimate = 0;  // due events counted; counting stops once it passes the budget
  u64 delivered = 0;     // events handed to the sink
  u64 summarized = 0;    // SummarizeInterval calls made
  u64 coarsened = 0;     // periodic timers re-armed past `to` instead of firing
  bool over_budget = false;
};

class TimingWheel {
 public:
  // One scheduled timer. Public because tests/size_table.cpp pins it: it is the only per-timer
  // allocation in the simulation's steady state and 10^6 of them is 56 MB.
  struct Slot {
    GameTime due;
    GameTime interval;  // 0 for a one-shot
    u64 sequence;
    TimerPayload payload;
    u32 next;
    u32 prev;
    u32 generation;
    u16 system;
    u8 level;  // k_wheel_levels means the far list
    u8 flags;
  };

  explicit TimingWheel(const TimingWheelConfig& config = {});
  ENGINE_NON_COPYABLE(TimingWheel);

  // --- shape ---------------------------------------------------------------------------------

  GameTime now() const noexcept { return now_; }
  GameTime step() const noexcept { return GameTime{resolution_[0]}; }
  static constexpr u32 level_count() noexcept { return k_wheel_levels; }
  u32 slots(u32 level) const noexcept { return slot_count_[level]; }
  // Level 0's resolution is the fixed step exactly; every higher level is an integer multiple of
  // the level below, so cascading is exact. The multiples are chosen to land near a second, a
  // minute, an hour and a day — near and not on, because 60 Hz is 16,667 us and no ladder of
  // integers off that hits a second. Level spans are an internal bucketing choice; a timer's
  // due time is exact microseconds whatever the ladder is.
  GameTime resolution(u32 level) const noexcept { return GameTime{resolution_[level]}; }
  // How far ahead the top level reaches from `now`. Beyond it timers wait in the far list.
  GameTime horizon() const noexcept;

  u32 live_count() const noexcept { return live_; }
  u32 far_count() const noexcept { return far_count_; }
  u32 slab_capacity() const noexcept { return slab_.size(); }
  u64 next_sequence() const noexcept { return next_sequence_; }

  // --- scheduling ----------------------------------------------------------------------------

  // A timer due at `at`. A time already past is due immediately (clamped to `now`), which is
  // what a reconciled tile wants and what a clamp-free version would turn into a lost event.
  TimerHandle schedule(GameTime at, const TimerPayload& payload);

  // `Periodic(system, interval, phase)`. The first firing is the smallest t > now with
  // t ≡ phase (mod interval), so an hourly economy registered at 09:13 still fires on the hour
  // and two worlds that registered it at different moments stay in step. The handle stays valid
  // across firings: cancel it once and the periodic is gone.
  TimerHandle schedule_periodic(u16 system, GameTime interval, GameTime phase);
  TimerHandle schedule_periodic(u16 system, GameTime interval, GameTime phase,
                                const TimerPayload& payload);

  // O(1). False for a stale, cancelled, or never-issued handle.
  bool cancel(TimerHandle handle) noexcept;
  bool is_live(TimerHandle handle) const noexcept;
  bool due_time(TimerHandle handle, GameTime& out) const noexcept;

  // --- advancing -----------------------------------------------------------------------------

  // Delivers every event due in (now, to] in (due, sequence) order, cascading between levels,
  // and leaves `now` at `to`. Re-entrant scheduling from the sink is allowed: a timer the sink
  // schedules for a time still inside this advance is delivered by this advance.
  void advance(GameTime to, const EventSink& sink);

  // Fast-forward (plan 05 §5.3, §13.5). When the number of events due in (from, to] exceeds
  // `budget`, every registered summarizer is asked to `summarize_interval(from, to)` once and
  // the periodics of summarized systems are re-armed past `to` instead of firing. One-shot
  // timers and the periodics of systems with no summarizer are delivered either way — they are
  // not coarsenable, which is why the budget is a trigger and not a cap.
  FastForwardResult advance(GameTime from, GameTime to, u64 budget, const EventSink& sink);

  // Counts events due in (now, to], stopping once the count exceeds `limit`. Periodic firings
  // are counted analytically, so a daily periodic over a game year costs one division.
  u64 count_due(GameTime to, u64 limit) const;

  // --- summarizers ---------------------------------------------------------------------------

  u32 add_summarizer(const Summarizer& summarizer);
  std::span<const Summarizer> summarizers() const noexcept {
    return {summarizers_.data(), summarizers_.size()};
  }
  void clear_summarizers() noexcept { summarizers_.clear(); }

  // Drops every timer and moves `now`. Summarizers are kept.
  void reset(GameTime at);

  // Debug-only: every live timer is in the bucket its due time and the current position say it
  // should be in, and the occupancy bits agree with the bucket heads.
  bool validate() const;

 private:
  // A time's bucket index at every level, plus one past the top for the top level's block.
  // Level L+1's index is level L's divided by level L's slot count, so the six of them cost one
  // 64-bit division plus five small ones and every level test afterwards is a comparison. That
  // matters: mapping a due time to a level was the wheel's largest single cost, and a cascade
  // does it once per timer it moves.
  static constexpr u32 k_index_count = k_wheel_levels + 1;

  u32 alloc_slot();
  void free_slot(u32 index) noexcept;
  void indices_of(i64 us, u64* out) const noexcept;
  void refresh_now_indices() noexcept;
  u32 choose_level(const u64* index) const noexcept;
  u32 bucket_from(u32 level, const u64* index) const noexcept;
  void link(u32 slot_index, u32 level, const u64* index) noexcept;
  void relink(u32 slot_index) noexcept;
  void unlink(u32 slot_index) noexcept;
  u32 bucket_of(u32 index) const noexcept;
  u64 level_index(u32 level, i64 us) const noexcept;
  void set_now(GameTime to);
  void cascade(u32 level, u32 slot);
  void rehome_far();
  bool seek(GameTime to, u64& out_index);
  bool next_occupied(u32 level, u32 first, u32 last, u32& out_slot) const noexcept;
  void deliver_bucket(u64 index0, GameTime to, const EventSink& sink, u64& delivered);
  void coarsen(GameTime to, FastForwardResult& result);
  bool summarizes(u16 system) const noexcept;

  i64 resolution_[k_wheel_levels] = {};
  u32 slot_count_[k_wheel_levels] = {};
  u32 bucket_base_[k_wheel_levels] = {};
  u32 word_base_[k_wheel_levels] = {};
  u64 now_index_[k_index_count] = {};

  Vector<Slot> slab_;
  Vector<u32> heads_;      // one per bucket, plus one for the far list
  Vector<u64> occupancy_;  // one bit per bucket, for the skip search
  Vector<u32> periodics_;  // slab indices of live periodics; a handful, not a per-entity list
  Vector<Summarizer> summarizers_;
  // Scratch: one bucket's due timers, as handles rather than indices, so a sink that cancels a
  // timer still in this batch is noticed instead of delivering a freed slot.
  Vector<TimerHandle> batch_;

  GameTime now_;
  u64 next_sequence_ = 0;
  u32 free_head_ = k_invalid_slot;
  u32 live_ = 0;
  u32 far_count_ = 0;
  u32 far_bucket_ = 0;
};

}  // namespace engine::sim

#pragma once

// The player of a headless world (docs/subsystems/world.md, "The player";
// docs/plan/05-simulation.md §5.10, "input log + event log => replay"): one of the tile ring's
// observers, steered by a recorded input log through an action map, the way engine-view's fly
// camera is steered by the live one (docs/subsystems/input.md).
//
// **Why an observer.** A headless world has one thing a player's input can move without a game to
// interpret it: an observer, which decides what the world streams in and so what it materializes,
// reconciles and writes back. It is the least a player is and still changes the persistent state,
// which is what a replay has to be asserted over; a game's own input consumers are its systems.
//
// **The input state at tick T is a fold of the log's events up to T.** `input::InputState` reads no
// clock and holds only what its events left it (input.md), so it is derived, not saved: whoever
// hands this a log hands it the whole log, and `catch_up` feeds every tick the world has already
// run before the next one steps. A world that ran from tick 0 and a world loaded at tick K
// therefore hold the same state at K, and the same velocity comes out of every tick after it. What
// a save keeps is the log itself, up to the tick it was taken at (`recorded`): the input that
// happened, not the input still to come.
//
// **Between ticks.** The world's observers move between two `SimScheduler::step()`s, where the ring
// updates (world.h): `step(t)` feeds tick t's events and returns the velocity its action asks for,
// and the host moves the observer by it over the fixed step. The action is an `Axis2`: x along
// world x, y forward along world -z (the direction an identity orientation faces, core/math).

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/math/math.h>
#include <foundation/input/input.h>
#include <foundation/input/input_log.h>

#include <memory>
#include <string>
#include <string_view>

namespace engine::world {

class InputObserver {
 public:
  InputObserver() = default;
  ENGINE_NON_COPYABLE(InputObserver);

  // Binds a log and the map it was recorded against, and steers observer `observer` by `action` at
  // `speed` metres per game second at full deflection. Refused, with nothing changed and a
  // sentence, when the map's hash is not the log's (the same code would mean another action), the
  // action is not an Axis2 of the map, or the speed is not a finite number >= 0. The state starts
  // empty: `catch_up` brings it to the world's tick.
  bool bind(const input::ActionMap& map, const input::InputLog& log, std::string_view action,
            f32 speed, u32 observer, std::string& error);
  bool bound() const noexcept { return state_ != nullptr; }
  void unbind() noexcept;

  // Feeds every tick through `tick` that has not been fed yet, from the log's first.
  void catch_up(u64 tick);
  // Feeds tick `tick` (catching up first when ticks were skipped) and returns the velocity the
  // action asks for on the ground plane, metres per game second.
  Vec3 step(u64 tick);

  // The log's events stamped at or before `tick`, under the same map hash and session block.
  void recorded(u64 tick, input::InputLog& out) const;

  const input::ActionMap& map() const noexcept { return map_; }
  const input::InputLog& log() const noexcept { return log_; }
  std::string_view action() const noexcept { return action_name_; }
  f32 speed() const noexcept { return speed_; }
  u32 observer() const noexcept { return observer_; }
  // The last tick fed, or none (`fed_any` false).
  bool fed_any() const noexcept { return fed_any_; }
  u64 fed_through() const noexcept { return fed_through_; }
  // What `input::InputState::state_hash` says about the state after the last tick fed: what two
  // worlds compare to know they hold the same input.
  u64 state_hash() const noexcept;

 private:
  input::ActionMap map_;
  input::InputLog log_;
  // Holds a pointer to `map_`, which is why this class neither copies nor moves.
  std::unique_ptr<input::InputState> state_;
  input::ActionId action_ = input::k_invalid_action;
  std::string action_name_;
  f32 speed_ = 0.0f;
  u32 observer_ = 0;
  u32 cursor_ = 0;
  u64 fed_through_ = 0;
  bool fed_any_ = false;
};

}  // namespace engine::world

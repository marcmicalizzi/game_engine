#pragma once

// Input mapping: named actions, bindings, and one deterministic InputState per simulation tick
// (docs/plan/05-simulation.md §5.2 "input" phase and §5.10 determinism and replay).
//
// This module never sees a window. It consumes `RawEvent`, a plain tick-stamped record, so the
// same code path serves a live session, a headless server, and a replay read from a log with no
// display attached. The caller does the conversion once, at the edge:
//
//     window::Event e;
//     while (window.poll(e)) {
//       input::RawEvent r{.tick = clock.tick()};
//       switch (e.kind) {
//         case window::EventKind::KeyDown:
//         case window::EventKind::KeyUp:
//           r.source = input::Source::Key;
//           r.code   = e.scancode;                              // the raw SDL scancode
//           r.value  = e.kind == window::EventKind::KeyDown ? 1.0f : 0.0f;
//           break;
//         case window::EventKind::MouseButtonDown:
//         case window::EventKind::MouseButtonUp:
//           r.source = input::Source::MouseButton;
//           r.code   = e.button;                                // 1 left, 2 middle, 3 right
//           r.value  = e.kind == window::EventKind::MouseButtonDown ? 1.0f : 0.0f;
//           break;
//         case window::EventKind::MouseMove:                    // two events, dx and dy
//           r.source = input::Source::MouseAxis;
//           r.code   = u32(input::MouseAxisCode::X);            // then ::Y with e.dy
//           r.value  = e.dx;
//           break;
//         case window::EventKind::MouseWheel:                   // MouseAxisCode::WheelX/WheelY
//           ...
//         case window::EventKind::GamepadButtonDown:
//         case window::EventKind::GamepadButtonUp:
//           r.source = input::Source::GamepadButton;
//           r.code   = u32(e.gamepad_button);                   // window::GamepadButton
//           r.value  = e.kind == window::EventKind::GamepadButtonDown ? 1.0f : 0.0f;
//           r.device = e.gamepad;                               // the window module's slot id
//           break;
//         case window::EventKind::GamepadAxis:
//           r.source = input::Source::GamepadAxis;
//           r.code   = u32(e.gamepad_axis);                     // window::GamepadAxis
//           r.value  = e.value;                                 // -1..1, or 0..1 for triggers
//           r.device = e.gamepad;
//           break;
//       }
//       state.feed(r); log.record(r);
//     }
//
// So `code` is a key scancode, a mouse button number, a `MouseAxisCode`, a
// `window::GamepadButton`, or a `window::GamepadAxis`, by source. Those numbering schemes are
// part of the log format: enumerators are appended, never reordered.
//
// Per tick:
//
//     state.begin_tick(clock.tick());
//     for (const RawEvent& e : events_this_tick) state.feed(e);
//     state.end_tick();
//     if (state.pressed(fire)) ...;  Vec2 move = state.axis2(walk);
//
// Nothing here reads a clock or measures a duration: a hold time is the caller's tick
// arithmetic. The same event sequence therefore produces the same states on every machine,
// which is what makes an InputLog replay bit for bit.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/flat_map.h>
#include <core/containers/vector.h>
#include <core/json/json_value.h>
#include <core/math/math.h>
#include <core/time/time.h>

#include <span>
#include <string>
#include <string_view>

namespace engine::input {

// What a binding reads. The meaning of `Binding::code` follows from it (see the header note).
enum class Source : u8 { Key, MouseButton, MouseAxis, GamepadButton, GamepadAxis };
inline constexpr u32 k_source_count = 5;

// "key", "mouse_button", "mouse_axis", "gamepad_button", "gamepad_axis": the names in a saved
// ActionMap and in a log header. Stable; they are file format, not display text.
const char* source_name(Source source) noexcept;
bool source_from_name(std::string_view name, Source& out) noexcept;
// True for the sources that report a press and a release (Key, MouseButton, GamepadButton).
constexpr bool is_digital(Source source) noexcept {
  return source == Source::Key || source == Source::MouseButton || source == Source::GamepadButton;
}

// `Binding::code` for Source::MouseAxis. X and Y are pointer motion in window pixels for the
// tick; the wheel axes are notches.
enum class MouseAxisCode : u32 { X = 0, Y = 1, WheelX = 2, WheelY = 3 };

// One input a player can be moved by. `scale` turns a key into an axis end (+1 or -1) and
// inverts or attenuates an analog axis; `deadzone` is the fraction of an analog gamepad axis's
// travel that reads as zero, with the rest rescaled so the usable range still reaches 1.
struct Binding {
  Source source = Source::Key;
  u32 code = 0;
  f32 scale = 1.0f;
  f32 deadzone = 0.0f;  // Source::GamepadAxis only
};

enum class ActionKind : u8 { Button, Axis, Axis2 };
const char* action_kind_name(ActionKind kind) noexcept;
bool action_kind_from_name(std::string_view name, ActionKind& out) noexcept;

// A handle into one ActionMap: an index, stable for that map's lifetime.
using ActionId = u32;
inline constexpr ActionId k_invalid_action = 0xFFFFFFFFu;

// A binding plus the component of an Axis2 action it drives (0 = x, 1 = y). Ignored by Button
// and Axis actions, which is why `bind()` defaults it.
struct ActionBinding {
  Binding binding;
  u32 component = 0;
};

// An analog binding on a Button action counts as down at or past this much travel, so a
// trigger or a stick can fire.
inline constexpr f32 k_analog_button_threshold = 0.5f;

// The bindings a game ships and a player edits. Names are unique; ids are dense and start at 0.
//
// Serializes to and from JSON through the canonical writer, so a default map is a file in the
// game's content and a rebind is a diff. `hash()` identifies a map's contents; an InputLog
// carries it so a replay cannot be run against bindings it was not recorded with.
class ActionMap {
 public:
  ActionMap() = default;

  // Returns the existing id when `name` is already an action of the same kind, and
  // k_invalid_action when it exists with a different kind or the name is empty.
  ActionId add_action(std::string_view name, ActionKind kind);
  ActionId find_action(std::string_view name) const noexcept;
  u32 action_count() const noexcept { return actions_.size(); }
  bool valid(ActionId action) const noexcept { return action < actions_.size(); }
  std::string_view action_name(ActionId action) const noexcept;
  ActionKind action_kind(ActionId action) const noexcept;

  // Several bindings may drive one action; they are summed (axes) or or-ed (buttons) in the
  // order they were added. `component` picks x or y of an Axis2 action.
  void bind(ActionId action, const Binding& binding, u32 component = 0);
  std::span<const ActionBinding> bindings(ActionId action) const noexcept;
  void clear_bindings(ActionId action);
  void clear() noexcept;

  // Whether any action reads this signal. InputState drops events that answer false, so an
  // unbound key costs one lookup and nothing else.
  bool is_bound(Source source, u32 code) const noexcept;

  // A digest of every action name, kind, and binding, in order. Two maps that describe the
  // same bindings hash the same on every platform; any edit changes it.
  u64 hash() const noexcept;

  JsonValue to_json() const;
  // Replaces the whole map. On failure the map is left empty and `error` says what was wrong.
  bool from_json(const JsonValue& value, std::string* error = nullptr);
  // The map as text for tools and tests: a header line with the action count and the hash,
  // then the canonical JSON. Two maps with the same bindings describe identically.
  std::string describe() const;

 private:
  struct Action {
    std::string name;
    ActionKind kind = ActionKind::Button;
    Vector<ActionBinding> list;
  };

  static u64 signal_key(Source source, u32 code) noexcept {
    return (static_cast<u64>(source) << 32) | code;
  }

  Vector<Action> actions_;
  FlatMap<std::string, u32> by_name_;
  FlatMap<u64, u32> bound_;  // signal key -> how many bindings read it
};

// One input event, stamped with the tick it belongs to (plan 05 §5.12: "every input carries its
// tick number"). `device` is the gamepad slot for gamepad sources and 0 for keyboard and mouse.
struct RawEvent {
  SimTick tick;
  Source source = Source::Key;
  u32 code = 0;
  f32 value = 0.0f;  // 1/0 for a press and release, the axis position for an analog source
  u32 device = 0;
};

// Every device, for InputState::set_device_filter.
inline constexpr u32 k_any_device = 0xFFFFFFFFu;

// The state of every action for one tick, built from events.
//
// Deterministic by construction: the only inputs are the action map and the event sequence, and
// nothing inside reads a clock. Events for codes no binding reads are dropped, so an InputState
// holds one signal per bound code and no more.
class InputState {
 public:
  InputState() noexcept = default;
  explicit InputState(const ActionMap& map) noexcept : map_(&map) {}

  // The map must outlive the state. Setting one clears the signals, because a code's meaning
  // has changed; do it between ticks.
  void set_map(const ActionMap& map) noexcept;
  const ActionMap* map() const noexcept { return map_; }

  // Ignore events from other gamepad slots, which is how two players share one event stream.
  // k_any_device (the default) merges every device into one state.
  void set_device_filter(u32 device) noexcept { device_filter_ = device; }
  u32 device_filter() const noexcept { return device_filter_; }

  // Rolls this tick's values into the previous tick's, clears the edges, and zeroes the
  // per-tick accumulators (mouse motion and wheel).
  void begin_tick(SimTick tick);
  void feed(const RawEvent& event);
  void end_tick();
  SimTick tick() const noexcept { return tick_; }
  // Forgets every signal; the next tick starts as if nothing had ever been pressed.
  void reset() noexcept;

  // Went down during this tick. A digital binding reports the edge even when the same code
  // went up again before the tick ended, so a press is never swallowed by a slow tick; an
  // analog binding on a Button action reports the tick it crossed k_analog_button_threshold.
  bool pressed(ActionId action) const noexcept;
  bool released(ActionId action) const noexcept;
  bool held(ActionId action) const noexcept;
  // Sum of the bindings' values times their scales, clamped to -1..1.
  f32 axis(ActionId action) const noexcept;
  // Components 0 and 1 of an Axis2 action. A pair of gamepad axes bound one per component with
  // the same non-zero deadzone is treated as a stick and gets a radial deadzone, so a diagonal
  // is not harder to reach than an edge.
  Vec2 axis2(ActionId action) const noexcept;
  // Pointer motion accumulated over this tick, in window pixels. Tracked whether or not
  // anything is bound to it, because it is the state's own reading rather than an action.
  Vec2 mouse_delta() const noexcept { return Vec2{mouse_dx_, mouse_dy_}; }

  // A digest of every query result at this tick. Two runs that produce the same digest for
  // every tick produced the same input; this is what a replay test compares.
  u64 state_hash() const noexcept;

 private:
  struct Signal {
    f32 value = 0.0f;
    f32 previous = 0.0f;
    u16 down_edges = 0;
    u16 up_edges = 0;
  };

  static u64 signal_key(Source source, u32 code) noexcept {
    return (static_cast<u64>(source) << 32) | code;
  }

  const Signal* find_signal(Source source, u32 code) const noexcept;
  f32 bound_value(const Binding& binding, bool previous) const noexcept;
  bool binding_active(const Binding& binding, bool previous) const noexcept;

  const ActionMap* map_ = nullptr;
  FlatMap<u64, Signal> signals_;
  SimTick tick_;
  u32 device_filter_ = k_any_device;
  f32 mouse_dx_ = 0.0f;
  f32 mouse_dy_ = 0.0f;
  bool in_tick_ = false;
};

// The deadzone curve a binding applies: zero inside the zone, and the rest rescaled so full
// travel still reads 1. Exposed because the same curve is the radial one in axis2().
f32 apply_deadzone(f32 value, f32 deadzone) noexcept;

}  // namespace engine::input

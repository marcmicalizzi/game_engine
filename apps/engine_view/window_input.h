#pragma once

// **Window events into input, once** (docs/subsystems/input.md, AGENTS.md "Input reaches a game
// as tick-stamped RawEvents"). engine-view owns the window, so engine-view is where a
// `window::Event` becomes the `input::RawEvent`s it means — here, and nowhere after it: the camera
// reads an `InputState` and never sees the window, so a replay read from a file and a session with
// a window in front of it feed the camera through the same door.
//
// Why the conversion lives in the app and not in either module: `foundation/input` must not
// depend on `foundation/window` — a headless server and a replay have no window to depend on — and
// `foundation/window` stays raw and knows nothing of bindings. The only code that knows both
// vocabularies is the code that owns both, and a game writes this same file for its own window.
//
// Two things happen at this edge on purpose, because the edge is where the information is:
//
// - **Pointer motion is only looking while the window holds the pointer.** Uncaptured, the cursor
//   is pointing — at the title bar, at another window — and its motion is dropped here, so it is
//   never fed and never recorded. Whether the window held the pointer is therefore already in the
//   log, as the events that are there and the ones that are not, and a replay needs no window
//   state to reproduce it.
// - **Motion and the wheel are summed per tick.** A 1 kHz mouse sends a thousand events a second
//   and the camera reads their sum per tick; summing here gives `InputState` the same total in the
//   same order of additions, and keeps a log's size proportional to the ticks rather than to the
//   mouse's polling rate. A key repeat is dropped for the same reason: `InputState` would ignore
//   it (a repeat is not a second press) and it would only lengthen the log.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/time/time.h>
#include <foundation/input/input.h>
#include <foundation/window/window.h>

#include <span>

namespace engine::view {

// One `window::Event` as `RawEvent`s stamped with `tick`, into `out` (room for four: a hat is four
// signals, one per direction). Returns how many. `pointer_captured` false drops pointer motion.
// Connections, focus, resizes and the rest mean nothing to input and give zero.
u32 convert_window_event(const window::Event& event, SimTick tick, bool pointer_captured,
                         input::RawEvent out[4]) noexcept;

// The events a live session has converted and not yet fed. Everything added between two runs of
// the tick loop carries one stamp — the tick they will be fed at — and pointer motion and the
// wheel are summed into one event per axis. Sized once; a frame that converts more than it holds
// grows it, which is the one allocation this path can make, and only under a flood.
class EdgeEvents {
 public:
  explicit EdgeEvents(u32 capacity = 1024);
  // Converts and appends. `tick` must not go backwards; a new tick starts new motion sums.
  void add(const window::Event& event, SimTick tick, bool pointer_captured);
  // Appends an already converted event, summing it into this tick's motion the same way.
  void add_raw(const input::RawEvent& event);
  std::span<const input::RawEvent> events() const noexcept {
    return {events_.data(), events_.size()};
  }
  bool empty() const noexcept { return events_.empty(); }
  void clear() noexcept;

 private:
  static constexpr u32 k_none = 0xFFFFFFFFu;
  Vector<input::RawEvent> events_;
  u32 summed_[4] = {k_none, k_none, k_none, k_none};  // this tick's MouseAxisCode entries
  SimTick tick_;
};

// The window event a recorded `RawEvent` came from, for `--inject-input`: a key, a mouse button,
// pointer motion or the wheel. False for a gamepad or a joystick, which name a device slot the
// window assigned to a real device (window::Window::push_event says the same).
bool window_event_of(const input::RawEvent& raw, window::Event& out) noexcept;

}  // namespace engine::view

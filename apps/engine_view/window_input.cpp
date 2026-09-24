#include "window_input.h"

namespace engine::view {

u32 convert_window_event(const window::Event& e, SimTick tick, bool pointer_captured,
                         input::RawEvent out[4]) noexcept {
  u32 n = 0;
  auto emit = [&](input::Source source, u32 code, f32 value, u32 device) {
    out[n++] = input::RawEvent{tick, source, code, value, device};
  };
  switch (e.kind) {
    case window::EventKind::KeyDown:
      if (e.repeat) break;  // not a second press, and InputState would say so too
      emit(input::Source::Key, e.scancode, 1.0f, 0);
      break;
    case window::EventKind::KeyUp: emit(input::Source::Key, e.scancode, 0.0f, 0); break;
    case window::EventKind::MouseButtonDown:
    case window::EventKind::MouseButtonUp:
      emit(input::Source::MouseButton, e.button,
           e.kind == window::EventKind::MouseButtonDown ? 1.0f : 0.0f, 0);
      break;
    case window::EventKind::MouseMove:
      if (!pointer_captured) break;  // pointing, not looking
      if (e.dx != 0.0f) emit(input::Source::MouseAxis, u32(input::MouseAxisCode::X), e.dx, 0);
      if (e.dy != 0.0f) emit(input::Source::MouseAxis, u32(input::MouseAxisCode::Y), e.dy, 0);
      break;
    case window::EventKind::MouseWheel:
      if (e.dx != 0.0f) {
        emit(input::Source::MouseAxis, u32(input::MouseAxisCode::WheelX), e.dx, 0);
      }
      if (e.dy != 0.0f) {
        emit(input::Source::MouseAxis, u32(input::MouseAxisCode::WheelY), e.dy, 0);
      }
      break;
    case window::EventKind::GamepadButtonDown:
    case window::EventKind::GamepadButtonUp:
      emit(input::Source::GamepadButton, static_cast<u32>(e.gamepad_button),
           e.kind == window::EventKind::GamepadButtonDown ? 1.0f : 0.0f, e.gamepad);
      break;
    case window::EventKind::GamepadAxis:
      emit(input::Source::GamepadAxis, static_cast<u32>(e.gamepad_axis), e.value, e.gamepad);
      break;
    case window::EventKind::JoystickAxis:
      emit(input::Source::JoystickAxis, e.index, e.value, e.joystick);
      break;
    case window::EventKind::JoystickButtonDown:
    case window::EventKind::JoystickButtonUp:
      emit(input::Source::JoystickButton, e.index,
           e.kind == window::EventKind::JoystickButtonDown ? 1.0f : 0.0f, e.joystick);
      break;
    case window::EventKind::JoystickHat: {
      // A position, not an edge: all four directions every time, and InputState counts an edge
      // only where one changed, which keeps this stateless (input.h says the same).
      const u32 directions[4] = {input::k_hat_up, input::k_hat_right, input::k_hat_down,
                                 input::k_hat_left};
      for (const u32 direction : directions) {
        emit(input::Source::JoystickHat, input::hat_code(e.index, direction),
             (static_cast<u32>(e.hat) & direction) != 0 ? 1.0f : 0.0f, e.joystick);
      }
      break;
    }
    default: break;
  }
  return n;
}

EdgeEvents::EdgeEvents(u32 capacity) { events_.reserve(capacity); }

void EdgeEvents::clear() noexcept {
  events_.clear();
  for (u32& index : summed_)
    index = k_none;
}

void EdgeEvents::add_raw(const input::RawEvent& event) {
  if (event.tick != tick_) {
    tick_ = event.tick;
    for (u32& index : summed_)
      index = k_none;
  }
  if (event.source == input::Source::MouseAxis && event.code < 4 && event.device == 0) {
    u32& index = summed_[event.code];
    if (index != k_none) {
      events_[index].value = events_[index].value + event.value;
      return;
    }
    index = events_.size();
  }
  events_.push_back(event);
}

void EdgeEvents::add(const window::Event& event, SimTick tick, bool pointer_captured) {
  input::RawEvent raw[4];
  const u32 n = convert_window_event(event, tick, pointer_captured, raw);
  for (u32 i = 0; i < n; ++i)
    add_raw(raw[i]);
}

bool window_event_of(const input::RawEvent& raw, window::Event& out) noexcept {
  out = window::Event{};
  switch (raw.source) {
    case input::Source::Key:
      out.kind = raw.value != 0.0f ? window::EventKind::KeyDown : window::EventKind::KeyUp;
      out.scancode = static_cast<u16>(raw.code);
      return raw.code <= 0xFFFFu;
    case input::Source::MouseButton:
      out.kind =
          raw.value != 0.0f ? window::EventKind::MouseButtonDown : window::EventKind::MouseButtonUp;
      out.button = static_cast<u8>(raw.code);
      return raw.code <= 0xFFu;
    case input::Source::MouseAxis:
      switch (static_cast<input::MouseAxisCode>(raw.code)) {
        case input::MouseAxisCode::X:
          out.kind = window::EventKind::MouseMove;
          out.dx = raw.value;
          return true;
        case input::MouseAxisCode::Y:
          out.kind = window::EventKind::MouseMove;
          out.dy = raw.value;
          return true;
        case input::MouseAxisCode::WheelX:
          out.kind = window::EventKind::MouseWheel;
          out.dx = raw.value;
          return true;
        case input::MouseAxisCode::WheelY:
          out.kind = window::EventKind::MouseWheel;
          out.dy = raw.value;
          return true;
      }
      return false;
    default: return false;
  }
}

}  // namespace engine::view

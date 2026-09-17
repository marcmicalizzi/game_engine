#include <core/base/assert.h>
#include <core/hash/hash.h>
#include <foundation/input/input.h>

#include <bit>
#include <cmath>

namespace engine::input {

namespace {

f32 clamp_unit(f32 v) noexcept { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

u32 float_bits(f32 v) noexcept {
  if (v == 0.0f) v = 0.0f;  // a sum of +1 and -1 may land on -0; it is the same state as 0
  return std::bit_cast<u32>(v);
}

}  // namespace

f32 apply_deadzone(f32 value, f32 deadzone) noexcept {
  if (deadzone <= 0.0f) return value;
  if (deadzone >= 1.0f) return 0.0f;
  const f32 magnitude = value < 0.0f ? -value : value;
  if (magnitude <= deadzone) return 0.0f;
  const f32 rescaled = (magnitude - deadzone) / (1.0f - deadzone);
  return value < 0.0f ? -rescaled : rescaled;
}

void InputState::set_map(const ActionMap& map) noexcept {
  map_ = &map;
  reset();
}

void InputState::reset() noexcept {
  signals_.clear();
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;
  in_tick_ = false;
}

void InputState::begin_tick(SimTick tick) {
  tick_ = tick;
  in_tick_ = true;
  mouse_dx_ = 0.0f;
  mouse_dy_ = 0.0f;
  for (auto [key, signal] : signals_) {
    signal.previous = signal.value;
    signal.down_edges = 0;
    signal.up_edges = 0;
    // Mouse motion and the wheel are deltas, not positions: each tick starts them at rest.
    if (static_cast<Source>(key >> 32) == Source::MouseAxis) signal.value = 0.0f;
  }
}

void InputState::feed(const RawEvent& event) {
  ENGINE_ASSERT(in_tick_, "input::InputState::feed: call begin_tick() first");
  if (device_filter_ != k_any_device && event.device != device_filter_) return;

  // The pointer's motion is the state's own reading rather than an action, so it is tracked
  // whether or not a binding asks for it.
  if (event.source == Source::MouseAxis) {
    if (event.code == static_cast<u32>(MouseAxisCode::X)) mouse_dx_ += event.value;
    if (event.code == static_cast<u32>(MouseAxisCode::Y)) mouse_dy_ += event.value;
  }

  if (map_ == nullptr || !map_->is_bound(event.source, event.code)) return;
  Signal& signal = signals_[signal_key(event.source, event.code)];
  switch (event.source) {
    case Source::Key:
    case Source::MouseButton:
    case Source::GamepadButton:
    case Source::JoystickButton:
    // A hat direction is fed as 1 or 0 per event, so it edges exactly like a button.
    case Source::JoystickHat: {
      const bool down = event.value != 0.0f;
      const bool was_down = signal.value != 0.0f;
      // The counters saturate: what a query asks is whether the edge happened at all.
      if (down && !was_down && signal.down_edges < 0xFFFFu) {
        signal.down_edges = static_cast<u16>(signal.down_edges + 1);
      }
      if (!down && was_down && signal.up_edges < 0xFFFFu) {
        signal.up_edges = static_cast<u16>(signal.up_edges + 1);
      }
      signal.value = down ? 1.0f : 0.0f;
      break;
    }
    case Source::MouseAxis: signal.value += event.value; break;
    case Source::GamepadAxis:
    case Source::JoystickAxis: signal.value = clamp_unit(event.value); break;
  }
}

void InputState::end_tick() { in_tick_ = false; }

const InputState::Signal* InputState::find_signal(Source source, u32 code) const noexcept {
  return signals_.find_value(signal_key(source, code));
}

f32 InputState::bound_value(const Binding& binding, bool previous) const noexcept {
  const Signal* signal = find_signal(binding.source, binding.code);
  if (signal == nullptr) return 0.0f;
  const f32 raw = previous ? signal->previous : signal->value;
  return is_analog_axis(binding.source) ? apply_deadzone(raw, binding.deadzone) : raw;
}

bool InputState::binding_active(const Binding& binding, bool previous) const noexcept {
  const f32 v = bound_value(binding, previous);
  if (is_digital(binding.source)) return v != 0.0f;
  const f32 scaled = v * binding.scale;
  const f32 magnitude = scaled < 0.0f ? -scaled : scaled;
  return magnitude >= k_analog_button_threshold;
}

bool InputState::pressed(ActionId action) const noexcept {
  if (map_ == nullptr) return false;
  for (const ActionBinding& ab : map_->bindings(action)) {
    if (is_digital(ab.binding.source)) {
      const Signal* signal = find_signal(ab.binding.source, ab.binding.code);
      if (signal != nullptr && signal->down_edges > 0) return true;
    } else if (binding_active(ab.binding, false) && !binding_active(ab.binding, true)) {
      return true;
    }
  }
  return false;
}

bool InputState::released(ActionId action) const noexcept {
  if (map_ == nullptr) return false;
  for (const ActionBinding& ab : map_->bindings(action)) {
    if (is_digital(ab.binding.source)) {
      const Signal* signal = find_signal(ab.binding.source, ab.binding.code);
      if (signal != nullptr && signal->up_edges > 0) return true;
    } else if (!binding_active(ab.binding, false) && binding_active(ab.binding, true)) {
      return true;
    }
  }
  return false;
}

bool InputState::held(ActionId action) const noexcept {
  if (map_ == nullptr) return false;
  for (const ActionBinding& ab : map_->bindings(action)) {
    if (binding_active(ab.binding, false)) return true;
  }
  return false;
}

f32 InputState::axis(ActionId action) const noexcept {
  if (map_ == nullptr) return 0.0f;
  f32 sum = 0.0f;
  for (const ActionBinding& ab : map_->bindings(action)) {
    sum += bound_value(ab.binding, false) * ab.binding.scale;
  }
  return clamp_unit(sum);
}

Vec2 InputState::axis2(ActionId action) const noexcept {
  if (map_ == nullptr) return Vec2{};
  const std::span<const ActionBinding> list = map_->bindings(action);

  // A stick is two analog axes of the same source, one per component, with the same deadzone.
  // Taking their deadzone radially instead of per axis keeps the usable area a disc: without it
  // a diagonal needs more travel than a cardinal direction, which is what makes a stick feel
  // square. A flight stick's two raw joystick axes are a stick by the same rule.
  const ActionBinding* x_axis = nullptr;
  const ActionBinding* y_axis = nullptr;
  bool stick = true;
  for (const ActionBinding& ab : list) {
    if (!is_analog_axis(ab.binding.source) || ab.binding.deadzone <= 0.0f || ab.component > 1) {
      stick = false;
      break;
    }
    const ActionBinding*& slot = ab.component == 0 ? x_axis : y_axis;
    if (slot != nullptr) {  // more than one binding per component: no pair to pick
      stick = false;
      break;
    }
    slot = &ab;
  }
  if (stick && x_axis != nullptr && y_axis != nullptr &&
      x_axis->binding.source == y_axis->binding.source &&
      x_axis->binding.deadzone == y_axis->binding.deadzone) {
    const Signal* sx = find_signal(x_axis->binding.source, x_axis->binding.code);
    const Signal* sy = find_signal(y_axis->binding.source, y_axis->binding.code);
    const f32 raw_x = sx != nullptr ? sx->value : 0.0f;
    const f32 raw_y = sy != nullptr ? sy->value : 0.0f;
    const f32 deadzone = x_axis->binding.deadzone;
    const f32 magnitude = std::sqrt(raw_x * raw_x + raw_y * raw_y);
    if (magnitude <= deadzone || magnitude <= 0.0f) return Vec2{};
    f32 rescaled = (magnitude - deadzone) / (1.0f - deadzone);
    if (rescaled > 1.0f) rescaled = 1.0f;
    const f32 factor = rescaled / magnitude;
    return Vec2{clamp_unit(raw_x * factor * x_axis->binding.scale),
                clamp_unit(raw_y * factor * y_axis->binding.scale)};
  }

  f32 x = 0.0f;
  f32 y = 0.0f;
  for (const ActionBinding& ab : list) {
    const f32 v = bound_value(ab.binding, false) * ab.binding.scale;
    if (ab.component == 0) {
      x += v;
    } else if (ab.component == 1) {
      y += v;
    }
  }
  return Vec2{clamp_unit(x), clamp_unit(y)};
}

u64 InputState::state_hash() const noexcept {
  u64 digest = hash_combine(k_hash_seed, tick_.value);
  if (map_ != nullptr) {
    const u32 count = map_->action_count();
    for (ActionId action = 0; action < count; ++action) {
      u64 flags = 0;
      if (pressed(action)) flags |= 1;
      if (released(action)) flags |= 2;
      if (held(action)) flags |= 4;
      digest = hash_combine(digest, flags);
      digest = hash_combine(digest, float_bits(axis(action)));
      const Vec2 v = axis2(action);
      digest = hash_combine(digest, float_bits(v.x));
      digest = hash_combine(digest, float_bits(v.y));
    }
  }
  digest = hash_combine(digest, float_bits(mouse_dx_));
  digest = hash_combine(digest, float_bits(mouse_dy_));
  return digest;
}

}  // namespace engine::input

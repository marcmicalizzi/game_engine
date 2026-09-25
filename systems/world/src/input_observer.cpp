#include <systems/world/input_observer.h>

#include <cmath>
#include <utility>

namespace engine::world {

bool InputObserver::bind(const input::ActionMap& map, const input::InputLog& log,
                         std::string_view action, f32 speed, u32 observer, std::string& error) {
  if (log.map_hash() != map.hash()) {
    error = "the input log was recorded against another action map (the log says " +
            std::to_string(log.map_hash()) + ", the map hashes " + std::to_string(map.hash()) +
            "), so its codes would mean other actions";
    return false;
  }
  const input::ActionId id = map.find_action(action);
  if (id == input::k_invalid_action || map.action_kind(id) != input::ActionKind::Axis2) {
    error = "the action map has no Axis2 action '" + std::string(action) + "' to steer by";
    return false;
  }
  if (!std::isfinite(speed) || speed < 0.0f) {
    error = "the player's speed is a finite number of metres per second, 0 or more";
    return false;
  }
  map_ = map;
  log_ = log;
  state_ = std::make_unique<input::InputState>(map_);
  action_ = map_.find_action(action);
  action_name_ = std::string(action);
  speed_ = speed;
  observer_ = observer;
  cursor_ = 0;
  fed_through_ = 0;
  fed_any_ = false;
  return true;
}

void InputObserver::unbind() noexcept {
  state_.reset();
  log_.clear();
  action_ = input::k_invalid_action;
  cursor_ = 0;
  fed_through_ = 0;
  fed_any_ = false;
}

void InputObserver::catch_up(u64 tick) {
  if (state_ == nullptr) return;
  const u64 from = fed_any_ ? fed_through_ + 1 : 0;
  if (fed_any_ && fed_through_ >= tick) return;
  // `feed_ticks` is `InputLog::replay`'s own loop, so a world that feeds a tick at a time and one
  // that catches up in one call run the same code over the same events (input.md, "The replay
  // guarantee").
  cursor_ = input::feed_ticks(*state_, log_.events(), cursor_, SimTick{from}, SimTick{tick});
  fed_through_ = tick;
  fed_any_ = true;
}

Vec3 InputObserver::step(u64 tick) {
  if (state_ == nullptr) return Vec3{};
  catch_up(tick);
  const Vec2 axis = state_->axis2(action_);
  return Vec3{axis.x * speed_, 0.0f, -axis.y * speed_};
}

void InputObserver::recorded(u64 tick, input::InputLog& out) const {
  out.clear();
  out.set_map_hash(log_.map_hash());
  out.set_session(log_.session());
  for (const input::RawEvent& event : log_.events()) {
    if (event.tick.value > tick) break;
    out.record(event);
  }
}

u64 InputObserver::state_hash() const noexcept {
  return state_ != nullptr ? state_->state_hash() : 0;
}

}  // namespace engine::world

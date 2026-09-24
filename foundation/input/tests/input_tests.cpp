#include <core/json/json.h>
#include <foundation/input/input.h>

#include <doctest/doctest.h>

#include <string>

using namespace engine;
using namespace engine::input;

namespace {

// Codes as the window module numbers them, spelled out here so a change on that side shows up
// as a failing expectation rather than as silently different bindings (docs/subsystems/input.md).
constexpr u32 k_key_w = 26;  // SDL scancodes
constexpr u32 k_key_a = 4;
constexpr u32 k_key_s = 22;
constexpr u32 k_key_d = 7;
constexpr u32 k_key_space = 44;
constexpr u32 k_mouse_left = 1;

constexpr u32 k_pad_south = 1;   // window::GamepadButton::South
constexpr u32 k_pad_left_x = 0;  // window::GamepadAxis::LeftX
constexpr u32 k_pad_left_y = 1;
constexpr u32 k_pad_left_trigger = 4;

RawEvent key_event(u64 tick, u32 code, bool down) {
  return RawEvent{SimTick{tick}, Source::Key, code, down ? 1.0f : 0.0f, 0};
}

RawEvent pad_axis(u64 tick, u32 code, f32 value) {
  return RawEvent{SimTick{tick}, Source::GamepadAxis, code, value, 0};
}

// begin_tick / feed / end_tick for one tick of events.
void run_tick(InputState& state, u64 tick, std::span<const RawEvent> events) {
  state.begin_tick(SimTick{tick});
  for (const RawEvent& e : events)
    state.feed(e);
  state.end_tick();
}

void run_empty_tick(InputState& state, u64 tick) { run_tick(state, tick, {}); }

}  // namespace

TEST_CASE("input: a key drives a button action") {
  ActionMap map;
  const ActionId fire = map.add_action("fire", ActionKind::Button);
  REQUIRE(fire != k_invalid_action);
  map.bind(fire, Binding{Source::Key, k_key_space});
  CHECK(map.find_action("fire") == fire);
  CHECK(map.find_action("nothing") == k_invalid_action);
  CHECK(map.action_name(fire) == "fire");
  CHECK(map.action_kind(fire) == ActionKind::Button);
  CHECK(map.bindings(fire).size() == 1);
  CHECK(map.is_bound(Source::Key, k_key_space));
  CHECK_FALSE(map.is_bound(Source::Key, k_key_w));
  CHECK_FALSE(map.is_bound(Source::MouseButton, k_key_space));

  InputState state(map);
  const RawEvent down = key_event(1, k_key_space, true);
  run_tick(state, 1, {&down, 1});
  CHECK(state.tick().value == 1);
  CHECK(state.pressed(fire));
  CHECK(state.held(fire));
  CHECK_FALSE(state.released(fire));

  // Held, but no longer new.
  run_empty_tick(state, 2);
  CHECK_FALSE(state.pressed(fire));
  CHECK(state.held(fire));
  CHECK_FALSE(state.released(fire));

  const RawEvent up = key_event(3, k_key_space, false);
  run_tick(state, 3, {&up, 1});
  CHECK_FALSE(state.pressed(fire));
  CHECK_FALSE(state.held(fire));
  CHECK(state.released(fire));

  // Released exactly once.
  run_empty_tick(state, 4);
  CHECK_FALSE(state.released(fire));
  CHECK_FALSE(state.held(fire));
}

TEST_CASE("input: a press and a release inside one tick are both reported") {
  ActionMap map;
  const ActionId fire = map.add_action("fire", ActionKind::Button);
  map.bind(fire, Binding{Source::Key, k_key_space});
  InputState state(map);

  const RawEvent both[] = {key_event(7, k_key_space, true), key_event(7, k_key_space, false)};
  run_tick(state, 7, both);
  CHECK(state.pressed(fire));
  CHECK(state.released(fire));
  CHECK_FALSE(state.held(fire));  // it is up again by the end of the tick

  // A key repeat while the key is down is not a second press.
  const RawEvent repeat[] = {key_event(8, k_key_space, true), key_event(8, k_key_space, true)};
  run_tick(state, 8, repeat);
  CHECK(state.pressed(fire));
  CHECK(state.held(fire));
  run_empty_tick(state, 9);
  CHECK_FALSE(state.pressed(fire));
  CHECK(state.held(fire));
}

TEST_CASE("input: two keys drive one axis at +1 and -1") {
  ActionMap map;
  const ActionId strafe = map.add_action("strafe", ActionKind::Axis);
  map.bind(strafe, Binding{Source::Key, k_key_d, 1.0f});
  map.bind(strafe, Binding{Source::Key, k_key_a, -1.0f});
  InputState state(map);

  run_empty_tick(state, 0);
  CHECK(state.axis(strafe) == doctest::Approx(0.0f));

  const RawEvent right = key_event(1, k_key_d, true);
  run_tick(state, 1, {&right, 1});
  CHECK(state.axis(strafe) == doctest::Approx(1.0f));

  // Both down cancel out rather than fighting.
  const RawEvent left = key_event(2, k_key_a, true);
  run_tick(state, 2, {&left, 1});
  CHECK(state.axis(strafe) == doctest::Approx(0.0f));

  const RawEvent right_up = key_event(3, k_key_d, false);
  run_tick(state, 3, {&right_up, 1});
  CHECK(state.axis(strafe) == doctest::Approx(-1.0f));
}

TEST_CASE("input: a gamepad axis takes its deadzone and its scale") {
  ActionMap map;
  const ActionId look = map.add_action("look", ActionKind::Axis);
  map.bind(look, Binding{Source::GamepadAxis, k_pad_left_x, 1.0f, 0.25f});
  const ActionId inverted = map.add_action("look_inverted", ActionKind::Axis);
  map.bind(inverted, Binding{Source::GamepadAxis, k_pad_left_x, -1.0f, 0.25f});
  InputState state(map);

  // The curve itself: zero inside the zone, and full travel still reaches 1.
  CHECK(apply_deadzone(0.2f, 0.25f) == doctest::Approx(0.0f));
  CHECK(apply_deadzone(0.25f, 0.25f) == doctest::Approx(0.0f));
  CHECK(apply_deadzone(1.0f, 0.25f) == doctest::Approx(1.0f));
  CHECK(apply_deadzone(-1.0f, 0.25f) == doctest::Approx(-1.0f));
  CHECK(apply_deadzone(0.5f, 0.25f) == doctest::Approx(1.0f / 3.0f));
  CHECK(apply_deadzone(0.5f, 0.0f) == doctest::Approx(0.5f));

  const RawEvent drift = pad_axis(1, k_pad_left_x, 0.2f);
  run_tick(state, 1, {&drift, 1});
  CHECK(state.axis(look) == doctest::Approx(0.0f));

  const RawEvent half = pad_axis(2, k_pad_left_x, 0.5f);
  run_tick(state, 2, {&half, 1});
  CHECK(state.axis(look) == doctest::Approx(1.0f / 3.0f));
  CHECK(state.axis(inverted) == doctest::Approx(-1.0f / 3.0f));

  const RawEvent full = pad_axis(3, k_pad_left_x, -1.0f);
  run_tick(state, 3, {&full, 1});
  CHECK(state.axis(look) == doctest::Approx(-1.0f));

  // An analog axis is a button too, once it is far enough along.
  ActionMap trigger_map;
  const ActionId shoot = trigger_map.add_action("shoot", ActionKind::Button);
  trigger_map.bind(shoot, Binding{Source::GamepadAxis, k_pad_left_trigger, 1.0f, 0.1f});
  InputState trigger_state(trigger_map);
  const RawEvent squeeze = pad_axis(1, k_pad_left_trigger, 0.3f);
  run_tick(trigger_state, 1, {&squeeze, 1});
  CHECK_FALSE(trigger_state.held(shoot));
  CHECK_FALSE(trigger_state.pressed(shoot));
  const RawEvent pull = pad_axis(2, k_pad_left_trigger, 0.9f);
  run_tick(trigger_state, 2, {&pull, 1});
  CHECK(trigger_state.held(shoot));
  CHECK(trigger_state.pressed(shoot));
  run_empty_tick(trigger_state, 3);
  CHECK(trigger_state.held(shoot));
  CHECK_FALSE(trigger_state.pressed(shoot));
  const RawEvent release = pad_axis(4, k_pad_left_trigger, 0.0f);
  run_tick(trigger_state, 4, {&release, 1});
  CHECK(trigger_state.released(shoot));
  CHECK_FALSE(trigger_state.held(shoot));
}

// A racing rig as the window module hands it over: a wheel on joystick slot 0 with steering on
// axis 0 and two pedals on axes 1 and 2, a hat on the rim, and a gear shifter on slot 1 whose
// gates are plain buttons. Nothing here is named by the device; the binding table is what turns
// "axis 2 of device 0" into "brake".
TEST_CASE("input: a wheel, its pedals, its hat, and a shifter") {
  constexpr u32 k_wheel = 0;    // joystick slots
  constexpr u32 k_shifter = 1;  //
  constexpr u32 k_steer = 0;    // axis indices on the wheel
  constexpr u32 k_throttle = 1;
  constexpr u32 k_brake = 2;

  ActionMap map;
  const ActionId steer = map.add_action("steer", ActionKind::Axis);
  map.bind(steer, Binding{Source::JoystickAxis, k_steer, 1.0f, 0.05f});
  // Two pedals on one axis action: the throttle pushes it positive, the brake negative. A real
  // pedal set rests at one end of its travel, which is the scale's business, not the module's.
  const ActionId drive = map.add_action("drive", ActionKind::Axis);
  map.bind(drive, Binding{Source::JoystickAxis, k_throttle, 1.0f, 0.02f});
  map.bind(drive, Binding{Source::JoystickAxis, k_brake, -1.0f, 0.02f});
  // A hat direction is a button: code = hat 0, direction Up.
  const ActionId look_back = map.add_action("look_back", ActionKind::Button);
  map.bind(look_back, Binding{Source::JoystickHat, hat_code(0, k_hat_up)});
  // Shifter gates.
  const ActionId first = map.add_action("gear_1", ActionKind::Button);
  map.bind(first, Binding{Source::JoystickButton, 0});
  const ActionId second = map.add_action("gear_2", ActionKind::Button);
  map.bind(second, Binding{Source::JoystickButton, 1});

  CHECK(hat_code(0, k_hat_up) == 1);
  CHECK(hat_code(2, k_hat_left) == 520);
  CHECK(hat_index_of(hat_code(2, k_hat_left)) == 2);
  CHECK(hat_direction_of(hat_code(2, k_hat_left)) == k_hat_left);

  InputState state(map);

  // The wheel a quarter turn right, the throttle half down, no brake.
  const RawEvent turn[] = {
      RawEvent{SimTick{1}, Source::JoystickAxis, k_steer, 0.25f, k_wheel},
      RawEvent{SimTick{1}, Source::JoystickAxis, k_throttle, 0.5f, k_wheel},
  };
  run_tick(state, 1, turn);
  // (0.25 - 0.05) / 0.95 and (0.5 - 0.02) / 0.98.
  CHECK(state.axis(steer) == doctest::Approx(0.2f / 0.95f));
  CHECK(state.axis(drive) == doctest::Approx(0.48f / 0.98f));

  // Off the throttle and hard on the brake: the same action swings negative.
  const RawEvent brake[] = {
      RawEvent{SimTick{2}, Source::JoystickAxis, k_throttle, 0.0f, k_wheel},
      RawEvent{SimTick{2}, Source::JoystickAxis, k_brake, 1.0f, k_wheel},
  };
  run_tick(state, 2, brake);
  CHECK(state.axis(drive) == doctest::Approx(-1.0f));
  // Wheel drift inside the deadzone is nothing at all.
  const RawEvent drift{SimTick{3}, Source::JoystickAxis, k_steer, 0.04f, k_wheel};
  run_tick(state, 3, {&drift, 1});
  CHECK(state.axis(steer) == doctest::Approx(0.0f));

  // The hat: all four directions are fed every time it moves, and only the one that changed
  // reports an edge.
  auto hat_tick = [&](u64 tick, u32 mask) {
    const u32 directions[4] = {k_hat_up, k_hat_right, k_hat_down, k_hat_left};
    RawEvent events[4];
    for (u32 i = 0; i < 4; ++i) {
      events[i] = RawEvent{SimTick{tick}, Source::JoystickHat, hat_code(0, directions[i]),
                           (mask & directions[i]) != 0 ? 1.0f : 0.0f, k_wheel};
    }
    run_tick(state, tick, events);
  };
  hat_tick(4, k_hat_up);
  CHECK(state.pressed(look_back));
  CHECK(state.held(look_back));
  hat_tick(5, k_hat_up | k_hat_right);  // to a diagonal: Up stays down, no second press
  CHECK_FALSE(state.pressed(look_back));
  CHECK(state.held(look_back));
  hat_tick(6, 0);
  CHECK(state.released(look_back));
  CHECK_FALSE(state.held(look_back));

  // The shifter: two gates on a second device, one down at a time.
  const RawEvent into_first{SimTick{7}, Source::JoystickButton, 0, 1.0f, k_shifter};
  run_tick(state, 7, {&into_first, 1});
  CHECK(state.pressed(first));
  CHECK_FALSE(state.held(second));
  const RawEvent shift[] = {
      RawEvent{SimTick{8}, Source::JoystickButton, 0, 0.0f, k_shifter},
      RawEvent{SimTick{8}, Source::JoystickButton, 1, 1.0f, k_shifter},
  };
  run_tick(state, 8, shift);
  CHECK(state.released(first));
  CHECK_FALSE(state.held(first));
  CHECK(state.pressed(second));
  CHECK(state.held(second));
}

// The joystick slot space and the gamepad slot space overlap numerically: device 0 means two
// different things by source, and a filter on one does not silence the other.
TEST_CASE("input: a joystick axis and a gamepad axis on the same slot number are separate") {
  ActionMap map;
  const ActionId wheel = map.add_action("wheel", ActionKind::Axis);
  map.bind(wheel, Binding{Source::JoystickAxis, 0});
  const ActionId stick = map.add_action("stick", ActionKind::Axis);
  map.bind(stick, Binding{Source::GamepadAxis, k_pad_left_x});
  InputState state(map);

  const RawEvent both[] = {
      RawEvent{SimTick{1}, Source::JoystickAxis, 0, 0.5f, 0},
      RawEvent{SimTick{1}, Source::GamepadAxis, k_pad_left_x, -0.5f, 0},
  };
  run_tick(state, 1, both);
  CHECK(state.axis(wheel) == doctest::Approx(0.5f));
  CHECK(state.axis(stick) == doctest::Approx(-0.5f));
}

TEST_CASE("input: a flight stick's two axes get the radial deadzone a gamepad stick gets") {
  ActionMap map;
  const ActionId fly = map.add_action("fly", ActionKind::Axis2);
  map.bind(fly, Binding{Source::JoystickAxis, 0, 1.0f, 0.2f}, 0);
  map.bind(fly, Binding{Source::JoystickAxis, 1, 1.0f, 0.2f}, 1);
  InputState state(map);

  // Each component is under the deadzone; the radius is not, so the stick still moves.
  const RawEvent nudge[] = {
      RawEvent{SimTick{1}, Source::JoystickAxis, 0, 0.18f, 0},
      RawEvent{SimTick{1}, Source::JoystickAxis, 1, 0.18f, 0},
  };
  run_tick(state, 1, nudge);
  const Vec2 v = state.axis2(fly);
  CHECK(v.x > 0.0f);
  CHECK(v.x == doctest::Approx(v.y));

  // Mixing sources across the components is not a stick: each axis takes its own deadzone.
  ActionMap mixed;
  const ActionId half = mixed.add_action("half", ActionKind::Axis2);
  mixed.bind(half, Binding{Source::JoystickAxis, 0, 1.0f, 0.2f}, 0);
  mixed.bind(half, Binding{Source::GamepadAxis, k_pad_left_x, 1.0f, 0.2f}, 1);
  InputState mixed_state(mixed);
  const RawEvent pair[] = {
      RawEvent{SimTick{1}, Source::JoystickAxis, 0, 0.18f, 0},
      RawEvent{SimTick{1}, Source::GamepadAxis, k_pad_left_x, 0.18f, 0},
  };
  run_tick(mixed_state, 1, pair);
  CHECK(mixed_state.axis2(half).x == doctest::Approx(0.0f));
  CHECK(mixed_state.axis2(half).y == doctest::Approx(0.0f));
}

TEST_CASE("input: axis2 from four keys and from a stick") {
  ActionMap map;
  const ActionId move = map.add_action("move", ActionKind::Axis2);
  map.bind(move, Binding{Source::Key, k_key_d, 1.0f}, 0);
  map.bind(move, Binding{Source::Key, k_key_a, -1.0f}, 0);
  map.bind(move, Binding{Source::Key, k_key_w, 1.0f}, 1);
  map.bind(move, Binding{Source::Key, k_key_s, -1.0f}, 1);
  InputState state(map);

  const RawEvent forward_right[] = {key_event(1, k_key_w, true), key_event(1, k_key_d, true)};
  run_tick(state, 1, forward_right);
  CHECK(state.axis2(move).x == doctest::Approx(1.0f));
  CHECK(state.axis2(move).y == doctest::Approx(1.0f));

  const RawEvent back = key_event(2, k_key_s, true);
  run_tick(state, 2, {&back, 1});
  CHECK(state.axis2(move).y == doctest::Approx(0.0f));

  // The same action from a stick, with a radial deadzone over the pair.
  ActionMap stick_map;
  const ActionId walk = stick_map.add_action("walk", ActionKind::Axis2);
  stick_map.bind(walk, Binding{Source::GamepadAxis, k_pad_left_x, 1.0f, 0.2f}, 0);
  stick_map.bind(walk, Binding{Source::GamepadAxis, k_pad_left_y, -1.0f, 0.2f}, 1);
  InputState stick(stick_map);

  // Inside the disc: resting drift on both axes reads as no movement at all.
  const RawEvent drift[] = {pad_axis(1, k_pad_left_x, 0.12f), pad_axis(1, k_pad_left_y, -0.12f)};
  run_tick(stick, 1, drift);
  CHECK(stick.axis2(walk).x == doctest::Approx(0.0f));
  CHECK(stick.axis2(walk).y == doctest::Approx(0.0f));

  // Just outside it: a diagonal nudge whose components are each under the deadzone still moves
  // the player, which is the whole point of taking the zone radially instead of per axis.
  const RawEvent nudge[] = {pad_axis(2, k_pad_left_x, 0.15f), pad_axis(2, k_pad_left_y, -0.15f)};
  run_tick(stick, 2, nudge);
  CHECK(stick.axis2(walk).x > 0.0f);
  CHECK(stick.axis2(walk).x < 0.05f);
  CHECK(stick.axis2(walk).y == doctest::Approx(stick.axis2(walk).x));

  // Straight right at full travel: the curve reaches 1, and y stays put.
  const RawEvent hard_right[] = {pad_axis(3, k_pad_left_x, 1.0f), pad_axis(3, k_pad_left_y, 0.0f)};
  run_tick(stick, 3, hard_right);
  CHECK(stick.axis2(walk).x == doctest::Approx(1.0f));
  CHECK(stick.axis2(walk).y == doctest::Approx(0.0f));

  // A diagonal keeps its direction and does not exceed the unit disc; the y binding's -1 scale
  // turns SDL's y-down stick into a y-up movement vector.
  const RawEvent diagonal[] = {pad_axis(4, k_pad_left_x, 1.0f), pad_axis(4, k_pad_left_y, -1.0f)};
  run_tick(stick, 4, diagonal);
  const Vec2 v = stick.axis2(walk);
  CHECK(v.x == doctest::Approx(v.y));
  CHECK(v.x > 0.7f);
  CHECK(v.y > 0.7f);
  CHECK(v.x <= 1.0f);
}

TEST_CASE("input: mouse motion and unbound codes") {
  ActionMap map;
  const ActionId fire = map.add_action("fire", ActionKind::Button);
  map.bind(fire, Binding{Source::MouseButton, k_mouse_left});
  const ActionId turn = map.add_action("turn", ActionKind::Axis);
  map.bind(turn, Binding{Source::MouseAxis, static_cast<u32>(MouseAxisCode::X), 0.01f});
  InputState state(map);

  const RawEvent moves[] = {
      RawEvent{SimTick{1}, Source::MouseAxis, static_cast<u32>(MouseAxisCode::X), 20.0f, 0},
      RawEvent{SimTick{1}, Source::MouseAxis, static_cast<u32>(MouseAxisCode::X), 10.0f, 0},
      RawEvent{SimTick{1}, Source::MouseAxis, static_cast<u32>(MouseAxisCode::Y), -4.0f, 0},
      RawEvent{SimTick{1}, Source::MouseButton, k_mouse_left, 1.0f, 0},
      // Nothing is bound to these; they must leave no trace.
      key_event(1, k_key_w, true),
      RawEvent{SimTick{1}, Source::MouseButton, 3, 1.0f, 0},
      RawEvent{SimTick{1}, Source::GamepadButton, k_pad_south, 1.0f, 0},
  };
  run_tick(state, 1, moves);
  CHECK(state.pressed(fire));
  CHECK(state.axis(turn) == doctest::Approx(0.30f));
  // The deltas of a tick add up, and the pointer is read even where nothing binds it.
  CHECK(state.mouse_delta().x == doctest::Approx(30.0f));
  CHECK(state.mouse_delta().y == doctest::Approx(-4.0f));

  // Per-tick accumulators start at rest again.
  run_empty_tick(state, 2);
  CHECK(state.mouse_delta().x == doctest::Approx(0.0f));
  CHECK(state.axis(turn) == doctest::Approx(0.0f));
  CHECK(state.held(fire));  // a button is a position, not a delta

  // An unbound key never became a signal, so binding it later starts from rest.
  ActionMap other;
  const ActionId walk = other.add_action("walk", ActionKind::Button);
  other.bind(walk, Binding{Source::Key, k_key_w});
  InputState fresh(other);
  run_empty_tick(fresh, 1);
  CHECK_FALSE(fresh.held(walk));
}

// A mouse says how far, a stick how fast. `delta2` is the first: the tick's pointer motion through
// the bindings' scales, never clamped, and blind to the positions `axis2` reads — so one action can
// carry a mouse binding and a stick binding and a camera reads each the way it means.
TEST_CASE("input: delta2 is the tick's pointer motion, unclamped, and ignores positions") {
  ActionMap map;
  const ActionId look = map.add_action("look", ActionKind::Axis2);
  map.bind(look, Binding{Source::MouseAxis, static_cast<u32>(MouseAxisCode::X), 1.0f}, 0);
  // Window y grows downwards; a -1 scale makes "up" positive, and is how a player inverts it.
  map.bind(look, Binding{Source::MouseAxis, static_cast<u32>(MouseAxisCode::Y), -1.0f}, 1);
  map.bind(look, Binding{Source::GamepadAxis, k_pad_left_x, 1.0f, 0.1f}, 0);
  InputState state(map);

  const RawEvent tick1[] = {
      RawEvent{SimTick{1}, Source::MouseAxis, static_cast<u32>(MouseAxisCode::X), 250.0f, 0},
      RawEvent{SimTick{1}, Source::MouseAxis, static_cast<u32>(MouseAxisCode::X), 150.0f, 0},
      RawEvent{SimTick{1}, Source::MouseAxis, static_cast<u32>(MouseAxisCode::Y), 30.0f, 0},
      pad_axis(1, k_pad_left_x, 0.8f),
  };
  run_tick(state, 1, tick1);
  // 400 pixels is 400, not the 1 `axis2` would clamp it to, and the stick is not in it.
  CHECK(state.delta2(look).x == 400.0f);
  CHECK(state.delta2(look).y == -30.0f);
  // axis2 is unchanged by the mouse binding's existence: it sums everything and clamps.
  CHECK(state.axis2(look).x == doctest::Approx(1.0f));

  // A tick with no motion reads zero, while the stick, a position, stays where it was.
  run_empty_tick(state, 2);
  CHECK(state.delta2(look).x == 0.0f);
  CHECK(state.delta2(look).y == 0.0f);
  CHECK(state.axis2(look).x == doctest::Approx((0.8f - 0.1f) / 0.9f));

  // An action with no mouse binding, and a state with no map, read zero.
  ActionMap keys;
  const ActionId walk = keys.add_action("walk", ActionKind::Axis2);
  keys.bind(walk, Binding{Source::Key, k_key_w, 1.0f}, 1);
  InputState key_state(keys);
  const RawEvent w[] = {key_event(1, k_key_w, true)};
  run_tick(key_state, 1, w);
  CHECK(key_state.delta2(walk).y == 0.0f);
  CHECK(key_state.axis2(walk).y == doctest::Approx(1.0f));
  InputState none;
  CHECK(none.delta2(0).x == 0.0f);
}

TEST_CASE("input: an axis sum is clamped to the unit range") {
  ActionMap map;
  const ActionId push = map.add_action("push", ActionKind::Axis);
  map.bind(push, Binding{Source::Key, k_key_w, 1.0f});
  map.bind(push, Binding{Source::Key, k_key_d, 1.0f});
  InputState state(map);
  const RawEvent both[] = {key_event(1, k_key_w, true), key_event(1, k_key_d, true)};
  run_tick(state, 1, both);
  CHECK(state.axis(push) == doctest::Approx(1.0f));
}

TEST_CASE("input: a device filter splits one event stream between two pads") {
  ActionMap map;
  const ActionId jump = map.add_action("jump", ActionKind::Button);
  map.bind(jump, Binding{Source::GamepadButton, k_pad_south});

  InputState player_one(map);
  InputState player_two(map);
  player_one.set_device_filter(0);
  player_two.set_device_filter(1);
  CHECK(player_two.device_filter() == 1);

  const RawEvent press{SimTick{1}, Source::GamepadButton, k_pad_south, 1.0f, 1};
  run_tick(player_one, 1, {&press, 1});
  run_tick(player_two, 1, {&press, 1});
  CHECK_FALSE(player_one.pressed(jump));
  CHECK(player_two.pressed(jump));
}

TEST_CASE("input: an ActionMap round-trips through JSON") {
  ActionMap map;
  const ActionId fire = map.add_action("fire", ActionKind::Button);
  map.bind(fire, Binding{Source::Key, k_key_space});
  map.bind(fire, Binding{Source::GamepadButton, k_pad_south});
  const ActionId move = map.add_action("move", ActionKind::Axis2);
  map.bind(move, Binding{Source::Key, k_key_d, 1.0f}, 0);
  map.bind(move, Binding{Source::Key, k_key_a, -1.0f}, 0);
  map.bind(move, Binding{Source::GamepadAxis, k_pad_left_x, 1.0f, 0.2f}, 0);
  map.bind(move, Binding{Source::GamepadAxis, k_pad_left_y, -1.0f, 0.2f}, 1);
  const ActionId turn = map.add_action("turn", ActionKind::Axis);
  map.bind(turn, Binding{Source::MouseAxis, static_cast<u32>(MouseAxisCode::X), 0.01f});

  // The same map built twice describes and hashes identically; adding a binding changes both.
  const std::string described = map.describe();
  const u64 fingerprint = map.hash();

  const std::string text = write_json(map.to_json());
  JsonValue parsed;
  REQUIRE(parse_json(text, parsed).ok);

  ActionMap loaded;
  std::string error;
  REQUIRE_MESSAGE(loaded.from_json(parsed, &error), error);
  CHECK(loaded.describe() == described);
  CHECK(loaded.hash() == fingerprint);
  CHECK(loaded.action_count() == map.action_count());
  CHECK(loaded.find_action("move") == move);
  CHECK(loaded.action_kind(loaded.find_action("move")) == ActionKind::Axis2);
  CHECK(loaded.bindings(loaded.find_action("move")).size() == 4);
  CHECK(loaded.bindings(loaded.find_action("move"))[3].component == 1);
  CHECK(loaded.bindings(loaded.find_action("move"))[3].binding.scale == doctest::Approx(-1.0f));
  CHECK(loaded.bindings(loaded.find_action("move"))[3].binding.deadzone == doctest::Approx(0.2f));
  CHECK(loaded.is_bound(Source::GamepadAxis, k_pad_left_y));

  // Canonical: writing the loaded map again gives the same bytes.
  CHECK(write_json(loaded.to_json()) == text);

  // A rebind changes the hash, which is what makes a stale replay detectable.
  ActionMap rebound;
  REQUIRE(rebound.from_json(parsed, &error));
  rebound.clear_bindings(rebound.find_action("fire"));
  rebound.bind(rebound.find_action("fire"), Binding{Source::Key, k_key_w});
  CHECK(rebound.hash() != fingerprint);
  CHECK_FALSE(rebound.is_bound(Source::Key, k_key_space));
  CHECK(rebound.is_bound(Source::Key, k_key_w));
}

TEST_CASE("input: a malformed action map is refused with a message") {
  const char* bad[] = {
      "[]",
      "{}",
      R"({"actions": 3})",
      R"({"actions": [{"kind": "button"}]})",
      R"({"actions": [{"name": "fire", "kind": "wiggle"}]})",
      R"({"actions": [{"name": "fire", "kind": "button", "bindings": 2}]})",
      R"({"actions": [{"name": "fire", "kind": "button", "bindings": [{"code": 1}]}]})",
      R"({"actions": [{"name": "fire", "kind": "button", "bindings": [{"source": "hand"}]}]})",
      R"({"actions": [{"name": "fire", "kind": "button", "bindings": [{"source": "key"}]}]})",
      R"({"actions": [{"name": "a", "kind": "button"}, {"name": "a", "kind": "axis"}]})",
  };
  for (const char* text : bad) {
    JsonValue value;
    REQUIRE_MESSAGE(parse_json(text, value).ok, text);
    ActionMap map;
    std::string error;
    CHECK_MESSAGE(!map.from_json(value, &error), text);
    CHECK_MESSAGE(!error.empty(), text);
    CHECK(map.action_count() == 0);
  }
}

TEST_CASE("input: an action map rejects a repeat name of another kind and an empty name") {
  ActionMap map;
  const ActionId fire = map.add_action("fire", ActionKind::Button);
  CHECK(map.add_action("fire", ActionKind::Button) == fire);
  CHECK(map.add_action("fire", ActionKind::Axis) == k_invalid_action);
  CHECK(map.add_action("", ActionKind::Button) == k_invalid_action);
  CHECK(map.action_count() == 1);
  CHECK_FALSE(map.valid(k_invalid_action));
  CHECK(map.action_name(k_invalid_action).empty());
  CHECK(map.bindings(k_invalid_action).empty());

  // A state with no map answers every query with rest.
  InputState empty;
  empty.begin_tick(SimTick{1});
  empty.feed(key_event(1, k_key_space, true));
  empty.end_tick();
  CHECK(empty.map() == nullptr);
  CHECK_FALSE(empty.pressed(fire));
  CHECK(empty.axis(fire) == doctest::Approx(0.0f));
  CHECK(empty.axis2(fire).x == doctest::Approx(0.0f));

  empty.set_map(map);
  CHECK(empty.map() == &map);
}

TEST_CASE("input: source and kind names round-trip") {
  for (u32 i = 0; i < k_source_count; ++i) {
    const Source source = static_cast<Source>(i);
    Source parsed = Source::MouseAxis;
    REQUIRE(source_from_name(source_name(source), parsed));
    CHECK(parsed == source);
  }
  Source unknown = Source::Key;
  CHECK_FALSE(source_from_name("thumb", unknown));
  CHECK(is_digital(Source::Key));
  CHECK(is_digital(Source::MouseButton));
  CHECK(is_digital(Source::GamepadButton));
  CHECK(is_digital(Source::JoystickButton));
  CHECK(is_digital(Source::JoystickHat));
  CHECK_FALSE(is_digital(Source::MouseAxis));
  CHECK_FALSE(is_digital(Source::GamepadAxis));
  CHECK_FALSE(is_digital(Source::JoystickAxis));
  CHECK(is_analog_axis(Source::GamepadAxis));
  CHECK(is_analog_axis(Source::JoystickAxis));
  CHECK_FALSE(is_analog_axis(Source::MouseAxis));

  // The enumerators are log format: their numbering is pinned, and the new ones are appended.
  CHECK(static_cast<u32>(Source::Key) == 0);
  CHECK(static_cast<u32>(Source::GamepadAxis) == 4);
  CHECK(static_cast<u32>(Source::JoystickAxis) == 5);
  CHECK(static_cast<u32>(Source::JoystickButton) == 6);
  CHECK(static_cast<u32>(Source::JoystickHat) == 7);
  CHECK(k_source_count == 8);
  CHECK(std::string(source_name(Source::JoystickHat)) == "joystick_hat");

  const ActionKind kinds[] = {ActionKind::Button, ActionKind::Axis, ActionKind::Axis2};
  for (ActionKind kind : kinds) {
    ActionKind parsed = ActionKind::Axis2;
    REQUIRE(action_kind_from_name(action_kind_name(kind), parsed));
    CHECK(parsed == kind);
  }
  ActionKind bad = ActionKind::Button;
  CHECK_FALSE(action_kind_from_name("toggle", bad));
}

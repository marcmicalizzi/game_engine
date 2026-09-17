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
  CHECK_FALSE(is_digital(Source::MouseAxis));
  CHECK_FALSE(is_digital(Source::GamepadAxis));

  const ActionKind kinds[] = {ActionKind::Button, ActionKind::Axis, ActionKind::Axis2};
  for (ActionKind kind : kinds) {
    ActionKind parsed = ActionKind::Axis2;
    REQUIRE(action_kind_from_name(action_kind_name(kind), parsed));
    CHECK(parsed == kind);
  }
  ActionKind bad = ActionKind::Button;
  CHECK_FALSE(action_kind_from_name("toggle", bad));
}

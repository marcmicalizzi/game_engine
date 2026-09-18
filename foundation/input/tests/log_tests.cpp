#include <core/containers/vector.h>
#include <core/json/json.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;
using namespace engine::input;

// One unguessable scratch directory per object, so a second copy of this binary cannot delete
// this one's logs while it replays them (tests/support/test_temp_dir.h).
using TempDir = test::TempDir;

namespace {

constexpr u32 k_key_w = 26;
constexpr u32 k_key_a = 4;
constexpr u32 k_key_d = 7;
constexpr u32 k_key_space = 44;
constexpr u32 k_pad_left_x = 0;
constexpr u32 k_pad_left_y = 1;

// The map a "game" ships: a button, a stick, and a mouse axis, so a replay exercises every
// query rather than only the digital path.
ActionMap make_map() {
  ActionMap map;
  const ActionId fire = map.add_action("fire", ActionKind::Button);
  map.bind(fire, Binding{Source::Key, k_key_space});
  const ActionId move = map.add_action("move", ActionKind::Axis2);
  map.bind(move, Binding{Source::Key, k_key_d, 1.0f}, 0);
  map.bind(move, Binding{Source::Key, k_key_a, -1.0f}, 0);
  map.bind(move, Binding{Source::GamepadAxis, k_pad_left_x, 1.0f, 0.2f}, 0);
  map.bind(move, Binding{Source::GamepadAxis, k_pad_left_y, -1.0f, 0.2f}, 1);
  const ActionId turn = map.add_action("turn", ActionKind::Axis);
  map.bind(turn, Binding{Source::MouseAxis, static_cast<u32>(MouseAxisCode::X), 0.01f});
  const ActionId forward = map.add_action("forward", ActionKind::Button);
  map.bind(forward, Binding{Source::Key, k_key_w});
  return map;
}

// A scripted session: presses, an axis sweep, mouse motion, and an unbound key, over 9 ticks.
Vector<RawEvent> scripted_events() {
  Vector<RawEvent> events;
  events.push_back(RawEvent{SimTick{1}, Source::Key, k_key_space, 1.0f, 0});
  events.push_back(RawEvent{SimTick{1}, Source::Key, k_key_d, 1.0f, 0});
  events.push_back(
      RawEvent{SimTick{2}, Source::MouseAxis, static_cast<u32>(MouseAxisCode::X), 12.5f, 0});
  events.push_back(RawEvent{SimTick{2}, Source::Key, k_key_space, 0.0f, 0});
  events.push_back(RawEvent{SimTick{3}, Source::GamepadAxis, k_pad_left_x, 0.75f, 1});
  events.push_back(RawEvent{SimTick{3}, Source::GamepadAxis, k_pad_left_y, -0.5f, 1});
  events.push_back(RawEvent{SimTick{4}, Source::Key, k_key_d, 0.0f, 0});
  events.push_back(RawEvent{SimTick{4}, Source::Key, k_key_a, 1.0f, 0});
  // Nothing binds F1 (scancode 58); it must not change a single digest.
  events.push_back(RawEvent{SimTick{5}, Source::Key, 58, 1.0f, 0});
  events.push_back(RawEvent{SimTick{6}, Source::Key, k_key_w, 1.0f, 0});
  events.push_back(RawEvent{SimTick{6}, Source::Key, k_key_w, 0.0f, 0});
  events.push_back(RawEvent{SimTick{8}, Source::GamepadAxis, k_pad_left_x, 0.1f, 1});
  events.push_back(
      RawEvent{SimTick{8}, Source::MouseAxis, static_cast<u32>(MouseAxisCode::Y), -3.0f, 0});
  return events;
}

struct Recorder {
  Vector<u64> digests;
};

void collect(void* user, SimTick tick, const InputState& state) {
  Recorder* recorder = static_cast<Recorder*>(user);
  CHECK(state.tick().value == tick.value);
  recorder->digests.push_back(state.state_hash());
}

}  // namespace

TEST_CASE("input log: a scripted session replays to the same state on every tick") {
  const ActionMap map = make_map();
  const Vector<RawEvent> events = scripted_events();

  // The live run: feed the state and record at the same time, as a game would.
  InputState live(map);
  InputLog log;
  log.set_map(map);
  CHECK(log.empty());

  Vector<u64> expected;
  u32 cursor = 0;
  for (u64 t = 0; t <= 9; ++t) {
    live.begin_tick(SimTick{t});
    while (cursor < events.size() && events[cursor].tick.value == t) {
      live.feed(events[cursor]);
      log.record(events[cursor]);
      ++cursor;
    }
    live.end_tick();
    expected.push_back(live.state_hash());
  }
  CHECK(cursor == events.size());
  CHECK(log.size() == events.size());
  CHECK(log.first_tick().value == 1);
  CHECK(log.last_tick().value == 8);

  // The session had something to say on most of its ticks.
  u32 distinct = 0;
  for (u32 i = 1; i < expected.size(); ++i) {
    if (expected[i] != expected[i - 1]) ++distinct;
  }
  CHECK(distinct >= 5);

  // A fresh state fed from the log reproduces every tick's queries.
  InputState replayed(map);
  Recorder recorder;
  std::string error;
  REQUIRE_MESSAGE(log.replay(replayed, SimTick{0}, SimTick{9}, {&collect, &recorder}, &error),
                  error);
  REQUIRE(recorder.digests.size() == expected.size());
  for (u32 i = 0; i < expected.size(); ++i) {
    CHECK_MESSAGE(recorder.digests[i] == expected[i], "tick " << i);
  }
  CHECK(replayed.state_hash() == live.state_hash());
}

TEST_CASE("input log: saved as JSON lines and loaded back unchanged") {
  const TempDir tmp("engine_input_log");
  const ActionMap map = make_map();
  const Vector<RawEvent> events = scripted_events();

  InputLog log;
  log.set_map(map);
  for (const RawEvent& e : events)
    log.record(e);

  const std::string path = io::join_path(tmp.path(), "run.jsonl");
  REQUIRE(log.save(path) == io::Status::Ok);

  // The file is a header line and one line per event, tick first, and every line is JSON.
  std::string text;
  REQUIRE(io::read_file(path, text) == io::Status::Ok);
  Vector<std::string> lines;
  for (usize begin = 0; begin < text.size();) {
    const usize end = text.find('\n', begin);
    const usize stop = end == std::string::npos ? text.size() : end;
    lines.push_back(text.substr(begin, stop - begin));
    begin = stop + 1;
  }
  REQUIRE(lines.size() == events.size() + 1);
  JsonValue header;
  REQUIRE(parse_json(lines[0], header).ok);
  REQUIRE(header.find("type") != nullptr);
  CHECK(header.find("type")->as_string() == k_log_type);
  u64 number = 0;
  REQUIRE(header.find("version") != nullptr);
  REQUIRE(header.find("version")->get_u64(number));
  CHECK(number == k_log_version);
  REQUIRE(header.find("map") != nullptr);
  REQUIRE(header.find("map")->get_u64(number));
  CHECK(number == map.hash());
  REQUIRE(header.find("columns") != nullptr);
  CHECK(header.find("columns")->size() == 5);
  JsonValue first;
  REQUIRE(parse_json(lines[1], first).ok);
  REQUIRE(first.is_array());
  REQUIRE(first[0].get_u64(number));
  CHECK(number == 1);  // the tick comes first
  CHECK(first[1].as_string() == "key");
  REQUIRE(first[2].get_u64(number));
  CHECK(number == k_key_space);

  InputLog loaded;
  std::string error;
  REQUIRE_MESSAGE(loaded.load(path, &error) == io::Status::Ok, error);
  CHECK(loaded.map_hash() == log.map_hash());
  REQUIRE(loaded.size() == log.size());
  for (u32 i = 0; i < loaded.size(); ++i) {
    CHECK(loaded.events()[i].tick.value == events[i].tick.value);
    CHECK(loaded.events()[i].source == events[i].source);
    CHECK(loaded.events()[i].code == events[i].code);
    CHECK(loaded.events()[i].value == doctest::Approx(events[i].value));
    CHECK(loaded.events()[i].device == events[i].device);
  }

  // Saving what was loaded gives byte-identical text: the format is canonical.
  const std::string second_path = io::join_path(tmp.path(), "again.jsonl");
  REQUIRE(loaded.save(second_path) == io::Status::Ok);
  std::string second_text;
  REQUIRE(io::read_file(second_path, second_text) == io::Status::Ok);
  CHECK(second_text == text);

  // And the loaded log replays to the same digests as the log it was written from.
  InputState from_memory(map);
  InputState from_file(map);
  Recorder memory_digests;
  Recorder file_digests;
  REQUIRE(log.replay(from_memory, SimTick{0}, SimTick{9}, {&collect, &memory_digests}, &error));
  REQUIRE(loaded.replay(from_file, SimTick{0}, SimTick{9}, {&collect, &file_digests}, &error));
  REQUIRE(memory_digests.digests.size() == file_digests.digests.size());
  for (u32 i = 0; i < memory_digests.digests.size(); ++i) {
    CHECK_MESSAGE(memory_digests.digests[i] == file_digests.digests[i], "tick " << i);
  }
}

// The same round trip over the raw-joystick sources, which are the log format's newest members:
// a wheel axis, a pedal, a shifter button on a second device, and a hat direction.
TEST_CASE("input log: a wheel, a pedal, a shifter, and a hat round-trip and replay") {
  const TempDir tmp("engine_input_log_joystick");

  ActionMap map;
  const ActionId steer = map.add_action("steer", ActionKind::Axis);
  map.bind(steer, Binding{Source::JoystickAxis, 0, 1.0f, 0.05f});
  const ActionId throttle = map.add_action("throttle", ActionKind::Axis);
  map.bind(throttle, Binding{Source::JoystickAxis, 1, 1.0f});
  const ActionId gear = map.add_action("gear_1", ActionKind::Button);
  map.bind(gear, Binding{Source::JoystickButton, 0});
  const ActionId look = map.add_action("look_left", ActionKind::Button);
  map.bind(look, Binding{Source::JoystickHat, hat_code(0, k_hat_left)});

  Vector<RawEvent> events;
  events.push_back(RawEvent{SimTick{1}, Source::JoystickAxis, 0, -0.75f, 0});
  events.push_back(RawEvent{SimTick{1}, Source::JoystickAxis, 1, 0.5f, 0});
  events.push_back(RawEvent{SimTick{2}, Source::JoystickHat, hat_code(0, k_hat_left), 1.0f, 0});
  events.push_back(RawEvent{SimTick{2}, Source::JoystickButton, 0, 1.0f, 1});
  events.push_back(RawEvent{SimTick{3}, Source::JoystickHat, hat_code(0, k_hat_left), 0.0f, 0});
  events.push_back(RawEvent{SimTick{4}, Source::JoystickButton, 0, 0.0f, 1});
  events.push_back(RawEvent{SimTick{4}, Source::JoystickAxis, 0, 0.0f, 0});

  InputLog log;
  log.set_map(map);
  InputState live(map);
  Vector<u64> expected;
  u32 cursor = 0;
  for (u64 t = 0; t <= 5; ++t) {
    live.begin_tick(SimTick{t});
    while (cursor < events.size() && events[cursor].tick.value == t) {
      live.feed(events[cursor]);
      log.record(events[cursor]);
      ++cursor;
    }
    live.end_tick();
    expected.push_back(live.state_hash());
  }
  CHECK(cursor == events.size());

  const std::string path = io::join_path(tmp.path(), "wheel.jsonl");
  REQUIRE(log.save(path) == io::Status::Ok);
  std::string text;
  REQUIRE(io::read_file(path, text) == io::Status::Ok);
  // The source names are what a hand-written or third-party log has to say.
  CHECK(text.find("\"joystick_axis\"") != std::string::npos);
  CHECK(text.find("\"joystick_button\"") != std::string::npos);
  CHECK(text.find("\"joystick_hat\"") != std::string::npos);

  InputLog loaded;
  std::string error;
  REQUIRE_MESSAGE(loaded.load(path, &error) == io::Status::Ok, error);
  REQUIRE(loaded.size() == events.size());
  for (u32 i = 0; i < loaded.size(); ++i) {
    CHECK(loaded.events()[i].source == events[i].source);
    CHECK(loaded.events()[i].code == events[i].code);
    CHECK(loaded.events()[i].device == events[i].device);
    CHECK(loaded.events()[i].value == doctest::Approx(events[i].value));
  }

  InputState replayed(map);
  Recorder recorder;
  REQUIRE_MESSAGE(loaded.replay(replayed, SimTick{0}, SimTick{5}, {&collect, &recorder}, &error),
                  error);
  REQUIRE(recorder.digests.size() == expected.size());
  for (u32 i = 0; i < expected.size(); ++i)
    CHECK_MESSAGE(recorder.digests[i] == expected[i], "tick " << i);
  CHECK(replayed.held(gear) == false);
  CHECK(replayed.axis(steer) == doctest::Approx(0.0f));
  CHECK(map.is_bound(Source::JoystickHat, hat_code(0, k_hat_left)));
  CHECK_FALSE(map.is_bound(Source::JoystickHat, hat_code(0, k_hat_right)));
  CHECK(map.action_count() == 4);
  CHECK(map.find_action("throttle") == throttle);
  CHECK(map.find_action("look_left") == look);
}

TEST_CASE("input log: a replay against a different map is refused") {
  const ActionMap map = make_map();
  InputLog log;
  log.set_map(map);
  log.record(RawEvent{SimTick{1}, Source::Key, k_key_space, 1.0f, 0});

  // The player rebound fire from space to W after the session was recorded.
  ActionMap rebound = make_map();
  rebound.clear_bindings(rebound.find_action("fire"));
  rebound.bind(rebound.find_action("fire"), Binding{Source::Key, k_key_w});
  REQUIRE(rebound.hash() != map.hash());

  InputState state(rebound);
  std::string error;
  CHECK_FALSE(log.replay(state, SimTick{0}, SimTick{2}, {}, &error));
  CHECK_FALSE(error.empty());
  CHECK(error.find("action map") != std::string::npos);
  CHECK(error.find(std::to_string(map.hash())) != std::string::npos);
  CHECK(error.find(std::to_string(rebound.hash())) != std::string::npos);

  // A state with no map at all is refused too, with its own message.
  InputState mapless;
  error.clear();
  CHECK_FALSE(log.replay(mapless, SimTick{0}, SimTick{2}, {}, &error));
  CHECK_FALSE(error.empty());

  // The same log against the map it was recorded with is fine.
  InputState good(map);
  error.clear();
  CHECK_MESSAGE(log.replay(good, SimTick{0}, SimTick{2}, {}, &error), error);
  CHECK(good.held(map.find_action("fire")));
}

TEST_CASE("input log: an empty range and an empty log") {
  const ActionMap map = make_map();
  InputLog log;
  log.set_map(map);
  CHECK(log.first_tick().value > log.last_tick().value);

  InputState state(map);
  std::string error;
  // to < from does nothing at all, not even a tick.
  Recorder recorder;
  CHECK(log.replay(state, SimTick{4}, SimTick{2}, {&collect, &recorder}, &error));
  CHECK(recorder.digests.empty());
  // A log with no events still ages the state over the range.
  CHECK(log.replay(state, SimTick{0}, SimTick{3}, {&collect, &recorder}, &error));
  CHECK(recorder.digests.size() == 4);

  // Replaying from the middle of a log picks up only the events in the range.
  InputLog filled;
  filled.set_map(map);
  filled.record(RawEvent{SimTick{1}, Source::Key, k_key_space, 1.0f, 0});
  filled.record(RawEvent{SimTick{5}, Source::Key, k_key_w, 1.0f, 0});
  InputState late(map);
  REQUIRE(filled.replay(late, SimTick{4}, SimTick{6}, {}, &error));
  CHECK_FALSE(late.held(map.find_action("fire")));  // the tick 1 press was skipped
  CHECK(late.held(map.find_action("forward")));
}

TEST_CASE("input log: a broken file is refused with a message") {
  const TempDir tmp("engine_input_log_bad");
  const ActionMap map = make_map();

  struct Case {
    const char* name;
    const char* text;
  };
  const Case cases[] = {
      {"empty.jsonl", ""},
      {"not_json.jsonl", "this is not json\n"},
      {"wrong_type.jsonl", "{\"type\":\"engine.bench\",\"version\":1,\"map\":1}\n"},
      {"no_map.jsonl", "{\"type\":\"engine.input.log\",\"version\":1}\n"},
      {"future.jsonl", "{\"type\":\"engine.input.log\",\"version\":99,\"map\":1}\n"},
      {"short_row.jsonl", "{\"type\":\"engine.input.log\",\"version\":1,\"map\":1}\n[1,\"key\"]\n"},
      {"bad_source.jsonl",
       "{\"type\":\"engine.input.log\",\"version\":1,\"map\":1}\n[1,\"elbow\",4,1.0,0]\n"},
      {"bad_field.jsonl",
       "{\"type\":\"engine.input.log\",\"version\":1,\"map\":1}\n[1,\"key\",\"four\",1.0,0]\n"},
      {"out_of_order.jsonl",
       "{\"type\":\"engine.input.log\",\"version\":1,\"map\":1}\n[4,\"key\",4,1.0,0]\n"
       "[1,\"key\",4,0.0,0]\n"},
  };
  for (const Case& c : cases) {
    const std::string path = io::join_path(tmp.path(), c.name);
    REQUIRE(io::write_file(path, c.text) == io::Status::Ok);
    InputLog log;
    std::string error;
    CHECK_MESSAGE(log.load(path, &error) != io::Status::Ok, c.name);
    CHECK_MESSAGE(!error.empty(), c.name);
    CHECK_MESSAGE(log.empty(), c.name);
  }

  // A file that is not there at all reports the read's own status.
  InputLog missing_log;
  std::string error;
  CHECK(missing_log.load(io::join_path(tmp.path(), "nothing.jsonl"), &error) ==
        io::Status::NotFound);
  CHECK_FALSE(error.empty());

  // A good file with blank lines and CRLF endings still loads.
  const std::string path = io::join_path(tmp.path(), "crlf.jsonl");
  const u64 expected_map = map.hash();
  std::string text = "{\"map\":" + std::to_string(expected_map) +
                     ",\"type\":\"engine.input.log\",\"version\":1}\r\n";
  text += "[1,\"key\",44,1.0,0]\r\n\r\n";
  REQUIRE(io::write_file(path, text) == io::Status::Ok);
  InputLog log;
  CHECK_MESSAGE(log.load(path, &error) == io::Status::Ok, error);
  CHECK(log.size() == 1);
  CHECK(log.map_hash() == expected_map);
}

// The device log corpus: three recordings of real hardware nobody on the project can plug in
// (content/input-logs/README.md says what each one is), replayed here on every build.
//
// This is the test the probe exists for. A wheel, a flight stick, and a pad were exercised for
// ten seconds each by their owner; `engine-input probe --log` turned that into `RawEvent`s, and
// those files are now the only description of those devices this repository has. So the numbers
// below are not invented: every one of them was measured, and a change that moves one has
// changed what a replay of a real session means. Adding a device means adding its `.jsonl`, a
// paragraph in the corpus README, and a row here.
//
// The logs are found through ENGINE_SOURCE_DIR (foundation/input/CMakeLists.txt defines it),
// not relative to the executable, because a test binary can sit anywhere under build/ — or
// through the test bundle that carries a copy of them, when this run came out of one
// (tests/support/test_paths.h, tools/package-tests.ps1).
#include <core/base/types.h>
#include <foundation/input/input.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <bit>
#include <string>

#if !defined(ENGINE_SOURCE_DIR)
#error "ENGINE_SOURCE_DIR must name the repository root; see foundation/input/CMakeLists.txt"
#endif

using namespace engine;

namespace {

// Which axis, button, and hat indices a device was seen to use. 256 is well past any device
// SDL reports; anything beyond it would be a different kind of problem.
struct IndexSet {
  u64 words[4] = {0, 0, 0, 0};

  void add(u32 index) noexcept {
    if (index >= 256) return;
    words[index / 64] |= u64{1} << (index % 64);
  }
  u32 count() const noexcept {
    u32 total = 0;
    for (u64 word : words)
      total += static_cast<u32>(std::popcount(word));
    return total;
  }
};

// One device in one log, by the slot it held in its own space. `name` is what the device is
// called in the corpus README; the log itself carries no names, only slots, so the README is
// where the two are tied together and the test checks that the tie is still there.
struct DeviceExpect {
  const char* name = nullptr;
  u32 device = 0;
  bool joystick = true;  // which slot space, and so which Source family
  u32 axes = 0;
  u32 buttons = 0;
  u32 hats = 0;
  u32 events = 0;  // RawEvents attributed to this device (a hat event counts as four)
};

struct LogExpect {
  const char* file;
  u32 events;
  u64 first_tick;
  u64 last_tick;
  u64 ticks;  // replayed, including the ticks nothing happened on
  u32 actions;
  u32 button_actions;
  u32 axis_actions;
  u64 presses;
  u64 releases;
  DeviceExpect devices[2];
};

// Measured, not chosen. `engine-input probe --seconds 10` on 2026-09-17, one device at a time
// except the T300, whose wheel and shifter are two joysticks on one rig.
constexpr LogExpect k_logs[] = {
    // A pad: SDL has a mapping for it, so its sources are the named gamepad ones and its d-pad
    // arrives as four buttons rather than as a hat. Four axes, eight of its twelve buttons.
    {.file = "f710.jsonl",
     .events = 796,
     .first_tick = 9,
     .last_tick = 583,
     .ticks = 575,
     .actions = 12,
     .button_actions = 8,
     .axis_actions = 4,
     .presses = 67,
     .releases = 63,
     .devices = {{.name = "Logitech F710 Gamepad",
                  .device = 0,
                  .joystick = false,
                  .axes = 4,
                  .buttons = 8,
                  .hats = 0,
                  .events = 796}}},
    // A flight stick: no mapping, so bare indices, and the only device in the corpus with a hat.
    {.file = "t16000m.jsonl",
     .events = 907,
     .first_tick = 15,
     .last_tick = 596,
     .ticks = 582,
     .actions = 19,
     .button_actions = 15,
     .axis_actions = 4,
     .presses = 49,
     .releases = 47,
     .devices = {{.name = "Thrustmaster T.16000M",
                  .device = 0,
                  .joystick = true,
                  .axes = 4,
                  .buttons = 11,
                  .hats = 1,
                  .events = 907}}},
    // Two joysticks at once: the wheel on slot 0 (steering on axis 0, three pedals on 1..3) and
    // the shifter on slot 1, which has no axis and no hat and contributes nothing but buttons.
    {.file = "t300.jsonl",
     .events = 2073,
     .first_tick = 5,
     .last_tick = 591,
     .ticks = 587,
     .actions = 16,
     .button_actions = 12,
     .axis_actions = 4,
     .presses = 34,
     .releases = 30,
     .devices = {{.name = "Thrustmaster T300 RS",
                  .device = 0,
                  .joystick = true,
                  .axes = 4,
                  .buttons = 10,
                  .hats = 0,
                  .events = 2062},
                 {.name = "Thrustmaster T500 RS Gear Shift",
                  .device = 1,
                  .joystick = true,
                  .axes = 0,
                  .buttons = 6,
                  .hats = 0,
                  .events = 11}}},
};

// The action name a recorded signal gets, the same rule `engine-input replay` uses: the source
// and the code, because that is all a log knows about a signal. No device filter, so a code two
// devices both use feeds one action — which is exactly what a game with one binding table and
// two wheels would get, and why the shifter's buttons are not a separate set here.
std::string action_name_for(input::Source source, u32 code) {
  std::string name = input::source_name(source);
  name.push_back('_');
  if (source == input::Source::JoystickHat) {
    name += std::to_string(input::hat_index_of(code));
    name.push_back('_');
    name += std::to_string(input::hat_direction_of(code));
    return name;
  }
  name += std::to_string(code);
  return name;
}

struct Tally {
  u64 ticks = 0;
  u64 presses = 0;
  u64 releases = 0;
  const input::ActionMap* map = nullptr;
};

void tally_tick(void* user, SimTick tick, const input::InputState& state) {
  Tally* tally = static_cast<Tally*>(user);
  (void)tick;
  ++tally->ticks;
  for (input::ActionId action = 0; action < tally->map->action_count(); ++action) {
    if (state.pressed(action)) ++tally->presses;
    if (state.released(action)) ++tally->releases;
  }
}

bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

// One unguessable scratch directory per object, so a second copy of this binary cannot delete
// this one's re-recorded logs (tests/support/test_temp_dir.h).
using TempDir = test::TempDir;

}  // namespace

TEST_CASE("input: the committed device logs replay to the numbers they were recorded with") {
  const std::string dir =
      test::data_path(ENGINE_SOURCE_DIR "/content/input-logs", "content/input-logs");
  std::string readme;
  REQUIRE_MESSAGE(io::read_file(io::join_path(dir, "README.md"), readme) == io::Status::Ok, dir);

  for (const LogExpect& expect : k_logs) {
    INFO("log: " << expect.file);
    input::InputLog log;
    std::string error;
    REQUIRE_MESSAGE(log.load(io::join_path(dir, expect.file), &error) == io::Status::Ok, error);

    CHECK(log.size() == expect.events);
    CHECK(log.first_tick().value == expect.first_tick);
    CHECK(log.last_tick().value == expect.last_tick);

    // Every distinct signal becomes one action, digital ones as Buttons and analog ones as Axes.
    input::ActionMap map;
    u32 button_actions = 0;
    u32 axis_actions = 0;
    for (const input::RawEvent& event : log.events()) {
      const std::string name = action_name_for(event.source, event.code);
      if (map.find_action(name) != input::k_invalid_action) continue;
      const bool digital = input::is_digital(event.source);
      const input::ActionId action =
          map.add_action(name, digital ? input::ActionKind::Button : input::ActionKind::Axis);
      REQUIRE(action != input::k_invalid_action);
      map.bind(action, input::Binding{event.source, event.code});
      if (digital) {
        ++button_actions;
      } else {
        ++axis_actions;
      }
    }
    CHECK(map.action_count() == expect.actions);
    CHECK(button_actions == expect.button_actions);
    CHECK(axis_actions == expect.axis_actions);

    // What each device is, read off the events rather than off a name: how many distinct axes,
    // buttons, and hats it used, and in which of the two slot spaces.
    for (const DeviceExpect& device : expect.devices) {
      if (device.name == nullptr) continue;
      INFO("device: " << device.name);
      // The corpus README is the only place a slot is tied to a device, so it has to name it.
      CHECK_MESSAGE(contains(readme, device.name),
                    "content/input-logs/README.md does not name " << device.name);
      IndexSet axes;
      IndexSet buttons;
      IndexSet hats;
      u32 events = 0;
      for (const input::RawEvent& event : log.events()) {
        if (event.device != device.device) continue;
        switch (event.source) {
          case input::Source::GamepadAxis:
            if (device.joystick) continue;
            axes.add(event.code);
            break;
          case input::Source::GamepadButton:
            if (device.joystick) continue;
            buttons.add(event.code);
            break;
          case input::Source::JoystickAxis:
            if (!device.joystick) continue;
            axes.add(event.code);
            break;
          case input::Source::JoystickButton:
            if (!device.joystick) continue;
            buttons.add(event.code);
            break;
          case input::Source::JoystickHat:
            if (!device.joystick) continue;
            hats.add(input::hat_index_of(event.code));
            break;
          default: continue;  // no keyboard or mouse was recorded into these
        }
        ++events;
      }
      CHECK(axes.count() == device.axes);
      CHECK(buttons.count() == device.buttons);
      CHECK(hats.count() == device.hats);
      CHECK(events == device.events);
    }

    // A probe records no map, so the log takes the one just built from it; then the whole
    // session runs through an InputState with nothing plugged in.
    log.set_map(map);
    input::InputState state(map);
    Tally tally;
    tally.map = &map;
    REQUIRE_MESSAGE(
        log.replay(state, log.first_tick(), log.last_tick(), {&tally_tick, &tally}, &error), error);
    CHECK(tally.ticks == expect.ticks);
    CHECK(tally.presses == expect.presses);
    CHECK(tally.releases == expect.releases);

    // And the committed bytes are canonical: writing the loaded events back gives them again,
    // line for line, which is what makes a file in the repository worth more than a note.
    std::string original;
    REQUIRE(io::read_file(io::join_path(dir, expect.file), original) == io::Status::Ok);
    const TempDir tmp("engine_input_corpus");
    const std::string round_trip = io::join_path(tmp.path(), expect.file);
    REQUIRE(log.save(round_trip) == io::Status::Ok);
    std::string written;
    REQUIRE(io::read_file(round_trip, written) == io::Status::Ok);
    // The header holds the map hash this replay stamped in, which the recording had as 0, so
    // the comparison starts after the first line.
    const usize a = original.find('\n');
    const usize b = written.find('\n');
    REQUIRE(a != std::string::npos);
    REQUIRE(b != std::string::npos);
    CHECK(original.substr(a) == written.substr(b));
  }
}

// engine-input: what is plugged in, what it emits, and what a recording of it replays to.
//
// The engine's input path (docs/subsystems/window.md, docs/subsystems/input.md) has to work
// with devices nobody on the project owns: wheels, pedal boxes, shifters, flight sticks, pads
// on dongles. This is the tool that closes that gap. The owner of a device runs
//
//     engine-input devices                       # one JSON line per attached device
//     engine-input probe --seconds 10 --log wheel.jsonl
//
// moves everything, and sends the two outputs back; `engine-input replay wheel.jsonl` then
// turns the recording into an `input::InputState` here, with no device attached at all. So the
// axis numbering of a shifter can be read off a file instead of guessed.
//
// SDL needs no window for joystick events, but it does need its own event pump, and it drops
// device events while a process owns windows and none of them holds keyboard focus. This tool
// therefore creates one hidden, non-Vulkan window (that is what SDL's video subsystem wants to
// exist before it will pump anything on some backends) and turns
// `window::set_background_input(true)` on, so the terminal it was started from can keep focus
// while the owner has both hands on a wheel.
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/json/json.h>
#include <core/platform/thread.h>
#include <core/time/time.h>
#include <foundation/input/input.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>
#include <foundation/window/window.h>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

using namespace engine;

namespace {

// clang-format off
constexpr const char* k_usage =
    "usage: engine-input <command> [options]\n"
    "\n"
    "  devices                       one JSON line per connected gamepad and joystick:\n"
    "                                name, guid, vendor, product, axes, buttons, hats, slot\n"
    "  probe [options]               stream one JSON line per event, then a summary line\n"
    "    --seconds <n>               how long to listen (default 10, max 3600)\n"
    "    --device <slot>             only this slot id\n"
    "    --gamepad                   only devices SDL has a gamepad mapping for\n"
    "    --joystick                  only raw joysticks (wheels, pedals, shifters, sticks)\n"
    "    --log <file.jsonl>          also record the events as an input::InputLog\n"
    "  replay <file.jsonl>           load a recorded log, bind every axis, button, and hat it\n"
    "                                saw, replay it into an InputState, print a summary\n"
    "\n"
    "The gamepad and joystick slot spaces are separate: gamepad 0 and joystick 0 are two\n"
    "different devices, and each event line says which space its `device` belongs to.\n"
    "\n"
    "examples:\n"
    "  engine-input devices\n"
    "  engine-input probe --seconds 10 --log wheel.jsonl\n"
    "  engine-input probe --joystick --device 1 --seconds 20\n"
    "  engine-input replay wheel.jsonl\n"
    "\n"
    "exit codes: 0 ok, 1 error, 2 usage, 3 unavailable (no display or no SDL video driver)\n";
// clang-format on

constexpr int k_exit_ok = 0;
constexpr int k_exit_error = 1;
constexpr int k_exit_usage = 2;
constexpr int k_exit_unavailable = 3;

// The probe stamps events onto a simulation tick so the recording replays at the rate a game
// would have read it. 60 Hz is the engine's default fixed step.
constexpr u64 k_ticks_per_second = 60;

int fail(const char* what, const std::string& detail) {
  std::fprintf(stderr, "engine-input: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
  return k_exit_error;
}

int unavailable(const char* what, const std::string& detail) {
  std::fprintf(stderr, "engine-input: unavailable: %s%s%s\n", what, detail.empty() ? "" : ": ",
               detail.c_str());
  return k_exit_unavailable;
}

int usage_error(const char* what, std::string_view detail) {
  std::fprintf(stderr, "engine-input: %s%s%.*s\n%s", what, detail.empty() ? "" : " ",
               static_cast<int>(detail.size()), detail.data(), k_usage);
  return k_exit_usage;
}

void print_line(const JsonValue& value) {
  std::string text = write_json(value, JsonWriteOptions{.pretty = false});
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fflush(stdout);  // a probe is watched while it runs
}

bool parse_u32(const char* text, u32& out) {
  char* end = nullptr;
  const unsigned long v = std::strtoul(text, &end, 10);
  if (end == text || *end != '\0' || v > 0xFFFFFFFFul) return false;
  out = static_cast<u32>(v);
  return true;
}

// Up to 128 indices seen, which covers the widest button panel SDL reports.
struct IndexSet {
  u64 words[2] = {0, 0};

  void add(u32 index) noexcept {
    if (index >= 128) return;
    words[index / 64] |= u64{1} << (index % 64);
  }
  u32 count() const noexcept {
    return static_cast<u32>(std::popcount(words[0]) + std::popcount(words[1]));
  }
};

struct DeviceStats {
  bool seen = false;
  std::string name;
  u32 events = 0;
  IndexSet axes;
  IndexSet buttons;
  IndexSet hats;
};

// Indexed [is_joystick][slot]: the two slot spaces are separate and must not be merged.
struct Probe {
  DeviceStats gamepads[window::k_max_gamepads];
  DeviceStats joysticks[window::k_max_joysticks];

  DeviceStats* slot(bool joystick, u32 index) noexcept {
    if (joystick) return index < window::k_max_joysticks ? &joysticks[index] : nullptr;
    return index < window::k_max_gamepads ? &gamepads[index] : nullptr;
  }
};

std::string device_key(bool joystick, u32 slot) {
  return (joystick ? "joystick:" : "gamepad:") + std::to_string(slot);
}

// --- session --------------------------------------------------------------------------------

struct Session {
  window::Window win;
  bool open(std::string& error) {
    if (!window::init(&error)) return false;
    // The owner runs this from a terminal and then puts both hands on a wheel; without this SDL
    // would drop every event because the hidden window never takes focus.
    window::set_background_input(true);
    window::WindowDesc desc;
    desc.title = "engine-input";
    desc.width = 64;
    desc.height = 64;
    desc.resizable = false;
    desc.vulkan = false;  // nothing is drawn; a Vulkan window would need a loader
    desc.hidden = true;
    return win.create(desc, &error);
  }
  ~Session() {
    win.destroy();
    window::shutdown();
  }
};

// SDL announces everything already plugged in as an arrival, so a moment of polling is what
// assigns the slots. Returns the arrival events, in order, for the caller to report.
void settle(Session& session, Vector<window::Event>& arrivals, u32 milliseconds) {
  const time::Stopwatch watch;
  window::Event event;
  for (;;) {
    while (session.win.poll(event)) {
      if (event.kind == window::EventKind::GamepadConnected ||
          event.kind == window::EventKind::JoystickConnected) {
        arrivals.push_back(event);
      }
    }
    if (watch.elapsed_ns() >= static_cast<i64>(milliseconds) * 1'000'000) break;
    platform::sleep_ms(1);
  }
}

// --- devices --------------------------------------------------------------------------------

int command_devices() {
  Session session;
  std::string error;
  if (!session.open(error)) return unavailable("cannot open a window for device events", error);
  Vector<window::Event> arrivals;
  settle(session, arrivals, 250);

  Vector<window::InputDeviceInfo> devices;
  const u32 count = window::input_devices(devices);
  for (u32 i = 0; i < count; ++i) {
    const window::InputDeviceInfo& info = devices[i];
    JsonValue line = JsonValue::object();
    line.set("space", info.is_gamepad ? "gamepad" : "joystick");
    line.set("slot", info.slot == window::k_invalid_slot ? JsonValue()
                                                         : JsonValue(static_cast<u64>(info.slot)));
    line.set("name", info.name);
    line.set("guid", info.guid);
    line.set("vendor", static_cast<u64>(info.vendor));
    line.set("product", static_cast<u64>(info.product));
    line.set("axes", static_cast<u64>(info.axes));
    line.set("buttons", static_cast<u64>(info.buttons));
    line.set("hats", static_cast<u64>(info.hats));
    line.set("is_gamepad", info.is_gamepad);
    print_line(line);
  }
  if (count == 0) std::fprintf(stderr, "engine-input: no gamepad or joystick is attached\n");
  return k_exit_ok;
}

// --- probe ----------------------------------------------------------------------------------

struct ProbeOptions {
  u32 seconds = 10;
  u32 device = 0xFFFFFFFFu;  // every slot
  bool gamepads = true;
  bool joysticks = true;
  std::string log_path;
};

// What one window event is, for the stream and for the log. `joystick` picks the slot space.
struct Translated {
  const char* kind = "";
  bool joystick = false;
  u32 slot = 0;
  u32 index = 0;
  f32 value = 0.0f;
  const char* name = nullptr;  // the enum's name where the device has a layout
  bool is_axis = false;
  bool is_button = false;
  bool is_hat = false;
  bool arrival = false;  // connected or disconnected: no index, no value
};

bool translate(const window::Event& event, Translated& out) {
  const bool down = event.kind == window::EventKind::GamepadButtonDown ||
                    event.kind == window::EventKind::JoystickButtonDown;
  switch (event.kind) {
    case window::EventKind::GamepadConnected:
      out = {.kind = "gamepad_connected",
             .slot = event.gamepad,
             .name = window::gamepad_name(event.gamepad),
             .arrival = true};
      return true;
    case window::EventKind::GamepadDisconnected:
      out = {.kind = "gamepad_disconnected", .slot = event.gamepad, .arrival = true};
      return true;
    case window::EventKind::GamepadButtonDown:
    case window::EventKind::GamepadButtonUp:
      out = {.kind = "gamepad_button",
             .slot = event.gamepad,
             .index = static_cast<u32>(event.gamepad_button),
             .value = down ? 1.0f : 0.0f,
             .name = window::gamepad_button_name(event.gamepad_button),
             .is_button = true};
      return true;
    case window::EventKind::GamepadAxis:
      out = {.kind = "gamepad_axis",
             .slot = event.gamepad,
             .index = static_cast<u32>(event.gamepad_axis),
             .value = event.value,
             .name = window::gamepad_axis_name(event.gamepad_axis),
             .is_axis = true};
      return true;
    case window::EventKind::JoystickConnected:
      out = {.kind = "joystick_connected",
             .joystick = true,
             .slot = event.joystick,
             .name = window::joystick_name(event.joystick),
             .arrival = true};
      return true;
    case window::EventKind::JoystickDisconnected:
      out = {.kind = "joystick_disconnected",
             .joystick = true,
             .slot = event.joystick,
             .arrival = true};
      return true;
    case window::EventKind::JoystickAxis:
      out = {.kind = "joystick_axis",
             .joystick = true,
             .slot = event.joystick,
             .index = event.index,
             .value = event.value,
             .is_axis = true};
      return true;
    case window::EventKind::JoystickButtonDown:
    case window::EventKind::JoystickButtonUp:
      out = {.kind = "joystick_button",
             .joystick = true,
             .slot = event.joystick,
             .index = event.index,
             .value = down ? 1.0f : 0.0f,
             .is_button = true};
      return true;
    case window::EventKind::JoystickHat:
      // A hat's value is the direction bitmask itself; the stream carries the name beside it.
      out = {.kind = "joystick_hat",
             .joystick = true,
             .slot = event.joystick,
             .index = event.index,
             .value = static_cast<f32>(static_cast<u32>(event.hat)),
             .name = window::hat_direction_name(event.hat),
             .is_hat = true};
      return true;
    default: return false;
  }
}

// One event as the RawEvent(s) a game would have seen. A hat becomes four digital signals, one
// per cardinal direction, because a binding names a direction and not a position; the rest are
// one for one. See foundation/input/input.h.
void record_raw(input::InputLog& log, const window::Event& event, u64 tick) {
  input::RawEvent raw;
  raw.tick = SimTick{tick};
  switch (event.kind) {
    case window::EventKind::GamepadButtonDown:
    case window::EventKind::GamepadButtonUp:
      raw.source = input::Source::GamepadButton;
      raw.code = static_cast<u32>(event.gamepad_button);
      raw.value = event.kind == window::EventKind::GamepadButtonDown ? 1.0f : 0.0f;
      raw.device = event.gamepad;
      break;
    case window::EventKind::GamepadAxis:
      raw.source = input::Source::GamepadAxis;
      raw.code = static_cast<u32>(event.gamepad_axis);
      raw.value = event.value;
      raw.device = event.gamepad;
      break;
    case window::EventKind::JoystickAxis:
      raw.source = input::Source::JoystickAxis;
      raw.code = event.index;
      raw.value = event.value;
      raw.device = event.joystick;
      break;
    case window::EventKind::JoystickButtonDown:
    case window::EventKind::JoystickButtonUp:
      raw.source = input::Source::JoystickButton;
      raw.code = event.index;
      raw.value = event.kind == window::EventKind::JoystickButtonDown ? 1.0f : 0.0f;
      raw.device = event.joystick;
      break;
    case window::EventKind::JoystickHat: {
      const u32 directions[4] = {input::k_hat_up, input::k_hat_right, input::k_hat_down,
                                 input::k_hat_left};
      const u32 mask = static_cast<u32>(event.hat);
      raw.source = input::Source::JoystickHat;
      raw.device = event.joystick;
      for (u32 direction : directions) {
        raw.code = input::hat_code(event.index, direction);
        raw.value = (mask & direction) != 0 ? 1.0f : 0.0f;
        log.record(raw);
      }
      return;
    }
    default: return;  // arrivals and departures are not player input
  }
  log.record(raw);
}

int command_probe(const ProbeOptions& options) {
  Session session;
  std::string error;
  if (!session.open(error)) return unavailable("cannot open a window for device events", error);

  auto wanted = [&](const Translated& t) {
    if (t.joystick ? !options.joysticks : !options.gamepads) return false;
    return options.device == 0xFFFFFFFFu || t.slot == options.device;
  };

  // Let the devices already plugged in arrive, and report them at t = 0 so the reader can see
  // which slot each one took before any motion.
  Vector<window::Event> arrivals;
  settle(session, arrivals, 250);
  Probe probe;
  input::InputLog log;
  u32 total = 0;
  Translated translated;
  for (const window::Event& arrival : arrivals) {
    if (!translate(arrival, translated) || !wanted(translated)) continue;
    JsonValue line = JsonValue::object();
    line.set("t_ms", static_cast<u64>(0));
    line.set("kind", translated.kind);
    line.set("space", translated.joystick ? "joystick" : "gamepad");
    line.set("device", static_cast<u64>(translated.slot));
    line.set("name", translated.name != nullptr ? translated.name : "");
    print_line(line);
    if (DeviceStats* stats = probe.slot(translated.joystick, translated.slot); stats != nullptr) {
      stats->seen = true;
      if (translated.name != nullptr) stats->name.assign(translated.name);
    }
  }

  const time::Stopwatch watch;
  const i64 limit_ns = static_cast<i64>(options.seconds) * 1'000'000'000;
  bool quit = false;
  window::Event event;
  while (!quit) {
    const i64 elapsed_ns = watch.elapsed_ns();
    if (elapsed_ns >= limit_ns) break;
    while (session.win.poll(event)) {
      if (event.kind == window::EventKind::Quit) {
        quit = true;
        break;
      }
      if (!translate(event, translated) || !wanted(translated)) continue;
      const u64 t_ms = static_cast<u64>(watch.elapsed_ns() / 1'000'000);
      JsonValue line = JsonValue::object();
      line.set("t_ms", t_ms);
      line.set("kind", translated.kind);
      line.set("space", translated.joystick ? "joystick" : "gamepad");
      line.set("device", static_cast<u64>(translated.slot));
      if (!translated.arrival) {
        line.set("index", static_cast<u64>(translated.index));
        line.set("value", static_cast<f64>(translated.value));
      }
      if (translated.name != nullptr) line.set("name", translated.name);
      print_line(line);

      ++total;
      if (DeviceStats* stats = probe.slot(translated.joystick, translated.slot); stats != nullptr) {
        stats->seen = true;
        ++stats->events;
        if (stats->name.empty() && translated.name != nullptr) stats->name.assign(translated.name);
        if (translated.is_axis) stats->axes.add(translated.index);
        if (translated.is_button) stats->buttons.add(translated.index);
        if (translated.is_hat) stats->hats.add(translated.index);
      }
      if (!options.log_path.empty()) {
        record_raw(log, event, t_ms * k_ticks_per_second / 1000);
      }
    }
    platform::sleep_ms(1);
  }

  JsonValue per_device = JsonValue::object();
  u32 axes_seen = 0;
  u32 buttons_seen = 0;
  u32 hats_seen = 0;
  auto add_device = [&](const DeviceStats& stats, bool joystick, u32 slot) {
    if (!stats.seen) return;
    JsonValue entry = JsonValue::object();
    entry.set("name", stats.name);
    entry.set("events", static_cast<u64>(stats.events));
    entry.set("axes", static_cast<u64>(stats.axes.count()));
    entry.set("buttons", static_cast<u64>(stats.buttons.count()));
    entry.set("hats", static_cast<u64>(stats.hats.count()));
    per_device.set(device_key(joystick, slot), std::move(entry));
    axes_seen += stats.axes.count();
    buttons_seen += stats.buttons.count();
    hats_seen += stats.hats.count();
  };
  for (u32 i = 0; i < window::k_max_gamepads; ++i)
    add_device(probe.gamepads[i], false, i);
  for (u32 i = 0; i < window::k_max_joysticks; ++i)
    add_device(probe.joysticks[i], true, i);

  int exit_code = k_exit_ok;
  if (!options.log_path.empty()) {
    const io::Status status = log.save(options.log_path);
    if (status != io::Status::Ok) {
      exit_code = fail("cannot write the log",
                       std::string(options.log_path) + ": " + io::status_name(status));
    }
  }

  JsonValue summary = JsonValue::object();
  summary.set("events", static_cast<u64>(total));
  summary.set("seconds", static_cast<f64>(watch.elapsed_ns()) / 1.0e9);
  summary.set("per_device", std::move(per_device));
  summary.set("axes_seen", static_cast<u64>(axes_seen));
  summary.set("buttons_seen", static_cast<u64>(buttons_seen));
  summary.set("hats_seen", static_cast<u64>(hats_seen));
  if (!options.log_path.empty()) {
    summary.set("log", options.log_path);
    summary.set("log_events", static_cast<u64>(log.size()));
  }
  print_line(summary);
  return exit_code;
}

// --- replay ---------------------------------------------------------------------------------

// The action name a recorded signal gets: the source name and the code, which is the only thing
// the log knows about it. "joystick_axis_2" is axis 2 of whichever joystick moved.
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

struct ReplayTally {
  u64 ticks = 0;
  u64 presses = 0;
  u64 releases = 0;
  const input::ActionMap* map = nullptr;
};

void tally_tick(void* user, SimTick tick, const input::InputState& state) {
  ReplayTally* tally = static_cast<ReplayTally*>(user);
  (void)tick;
  ++tally->ticks;
  const u32 count = tally->map->action_count();
  for (input::ActionId action = 0; action < count; ++action) {
    if (state.pressed(action)) ++tally->presses;
    if (state.released(action)) ++tally->releases;
  }
}

int command_replay(const std::string& path) {
  input::InputLog log;
  std::string error;
  if (log.load(path, &error) != io::Status::Ok) return fail("cannot load the log", error);

  // Every distinct signal in the log becomes one action: a Button for the digital sources, an
  // Axis for the analog ones. That is the most a recording can say on its own, and it is enough
  // to prove the file replays.
  input::ActionMap map;
  u32 button_actions = 0;
  u32 axis_actions = 0;
  for (const input::RawEvent& event : log.events()) {
    const std::string name = action_name_for(event.source, event.code);
    if (map.find_action(name) != input::k_invalid_action) continue;
    const bool digital = input::is_digital(event.source);
    const input::ActionId action =
        map.add_action(name, digital ? input::ActionKind::Button : input::ActionKind::Axis);
    if (action == input::k_invalid_action) continue;
    map.bind(action, input::Binding{event.source, event.code});
    if (digital) {
      ++button_actions;
    } else {
      ++axis_actions;
    }
  }

  // A probe records no action map (the header's hash is 0), so the log is stamped with the map
  // just built from it before replaying; a log that does carry a hash keeps it, and replaying a
  // rebound session is refused by InputLog::replay as it should be.
  if (log.map_hash() == 0) log.set_map_hash(map.hash());

  input::InputState state(map);
  ReplayTally tally;
  tally.map = &map;
  const SimTick from = log.empty() ? SimTick{0} : log.first_tick();
  const SimTick to = log.empty() ? SimTick{0} : log.last_tick();
  if (!log.replay(state, from, to, {&tally_tick, &tally}, &error)) {
    return fail("cannot replay the log", error);
  }

  JsonValue summary = JsonValue::object();
  summary.set("file", path);
  summary.set("events", static_cast<u64>(log.size()));
  summary.set("first_tick", from.value);
  summary.set("last_tick", to.value);
  summary.set("ticks", tally.ticks);
  summary.set("actions", static_cast<u64>(map.action_count()));
  summary.set("button_actions", static_cast<u64>(button_actions));
  summary.set("axis_actions", static_cast<u64>(axis_actions));
  summary.set("presses", tally.presses);
  summary.set("releases", tally.releases);
  summary.set("map", map.hash());
  summary.set("state", state.state_hash());
  print_line(summary);
  return k_exit_ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fputs(k_usage, stderr);
    return k_exit_usage;
  }
  const std::string_view command = argv[1];
  if (command == "--help" || command == "-h" || command == "help") {
    std::fputs(k_usage, stdout);
    return k_exit_ok;
  }

  if (command == "devices") {
    if (argc > 2) return usage_error("devices takes no arguments, got", argv[2]);
    return command_devices();
  }

  if (command == "probe") {
    ProbeOptions options;
    bool only_gamepads = false;
    bool only_joysticks = false;
    for (int i = 2; i < argc; ++i) {
      const std::string_view a = argv[i];
      auto value = [&](const char** out) {
        if (i + 1 >= argc) return false;
        *out = argv[++i];
        return true;
      };
      const char* text = nullptr;
      if (a == "--seconds") {
        if (!value(&text) || !parse_u32(text, options.seconds) || options.seconds == 0 ||
            options.seconds > 3600) {
          return usage_error("--seconds needs a count of 1..3600, got",
                             text != nullptr ? text : "");
        }
      } else if (a == "--device") {
        if (!value(&text) || !parse_u32(text, options.device) || options.device >= 255) {
          return usage_error("--device needs a slot id, got", text != nullptr ? text : "");
        }
      } else if (a == "--log") {
        if (!value(&text)) return usage_error("--log needs a file", "");
        options.log_path.assign(text);
      } else if (a == "--gamepad") {
        only_gamepads = true;
      } else if (a == "--joystick") {
        only_joysticks = true;
      } else {
        return usage_error("unknown option", a);
      }
    }
    if (only_gamepads != only_joysticks) {
      options.gamepads = only_gamepads;
      options.joysticks = only_joysticks;
    }
    return command_probe(options);
  }

  if (command == "replay") {
    if (argc != 3) return usage_error("replay needs exactly one log file", "");
    return command_replay(argv[2]);
  }

  return usage_error("unknown command", command);
}

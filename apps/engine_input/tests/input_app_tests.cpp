// End to end: engine-input lists devices, listens for a second, and replays a log written here
// by hand. No case may need a device attached — the machine this runs on in CI has none, and
// the one it was written on has none either — so the device-dependent assertions are about the
// shape of the output, not about its contents. Where SDL cannot open a display the app exits 3
// and the test records the skip, exactly as engine-view's suite does.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <foundation/input/input.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

// The child's stderr is inherited rather than merged, so stdout stays pure JSON lines.
Run engine_input(std::vector<std::string> args) {
  std::vector<std::string_view> argv;
  argv.push_back(ENGINE_APP_PATH);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn engine-input: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
  return run;
}

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> lines;
  for (usize begin = 0; begin < text.size();) {
    usize end = text.find('\n', begin);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(begin, end - begin);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (!line.empty()) lines.push_back(line);
    begin = end + 1;
  }
  return lines;
}

u64 number(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  u64 out = 0;
  REQUIRE_MESSAGE(value != nullptr, key);
  REQUIRE_MESSAGE(value->get_u64(out), key);
  return out;
}

struct TempDir {
  std::string path;
  explicit TempDir(const char* name) {
    const auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    path = io::normalize_path(p.string());
  }
  ~TempDir() { std::filesystem::remove_all(std::filesystem::path(path)); }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

}  // namespace

TEST_CASE("engine-input: usage errors exit 2 and --help exits 0") {
  CHECK(engine_input({}).exit_code == 2);
  CHECK(engine_input({"wiggle"}).exit_code == 2);
  CHECK(engine_input({"devices", "extra"}).exit_code == 2);
  CHECK(engine_input({"probe", "--bogus"}).exit_code == 2);
  CHECK(engine_input({"probe", "--seconds"}).exit_code == 2);
  CHECK(engine_input({"probe", "--seconds", "0"}).exit_code == 2);
  CHECK(engine_input({"probe", "--seconds", "nine"}).exit_code == 2);
  CHECK(engine_input({"probe", "--device", "abc"}).exit_code == 2);
  CHECK(engine_input({"probe", "--log"}).exit_code == 2);
  CHECK(engine_input({"replay"}).exit_code == 2);
  CHECK(engine_input({"replay", "a.jsonl", "b.jsonl"}).exit_code == 2);
  CHECK(engine_input({"probe", "--events"}).exit_code == 2);

  // ffb and rumble name a slot in two different spaces, and every level has a range.
  CHECK(engine_input({"ffb"}).exit_code == 2);
  CHECK(engine_input({"ffb", "--spring", "0.5"}).exit_code == 2);  // no --device
  CHECK(engine_input({"ffb", "--device"}).exit_code == 2);
  CHECK(engine_input({"ffb", "--device", "left"}).exit_code == 2);
  CHECK(engine_input({"ffb", "--device", "8"}).exit_code == 2);  // past k_max_joysticks
  CHECK(engine_input({"ffb", "--device", "0", "--spring", "2"}).exit_code == 2);
  CHECK(engine_input({"ffb", "--device", "0", "--spring", "-0.5"}).exit_code == 2);
  CHECK(engine_input({"ffb", "--device", "0", "--damper", "hard"}).exit_code == 2);
  CHECK(engine_input({"ffb", "--device", "0", "--constant", "-2"}).exit_code == 2);
  CHECK(engine_input({"ffb", "--device", "0", "--seconds", "0"}).exit_code == 2);
  CHECK(engine_input({"ffb", "--device", "0", "--seconds", "61"}).exit_code == 2);
  CHECK(engine_input({"ffb", "--device", "0", "--wobble"}).exit_code == 2);
  CHECK(engine_input({"rumble"}).exit_code == 2);
  CHECK(engine_input({"rumble", "--low", "0.5"}).exit_code == 2);   // no --device
  CHECK(engine_input({"rumble", "--device", "8"}).exit_code == 2);  // past k_max_gamepads
  CHECK(engine_input({"rumble", "--device", "0", "--low", "1.5"}).exit_code == 2);
  CHECK(engine_input({"rumble", "--device", "0", "--high", "loud"}).exit_code == 2);
  CHECK(engine_input({"rumble", "--device", "0", "--ms", "0"}).exit_code == 2);
  CHECK(engine_input({"rumble", "--device", "0", "--ms", "10001"}).exit_code == 2);

  const Run help = engine_input({"--help"});
  CHECK(help.exit_code == 0);
  CHECK(help.output.find("usage: engine-input") != std::string::npos);
  CHECK(help.output.find("exit codes: 0 ok, 1 error, 2 usage, 3 unavailable") != std::string::npos);
  CHECK(help.output.find("ffb --device <slot>") != std::string::npos);
  CHECK(help.output.find("rumble --device <slot>") != std::string::npos);
  CHECK(help.output.find("--events <file.jsonl>") != std::string::npos);
}

// Force feedback and rumble are output: no test can feel one, and the machines this runs on
// have neither a wheel nor (reliably) a pad. What is checkable is that the last slot of each
// space is empty and that asking it for a force is "unavailable" rather than a crash or a
// success — the same 3 a machine with no display answers, which is why one check covers both.
TEST_CASE("engine-input: ffb and rumble on an empty slot exit 3") {
  const Run ffb = engine_input({"ffb", "--device", "7", "--spring", "0.5", "--seconds", "1"});
  CHECK_MESSAGE(ffb.exit_code == 3, ffb.output);
  CHECK(ffb.output.empty());  // nothing is printed for a device that is not there
  const Run rumble = engine_input({"rumble", "--device", "7", "--ms", "1"});
  CHECK_MESSAGE(rumble.exit_code == 3, rumble.output);
  CHECK(rumble.output.empty());
}

TEST_CASE("engine-input: devices prints one JSON line per device, or nothing at all") {
  const Run run = engine_input({"devices"});
  if (run.exit_code == 3) {
    MESSAGE("engine-input unavailable here (no display)");
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  for (const std::string& line : lines_of(run.output)) {
    JsonValue device;
    REQUIRE_MESSAGE(parse_json(line, device).ok, line);
    REQUIRE(device.is_object());
    CHECK(device.find("name") != nullptr);
    CHECK(device.find("guid") != nullptr);
    CHECK(device.find("is_gamepad") != nullptr);
    const JsonValue* space = device.find("space");
    REQUIRE(space != nullptr);
    CHECK((space->as_string() == "gamepad" || space->as_string() == "joystick"));
    CHECK(number(device, "axes") <= 255);
    CHECK(number(device, "buttons") <= 255);
    CHECK(number(device, "hats") <= 255);
    MESSAGE(line);
  }
}

TEST_CASE("engine-input: a one-second probe ends with a summary line") {
  const TempDir tmp("engine_input_app");
  const std::string log_path = io::join_path(tmp.path, "probe.jsonl");
  const Run run = engine_input({"probe", "--seconds", "1", "--log", log_path});
  if (run.exit_code == 3) {
    MESSAGE("engine-input unavailable here (no display)");
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  const std::vector<std::string> lines = lines_of(run.output);
  REQUIRE_FALSE(lines.empty());
  // Every line is JSON; the last one is the summary.
  for (const std::string& line : lines) {
    JsonValue value;
    REQUIRE_MESSAGE(parse_json(line, value).ok, line);
  }
  JsonValue summary;
  REQUIRE_MESSAGE(parse_json(lines.back(), summary).ok, lines.back());
  // No device is needed: zero events is a valid answer, and the counts must still be there.
  const u64 events = number(summary, "events");
  REQUIRE(summary.find("per_device") != nullptr);
  CHECK(summary.find("per_device")->is_object());
  CHECK(number(summary, "axes_seen") <= events);
  CHECK(number(summary, "buttons_seen") <= events);
  CHECK(number(summary, "hats_seen") <= events);
  const JsonValue* seconds = summary.find("seconds");
  REQUIRE(seconds != nullptr);
  f64 elapsed = 0.0;
  REQUIRE(seconds->get_f64(elapsed));
  CHECK(elapsed >= 0.9);
  // --log wrote a file even with nothing to record, and what it wrote loads back as an input
  // log with exactly the number of events the summary claims.
  REQUIRE(summary.find("log") != nullptr);
  CHECK(summary.find("log")->as_string() == log_path);
  CHECK(io::exists(log_path));
  input::InputLog recorded;
  std::string error;
  REQUIRE_MESSAGE(recorded.load(log_path, &error) == io::Status::Ok, error);
  CHECK(recorded.size() == number(summary, "log_events"));
}

// --events exists because a shell redirect was not good enough: PowerShell's `>` wrote the
// probe's stream as UTF-16 with a BOM, and the files that came back that way were unreadable to
// every text tool that met them. So the probe opens the file itself, and this checks the bytes.
TEST_CASE("engine-input: probe --events writes UTF-8 lines to a file of its own") {
  const TempDir tmp("engine_input_app_events");
  const std::string events_path = io::join_path(tmp.path, "probe.events.jsonl");
  const Run run = engine_input({"probe", "--seconds", "1", "--events", events_path});
  if (run.exit_code == 3) {
    MESSAGE("engine-input unavailable here (no display)");
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);

  // The event stream went to the file, so stdout carries the summary and nothing else.
  const std::vector<std::string> printed = lines_of(run.output);
  REQUIRE(printed.size() == 1);
  JsonValue summary;
  REQUIRE_MESSAGE(parse_json(printed[0], summary).ok, printed[0]);
  REQUIRE(summary.find("events_file") != nullptr);
  CHECK(summary.find("events_file")->as_string() == events_path);

  std::string bytes;
  REQUIRE(io::read_file(events_path, bytes) == io::Status::Ok);
  REQUIRE_FALSE(bytes.empty());
  // UTF-8, not UTF-16: no byte order mark of any width, and no embedded NULs.
  CHECK(bytes.find('\0') == std::string::npos);
  CHECK_FALSE(bytes.compare(0, 3, "\xEF\xBB\xBF") == 0);
  CHECK_FALSE(bytes.compare(0, 2, "\xFF\xFE") == 0);
  CHECK_FALSE(bytes.compare(0, 2, "\xFE\xFF") == 0);
  CHECK(bytes.find('\r') == std::string::npos);  // "wb", so the newlines stay bare
  const std::vector<std::string> written = lines_of(bytes);
  REQUIRE_FALSE(written.empty());
  for (const std::string& line : written) {
    JsonValue value;
    REQUIRE_MESSAGE(parse_json(line, value).ok, line);
  }
  // The file ends with the same summary, the way redirecting the whole stream used to.
  CHECK(written.back() == printed[0]);

  // A file that cannot be opened is an error, and the probe says so before it waits a second.
  const std::string nowhere = io::join_path(io::join_path(tmp.path, "missing"), "events.jsonl");
  CHECK(engine_input({"probe", "--seconds", "1", "--events", nowhere}).exit_code == 1);
}

TEST_CASE("engine-input: replay of a hand-written wheel log reports the expected counts") {
  const TempDir tmp("engine_input_app_replay");
  const std::string path = io::join_path(tmp.path, "wheel.jsonl");

  // A shifter gate held for two ticks and a wheel axis swung past the analog button threshold
  // and back: two presses and two releases, over four ticks, from two distinct signals.
  input::InputLog log;
  log.record(input::RawEvent{SimTick{1}, input::Source::JoystickButton, 2, 1.0f, 0});
  log.record(input::RawEvent{SimTick{2}, input::Source::JoystickAxis, 0, 0.75f, 0});
  log.record(input::RawEvent{SimTick{3}, input::Source::JoystickButton, 2, 0.0f, 0});
  log.record(input::RawEvent{SimTick{4}, input::Source::JoystickAxis, 0, 0.0f, 0});
  REQUIRE(log.save(path) == io::Status::Ok);

  const Run run = engine_input({"replay", path});
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  const std::vector<std::string> lines = lines_of(run.output);
  REQUIRE(lines.size() == 1);
  JsonValue summary;
  REQUIRE_MESSAGE(parse_json(lines[0], summary).ok, lines[0]);
  CHECK(number(summary, "events") == 4);
  CHECK(number(summary, "first_tick") == 1);
  CHECK(number(summary, "last_tick") == 4);
  CHECK(number(summary, "ticks") == 4);
  CHECK(number(summary, "actions") == 2);
  CHECK(number(summary, "button_actions") == 1);
  CHECK(number(summary, "axis_actions") == 1);
  CHECK(number(summary, "presses") == 2);
  CHECK(number(summary, "releases") == 2);
  REQUIRE(summary.find("map") != nullptr);
  REQUIRE(summary.find("state") != nullptr);

  // A hat records four signals per event, one per direction, and only the bound one edges.
  input::InputLog hat_log;
  const u32 directions[4] = {input::k_hat_up, input::k_hat_right, input::k_hat_down,
                             input::k_hat_left};
  for (u64 tick = 1; tick <= 2; ++tick) {
    for (u32 direction : directions) {
      hat_log.record(input::RawEvent{SimTick{tick}, input::Source::JoystickHat,
                                     input::hat_code(0, direction),
                                     tick == 1 && direction == input::k_hat_left ? 1.0f : 0.0f, 0});
    }
  }
  const std::string hat_path = io::join_path(tmp.path, "hat.jsonl");
  REQUIRE(hat_log.save(hat_path) == io::Status::Ok);
  const Run hats = engine_input({"replay", hat_path});
  REQUIRE_MESSAGE(hats.exit_code == 0, hats.output);
  JsonValue hat_summary;
  REQUIRE(parse_json(lines_of(hats.output).back(), hat_summary).ok);
  CHECK(number(hat_summary, "events") == 8);
  CHECK(number(hat_summary, "actions") == 4);  // one per direction of hat 0
  CHECK(number(hat_summary, "button_actions") == 4);
  CHECK(number(hat_summary, "axis_actions") == 0);
  CHECK(number(hat_summary, "presses") == 1);
  CHECK(number(hat_summary, "releases") == 1);

  // A log that is not there is an error, not a usage problem.
  CHECK(engine_input({"replay", io::join_path(tmp.path, "missing.jsonl")}).exit_code == 1);
}

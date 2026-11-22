// End to end: a game's sound (game_sound.h; docs/subsystems/apps.md "Sound a module plays";
// roadmap S111). A scene of this test's own declares a generated chirp; the fixture module plays it
// at its spring once a second; one replayed walk is run with `--audio null` (its WAV kept), with
// `--audio off` and with no `--audio` at all (the offscreen default, off), and all three reach the
// same trajectory and the same module state, because what a module hears does not depend on
// whether anything is heard (ADR-0079 decision 5). Every run is offscreen and none opens an audio
// device: the null backend's samples are the ears (view_process.h).

#include "../fly_camera.h"
#include "view_process.h"

#include <core/json/json.h>
#include <foundation/input/input_log.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace engine;

#if ENGINE_VIEW_GAME && ENGINE_VIEW_WORLD_SOUND && defined(WALK_POC_PATH)
#include "../game_sound.h"

// The forest's need 169 (S112): the owner's launcher always passes `--benchmark` to get frame
// records, and `auto` read that as "nobody hears this", so their window was silent. `auto` is keyed
// on the window alone: main.cpp hands `resolve_audio` `!offscreen` and nothing else.
TEST_CASE("engine-view: --audio auto is the device in a window, whatever the session records") {
  world_sound::AudioMode mode = world_sound::AudioMode::Null;
  std::string device = "stale";
  REQUIRE(view::resolve_audio("auto", true, mode, device, nullptr));
  CHECK(mode == world_sound::AudioMode::Device);
  CHECK(device.empty());  // the default device
  REQUIRE(view::resolve_audio("auto", false, mode, device, nullptr));
  CHECK(mode == world_sound::AudioMode::Off);  // offscreen: silent
  REQUIRE(view::resolve_audio("null", true, mode, device, nullptr));
  CHECK(mode == world_sound::AudioMode::Null);
  REQUIRE(view::resolve_audio("Speakers (Realtek)", false, mode, device, nullptr));
  CHECK(mode == world_sound::AudioMode::Device);
  CHECK(device == "Speakers (Realtek)");
  std::string error;
  CHECK_FALSE(view::resolve_audio("", true, mode, device, &error));
  CHECK(error.find("--audio") != std::string::npos);
}

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
};

Run run_view(const std::vector<std::string>& args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  Run run;
  run.exit_code = view_test::run_program(
      std::span<const std::string_view>(argv.data(), argv.size()), run.output);
  return run;
}

bool summary_of(const Run& run, JsonValue& out) {
  usize end = run.output.size();
  while (end > 0) {
    usize begin = run.output.find_last_of('\n', end - 1);
    begin = begin == std::string::npos ? 0 : begin + 1;
    const std::string line = run.output.substr(begin, end - begin);
    if (!line.empty() && line[0] == '{' && parse_json(line, out).ok && out.is_object()) return true;
    if (begin == 0) break;
    end = begin - 1;
  }
  return false;
}

const JsonValue* at(const JsonValue& v, std::initializer_list<const char*> path) {
  const JsonValue* p = &v;
  for (const char* key : path)
    p = p != nullptr && p->is_object() ? p->find(key) : nullptr;
  return p;
}

f64 number_at(const JsonValue& v, std::initializer_list<const char*> path) {
  const JsonValue* p = at(v, path);
  f64 out = -1.0;
  return p != nullptr && p->get_f64(out) ? out : -1.0;
}

std::string text_at(const JsonValue& v, std::initializer_list<const char*> path) {
  const JsonValue* p = at(v, path);
  return p != nullptr ? write_json(*p) : std::string("(none)");
}

std::string fixture(const test::TempDir& tmp, const char* built) {
  const std::filesystem::path from(built);
  const std::string to = tmp.file("walk_poc" + from.extension().string());
  std::error_code ec;
  std::filesystem::copy_file(from, std::filesystem::path(to),
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_MESSAGE(!ec, "copying " << built << ": " << ec.message());
  return to;
}

// The renderer's own waves and one generated chirp: no capability is needed to draw it and no
// binary to hear it.
std::string sound_scene(const test::TempDir& tmp) {
  const std::string scene = tmp.file("sound.json");
  REQUIRE(io::write_file(scene, R"({"format":"engine.scene.v1","name":"sound",
      "terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30},
      "sound":{"clips":[{"name":"chirp","recipe":{"kind":"Chirp"},"min_m":2,"max_m":80}]}})") ==
          io::Status::Ok);
  return scene;
}

// Standing at (0, 3, 10) looking along -z, W held from tick 10, 480 ticks: two seconds, in which
// the fixture's spring 25 m ahead chirps at ticks 120 and 360.
std::string walk_session(const test::TempDir& tmp, const std::string& scene) {
  input::InputLog keys;
  keys.set_map(view::default_fly_map());
  keys.record(input::RawEvent{SimTick{10}, input::Source::Key, 26, 1.0f, 0});  // W
  view::SessionHeader header;
  header.ticks = 480;
  header.scene = scene;
  header.start.position = WorldPos{0.0, 3.0, 10.0};
  header.start.yaw = 0.0f;
  header.start.pitch = -0.15f;
  header.has_walk = true;
  header.walking = true;
  keys.set_session(view::session_to_json(header));
  const std::string path = tmp.file("walk.jsonl");
  REQUIRE(keys.save(path) == io::Status::Ok);
  return path;
}

}  // namespace

TEST_CASE(
    "engine-view: a game's chirp is heard in the null render, and a replay heard, muted and "
    "with no --audio reaches the same state") {
  const test::TempDir tmp("engine_view_game_sound");
  const std::string scene = sound_scene(tmp);
  const std::string session = walk_session(tmp, scene);
  const std::string module = fixture(tmp, WALK_POC_PATH);
  const std::string wav = tmp.file("heard.wav");
  const std::vector<std::string> common = {
      "--replay-input", session, "--scene", scene,  "--offscreen", "--width",      "320",
      "--height",       "180",   "--game",  module, "--ddc",       tmp.file("ddc")};
  std::vector<std::string> args = common;
  args.insert(args.end(), {"--audio", "null", "--audio-out", wav});
  const Run heard = run_view(args);
  if (heard.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << heard.output);
    return;
  }
  REQUIRE_MESSAGE(heard.exit_code == 0, heard.output);
  args = common;
  args.insert(args.end(), {"--audio", "off"});
  const Run muted = run_view(args);
  REQUIRE_MESSAGE(muted.exit_code == 0, muted.output);
  const Run absent = run_view(common);  // offscreen: auto is off
  REQUIRE_MESSAGE(absent.exit_code == 0, absent.output);

  JsonValue a;
  JsonValue b;
  JsonValue c;
  REQUIRE_MESSAGE(summary_of(heard, a), heard.output);
  REQUIRE_MESSAGE(summary_of(muted, b), muted.output);
  REQUIRE_MESSAGE(summary_of(absent, c), absent.output);
  const JsonValue* sound = at(a, {"interactive", "game", "sound"});
  REQUIRE_MESSAGE(sound != nullptr, heard.output);
  MESSAGE("heard: " << write_json(*sound));
  CHECK(text_at(a, {"interactive", "game", "sound", "mode"}) == "\"null\"");
  CHECK(text_at(b, {"interactive", "game", "sound", "mode"}) == "\"off\"");
  CHECK(text_at(c, {"interactive", "game", "sound", "mode"}) == "\"off\"");
  // The commands, counted: two chirps asked for and two voices started where sound is heard, and
  // the same two asked for where it is not.
  CHECK(number_at(*sound, {"plays"}) == 2.0);
  CHECK(number_at(*sound, {"voices_started"}) == 2.0);
  CHECK(number_at(*sound, {"listener_moves"}) > 100.0);  // the walker moves the ears
  CHECK(number_at(a, {"interactive", "game", "sound", "frames"}) == 480.0 * 200.0);
  CHECK(number_at(b, {"interactive", "game", "sound", "plays"}) == 2.0);
  CHECK(number_at(b, {"interactive", "game", "sound", "voices_started"}) == 0.0);
  CHECK(number_at(b, {"interactive", "game", "sound", "frames"}) == 0.0);
  // The same walk and the same module state, to the bit, in all three.
  const std::string trajectory = text_at(a, {"interactive", "trajectory", "hash"});
  CHECK(trajectory != "(none)");
  CHECK(text_at(b, {"interactive", "trajectory", "hash"}) == trajectory);
  CHECK(text_at(c, {"interactive", "trajectory", "hash"}) == trajectory);
  const std::string state = text_at(a, {"interactive", "game", "state"});
  CHECK(number_at(a, {"interactive", "game", "state", "chirps"}) == 2.0);
  CHECK(number_at(a, {"interactive", "game", "state", "chirp_voice"}) == 2.0);
  CHECK(text_at(b, {"interactive", "game", "state"}) == state);
  CHECK(text_at(c, {"interactive", "game", "state"}) == state);
  CHECK(text_at(b, {"interactive", "walk", "hash"}) == text_at(a, {"interactive", "walk", "hash"}));

  // The WAV: every frame of the session, stereo float, silent but for the two chirps.
  std::string bytes;
  REQUIRE(io::read_file(wav, bytes) == io::Status::Ok);
  REQUIRE(bytes.size() == 44u + 96000u * 2u * 4u);
  std::vector<f32> samples((bytes.size() - 44) / 4);
  std::memcpy(samples.data(), bytes.data() + 44, samples.size() * sizeof(f32));
  // Energy by quarter second (12,000 frames): the chirps land in the quarters holding ticks 120
  // (frame 24,000) and 360 (frame 72,000), and each rings 0.18 s.
  f64 quarter[8] = {};
  for (usize f = 0; f < 96000; ++f) {
    const f64 l = static_cast<f64>(samples[2 * f]);
    const f64 r = static_cast<f64>(samples[2 * f + 1]);
    quarter[f / 12000] += l * l + r * r;
  }
  MESSAGE("energy by quarter second: " << quarter[0] << " " << quarter[1] << " " << quarter[2]
                                       << " " << quarter[3] << " " << quarter[4] << ' '
                                       << quarter[5] << " " << quarter[6] << " " << quarter[7]);
  CHECK(quarter[2] > 1.0);
  CHECK(quarter[6] > 1.0);
  // Ticks 120 and 360 are drawn into the 200 frames before 24,000 and 72,000: the chirps start
  // one block before their quarter and are silent before it, and nothing sounds between them.
  for (u32 q : {0u, 3u, 4u, 7u})
    CHECK(quarter[q] == 0.0);
  CHECK(quarter[1] < 0.01 * quarter[2]);
  CHECK(quarter[5] < 0.01 * quarter[6]);
  // The spring is ahead and a little to no side: the two channels carry the chirp nearly alike.
  f64 left = 0.0;
  f64 right = 0.0;
  for (usize f = 24000; f < 36000; ++f) {
    left += static_cast<f64>(samples[2 * f]) * static_cast<f64>(samples[2 * f]);
    right += static_cast<f64>(samples[2 * f + 1]) * static_cast<f64>(samples[2 * f + 1]);
  }
  CHECK(left / right == doctest::Approx(1.0).epsilon(0.05));
}

namespace {

// S112: two small oaks of the flora's own (leaves two-sided and translucent, no image), a scene
// wind of `speed` m/s, and no `sound` block: the world's sound alone.
std::string grove_scene(const test::TempDir& tmp, const char* name, f64 speed) {
  REQUIRE(io::write_file(tmp.file("oak.flora.json"),
                         R"({"species":"broadleaf","broadleaf":{"seed":7,"height_m":8.0,)"
                         R"("trunk_radius_m":0.25,"triangle_budget":12000}})") == io::Status::Ok);
  const std::string scene = tmp.file(name);
  const std::string text =
      std::string(R"({"format":"engine.scene.v1","name":"grove",)") +
      R"("terrain":{"size":65,"extent":64,"seed":3,"dune_height":0.2,"dune_wavelength":30},)" +
      R"("sky":{"latitude_deg":45,"day_of_year":170,"wind":{"speed_m_s":)" + std::to_string(speed) +
      R"(,"to_deg":90}},"meshes":[{"name":"oak","path":"oak.flora.json"}],"instances":[)" +
      R"({"ground":true,"translation":[4,0,-6],"mesh":0},)" +
      R"({"ground":true,"translation":[-7,0,-14],"yaw_deg":80,"mesh":0}]})";
  REQUIRE(io::write_file(scene, text) == io::Status::Ok);
  return scene;
}

f64 wav_power(const std::string& path) {
  std::string bytes;
  REQUIRE(io::read_file(path, bytes) == io::Status::Ok);
  REQUIRE(bytes.size() > 44u);
  std::vector<f32> samples((bytes.size() - 44) / 4);
  std::memcpy(samples.data(), bytes.data() + 44, samples.size() * sizeof(f32));
  f64 sum = 0.0;
  for (const f32 x : samples)
    sum += static_cast<f64>(x) * static_cast<f64>(x);
  return sum / static_cast<f64>(samples.size());
}

}  // namespace

TEST_CASE("engine-view: the wind in a grove is heard in the null render, and calm is silent") {
  const test::TempDir tmp("engine_view_grove_sound");
  const std::string module = fixture(tmp, WALK_POC_PATH);
  struct Leg {
    const char* name;
    f64 speed;
    std::string wav;
    JsonValue summary;
  };
  Leg legs[3] = {{"calm.json", 0.0, tmp.file("calm.wav"), {}},
                 {"light.json", 3.0, tmp.file("light.wav"), {}},
                 {"strong.json", 9.0, tmp.file("strong.wav"), {}}};
  for (Leg& leg : legs) {
    const std::string scene = grove_scene(tmp, leg.name, leg.speed);
    const std::string session = walk_session(tmp, scene);
    const Run run = run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--width",
                              "320", "--height", "180", "--game", module, "--ddc", tmp.file("ddc"),
                              "--audio", "null", "--audio-out", leg.wav});
    if (run.exit_code == 3) {
      MESSAGE("engine-view unavailable here: " << run.output);
      return;
    }
    REQUIRE_MESSAGE(run.exit_code == 0, run.output);
    REQUIRE_MESSAGE(summary_of(run, leg.summary), run.output);
    MESSAGE(
        std::string(leg.name) << ": "
                              << text_at(leg.summary, {"interactive", "game", "sound", "world"}));
  }
  // The scene's two oaks are the canopy table's crowns in every leg.
  for (const Leg& leg : legs)
    CHECK(number_at(leg.summary, {"interactive", "game", "sound", "world", "crowns_seen"}) == 2.0);
  // Calm: no world voice was ever started, and the render is silent.
  CHECK(number_at(legs[0].summary, {"interactive", "game", "sound", "world", "plays"}) == 0.0);
  CHECK(wav_power(legs[0].wav) == 0.0);
  // In a wind: both crowns and the two ears sound, and the strong wind is the louder.
  for (u32 i = 1; i < 3; ++i) {
    CHECK(number_at(legs[i].summary, {"interactive", "game", "sound", "world", "crown_voices"}) ==
          2.0);
    CHECK(number_at(legs[i].summary, {"interactive", "game", "sound", "world", "ear_voices"}) ==
          2.0);
  }
  const f64 light = wav_power(legs[1].wav);
  const f64 strong = wav_power(legs[2].wav);
  MESSAGE("light " << 10.0 * std::log10(light) << " dBFS, strong " << 10.0 * std::log10(strong)
                   << " dBFS");
  CHECK(light > 0.0);
  CHECK(10.0 * std::log10(strong / light) > 12.0);  // 30 log 3 for the trees, 40 log 3 the ears
}

TEST_CASE("engine-view: --audio-out needs the null backend") {
  const Run run = run_view({"--audio", "off", "--audio-out", "x.wav", "--offscreen"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("--audio-out keeps the null backend's render") != std::string::npos);
}

#endif  // ENGINE_VIEW_GAME && ENGINE_VIEW_WORLD_SOUND && WALK_POC_PATH

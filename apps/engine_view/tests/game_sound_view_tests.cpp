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
#include <test_environment.h>
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

// **Who `--audio auto` plays to** (S112-3, N1 of S112's second review; apps.md, "Sound a module
// plays"): the speakers only for a windowed run somebody attends — `--interactive`, a game module
// (the owner's launcher passes `--benchmark` beside one, the forest's need 169), or a run until its
// window is closed — and never under a test or a bench, whatever the flags. The table, with no
// window: every row the one function main.cpp asks.
TEST_CASE("engine-view: --audio auto is the device only for somebody at the window, never a test") {
  using world_sound::AudioMode;
  struct Row {
    const char* flag;
    bool windowed, interactive, module, ends, ticked, scene;
    const char* env;
    AudioMode want;
  };
  const Row rows[] = {
      // auto, unset: the attended window is the device; everything else is off.
      {"auto", true, true, false, false, true, true, "", AudioMode::Device},  // live session
      {"auto", true, true, false, true, true, true, "", AudioMode::Device},   // ... with --frames
      {"auto", true, false, true, true, true, true, "", AudioMode::Device},   // module, --benchmark
      {"auto", true, false, false, false, true, true, "", AudioMode::Device},  // until closed
      {"auto", true, false, false, true, true, true, "", AudioMode::Off},      // --frames N
      {"auto", true, false, false, false, false, true, "", AudioMode::Off},    // nothing ticks
      {"auto", true, true, false, false, true, false, "", AudioMode::Off},     // no scene
      {"auto", false, false, false, true, true, true, "", AudioMode::Off},     // offscreen
      {"auto", false, true, true, false, true, true, "", AudioMode::Off},      // offscreen replay
      // auto under a test or a bench: never the device.
      {"auto", true, true, false, false, true, true, "null", AudioMode::Null},
      {"auto", true, false, true, true, true, true, "null", AudioMode::Null},
      {"auto", true, true, false, false, true, true, "off", AudioMode::Off},
      {"auto", true, true, false, false, true, true, "1", AudioMode::Null},  // any other word
      {"auto", false, false, false, true, true, true, "null", AudioMode::Off},
      // Explicit words are themselves, whatever the run and the variable.
      {"off", true, true, true, false, true, true, "", AudioMode::Off},
      {"null", false, false, false, true, false, false, "", AudioMode::Null},
      {"null", true, true, false, false, true, true, "null", AudioMode::Null},
      {"Speakers (Realtek)", false, false, false, true, false, false, "null", AudioMode::Device},
  };
  for (const Row& r : rows) {
    view::AudioRun run;
    run.windowed = r.windowed;
    run.interactive = r.interactive;
    run.module = r.module;
    run.ends_by_itself = r.ends;
    run.ticked = r.ticked;
    run.scene = r.scene;
    run.auto_env = r.env;
    AudioMode mode = AudioMode::Device;
    std::string device = "stale";
    REQUIRE(view::resolve_audio(r.flag, run, mode, device, nullptr));
    CAPTURE(r.flag);
    CAPTURE(r.env);
    CAPTURE(&r - rows);
    CHECK(mode == r.want);
    CHECK(device == (mode == AudioMode::Device && std::string_view(r.flag) != "auto"
                         ? std::string(r.flag)
                         : std::string()));
  }
  std::string error;
  AudioMode mode = AudioMode::Off;
  std::string device;
  CHECK_FALSE(view::resolve_audio("", view::AudioRun{}, mode, device, &error));
  CHECK(error.find("--audio") != std::string::npos);
  // The variable is the test main's, in this process and every one it starts (test_main.cpp).
  CHECK(std::string_view(view::k_audio_auto_env) == test::k_audio_auto_env);
  CHECK(view::audio_auto_env() == "null");
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

namespace {

// A module-less run's sound summary: its own stderr line (main.cpp, S112-2).
bool sound_summary_of(const Run& run, JsonValue& out) {
  const std::string tag = "engine-view: sound summary ";
  const usize at = run.output.find(tag);
  if (at == std::string::npos) return false;
  const usize end = run.output.find('\n', at);
  return parse_json(
             run.output.substr(at + tag.size(), end == std::string::npos ? std::string::npos
                                                                         : end - at - tag.size()),
             out)
             .ok &&
         out.is_object();
}

// **A run's last line is its JSON summary**, the sound's two stderr lines before it: thirty test
// files and every script that reads a run take the summary from the last line, and batch 72's gate
// was red on a destructor that printed these two after it.
bool summary_is_last_line(const Run& run) {
  if (run.output.size() < 2) return false;
  const usize start = run.output.find_last_of('\n', run.output.size() - 2);
  const std::string last = run.output.substr(start == std::string::npos ? 0 : start + 1);
  JsonValue value;
  return parse_json(last, value).ok && value.is_object() && value.find("frames") != nullptr &&
         run.output.find("engine-view: sound table:") < (start == std::string::npos ? 0 : start);
}

}  // namespace

// S112-2 (the review's item 4): the host's hooks no longer need a module, so a scene played with no
// `--game` hears its world; and a mesh that declares itself `None` is not heard, whatever its
// leaves (the review's M6: the class is the asset's own word, never a name).
TEST_CASE(
    "engine-view: a scene with no game module hears its grove, and a mesh declared None "
    "is silent") {
  const test::TempDir tmp("engine_view_solo_sound");
  struct Leg {
    const char* name;
    bool declared_none;
    std::string wav;
    JsonValue summary;
  };
  Leg legs[2] = {{"heard.json", false, tmp.file("heard.wav"), {}},
                 {"none.json", true, tmp.file("none.wav"), {}}};
  for (Leg& leg : legs) {
    std::string scene = grove_scene(tmp, leg.name, 9.0);
    if (leg.declared_none) {
      std::string text;
      REQUIRE(io::read_file(scene, text) == io::Status::Ok);
      const std::string from = R"("path":"oak.flora.json"})";
      const usize at = text.find(from);
      REQUIRE(at != std::string::npos);
      text.replace(at, from.size(), R"("path":"oak.flora.json","sound":{"canopy":"None"}})");
      REQUIRE(io::write_file(scene, text) == io::Status::Ok);
    }
    const std::string session = walk_session(tmp, scene);
    const Run run = run_view({"--replay-input", session, "--scene", scene, "--offscreen", "--width",
                              "320", "--height", "180", "--ddc", tmp.file("ddc"), "--audio", "null",
                              "--audio-out", leg.wav});
    if (run.exit_code == 3) {
      MESSAGE("engine-view unavailable here: " << run.output);
      return;
    }
    REQUIRE_MESSAGE(run.exit_code == 0, run.output);
    REQUIRE_MESSAGE(sound_summary_of(run, leg.summary), run.output);
    CHECK_MESSAGE(summary_is_last_line(run), run.output);
    MESSAGE(std::string(leg.name) << ": " << text_at(leg.summary, {"world"}));
  }
  CHECK(number_at(legs[0].summary, {"world", "crowns_seen"}) == 2.0);
  CHECK(number_at(legs[0].summary, {"world", "crown_voices"}) == 2.0);
  CHECK(wav_power(legs[0].wav) > 0.0);
  CHECK(number_at(legs[1].summary, {"world", "crowns_seen"}) == 0.0);  // declared None
  CHECK(number_at(legs[1].summary, {"world", "ear_voices"}) == 2.0);   // the wind at the ears
}

// N1 end to end (S112-3): a live window under the test main, `--interactive` and no `--audio` —
// the run that would have played through the machine's speakers — is the null backend, because
// the test main set `ENGINE_AUDIO_AUTO` and engine-view inherited it. The one windowed case here.
TEST_CASE("engine-view: a live window under a test hears its world on the null backend") {
  const test::TempDir tmp("engine_view_window_no_device");
  const std::string scene = grove_scene(tmp, "grove.json", 9.0);
  const std::string inject = walk_session(tmp, scene);
  const Run run = run_view({"--interactive", "--walk", "--start", "0,3,10", "0,-8.6",
                            "--inject-input", inject, "--scene", scene, "--width", "160",
                            "--height", "96", "--no-vsync", "--ddc", tmp.file("ddc")});
  if (run.exit_code == 3) {
    MESSAGE("engine-view unavailable here: " << run.output);
    return;
  }
  REQUIRE_MESSAGE(run.exit_code == 0, run.output);
  JsonValue sound;
  REQUIRE_MESSAGE(sound_summary_of(run, sound), run.output);
  CHECK_MESSAGE(summary_is_last_line(run), run.output);
  CHECK(text_at(sound, {"mode"}) == "\"null\"");
  CHECK(text_at(sound, {"backend"}) == "\"null\"");
  CHECK(number_at(sound, {"world", "ear_voices"}) == 2.0);  // the world modelled all the same
}

TEST_CASE("engine-view: --audio-out needs the null backend") {
  const Run run = run_view({"--audio", "off", "--audio-out", "x.wav", "--offscreen"});
  CHECK(run.exit_code == 2);
  CHECK(run.output.find("--audio-out keeps the null backend's render") != std::string::npos);
}

#endif  // ENGINE_VIEW_GAME && ENGINE_VIEW_WORLD_SOUND && WALK_POC_PATH

#if ENGINE_VIEW_GAME && ENGINE_VIEW_WORLD_SOUND
#include "../game_sound.h"

#include <core/memory/memory.h>
#include <domain/geometry/flora_species.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/scene.h>

namespace {

// Drawn tiles of the case's own: `count` placements each, every other one a tree (mesh 0) and the
// rest a mesh with no class (a decal's alpha cards, mesh 1), across the tile's 64 m.
struct Tiles {
  std::vector<u64> keys;
  std::vector<u64> versions;
  std::vector<std::vector<renderer::SceneInstance>> placed;
  void add(u64 key, f64 x0, u32 count) {
    keys.push_back(key);
    versions.push_back(1);
    std::vector<renderer::SceneInstance> p(count);
    for (u32 i = 0; i < count; ++i) {
      p[i].mesh = i % 2;
      p[i].origin = WorldPos{x0 + 0.6 * i, 0.0, 0.3 * (i % 7)};
    }
    placed.push_back(std::move(p));
  }
  static void list(const void* user, Vector<view::SoundTile>& out) {
    const Tiles& t = *static_cast<const Tiles*>(user);
    out.clear();
    for (usize i = 0; i < t.keys.size(); ++i)
      out.push_back(view::SoundTile{t.keys[i], t.placed[i].data(),
                                    static_cast<u32>(t.placed[i].size()), t.versions[i]});
  }
};

struct Count {
  u64 crowns = 0;
  u64 merged = 0;
  static void emit(void* user, const world_sound::Crown& c) noexcept {
    Count& n = *static_cast<Count*>(user);
    n.crowns += c.merged ? 0u : 1u;
    n.merged += c.merged ? 1u : 0u;
  }
};

}  // namespace

// **H1 of S112's review**: a refresh rebuilt the tiles' whole tail and walked every hand-off tile's
// placements at 8 Hz on the frame's thread. Now it lists the tiles by key, walks a tile's
// placements only when its version moves, within an allowance a refresh, and allocates nothing in
// a steady state: held by counts, never by a clock.
TEST_CASE("engine-view: a sound refresh walks what changed and nothing else") {
  renderer::SceneData scene;
  scene.mesh_leaves.push_back(renderer::MeshLeaves{60.0f, 2.0f, 10.0f, 0.0f, 0.0f, 3.0f});
  scene.mesh_leaves.push_back(renderer::MeshLeaves{5.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f});
  scene.sources.resize(2);
  scene.sources[0].species = "oak";  // a generated tree's container names it; mesh 1 says nothing
  scene.mesh_info.resize(2);
  view::GameSoundHost host;
  host.attach_scene(scene);
  Tiles tiles;
  tiles.add(10, 0.0, 100);
  tiles.add(20, 64.0, 100);
  tiles.add(30, 128.0, 100);
  host.attach_tile_list(&Tiles::list, &tiles);
  const WorldPos centre{10.0, 1.7, 0.0};
  const auto refresh = [&]() {
    Count n;
    view::GameSoundHost::visit(&host, centre, 2000.0, world_sound::CrownSink{&n, &Count::emit});
    return n;
  };
  // The first refresh walks every tile once; mesh 1 has no class and is never a crown.
  Count first = refresh();
  CHECK(host.counters().instances_walked == 300u);
  CHECK(host.counters().tiles_walked == 3u);
  CHECK(first.crowns + first.merged > 0u);
  CHECK(first.crowns <= 150u);
  // An unchanged world walks nothing, and allocates nothing.
  (void)refresh();
  const u64 allocations = mem::allocations_on_thread();
  for (u32 i = 0; i < 8; ++i)
    (void)refresh();
  CHECK(mem::allocations_on_thread() == allocations);
  CHECK(host.counters().last_walked == 0u);
  CHECK(host.counters().instances_walked == 300u);
  // One tile's change walks that tile's placements and no other.
  ++tiles.versions[1];
  (void)refresh();
  CHECK(host.counters().last_walked == 100u);
  CHECK(host.counters().instances_walked == 400u);
  // A tile let go and another drawn: only the new one is walked.
  tiles.keys[2] = 40;
  tiles.versions[2] = 7;
  (void)refresh();
  CHECK(host.counters().last_walked == 100u);
  CHECK(host.counters().tile_groups == 3u);
  // The allowance: every tile changed at once, 100 placements a refresh, the rest carried.
  tunables::Tunable* walk = tunables::find("audio.world.walk_instances");
  REQUIRE(walk != nullptr);
  REQUIRE(walk->set_from_text("100"));
  for (u64& v : tiles.versions)
    ++v;
  const u64 carried = host.counters().tiles_carried;
  (void)refresh();
  CHECK(host.counters().last_walked == 100u);
  CHECK(host.counters().tiles_carried == carried + 2u);
  (void)refresh();
  (void)refresh();
  CHECK(host.counters().instances_walked == 800u);
  (void)refresh();
  CHECK(host.counters().last_walked == 0u);  // all made again
  walk->reset();
  // Nearest first (M1): with room for one tile's crowns, the listener's own tile is the one sent
  // one by one and the rest merged.
  tunables::Tunable* near = tunables::find("audio.world.near_crowns");
  REQUIRE(near != nullptr);
  REQUIRE(near->set_from_text("50"));
  const Count tight = refresh();
  CHECK(tight.crowns == 50u);
  CHECK(tight.merged >= 1u);
  near->reset();
}

// **N2 of S112's second review**: changed tiles are walked nearest the listener first, not in key
// order, so after a load or a cut the listener's own trees are made first. A far tile with the
// smaller key and a near one arrive in one refresh with room for one: the near one is walked (its
// crowns sent one by one), the far one carried and silent until the next refresh walks it.
TEST_CASE("engine-view: a sound refresh walks the changed tile nearest the listener first") {
  renderer::SceneData scene;
  scene.mesh_leaves.push_back(renderer::MeshLeaves{60.0f, 2.0f, 10.0f, 0.0f, 0.0f, 3.0f});
  scene.mesh_leaves.push_back(renderer::MeshLeaves{5.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f});
  scene.sources.resize(2);
  scene.sources[0].species = "oak";
  scene.mesh_info.resize(2);
  view::GameSoundHost host;
  host.attach_scene(scene);
  Tiles tiles;
  tiles.add(10, 1500.0, 100);  // far, and first by key
  tiles.add(20, 0.0, 100);     // under the listener
  host.attach_tile_list(&Tiles::list, &tiles);
  tunables::Tunable* walk = tunables::find("audio.world.walk_instances");
  REQUIRE(walk != nullptr);
  REQUIRE(walk->set_from_text("100"));
  const WorldPos centre{10.0, 1.7, 0.0};
  Count n;
  view::GameSoundHost::visit(&host, centre, 2000.0, world_sound::CrownSink{&n, &Count::emit});
  CHECK(host.counters().tiles_walked == 1u);
  CHECK(host.counters().tiles_carried == 1u);
  CHECK(host.counters().last_walked == 100u);
  CHECK(n.crowns > 0u);   // the near tile's crowns, one by one: it was the one walked
  CHECK(n.merged == 0u);  // the far one not yet made, so nothing of it is heard
  Count next;
  view::GameSoundHost::visit(&host, centre, 2000.0, world_sound::CrownSink{&next, &Count::emit});
  CHECK(host.counters().tiles_walked == 2u);
  CHECK(next.merged > 0u);  // now the far tile, by its merged crowns
  // Both changed again, the far tile's trees gone: the near one is walked first again, so the far
  // one keeps its last crowns this refresh (a walk of it would have left it nothing to send).
  ++tiles.versions[0];
  ++tiles.versions[1];
  for (renderer::SceneInstance& s : tiles.placed[0])
    s.mesh = 1;
  const u64 walked = host.counters().instances_walked;
  Count again;
  view::GameSoundHost::visit(&host, centre, 2000.0, world_sound::CrownSink{&again, &Count::emit});
  CHECK(host.counters().instances_walked == walked + 100u);
  CHECK(host.counters().tiles_carried == 2u);
  CHECK(again.crowns > 0u);
  CHECK(again.merged > 0u);
  Count last;
  view::GameSoundHost::visit(&host, centre, 2000.0, world_sound::CrownSink{&last, &Count::emit});
  CHECK(last.merged == 0u);  // the far tile walked at last: no tree left in it
  walk->reset();
}

TEST_CASE("engine-view: a crown's class is the mesh's own word, then its species, else silence") {
  world_sound::CanopyClass kind = world_sound::CanopyClass::Bare;
  f32 needle = 0.0f;
  constexpr u8 k_none = renderer::SceneMeshInfo::k_sound_undeclared;
  CHECK(view::canopy_class_of(k_none, 0.0f, "pine", kind, needle));
  CHECK(kind == world_sound::CanopyClass::Needle);
  CHECK(needle == 1.7f);
  CHECK(view::canopy_class_of(k_none, 0.0f, "oak", kind, needle));
  CHECK(kind == world_sound::CanopyClass::Blade);
  CHECK_FALSE(view::canopy_class_of(k_none, 0.0f, "", kind, needle));       // a fence, a decal
  CHECK_FALSE(view::canopy_class_of(k_none, 0.0f, "sward", kind, needle));  // the ground's cover
  // S112-3, N3: the species' own declaration (flora_species.h `canopy`), never a name in the app;
  // the grasses, the water plants, the alpine mats and the low shrubs declare no crown.
  for (const char* ground : {"meadow", "sedge", "reed", "water_lily", "silene_acaulis", "bearberry",
                             "big_sagebrush", "fern", "ivy"})
    CHECK_FALSE_MESSAGE(view::canopy_class_of(k_none, 0.0f, ground, kind, needle), ground);
  CHECK(view::canopy_class_of(k_none, 0.0f, "spruce", kind, needle));
  CHECK(kind == world_sound::CanopyClass::Needle);
  CHECK(needle == 1.2f);
  CHECK(view::canopy_class_of(k_none, 0.0f, "beech", kind, needle));
  CHECK(kind == world_sound::CanopyClass::Blade);
  for (const geometry::FloraSpeciesInfo& info : geometry::k_flora_species) {
    // A species that moves as a field or not at all (grass, mats, litter) is never a crown.
    if (info.motion != geometry::FloraMotionKind::parts)
      CHECK_MESSAGE(info.canopy == geometry::FloraCanopy::none, info.name);
  }
  // Declared: wins over the species, its width taken.
  CHECK(view::canopy_class_of(1, 2.5f, "oak", kind, needle));
  CHECK(kind == world_sound::CanopyClass::Needle);
  CHECK(needle == 2.5f);
  CHECK(view::canopy_class_of(2, 0.0f, "", kind, needle));
  CHECK(kind == world_sound::CanopyClass::Bare);
  CHECK_FALSE(view::canopy_class_of(3, 0.0f, "pine", kind, needle));  // None
}

namespace {

// A case's own world for the ears' shelter (S114): ground rising `slope` a metre toward +z from
// y = 0 at z = 0 (0: open flat ground), a wall 3 m high over it and 40 m long across the x axis at
// z 4.8 to 5.2 (when `wall`), and a trunk's proxy 10 m high and 0.3 m round at (0, 10). A downward
// cast meets the highest of them under its origin: the ground as `what` 1, a placement as 2.
struct ShelterWorld {
  f64 slope = 0.0;
  bool wall = true;
};
bool shelter_world(const void* user, WorldPos origin, f32 length, view::WalkCast& out) {
  const ShelterWorld& w = *static_cast<const ShelterWorld*>(user);
  out = view::WalkCast{};
  const f64 ground = w.slope * origin.z;
  f64 top = ground;
  u8 what = 1;
  if (w.wall && origin.z >= 4.8 && origin.z <= 5.2 && std::fabs(origin.x) <= 20.0) {
    top = ground + 3.0;
    what = 2;
  }
  if (std::hypot(origin.x, origin.z - 10.0) <= 0.3) {
    top = ground + 10.0;
    what = 2;
  }
  if (origin.y - top > static_cast<f64>(length)) return true;
  out.hit = true;
  out.what = what;
  out.position = WorldPos{origin.x, top, origin.z};
  return true;
}
bool shelter_ground(const void* user, f64, f64 z, f64& out) {
  out = static_cast<const ShelterWorld*>(user)->slope * z;
  return true;
}

}  // namespace

TEST_CASE("engine-view: the ears' shelter is a wall across the wind, never one trunk") {
  // The wind along z, across it x: three casts a pace apart across the wind each meet the wall,
  // and only the middle one meets the trunk.
  const ShelterWorld flat;
  const void* u = &flat;
  CHECK(view::shelter_height_at(&shelter_world, &shelter_ground, u, 0.0, 0.0, 5.0, 1.0f, 0.0f) ==
        3.0f);
  CHECK(view::shelter_height_at(&shelter_world, &shelter_ground, u, 0.0, 0.0, 10.0, 1.0f, 0.0f) ==
        0.0f);
  CHECK(view::shelter_height_at(&shelter_world, &shelter_ground, u, 0.0, 0.0, 20.0, 1.0f, 0.0f) ==
        0.0f);
  // Measured over the ground at the probe, not over the feet (S114-M1): standing 1 m up a bank,
  // the wall is still its own 3 m; only with no ground under the cast (a mesh floor) is it measured
  // over the feet, 2 m.
  CHECK(view::shelter_height_at(&shelter_world, &shelter_ground, u, 1.0, 0.0, 5.0, 1.0f, 0.0f) ==
        3.0f);
  CHECK(view::shelter_height_at(&shelter_world, nullptr, u, 1.0, 0.0, 5.0, 1.0f, 0.0f) == 2.0f);
  // With the wind along x the casts run along z, off the wall's 0.4 m: no shelter.
  CHECK(view::shelter_height_at(&shelter_world, &shelter_ground, u, 0.0, 0.0, 5.0, 0.0f, 1.0f) ==
        0.0f);
  CHECK(view::shelter_height_at(nullptr, nullptr, u, 0.0, 0.0, 5.0, 1.0f, 0.0f) == 0.0f);
}

TEST_CASE("engine-view: a slope rising into the wind shelters nothing, a wall on it its height") {
  // S114-M1: the ground met by the casts is no obstacle. Measured over the feet, a slope of 1 in 8
  // (7 degrees) read as 0.19 m of shelter at 1.5 m upwind and 3 m at 24 m, and 1 in 4 (14 degrees)
  // took the full lee (0.3 of the open wind, -20.9 dB at the ears) on every dune; the feet stand
  // at the slope's foot, z 0, and the wind comes down the slope toward them along -z.
  for (const f64 slope : {0.125, 0.25}) {
    const ShelterWorld open{slope, false};
    for (const f64 z : {1.5, 3.0, 6.0, 12.0, 24.0})
      CHECK_MESSAGE(view::shelter_height_at(&shelter_world, &shelter_ground, &open, 0.0, 0.0, z,
                                            1.0f, 0.0f) == 0.0f,
                    "slope " << slope << " at " << z << " m");
    // The same slope with the wall standing on it: the wall's own 3 m, wherever it stands.
    const ShelterWorld walled{slope, true};
    CHECK(view::shelter_height_at(&shelter_world, &shelter_ground, &walled, 0.0, 0.0, 5.0, 1.0f,
                                  0.0f) == doctest::Approx(3.0).epsilon(1e-6));
  }
}

namespace {

// Flat open ground of the renderer's own terrain, 120 m across.
renderer::SceneData shelter_flat_ground() {
  renderer::SceneData scene;
  scene.terrain.enabled = true;
  scene.terrain.size = 65;
  scene.terrain.extent = 60.0f;
  scene.terrain.seed = 4;
  scene.terrain.dune_height = 0.0f;
  return scene;
}

}  // namespace

TEST_CASE("engine-view: a crouched walker's ears find no shelter on flat open ground") {
  // S114-H1: the feet were the drawn eye less the standing eye height, so crouched (the eye 1.1 m
  // over the feet against 1.65) they came out 0.55 m under the ground, the ground read as a 0.55 m
  // obstacle and the ears lost 20.9 dB in the open. The feet are the walker's own now.
  const renderer::SceneData scene = shelter_flat_ground();
  view::Walker walker;
  std::string error;
  REQUIRE_MESSAGE(walker.start(view::WalkParams{}, 240, scene, &error), error);
  REQUIRE(walker.available());
  if (std::string(walker.collision()) != "physics") {
    MESSAGE("this build's walker follows the ground and casts nothing: no shelter to measure");
    return;
  }
  (void)walker.drop(WorldPos{2.0, 20.0, 3.0});
  view::WalkInput crouch;
  crouch.crouch = true;
  for (u32 t = 0; t < 240; ++t)
    (void)walker.step(crouch);
  REQUIRE(walker.crouched());
  REQUIRE(walker.eye_height() < 1.2f);
  const WorldPos feet = walker.feet();
  for (const f64 d : {1.5, 3.0, 6.0, 12.0})
    CHECK_MESSAGE(view::walker_shelter(&walker, feet.x, feet.z - d, 1.0f, 0.0f) == 0.0f,
                  "at " << d << " m");
}

TEST_CASE("engine-view: a flying session hands the ears no walker, a walking one its own") {
  // S114-M2: a session keeps its walker attached while it flies (`FlySession::walker`), so the
  // shelter took a flight's eye for a walker's. The host is the session's tick observer, as a
  // module-less session's is.
  const test::TempDir tmp("engine_view_shelter_walker");
  view::GameSoundHost host;
  std::string error;
  REQUIRE_MESSAGE(
      host.open(world_sound::AudioMode::Null, "", tmp.file("ears.wav"), "", 240, &error), error);
  const renderer::SceneData scene = shelter_flat_ground();
  view::Walker walker;
  REQUIRE_MESSAGE(walker.start(view::WalkParams{}, 240, scene, &error), error);
  REQUIRE(walker.available());
  // Two sessions with the walker attached, ten ticks each: one that flies, one started walking
  // (`--walk`'s header).
  for (const bool walking : {false, true}) {
    view::SessionHeader header;
    header.start.position = WorldPos{2.0, 20.0, 3.0};
    header.params.tick_hz = 240;
    header.has_walk = true;
    header.walk = view::WalkParams{};
    header.walking = walking;
    view::FlySession session;
    session.set_walker(&walker);
    REQUIRE_MESSAGE(session.start(view::default_fly_map(), header, &error), error);
    session.set_tick_observer(&view::GameSoundHost::observe, &host);
    u32 cursor = 0;
    for (u64 t = 1; t <= 10; ++t)
      cursor = session.run(std::span<const input::RawEvent>{}, cursor, SimTick{t});
    REQUIRE(session.walking() == walking);
    CHECK_MESSAGE(host.walker() == (walking ? &walker : nullptr), (walking ? "walking" : "flying"));
    session.set_tick_observer(nullptr, nullptr);
  }
  std::string close_error;
  CHECK_MESSAGE(host.stage().close(&close_error), close_error);
}

#include <domain/scene_gen/water_channel.h>

TEST_CASE(
    "engine-view: the flow table is the scene's solved channel, a fall where a lip meets "
    "its foot") {
  // S113: a channel as a solve writes it — two stretches of a reach, a fall of 4 m landing in its
  // pool, and a still pool after it, which gives up nothing — heard from beside the fall.
  const test::TempDir tmp("engine_view_flow_table");
  view::GameSoundHost host;
  std::string error;
  REQUIRE_MESSAGE(host.open(world_sound::AudioMode::Null, "", tmp.file("flow.wav"), "", 60, &error),
                  error);
  renderer::SceneData scene;
  scene.mesh_info.resize(1);
  renderer::SceneWater water;
  water.channel = true;
  scene::WaterChannel& c = water.channel_source;
  c.points = {WorldPos{0.0, 10.0, 0.0}, WorldPos{10.0, 9.8, 0.0}, WorldPos{12.0, 9.7, 0.0},
              WorldPos{13.0, 5.7, 0.0}, WorldPos{23.0, 5.7, 0.0}};
  c.discharge_m3s = {0.4f, 0.4f, 0.4f, 0.4f, 0.4f};
  c.depth_m = {0.3f, 0.3f, 0.2f, 0.6f, 0.6f};
  c.wet_left_m = {1.0f, 1.0f, 1.0f, 1.5f, 2.0f};
  c.wet_right_m = {1.0f, 1.0f, 1.0f, 1.5f, 2.0f};
  c.froude = {0.5f, 0.5f, 0.9f, 0.3f, 0.1f};
  c.flags = {0, 0, scene_gen::k_channel_fall_lip, scene_gen::k_channel_fall_foot, 0};
  scene.mesh_info[0].water = water;
  host.attach_scene(scene);
  // The reach's two stretches joined into one (12 m: under `k_stretch_join_m`), the fall, and the
  // still pool's stretch left out.
  CHECK(host.stretches() == 2u);
  struct Seen {
    u32 falls = 0;
    u32 on_water = 0;
    f32 height = 0.0f;
    static void emit(void* user, const world_sound::Stretch& s) noexcept {
      Seen& seen = *static_cast<Seen*>(user);
      seen.falls += s.fall ? 1u : 0u;
      seen.on_water += s.fall && s.lands_on_water ? 1u : 0u;
      if (s.fall) seen.height = s.drop_m;
    }
  } seen;
  view::GameSoundHost::visit_flow(&host, WorldPos{12.0, 7.0, 8.0}, 100.0,
                                  world_sound::StretchSink{&seen, &Seen::emit});
  CHECK(seen.falls == 1u);
  CHECK(seen.on_water == 1u);
  CHECK(seen.height == doctest::Approx(4.0f));
  // Heard from 8 m off the fall: a fall's voice and the reach's.
  for (u32 i = 0; i < 30; ++i)
    host.tick_at(WorldPos{12.0, 7.0, 8.0}, Vec3{0.0f, 0.0f, -1.0f}, gfx::WindLive{}, 0.0, 0.0);
  CHECK(host.stage().world_stats().fall_voices == 1u);
  CHECK(host.stage().world_stats().stream_voices >= 1u);
  std::string close_error;
  CHECK_MESSAGE(host.stage().close(&close_error), close_error);
}

TEST_CASE("engine-view: a refresh visits the water in reach, not the scene's") {
  // S113, M2: a channel 40 km long, a stretch every 100 m (too far apart to join), heard from its
  // middle. The flow table is bucketed by 128 m cell at attach; a visit with the streams' reach at
  // 800 m walks the cells within it and sends exactly the stretches the sink would take.
  const test::TempDir tmp("engine_view_flow_cells");
  view::GameSoundHost host;
  std::string error;
  REQUIRE_MESSAGE(
      host.open(world_sound::AudioMode::Null, "", tmp.file("cells.wav"), "", 60, &error), error);
  renderer::SceneData scene;
  scene.mesh_info.resize(1);
  renderer::SceneWater water;
  water.channel = true;
  scene::WaterChannel& c = water.channel_source;
  constexpr u32 k_stations = 401;
  for (u32 i = 0; i < k_stations; ++i) {
    c.points.push_back(WorldPos{100.0 * i, 400.0 - 0.5 * i, 0.0});
    c.discharge_m3s.push_back(i % 2 == 0 ? 0.4f : 0.8f);  // never within a tenth: no joins
    c.depth_m.push_back(0.3f);
    c.wet_left_m.push_back(1.0f);
    c.wet_right_m.push_back(1.0f);
    c.froude.push_back(0.5f);
    c.flags.push_back(0);
  }
  scene.mesh_info[0].water = water;
  host.attach_scene(scene);
  REQUIRE(host.stretches() == k_stations - 1);
  const WorldPos centre{20000.0, 300.0, 30.0};
  constexpr f64 k_stream_reach = 800.0;
  u32 expected = 0;
  for (u32 i = 0; i + 1 < k_stations; ++i) {
    const f64 mx = 100.0 * i + 50.0 - centre.x, mz = -centre.z;
    if (std::sqrt(mx * mx + mz * mz) - 50.0 <= k_stream_reach) ++expected;
  }
  struct Count {
    u32 sent = 0;
    static void emit(void* user, const world_sound::Stretch&) noexcept {
      ++static_cast<Count*>(user)->sent;
    }
  } count;
  view::GameSoundHost::visit_flow(&host, centre, 2000.0,
                                  world_sound::StretchSink{&count, &Count::emit, k_stream_reach});
  MESSAGE(count.sent << " stretches sent of " << host.stretches() << ", "
                     << host.water_cells_walked() << " cells walked");
  CHECK(count.sent == expected);
  // The cells holding water within the reach and the longest stretch's half: the line's, 850 m
  // either side of the listener's cell and its own, at most 15 of the table's 313.
  CHECK(host.water_cells_walked() <= 15u);
  CHECK(host.water_cells_walked() >= 13u);
  std::string close_error;
  CHECK_MESSAGE(host.stage().close(&close_error), close_error);
}
#endif  // ENGINE_VIEW_GAME && ENGINE_VIEW_WORLD_SOUND

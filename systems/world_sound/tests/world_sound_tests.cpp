// Tests for the world_sound capability (docs/subsystems/world_sound.md, "Testing"; roadmap S111).
// Every case runs the null backend and judges by its samples: no case opens an audio device
// (ADR-0079 decision 6).
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/memory/memory.h>
#include <domain/audio/audio.h>
#include <foundation/io/vfs.h>
#include <systems/world_sound/world_sound.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <schemas/scene.h>
#include <string>
#include <string_view>
#include <vector>

using namespace engine;
using namespace engine::world_sound;

namespace {

scene::SoundClip recipe_clip(const char* name,
                             scene::SoundRecipeKind kind = scene::SoundRecipeKind::Chirp) {
  scene::SoundClip c;
  c.name = name;
  scene::SoundRecipe r;
  r.kind = kind;
  c.recipe = r;
  return c;
}

scene::Sound chirp_block() {
  scene::Sound block;
  block.clips.push_back(recipe_clip("chirp"));
  scene::SoundClip tone = recipe_clip("tone", scene::SoundRecipeKind::Tone);
  tone.recipe->frequency_hz = 440.0f;
  tone.recipe->duration_s = 2.0f;
  block.clips.push_back(tone);
  return block;
}

// Energy per channel of a stereo run of frames.
struct Energy {
  f64 left = 0.0;
  f64 right = 0.0;
};
void add_energy(const SoundStage& stage, Energy& e) {
  const f32* b = stage.last_block();
  for (u32 i = 0; i < stage.last_block_frames(); ++i) {
    e.left += static_cast<f64>(b[2 * i]) * static_cast<f64>(b[2 * i]);
    e.right += static_cast<f64>(b[2 * i + 1]) * static_cast<f64>(b[2 * i + 1]);
  }
}

const Vec3 k_ahead{0.0f, 0.0f, -1.0f};
const Vec3 k_behind{0.0f, 0.0f, 1.0f};
const Vec3 k_up{0.0f, 1.0f, 0.0f};

// The scripted session the pin and the mode cases run: a listener 419 km out, a chirp 3 m to its
// left, a tone 5 m ahead stopped mid-way, a 2D chirp, a half turn, a walk across a cell's edge.
struct Script {
  std::vector<u32> answers;  // every call's answer and handle, in order
};
Script run_script(SoundStage& stage, u32 ticks = 240) {
  Script out;
  const WorldPos ears{419'000.5, 2.0, -12.25};
  stage.set_listener(ears, k_ahead, k_up);
  u32 chirp = 0;
  u32 tone = 0;
  out.answers.push_back(static_cast<u32>(stage.clip("chirp", chirp)));
  out.answers.push_back(static_cast<u32>(stage.clip("tone", tone)));
  for (u32 t = 0; t < ticks; ++t) {
    u32 handle = 0;
    if (t == 0) {
      const WorldPos left{ears.x - 3.0, ears.y, ears.z};
      out.answers.push_back(static_cast<u32>(stage.play(chirp, &left, 1.0f, handle)));
      out.answers.push_back(handle);
    }
    if (t == 10) {
      const WorldPos ahead{ears.x, ears.y, ears.z - 5.0};
      out.answers.push_back(static_cast<u32>(stage.play(tone, &ahead, 0.5f, handle)));
      out.answers.push_back(handle);
    }
    if (t == 60) out.answers.push_back(static_cast<u32>(stage.stop(2)));
    if (t == 80) {
      out.answers.push_back(static_cast<u32>(stage.play(chirp, nullptr, 0.25f, handle)));
      out.answers.push_back(handle);
    }
    if (t == 120) stage.set_listener(ears, k_behind, k_up);
    if (t == 140) {  // a tone sounding while the listener crosses into the next cell
      const WorldPos right{ears.x + 4.0, ears.y, ears.z};
      out.answers.push_back(static_cast<u32>(stage.play(tone, &right, 0.5f, handle)));
      out.answers.push_back(handle);
    }
    if (t >= 150 && t < 200)  // a walk east across the cell's edge at 419,008 m
      stage.set_listener(WorldPos{ears.x + 0.25 * (t - 149), ears.y, ears.z}, k_behind, k_up);
    stage.tick();
  }
  return out;
}

}  // namespace

TEST_CASE("world_sound: a gain from dB is the series' and not the C library's") {
  CHECK(gain_from_db(0.0) == 1.0);
  CHECK(std::abs(gain_from_db(-20.0) - 0.1) < 1e-16);
  CHECK(std::abs(gain_from_db(-6.0) / std::pow(10.0, -0.3) - 1.0) < 1e-14);
  CHECK(std::abs(gain_from_db(24.0) / std::pow(10.0, 1.2) - 1.0) < 1e-14);
  CHECK(std::abs(gain_from_db(-60.0) / 1e-3 - 1.0) < 1e-14);
}

TEST_CASE("world_sound: a recipe is the same samples on every toolchain") {
  scene::SoundRecipe chirp;
  Vector<f32> pcm;
  std::string error;
  REQUIRE(render_recipe(chirp, pcm, &error));
  CHECK(pcm.size() == 8640);  // 0.18 s at 48 kHz
  CHECK(pcm[0] == 0.0f);      // the rise starts at nothing and the fall ends at it
  CHECK(pcm[pcm.size() - 1] == 0.0f);
  f32 peak = 0.0f;
  for (f32 v : pcm)
    peak = std::abs(v) > peak ? std::abs(v) : peak;
  CHECK(peak <= 0.50119f);  // -6 dB of full scale
  CHECK(peak > 0.49f);
  // Integer phase, the mixer's quarter-wave polynomial and the dB series: pinned on MSVC, and the
  // Linux container's GCC and Clang must reproduce it (the same claim as audio.md "Determinism").
  const u64 chirp_hash = hash_bytes(pcm.data(), pcm.size() * sizeof(f32));
  MESSAGE("chirp " << chirp_hash);
  CHECK(chirp_hash == 0x812eb5dafa81ec80ull);
  scene::SoundRecipe noise;
  noise.kind = scene::SoundRecipeKind::Noise;
  noise.seed = 7;
  REQUIRE(render_recipe(noise, pcm, &error));
  const u64 noise_hash = hash_bytes(pcm.data(), pcm.size() * sizeof(f32));
  MESSAGE("noise " << noise_hash);
  CHECK(noise_hash == 0x3037d56583c9bee2ull);
  // Refusals name the field.
  scene::SoundRecipe bad;
  bad.frequency_hz = 5.0f;
  CHECK_FALSE(render_recipe(bad, pcm, &error));
  CHECK(error.find("frequency_hz") != std::string::npos);
  bad = scene::SoundRecipe{};
  bad.attack_s = 0.15f;
  CHECK_FALSE(render_recipe(bad, pcm, &error));
}

TEST_CASE("world_sound: a scene's sound block is read and its mistakes refused") {
  const test::TempDir tmp("world_sound_read");
  const auto scene_with = [&](const std::string& sound) {
    const std::string path = tmp.file("scene.json");
    REQUIRE(io::write_file(path, R"({"format":"engine.scene.v1","name":"s")" +
                                     (sound.empty() ? std::string() : ",\"sound\":" + sound) +
                                     "}") == io::Status::Ok);
    return path;
  };
  scene::Sound block;
  bool present = true;
  std::string error;
  REQUIRE(read_scene_sound(scene_with(""), block, present, &error));
  CHECK_FALSE(present);
  REQUIRE(read_scene_sound(scene_with(R"({"clips":[{"name":"chirp","recipe":{"kind":"Chirp"}},
                              {"name":"door","path":"door.wav","gain_db":-3,"max_m":20}]})"),
                           block, present, &error));
  CHECK(present);
  REQUIRE(block.clips.size() == 2);
  CHECK(block.clips[1].path == "door.wav");
  CHECK(block.clips[1].max_m == 20.0f);
  CHECK_FALSE(read_scene_sound(
      scene_with(R"({"clips":[{"name":"a","recipe":{}},{"name":"a","recipe":{}}]})"), block,
      present, &error));
  CHECK(error.find("named twice") != std::string::npos);
  CHECK_FALSE(read_scene_sound(scene_with(R"({"clips":[{"name":"a"}]})"), block, present, &error));
  CHECK(error.find("neither or both") != std::string::npos);
  CHECK_FALSE(read_scene_sound(scene_with(R"({"clips":[{"name":"a","path":"a.wav","recipe":{}}]})"),
                               block, present, &error));
  CHECK_FALSE(
      read_scene_sound(scene_with(R"({"clips":[{"name":"a","path":"a.wav","min_m":5,"max_m":2}]})"),
                       block, present, &error));
}

TEST_CASE(
    "world_sound: a prop to the left is louder on the left, and on the right after a half turn") {
  const scene::Sound block = chirp_block();
  SoundStage stage;
  StageConfig config;
  config.mode = AudioMode::Null;
  config.tick_hz = 240;
  std::string error;
  REQUIRE_MESSAGE(stage.open(config, &block, &error), error);
  REQUIRE(stage.channels() == 2);
  u32 chirp = 0;
  REQUIRE(stage.clip("chirp", chirp) == Answer::Ok);
  // 10,000 km out, in the mixer's frame at the listener's cell: the floats it gets are metres.
  const WorldPos ears{1.0e7 + 20.5, 1.7, 33.0};
  const WorldPos left{ears.x - 3.0, ears.y, ears.z};
  const auto hear = [&](const Vec3& forward) {
    stage.set_listener(ears, forward, k_up);
    u32 handle = 0;
    REQUIRE(stage.play(chirp, &left, 1.0f, handle) == Answer::Ok);
    Energy e;
    for (u32 t = 0; t < 48; ++t) {  // 0.2 s at 240 ticks a second: the whole chirp
      stage.tick();
      CHECK(stage.last_block_frames() == 200);
      add_energy(stage, e);
    }
    return e;
  };
  const Energy facing = hear(k_ahead);
  const Energy turned = hear(k_behind);
  MESSAGE("facing -z: left " << facing.left << " right " << facing.right << "; turned: left "
                             << turned.left << " right " << turned.right);
  CHECK(facing.left > 0.0);
  CHECK(facing.left > 30.0 * facing.right);  // hard left: the constant-power pan's far side
  CHECK(turned.right > 30.0 * turned.left);
  // The same source, the same distance: the same power either way round.
  CHECK(std::abs((facing.left + facing.right) / (turned.left + turned.right) - 1.0) < 1e-3);
  // The commands, counted.
  const StageStats& stats = stage.stats();
  CHECK(stats.plays == 2);
  CHECK(stats.voices_started == 2);
  CHECK(stats.listener_moves == 2);
  CHECK(stats.frames == 96u * 200u);
  CHECK(stage.close(&error));
}

TEST_CASE("world_sound: the null render of a scripted session is pinned") {
  const scene::Sound block = chirp_block();
  u64 hashes[2] = {0, 0};
  for (u64& h : hashes) {
    SoundStage stage;
    StageConfig config;
    config.mode = AudioMode::Null;
    std::string error;
    REQUIRE_MESSAGE(stage.open(config, &block, &error), error);
    run_script(stage);
    h = stage.render_hash();
    const StageStats& stats = stage.stats();
    CHECK(stats.plays == 4);
    CHECK(stats.stops == 1);
    CHECK(stats.voices_started == 4);
    CHECK(stats.frame_moves == 2);     // to the first listener's cell, and into 419,008's once
    CHECK(stats.source_resends == 1);  // the second tone; the others have ended or are 2D
    CHECK(stats.listener_moves == 52);
  }
  CHECK(hashes[0] == hashes[1]);
  // Taken on MSVC; GCC 13 and Clang 18 in the Linux container must reproduce it.
  MESSAGE("session " << hashes[0]);
  CHECK(hashes[0] == 0x9b8ce750f9dab6e5ull);
}

TEST_CASE("world_sound: a mute stage, a sounding one and one with no block answer as they must") {
  const scene::Sound block = chirp_block();
  SoundStage off;
  SoundStage null;
  StageConfig config;
  std::string error;
  REQUIRE(off.open(config, &block, &error));
  config.mode = AudioMode::Null;
  REQUIRE(null.open(config, &block, &error));
  // What a module sees is the same whether or not anything is heard (ADR-0079 decision 5).
  const Script a = run_script(off);
  const Script b = run_script(null);
  CHECK(a.answers == b.answers);
  CHECK(off.stats().plays == null.stats().plays);
  CHECK(off.channels() == 0);
  CHECK(off.stats().frames == 0);
  CHECK(off.stats().voices_started == 0);
  CHECK(null.stats().frames > 0);
  // The refusals are the count's too, so they are the same either way.
  for (SoundStage* s : {&off, &null}) {
    u32 handle = 99;
    u32 clip = 0;
    CHECK(s->clip("nightingale", clip) == Answer::NotFound);
    CHECK(s->play(7, nullptr, 1.0f, handle) == Answer::InvalidArgument);
    CHECK(s->play(0, nullptr, -1.0f, handle) == Answer::InvalidArgument);
    const WorldPos nowhere{std::nan(""), 0.0, 0.0};
    CHECK(s->play(0, &nowhere, 1.0f, handle) == Answer::InvalidArgument);
    CHECK(handle == 99);
    CHECK(s->stop(0) == Answer::InvalidArgument);
    CHECK(s->stop(5) == Answer::InvalidArgument);  // four issued
    CHECK(s->stop(1) == Answer::Ok);               // long ended: Ok, and nothing sent
  }
  // No block: every call is Unsupported, and nothing is counted.
  SoundStage none;
  REQUIRE(none.open(config, nullptr, &error));
  u32 x = 0;
  CHECK(none.clip("chirp", x) == Answer::Unsupported);
  CHECK(none.play(0, nullptr, 1.0f, x) == Answer::Unsupported);
  CHECK(none.stop(1) == Answer::Unsupported);
  none.tick();
  CHECK(none.channels() == 0);
}

TEST_CASE("world_sound: the stage's tick allocates nothing in steady state") {
  const scene::Sound block = chirp_block();
  SoundStage stage;
  StageConfig config;
  config.mode = AudioMode::Null;
  std::string error;
  REQUIRE(stage.open(config, &block, &error));
  run_script(stage, 60);  // warm: every path once
  const u64 before = mem::allocations_on_thread();
  const WorldPos ears{100.0, 2.0, 100.0};
  for (u32 t = 0; t < 2400; ++t) {
    // Walking, turning, a chirp every 50 ticks and a stop of the one before: every call a host
    // makes, ten seconds of them, across four cells.
    const WorldPos at{ears.x + 0.1 * t, ears.y, ears.z};
    stage.set_listener(at, t % 400 < 200 ? k_ahead : k_behind, k_up);
    u32 handle = 0;
    if (t % 50 == 0) {
      const WorldPos src{at.x - 2.0, at.y, at.z + 1.0};
      CHECK(stage.play(0, &src, 1.0f, handle) == Answer::Ok);
      if (handle > 1) CHECK(stage.stop(handle - 1) == Answer::Ok);
    }
    stage.tick();
  }
  const u64 allocations = mem::allocations_on_thread() - before;
  CHECK(allocations == 0);
  CHECK(stage.stats().frame_moves >= 3);
}

TEST_CASE("world_sound: --audio-out's WAV holds every frame the null backend pulled") {
  const test::TempDir tmp("world_sound_wav");
  const scene::Sound block = chirp_block();
  SoundStage stage;
  StageConfig config;
  config.mode = AudioMode::Null;
  config.tick_hz = 60;  // 800 frames a tick
  config.wav_out = tmp.file("out.wav");
  std::string error;
  REQUIRE_MESSAGE(stage.open(config, &block, &error), error);
  stage.set_listener(WorldPos{0.0, 0.0, 0.0}, k_ahead, k_up);
  u32 handle = 0;
  REQUIRE(stage.play(1, nullptr, 1.0f, handle) == Answer::Ok);
  std::vector<f32> heard;
  for (u32 t = 0; t < 75; ++t) {  // 1.25 s: past the WAV's one-second buffer
    stage.tick();
    heard.insert(heard.end(), stage.last_block(),
                 stage.last_block() + stage.last_block_frames() * 2);
  }
  REQUIRE(stage.close(&error));
  CHECK(stage.stats().wav_frames == 60000);
  std::string text;
  REQUIRE(io::read_file(config.wav_out, text) == io::Status::Ok);
  REQUIRE(text.size() == 44 + 60000u * 2u * 4u);
  CHECK(std::memcmp(text.data(), "RIFF", 4) == 0);
  CHECK(std::memcmp(text.data() + 44, heard.data(), heard.size() * sizeof(f32)) == 0);
  // The store's own decoder reads it back, every frame bit for bit.
  audio::DecodedClip decoded;
  REQUIRE(
      audio::decode_clip(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()),
                         decoded) == audio::DecodeStatus::Ok);
  REQUIRE(decoded.samples.size() == heard.size());
  CHECK(std::memcmp(decoded.samples.data(), heard.data(), heard.size() * sizeof(f32)) == 0);
  // A tone at 440 Hz centred and 2D: the same on both channels, and not silent.
  f64 energy = 0.0;
  u64 differ = 0;
  for (usize i = 0; i < heard.size(); i += 2) {
    differ += heard[i] != heard[i + 1] ? 1u : 0u;
    energy += static_cast<f64>(heard[i]) * static_cast<f64>(heard[i]);
  }
  CHECK(differ == 0);
  CHECK(energy > 100.0);
}

namespace {

// A WAV of `frames` frames of a 1 kHz square-ish ramp, PCM 16 or 24 bit or 32-bit float, at `rate`:
// the formats the owner asked a game's clips to come in (2026-11-21), written here so nothing is
// committed.
std::string wav_bytes(u32 bits, bool is_float, u32 channels, u32 rate, u32 frames) {
  std::string out;
  const auto put = [&](u32 v, u32 n) {
    for (u32 b = 0; b < n; ++b)
      out.push_back(static_cast<char>((v >> (8 * b)) & 0xFF));
  };
  const u32 block = channels * bits / 8;
  out += "RIFF";
  put(36 + frames * block, 4);
  out += "WAVEfmt ";
  put(16, 4);
  put(is_float ? 3 : 1, 2);
  put(channels, 2);
  put(rate, 4);
  put(rate * block, 4);
  put(block, 2);
  put(bits, 2);
  out += "data";
  put(frames * block, 4);
  for (u32 f = 0; f < frames; ++f) {
    const f32 v = ((f / 22) % 2 == 0 ? 0.5f : -0.5f);
    for (u32 c = 0; c < channels; ++c) {
      if (is_float) {
        u32 u = 0;
        std::memcpy(&u, &v, 4);
        put(u, 4);
      } else if (bits == 16) {
        put(static_cast<u32>(static_cast<i32>(v * 32767.0f)) & 0xFFFF, 2);
      } else {
        put(static_cast<u32>(static_cast<i32>(v * 8388607.0f)) & 0xFFFFFF, 3);
      }
    }
  }
  return out;
}

}  // namespace

TEST_CASE(
    "world_sound: a game's WAV clips load from beside the scene, 16, 24 and float, any rate") {
  const test::TempDir tmp("world_sound_formats");
  struct Kind {
    const char* name;
    u32 bits;
    bool is_float;
    u32 channels;
    u32 rate;
  };
  const Kind kinds[] = {{"pcm16", 16, false, 1, 48000},
                        {"pcm24", 24, false, 2, 44100},
                        {"float", 32, true, 1, 22050},
                        {"pcm16_96k", 16, false, 2, 96000}};
  scene::Sound block;
  std::error_code ec;
  std::filesystem::create_directories(tmp.native_file("sounds"), ec);
  for (const Kind& k : kinds) {
    REQUIRE(io::write_file(tmp.file(std::string("sounds/") + k.name + ".wav"),
                           wav_bytes(k.bits, k.is_float, k.channels, k.rate, k.rate / 2)) ==
            io::Status::Ok);
    scene::SoundClip c;
    c.name = k.name;
    c.path = std::string("sounds/") + k.name + ".wav";
    block.clips.push_back(c);
  }
  SoundStage stage;
  StageConfig config;
  config.mode = AudioMode::Null;
  config.base_dir = tmp.path();
  std::string error;
  REQUIRE_MESSAGE(stage.open(config, &block, &error), error);
  // Every one half a second long at the mix's 48 kHz, whatever its own rate.
  const JsonValue summary = stage.summary_json();
  const JsonValue* clips = summary.find("clips");
  REQUIRE(clips != nullptr);
  REQUIRE(clips->size() == 4);
  for (usize i = 0; i < clips->size(); ++i) {
    f64 frames = 0.0;
    REQUIRE((*clips)[i].find("frames")->get_f64(frames));
    CHECK(std::abs(frames - 24000.0) <= 2.0);
  }
  // And each is heard.
  for (u32 c = 0; c < 4; ++c) {
    u32 handle = 0;
    REQUIRE(stage.play(c, nullptr, 1.0f, handle) == Answer::Ok);
  }
  Energy e;
  for (u32 t = 0; t < 24; ++t) {
    stage.tick();
    add_energy(stage, e);
  }
  CHECK(stage.stats().voices_started == 4);
  CHECK(e.left > 100.0);
  // A file that is not there refuses the stage, naming the clip.
  scene::Sound missing;
  scene::SoundClip gone;
  gone.name = "gone";
  gone.path = "sounds/gone.wav";
  missing.clips.push_back(gone);
  SoundStage refused;
  CHECK_FALSE(refused.open(config, &missing, &error));
  CHECK(error.find("'gone'") != std::string::npos);
}

TEST_CASE(
    "world_sound: an output refused or failing leaves the stage silent and the answers the same") {
  // The fault seam on the null backend: the output refused at open, and its write failing at tick
  // 100. Neither fails the stage; both go silent, say why, and answer every call exactly as a
  // stage that never failed (the scripted session's answers compared).
  const scene::Sound block = chirp_block();
  SoundStage healthy;
  StageConfig config;
  config.mode = AudioMode::Null;
  std::string error;
  REQUIRE(healthy.open(config, &block, &error));
  const Script expected = run_script(healthy);
  for (u32 path = 0; path < 2; ++path) {
    SoundStage stage;
    StageConfig faulty = config;
    faulty.fault_refuse_open = path == 0;
    faulty.fault_fail_write_at_tick = path == 1 ? 100u : 0u;
    REQUIRE_MESSAGE(stage.open(faulty, &block, &error), error);
    const Script got = run_script(stage);
    CHECK(got.answers == expected.answers);
    CHECK(stage.stats().plays == healthy.stats().plays);
    const JsonValue summary = stage.summary_json();
    const JsonValue* stopped = summary.find("stopped");
    REQUIRE(stopped != nullptr);
    CHECK(write_json(*stopped) == "true");
    MESSAGE(write_json(*summary.find("stopped_why")));
    // Silent from the failure on: no frame pulled after it, none of the later plays sent.
    CHECK(stage.stats().frames == (path == 0 ? 0u : 99u * 200u));
    CHECK(stage.stats().voices_started == (path == 0 ? 0u : 3u));  // ticks 0, 10, 80
    CHECK(stage.close(&error));
  }
}

TEST_CASE("world_sound declares a determinism stance") {
  CHECK(std::string_view(k_determinism) == "derived");
}

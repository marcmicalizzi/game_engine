// The sound stage a host creates for a game (world_sound.h; docs/subsystems/world_sound.md).

#include "world_field.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/log/log.h>
#include <core/schema/json_reflect.h>
#include <core/schema/json_text.h>
#include <domain/audio/audio.h>
#include <foundation/io/vfs.h>
#include <systems/world_sound/path.h>
#include <systems/world_sound/world_sound.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <schemas/scene.h>

ENGINE_LOG_CATEGORY_DEFINE(log_world_sound, "world_sound");

namespace engine::world_sound {

namespace {

using audio::k_sample_rate;

// Handles are counted by the stage; the voices behind the last this many are remembered, so a
// stop reaches a voice that may still sound. An older handle's voice has ended long since (a
// one-shot), and its stop answers Ok and sends nothing.
constexpr u32 k_handle_slots = 256;

// The WAV is written a second at a time, from a buffer made at `open`.
constexpr u32 k_wav_buffer_frames = k_sample_rate;

template <class T>
bool finite(T v) noexcept {
  return std::isfinite(v);
}

std::string join(const std::string& dir, const std::string& path) {
  if (path.empty()) return path;
  const std::filesystem::path p(path);
  if (p.is_absolute() || dir.empty()) return path;
  return (std::filesystem::path(dir) / p).lexically_normal().string();
}

// The corner of the 64 m cell `p` stands in: the mixer's frame (audio_system.md "The mixer's
// frame"). A function of where the listener is now, never of where it has been.
WorldPos cell_corner(const WorldPos& p) noexcept {
  return WorldPos{std::floor(p.x / k_world_cell_m) * k_world_cell_m,
                  std::floor(p.y / k_world_cell_m) * k_world_cell_m,
                  std::floor(p.z / k_world_cell_m) * k_world_cell_m};
}

// sin(2 pi phase / 2^32) through the mixer's own quarter-wave polynomial: no C library.
f32 sine_of_phase(u32 phase) noexcept {
  const u32 quadrant = phase >> 30;
  const f32 t = static_cast<f32>(static_cast<f64>(phase & 0x3FFFFFFFu) * (1.0 / 1073741824.0));
  switch (quadrant) {
    case 0: return audio::sin_quarter(t);
    case 1: return audio::sin_quarter(1.0f - t);
    case 2: return -audio::sin_quarter(t);
    default: return -audio::sin_quarter(1.0f - t);
  }
}

u32 phase_step(f64 hz) noexcept {
  return static_cast<u32>(hz * (4294967296.0 / static_cast<f64>(k_sample_rate)) + 0.5);
}

bool check_recipe(const scene::SoundRecipe& r, std::string* error) {
  const auto bad = [&](const char* what) {
    if (error != nullptr) *error = what;
    return false;
  };
  if (!finite(r.frequency_hz) || r.frequency_hz < 20.0f || r.frequency_hz > 20000.0f)
    return bad("a recipe's frequency_hz is outside 20 to 20000");
  if (!finite(r.end_hz) || r.end_hz < 20.0f || r.end_hz > 20000.0f)
    return bad("a recipe's end_hz is outside 20 to 20000");
  if (!finite(r.duration_s) || r.duration_s < 0.001f || r.duration_s > 10.0f)
    return bad("a recipe's duration_s is outside 0.001 to 10");
  if (!finite(r.attack_s) || !finite(r.release_s) || r.attack_s < 0.0f || r.release_s < 0.0f ||
      r.attack_s + r.release_s > r.duration_s)
    return bad("a recipe's attack_s and release_s are negative or longer than the clip");
  if (!finite(r.level_db) || r.level_db < -60.0f || r.level_db > 0.0f)
    return bad("a recipe's level_db is outside -60 to 0");
  return true;
}

bool check_clip(const scene::SoundClip& c, std::string* error) {
  const auto bad = [&](const std::string& what) {
    if (error != nullptr) *error = "the sound clip '" + c.name + "': " + what;
    return false;
  };
  if (c.name.empty()) return bad("has no name");
  if (c.path.empty() == !c.recipe.has_value())
    return bad("names neither or both of a path and a recipe");
  if (!finite(c.gain_db) || c.gain_db < -60.0f || c.gain_db > 24.0f)
    return bad("gain_db is outside -60 to 24");
  if (!finite(c.min_m) || !finite(c.max_m) || c.min_m <= 0.0f || c.max_m <= c.min_m)
    return bad("min_m must be above 0 and max_m above min_m");
  std::string why;
  if (c.recipe.has_value() && !check_recipe(*c.recipe, &why)) return bad(why);
  return true;
}

// The 44-byte header of a 32-bit float WAV of `frames` frames.
void wav_header(u8 (&h)[44], u32 channels, u64 frames) noexcept {
  const u64 data = frames * channels * 4u;
  const u32 data32 = data > 0xFFFFFFF0ull ? 0xFFFFFFF0u : static_cast<u32>(data);
  const auto put32 = [&](u32 at, u32 v) {
    for (u32 b = 0; b < 4; ++b)
      h[at + b] = static_cast<u8>(v >> (8 * b));
  };
  const auto put16 = [&](u32 at, u32 v) {
    h[at] = static_cast<u8>(v);
    h[at + 1] = static_cast<u8>(v >> 8);
  };
  std::memcpy(h, "RIFF", 4);
  put32(4, 36u + data32);
  std::memcpy(h + 8, "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 3);  // WAVE_FORMAT_IEEE_FLOAT
  put16(22, channels);
  put32(24, k_sample_rate);
  put32(28, k_sample_rate * channels * 4u);
  put16(32, channels * 4u);
  put16(34, 32);
  std::memcpy(h + 36, "data", 4);
  put32(40, data32);
}

}  // namespace

const char* audio_mode_name(AudioMode mode) noexcept {
  switch (mode) {
    case AudioMode::Off: return "off";
    case AudioMode::Null: return "null";
    case AudioMode::Device: return "device";
  }
  return "off";
}

f64 gain_from_db(f64 db) noexcept {
  // 10^(db/20) = 2^x, x = db * log2(10) / 20; 2^x = 2^n * e^(f ln 2) with n = floor(x), f in [0,
  // 1).
  const f64 x = db * 0.16609640474436813;
  const f64 n = std::floor(x);
  const f64 y = (x - n) * 0.69314718055994531;  // in [0, ln 2)
  f64 term = 1.0;
  f64 sum = 1.0;
  for (u32 k = 1; k <= 18; ++k) {
    term = term * y / static_cast<f64>(k);
    sum = sum + term;
  }
  return std::ldexp(sum, static_cast<int>(n));  // exact: a power of two
}

bool render_recipe(const scene::SoundRecipe& recipe, Vector<f32>& out, std::string* error) {
  out.clear();
  if (!check_recipe(recipe, error)) return false;
  const u32 frames = static_cast<u32>(static_cast<f64>(recipe.duration_s) * k_sample_rate + 0.5);
  const u32 attack = static_cast<u32>(static_cast<f64>(recipe.attack_s) * k_sample_rate + 0.5);
  const u32 release = static_cast<u32>(static_cast<f64>(recipe.release_s) * k_sample_rate + 0.5);
  const f32 level = static_cast<f32>(gain_from_db(static_cast<f64>(recipe.level_db)));
  const i64 s0 = phase_step(static_cast<f64>(recipe.frequency_hz));
  const i64 s1 = recipe.kind == scene::SoundRecipeKind::Chirp
                     ? phase_step(static_cast<f64>(recipe.end_hz))
                     : s0;
  const i64 span = frames > 1 ? static_cast<i64>(frames - 1) : 1;
  out.resize(frames);
  u32 phase = 0;
  u32 noise = recipe.seed != 0 ? recipe.seed : 0x9E3779B9u;
  for (u32 i = 0; i < frames; ++i) {
    f32 v = 0.0f;
    if (recipe.kind == scene::SoundRecipeKind::Noise) {
      noise ^= noise << 13;
      noise ^= noise >> 17;
      noise ^= noise << 5;
      v = static_cast<f32>(static_cast<f64>(static_cast<i32>(noise)) * (1.0 / 2147483648.0));
    } else {
      v = sine_of_phase(phase);
      phase += static_cast<u32>(s0 + (s1 - s0) * static_cast<i64>(i) / span);
    }
    f32 env = 1.0f;
    if (attack > 0 && i < attack) env = static_cast<f32>(i) / static_cast<f32>(attack);
    const u32 left = frames - 1 - i;
    if (release > 0 && left < release) {
      const f32 fall = static_cast<f32>(left) / static_cast<f32>(release);
      env = fall < env ? fall : env;
    }
    out[i] = v * env * level;
  }
  return true;
}

bool read_scene_sound(const std::string& scene_path, scene::Sound& out, bool& present,
                      std::string* error) {
  out = scene::Sound{};
  present = false;
  std::string text;
  const io::Status status = io::read_file(scene_path, text);
  if (status != io::Status::Ok) {
    if (error != nullptr) *error = "cannot read " + scene_path + ": " + io::status_name(status);
    return false;
  }
  scene::Scene file;
  schema::ReadContext ctx;
  JsonParseResult parsed;
  const bool read = schema::read_json_text(file, text, ctx, parsed);
  if (!parsed.ok) {
    if (error != nullptr)
      *error = scene_path + ":" + std::to_string(parsed.line) + ": " + parsed.message;
    return false;
  }
  if (!read || !ctx.ok()) {
    std::string why;
    for (const schema::Diagnostic& d : ctx.diagnostics)
      why += (why.empty() ? "" : "; ") + (d.path.empty() ? d.message : d.path + ": " + d.message);
    if (error != nullptr) *error = scene_path + ": " + (why.empty() ? "not a valid scene" : why);
    return false;
  }
  if (!file.sound.has_value()) return true;
  for (u32 i = 0; i < file.sound->clips.size(); ++i) {
    const scene::SoundClip& c = file.sound->clips[i];
    std::string why;
    if (!check_clip(c, &why)) {
      if (error != nullptr) *error = scene_path + ": " + why;
      return false;
    }
    for (u32 j = 0; j < i; ++j) {
      if (file.sound->clips[j].name == c.name) {
        if (error != nullptr)
          *error = scene_path + ": the sound clip '" + c.name + "' is named twice";
        return false;
      }
    }
  }
  out = std::move(*file.sound);
  present = true;
  return true;
}

// ---- the stage
// ------------------------------------------------------------------------------------

struct SoundStage::Impl {
  StageConfig config;
  bool open = false;
  bool has_block = false;
  // The declared clips, in the block's order: the index a module holds.
  struct Clip {
    std::string name;
    std::string source;  // the file, or "recipe"
    audio::ClipHandle handle;
    f32 gain = 1.0f;  // linear, the clip's own
    f32 min_m = 1.0f;
    f32 max_m = 60.0f;
    u32 frames = 0;
  };
  Vector<Clip> clips;
  // Sounding: the store, the mixer, the output. Null with the stage Off or with no block.
  std::unique_ptr<audio::ClipStore> store;
  std::unique_ptr<audio::Mixer> mixer;
  std::unique_ptr<audio::Output> output;
  // The handles' voices, by handle modulo the slots, and where each 3D one stands in the world.
  struct Handle {
    u32 handle = 0;
    audio::VoiceHandle voice;
    bool two_d = false;
    WorldPos at{};
    audio::SourceSpatial source;
  };
  Handle handles[k_handle_slots];
  u32 issued = 0;
  // The listener and the mixer's frame.
  bool has_listener = false;
  WorldPos listener{};
  WorldPos frame{};
  Vec3 forward{0.0f, 0.0f, -1.0f};
  Vec3 up{0.0f, 1.0f, 0.0f};
  // The null backend's pull.
  Vector<f32> block;
  u32 block_frames = 0;
  u64 frame_numerator = 0;  // ticks x 48,000, for the exact remainder
  u64 hash = 0;
  // The WAV.
  Vector<f32> wav;
  u32 wav_fill = 0;  // frames in `wav`
  bool wav_ok = true;
  std::string wav_error;
  StageStats stats;

  u32 channels() const noexcept { return mixer != nullptr ? mixer->channels() : 0u; }

  void flush_wav() noexcept {
    if (config.wav_out.empty() || wav_fill == 0 || !wav_ok) {
      wav_fill = 0;
      return;
    }
    const std::string_view bytes(reinterpret_cast<const char*>(wav.data()),
                                 static_cast<usize>(wav_fill) * channels() * sizeof(f32));
    if (io::append_file(config.wav_out, bytes) != io::Status::Ok) {
      wav_ok = false;
      wav_error = "cannot write " + config.wav_out;
      wav_fill = 0;
      stop_audio(wav_error);
      return;
    }
    stats.wav_frames += wav_fill;
    wav_fill = 0;
  }

  // **Audio stopped**: the output closed, nothing mixed or sent from here on, the reason logged
  // once and kept for the summary; the names and the count go on answering as before.
  bool stopped = false;
  std::string stopped_why;
  u64 stopped_tick = 0;
  void stop_audio(const std::string& why) {
    if (stopped) return;
    stopped = true;
    stopped_why = why;
    stopped_tick = stats.ticks;
    if (output != nullptr) output->close();
    ENGINE_LOG_WARN(log_world_sound, "audio stopped; the game's calls are answered as before",
                    log::field("why", std::string_view(stopped_why)));
  }

  // A source in the mixer's frame: its world position less the frame's corner.
  void place(Handle& h) noexcept { h.source.position = relative(h.at, frame); }

  // The world's sound (S112): open when the config asks for it and the stage sounds.
  WorldField field;
  bool frame_moved = false;  // since the last tick: the world's voices are placed again
};

SoundStage::SoundStage() : impl_(std::make_unique<Impl>()) {}

SoundStage::~SoundStage() {
  close(nullptr);
  impl_->output.reset();
  impl_->mixer.reset();
  impl_->store.reset();
}

bool SoundStage::open(const StageConfig& config, const scene::Sound* block, std::string* error) {
  Impl& s = *impl_;
  if (s.open) {
    if (error != nullptr) *error = "the sound stage is already open";
    return false;
  }
  s.config = config;
  if (s.config.tick_hz == 0) s.config.tick_hz = 240;
  s.open = true;
  s.has_block = block != nullptr;
  // With no block and the world off there is nothing to hear; with the world on (S112) the mixer
  // is built for the wind's voices, and a module's calls still answer `Unsupported`.
  if (block == nullptr && !config.world) return true;
  static const scene::Sound k_no_block{};
  if (block == nullptr) block = &k_no_block;
  s.clips.reserve(block->clips.size());
  for (const scene::SoundClip& c : block->clips) {
    std::string why;
    if (!check_clip(c, &why)) {
      if (error != nullptr) *error = why;
      return false;
    }
    Impl::Clip clip;
    clip.name = c.name;
    clip.source = c.recipe.has_value() ? std::string("recipe") : join(config.base_dir, c.path);
    clip.gain = static_cast<f32>(gain_from_db(static_cast<f64>(c.gain_db)));
    clip.min_m = c.min_m;
    clip.max_m = c.max_m;
    s.clips.push_back(std::move(clip));
  }
  if (config.mode == AudioMode::Off) return true;

  audio::ClipStoreConfig store_config;
  store_config.budget_bytes = config.clip_budget_bytes;
  s.store = std::make_unique<audio::ClipStore>(store_config);
  for (u32 i = 0; i < s.clips.size(); ++i) {
    Impl::Clip& clip = s.clips[i];
    const scene::SoundClip& c = block->clips[i];
    if (c.recipe.has_value()) {
      Vector<f32> pcm;
      std::string why;
      if (!render_recipe(*c.recipe, pcm, &why)) {
        if (error != nullptr) *error = "the sound clip '" + c.name + "': " + why;
        return false;
      }
      const std::span<const u8> bytes(reinterpret_cast<const u8*>(pcm.data()),
                                      pcm.size() * sizeof(f32));
      clip.handle =
          s.store->add_pcm(audio::clip_key(bytes), std::span<const f32>(pcm.data(), pcm.size()), 1);
    } else {
      const std::span<const u8> key(reinterpret_cast<const u8*>(clip.source.data()),
                                    clip.source.size());
      clip.handle = s.store->load_file(audio::clip_key(key), clip.source);
    }
    s.store->wait();
    if (s.store->state(clip.handle) != audio::ClipState::Ready) {
      if (error != nullptr)
        *error = "the sound clip '" + c.name + "' (" + clip.source + ") did not decode";
      return false;
    }
    clip.frames = s.store->info(clip.handle).frames;
  }

  // The layout: the null backend's is stereo; a device's is chosen from what it reports.
  audio::MixerConfig mixer_config;
  mixer_config.voices = config.voices;
  mixer_config.layout = audio::ChannelLayout::Stereo;
  // A game's clips lose their top with distance as the world's voices do (S114; world_sound.md,
  // "Distance"): the mixer's one-pole by distance, at the air the world is heard in.
  {
    const FieldConfig air = field_config_from_tunables();
    if (air.path) {
      mixer_config.air_db_per_m_8k =
          static_cast<f32>(air_absorption_db_per_m(8000.0, air.temperature_c, air.humidity_pct));
      mixer_config.air_db_per_m_1k =
          static_cast<f32>(air_absorption_db_per_m(1000.0, air.temperature_c, air.humidity_pct));
    }
  }
  audio::OutputConfig output_config;
  output_config.backend = audio::OutputBackend::Null;
  if (config.mode == AudioMode::Device) {
    output_config.backend = audio::OutputBackend::Device;
    output_config.device = config.device;
    const audio::DeviceList list = audio::enumerate_devices();
    for (const audio::DeviceInfo& d : list.devices) {
      if (config.device.empty() ? d.is_default : d.name == config.device) {
        mixer_config.layout = audio::resolve_layout(d);
        break;
      }
    }
    if (mixer_config.layout == audio::ChannelLayout::Unknown)
      mixer_config.layout = audio::ChannelLayout::Stereo;
  }
  s.mixer = std::make_unique<audio::Mixer>(*s.store, mixer_config);
  s.output = std::make_unique<audio::Output>(*s.mixer);
  // A device that will not open leaves the stage silent, never the session failed: the game's
  // calls are answered as before (apps.md "Sound a module plays", "When the device fails").
  if (config.fault_refuse_open) {
    s.stop_audio("the output would not open: refused by the fault seam");
    return true;
  }
  if (!s.output->open(output_config)) {
    s.stop_audio("the output would not open: " + s.output->fallback_reason());
    return true;
  }
  if (!s.output->fallback_reason().empty()) {
    ENGINE_LOG_WARN(log_world_sound, "no audio device, the null backend instead",
                    log::field("why", std::string_view(s.output->fallback_reason())));
  }
  if (config.world) {
    FieldConfig field = field_config_from_tunables();
    field.turbulence = config.turbulence;
    s.field.open(s.mixer.get(), s.config.tick_hz, field);
  }
  const u32 ch = s.mixer->channels();
  s.block_frames = 0;
  s.block.resize(static_cast<usize>(k_sample_rate / s.config.tick_hz + 1) * ch, 0.0f);
  if (!config.wav_out.empty()) {
    u8 header[44];
    wav_header(header, ch, 0);
    if (io::write_file(config.wav_out, std::string_view(reinterpret_cast<const char*>(header),
                                                        44)) != io::Status::Ok) {
      if (error != nullptr) *error = "cannot write " + config.wav_out;
      return false;
    }
    s.wav.resize(static_cast<usize>(k_wav_buffer_frames) * ch, 0.0f);
  }
  return true;
}

bool SoundStage::is_open() const noexcept { return impl_->open; }
bool SoundStage::has_block() const noexcept { return impl_->has_block; }
AudioMode SoundStage::mode() const noexcept { return impl_->config.mode; }

Answer SoundStage::clip(std::string_view name, u32& out) const noexcept {
  const Impl& s = *impl_;
  if (!s.has_block) return Answer::Unsupported;
  for (u32 i = 0; i < s.clips.size(); ++i) {
    if (s.clips[i].name == name) {
      out = i;
      return Answer::Ok;
    }
  }
  return Answer::NotFound;
}

Answer SoundStage::play(u32 clip, const WorldPos* at, f32 gain, u32& out_handle) noexcept {
  Impl& s = *impl_;
  if (!s.has_block) return Answer::Unsupported;
  if (clip >= s.clips.size() || !std::isfinite(gain) || gain < 0.0f || gain > 16.0f ||
      (at != nullptr && (!finite(at->x) || !finite(at->y) || !finite(at->z)))) {
    ++s.stats.refused;
    return Answer::InvalidArgument;
  }
  // The handle first, and whatever the mixer says after: the answer is the count's.
  out_handle = ++s.issued;
  ++s.stats.plays;
  Impl::Handle& h = s.handles[out_handle % k_handle_slots];
  h = Impl::Handle{};
  h.handle = out_handle;
  if (s.mixer == nullptr || s.stopped) return Answer::Ok;
  const Impl::Clip& c = s.clips[clip];
  audio::PlayParams p;
  p.clip = c.handle;
  p.gain = gain * c.gain;
  p.bus = audio::k_bus_sfx;
  h.two_d = at == nullptr;
  if (h.two_d) {
    p.source.flags = audio::k_source_2d;
  } else {
    h.at = *at;
    h.source.min_distance = c.min_m;
    h.source.max_distance = c.max_m;
    s.place(h);
    p.source = h.source;
  }
  h.voice = s.mixer->play(p);
  if (h.voice.is_null()) {
    ++s.stats.voices_refused;
  } else {
    ++s.stats.voices_started;
  }
  return Answer::Ok;
}

Answer SoundStage::stop(u32 handle) noexcept {
  Impl& s = *impl_;
  if (!s.has_block) return Answer::Unsupported;
  if (handle == 0 || handle > s.issued) {
    ++s.stats.refused;
    return Answer::InvalidArgument;
  }
  ++s.stats.stops;
  Impl::Handle& h = s.handles[handle % k_handle_slots];
  if (s.mixer != nullptr && !s.stopped && h.handle == handle && !h.voice.is_null()) {
    s.mixer->stop(h.voice);
    h.voice = audio::VoiceHandle{};
  }
  return Answer::Ok;
}

void SoundStage::set_listener(const WorldPos& at, const Vec3& forward, const Vec3& up) noexcept {
  Impl& s = *impl_;
  if (s.mixer == nullptr || s.stopped) return;
  const bool same = s.has_listener && at.x == s.listener.x && at.y == s.listener.y &&
                    at.z == s.listener.z && forward.x == s.forward.x && forward.y == s.forward.y &&
                    forward.z == s.forward.z && up.x == s.up.x && up.y == s.up.y && up.z == s.up.z;
  if (same) return;
  s.has_listener = true;
  s.listener = at;
  s.forward = forward;
  s.up = up;
  const WorldPos corner = cell_corner(at);
  if (corner.x != s.frame.x || corner.y != s.frame.y || corner.z != s.frame.z) {
    // The frame moves only when the listener crosses into another cell; every voice still
    // sounding in 3D is placed again in it, one command each.
    s.frame = corner;
    s.frame_moved = true;
    ++s.stats.frame_moves;
    for (Impl::Handle& h : s.handles) {
      if (h.handle == 0 || h.two_d || h.voice.is_null() || !s.mixer->is_live(h.voice)) continue;
      s.place(h);
      s.mixer->set_source(h.voice, h.source);
      ++s.stats.source_resends;
    }
  }
  audio::Listener l;
  l.position = relative(at, s.frame);
  l.forward = forward;
  l.up = up;
  s.mixer->set_listener(l);
  ++s.stats.listener_moves;
}

void SoundStage::tick() noexcept {
  Impl& s = *impl_;
  ++s.stats.ticks;
  if (s.mixer == nullptr || !s.open || s.stopped) return;
  // The world's voices, described from this tick's wind before the mixer takes its commands.
  if (s.field.is_open() && s.has_listener) {
    s.field.tick(s.listener, s.forward, s.frame, s.frame_moved);
    s.frame_moved = false;
  }
  s.mixer->update();
  s.output->update();
  // A device lost mid-session that could not be opened again: the output fell back to the null
  // backend, and the stage goes silent rather than mixing for nobody.
  if (s.config.mode == AudioMode::Device && s.output->backend() == audio::OutputBackend::Null) {
    s.stop_audio("the device was lost: " + s.output->fallback_reason());
    return;
  }
  if (s.output->backend() != audio::OutputBackend::Null) return;
  if (s.config.fault_fail_write_at_tick != 0 &&
      s.stats.ticks == s.config.fault_fail_write_at_tick) {
    s.stop_audio("the output's write failed: the fault seam, at tick " +
                 std::to_string(s.stats.ticks));
    return;
  }
  // 48,000 / tick_hz frames, the remainder carried: tick t ends at floor(t * 48000 / hz).
  const u64 before = s.frame_numerator / s.config.tick_hz;
  s.frame_numerator += k_sample_rate;
  const u32 frames = static_cast<u32>(s.frame_numerator / s.config.tick_hz - before);
  const u32 ch = s.mixer->channels();
  s.output->render(s.block.data(), frames);
  s.block_frames = frames;
  s.stats.frames += frames;
  s.hash = hash_combine(s.hash, hash_bytes(s.block.data(), static_cast<usize>(frames) * ch * 4u));
  if (s.config.wav_out.empty()) return;
  u32 done = 0;
  while (done < frames) {
    const u32 room = k_wav_buffer_frames - s.wav_fill;
    const u32 take = frames - done < room ? frames - done : room;
    std::memcpy(s.wav.data() + static_cast<usize>(s.wav_fill) * ch,
                s.block.data() + static_cast<usize>(done) * ch,
                static_cast<usize>(take) * ch * sizeof(f32));
    s.wav_fill += take;
    done += take;
    if (s.wav_fill == k_wav_buffer_frames) s.flush_wav();
  }
}

bool SoundStage::close(std::string* error) {
  Impl& s = *impl_;
  (void)error;  // nothing fails a session here: a WAV that could not be written stops the audio
  if (!s.open) return true;
  bool ok = true;
  if (s.mixer != nullptr && !s.config.wav_out.empty() && s.wav_ok) {
    s.flush_wav();
    if (s.wav_ok) {
      // The sizes the header said zero to: read the file back, patch its header, write it whole.
      std::string text;
      if (io::read_file(s.config.wav_out, text) == io::Status::Ok && text.size() >= 44) {
        u8 header[44];
        wav_header(header, s.mixer->channels(), s.stats.wav_frames);
        std::memcpy(text.data(), header, 44);
        if (io::write_file(s.config.wav_out, text) != io::Status::Ok) {
          s.wav_ok = false;
          s.wav_error = "cannot write " + s.config.wav_out;
        }
      } else {
        s.wav_ok = false;
        s.wav_error = "cannot read back " + s.config.wav_out;
      }
    }
    if (!s.wav_ok) s.stop_audio(s.wav_error);  // said in the summary, never a failed session
  }
  // The device stops pulling; the mixer and its counters stay for the summary, and go with the
  // stage (the output first: its thread calls into the mixer).
  if (s.output != nullptr) s.output->close();
  s.open = false;
  return ok;
}

void SoundStage::set_canopy(const CanopySource& canopy) noexcept {
  impl_->field.set_canopy(canopy);
}

void SoundStage::set_shelter(const ShelterSource& shelter) noexcept {
  impl_->field.set_shelter(shelter);
}
void SoundStage::set_flow(const FlowSource& flow) noexcept { impl_->field.set_flow(flow); }
void SoundStage::set_surf(const SurfSource& surf) noexcept { impl_->field.set_surf(surf); }
void SoundStage::set_wind(const WorldWind& wind) noexcept { impl_->field.set_wind(wind); }
const WorldStats& SoundStage::world_stats() const noexcept { return impl_->field.stats(); }
u32 SoundStage::world_voice_count() const noexcept { return impl_->field.voice_count(); }
WorldVoiceInfo SoundStage::world_voice(u32 index) const noexcept {
  return impl_->field.voice(index);
}

const StageStats& SoundStage::stats() const noexcept { return impl_->stats; }
const f32* SoundStage::last_block() const noexcept { return impl_->block.data(); }
u32 SoundStage::last_block_frames() const noexcept { return impl_->block_frames; }
u32 SoundStage::channels() const noexcept { return impl_->channels(); }
u64 SoundStage::render_hash() const noexcept { return impl_->hash; }

JsonValue SoundStage::summary_json() const {
  const Impl& s = *impl_;
  JsonValue out = JsonValue::object();
  out.set("mode", JsonValue(std::string_view(audio_mode_name(s.config.mode))));
  out.set("block", JsonValue(s.has_block));
  out.set("backend",
          JsonValue(std::string_view(
              s.output != nullptr ? audio::output_backend_name(s.output->backend()) : "none")));
  if (s.output != nullptr && !s.output->device().name.empty())
    out.set("device", JsonValue(std::string_view(s.output->device().name)));
  out.set("channels", JsonValue(static_cast<u64>(s.channels())));
  JsonValue clips = JsonValue::array();
  for (const Impl::Clip& c : s.clips) {
    JsonValue clip = JsonValue::object();
    clip.set("name", JsonValue(std::string_view(c.name)));
    clip.set("source", JsonValue(std::string_view(c.source)));
    clip.set("frames", JsonValue(static_cast<u64>(c.frames)));
    clips.push_back(std::move(clip));
  }
  out.set("clips", std::move(clips));
  out.set("ticks", JsonValue(s.stats.ticks));
  out.set("plays", JsonValue(s.stats.plays));
  out.set("stops", JsonValue(s.stats.stops));
  out.set("refused", JsonValue(s.stats.refused));
  out.set("voices_started", JsonValue(s.stats.voices_started));
  out.set("voices_refused", JsonValue(s.stats.voices_refused));
  out.set("listener_moves", JsonValue(s.stats.listener_moves));
  out.set("frame_moves", JsonValue(s.stats.frame_moves));
  out.set("frames", JsonValue(s.stats.frames));
  if (!s.config.wav_out.empty()) {
    out.set("wav", JsonValue(std::string_view(s.config.wav_out)));
    out.set("wav_frames", JsonValue(s.stats.wav_frames));
  }
  if (s.mixer != nullptr) {
    const audio::MixerStats m = s.mixer->stats();
    out.set("commands", JsonValue(m.commands));
    out.set("clipped_samples", JsonValue(m.clipped_samples));
    out.set("voices_peak", JsonValue(static_cast<u64>(m.voices_peak)));
  }
  if (s.field.is_open()) {
    const WorldStats& w = s.field.stats();
    JsonValue world = JsonValue::object();
    world.set("refreshes", JsonValue(w.refreshes));
    world.set("crowns_seen", JsonValue(w.crowns_seen));
    world.set("crowns_dropped", JsonValue(w.crowns_dropped));
    world.set("commands", JsonValue(w.commands));
    world.set("plays", JsonValue(w.plays));
    world.set("stops", JsonValue(w.stops));
    world.set("crown_voices", JsonValue(static_cast<u64>(w.crown_voices)));
    world.set("bed_voices", JsonValue(static_cast<u64>(w.bed_voices)));
    world.set("ear_voices", JsonValue(static_cast<u64>(w.ear_voices)));
    world.set("stretches_seen", JsonValue(w.stretches_seen));  // S113
    world.set("surf_seen", JsonValue(w.surf_seen));
    world.set("water_dropped", JsonValue(w.water_dropped));
    world.set("surf_culled", JsonValue(w.surf_culled));
    world.set("stream_voices", JsonValue(static_cast<u64>(w.stream_voices)));
    world.set("fall_voices", JsonValue(static_cast<u64>(w.fall_voices)));
    world.set("surf_voices", JsonValue(static_cast<u64>(w.surf_voices)));
    world.set("bubbles", JsonValue(w.bubbles));
    // Each kind's mean level at the listener over the run, dB SPL after the hold, and the ticks on
    // which running water stood over the wind's own (S113: a listening run's levels by kind).
    JsonValue levels = JsonValue::object();
    const char* names[WorldField::k_level_kinds] = {"crowns",  "beds",  "beds_water", "ears",
                                                    "streams", "falls", "surf"};
    for (u32 k = 0; k < WorldField::k_level_kinds; ++k)
      levels.set(names[k], JsonValue(impl_->field.mean_level_db(k)));
    world.set("levels_db", std::move(levels));
    world.set("water_over_wind_ticks", JsonValue(impl_->field.water_over_wind_ticks()));
    world.set("level_ticks", JsonValue(impl_->field.level_ticks()));
    world.set("held_db", JsonValue(static_cast<f64>(w.held_db)));
    world.set("held_ticks", JsonValue(w.held_ticks));
    world.set("place_retries", JsonValue(w.place_retries));
    world.set("shelter", JsonValue(static_cast<f64>(w.shelter)));
    out.set("world", std::move(world));
  }
  out.set("stopped", JsonValue(s.stopped));
  if (s.stopped) {
    out.set("stopped_why", JsonValue(std::string_view(s.stopped_why)));
    out.set("stopped_tick", JsonValue(s.stopped_tick));
  }
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(s.hash));
  out.set("render_hash", JsonValue(std::string_view(hex)));
  return out;
}

}  // namespace engine::world_sound

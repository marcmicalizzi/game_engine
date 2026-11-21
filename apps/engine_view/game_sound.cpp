// The session's sound for a game (game_sound.h; docs/subsystems/apps.md, "Sound a module plays").

#include "game_sound.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <schemas/scene.h>

namespace engine::view {

namespace {

hotload::Result result_of(world_sound::Answer a) noexcept {
  switch (a) {
    case world_sound::Answer::Ok: return hotload::Result::Ok;
    case world_sound::Answer::NotFound: return hotload::Result::NotFound;
    case world_sound::Answer::Unsupported: return hotload::Result::Unsupported;
    case world_sound::Answer::InvalidArgument: return hotload::Result::InvalidArgument;
  }
  return hotload::Result::Unsupported;
}

world_sound::SoundStage& stage_of(void* user) {
  return *static_cast<world_sound::SoundStage*>(user);
}

hotload::Result clip(void* user, std::string_view name, u32& out) {
  return result_of(stage_of(user).clip(name, out));
}
hotload::Result play(void* user, u32 clip, const f64* position, f32 gain, u32& out) {
  if (position == nullptr) return result_of(stage_of(user).play(clip, nullptr, gain, out));
  const WorldPos at{position[0], position[1], position[2]};
  return result_of(stage_of(user).play(clip, &at, gain, out));
}
hotload::Result stop(void* user, u32 voice) { return result_of(stage_of(user).stop(voice)); }
void tick(void* user, const WorldPos& eye, f32 heading, f32 pitch) {
  world_sound::SoundStage& stage = stage_of(user);
  Vec3 forward;
  Vec3 up;
  listener_axes(heading, pitch, forward, up);
  stage.set_listener(eye, forward, up);
  stage.tick();
}
JsonValue summary(const void* user) {
  return static_cast<const world_sound::SoundStage*>(user)->summary_json();
}
bool finish(void* user, std::string* error) { return stage_of(user).close(error); }

}  // namespace

bool resolve_audio(std::string_view flag, bool heard_by_a_person, world_sound::AudioMode& mode,
                   std::string& device, std::string* error) {
  device.clear();
  if (flag.empty()) {
    if (error != nullptr) *error = "--audio names auto, off, null or a device";
    return false;
  }
  if (flag == "auto") {
    mode = heard_by_a_person ? world_sound::AudioMode::Device : world_sound::AudioMode::Off;
  } else if (flag == "off") {
    mode = world_sound::AudioMode::Off;
  } else if (flag == "null") {
    mode = world_sound::AudioMode::Null;
  } else {
    mode = world_sound::AudioMode::Device;
    device = std::string(flag);
  }
  return true;
}

void listener_axes(f32 heading, f32 pitch, Vec3& forward, Vec3& up) noexcept {
  const f32 sh = std::sin(heading);
  const f32 ch = std::cos(heading);
  const f32 sp = std::sin(pitch);
  const f32 cp = std::cos(pitch);
  forward = Vec3{-sh * cp, sp, -ch * cp};
  up = Vec3{sh * sp, cp, ch * sp};
}

bool GameSoundHost::open(world_sound::AudioMode mode, const std::string& device,
                         const std::string& wav_out, const std::string& scene, u32 tick_hz,
                         GameRunner& game, std::string* error) {
  scene::Sound block;
  bool present = false;
  if (!scene.empty() && !world_sound::read_scene_sound(scene, block, present, error)) return false;
  world_sound::StageConfig config;
  config.mode = mode;
  config.device = device;
  config.wav_out = wav_out;
  config.tick_hz = tick_hz;
  if (!scene.empty()) config.base_dir = std::filesystem::path(scene).parent_path().string();
  if (!stage_.open(config, present ? &block : nullptr, error)) return false;
  GameSound hooks;
  hooks.user = &stage_;
  hooks.clip = &clip;
  hooks.play = &play;
  hooks.stop = &stop;
  hooks.tick = &tick;
  hooks.summary = &summary;
  hooks.finish = &finish;
  game.set_sound(hooks);
  if (present) {
    std::fprintf(stderr, "engine-view: sound: %u clip%s, audio %s\n",
                 static_cast<u32>(block.clips.size()), block.clips.size() == 1 ? "" : "s",
                 world_sound::audio_mode_name(mode));
  }
  return true;
}

}  // namespace engine::view

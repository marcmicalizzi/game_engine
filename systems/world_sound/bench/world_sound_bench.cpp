// Micro-benchmarks for the world_sound capability (docs/subsystems/world_sound.md, "Performance
// notes"; plan 11 §11.8). One iteration is one host tick of the stage under the null backend at 60
// ticks a second: the controlling thread's update and the audio thread's block of 800 frames (16.7
// ms of sound) with N clip voices sounding, which is what a game's clips cost a session.
#include <foundation/bench/bench.h>
#include <systems/world_sound/world_sound.h>

#include <schemas/scene.h>

using namespace engine;

ENGINE_BENCH_ARGS(world_sound_tick, "world_sound.tick_voices", 0, 8, 32, 64) {
  const u32 voices = static_cast<u32>(state.arg());
  scene::Sound block;
  scene::SoundClip tone;
  tone.name = "tone";
  scene::SoundRecipe recipe;
  recipe.kind = scene::SoundRecipeKind::Tone;
  recipe.frequency_hz = 440.0f;
  recipe.duration_s = 10.0f;
  tone.recipe = recipe;
  block.clips.push_back(tone);
  world_sound::SoundStage stage;
  world_sound::StageConfig config;
  config.mode = world_sound::AudioMode::Null;
  config.tick_hz = 60;
  config.voices = 64;
  std::string error;
  if (!stage.open(config, &block, &error)) return;
  stage.set_listener(WorldPos{0.0, 0.0, 0.0}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f});
  u32 handles[64] = {};
  const auto start = [&] {
    for (u32 v = 0; v < voices; ++v) {
      if (handles[v] != 0) stage.stop(handles[v]);
      const WorldPos at{static_cast<f64>(v % 8) - 4.0, 0.0, -2.0 - static_cast<f64>(v / 8)};
      stage.play(0, &at, 0.1f, handles[v]);
    }
  };
  start();
  u32 ticks = 0;
  while (state.keep_running()) {
    if (++ticks == 500) {  // 8.3 s: before the 10 s tone runs out
      ticks = 0;
      start();
    }
    stage.tick();
    bench::keep(stage.last_block()[0]);
  }
  state.set_items(1);
}

// Micro-benchmarks for the world_sound capability (docs/subsystems/world_sound.md, "Performance
// notes"; plan 11 §11.8). One iteration is one host tick of the stage under the null backend at 60
// ticks a second: the controlling thread's update and the audio thread's block of 800 frames (16.7
// ms of sound) with N clip voices sounding, which is what a game's clips cost a session.
#include <foundation/bench/bench.h>
#include <foundation/tunables/tunables.h>
#include <systems/world_sound/world_sound.h>

#include <cmath>
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

namespace {

// A grove for `world_sound.world_tick`: N crowns on a 7 m grid round the listener, every tenth a
// conifer, so the six crowns of their own, the eight beds and the ears all sound.
struct Grove {
  Vector<world_sound::Crown> crowns;
  static void visit(void* user, const WorldPos& centre, f64 reach_m,
                    const world_sound::CrownSink& sink) {
    for (const world_sound::Crown& c : static_cast<Grove*>(user)->crowns) {
      const f64 dx = c.at.x - centre.x;
      const f64 dz = c.at.z - centre.z;
      if (dx * dx + dz * dz <= reach_m * reach_m) sink.emit(sink.user, c);
    }
  }
};

}  // namespace

// **The world's sound, one host tick** (S112): the wind described at every world voice (the gust
// at each crown's and bed's place, the ears), a refresh of the canopy table every 7.5 ticks at 60
// a second (`audio.world.refresh_hz` 8), and the audio thread's 800-frame block of 16 shaped-noise
// voices: what a session's world sound costs the frame's thread and the audio thread together.
// Since S114 every crown's path is taken at a refresh (path.h).
void world_tick(bench::State& state, bool path) {
  // S114: the path's terms on (the default) or the spreading alone, S112's world, for the pair.
  tunables::Tunable* terms = tunables::find("audio.world.path");
  if (terms != nullptr) terms->set_from_text(path ? "true" : "false");
  const u32 n = static_cast<u32>(state.arg());
  Grove grove;
  const u32 side = 1 + static_cast<u32>(std::sqrt(static_cast<f64>(n)));
  for (u32 i = 0; i < n; ++i) {
    world_sound::Crown c;
    c.at = WorldPos{7.0 * (static_cast<f64>(i % side) - side / 2.0), 0.0,
                    7.0 * (static_cast<f64>(i / side) - side / 2.0)};
    c.height_m = 9.0f;
    c.leaf_area_m2 = 150.0f;
    c.kind = i % 10 == 0 ? world_sound::CanopyClass::Needle : world_sound::CanopyClass::Blade;
    grove.crowns.push_back(c);
  }
  world_sound::SoundStage stage;
  world_sound::StageConfig config;
  config.mode = world_sound::AudioMode::Null;
  config.tick_hz = 60;
  config.world = true;
  std::string error;
  if (!stage.open(config, nullptr, &error)) return;
  stage.set_canopy(world_sound::CanopySource{&grove, &Grove::visit});
  gfx::WindDesc wind;
  wind.kind = gfx::WindKind::fixed;
  wind.speed_m_s = 6.0;
  wind.to_deg = 30.0;
  world_sound::WorldWind frame;
  frame.scene = &wind;
  frame.sky_time_s = 3600.0;
  f64 x = 0.0;
  while (state.keep_running()) {
    x += 1.4 / 60.0;  // a walk
    stage.set_listener(WorldPos{x, 1.7, 0.0}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f});
    frame.sky_time_s += 1.0 / 60.0;
    frame.world_time_s += 1.0 / 60.0;
    stage.set_wind(frame);
    stage.tick();
    bench::keep(stage.last_block()[0]);
  }
  state.set_items(1);
  if (terms != nullptr) terms->reset();
}

ENGINE_BENCH_ARGS(world_sound_world_tick, "world_sound.world_tick", 100, 1000, 8000) {
  world_tick(state, true);
}

// The same with the path's terms off: what S114's air, forest and ground cost a refresh.
ENGINE_BENCH_ARGS(world_sound_world_tick_spreading, "world_sound.world_tick_spreading", 100, 1000,
                  8000) {
  world_tick(state, false);
}

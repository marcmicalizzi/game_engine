// What the emitter system costs a tick (docs/subsystems/audio_system.md, "Performance notes"): a
// world of N emitters around a listener, a 64-voice mixer, one `SimWorld::step()` per iteration.
// Items are emitters, so "items per second" is emitter-ticks per second.
//
//   audio_system.tick.still    nothing changes: the diff finds nothing and sends nothing — the
//                              floor, which is what most emitters cost most ticks
//   audio_system.tick.moving   every emitter moves every tick: one source update per voiced emitter
//
// 473 emitters are within reach of a 64-voice pool, so this also prices the LOD test and the
// refusals the pool hands out every tick. The mixer is pumped with a zero-frame render after
// each tick so its command ring never fills; nothing is mixed.

#include <core/containers/vector.h>
#include <domain/audio/audio.h>
#include <domain/ecs/sim_world.h>
#include <foundation/bench/bench.h>
#include <systems/audio_system/audio_system.h>

#include <flecs.h>

using namespace engine;
using namespace engine::audio;

namespace {

void run(bench::State& state, bool moving) {
  const u32 count = static_cast<u32>(state.arg());
  ClipStore clips;
  Mixer mixer(clips);
  Vector<f32> pcm(4800u, 0.1f);
  const Id128 key{3, 3};
  clips.add_pcm(key, pcm, 1);
  ecs::SimWorld sim;
  AudioSystem system(mixer);
  system.install(sim);

  Vector<flecs::entity_t> entities;
  for (u32 i = 0; i < count; ++i) {
    AudioEmitter e;
    e.clip = key;
    e.looping = true;
    e.max_distance = 30.0f;
    // A spiral out from the listener: the near ones are in reach, most are not.
    const f32 r = 2.0f + 0.05f * static_cast<f32>(i);
    e.position =
        absolute(WorldPos::origin(), Vec3{r * (static_cast<f32>(i % 7u) - 3.0f) / 3.0f, 0.0f, -r});
    entities.push_back(sim.world().entity().set(e).id());
  }
  sim.step();
  mixer.render(nullptr, 0);

  u32 tick = 0;
  while (state.keep_running()) {
    if (moving) {
      state.pause_timing();
      for (const flecs::entity_t id : entities) {
        AudioEmitter* e = sim.world().entity(id).try_get_mut<AudioEmitter>();
        e->position.y = 0.01 * static_cast<f64>(tick % 100u);
      }
      state.resume_timing();
    }
    sim.step();
    mixer.render(nullptr, 0);
    ++tick;
  }
  bench::keep(system.stats().emitters);
  state.set_items(count);
}

}  // namespace

ENGINE_BENCH_ARGS(tick_still, "audio_system.tick.still", 1000, 10000) { run(state, false); }

ENGINE_BENCH_ARGS(tick_moving, "audio_system.tick.moving", 1000, 10000) { run(state, true); }

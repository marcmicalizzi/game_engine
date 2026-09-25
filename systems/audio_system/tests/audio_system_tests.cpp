// The emitter system (audio_system.h): every change to an AudioEmitter or AudioListener sends
// exactly the mixer commands it implies, and nothing else does. Counted at the mixer's own
// controlling-thread counters, so a command the system sent and should not have is caught as
// surely as one it forgot. All headless: the mixer renders into the test's buffer (the null
// backend) when a test needs voices to run out.
#include <domain/audio/audio.h>
#include <domain/ecs/scheduled_tick.h>
#include <domain/ecs/sim_world.h>
#include <domain/sim/scheduler.h>
#include <systems/audio_system/audio_system.h>

#include <doctest/doctest.h>

#include <cmath>
#include <flecs.h>
#include <optional>
#include <string_view>
#include <vector>

using namespace engine;
using namespace engine::audio;

namespace {

const Id128 k_loop_clip{7, 1};
const Id128 k_shot_clip{7, 2};

// What the mixer was asked to do over one tick.
struct Sent {
  u64 plays = 0;
  u64 stops = 0;
  u64 params = 0;
  u64 sources = 0;
  u64 listeners = 0;
};

Sent counters(const Mixer& mixer) {
  const ControlStats& c = mixer.control_stats();
  return Sent{c.plays, c.stops, c.params, c.sources, c.listeners};
}

MixerConfig with_voices(u32 voices) {
  MixerConfig config;
  config.voices = voices;
  return config;
}

struct Rig {
  ClipStore clips;
  Mixer mixer;
  ecs::SimWorld sim;
  AudioSystem audio{mixer};
  Vector<f32> buffer;

  explicit Rig(const MixerConfig& config = {}) : mixer(clips, config) {
    Vector<f32> pcm;
    for (u32 i = 0; i < 4800; ++i)
      pcm.push_back(static_cast<f32>(i % 48u) / 96.0f);
    clips.add_pcm(k_loop_clip, pcm, 1);
    Vector<f32> shot;
    for (u32 i = 0; i < 200; ++i)
      shot.push_back(0.25f);
    clips.add_pcm(k_shot_clip, shot, 1);
    audio.install(sim);
    buffer.resize_exact(480u * mixer.channels());
  }

  // One tick, then one block rendered so the commands are applied and voices can run out. Returns
  // the commands the tick sent.
  Sent tick() {
    const Sent before = counters(mixer);
    sim.step();
    const Sent after = counters(mixer);
    mixer.render(buffer.data(), 480);
    return Sent{after.plays - before.plays, after.stops - before.stops,
                after.params - before.params, after.sources - before.sources,
                after.listeners - before.listeners};
  }
};

void expect(const Sent& sent, u64 plays, u64 stops, u64 params, u64 sources, u64 listeners = 0) {
  CHECK(sent.plays == plays);
  CHECK(sent.stops == stops);
  CHECK(sent.params == params);
  CHECK(sent.sources == sources);
  CHECK(sent.listeners == listeners);
}

AudioEmitter loop_at(Vec3 position) {
  AudioEmitter e;
  e.clip = k_loop_clip;
  e.position = position;
  e.looping = true;
  return e;
}

}  // namespace

TEST_CASE("audio_system declares what it touches and when it runs") {
  Rig rig;
  const sim::SystemDesc& desc = rig.audio.desc();
  CHECK(std::string_view{desc.name} == "audio.emitters");
  CHECK(desc.phase == sim::TickPhase::EventsOut);
  CHECK(desc.determinism == sim::Determinism::Derived);
  CHECK(desc.reads.any());
  CHECK(desc.writes.any());
  CHECK_FALSE(desc.reads.intersects(desc.writes));
  CHECK(desc.writes_resources.any());
  CHECK(std::string_view{k_system_determinism} == "derived");
}

TEST_CASE("a new emitter plays once, and an unchanged one sends nothing at all") {
  Rig rig;
  // The listener at the origin is the mixer's default, but the system sends it once so the mixer
  // and the world agree from the first tick.
  const flecs::entity emitter = rig.sim.world().entity().set(loop_at(Vec3{2.0f, 0.0f, -3.0f}));
  expect(rig.tick(), 1, 0, 0, 0, 1);
  CHECK(rig.audio.stats().emitters == 1u);
  CHECK(rig.mixer.live_voices() == 1u);
  for (int i = 0; i < 5; ++i)
    expect(rig.tick(), 0, 0, 0, 0);
  CHECK(emitter.has<EmitterVoice>());
}

TEST_CASE("each change sends exactly the command it implies") {
  Rig rig;
  const flecs::entity emitter = rig.sim.world().entity().set(loop_at(Vec3{2.0f, 0.0f, -3.0f}));
  rig.tick();
  auto edit = [&](auto&& change) {
    change(*emitter.try_get_mut<AudioEmitter>());
    return rig.tick();
  };

  SUBCASE("gain, pitch, pan, bus and looping are one parameter update") {
    expect(edit([](AudioEmitter& e) { e.gain = 0.5f; }), 0, 0, 1, 0);
    expect(edit([](AudioEmitter& e) { e.pitch = 1.5f; }), 0, 0, 1, 0);
    expect(edit([](AudioEmitter& e) {
             e.gain = 0.7f;
             e.bus = k_bus_ambient;
           }),
           0, 0, 1, 0);  // two fields, one command
  }
  SUBCASE("position, orientation, distances, cone and spread are one source update") {
    expect(edit([](AudioEmitter& e) { e.position = Vec3{3.0f, 0.0f, -3.0f}; }), 0, 0, 0, 1);
    expect(edit([](AudioEmitter& e) { e.orientation = Quat{0.0f, 1.0f, 0.0f, 0.0f}; }), 0, 0, 0, 1);
    expect(edit([](AudioEmitter& e) {
             e.directivity = Directivity::Cone;
             e.cone_inner_degrees = 90.0f;
             e.spread = 0.5f;
           }),
           0, 0, 0, 1);
  }
  SUBCASE("a change to both groups is one of each") {
    expect(edit([](AudioEmitter& e) {
             e.gain = 0.2f;
             e.max_distance = 80.0f;
           }),
           0, 0, 1, 1);
  }
  SUBCASE("priority sends nothing: it matters at the next play") {
    expect(edit([](AudioEmitter& e) { e.priority = 250; }), 0, 0, 0, 0);
  }
  SUBCASE("a new cue or a new clip restarts: one stop, one play") {
    expect(edit([](AudioEmitter& e) { ++e.cue; }), 1, 1, 0, 0);
    expect(edit([](AudioEmitter& e) { e.clip = k_shot_clip; }), 1, 1, 0, 0);
  }
  SUBCASE("playing off is a stop, and on again is a play") {
    expect(edit([](AudioEmitter& e) { e.playing = false; }), 0, 1, 0, 0);
    expect(rig.tick(), 0, 0, 0, 0);
    expect(edit([](AudioEmitter& e) { e.playing = true; }), 1, 0, 0, 0);
  }
}

TEST_CASE("the listener is sent when it moves, and only then") {
  Rig rig;
  rig.sim.world().entity().set(loop_at(Vec3{0.0f, 0.0f, -5.0f}));
  expect(rig.tick(), 1, 0, 0, 0, 1);
  AudioListener l;
  l.forward = Vec3{0.0f, 0.0f, -1.0f};
  l.up = Vec3{0.0f, 1.0f, 0.0f};
  const flecs::entity listener = rig.sim.world().entity().set(l);
  // The same pose as the default listener: nothing to send.
  expect(rig.tick(), 0, 0, 0, 0, 0);
  listener.try_get_mut<AudioListener>()->position = Vec3{1.0f, 0.0f, 0.0f};
  expect(rig.tick(), 0, 0, 0, 0, 1);
  expect(rig.tick(), 0, 0, 0, 0, 0);
  // A second listener is counted and ignored: the lowest entity id wins, and that is the first.
  AudioListener other;
  other.position = Vec3{50.0f, 0.0f, 0.0f};
  rig.sim.world().entity().set(other);
  expect(rig.tick(), 0, 0, 0, 0, 0);
  CHECK(rig.audio.stats().listeners == 2u);
}

TEST_CASE("a one-shot plays out and stays silent until it is retriggered") {
  Rig rig;
  AudioEmitter shot;
  shot.clip = k_shot_clip;  // 200 frames: over inside the first rendered block
  shot.position = Vec3{0.0f, 0.0f, -1.0f};
  const flecs::entity emitter = rig.sim.world().entity().set(shot);
  expect(rig.tick(), 1, 0, 0, 0, 1);
  for (int i = 0; i < 4; ++i)
    expect(rig.tick(), 0, 0, 0, 0);
  CHECK(rig.mixer.live_voices() == 0u);
  // Changing its level now sends nothing: there is no voice to change.
  emitter.try_get_mut<AudioEmitter>()->gain = 0.5f;
  expect(rig.tick(), 0, 0, 0, 0);
  // A new cue plays it again.
  ++emitter.try_get_mut<AudioEmitter>()->cue;
  expect(rig.tick(), 1, 0, 0, 0);
  // So does turning `playing` off and on.
  emitter.try_get_mut<AudioEmitter>()->playing = false;
  expect(rig.tick(), 0, 0, 0, 0);  // it had already finished: nothing to stop
  emitter.try_get_mut<AudioEmitter>()->playing = true;
  expect(rig.tick(), 1, 0, 0, 0);
}

TEST_CASE("a destroyed emitter's voice is stopped, and so is one whose component is removed") {
  Rig rig;
  const flecs::entity a = rig.sim.world().entity().set(loop_at(Vec3{1.0f, 0.0f, -1.0f}));
  const flecs::entity b = rig.sim.world().entity().set(loop_at(Vec3{-1.0f, 0.0f, -1.0f}));
  expect(rig.tick(), 2, 0, 0, 0, 1);
  a.destruct();
  const Sent after_destroy = rig.tick();
  CHECK(after_destroy.stops == 1u);
  CHECK(rig.audio.stats().swept == 1u);
  b.remove<AudioEmitter>();
  const Sent after_remove = rig.tick();
  CHECK(after_remove.stops == 1u);
  CHECK(rig.audio.stats().swept == 1u);
  rig.tick();
  CHECK(rig.mixer.live_voices() == 0u);
}

TEST_CASE("audio LOD: past max_distance the voice goes, back inside it returns") {
  CHECK(lod_tier(0.0f, 10.0f, k_tier_virtual, 0.1f) == k_tier_voiced);
  CHECK(lod_tier(10.5f * 10.5f, 10.0f, k_tier_voiced, 0.1f) == k_tier_voiced);    // in the band
  CHECK(lod_tier(10.5f * 10.5f, 10.0f, k_tier_virtual, 0.1f) == k_tier_virtual);  // in the band
  CHECK(lod_tier(11.5f * 11.5f, 10.0f, k_tier_voiced, 0.1f) == k_tier_virtual);
  CHECK(lod_tier(9.0f * 9.0f, 10.0f, k_tier_virtual, 0.1f) == k_tier_voiced);

  Rig rig;
  AudioEmitter e = loop_at(Vec3{0.0f, 0.0f, -5.0f});
  e.max_distance = 10.0f;
  const flecs::entity emitter = rig.sim.world().entity().set(e);
  expect(rig.tick(), 1, 0, 0, 0, 1);
  // Into the band: it keeps its voice and only its position is sent.
  emitter.try_get_mut<AudioEmitter>()->position = Vec3{0.0f, 0.0f, -10.5f};
  expect(rig.tick(), 0, 0, 0, 1);
  // Past the band: the voice goes.
  emitter.try_get_mut<AudioEmitter>()->position = Vec3{0.0f, 0.0f, -20.0f};
  expect(rig.tick(), 0, 1, 0, 0);
  CHECK(rig.audio.stats().virtualized == 1u);
  expect(rig.tick(), 0, 0, 0, 0);
  // Back inside max_distance: a loop gets a voice again.
  emitter.try_get_mut<AudioEmitter>()->position = Vec3{0.0f, 0.0f, -4.0f};
  expect(rig.tick(), 1, 0, 0, 0);
  CHECK(rig.audio.stats().devirtualized == 1u);

  // A 2D emitter has no distance: it never goes virtual.
  AudioEmitter music = loop_at(Vec3{0.0f, 0.0f, -500.0f});
  music.bus = k_bus_music;
  rig.sim.world().entity().set(music);
  expect(rig.tick(), 1, 0, 0, 0);
  // A one-shot triggered out of reach is over: it does not start when the listener arrives.
  AudioEmitter far_shot;
  far_shot.clip = k_shot_clip;
  far_shot.position = Vec3{0.0f, 0.0f, -100.0f};
  far_shot.max_distance = 10.0f;
  const flecs::entity shot = rig.sim.world().entity().set(far_shot);
  expect(rig.tick(), 0, 0, 0, 0);
  shot.try_get_mut<AudioEmitter>()->position = Vec3{0.0f, 0.0f, -1.0f};
  expect(rig.tick(), 0, 0, 0, 0);
}

TEST_CASE("a loop that comes back within reach resumes where it would have been") {
  // A clip whose every frame says where it is: frame i holds i / 8192, exact in f32. The emitter
  // sits straight ahead at its min_distance, so it is heard centred at full level, and the first
  // sample of the block after a play says which frame the voice started on.
  Rig rig;
  const Id128 k_ramp{7, 3};
  Vector<f32> ramp;
  for (u32 i = 0; i < 4800; ++i)
    ramp.push_back(static_cast<f32>(i) / 8192.0f);
  rig.clips.add_pcm(k_ramp, ramp, 1);
  const f32 centre = sin_quarter(0.5f);
  auto frame_at = [&](u32 k) {
    return static_cast<u32>(std::lround(rig.buffer[2u * k] / centre * 8192.0f));
  };

  AudioEmitter e = loop_at(Vec3{0.0f, 0.0f, -1.0f});
  e.clip = k_ramp;
  e.max_distance = 10.0f;
  const flecs::entity emitter = rig.sim.world().entity().set(e);
  auto move_to = [&](f32 z) {
    emitter.try_get_mut<AudioEmitter>()->position = Vec3{0.0f, 0.0f, z};
  };

  // Tick 1: the loop starts at its beginning. At 60 Hz a tick is 800 frames of the mix.
  expect(rig.tick(), 1, 0, 0, 0, 1);
  CHECK(frame_at(0) == 0u);
  CHECK(frame_at(1) == 1u);
  CHECK(rig.audio.stats().resumed == 0u);
  // Tick 2 it goes out of reach and loses its voice; ticks 3 and 4 it stays out.
  move_to(-50.0f);
  expect(rig.tick(), 0, 1, 0, 0);
  expect(rig.tick(), 0, 0, 0, 0);
  expect(rig.tick(), 0, 0, 0, 0);
  // Tick 5 it is back: four ticks after it started, 3,200 frames into its clip — not frame 0.
  move_to(-1.0f);
  expect(rig.tick(), 1, 0, 0, 0);
  CHECK(frame_at(0) == 3200u);
  CHECK(frame_at(1) == 3201u);
  CHECK(rig.audio.stats().resumed == 1u);

  // Out again at tick 6; its pitch doubles at tick 7 while it has no voice (nothing is sent); back
  // at tick 9. Six ticks at pitch 1 and two at pitch 2 are 4,800 + 3,200 frames: 3,200 into a
  // 4,800-frame loop, now read two frames at a time.
  move_to(-50.0f);
  expect(rig.tick(), 0, 1, 0, 0);
  emitter.try_get_mut<AudioEmitter>()->pitch = 2.0f;
  expect(rig.tick(), 0, 0, 0, 0);
  expect(rig.tick(), 0, 0, 0, 0);
  move_to(-1.0f);
  expect(rig.tick(), 1, 0, 0, 0);
  CHECK(frame_at(0) == 3200u);
  CHECK(frame_at(1) == 3202u);

  // A new cue restarts the loop, and its clock with it: from the beginning. (The block after it
  // also carries the old voice's fade, so the clock is read off the emitter's state instead.)
  ++emitter.try_get_mut<AudioEmitter>()->cue;
  expect(rig.tick(), 1, 1, 0, 0);
  const EmitterVoice* state = emitter.try_get<EmitterVoice>();
  REQUIRE(state != nullptr);
  CHECK(state->loop_tick == rig.sim.tick().value);
  CHECK(state->loop_frames == 0.0);
  CHECK(rig.audio.stats().resumed == 0u);

  // The clock is a function of the tick and the clip's length alone.
  EmitterVoice clock;
  clock.loop_tick = 10;
  clock.loop_frames = 100.0;
  CHECK(loop_position(clock, 10, 800.0, 1.0f, 4800) == 100u);
  CHECK(loop_position(clock, 16, 800.0, 1.0f, 4800) == 100u);  // 4,900 wraps to 100
  CHECK(loop_position(clock, 11, 800.0, 0.5f, 4800) == 500u);
  CHECK(loop_position(clock, 11, 800.0, 1.0f, 0) == 0u);  // no clip: the beginning
}

TEST_CASE("a full pool turns emitters away once a tick, and a stop lets the next one in") {
  Rig rig(with_voices(2));
  flecs::entity loud[3];
  for (u32 i = 0; i < 3; ++i) {
    AudioEmitter e = loop_at(Vec3{static_cast<f32>(i), 0.0f, -2.0f});
    e.priority = 100;
    loud[i] = rig.sim.world().entity().set(e);
  }
  expect(rig.tick(), 2, 0, 0, 0, 1);
  CHECK(rig.audio.stats().refused == 1u);
  CHECK(rig.mixer.control_stats().refused_pool == 1u);

  // Two quieter emitters: the pool has already refused priority 100 this tick, so they are turned
  // away without the mixer being asked — its refusal count moves by one a tick, not three.
  for (u32 i = 0; i < 2; ++i) {
    AudioEmitter e = loop_at(Vec3{-1.0f - static_cast<f32>(i), 0.0f, -2.0f});
    e.priority = 50;
    rig.sim.world().entity().set(e);
  }
  expect(rig.tick(), 0, 0, 0, 0);
  CHECK(rig.audio.stats().refused == 3u);
  CHECK(rig.mixer.control_stats().refused_pool == 2u);

  // One of the loud ones stops: its fading voice is anybody's, and the refused loud one takes it.
  loud[0].try_get_mut<AudioEmitter>()->playing = false;
  expect(rig.tick(), 1, 1, 0, 0);
}

TEST_CASE("an emitter whose clip is not loaded waits, and plays once it is") {
  Rig rig;
  AudioEmitter e = loop_at(Vec3{0.0f, 0.0f, -2.0f});
  e.clip = Id128{99, 99};
  rig.sim.world().entity().set(e);
  expect(rig.tick(), 0, 0, 0, 0, 1);
  CHECK(rig.audio.stats().waiting == 1u);
  expect(rig.tick(), 0, 0, 0, 0);
  Vector<f32> late(480, 0.1f);
  rig.clips.add_pcm(Id128{99, 99}, late, 1);
  expect(rig.tick(), 1, 0, 0, 0);
  CHECK(rig.audio.stats().waiting == 0u);
}

TEST_CASE("a cloned emitter gets a voice of its own rather than sharing its original's") {
  Rig rig;
  const flecs::entity original = rig.sim.world().entity().set(loop_at(Vec3{0.0f, 0.0f, -2.0f}));
  expect(rig.tick(), 1, 0, 0, 0, 1);
  original.clone();  // copies AudioEmitter *and* the EmitterVoice naming the original's voice
  const Sent sent = rig.tick();
  CHECK(sent.plays == 1u);
  CHECK(sent.stops == 0u);
  CHECK(rig.audio.stats().duplicates == 1u);
  CHECK(rig.mixer.live_voices() == 2u);
}

TEST_CASE(
    "the engine's scheduler ticks the emitter system in EventsOut, sending the same commands") {
  // ADR-0038 (proposed): `sim::SimScheduler` owns the clock and the phase order and runs each
  // phase's flecs systems through `ecs::ScheduledTick`. The emitter system declared `EventsOut` and
  // did not change for it, so the commands a sequence of edits sends have to be the same under
  // either executor, tick for tick — and so does the mix they make, which reads the tick too: a
  // loop that comes back within reach starts where the tick count puts it (loop_position).
  struct Run {
    std::vector<Sent> sent;
    Vector<f32> mix;
  };
  const auto run = [](bool scheduled) {
    Rig rig;
    sim::SimScheduler scheduler;
    Run out;
    std::vector<Sent>& sent = out.sent;
    const flecs::entity emitter = rig.sim.world().entity().set(loop_at(Vec3{2.0f, 0.0f, -3.0f}));
    std::optional<ecs::ScheduledTick> tick;
    if (scheduled) tick.emplace(rig.sim, scheduler);
    const auto step = [&]() {
      const Sent before = counters(rig.mixer);
      if (scheduled) {
        scheduler.step();
      } else {
        rig.sim.step();
      }
      const Sent after = counters(rig.mixer);
      rig.mixer.render(rig.buffer.data(), 480);
      out.mix.append(std::span<const f32>(rig.buffer.data(), rig.buffer.size()));
      sent.push_back(Sent{after.plays - before.plays, after.stops - before.stops,
                          after.params - before.params, after.sources - before.sources,
                          after.listeners - before.listeners});
    };
    step();
    emitter.try_get_mut<AudioEmitter>()->gain = 0.5f;
    step();
    emitter.try_get_mut<AudioEmitter>()->position = Vec3{0.0f, 0.0f, -1.0f};
    step();
    emitter.try_get_mut<AudioEmitter>()->position = Vec3{0.0f, 0.0f, -100.0f};  // out of reach
    step();
    step();
    emitter.try_get_mut<AudioEmitter>()->position = Vec3{0.0f, 0.0f, -1.0f};  // back, resumed
    step();
    emitter.try_get_mut<AudioEmitter>()->playing = false;
    step();
    CHECK(rig.sim.tick().value == 7);
    if (scheduled) CHECK(tick->phases_run() == 7);  // EventsOut, once a tick
    CHECK(rig.audio.stats().resumed == 0u);         // the last tick only stopped it
    return out;
  };
  const Run pipeline_run = run(false);
  const Run scheduled_run = run(true);
  const std::vector<Sent>& pipeline = pipeline_run.sent;
  const std::vector<Sent>& scheduled = scheduled_run.sent;
  CHECK(scheduled_run.mix == pipeline_run.mix);  // the same bytes, ramps and resumed loop included
  REQUIRE(pipeline.size() == scheduled.size());
  for (usize i = 0; i < pipeline.size(); ++i) {
    INFO("tick " << i + 1);
    CHECK(scheduled[i].plays == pipeline[i].plays);
    CHECK(scheduled[i].stops == pipeline[i].stops);
    CHECK(scheduled[i].params == pipeline[i].params);
    CHECK(scheduled[i].sources == pipeline[i].sources);
    CHECK(scheduled[i].listeners == pipeline[i].listeners);
  }
  // The sequence itself, as the tests above pin it one edit at a time.
  CHECK(scheduled[0].plays == 1u);
  CHECK(scheduled[1].params == 1u);
  CHECK(scheduled[2].sources == 1u);
  CHECK(scheduled[3].stops == 1u);  // out of reach: virtual
  CHECK(scheduled[5].plays == 1u);  // back: 5 ticks after it started, 4,000 frames in
  CHECK(scheduled[6].stops == 1u);
}

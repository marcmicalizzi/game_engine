#pragma once

// audio_system capability (ADR-0027; docs/plan/02-architecture.md §2.8, docs/plan/05-simulation.md
// §5.11, docs/plan/11-performance-principles.md §11.10). The page is
// docs/subsystems/audio_system.md.
//
// The ECS half of audio: the `AudioEmitter` and `AudioListener` components (declared in
// domain/audio's schema, registered with a world here) and one system that turns their changes
// into mixer commands, once per tick. It is a module of its own, in `systems/`, because it is the
// bridge between an ECS-free module and the world, and `<flecs.h>` belongs to `domain/ecs`,
// `systems/` and `game/` (ADR-0028 seam 5) — `domain/audio` stays free of it, and runs, tests and
// benches with no world at all. Nothing else in the engine learns about audio: the components are
// the whole interface a game touches, and a game that wants a one-shot with no entity calls
// `audio::Mixer::play` on the same thread.
//
//   [x] capability graph    engine_capability_requires(audio_system audio ecs)
//   [x] schema types        domain/audio/schemas/audio.schema: AudioEmitter, AudioListener
//   [x] scheduler entry     one sim::SystemDesc, registered with ecs::register_system in
//                           install(), phase EventsOut
//   [ ] render passes       none
//   [ ] derived data        none
//   [ ] protocol methods    none here: the components are already reachable through world.apply,
//                           and audio.devices is domain/audio's
//   [x] tunables            audio.lod.hysteresis, read once per tick
//   [x] LOD policy          lod_tier(): an emitter past its max_distance holds no voice
//   [x] determinism         k_determinism below
//   [x] zero cost unused    no linked code (ENGINE_WITH_AUDIO_SYSTEM=OFF, or either capability it
//                           requires off); installed and unused, a world with no AudioEmitter
//                           matches no row, and the tick is one listener query and one ring drain
//   [x] docs, tests, size table, bench
//
// **Determinism: derived.** The system reads the components and writes mixer commands and its own
// transient state; nothing it learns from the audio thread — that a one-shot ended, that a voice
// was stolen — is written into a component, because when the device asked for its next block is
// not simulation state. The components are desired state, hashed and saved like any other; the
// commands are output.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/audio/audio.h>
#include <domain/ecs/sim_world.h>
#include <domain/ecs/systems.h>
#include <domain/sim/scheduler.h>

#include <flecs.h>
#include <schemas/audio.h>

namespace engine::audio {

// ADR-0010's stance, in the header as the ADR requires. See the paragraph above.
inline constexpr const char* k_system_determinism = "derived";

// The emitter system runs in `EventsOut`: after gameplay and physics have moved everything this
// tick, and as the tick's output to the world outside it — which is what a sound is.
inline constexpr sim::TickPhase k_phase_emitters = sim::TickPhase::EventsOut;

// The mixer's controlling half — its command ring and voice table — as a `sim` resource, so a
// second system that drives the same mixer (a music director, a footstep system) is ordered
// against this one by the schedule rather than by luck.
inline constexpr const char* k_resource_mixer = "audio.mixer";

// ---- the LOD policy -----------------------------------------------------------------------------
//
// Tier 0 holds a voice; tier 3 is virtual — the emitter keeps its state and holds no voice, costs
// the audio thread nothing, and the pool's voices go to emitters that can be heard. The boundary is
// the emitter's own `max_distance` from the listener, where the tapered and linear distance models
// reach silence, so taking the voice away there is inaudible by construction; a band of
// `audio.lod.hysteresis` beyond it keeps an emitter walking along the boundary from churning its
// voice. A 2D emitter, and one whose distance model never reaches silence (`None`), is always tier
// 0. The boundary is per emitter rather than `sim::TierParams`' shared table because the question
// is "can this emitter be heard", and only the emitter knows how far it carries.
inline constexpr u8 k_tier_voiced = 0;
inline constexpr u8 k_tier_virtual = 3;

// The tier one emitter belongs at: promote inside `max_distance`, demote beyond
// `max_distance * (1 + hysteresis)`, and stay put between. Squared distances, so no square root.
u8 lod_tier(f32 distance_squared, f32 max_distance, u8 current, f32 hysteresis) noexcept;

// ---- per-emitter state --------------------------------------------------------------------------

// The emitter's transient state: a private component (never saved, never on the protocol), added
// with `AudioEmitter` through flecs' `With` so every emitter has one from its first tick. It holds
// the component as last sent — the diff that decides which commands a change implies — and the
// voice the emitter is sounding through, which means nothing outside this process.
struct EmitterVoice {
  AudioEmitter sent;
  VoiceHandle voice;
  u8 tier = k_tier_voiced;
  bool initialized = false;  // `sent` holds a real previous tick
  bool finished = false;     // a one-shot that has played out: silent until retriggered
  bool reserved = false;
  // A loop's clock, whether or not it holds a voice: at tick `loop_tick` it had played
  // `loop_frames` frames of its clip since it started (unwrapped; at its pitch). A loop that comes
  // back within reach, or gets a voice after the pool turned it away, starts where this says it
  // would have been — computed from ticks, since nothing the audio thread does is read back
  // (loop_position()).
  f64 loop_frames = 0.0;
  u64 loop_tick = 0;
};

// Where a loop is in its clip at `tick`: the frames it had played at `state.loop_tick`, plus
// `frames_per_tick` of the mix rate for every tick since at `pitch`, wrapped into the clip's
// `clip_frames`. IEEE f64 and `fmod`, which is exact: the same tick gives the same frame
// everywhere.
u32 loop_position(const EmitterVoice& state, u64 tick, f64 frames_per_tick, f32 pitch,
                  u32 clip_frames) noexcept;

// What the last tick did. Every counter is a command sent or a decision taken, so a test can say
// "exactly this" and mean it.
struct AudioSystemStats {
  u32 emitters = 0;
  u32 plays = 0;    // voices started (a restart is a stop and a play)
  u32 stops = 0;    // voices stopped because the component said so
  u32 params = 0;   // gain, pitch, pan, bus or loop changes sent
  u32 sources = 0;  // spatial-block changes sent
  u32 listener_updates = 0;
  u32 waiting = 0;        // the clip is not Ready yet: retried next tick
  u32 refused = 0;        // the mixer refused the play (pool, parameters): retried next tick
  u32 virtualized = 0;    // voices taken away by the LOD policy
  u32 devirtualized = 0;  // emitters brought back within reach
  u32 swept = 0;          // voices stopped because their emitter is gone
  u32 duplicates = 0;     // copied state (an entity cloned with its EmitterVoice) reset
  u32 listeners = 0;      // AudioListener entities seen; one is used
  u32 resumed = 0;        // loops started somewhere other than their beginning (loop_position)
};

class AudioSystem {
 public:
  // `mixer` must outlive the system. The system drives it from the tick's thread, which makes the
  // tick the mixer's controlling thread: nothing else may call the mixer's controlling half
  // concurrently with `SimWorld::step()`.
  explicit AudioSystem(Mixer& mixer);
  // Stops every voice the system started. The world the system was installed in must not step
  // again once the system is gone.
  ~AudioSystem();
  ENGINE_NON_COPYABLE(AudioSystem);

  // Registers the components (ADR-0028 seam 1), the private state and its `With` rule, and the
  // emitter system (seam 2). Once per world, outside a tick.
  void install(ecs::SimWorld& sim);

  const AudioSystemStats& stats() const noexcept { return stats_; }
  const sim::SystemDesc& desc() const noexcept { return desc_; }

 private:
  void tick(flecs::iter& it);
  void step(const AudioEmitter& emitter, EmitterVoice& state, const Vec3& ear, f32 hysteresis,
            u64 tick, AudioSystemStats& stats);
  void release(VoiceHandle voice) noexcept;
  Listener current_listener(u32& count);

  Mixer* mixer_;
  flecs::world* world_ = nullptr;
  flecs::query<const AudioListener> listeners_;
  sim::SystemDesc desc_{};
  // Per voice slot: the voice this system started there and has not seen end, and whether an
  // emitter claimed it this tick. What is owned and unclaimed at the end of a tick belongs to an
  // emitter that is gone, and is stopped — which is how a destroyed entity's loop stops without
  // this system holding a flecs id past a tick (ADR-0028 seam 3).
  Vector<VoiceHandle> owned_;
  Vector<u8> claimed_;
  Listener sent_listener_;
  bool listener_sent_ = false;
  // The highest priority the pool refused this tick, or -1. Every voice outranks it until
  // something stops, so an emitter at or below it is refused without asking the mixer again.
  i32 refused_priority_ = -1;
  // Mix-rate frames in one tick of the world the system is installed in (800 at 60 Hz): the
  // loops' clock (EmitterVoice::loop_frames).
  f64 frames_per_tick_ = 800.0;
  AudioSystemStats stats_;
};

}  // namespace engine::audio

#include <core/log/log.h>
#include <domain/ecs/components.h>
#include <foundation/tunables/tunables.h>
#include <systems/audio_system/audio_system.h>

#include <cmath>
#include <schemas/audio_ecs.h>

namespace engine::audio {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_audio_system, "audio.system");

// ---- tunables (ADR-0011) ------------------------------------------------------------------------
//
// How far beyond its max_distance an emitter keeps its voice once it has one. A tenth: an emitter
// walking along the boundary crosses it once instead of every tick, and the voice it keeps a
// little longer is silent anyway, because every tapering distance model is at zero there.
// `tunables::Float` takes f64: a float literal here is a widening conversion and -Wdouble-promotion
// is an error on both Linux compilers.
tunables::Float lod_hysteresis{"audio.lod.hysteresis", 0.1, 0.0, 4.0,
                               "Demote an emitter past max_distance widened by this fraction"};

// ---- what a change implies ----------------------------------------------------------------------
//
// The component's fields fall into three groups by the command a change to them sends:
//
//   restart   clip, cue, and `playing` turning true   stop the old voice, play a new one
//   params    gain, pitch, pan, bus, looping          one SetParams
//   source    position, orientation, the distance     one SetSource
//             model and its distances, the cone,
//             spread, mapping, two_d
//
// `priority` is none of them: it matters only when a voice is chosen, so it is read at the next
// play and sends nothing now. `playing` turning false is a stop.

bool params_changed(const AudioEmitter& a, const AudioEmitter& b) noexcept {
  return a.gain != b.gain || a.pitch != b.pitch || a.pan != b.pan || a.bus != b.bus ||
         a.looping != b.looping;
}

bool source_changed(const AudioEmitter& a, const AudioEmitter& b) noexcept {
  return a.position != b.position || a.orientation != b.orientation ||
         a.min_distance != b.min_distance || a.max_distance != b.max_distance ||
         a.distance_model != b.distance_model || a.directivity != b.directivity ||
         a.cone_inner_degrees != b.cone_inner_degrees ||
         a.cone_outer_degrees != b.cone_outer_degrees || a.cone_outer_gain != b.cone_outer_gain ||
         a.spread != b.spread || a.mapping != b.mapping || a.two_d != b.two_d;
}

SourceSpatial source_of(const AudioEmitter& e) noexcept {
  SourceSpatial s;
  s.position = e.position;
  s.orientation = e.orientation;
  s.min_distance = e.min_distance;
  s.max_distance = e.max_distance;
  s.distance_model = e.distance_model;
  s.directivity = e.directivity;
  const Cone cone = make_cone(e.cone_inner_degrees, e.cone_outer_degrees);
  s.cone_inner_cos = cone.inner_cos;
  s.cone_outer_cos = cone.outer_cos;
  s.cone_outer_gain = e.cone_outer_gain;
  s.spread = e.spread;
  s.mapping = e.mapping;
  s.flags = e.two_d ? k_source_2d : u8{0};
  return s;
}

VoiceParams params_of(const AudioEmitter& e) noexcept {
  VoiceParams p;
  p.gain = e.gain;
  p.pitch = e.pitch;
  p.pan = e.pan;
  p.bus = e.bus;
  p.loop = e.looping;
  return p;
}

bool same_listener(const Listener& a, const Listener& b) noexcept {
  return a.position == b.position && a.forward == b.forward && a.up == b.up;
}

}  // namespace

u32 loop_position(const EmitterVoice& state, u64 tick, f64 frames_per_tick, f32 pitch,
                  u32 clip_frames) noexcept {
  if (clip_frames == 0) return 0;
  const f64 elapsed = tick >= state.loop_tick ? static_cast<f64>(tick - state.loop_tick) : 0.0;
  const f64 played = state.loop_frames + elapsed * frames_per_tick * static_cast<f64>(pitch);
  if (!(played >= 0.0) || !std::isfinite(played)) return 0;
  const u32 frame = static_cast<u32>(std::fmod(played, static_cast<f64>(clip_frames)));
  return frame < clip_frames ? frame : 0u;
}

u8 lod_tier(f32 distance_squared, f32 max_distance, u8 current, f32 hysteresis) noexcept {
  const f32 inner = max_distance > 0.0f ? max_distance : 0.0f;
  const f32 outer = inner * (1.0f + (hysteresis > 0.0f ? hysteresis : 0.0f));
  if (distance_squared < inner * inner) return k_tier_voiced;
  if (distance_squared > outer * outer) return k_tier_virtual;
  return current;
}

AudioSystem::AudioSystem(Mixer& mixer) : mixer_(&mixer) {
  owned_.resize_exact(mixer.voice_count());
  claimed_.resize_exact(mixer.voice_count(), u8{0});
}

AudioSystem::~AudioSystem() {
  for (const VoiceHandle voice : owned_) {
    if (!voice.is_null()) mixer_->stop(voice);
  }
}

void AudioSystem::install(ecs::SimWorld& sim) {
  flecs::world& world = sim.world();
  world_ = &world;
  frames_per_tick_ = static_cast<f64>(k_sample_rate) / static_cast<f64>(sim.clock().hz());

  // Seam 1: the components come from the schema IDL. The emitter's transient state is the one
  // private component, and flecs' `With` rule gives every AudioEmitter one on the tick it appears,
  // so the system's query needs no optional term and no deferred add of its own.
  register_audio_components(world);
  const flecs::entity voice_state =
      ecs::register_private_component<EmitterVoice>(world, "engine::audio::EmitterVoice");
  world.component<AudioEmitter>().add(flecs::With, voice_state);

  listeners_ = world.query<const AudioListener>();

  sim::ComponentMask writes;
  const u32 voice_index = ecs::component_index(world, voice_state.id());
  if (voice_index != ecs::k_invalid_component_index) writes.set(voice_index);

  desc_ = sim::SystemDesc{};
  desc_.name = "audio.emitters";
  desc_.phase = k_phase_emitters;
  desc_.reads = ecs::mask_of<AudioEmitter, AudioListener>(world);
  desc_.writes = writes;
  desc_.writes_resources = sim::resource_mask(k_resource_mixer);
  desc_.tiers = 0x0Fu;  // every tier: the LOD policy is inside, per emitter (lod_tier)
  desc_.determinism = sim::Determinism::Derived;
  desc_.context = this;
  ecs::register_system(sim, desc_, [this](flecs::world& w, flecs::entity phase) {
    // Not multi-threaded: the mixer's controlling half is single-producer.
    return w.system<const AudioEmitter, EmitterVoice>("audio.emitters")
        .kind(phase)
        .run([this](flecs::iter& it) { tick(it); });
  });

  ENGINE_LOG_INFO(log_audio_system, "capability installed",
                  log::field("voices", mixer_->voice_count()),
                  log::field("layout", layout_name(mixer_->layout())));
}

Listener AudioSystem::current_listener(u32& count) {
  // The lowest entity id when there are several: a stable choice that does not depend on the order
  // tables happen to be iterated in.
  Listener chosen;
  flecs::entity_t best = 0;
  count = 0;
  listeners_.each([&](flecs::entity entity, const AudioListener& l) {
    ++count;
    if (best == 0 || entity.id() < best) {
      best = entity.id();
      chosen.position = l.position;
      chosen.forward = l.forward;
      chosen.up = l.up;
    }
  });
  return chosen;
}

void AudioSystem::release(VoiceHandle voice) noexcept {
  if (voice.is_null() || voice.slot >= owned_.size()) return;
  if (owned_[voice.slot] == voice) owned_[voice.slot] = VoiceHandle{};
}

void AudioSystem::tick(flecs::iter& it) {
  AudioSystemStats stats;
  // The controlling thread's half of the mixer, once per tick: learn which voices have ended.
  mixer_->update();
  for (u8& c : claimed_)
    c = 0;
  refused_priority_ = -1;

  const Listener listener = current_listener(stats.listeners);
  if (!listener_sent_ || !same_listener(listener, sent_listener_)) {
    if (mixer_->set_listener(listener)) {
      sent_listener_ = listener;
      listener_sent_ = true;
      ++stats.listener_updates;
    }
  }
  const f32 hysteresis = static_cast<f32>(lod_hysteresis.get());
  const u64 now = it.world().get<SimTick>().value;

  while (it.next()) {
    auto emitters = it.field<const AudioEmitter>(0);
    auto states = it.field<EmitterVoice>(1);
    for (auto row : it) {
      ++stats.emitters;
      step(emitters[row], states[row], listener.position, hysteresis, now, stats);
    }
  }

  // Voices this system started whose emitter did not claim them this tick: the entity was
  // destroyed or lost its AudioEmitter. Stop them, so a destroyed loop does not play for ever.
  const u32 slots = static_cast<u32>(owned_.size());
  for (u32 s = 0; s < slots; ++s) {
    const VoiceHandle voice = owned_[s];
    if (voice.is_null() || claimed_[s] != 0) continue;
    if (mixer_->stop(voice)) ++stats.swept;
    owned_[s] = VoiceHandle{};
  }
  stats_ = stats;
}

void AudioSystem::step(const AudioEmitter& e, EmitterVoice& state, const Vec3& ear, f32 hysteresis,
                       u64 tick, AudioSystemStats& stats) {
  // 1. The voice this state names: still alive, and claimed by one emitter only.
  if (!state.voice.is_null()) {
    const u32 slot = state.voice.slot;
    if (!mixer_->is_live(state.voice)) {
      // It ended: a one-shot played out, or the voice was stolen by a more important sound. A loop
      // asks for a voice again below; a one-shot is finished until it is retriggered.
      release(state.voice);
      state.voice = VoiceHandle{};
      if (!state.sent.looping) state.finished = true;
    } else if (slot >= owned_.size() || owned_[slot] != state.voice || claimed_[slot] != 0) {
      // Alive, but another emitter claimed it first this tick: this state was copied onto a
      // second entity (a clone). The copy starts over; the voice stays with its owner.
      ++stats.duplicates;
      state = EmitterVoice{};
    } else {
      claimed_[slot] = 1;
    }
  }

  // 2. What the change implies.
  const AudioEmitter& sent = state.sent;
  const bool retrigger = state.initialized &&
                         (e.clip != sent.clip || e.cue != sent.cue || (e.playing && !sent.playing));
  if (retrigger) state.finished = false;

  // The loop's clock. It starts when the emitter first wants to loop — new, retriggered, switched
  // to looping, or turned back on — and runs on while it plays, whether or not it holds a voice; a
  // pitch change settles what was played at the old pitch and runs on at the new one. Only then
  // is anything computed: a loop that holds its voice for an hour costs nothing here.
  if (e.playing && e.looping) {
    if (!state.initialized || retrigger || !sent.looping || !sent.playing) {
      state.loop_frames = 0.0;
      state.loop_tick = tick;
    } else if (e.pitch != sent.pitch) {
      const u64 ticks = tick >= state.loop_tick ? tick - state.loop_tick : 0u;
      state.loop_frames +=
          static_cast<f64>(ticks) * frames_per_tick_ * static_cast<f64>(sent.pitch);
      state.loop_tick = tick;
    }
  }

  if (!state.voice.is_null() && (retrigger || !e.playing)) {
    mixer_->stop(state.voice);
    release(state.voice);
    claimed_[state.voice.slot] = 0;
    state.voice = VoiceHandle{};
    ++stats.stops;
    refused_priority_ = -1;  // a stopping voice is anybody's to steal
  }

  // 3. The LOD policy: an emitter past its max_distance holds no voice.
  const bool always_voiced =
      e.two_d || mixer_->bus_is_2d(e.bus) || e.distance_model == DistanceModel::None;
  u8 tier = k_tier_voiced;
  if (!always_voiced) {
    const Vec3 offset = e.position - ear;
    tier = lod_tier(dot(offset, offset), e.max_distance, state.tier, hysteresis);
  }
  if (tier != state.tier) {
    if (tier == k_tier_virtual) {
      ++stats.virtualized;
      if (!state.voice.is_null()) {
        mixer_->stop(state.voice);
        release(state.voice);
        claimed_[state.voice.slot] = 0;
        refused_priority_ = -1;
        // A one-shot does not resume: it was at the edge of hearing when it went, and a sound that
        // restarts from its beginning when the listener walks back is a worse lie than silence. A
        // loop does, where its clock says it would be by then.
        if (!e.looping) state.finished = true;
        state.voice = VoiceHandle{};
      }
    } else {
      ++stats.devirtualized;
    }
    state.tier = tier;
  }
  // A one-shot triggered out of reach is an event nobody heard: it is over, and it does not start
  // later because the listener walked up to where it happened. A loop waits to be in reach.
  if (state.tier == k_tier_virtual && e.playing && !e.looping) state.finished = true;

  // 4. Start, or update what is sounding.
  if (e.playing && !state.finished && state.tier == k_tier_voiced && state.voice.is_null()) {
    const ClipHandle clip = mixer_->clips().find(e.clip);
    if (mixer_->clips().state(clip) != ClipState::Ready) {
      ++stats.waiting;
    } else if (static_cast<i32>(e.priority) <= refused_priority_) {
      // The pool already turned away a sound at least this important this tick, and nothing has
      // stopped since: every voice still outranks it. Asking again would scan the pool to be told
      // the same thing — with a full pool and hundreds of emitters in reach, that is most of the
      // tick (docs/subsystems/audio_system.md, "Performance notes").
      ++stats.refused;
    } else {
      PlayParams play;
      play.clip = clip;
      play.gain = e.gain;
      play.pitch = e.pitch;
      play.pan = e.pan;
      play.bus = e.bus;
      play.priority = e.priority;
      play.loop = e.looping;
      play.source = source_of(e);
      // A loop starts where its clock says it would be: its beginning on the tick it started,
      // somewhere else if it comes back within reach or the pool took a while to let it in. A
      // one-shot starts at its beginning, and only ever on the tick it is triggered in reach.
      if (e.looping) {
        play.start_frame = loop_position(state, tick, frames_per_tick_, e.pitch,
                                         mixer_->clips().view(clip).frames);
      }
      const u64 refused_before = mixer_->control_stats().refused_pool;
      const VoiceHandle voice = mixer_->play(play);
      if (voice.is_null()) {
        ++stats.refused;
        if (mixer_->control_stats().refused_pool != refused_before &&
            e.priority > refused_priority_)
          refused_priority_ = e.priority;
      } else {
        // The mixer may have stolen a voice another emitter owned; that emitter finds out at its
        // next step, from `is_live`.
        owned_[voice.slot] = voice;
        claimed_[voice.slot] = 1;
        state.voice = voice;
        ++stats.plays;
        if (play.start_frame != 0) ++stats.resumed;
      }
    }
  } else if (!state.voice.is_null() && state.initialized) {
    if (params_changed(e, sent)) {
      if (mixer_->set_params(state.voice, params_of(e))) ++stats.params;
    }
    if (source_changed(e, sent)) {
      if (mixer_->set_source(state.voice, source_of(e))) ++stats.sources;
    }
  }

  state.sent = e;
  state.initialized = true;
}

}  // namespace engine::audio

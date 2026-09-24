#include "audio_log.h"

#include <core/base/assert.h>
#include <core/base/macros.h>
#include <core/hash/hash.h>
#include <domain/audio/audio.h>
#include <domain/audio/mixer.h>

#include <bit>
#include <cmath>
#include <cstring>

namespace engine::audio {

namespace {

// The two sides' voice states. The controlling thread's `VoiceSlot::state` uses free/live/stopping
// with the same numbers, but the two are never compared with each other: each side reads only its
// own.
constexpr u8 k_free = 0;
constexpr u8 k_playing = 1;
constexpr u8 k_stopping = 2;

// A voice that has not been decoded for a block yet starts at its full level instead of ramping up
// from silence: ramping would smear the attack of every percussive sound across a whole block. The
// bit lives in the audio thread's flags only and never crosses the queue.
constexpr u8 k_voice_fresh = 1u << 7;

// Pitch is a playback rate over six octaves either way. The floor keeps the 32.32 step far from
// zero, where the run length below would divide by it.
constexpr f32 k_min_pitch = 1.0f / 64.0f;
constexpr f32 k_max_pitch = 64.0f;

constexpr f32 k_two_to_minus_32 = 2.3283064365386962890625e-10f;  // exact: a power of two

// master <- {music (2D), sfx, voice, ambient}. Music is the one non-diegetic bus by default: a
// score has no position, and a radio in the room that does is played on sfx.
constexpr BusDesc k_default_buses[] = {
    {"master", k_no_parent, 1.0f, false, ChannelMapping::FrontPair},
    {"music", k_bus_master, 1.0f, true, ChannelMapping::FrontPair},
    {"sfx", k_bus_master, 1.0f, false, ChannelMapping::FrontPair},
    {"voice", k_bus_master, 1.0f, false, ChannelMapping::FrontPair},
    {"ambient", k_bus_master, 1.0f, false, ChannelMapping::FrontPair},
};

u32 voices_for(const MixerConfig& config) noexcept {
  return config.voices != 0 ? config.voices : tunable_voices();
}

u32 commands_for(const MixerConfig& config) noexcept {
  return config.command_capacity != 0 ? config.command_capacity : tunable_command_queue();
}

// 32.32 fixed point. `pitch * 2^32` is exact in double for every f32 pitch (a 24-bit mantissa
// times a power of two), so the conversion drops nothing and is the same on every compiler.
u64 pitch_step(f32 pitch) noexcept {
  return static_cast<u64>(static_cast<f64>(pitch) * 4294967296.0);
}

bool finite(f32 v) noexcept { return std::isfinite(v); }

bool valid_voice(f32 gain, f32 pitch, f32 pan) noexcept {
  return finite(gain) && gain >= 0.0f && finite(pitch) && pitch >= k_min_pitch &&
         pitch <= k_max_pitch && finite(pan);
}

// A spatial block the audio thread can trust: finite, in range, orientation of unit length. False
// only for what cannot be repaired — a position or a distance that is not a number.
bool sanitize(const SourceSpatial& in, SourceSpatial& out) noexcept {
  if (!finite(in.position.x) || !finite(in.position.y) || !finite(in.position.z)) return false;
  if (!finite(in.min_distance) || !finite(in.max_distance) || in.min_distance < 0.0f ||
      in.max_distance < 0.0f) {
    return false;
  }
  out = in;
  const Quat q = in.orientation;
  const f32 length_squared = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  if (finite(length_squared) && length_squared > 1.0e-12f) {
    const f32 inv = 1.0f / std::sqrt(length_squared);
    out.orientation = Quat{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
  } else {
    out.orientation = Quat{};
  }
  out.cone_inner_cos = finite(in.cone_inner_cos) ? clamp(in.cone_inner_cos, -1.0f, 1.0f) : -1.0f;
  out.cone_outer_cos = finite(in.cone_outer_cos) ? clamp(in.cone_outer_cos, -1.0f, 1.0f) : -1.0f;
  if (out.cone_outer_cos > out.cone_inner_cos) out.cone_outer_cos = out.cone_inner_cos;
  out.cone_outer_gain =
      finite(in.cone_outer_gain) && in.cone_outer_gain >= 0.0f ? in.cone_outer_gain : 1.0f;
  out.spread = finite(in.spread) ? clamp(in.spread, 0.0f, 1.0f) : 0.0f;
  if (static_cast<u8>(in.distance_model) > static_cast<u8>(DistanceModel::None))
    out.distance_model = DistanceModel::InverseTapered;
  if (static_cast<u8>(in.directivity) > static_cast<u8>(Directivity::Cone))
    out.directivity = Directivity::Omni;
  if (static_cast<u8>(in.mapping) > static_cast<u8>(ChannelMapping::Direct))
    out.mapping = ChannelMapping::Inherit;
  out.flags = static_cast<u8>(in.flags & k_source_2d);
  return true;
}

void store3(f32 (&out)[3], const Vec3& v) noexcept {
  out[0] = v.x;
  out[1] = v.y;
  out[2] = v.z;
}

Vec3 load3(const f32 (&in)[3]) noexcept { return Vec3{in[0], in[1], in[2]}; }

SourcePayload pack(const SourceSpatial& s) noexcept {
  SourcePayload p{};
  store3(p.position, s.position);
  p.orientation[0] = s.orientation.x;
  p.orientation[1] = s.orientation.y;
  p.orientation[2] = s.orientation.z;
  p.orientation[3] = s.orientation.w;
  p.min_distance = s.min_distance;
  p.max_distance = s.max_distance;
  p.cone_inner_cos = s.cone_inner_cos;
  p.cone_outer_cos = s.cone_outer_cos;
  p.cone_outer_gain = s.cone_outer_gain;
  p.spread = s.spread;
  p.distance_model = static_cast<u8>(s.distance_model);
  p.directivity = static_cast<u8>(s.directivity);
  p.mapping = static_cast<u8>(s.mapping);
  p.flags = s.flags;
  return p;
}

SourceSpatial unpack(const SourcePayload& p) noexcept {
  SourceSpatial s;
  s.position = load3(p.position);
  s.orientation = Quat{p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]};
  s.min_distance = p.min_distance;
  s.max_distance = p.max_distance;
  s.cone_inner_cos = p.cone_inner_cos;
  s.cone_outer_cos = p.cone_outer_cos;
  s.cone_outer_gain = p.cone_outer_gain;
  s.spread = p.spread;
  s.distance_model = static_cast<DistanceModel>(p.distance_model);
  s.directivity = static_cast<Directivity>(p.directivity);
  s.mapping = static_cast<ChannelMapping>(p.mapping);
  s.flags = p.flags;
  return s;
}

// ---- the voice loop -----------------------------------------------------------------------------
//
// One voice's signal for one block, into the scratch buffer, before any gain: `count` frames from
// frame `first`, while the playhead stays in [0, frames - 1) — so both interpolation taps are
// inside the clip, and the loop has no bounds test, no end test and no branch on the data. The
// channel count is a template parameter (plan 11 §11.4): it changes the loop's shape, and it is
// chosen once per voice per block.
template <u32 Channels>
ENGINE_FORCE_INLINE u64 read_run(const f32* ENGINE_RESTRICT samples, u64 position, u64 step,
                                 u32 first, u32 count, f32* ENGINE_RESTRICT signal) noexcept {
  const u32 last = first + count;
  for (u32 i = first; i < last; ++i) {
    const u32 index = static_cast<u32>(position >> 32);
    const f32 frac = static_cast<f32>(static_cast<u32>(position)) * k_two_to_minus_32;
    if constexpr (Channels == 1) {
      const f32 x0 = samples[index];
      const f32 x1 = samples[index + 1u];
      signal[i] = x0 + (x1 - x0) * frac;
    } else {
      const f32* frame = samples + 2u * index;
      signal[2u * i] = frame[0] + (frame[2] - frame[0]) * frac;
      signal[2u * i + 1u] = frame[1] + (frame[3] - frame[1]) * frac;
    }
    position += step;
  }
  return position;
}

// The one output frame whose left tap is the clip's last frame: its right tap is the first frame
// again for a loop, and silence for a one-shot. Rare — a frame or two per pass through the clip —
// which is why it is out of the loop above rather than a select inside it.
template <u32 Channels>
void read_tail(const f32* samples, u32 frames, u64 position, bool loop, u32 i,
               f32* signal) noexcept {
  const f32 frac = static_cast<f32>(static_cast<u32>(position)) * k_two_to_minus_32;
  const u32 at = frames - 1u;
  if constexpr (Channels == 1) {
    const f32 x0 = samples[at];
    const f32 x1 = loop ? samples[0] : 0.0f;
    signal[i] = x0 + (x1 - x0) * frac;
  } else {
    const f32* frame = samples + 2u * at;
    const f32 x1l = loop ? samples[0] : 0.0f;
    const f32 x1r = loop ? samples[1] : 0.0f;
    signal[2u * i] = frame[0] + (x1l - frame[0]) * frac;
    signal[2u * i + 1u] = frame[1] + (x1r - frame[1]) * frac;
  }
}

// One voice for one block. Returns true when a one-shot ran off the end of its clip in this block;
// the frames after the end are zero.
template <u32 Channels>
bool read_voice(VoiceState& v, f32* signal, u32 frames) noexcept {
  const bool loop = (v.flags & k_voice_loop) != 0;
  const u64 step = v.step;
  const u64 last = static_cast<u64>(v.frames - 1u) << 32;  // the last spot with both taps inside
  const u64 end = static_cast<u64>(v.frames) << 32;
  u64 position = v.position;
  bool ended = false;
  u32 i = 0;
  while (i < frames) {
    if (position < last) {
      // Frames until the playhead reaches `last`: ceil((last - position) / step).
      const u64 steps = (last - position + step - 1u) / step;
      const u32 left = frames - i;
      const u32 run = steps < left ? static_cast<u32>(steps) : left;
      position = read_run<Channels>(v.samples, position, step, i, run, signal);
      i += run;
    } else if (position < end) {
      read_tail<Channels>(v.samples, v.frames, position, loop, i, signal);
      position += step;
      ++i;
    } else if (loop) {
      position %= end;
    } else {
      ended = true;
      std::memset(signal + static_cast<usize>(i) * Channels, 0,
                  static_cast<usize>(frames - i) * Channels * sizeof(f32));
      break;
    }
  }
  v.position = position;
  return ended;
}

}  // namespace

std::span<const BusDesc> default_buses() noexcept { return k_default_buses; }

u64 hash_mix(ChannelLayout layout, std::span<const f32> samples) noexcept {
  return hash_combine(static_cast<u64>(layout),
                      hash_bytes(samples.data(), samples.size() * sizeof(f32)));
}

// ---- construction -------------------------------------------------------------------------------

Mixer::Mixer(const ClipStore& clips, const MixerConfig& config)
    : clips_(&clips),
      layout_(&layout_info(config.layout)),
      decoder_(config.decoder != nullptr ? config.decoder : &k_stereo_panner),
      commands_(commands_for(config)),
      // Twice the pool: `play()` drains this ring before it starts a voice, so between two drains
      // at most one voice starts, each voice ends at most once, and the ring can never hold more
      // than the pool plus one. The margin is so that "never" does not depend on an off-by-one.
      events_(2u * voices_for(config)) {
  if (layout_->channels == 0) {
    ENGINE_LOG_WARN(log_audio, "mixer layout unknown, declaring stereo");
    layout_ = &layout_info(ChannelLayout::Stereo);
  }
  if (!decoder_->supports(layout_->layout)) {
    ENGINE_LOG_WARN(log_audio, "decoder does not support the layout, using the stereo panner",
                    log::field("decoder", decoder_->name), log::field("layout", layout_->name));
    decoder_ = &k_stereo_panner;
  }

  const u32 voices = voices_for(config);
  slots_.resize_exact(voices);
  voices_.resize_exact(voices);
  sources_.resize_exact(voices);
  decoder_state_.resize_exact(voices * decoder_->state_floats, 0.0f);
  scratch_.resize_exact(k_max_block_frames * 2u, 0.0f);

  std::span<const BusDesc> buses = config.buses.empty() ? default_buses() : config.buses;
  bool well_formed = buses.size() <= 255u && buses[0].parent == k_no_parent;
  for (usize b = 0; well_formed && b < buses.size(); ++b) {
    well_formed = finite(buses[b].gain) && buses[b].gain >= 0.0f &&
                  static_cast<u8>(buses[b].mapping) <= static_cast<u8>(ChannelMapping::Direct) &&
                  (b == 0 || buses[b].parent < b);
  }
  if (!well_formed) {
    ENGINE_LOG_WARN(log_audio, "malformed bus tree, using the default",
                    log::field("buses", static_cast<u64>(buses.size())));
    buses = default_buses();
  }
  const u32 bus_count = static_cast<u32>(buses.size());
  bus_parent_.resize_exact(bus_count);
  bus_gain_.resize_exact(bus_count);
  bus_effective_.resize_exact(bus_count);
  bus_gain_control_.resize_exact(bus_count);
  bus_two_d_.resize_exact(bus_count);
  bus_mapping_.resize_exact(bus_count);
  for (u32 b = 0; b < bus_count; ++b) {
    bus_parent_[b] = b == 0 ? k_no_parent : buses[b].parent;
    bus_gain_[b] = buses[b].gain;
    bus_gain_control_[b] = buses[b].gain;
    bus_two_d_[b] = buses[b].two_d ? 1u : 0u;
    bus_mapping_[b] =
        buses[b].mapping == ChannelMapping::Inherit ? ChannelMapping::FrontPair : buses[b].mapping;
  }
  publish(audio_stats_);
}

Mixer::~Mixer() = default;

// ---- the controlling thread ---------------------------------------------------------------------

bool Mixer::push(const Command& command) noexcept {
  if (commands_.try_push(command)) return true;
  ++control_.queue_full;
  return false;
}

u32 Mixer::choose_slot(u8 priority, bool loop, bool& stole) const noexcept {
  stole = false;
  const u32 count = static_cast<u32>(slots_.size());
  for (u32 s = 0; s < count; ++s) {
    if (slots_[s].state == k_free) return s;
  }
  // No free voice: the one that matters least among those the new sound may take. A stopping voice
  // sorts first (it is fading out anyway), then lower priority, then older. An equal priority is
  // takeable only by a one-shot: the newest of a burst is the one to hear, but two loops of equal
  // priority taking the pool from each other would trade voices every tick for as long as there
  // are more of them than voices.
  u32 best = ClipHandle::k_invalid;
  bool best_stopping = false;
  u8 best_priority = 0;
  u64 best_sequence = 0;
  for (u32 s = 0; s < count; ++s) {
    const VoiceSlot& slot = slots_[s];
    const bool stopping = slot.state == k_stopping;
    const bool takeable =
        stopping || slot.priority < priority || (slot.priority == priority && !loop);
    if (!takeable) continue;
    bool better = false;
    if (best == ClipHandle::k_invalid) {
      better = true;
    } else if (stopping != best_stopping) {
      better = stopping;
    } else if (slot.priority != best_priority) {
      better = slot.priority < best_priority;
    } else {
      better = slot.sequence < best_sequence;
    }
    if (better) {
      best = s;
      best_stopping = stopping;
      best_priority = slot.priority;
      best_sequence = slot.sequence;
    }
  }
  stole = best != ClipHandle::k_invalid;
  return best;
}

VoiceHandle Mixer::play(const PlayParams& params) noexcept {
  // Drain first: it frees whatever has ended, and it is what bounds the event ring (constructor).
  update();

  const ClipView clip = clips_->view(params.clip);
  if (clip.samples == nullptr || clip.frames == 0) {
    ++control_.refused_clip;
    return VoiceHandle{};
  }
  SourceSpatial source;
  if (!valid_voice(params.gain, params.pitch, params.pan) || params.bus >= bus_count() ||
      !sanitize(params.source, source)) {
    ++control_.refused_params;
    return VoiceHandle{};
  }
  bool stole = false;
  const u32 index = choose_slot(params.priority, params.loop, stole);
  if (index == ClipHandle::k_invalid) {
    ++control_.refused_pool;
    return VoiceHandle{};
  }
  VoiceSlot& slot = slots_[index];
  u32 generation = slot.generation + 1u;
  if (generation == 0) generation = 1;  // 0 is the null handle

  Command c;
  c.kind = CommandKind::Play;
  c.channels = clip.channels;
  c.bus = params.bus;
  c.flags = params.loop ? k_voice_loop : u8{0};
  c.slot = index;
  c.generation = generation;
  c.start_frame = params.start_frame;
  PlayPayload& p = c.payload.play;
  p.samples = clip.samples;
  p.frames = clip.frames;
  p.gain = params.gain;
  p.pitch = params.pitch;
  p.pan = clamp(params.pan, -1.0f, 1.0f);
  p.source = pack(source);
  if (!push(c)) return VoiceHandle{};

  if (slot.state == k_free) ++live_;
  slot.generation = generation;
  slot.state = k_playing;
  slot.priority = params.priority;
  slot.sequence = ++sequence_;
  slot.stop_pending = 0;
  ++control_.plays;
  if (stole) ++control_.steals;
  return VoiceHandle{index, generation};
}

bool Mixer::is_live(VoiceHandle voice) const noexcept {
  if (voice.is_null() || voice.slot >= slots_.size()) return false;
  const VoiceSlot& slot = slots_[voice.slot];
  return slot.generation == voice.generation && slot.state != k_free;
}

bool Mixer::stop(VoiceHandle voice) noexcept {
  if (!is_live(voice)) return false;
  VoiceSlot& slot = slots_[voice.slot];
  if (slot.state == k_stopping) return true;
  slot.state = k_stopping;
  ++control_.stops;
  Command c;
  c.kind = CommandKind::Stop;
  c.slot = voice.slot;
  c.generation = voice.generation;
  if (!push(c)) {
    // A lost stop is a loop that plays for ever, so it is kept and re-sent by update().
    slot.stop_pending = 1;
    pending_stops_ = true;
  }
  return true;
}

bool Mixer::set_params(VoiceHandle voice, const VoiceParams& params) noexcept {
  if (!is_live(voice)) return false;
  if (!valid_voice(params.gain, params.pitch, params.pan) || params.bus >= bus_count()) {
    ++control_.refused_params;
    return false;
  }
  Command c;
  c.kind = CommandKind::SetParams;
  c.bus = params.bus;
  c.flags = params.loop ? k_voice_loop : u8{0};
  c.slot = voice.slot;
  c.generation = voice.generation;
  c.payload.params = ParamsPayload{params.gain, params.pitch, clamp(params.pan, -1.0f, 1.0f)};
  if (!push(c)) return false;
  ++control_.params;
  return true;
}

bool Mixer::set_source(VoiceHandle voice, const SourceSpatial& source) noexcept {
  if (!is_live(voice)) return false;
  SourceSpatial clean;
  if (!sanitize(source, clean)) {
    ++control_.refused_params;
    return false;
  }
  Command c;
  c.kind = CommandKind::SetSource;
  c.slot = voice.slot;
  c.generation = voice.generation;
  c.payload.source = pack(clean);
  if (!push(c)) return false;
  ++control_.sources;
  return true;
}

bool Mixer::set_listener(const Listener& listener) noexcept {
  const ListenerBasis basis = make_listener_basis(listener);
  Command c;
  c.kind = CommandKind::SetListener;
  ListenerPayload& p = c.payload.listener;
  store3(p.position, basis.position);
  store3(p.right, basis.right);
  store3(p.up, basis.up);
  store3(p.forward, basis.forward);
  if (!push(c)) return false;
  ++control_.listeners;
  return true;
}

bool Mixer::set_bus_gain(u8 bus, f32 gain) noexcept {
  if (bus >= bus_count() || !finite(gain) || gain < 0.0f) {
    ++control_.refused_params;
    return false;
  }
  Command c;
  c.kind = CommandKind::SetBusGain;
  c.bus = bus;
  c.payload.bus = BusPayload{gain};
  if (!push(c)) return false;
  bus_gain_control_[bus] = gain;
  ++control_.bus_gains;
  return true;
}

f32 Mixer::bus_gain(u8 bus) const noexcept {
  return bus < bus_count() ? bus_gain_control_[bus] : 0.0f;
}

u32 Mixer::update() noexcept {
  VoiceEvent event;
  u32 drained = 0;
  while (events_.try_pop(event)) {
    ++drained;
    if (event.slot >= slots_.size()) continue;
    VoiceSlot& slot = slots_[event.slot];
    // An event for a generation the slot has moved past is a voice that was stolen after it
    // ended: the slot is somebody else's now.
    if (slot.generation != event.generation || slot.state == k_free) continue;
    slot.state = k_free;
    slot.stop_pending = 0;
    --live_;
  }
  control_.events += drained;

  if (pending_stops_) {
    pending_stops_ = false;
    const u32 count = static_cast<u32>(slots_.size());
    for (u32 s = 0; s < count; ++s) {
      VoiceSlot& slot = slots_[s];
      if (slot.stop_pending == 0) continue;
      Command c;
      c.kind = CommandKind::Stop;
      c.slot = s;
      c.generation = slot.generation;
      if (push(c)) {
        slot.stop_pending = 0;
      } else {
        pending_stops_ = true;
      }
    }
  }
  return drained;
}

// ---- any thread ---------------------------------------------------------------------------------
//
// The stats snapshot is a seqlock over words that are themselves atomics, so a reader racing the
// writer reads a torn snapshot at worst — which the sequence check then throws away — and never a
// data race in the language's sense. The writer is the audio thread and never waits; a reader
// retries.

namespace {

constexpr u32 k_stats_words = 9;

void pack_stats(const MixerStats& s, u64 (&w)[k_stats_words]) noexcept {
  w[0] = s.blocks;
  w[1] = s.frames;
  w[2] = s.commands;
  w[3] = s.stale_commands;
  w[4] = s.clipped_samples;
  w[5] = s.events_dropped;
  w[6] = static_cast<u64>(s.voices_playing) | (static_cast<u64>(s.voices_peak) << 32);
  w[7] = std::bit_cast<u32>(s.peak);
  w[8] = 0;
}

MixerStats unpack_stats(const u64 (&w)[k_stats_words]) noexcept {
  MixerStats s;
  s.blocks = w[0];
  s.frames = w[1];
  s.commands = w[2];
  s.stale_commands = w[3];
  s.clipped_samples = w[4];
  s.events_dropped = w[5];
  s.voices_playing = static_cast<u32>(w[6]);
  s.voices_peak = static_cast<u32>(w[6] >> 32);
  s.peak = std::bit_cast<f32>(static_cast<u32>(w[7]));
  return s;
}

}  // namespace

void Mixer::publish(const MixerStats& stats) noexcept {
  u64 words[k_stats_words];
  pack_stats(stats, words);
  const u64 sequence = stats_sequence_.load(std::memory_order_relaxed);
  stats_sequence_.store(sequence + 1u, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (u32 i = 0; i < k_stats_words; ++i)
    stats_words_[i].store(words[i], std::memory_order_relaxed);
  stats_sequence_.store(sequence + 2u, std::memory_order_release);
}

MixerStats Mixer::stats() const noexcept {
  u64 words[k_stats_words];
  for (;;) {
    const u64 before = stats_sequence_.load(std::memory_order_acquire);
    if ((before & 1u) != 0) continue;
    for (u32 i = 0; i < k_stats_words; ++i)
      words[i] = stats_words_[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (stats_sequence_.load(std::memory_order_relaxed) == before) break;
  }
  return unpack_stats(words);
}

// ---- the audio thread ---------------------------------------------------------------------------
//
// Nothing below allocates, locks or logs.

void Mixer::apply(const Command& c) noexcept {
  if (c.kind != CommandKind::SetListener && c.kind != CommandKind::SetBusGain &&
      c.slot >= voices_.size()) {
    ++audio_stats_.stale_commands;
    return;
  }
  switch (c.kind) {
    case CommandKind::Play: {
      const PlayPayload& p = c.payload.play;
      VoiceState& v = voices_[c.slot];
      v.samples = p.samples;
      v.frames = p.frames;
      v.channels = c.channels;
      v.bus = c.bus;
      v.flags = static_cast<u8>(c.flags | k_voice_fresh);
      v.generation = c.generation;
      v.gain = p.gain;
      v.pan = p.pan;
      v.step = pitch_step(p.pitch);
      v.position = static_cast<u64>(c.start_frame % p.frames) << 32;
      v.state = k_playing;
      sources_[c.slot] = unpack(p.source);
      return;
    }
    case CommandKind::Stop: {
      VoiceState& v = voices_[c.slot];
      if (v.state == k_playing && v.generation == c.generation) {
        v.state = k_stopping;
      } else {
        ++audio_stats_.stale_commands;
      }
      return;
    }
    case CommandKind::SetParams: {
      VoiceState& v = voices_[c.slot];
      if (v.state == k_free || v.generation != c.generation) {
        ++audio_stats_.stale_commands;
        return;
      }
      v.gain = c.payload.params.gain;
      v.pan = c.payload.params.pan;
      v.step = pitch_step(c.payload.params.pitch);
      v.bus = c.bus;
      v.flags = static_cast<u8>((v.flags & k_voice_fresh) | c.flags);
      return;
    }
    case CommandKind::SetSource: {
      const VoiceState& v = voices_[c.slot];
      if (v.state == k_free || v.generation != c.generation) {
        ++audio_stats_.stale_commands;
        return;
      }
      sources_[c.slot] = unpack(c.payload.source);
      return;
    }
    case CommandKind::SetListener: {
      const ListenerPayload& p = c.payload.listener;
      listener_.position = load3(p.position);
      listener_.right = load3(p.right);
      listener_.up = load3(p.up);
      listener_.forward = load3(p.forward);
      return;
    }
    case CommandKind::SetBusGain: {
      if (c.bus < bus_gain_.size()) {
        bus_gain_[c.bus] = c.payload.bus.gain;
      } else {
        ++audio_stats_.stale_commands;
      }
      return;
    }
  }
}

void Mixer::render(f32* out, u32 frames) noexcept {
  // Every command queued before this call, and no more than a ring's worth, so a controlling
  // thread writing flat out cannot keep the block from ever starting.
  u32 budget = commands_.capacity();
  Command command;
  u64 applied = 0;
  while (budget > 0 && commands_.try_pop(command)) {
    apply(command);
    ++applied;
    --budget;
  }
  audio_stats_.commands += applied;
  if (frames == 0 || out == nullptr) {
    publish(audio_stats_);
    return;
  }

  // Bus gains compose root first: a bus's parent is always an earlier bus, so one forward pass
  // leaves every bus holding the product of the gains on its path to the master.
  const u32 buses = static_cast<u32>(bus_gain_.size());
  for (u32 b = 0; b < buses; ++b) {
    const u8 parent = bus_parent_[b];
    bus_effective_[b] =
        parent == k_no_parent ? bus_gain_[b] : bus_gain_[b] * bus_effective_[parent];
  }

  const u32 channels = layout_->channels;
  for (u32 done = 0; done < frames;) {
    const u32 block = frames - done < k_max_block_frames ? frames - done : k_max_block_frames;
    mix_block(out + static_cast<usize>(done) * channels, block);
    done += block;
  }
  publish(audio_stats_);
}

void Mixer::mix_block(f32* out, u32 frames) noexcept {
  const u32 samples = frames * layout_->channels;
  std::memset(out, 0, static_cast<usize>(samples) * sizeof(f32));

  // Slot order is the mix order, and float addition is not associative: this order is part of what
  // the determinism test pins.
  u32 playing = 0;
  const u32 voice_count = static_cast<u32>(voices_.size());
  const u32 state_floats = decoder_->state_floats;
  f32* signal = scratch_.data();
  for (u32 s = 0; s < voice_count; ++s) {
    VoiceState& v = voices_[s];
    if (v.state == k_free) continue;

    // 1. The voice loop: the clip at the voice's pitch, before any gain.
    const bool ended =
        v.channels == 1 ? read_voice<1>(v, signal, frames) : read_voice<2>(v, signal, frames);

    // 2. The level and the source model: the voice's gain, its bus path, and — for a 3D source —
    //    what distance and directivity leave of it (spatial.h, the first seam).
    const SourceSpatial& source = sources_[s];
    const bool two_d = (source.flags & k_source_2d) != 0 || bus_two_d_[v.bus] != 0;
    DecodeInput in;
    in.signal = signal;
    in.frames = frames;
    in.channels = v.channels;
    in.gain = v.state == k_stopping ? 0.0f : v.gain * bus_effective_[v.bus];
    in.fresh = (v.flags & k_voice_fresh) != 0;
    in.two_d = two_d;
    in.mapping = source.mapping != ChannelMapping::Inherit ? source.mapping : bus_mapping_[v.bus];
    in.pan = v.pan;
    in.source = &source;
    in.listener = &listener_;
    if (!two_d) {
      in.spatial = spatialize(source, listener_);
      in.gain *= in.spatial.attenuation;
    }

    // 3. The decode stage: the one place that knows the layout (decoder.h, the second seam).
    decoder_->decode(in, *layout_, decoder_state_.data() + static_cast<usize>(s) * state_floats,
                     out);

    v.flags = static_cast<u8>(v.flags & ~k_voice_fresh);
    if (ended || v.state == k_stopping) {
      v.state = k_free;
      if (!events_.try_push(VoiceEvent{s, v.generation})) ++audio_stats_.events_dropped;
    } else {
      ++playing;
    }
  }

  // The master's policy: a hard clip at full scale, counted. No limiter in v0 — a limiter hides a
  // mix that is too hot, where a counter says so (docs/subsystems/audio.md, "The master").
  f32 peak = 0.0f;
  u64 clipped = 0;
  for (u32 i = 0; i < samples; ++i) {
    const f32 x = out[i];
    const f32 magnitude = std::fabs(x);
    peak = magnitude > peak ? magnitude : peak;
    clipped += magnitude > 1.0f ? 1u : 0u;
    out[i] = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
  }

  ++audio_stats_.blocks;
  audio_stats_.frames += frames;
  audio_stats_.clipped_samples += clipped;
  audio_stats_.voices_playing = playing;
  if (playing > audio_stats_.voices_peak) audio_stats_.voices_peak = playing;
  audio_stats_.peak = peak;
}

}  // namespace engine::audio

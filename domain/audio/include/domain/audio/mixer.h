#pragma once

// The mixer: a fixed pool of voices — each one a source, an object with a spatial block — a bus
// tree whose master declares its speaker layout, one isolated decode stage from sources to that
// layout, a hard clip, and the two queues that are the only way in and out
// (docs/subsystems/audio.md).
//
// ---- the pipeline, per block --------------------------------------------------------------------
//
//   commands -> for each voice, in slot order:
//                 read the clip at the voice's pitch into a scratch signal   (the voice loop)
//                 gain x bus path x source model's attenuation               (spatial.h)
//                 decode stage: signal + SpatialParams -> layout channels    (decoder.h)
//            -> master: look-ahead limiter (off by default, limiter.h) -> hard clip -> out
//
// Nothing before the decode stage knows the layout. That is the owner's direction for audio
// (docs/plan/05-simulation.md §5.11, "Direction note, 2026-09-24"): an object-based sound stage,
// decoded last, so that a VBAP, ambisonic, platform-object or binaural decoder replaces one table
// and touches nothing here.
//
// ---- two threads, two halves --------------------------------------------------------------------
//
// **The controlling thread** — the simulation tick, or whatever single thread drives audio — owns
// the top half of this class: it allocates voices from the pool, turns every request into a
// `Command` on a single-producer single-consumer ring, and drains `VoiceEvent`s from a second ring
// to learn which voices have ended. Exactly one thread at a time may call these; calls from
// successive threads are fine when something (a tick boundary, a join) orders them.
//
// **The audio thread** — the device's callback, or the caller's own thread under the null backend
// — owns `render()`. It applies every queued command, mixes, decodes, clips, posts ended-voice
// events and publishes a stats snapshot through a seqlock. It **allocates nothing, takes no lock,
// and logs nothing**: every buffer it touches was sized in the constructor, and both rings are
// wait-free. The tests hold it to that with a counting allocator and a counting log sink.
//
// ---- the voice policy --------------------------------------------------------------------------
//
// A play takes a free voice if there is one. With none free it **steals** the voice that matters
// least among those it may take: a voice already stopping first (it is fading out anyway),
// otherwise the lowest priority, the oldest among equals. A one-shot may take a voice of lower or
// equal priority, so the newest of a burst of equal-priority sounds is always heard — the usual
// answer for footsteps and gunfire; a loop only a lower one, because loops of equal priority taking
// the pool from each other would trade voices on every retry for as long as they outnumber it. If
// nothing is takeable the play is **refused**: `play()` returns a null handle and
// `ControlStats::refused_pool` counts it.
//
// ---- determinism -------------------------------------------------------------------------------
//
// For a given clip set, **a given layout and decoder**, a given ramp time, and a given sequence of
// commands and the frames they are applied at, the output is the same bytes on every run, compiler
// and C library: voices are mixed in slot order, every operation is IEEE-754 arithmetic with
// contraction off (ADR-0035), the playhead is 32.32 fixed point, and nothing on the audio thread
// calls a transcendental function. The test pins a hash of a scripted mix per layout. How the
// frames between two commands are divided into `render()` calls does not matter — a ramp is a time,
// not a block, so a device with a 256-frame period and one with a 4096-frame period mix the same
// commands at the same frames to the same bytes, and the test renders the session in uneven calls
// to hold it to that. Which frame a command lands on on a live device depends on when the device
// asked for its block, and that is not claimed.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/audio/clip_store.h>
#include <domain/audio/commands.h>
#include <domain/audio/decoder.h>
#include <domain/audio/format.h>
#include <domain/audio/limiter.h>
#include <domain/audio/spatial.h>
#include <domain/audio/spsc_queue.h>
#include <domain/audio/stream.h>

#include <atomic>
#include <memory>
#include <schemas/audio.h>
#include <span>

namespace engine::audio {

struct Stream;  // src/stream.h: a ring and the fill that keeps it ahead of its voice

// ---- buses --------------------------------------------------------------------------------------

inline constexpr u8 k_no_parent = 0xFFu;

// The default tree: 0 master, and four category buses under it.
inline constexpr u8 k_bus_master = 0;
inline constexpr u8 k_bus_music = 1;
inline constexpr u8 k_bus_sfx = 2;
inline constexpr u8 k_bus_voice = 3;
inline constexpr u8 k_bus_ambient = 4;

struct BusDesc {
  const char* name = "";
  // An earlier bus. Only bus 0, the master, has none; every other bus must name one below it,
  // which is what lets a single forward pass compose the gains.
  u8 parent = k_no_parent;
  f32 gain = 1.0f;
  // Every source on this bus is 2D — non-diegetic — whatever its own flag says. The default tree's
  // music bus is; a game that wants diegetic music (a radio in the room) plays it on sfx.
  bool two_d = false;
  // How a 2D source on this bus maps onto the layout when the source itself says `Inherit`.
  ChannelMapping mapping = ChannelMapping::FrontPair;
};

// master <- {music (2D), sfx, voice, ambient}.
std::span<const BusDesc> default_buses() noexcept;

// Whether the master's limiter runs: as the tunable says, or forced either way.
enum class LimiterMode : u8 { Default = 0, Off, On };

// ---- configuration and requests -----------------------------------------------------------------

struct MixerConfig {
  // Voices in the pool. 0 reads the `audio.voices` tunable (64).
  u32 voices = 0;
  // Commands the ring holds. 0 reads `audio.command_queue` (1024).
  u32 command_capacity = 0;
  // Frames a gain or pan change takes to reach its target, and a stop to fade out. 0 reads
  // `audio.ramp_ms` (10 ms: 480 frames). A time, never the block size.
  u32 ramp_frames = 0;
  // The master's look-ahead limiter (limiter.h). `Default` reads `audio.limiter` (off).
  LimiterMode limiter = LimiterMode::Default;
  // Its ceiling, linear in (0, 1]. 0 reads `audio.limiter.threshold_db` (-1 dBFS) through the C
  // library's pow; a caller that pins limited output passes the linear value instead.
  f32 limiter_ceiling = 0.0f;
  // Its release, in frames. 0 reads `audio.limiter.release_ms` (100 ms).
  u32 limiter_release_frames = 0;
  // What the master feeds. Chosen by the caller from the device's own layout or from the
  // `audio.layout` setting (device.h, `resolve_layout`); `Unknown` is refused in favour of stereo,
  // with a log line.
  ChannelLayout layout = ChannelLayout::Stereo;
  // The decode stage. One that does not support `layout` is replaced by the stereo panner, with a
  // log line.
  const Decoder* decoder = &k_stereo_panner;
  // The tree. Empty is `default_buses()`. A malformed tree (a parent that is not an earlier bus, a
  // gain that is not finite, more than 255 buses) is replaced by the default one, with a log line.
  std::span<const BusDesc> buses;
  // Streams: how many streamed voices can play at once (0 reads `audio.streams`, 16), the frames
  // each one's ring holds (0 reads `audio.stream_ring_ms`, 500 ms) and how far ahead of its voice a
  // stream is kept (0 reads `audio.stream_fill_ms`, 250 ms; clamped below the ring).
  u32 streams = 0;
  u32 stream_ring_frames = 0;
  u32 stream_fill_frames = 0;
};

struct PlayParams {
  ClipHandle clip;
  f32 gain = 1.0f;   // linear, >= 0
  f32 pitch = 1.0f;  // playback rate: 2 is an octave up; [1/64, 64]
  f32 pan = 0.0f;    // 2D sources: -1 left .. +1 right
  u8 bus = k_bus_sfx;
  u8 priority = 128;  // higher outranks lower when the pool is full
  bool loop = false;
  u32 start_frame = 0;  // wraps into the clip
  SourceSpatial source;
};

struct VoiceParams {
  f32 gain = 1.0f;
  f32 pitch = 1.0f;
  f32 pan = 0.0f;
  u8 bus = k_bus_sfx;
  bool loop = false;
};

// What the controlling thread did, counted where it did it. Plain integers: one thread.
struct ControlStats {
  u64 plays = 0;           // voices started, including steals
  u64 steals = 0;          // plays that took a live voice
  u64 refused_pool = 0;    // every voice outranked the new sound
  u64 refused_clip = 0;    // the clip was not Ready
  u64 refused_params = 0;  // a non-finite or out-of-range parameter, or an unknown bus
  u64 queue_full = 0;      // a command the ring had no room for (a stop is retried, see update())
  u64 refused_stream = 0;  // a streamed clip with every stream playing
  u64 stops = 0;
  u64 params = 0;
  u64 sources = 0;
  u64 listeners = 0;
  u64 bus_gains = 0;
  u64 events = 0;            // ended-voice events drained
  u64 stream_plays = 0;      // of `plays`, the voices that play a stream
  u64 stream_fills = 0;      // fills scheduled (or run inline, with no job system)
  u64 stream_underruns = 0;  // streamed voices that ran dry at least once, each logged once
};

// What the audio thread did, as of the last block it finished. Read with `Mixer::stats()` from any
// thread; the snapshot is consistent (a seqlock), never torn.
struct MixerStats {
  u64 blocks = 0;           // blocks mixed: a render() call is ceil(frames / k_max_block_frames)
  u64 frames = 0;           // frames mixed
  u64 commands = 0;         // commands applied
  u64 stale_commands = 0;   // commands naming a voice that had already ended
  u64 clipped_samples = 0;  // output samples the master's hard clip changed
  u64 events_dropped = 0;   // ended-voice events the ring refused; sized so it never happens
  u64 limited_frames = 0;   // frames the limiter turned down; 0 while it is off
  // Frames of silence streamed voices played because their stream had not decoded that far: the
  // underruns. A stream that has not decoded its first frame yet is starting, not starving, and
  // is not counted.
  u64 underrun_frames = 0;
  u32 voices_playing = 0;   // after the last block
  u32 voices_peak = 0;      // most voices playing at the end of any block
  f32 peak = 0.0f;          // largest |sample| of the last block, before the limiter and the clip
  f32 limiter_gain = 1.0f;  // the limiter's lowest gain in the last block: 1 when it did nothing
};

// ---- the two records of one voice ---------------------------------------------------------------
//
// Public so the size table can pin them (ADR-0019); nothing outside the mixer reads or writes one.

// The audio thread's voice: the part of a source the voice loop reads every block, all of it, so
// it is one record rather than columns — 48 bytes, and the default pool of 64 is 3 KB, resident in
// L1 for the block. The spatial block (56 bytes) is a parallel array read once per block by the
// source model, and the decode stage's gains are the decoder's own state.
struct VoiceState {
  const f32* samples = nullptr;  // the clip, interleaved, `channels` per frame
  u64 position = 0;              // 32.32 fixed-point frames into the clip
  u64 step = 0;                  // 32.32 frames advanced per output frame: the pitch
  u32 frames = 0;                // clip length
  u32 generation = 0;
  f32 gain = 1.0f;
  f32 pan = 0.0f;
  u32 fade = 0;  // a stopping voice: frames of its fade still to mix before the slot is freed
  u8 state = 0;  // free, playing, stopping
  u8 channels = 0;
  u8 bus = 0;
  u8 flags = 0;  // k_voice_* plus the mixer's private "fresh" bit
};

// The controlling thread's record of the same slot: enough to allocate, steal and address it.
struct VoiceSlot {
  u64 sequence = 0;  // play order, for "oldest"
  u32 generation = 0;
  u8 state = 0;  // free, live, stopping
  u8 priority = 0;
  u8 stop_pending = 0;  // a stop the ring refused; update() re-sends it
  u8 reserved = 0;
};

class Mixer {
 public:
  // The largest block the voice loop fills at once. `render()` takes any frame count and works
  // through a longer one in blocks of this size; commands are applied once, at the start of the
  // call. A block boundary is invisible in the output: a ramp that crosses one carries on where it
  // was. 1024 frames is 21 ms.
  static constexpr u32 k_max_block_frames = 1024;

  // `clips` must outlive the mixer; a voice holds a pointer into one of its clips.
  explicit Mixer(const ClipStore& clips, const MixerConfig& config = {});
  ~Mixer();
  ENGINE_NON_COPYABLE(Mixer);

  // ---- the controlling thread -----------------------------------------------------------------

  // Starts a voice. Null when refused: the clip is not Ready, a parameter is out of range, every
  // voice outranks it, or the ring is full. `ControlStats` says which.
  VoiceHandle play(const PlayParams& params) noexcept;
  // Fades the voice out over the ramp time (`ramp_frames()`) and frees it. False for a voice that
  // is not live.
  bool stop(VoiceHandle voice) noexcept;
  bool set_params(VoiceHandle voice, const VoiceParams& params) noexcept;
  // Replaces the voice's whole spatial block.
  bool set_source(VoiceHandle voice, const SourceSpatial& source) noexcept;
  bool set_listener(const Listener& listener) noexcept;
  bool set_bus_gain(u8 bus, f32 gain) noexcept;

  // Drains the ended-voice events, freeing their slots, and re-sends any stop the ring refused
  // earlier; tops up every stream that has less than the fill-ahead decoded (a fill job each, on
  // the clip store's Efficiency pool), frees the streams whose voices have ended, and logs a
  // streamed voice's first underrun. Call once per tick. Returns the events drained.
  u32 update() noexcept;

  // Blocks until no stream fill is in flight. Tests and load screens; never the tick.
  void wait_streams() noexcept;
  u32 stream_count() const noexcept { return stream_count_; }
  // Streams a voice holds, or that are waiting for their last fill to land before they are free.
  u32 streams_in_use() const noexcept { return streams_in_use_; }
  u32 stream_ring_frames() const noexcept { return ring_frames_; }
  u32 stream_fill_frames() const noexcept { return fill_frames_; }

  // The controlling thread's view: started and not yet reported ended. A voice may have gone
  // silent on the audio thread already; it is live here until `update()` reads its event.
  bool is_live(VoiceHandle voice) const noexcept;
  u32 live_voices() const noexcept { return live_; }
  u32 voice_count() const noexcept { return static_cast<u32>(slots_.size()); }
  u32 bus_count() const noexcept { return static_cast<u32>(bus_parent_.size()); }
  f32 bus_gain(u8 bus) const noexcept;
  // Whether every source on `bus` is 2D. Fixed at construction, so any thread may ask.
  bool bus_is_2d(u8 bus) const noexcept { return bus < bus_count() && bus_two_d_[bus] != 0; }
  u32 command_capacity() const noexcept { return commands_.capacity(); }
  const ControlStats& control_stats() const noexcept { return control_; }
  const ClipStore& clips() const noexcept { return *clips_; }

  // ---- any thread ------------------------------------------------------------------------------

  // The declared layout and the decoder feeding it, fixed at construction.
  ChannelLayout layout() const noexcept { return layout_->layout; }
  u32 channels() const noexcept { return layout_->channels; }
  const Decoder& decoder() const noexcept { return *decoder_; }
  // Frames a change takes to arrive and a stop to fade, fixed at construction.
  u32 ramp_frames() const noexcept { return ramp_frames_; }
  // The master's limiter as configured (whether it runs, its ceiling and release), fixed at
  // construction. Read its counters through `stats()`, not here: the audio thread writes them.
  bool limiter_enabled() const noexcept { return limiter_.enabled(); }
  f32 limiter_ceiling() const noexcept { return limiter_.ceiling(); }
  // Frames between a command's effect entering the mix and leaving the master: the limiter's
  // look-ahead when it runs, 0 otherwise.
  u32 latency_frames() const noexcept { return limiter_.latency_frames(); }
  MixerStats stats() const noexcept;

  // ---- the audio thread ------------------------------------------------------------------------

  // Applies the queued commands and mixes `frames` frames of the declared layout, interleaved,
  // into `out` — `frames * channels()` floats — overwriting it. `frames == 0` applies commands and
  // publishes stats without mixing, and `out` may then be null. Parameter changes take effect at
  // the start of the call and ramp linearly to their targets over `ramp_frames()`, however the
  // frames are divided into calls; a new voice starts at its full level.
  void render(f32* out, u32 frames) noexcept;

 private:
  bool push(const Command& command) noexcept;
  u32 choose_slot(u8 priority, bool loop, bool& stole) const noexcept;
  u32 free_stream() const noexcept;
  void schedule_fill(Stream& stream) noexcept;
  void service_streams() noexcept;
  void apply(const Command& command) noexcept;
  void mix_block(f32* out, u32 frames) noexcept;
  void publish(const MixerStats& stats) noexcept;

  const ClipStore* clips_;
  const LayoutInfo* layout_;
  const Decoder* decoder_;
  u32 ramp_frames_;

  // ---- controlling thread
  Vector<VoiceSlot> slots_;
  Vector<u32> slot_stream_;  // the stream the slot's current voice reads, or k_no_stream
  Vector<f32> bus_gain_control_;
  ControlStats control_;
  u64 sequence_ = 0;
  u64 pushed_ = 0;  // commands pushed: a command's sequence number is this, after its push
  u32 live_ = 0;
  u32 streams_in_use_ = 0;
  bool pending_stops_ = false;

  // ---- streams: records fixed at construction, rings allocated at first use (stream.h)
  std::unique_ptr<Stream[]> streams_;
  u32 stream_count_ = 0;
  u32 ring_frames_ = 0;
  u32 fill_frames_ = 0;

  // ---- shared: the two rings, the count of commands applied, and the stats seqlock
  SpscQueue<Command> commands_;
  SpscQueue<VoiceEvent> events_;
  // Commands the audio thread has applied, stored with release after it applied them: a command
  // whose sequence number is at most this has taken effect, which is when a stolen voice's
  // stream is no longer read.
  alignas(64) std::atomic<u64> applied_{0};
  alignas(64) std::atomic<u64> stats_sequence_{0};
  std::atomic<u64> stats_words_[11];

  // ---- audio thread
  alignas(64) Vector<VoiceState> voices_;
  Vector<StreamRing*> voice_ring_;  // a streamed voice's ring, by slot
  Vector<SourceSpatial> sources_;
  Vector<f32> decoder_state_;  // voices x decoder_->state_floats
  Vector<f32> scratch_;        // one voice's signal for one block: k_max_block_frames x 2
  Vector<u8> bus_parent_;
  Vector<f32> bus_gain_;
  Vector<f32> bus_effective_;
  Vector<u8> bus_two_d_;
  Vector<ChannelMapping> bus_mapping_;
  ListenerBasis listener_;
  Limiter limiter_;
  MixerStats audio_stats_;
};

// A stable 64-bit summary of a mix: its layout and its samples, by their bits. What the
// determinism test pins — the layout is part of it, because the same session decoded to two
// layouts is two different outputs.
u64 hash_mix(ChannelLayout layout, std::span<const f32> samples) noexcept;

}  // namespace engine::audio

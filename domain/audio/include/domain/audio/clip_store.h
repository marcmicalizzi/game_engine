#pragma once

// Decoded clips (docs/subsystems/audio.md, "Clips").
//
// A clip is decoded **ahead of time**, off the audio thread, into a buffer the engine owns: f32,
// interleaved, mono or stereo, at the mix rate. The voice loop then reads samples and nothing
// else — no decoder state, no format conversion, no I/O — which is what keeps the audio thread's
// cost a function of the voice count alone.
//
// Decoding runs on the job system's **Efficiency pool** (plan 11 §11.5 names streaming and
// decompression as that pool's work): it is latency-tolerant, it can take milliseconds for a long
// file, and it must not take a performance worker from the tick. WAV, FLAC and MP3 are decoded by
// miniaudio's built-in decoders; anything that is not at 48 kHz is converted by miniaudio's linear
// resampler with its low-pass filter at the highest order it offers, once, at decode time. Sources
// with more than two channels are downmixed to stereo by miniaudio's channel converter.
//
// **Streaming.** A clip whose decoded size is over `audio.stream_threshold_kb` (five seconds of
// stereo) is not decoded at all at load: the store reads its format, and a voice that plays it
// streams it — a ring of decoded frames a decode job keeps ahead of the voice, from ranged reads of
// the file or from the bytes the store keeps for it (stream.h). Such a clip is Ready with no
// samples and `ClipView::stream` set.
//
// **The budget, and eviction.** Decoded audio is large (a minute of stereo is 23 MB), so the store
// holds a byte budget (`audio.clip_budget_mb`, 256 MB by default). A decode that would take the
// store past it **evicts** clips first, least recently used first — but only clips that nothing
// plays: the mixer tells the store which clips its voices hold (`retain`/`release`, at the tick,
// never in the callback), and a clip a voice holds is never evicted. Only a clip loaded from a
// file is evictable, because the store can go back to the file: evicted, it stays Ready and
// streams from its file, and the next voice started on it asks the store to decode it again
// (`request`). A clip loaded from bytes or samples is the only copy there is and is never evicted.
// A decode that cannot fit even with everything unused evicted is refused: a file's clip then
// streams, anything else is `ClipState::OverBudget`.
//
// **Lifetime.** A resident clip's samples do not move or disappear while a voice holds the clip,
// which is the guarantee a voice's raw pointer rests on. The store must outlive every mixer that
// plays from it.
//
// **Threads.** Every member function is for the controlling thread, the same one that drives the
// mixer. Decode jobs touch only their own clip's staging area and say they are done with a release
// store; the controlling thread takes what they decoded — and evicts, if it must — in `update()`,
// which the mixer's `update()` calls every tick, so a clip changes state only inside a call the
// controlling thread makes.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/hash_map.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/jobs/job_system.h>

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace engine::audio {

struct ClipHandle {
  static constexpr u32 k_invalid = 0xFFFF'FFFFu;
  u32 index = k_invalid;

  constexpr bool is_valid() const noexcept { return index != k_invalid; }
  constexpr bool operator==(const ClipHandle&) const noexcept = default;
};

enum class ClipState : u8 {
  Pending = 0,  // queued or decoding
  Ready,        // samples may be read
  Failed,       // the bytes were not a format the decoders know, or were damaged
  OverBudget,   // decoded, then refused: it would have exceeded the store's byte budget
  Invalid,      // the handle names no clip
};

const char* clip_state_name(ClipState state) noexcept;

// What a voice needs of a clip. Empty unless the clip is Ready; then either `samples` names the
// whole clip, resident, or `stream` says that a voice plays it through a stream and `samples` is
// null.
struct ClipView {
  const f32* samples = nullptr;
  u32 frames = 0;
  u8 channels = 0;
  bool stream = false;
};

struct ClipInfo {
  ClipState state = ClipState::Invalid;
  u8 channels = 0;         // as stored: 1 or 2
  u8 source_channels = 0;  // as decoded from the file
  u32 frames = 0;          // at the mix rate
  u32 source_rate = 0;     // as decoded from the file
  u64 bytes = 0;           // resident, when Ready: the samples, or a streamed clip's kept bytes
  bool streamed = false;   // voices stream it: over the threshold, evicted, or refused residency
  bool resident = false;   // its samples are in memory
  bool evictable = false;  // loaded from a file, so the store may evict it and stream it
  u32 refs = 0;            // voices that hold it (Mixer::play retains, the voice's end releases)
};

// Where a clip came from, which is where a stream reads it.
enum class ClipSourceKind : u8 {
  None = 0,  // samples handed to `add_pcm`: nothing to stream from
  Memory,    // bytes handed to `load`, which the store keeps for a streamed clip
  File,      // a file named to `load_file`, read by range
  ClipFile,  // a `.clip` the content build wrote: frames already in the mix format
};

// What a stream needs to read a clip (stream.h). The views point into the store's own record and
// live as long as the store.
struct ClipSource {
  ClipSourceKind kind = ClipSourceKind::None;
  std::span<const u8> memory;  // Memory
  std::string_view path;       // File, ClipFile
  u64 samples_offset = 0;      // ClipFile: the byte offset of the first frame
  u32 frames = 0;              // at the mix rate, as the store knows it
  u8 channels = 0;
  Id128 key;
};

// A clip decoded outside any store: what the store's jobs produce, and what a test or a tool
// can call directly.
struct DecodedClip {
  Vector<f32> samples;  // interleaved, `channels` per frame, at the mix rate
  u32 frames = 0;
  u8 channels = 0;
  u8 source_channels = 0;
  u32 source_rate = 0;
};

enum class DecodeStatus : u8 {
  Ok = 0,
  UnknownFormat,  // no decoder recognized the bytes
  Corrupt,        // a decoder recognized them and then failed partway
  Empty,          // it decoded to zero frames
  TooLong,        // more frames than a u32 counts at the mix rate (a day and more at 48 kHz)
};

const char* decode_status_name(DecodeStatus status) noexcept;

// Bytes of WAV, FLAC or MP3 in, 48 kHz f32 out. Allocates; never call it on the audio thread.
DecodeStatus decode_clip(std::span<const u8> encoded, DecodedClip& out);

// The content key for a clip loaded from bytes: two independent 64-bit hashes of the bytes, so a
// store asked for the same file twice decodes it once. A clip that has an asset id uses that
// instead; the store does not care which a key is.
Id128 clip_key(std::span<const u8> encoded) noexcept;

struct ClipStoreConfig {
  // Bytes of decoded audio the store may hold. 0 reads the `audio.clip_budget_mb` tunable.
  u64 budget_bytes = 0;
  // A clip decoded larger than this streams. 0 reads `audio.stream_threshold_kb`.
  u64 stream_threshold_bytes = 0;
  // Where decoding runs — and where the mixer's stream fills run. Null decodes inline on the
  // calling thread, which is what tests and tools want; with a job system, decodes go to its
  // Efficiency pool.
  jobs::JobSystem* jobs = nullptr;
  // The derived-data root (`<repo>/ddc`, or wherever the game keeps its built data). With one, a
  // clip loaded from an encoded source is first looked for as `<ddc>/clips/<key>.clip`, the
  // content build's decode of the same bytes (clip_file.h): found, it is a copy, or a stream by
  // range, and nothing is decoded. Empty looks for nothing. The store reads the cache and never
  // writes it: `engine-content build` fills it.
  std::string ddc_root;
};

class ClipStore {
 public:
  explicit ClipStore(const ClipStoreConfig& config = {});
  // Waits for any decode still in flight: a job holds a pointer to its clip.
  ~ClipStore();
  ENGINE_NON_COPYABLE(ClipStore);

  // Starts decoding `encoded` under `key` and returns the clip's handle at once; the clip is
  // Pending until the decode has finished and the controlling thread has taken it (`update()`,
  // `wait()`; at once with no job system). The bytes are copied. A key the store already holds
  // returns the existing handle and decodes nothing. A clip over the stream threshold is not
  // decoded: the store keeps the bytes and voices stream from them.
  ClipHandle load(const Id128& key, std::span<const u8> encoded);

  // The same from a file (a native path), read on the decode job. A clip over the stream threshold
  // is not read whole: voices stream it from the file by range. A clip loaded this way is the one
  // kind the budget may evict, since the file is still there to stream from and decode again.
  // The file may be a `.clip` the content build wrote (told by its magic, not its name): then it
  // is copied, or streamed by range, and never decoded.
  ClipHandle load_file(const Id128& key, std::string_view path);

  // A clip from samples already at the mix rate — generated audio, tests. Ready at once, or
  // OverBudget. `channels` is 1 or 2; `interleaved.size()` must be a multiple of it.
  ClipHandle add_pcm(const Id128& key, std::span<const f32> interleaved, u8 channels);

  ClipHandle find(const Id128& key) const noexcept;
  ClipState state(ClipHandle clip) const noexcept;
  ClipInfo info(ClipHandle clip) const noexcept;
  // The samples, once Ready; an empty view otherwise.
  ClipView view(ClipHandle clip) const noexcept;
  // Where a Ready streamed clip is read from; `kind` None for anything else.
  ClipSource source(ClipHandle clip) const noexcept;

  // ---- what the mixer tells the store (the controlling thread, at the tick) --------------------
  //
  // A voice holds its clip from the `play()` that starts it until the audio thread says it has
  // ended — or, for a stolen voice, until the audio thread has applied the `Play` that stole it.
  // A clip with a holder is never evicted.
  void retain(ClipHandle clip) noexcept;
  void release(ClipHandle clip) noexcept;
  // A voice is being started on a clip that is not resident: if it was evicted (it is under the
  // threshold and has a file), decode it again, so the voices after this one play it resident.
  // This voice streams it meanwhile. A no-op for anything else, and for a clip whose decode could
  // not fit however much were evicted.
  void request(ClipHandle clip);

  // Takes every finished decode: a clip that fits becomes Ready, evicting what it must; one that
  // does not is refused. The mixer's `update()` calls it every tick; `wait()` calls it too.
  // Returns the decodes taken.
  u32 update();
  // Blocks until no decode is in flight, then takes them. Tests and load screens; never the tick.
  void wait();

  u32 count() const noexcept { return static_cast<u32>(clips_.size()); }
  u64 resident_bytes() const noexcept { return resident_; }
  u64 budget_bytes() const noexcept { return budget_; }
  u64 stream_threshold_bytes() const noexcept { return stream_threshold_; }
  // Clips refused for the budget since construction.
  u32 over_budget() const noexcept { return over_budget_; }
  // Clips evicted for the budget since construction.
  u32 evictions() const noexcept { return evictions_; }
  // Evicted clips decoded again because a voice was started on them.
  u32 reloads() const noexcept { return reloads_; }
  // Loads answered by a built `.clip` — named directly, or found in the derived-data cache — with
  // no decode.
  u32 built_loads() const noexcept { return built_loads_; }
  // The job system decodes run on, null when they run inline; a mixer's stream fills run there too.
  jobs::JobSystem* jobs() const noexcept { return jobs_; }

  struct Clip;  // defined in src/clip_store.cpp; public so the decode job can name it

 private:
  ClipHandle add_entry(const Id128& key);
  void start_decode(u32 index);
  static void decode_job(void* clip);
  void take(Clip& clip) noexcept;
  bool make_room(u64 bytes, const Clip* keep) noexcept;
  void evict(Clip& clip) noexcept;
  bool commit(Clip& clip, Vector<f32>& samples) noexcept;
  bool commit_stream(Clip& clip, u64 kept_bytes) noexcept;
  void touch(Clip& clip) noexcept;

  jobs::JobSystem* jobs_ = nullptr;
  u64 budget_ = 0;
  u64 stream_threshold_ = 0;
  std::string ddc_root_;
  Vector<std::unique_ptr<Clip>> clips_;
  HashMap<Id128, u32> by_key_;
  // Clips with a decode job in flight, whose results `update()` takes when they land.
  Vector<u32> in_flight_;
  u64 resident_ = 0;
  // The resident bytes eviction could free now: evictable, resident, held by no voice.
  u64 evictable_bytes_ = 0;
  u64 clock_ = 0;  // the LRU order: bumped by every retain, release and commit
  u32 over_budget_ = 0;
  u32 evictions_ = 0;
  u32 reloads_ = 0;
  u32 built_loads_ = 0;
  // One count per decode job in flight; `wait()` and the destructor wait on it.
  jobs::Counter pending_;
};

}  // namespace engine::audio

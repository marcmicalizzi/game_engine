// The clip store (clip_store.h, docs/subsystems/audio.md "Clips", "Streaming", "Eviction").
#include "audio_log.h"
#include "backend.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/audio/audio.h>
#include <domain/audio/clip_file.h>
#include <domain/audio/clip_store.h>

#include <utility>

namespace engine::audio {

// One clip. Allocated on its own so its address survives the table growing: a decode job holds a
// pointer to it while the controlling thread may be adding more clips.
struct ClipStore::Clip {
  Id128 key;
  ClipStore* store = nullptr;
  ClipSourceKind source = ClipSourceKind::None;
  std::string path;    // File: the file, which an evicted clip streams from and decodes again from
  Vector<u8> encoded;  // Memory: the bytes, until the decode has read them — kept for a stream
  Vector<f32> samples;
  u32 frames = 0;
  u8 channels = 0;
  u8 source_channels = 0;
  u32 source_rate = 0;
  u64 bytes = 0;  // what it holds resident: its samples, or a stream's kept bytes
  // Where a voice streams it from: its file, its bytes, or a built `.clip` (the file named, or its
  // entry in the derived-data cache). Set when the first decode lands and never changed after,
  // since a stream holds a view of the path.
  ClipSourceKind stream_kind = ClipSourceKind::None;
  std::string stream_path;
  u64 stream_offset = 0;
  bool streamed = false;        // voices stream it
  bool resident = false;        // its samples are in memory
  bool over_threshold = false;  // never decoded whole: it always streams
  bool decoding = false;        // a decode job for it is in flight
  u32 refs = 0;                 // voices that hold it
  u64 last_use = 0;             // the store's clock at its last retain, release or commit
  // Only ever written by the controlling thread; atomic because `state()` is the one question a
  // tool on another thread might reasonably ask.
  std::atomic<ClipState> state{ClipState::Pending};

  // ---- what the decode job leaves for the controlling thread: written by the job, then `landed`
  //      with release; read by `take()` after an acquire of `landed`
  struct Staged {
    DecodeStatus status = DecodeStatus::Ok;
    bool stream = false;  // over the threshold: not decoded, stream it
    u64 kept = 0;         // for a stream from bytes, the bytes kept
    bool built = false;   // answered by a built `.clip`, not decoded
    ClipSourceKind stream_kind = ClipSourceKind::None;
    std::string stream_path;
    u64 stream_offset = 0;
    Vector<f32> samples;
    u32 frames = 0;
    u8 channels = 0;
    u8 source_channels = 0;
    u32 source_rate = 0;
  } staged;
  std::atomic<u8> landed{0};
};

const char* clip_state_name(ClipState state) noexcept {
  switch (state) {
    case ClipState::Pending: return "pending";
    case ClipState::Ready: return "ready";
    case ClipState::Failed: return "failed";
    case ClipState::OverBudget: return "over_budget";
    case ClipState::Invalid: return "invalid";
  }
  return "unknown";
}

const char* decode_status_name(DecodeStatus status) noexcept {
  switch (status) {
    case DecodeStatus::Ok: return "ok";
    case DecodeStatus::UnknownFormat: return "unknown_format";
    case DecodeStatus::Corrupt: return "corrupt";
    case DecodeStatus::Empty: return "empty";
    case DecodeStatus::TooLong: return "too_long";
  }
  return "unknown";
}

DecodeStatus decode_clip(std::span<const u8> encoded, DecodedClip& out) {
  return backend::decode(encoded, out);
}

Id128 clip_key(std::span<const u8> encoded) noexcept {
  // Two seeds, two independent 64-bit hashes: 128 bits of key for a store that may hold thousands
  // of clips, where one 64-bit hash would already be enough and two make a collision a non-event.
  return Id128{hash_bytes(encoded.data(), encoded.size(), 0x6175'6469'6f2e'6869ull),
               hash_bytes(encoded.data(), encoded.size(), 0x6175'6469'6f2e'6c6full)};
}

ClipStore::ClipStore(const ClipStoreConfig& config)
    : jobs_(config.jobs),
      budget_(config.budget_bytes != 0 ? config.budget_bytes : tunable_clip_budget_bytes()),
      stream_threshold_(config.stream_threshold_bytes != 0 ? config.stream_threshold_bytes
                                                           : tunable_stream_threshold_bytes()),
      ddc_root_(config.ddc_root) {}

// A job holds a pointer to its clip; none may outlive the store.
ClipStore::~ClipStore() {
  if (pending_.done()) return;
  if (jobs_ != nullptr) {
    jobs_->wait(pending_);
  } else {
    pending_.wait_blocking();
  }
}

void ClipStore::wait() {
  if (!pending_.done()) {
    if (jobs_ != nullptr) {
      jobs_->wait(pending_);
    } else {
      pending_.wait_blocking();
    }
  }
  update();
}

u32 ClipStore::update() {
  // In the order the decodes were started, whatever order they finished in: which clip a commit
  // evicts depends on the commits before it, and that order is the caller's.
  u32 taken = 0;
  u32 kept = 0;
  for (u32 i = 0; i < in_flight_.size(); ++i) {
    Clip& clip = *clips_[in_flight_[i]];
    if (clip.landed.load(std::memory_order_acquire) != 0) {
      take(clip);
      ++taken;
    } else {
      in_flight_[kept++] = in_flight_[i];
    }
  }
  in_flight_.resize(kept);
  return taken;
}

ClipHandle ClipStore::add_entry(const Id128& key) {
  const u32 index = static_cast<u32>(clips_.size());
  auto clip = std::make_unique<Clip>();
  clip->key = key;
  clip->store = this;
  clips_.push_back(std::move(clip));
  by_key_[Id128{key}] = index;
  return ClipHandle{index};
}

void ClipStore::start_decode(u32 index) {
  Clip& clip = *clips_[index];
  clip.decoding = true;
  if (jobs_ != nullptr) {
    in_flight_.push_back(index);
    pending_.add(1);
    jobs_->schedule(jobs::Pool::Efficiency, jobs::Job{&ClipStore::decode_job, &clip, &pending_});
  } else {
    decode_job(&clip);
    take(clip);
  }
}

ClipHandle ClipStore::load(const Id128& key, std::span<const u8> encoded) {
  if (const u32* found = by_key_.find_value(key)) return ClipHandle{*found};
  const ClipHandle handle = add_entry(key);
  Clip& clip = *clips_[handle.index];
  clip.source = ClipSourceKind::Memory;
  clip.encoded.append(encoded);
  start_decode(handle.index);
  return handle;
}

ClipHandle ClipStore::load_file(const Id128& key, std::string_view path) {
  if (const u32* found = by_key_.find_value(key)) return ClipHandle{*found};
  const ClipHandle handle = add_entry(key);
  Clip& clip = *clips_[handle.index];
  clip.source = ClipSourceKind::File;
  clip.path = std::string(path);
  start_decode(handle.index);
  return handle;
}

ClipHandle ClipStore::add_pcm(const Id128& key, std::span<const f32> interleaved, u8 channels) {
  if (const u32* found = by_key_.find_value(key)) return ClipHandle{*found};
  const ClipHandle handle = add_entry(key);
  Clip& clip = *clips_[handle.index];
  if ((channels != 1 && channels != 2) || interleaved.empty() ||
      interleaved.size() % channels != 0 ||
      interleaved.size() >= static_cast<usize>(0xFFFF'0000u)) {
    ENGINE_LOG_WARN(log_audio, "pcm clip refused", log::field("channels", u32{channels}),
                    log::field("samples", static_cast<u64>(interleaved.size())));
    clip.state.store(ClipState::Failed, std::memory_order_release);
    return handle;
  }
  clip.frames = static_cast<u32>(interleaved.size() / channels);
  clip.channels = channels;
  clip.source_channels = channels;
  clip.source_rate = k_sample_rate;
  Vector<f32> samples;
  samples.append(interleaved);
  commit(clip, samples);
  return handle;
}

// ---- the decode job -----------------------------------------------------------------------------

namespace {

// What a clip of this format takes decoded, which is what the stream threshold is compared with.
u64 decoded_bytes(const backend::StreamFormat& format) noexcept {
  return format.frames * format.channels * sizeof(f32);
}

void stage_format(ClipStore::Clip& clip, const backend::StreamFormat& format, u64 kept) {
  clip.staged.stream = true;
  clip.staged.kept = kept;
  clip.staged.frames = static_cast<u32>(format.frames);
  clip.staged.channels = format.channels;
  clip.staged.source_channels = format.source_channels;
  clip.staged.source_rate = format.source_rate;
}

// A built `.clip` at `path` whose front `layout` has been read: over the threshold it is streamed
// from the file by range, under it the whole file is read — its content hash checked — and copied.
// False when the full read fails; the caller decides whether there is anything to fall back on.
bool stage_built(ClipStore::Clip& clip, const std::string& path, const ClipFileLayout& layout,
                 u64 threshold) {
  ClipStore::Clip::Staged& staged = clip.staged;
  const u64 bytes = u64{layout.frames} * layout.channels * sizeof(f32);
  if (bytes > threshold) {
    staged.stream = true;
    staged.stream_kind = ClipSourceKind::ClipFile;
    staged.stream_path = path;
    staged.stream_offset = layout.samples_offset;
  } else {
    ClipFileData data;
    std::string error;
    if (!read_clip_file(path, data, &error)) {
      ENGINE_LOG_WARN(log_audio, "built clip unreadable", log::field("clip", clip.key),
                      log::field("path", path), log::field("error", error));
      return false;
    }
    staged.samples = std::move(data.samples);
  }
  staged.built = true;
  staged.frames = layout.frames;
  staged.channels = static_cast<u8>(layout.channels);
  staged.source_channels =
      static_cast<u8>(layout.source_channels > 255u ? 255u : layout.source_channels);
  staged.source_rate = layout.source_rate;
  return true;
}

// The content build's decode of these bytes, if the derived-data cache has it: the key is over the
// bytes (clip_file.h), and an entry is taken only when it records that same source and key.
bool stage_cached(ClipStore::Clip& clip, std::span<const u8> encoded, const std::string& ddc,
                  u64 threshold) {
  const u64 source_hash = clip_source_hash(encoded);
  const u64 key = clip_cache_key(source_hash);
  const std::string path = clip_cache_path(ddc, key);
  if (!io::exists(path)) return false;
  io::FileHandle file;
  ClipFileLayout layout;
  std::string error;
  if (file.open(path) != io::Status::Ok || !read_clip_file_layout(file, layout, &error) ||
      layout.source_hash != source_hash || layout.build_key != key) {
    ENGINE_LOG_DEBUG(log_audio, "cached clip unusable, decoding", log::field("clip", clip.key),
                     log::field("path", path), log::field("error", error));
    return false;
  }
  return stage_built(clip, path, layout, threshold);
}

}  // namespace

void ClipStore::decode_job(void* data) {
  Clip& clip = *static_cast<Clip*>(data);
  const ClipStore& store = *clip.store;
  Clip::Staged& staged = clip.staged;
  staged = Clip::Staged{};
  // Where a voice streams it from unless a built clip turns up: its file, or its bytes.
  staged.stream_kind = clip.source;
  staged.stream_path = clip.path;
  const auto land = [&clip] { clip.landed.store(1, std::memory_order_release); };

  // The format first, without decoding: a clip over the threshold is never decoded whole — a voice
  // streams it, from the file by range or from the bytes the store keeps (stream.h). The length is
  // the decoder's own answer at the mix rate; a decoder that cannot give one without decoding the
  // whole file answers 0, and that clip is decoded whole. And before that, whether it needs a
  // decoder at all: a `.clip` is the content build's decode already.
  backend::StreamFormat format;
  std::string file_bytes;
  std::span<const u8> encoded(clip.encoded.data(), clip.encoded.size());
  if (clip.source == ClipSourceKind::File) {
    io::FileHandle file;
    if (file.open(clip.path) != io::Status::Ok) {
      ENGINE_LOG_WARN(log_audio, "clip file unreadable", log::field("clip", clip.key),
                      log::field("path", clip.path));
      staged.status = DecodeStatus::UnknownFormat;
      land();
      return;
    }
    u8 magic[4] = {};
    u64 got = 0;
    if (file.read_at(0, magic, sizeof(magic), got) == io::Status::Ok && got == sizeof(magic) &&
        is_clip_file(std::span<const u8>(magic, sizeof(magic)))) {
      // A built clip named directly: nothing to fall back on if it is damaged.
      ClipFileLayout layout;
      std::string error;
      if (!read_clip_file_layout(file, layout, &error) ||
          !stage_built(clip, clip.path, layout, store.stream_threshold_)) {
        ENGINE_LOG_WARN(log_audio, "built clip unreadable", log::field("clip", clip.key),
                        log::field("path", clip.path), log::field("error", error));
        staged.status = DecodeStatus::Corrupt;
      }
      land();
      return;
    }
    if (!store.ddc_root_.empty()) {
      // The cache is keyed by the source's bytes, so the file is read whole to hash it — once,
      // sequentially, here — and on a miss the bytes in hand are decoded (or, over the threshold,
      // streamed from the file by range after all).
      if (io::read_file(clip.path, file_bytes) != io::Status::Ok) {
        ENGINE_LOG_WARN(log_audio, "clip file unreadable", log::field("clip", clip.key),
                        log::field("path", clip.path));
        staged.status = DecodeStatus::UnknownFormat;
        land();
        return;
      }
      encoded =
          std::span<const u8>(reinterpret_cast<const u8*>(file_bytes.data()), file_bytes.size());
      if (stage_cached(clip, encoded, store.ddc_root_, store.stream_threshold_)) {
        land();
        return;
      }
    } else {
      backend::ByteSource source;
      source.file = &file;
      source.size = file.size();
      staged.status = backend::probe(source, format);
      if (staged.status == DecodeStatus::Ok && format.frames != 0 &&
          decoded_bytes(format) > store.stream_threshold_) {
        stage_format(clip, format, 0);
        land();
        return;
      }
      // Under the threshold: the whole file, decoded as bytes.
      if (staged.status == DecodeStatus::Ok &&
          io::read_file(clip.path, file_bytes) != io::Status::Ok) {
        ENGINE_LOG_WARN(log_audio, "clip file unreadable", log::field("clip", clip.key),
                        log::field("path", clip.path));
        staged.status = DecodeStatus::UnknownFormat;
      }
      if (staged.status != DecodeStatus::Ok) {
        ENGINE_LOG_WARN(log_audio, "clip decode failed", log::field("clip", clip.key),
                        log::field("status", decode_status_name(staged.status)));
        land();
        return;
      }
      encoded =
          std::span<const u8>(reinterpret_cast<const u8*>(file_bytes.data()), file_bytes.size());
    }
  } else if (is_clip_file(encoded)) {
    // A built clip handed over as bytes: already the mix format, so a copy — even over the
    // threshold, since the bytes are in memory either way.
    ClipFileData built;
    std::string error;
    if (!read_clip_file_memory(encoded, built, &error)) {
      ENGINE_LOG_WARN(log_audio, "built clip unreadable", log::field("clip", clip.key),
                      log::field("error", error));
      staged.status = DecodeStatus::Corrupt;
    } else {
      staged.built = true;
      staged.frames = built.frames;
      staged.channels = static_cast<u8>(built.channels);
      staged.source_channels =
          static_cast<u8>(built.source_channels > 255u ? 255u : built.source_channels);
      staged.source_rate = built.source_rate;
      staged.samples = std::move(built.samples);
    }
    clip.encoded = Vector<u8>{};
    land();
    return;
  } else if (!store.ddc_root_.empty() &&
             stage_cached(clip, encoded, store.ddc_root_, store.stream_threshold_)) {
    clip.encoded = Vector<u8>{};
    land();
    return;
  }

  // The source's bytes are in hand. Over the threshold, a stream: from the file it was read from,
  // or from the bytes, which the store keeps.
  {
    backend::ByteSource source;
    source.memory = encoded;
    if (backend::probe(source, format) == DecodeStatus::Ok && format.frames != 0 &&
        decoded_bytes(format) > store.stream_threshold_) {
      stage_format(clip, format, clip.source == ClipSourceKind::Memory ? clip.encoded.size() : 0u);
      land();
      return;
    }
    // Anything the probe refused goes on to the decoder, which says why in its own words.
  }

  DecodedClip decoded;
  staged.status = backend::decode(encoded, decoded);
  if (clip.source == ClipSourceKind::Memory) clip.encoded = Vector<u8>{};
  if (staged.status != DecodeStatus::Ok) {
    ENGINE_LOG_WARN(log_audio, "clip decode failed", log::field("clip", clip.key),
                    log::field("status", decode_status_name(staged.status)));
  } else {
    staged.samples = std::move(decoded.samples);
    staged.frames = decoded.frames;
    staged.channels = decoded.channels;
    staged.source_channels = decoded.source_channels;
    staged.source_rate = decoded.source_rate;
  }
  clip.landed.store(1, std::memory_order_release);
}

// ---- the controlling thread's side of a decode --------------------------------------------------

void ClipStore::take(Clip& clip) noexcept {
  clip.landed.store(0, std::memory_order_relaxed);
  clip.decoding = false;
  Clip::Staged& staged = clip.staged;
  const bool first = clip.state.load(std::memory_order_relaxed) == ClipState::Pending;
  if (staged.status != DecodeStatus::Ok) {
    // A first load that failed is Failed. A reload that failed leaves the clip as it was: evicted,
    // streaming from its file.
    if (first) {
      clip.encoded = Vector<u8>{};
      clip.state.store(ClipState::Failed, std::memory_order_release);
    }
    staged = Clip::Staged{};
    return;
  }
  if (first) {
    clip.frames = staged.frames;
    clip.channels = staged.channels;
    clip.source_channels = staged.source_channels;
    clip.source_rate = staged.source_rate;
    // Never changed after this: a stream playing the clip holds a view of the path.
    clip.stream_kind = staged.stream_kind;
    clip.stream_path = std::move(staged.stream_path);
    clip.stream_offset = staged.stream_offset;
  }
  if (staged.built) ++built_loads_;
  if (staged.stream) {
    clip.over_threshold = true;
    commit_stream(clip, staged.kept);
  } else {
    commit(clip, staged.samples);
  }
  staged = Clip::Staged{};
}

void ClipStore::touch(Clip& clip) noexcept { clip.last_use = ++clock_; }

bool ClipStore::make_room(u64 bytes, const Clip* keep) noexcept {
  if (resident_ + bytes <= budget_) return true;
  // Even with every clip nothing plays evicted it would not fit: evict nothing.
  if (resident_ - evictable_bytes_ + bytes > budget_) return false;
  while (resident_ + bytes > budget_) {
    // Least recently used first: the oldest last retain, release or commit. A scan, because this
    // runs only when a decode lands and does not fit, over a store of hundreds of clips.
    Clip* victim = nullptr;
    for (const std::unique_ptr<Clip>& c : clips_) {
      if (c.get() == keep || !c->resident || c->source != ClipSourceKind::File || c->refs != 0 ||
          c->state.load(std::memory_order_relaxed) != ClipState::Ready) {
        continue;
      }
      if (victim == nullptr || c->last_use < victim->last_use) victim = c.get();
    }
    if (victim == nullptr) return false;
    evict(*victim);
  }
  return true;
}

void ClipStore::evict(Clip& clip) noexcept {
  ENGINE_LOG_DEBUG(log_audio, "clip evicted", log::field("clip", clip.key),
                   log::field("bytes", clip.bytes), log::field("last_use", clip.last_use),
                   log::field("resident", resident_), log::field("budget", budget_));
  evictable_bytes_ -= clip.bytes;
  resident_ -= clip.bytes;
  clip.samples = Vector<f32>{};
  clip.bytes = 0;
  clip.resident = false;
  // It stays Ready: a voice started on it streams it from its file, and asks for it back.
  clip.streamed = true;
  ++evictions_;
}

bool ClipStore::commit_stream(Clip& clip, u64 kept_bytes) noexcept {
  // A streamed clip holds no samples; what it keeps resident is the bytes a memory clip streams
  // from, and those are counted like samples — and, being the only copy, are never evicted.
  if (!make_room(kept_bytes, &clip)) {
    ++over_budget_;
    ENGINE_LOG_WARN(log_audio, "clip over the budget", log::field("clip", clip.key),
                    log::field("bytes", kept_bytes), log::field("resident", resident_),
                    log::field("budget", budget_));
    clip.encoded = Vector<u8>{};
    clip.state.store(ClipState::OverBudget, std::memory_order_release);
    return false;
  }
  resident_ += kept_bytes;
  clip.bytes = kept_bytes;
  clip.streamed = true;
  touch(clip);
  clip.state.store(ClipState::Ready, std::memory_order_release);
  return true;
}

bool ClipStore::commit(Clip& clip, Vector<f32>& samples) noexcept {
  // The decoder's buffer grew in steps; the clip keeps exactly its samples, and the budget counts
  // those bytes — so the number `resident_bytes()` reports is the memory the clips occupy.
  const u64 bytes = static_cast<u64>(samples.size()) * sizeof(f32);
  if (!make_room(bytes, &clip)) {
    ++over_budget_;
    const bool from_file = clip.source == ClipSourceKind::File;
    ENGINE_LOG_WARN(log_audio, "clip over the budget", log::field("clip", clip.key),
                    log::field("bytes", bytes), log::field("resident", resident_),
                    log::field("budget", budget_), log::field("streams", from_file));
    if (from_file) {
      // Refused residency, not refused: its file is there to stream from, as an evicted clip's is.
      clip.streamed = true;
      clip.state.store(ClipState::Ready, std::memory_order_release);
    } else if (clip.state.load(std::memory_order_relaxed) == ClipState::Pending) {
      clip.state.store(ClipState::OverBudget, std::memory_order_release);
    }
    return false;
  }
  clip.samples = Vector<f32>{};
  clip.samples.reserve(samples.size());
  clip.samples.append(std::span<const f32>(samples.data(), samples.size()));
  resident_ += bytes;
  clip.bytes = bytes;
  clip.resident = true;
  clip.streamed = false;
  if (clip.source == ClipSourceKind::File && clip.refs == 0) evictable_bytes_ += bytes;
  touch(clip);
  clip.state.store(ClipState::Ready, std::memory_order_release);
  return true;
}

// ---- what the mixer tells the store -------------------------------------------------------------

void ClipStore::retain(ClipHandle handle) noexcept {
  if (state(handle) != ClipState::Ready) return;
  Clip& clip = *clips_[handle.index];
  if (clip.refs == 0 && clip.resident && clip.source == ClipSourceKind::File)
    evictable_bytes_ -= clip.bytes;
  ++clip.refs;
  touch(clip);
}

void ClipStore::release(ClipHandle handle) noexcept {
  if (!handle.is_valid() || handle.index >= clips_.size()) return;
  Clip& clip = *clips_[handle.index];
  if (clip.refs == 0) return;
  --clip.refs;
  if (clip.refs == 0 && clip.resident && clip.source == ClipSourceKind::File)
    evictable_bytes_ += clip.bytes;
  touch(clip);
}

void ClipStore::request(ClipHandle handle) {
  if (state(handle) != ClipState::Ready) return;
  Clip& clip = *clips_[handle.index];
  if (clip.resident || clip.over_threshold || clip.decoding || clip.source != ClipSourceKind::File)
    return;
  // Only if it could fit: a clip refused residency because every resident clip is playing would
  // otherwise be decoded again on every play, to be refused again.
  const u64 bytes = u64{clip.frames} * clip.channels * sizeof(f32);
  if (resident_ - evictable_bytes_ + bytes > budget_) return;
  ++reloads_;
  start_decode(handle.index);
}

// ---- queries ------------------------------------------------------------------------------------

ClipHandle ClipStore::find(const Id128& key) const noexcept {
  if (const u32* found = by_key_.find_value(key)) return ClipHandle{*found};
  return ClipHandle{};
}

ClipState ClipStore::state(ClipHandle clip) const noexcept {
  if (!clip.is_valid() || clip.index >= clips_.size()) return ClipState::Invalid;
  return clips_[clip.index]->state.load(std::memory_order_acquire);
}

ClipInfo ClipStore::info(ClipHandle clip) const noexcept {
  ClipInfo out;
  out.state = state(clip);
  if (out.state != ClipState::Ready) return out;
  const Clip& c = *clips_[clip.index];
  out.channels = c.channels;
  out.source_channels = c.source_channels;
  out.frames = c.frames;
  out.source_rate = c.source_rate;
  out.bytes = c.bytes;
  out.streamed = c.streamed;
  out.resident = c.resident;
  out.evictable = c.source == ClipSourceKind::File;
  out.refs = c.refs;
  return out;
}

ClipView ClipStore::view(ClipHandle clip) const noexcept {
  if (state(clip) != ClipState::Ready) return ClipView{};
  const Clip& c = *clips_[clip.index];
  if (!c.resident) return ClipView{nullptr, c.frames, c.channels, true};
  return ClipView{c.samples.data(), c.frames, c.channels, false};
}

ClipSource ClipStore::source(ClipHandle clip) const noexcept {
  if (state(clip) != ClipState::Ready) return ClipSource{};
  const Clip& c = *clips_[clip.index];
  if (c.resident) return ClipSource{};
  ClipSource out;
  out.kind = c.stream_kind;
  if (c.stream_kind == ClipSourceKind::Memory)
    out.memory = std::span<const u8>(c.encoded.data(), c.encoded.size());
  out.path = c.stream_path;
  out.samples_offset = c.stream_offset;
  out.frames = c.frames;
  out.channels = c.channels;
  out.key = c.key;
  return out;
}

}  // namespace engine::audio

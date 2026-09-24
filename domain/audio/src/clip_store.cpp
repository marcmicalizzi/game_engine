#include "audio_log.h"
#include "backend.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <domain/audio/audio.h>
#include <domain/audio/clip_store.h>

namespace engine::audio {

// One clip. Allocated on its own so its address survives the table growing: a decode job holds a
// pointer to it while the controlling thread may be adding more clips.
struct ClipStore::Clip {
  Id128 key;
  ClipStore* store = nullptr;
  Vector<u8> encoded;  // the bytes, until the decode has read them
  Vector<f32> samples;
  u32 frames = 0;
  u8 channels = 0;
  u8 source_channels = 0;
  u32 source_rate = 0;
  u64 bytes = 0;
  // Written last by whoever finishes the clip, with release; read with acquire before anything
  // above it. That pair is the whole synchronization between a decode job and the mixer.
  std::atomic<ClipState> state{ClipState::Pending};
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
      budget_(config.budget_bytes != 0 ? config.budget_bytes : tunable_clip_budget_bytes()) {}

ClipStore::~ClipStore() { wait(); }

void ClipStore::wait() {
  if (pending_.done()) return;
  if (jobs_ != nullptr) {
    jobs_->wait(pending_);
  } else {
    pending_.wait_blocking();
  }
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

ClipHandle ClipStore::load(const Id128& key, std::span<const u8> encoded) {
  if (const u32* found = by_key_.find_value(key)) return ClipHandle{*found};
  const ClipHandle handle = add_entry(key);
  Clip& clip = *clips_[handle.index];
  clip.encoded.append(encoded);
  if (jobs_ != nullptr) {
    pending_.add(1);
    jobs_->schedule(jobs::Pool::Efficiency, jobs::Job{&ClipStore::decode_job, &clip, &pending_});
  } else {
    decode_job(&clip);
  }
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
  DecodedClip decoded;
  decoded.samples.append(interleaved);
  decoded.frames = static_cast<u32>(interleaved.size() / channels);
  decoded.channels = channels;
  decoded.source_channels = channels;
  decoded.source_rate = k_sample_rate;
  commit(clip, decoded);
  return handle;
}

void ClipStore::decode_job(void* data) {
  Clip& clip = *static_cast<Clip*>(data);
  DecodedClip decoded;
  const DecodeStatus status =
      backend::decode(std::span<const u8>(clip.encoded.data(), clip.encoded.size()), decoded);
  clip.encoded = Vector<u8>{};
  if (status != DecodeStatus::Ok) {
    ENGINE_LOG_WARN(log_audio, "clip decode failed", log::field("clip", clip.key),
                    log::field("status", decode_status_name(status)));
    clip.state.store(ClipState::Failed, std::memory_order_release);
    return;
  }
  clip.store->commit(clip, decoded);
}

bool ClipStore::commit(Clip& clip, DecodedClip& decoded) noexcept {
  // The decoder's buffer grew in steps; the clip keeps exactly its samples, and the budget counts
  // those bytes — so the number `resident_bytes()` reports is the memory the clips occupy.
  const u64 bytes = static_cast<u64>(decoded.samples.size()) * sizeof(f32);
  u64 resident = resident_.load(std::memory_order_relaxed);
  do {
    if (resident + bytes > budget_) {
      over_budget_.fetch_add(1, std::memory_order_relaxed);
      ENGINE_LOG_WARN(log_audio, "clip over the budget", log::field("clip", clip.key),
                      log::field("bytes", bytes), log::field("resident", resident),
                      log::field("budget", budget_));
      clip.state.store(ClipState::OverBudget, std::memory_order_release);
      return false;
    }
  } while (!resident_.compare_exchange_weak(resident, resident + bytes, std::memory_order_acq_rel,
                                            std::memory_order_relaxed));

  clip.samples.reserve(decoded.samples.size());
  clip.samples.append(std::span<const f32>(decoded.samples.data(), decoded.samples.size()));
  clip.frames = decoded.frames;
  clip.channels = decoded.channels;
  clip.source_channels = decoded.source_channels;
  clip.source_rate = decoded.source_rate;
  clip.bytes = bytes;
  clip.state.store(ClipState::Ready, std::memory_order_release);
  return true;
}

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
  return out;
}

ClipView ClipStore::view(ClipHandle clip) const noexcept {
  if (state(clip) != ClipState::Ready) return ClipView{};
  const Clip& c = *clips_[clip.index];
  return ClipView{c.samples.data(), c.frames, c.channels};
}

}  // namespace engine::audio

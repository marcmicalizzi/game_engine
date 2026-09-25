#pragma once

// One stream: the ring a streaming voice reads and the fill that keeps it ahead of the voice
// (docs/subsystems/audio.md, "Streaming"; the protocol between the two is in stream.h).
//
// Three owners, each touching its own part:
//
//   the fill        the source, the decoder and the position in the clip, while a fill is in
//                   flight — the controlling thread schedules at most one per stream at a time and
//                   reads that state only once the job's counter says it is done
//   the voice       `ring.consumed`, `ring.underrun_frames`, `ring.base` (the audio thread)
//   the controller  everything else: which voice owns the stream, when to fill, when to free it
//
// The ring's floats are written by the fill and read by the voice, with `ring.filled` (release by
// the fill, acquire by the voice) and `ring.consumed` (release by the voice, acquire by the fill)
// ordering every write before the read of the same slot.

#include "backend.h"

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/ids/id128.h>
#include <core/jobs/job_system.h>
#include <domain/audio/clip_store.h>
#include <domain/audio/stream.h>
#include <foundation/io/vfs.h>

#include <atomic>

namespace engine::audio {

struct Stream {
  Stream() = default;
  ~Stream();
  ENGINE_NON_COPYABLE(Stream);

  StreamRing ring;
  // The ring's frames, `capacity` of them at up to two channels, allocated by the controlling
  // thread the first time the stream plays and kept.
  Vector<f32> buffer;
  u32 capacity = 0;
  u8 channels = 0;

  // ---- the fill's
  ClipSource source;
  io::FileHandle file;
  backend::StreamDecoder* decoder = nullptr;
  u64 source_frame = 0;  // the clip frame the next frame read is
  u32 start_frame = 0;   // where the voice started, in clip frames
  bool opened = false;
  bool finished = false;  // the end, or a failure, is in the ring: nothing more to fill
  bool failed = false;
  // ---- written by the controlling thread, read by the fill at the clip's end
  std::atomic<u8> loop{0};

  // ---- the controlling thread's
  jobs::Counter pending;  // the fill in flight, if any
  u8 state = 0;           // k_stream_free, k_stream_playing, k_stream_retiring
  u32 slot = 0;           // the voice it serves: slot and generation
  u32 generation = 0;
  u32 clip = 0;  // the clip's handle index
  // Nonzero once the voice's slot was stolen: the sequence number of the Play that took it. The
  // voice reads the ring until the audio thread has applied that command.
  u64 retire_after = 0;
  bool underrun_logged = false;
};

inline constexpr u8 k_stream_free = 0;
inline constexpr u8 k_stream_playing = 1;
inline constexpr u8 k_stream_retiring = 2;

// Frames one read of a fill asks the source for: 4096 frames is 85 ms of audio, a ranged read of
// 16 KB of 16-bit stereo, and the unit in which the fill publishes, so a voice that has just
// started can play the first read's frames while the fill decodes the next.
inline constexpr u32 k_stream_fill_chunk = 4096;

// The controlling thread: makes `stream` the one `source` plays through, from clip frame
// `start_frame`, with a ring of `capacity` frames. No fill may be in flight.
void reset_stream(Stream& stream, const ClipSource& source, u32 start_frame, bool loop,
                  u32 capacity);
// The controlling thread: closes the source and the decoder. No fill may be in flight.
void close_stream(Stream& stream) noexcept;
// Frames decoded ahead of the voice: what the controlling thread compares with the fill-ahead.
u64 stream_buffered(const Stream& stream) noexcept;

// Tops the ring up: reads the source into every slot the voice has finished with, keeping one
// slot spare for a one-shot's silent end frame. Runs on the Efficiency pool (or inline, with no
// job system); never on the audio thread.
void fill_stream(Stream& stream) noexcept;
void fill_stream_job(void* stream);

}  // namespace engine::audio

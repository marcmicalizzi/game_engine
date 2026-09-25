// The fill side of a stream (stream.h, docs/subsystems/audio.md "Streaming"). Everything here runs
// on the job system's Efficiency pool, or inline on the controlling thread when there is no job
// system — never on the audio thread, which only reads what this publishes.
#include "stream.h"

#include "audio_log.h"

#include <cstring>

namespace engine::audio {

Stream::~Stream() { close_stream(*this); }

void reset_stream(Stream& stream, const ClipSource& source, u32 start_frame, bool loop,
                  u32 capacity) {
  close_stream(stream);
  stream.source = source;
  stream.channels = source.channels;
  stream.capacity = capacity;
  // Two channels' worth whatever this clip has, so a stream is never reallocated for the next.
  const u32 floats = capacity * 2u;
  if (stream.buffer.size() != floats) stream.buffer.resize_exact(floats, 0.0f);
  stream.start_frame = start_frame;
  stream.source_frame = start_frame;
  stream.opened = false;
  stream.finished = false;
  stream.failed = false;
  stream.loop.store(loop ? 1u : 0u, std::memory_order_relaxed);
  stream.underrun_logged = false;
  // No voice reads the ring until the Play that names it is applied, and the command ring's
  // release and acquire order these stores before that; `base` is the audio thread's own and is
  // reset when it applies the Play.
  stream.ring.filled.store(0, std::memory_order_relaxed);
  stream.ring.end.store(k_stream_no_end, std::memory_order_relaxed);
  stream.ring.consumed.store(0, std::memory_order_relaxed);
  stream.ring.underrun_frames.store(0, std::memory_order_relaxed);
}

void close_stream(Stream& stream) noexcept {
  if (stream.decoder != nullptr) {
    backend::close_stream_decoder(stream.decoder);
    stream.decoder = nullptr;
  }
  stream.file.close();
  stream.opened = false;
}

u64 stream_buffered(const Stream& stream) noexcept {
  const u64 filled = stream.ring.filled.load(std::memory_order_acquire);
  const u64 consumed = stream.ring.consumed.load(std::memory_order_acquire);
  return filled > consumed ? filled - consumed : 0u;
}

namespace {

bool open_source(Stream& s) noexcept {
  backend::StreamFormat format;
  DecodeStatus status = DecodeStatus::Ok;
  switch (s.source.kind) {
    case ClipSourceKind::Memory: {
      backend::ByteSource bytes;
      bytes.memory = s.source.memory;
      s.decoder = backend::open_stream_decoder(bytes, format, status);
      break;
    }
    case ClipSourceKind::File:
    case ClipSourceKind::ClipFile: {
      if (s.file.open(s.source.path) != io::Status::Ok) {
        ENGINE_LOG_WARN(log_audio, "stream source unreadable", log::field("clip", s.source.key),
                        log::field("path", s.source.path));
        return false;
      }
      if (s.source.kind == ClipSourceKind::ClipFile) {
        s.opened = true;
        return true;
      }
      backend::ByteSource bytes;
      bytes.file = &s.file;
      bytes.size = s.file.size();
      s.decoder = backend::open_stream_decoder(bytes, format, status);
      break;
    }
    case ClipSourceKind::None: return false;
  }
  if (s.decoder == nullptr || format.channels != s.channels) {
    ENGINE_LOG_WARN(log_audio, "stream decoder failed", log::field("clip", s.source.key),
                    log::field("status", decode_status_name(status)),
                    log::field("channels", u32{format.channels}));
    return false;
  }
  if (s.start_frame != 0 && !backend::seek_stream_decoder(s.decoder, s.start_frame)) {
    ENGINE_LOG_WARN(log_audio, "stream seek failed", log::field("clip", s.source.key),
                    log::field("frame", s.start_frame));
    return false;
  }
  s.opened = true;
  return true;
}

// Reads up to `frames` frames of the clip at `s.source_frame` into `out`. Fewer only at the clip's
// end; `failed` set on anything else.
u32 read_source(Stream& s, f32* out, u32 frames) noexcept {
  if (s.source.kind == ClipSourceKind::ClipFile) {
    // The content build's frames, as they are: a ranged read, no decoder.
    const u64 total = s.source.frames;
    const u64 left = s.source_frame < total ? total - s.source_frame : 0u;
    const u32 n = left < frames ? static_cast<u32>(left) : frames;
    if (n == 0) return 0;
    const u64 frame_bytes = u64{s.channels} * sizeof(f32);
    const u64 want = u64{n} * frame_bytes;
    u64 got = 0;
    const io::Status status =
        s.file.read_at(s.source.samples_offset + s.source_frame * frame_bytes, out, want, got);
    if (status != io::Status::Ok || got != want) {
      ENGINE_LOG_WARN(log_audio, "stream read failed", log::field("clip", s.source.key),
                      log::field("frame", s.source_frame), log::field("bytes", got));
      s.failed = true;
      return 0;
    }
    return n;
  }
  DecodeStatus status = DecodeStatus::Ok;
  const u32 got = backend::read_stream_decoder(s.decoder, out, frames, status);
  if (status != DecodeStatus::Ok) {
    ENGINE_LOG_WARN(log_audio, "stream decode failed", log::field("clip", s.source.key),
                    log::field("frame", s.source_frame),
                    log::field("status", decode_status_name(status)));
    s.failed = true;
    return 0;
  }
  return got;
}

bool rewind_source(Stream& s) noexcept {
  if (s.source.kind == ClipSourceKind::ClipFile) {
    s.source_frame = 0;
    return true;
  }
  if (!backend::seek_stream_decoder(s.decoder, 0)) {
    ENGINE_LOG_WARN(log_audio, "stream seek failed", log::field("clip", s.source.key),
                    log::field("frame", u32{0}));
    s.failed = true;
    return false;
  }
  s.source_frame = 0;
  return true;
}

// The end of the stream: a silent frame at `filled`, so the voice's last interpolation reads zero
// past the clip's last frame as it does on a stored one-shot, then the end, then the frame. The
// fill always leaves the slot for it.
void end_stream(Stream& s, u64 filled) noexcept {
  const u32 slot = static_cast<u32>(filled % s.capacity);
  std::memset(s.buffer.data() + static_cast<usize>(slot) * s.channels, 0, sizeof(f32) * s.channels);
  s.ring.end.store(filled, std::memory_order_release);
  s.ring.filled.store(filled + 1u, std::memory_order_release);
  s.finished = true;
}

}  // namespace

void fill_stream(Stream& s) noexcept {
  if (s.finished) return;
  u64 filled = s.ring.filled.load(std::memory_order_relaxed);  // this fill is the only writer
  if (!s.opened && !open_source(s)) {
    s.failed = true;
    end_stream(s, filled);
    return;
  }
  const u64 capacity = s.capacity;
  const u32 channels = s.channels;
  f32* ring = s.buffer.data();
  bool rewound = false;  // a loop's rewind that has not read a frame yet
  for (;;) {
    const u64 consumed = s.ring.consumed.load(std::memory_order_acquire);
    const u64 room = consumed + capacity - filled;
    if (room <= 1u) break;  // the last slot is kept for a one-shot's silent end frame
    const u64 slot = filled % capacity;
    u64 n = room - 1u;
    if (n > capacity - slot) n = capacity - slot;  // up to the ring's end; the next read wraps
    // A decoder's output is published a chunk at a time, so a voice can start on the first while
    // the next decodes. A built clip's frames need no decoding, and there the cost is the number of
    // reads, so they are read in one piece up to the ring's end.
    if (n > k_stream_fill_chunk && s.source.kind != ClipSourceKind::ClipFile)
      n = k_stream_fill_chunk;
    const u32 want = static_cast<u32>(n);
    const u32 got = read_source(s, ring + slot * channels, want);
    if (s.failed) break;
    if (got > 0) {
      filled += got;
      s.source_frame += got;
      rewound = false;
      s.ring.filled.store(filled, std::memory_order_release);
    }
    if (got < want) {
      // The clip's end. A loop starts it again — unless it has just done that and found nothing,
      // which is a clip with no frames to loop.
      if (s.loop.load(std::memory_order_relaxed) != 0 && !rewound) {
        if (!rewind_source(s)) break;
        rewound = true;
        continue;
      }
      end_stream(s, filled);
      return;
    }
  }
  if (s.failed) end_stream(s, filled);
}

void fill_stream_job(void* stream) { fill_stream(*static_cast<Stream*>(stream)); }

}  // namespace engine::audio

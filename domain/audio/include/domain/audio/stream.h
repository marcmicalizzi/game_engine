#pragma once

// Streaming (docs/subsystems/audio.md, "Streaming").
//
// A clip decoded larger than `audio.stream_threshold_kb` is never held whole: a voice playing it
// owns a **ring** of decoded frames, and a decode job on the job system's Efficiency pool keeps the
// ring filled ahead of the voice's playhead from ranged reads of the source — the store's own
// decoders run incrementally, or, for a clip the content build decoded into a `.clip`, the frames
// read as they are. The audio thread reads what is already in the ring and **never waits**: a
// frame that has not arrived is silence, counted, and the voice holds its place until it does.
//
// The ring is addressed by **stream frames**: frame 0 is the clip frame the voice started at, and
// the numbering runs on through every pass of a loop, so a stream is one sequence that only grows.
// Stream frame f lives in ring slot f mod R. Three counters, each written by one side, are the
// whole protocol between the fill and the voice:
//
//   filled     the fill has written stream frames [0, filled), with release after the samples
//   end        a one-shot's end — the stream frame after its last, which the fill writes as a
//              silent frame so the voice's interpolation reads zero past the end exactly as it
//              does on a stored clip — or `k_stream_no_end` while the fill has not reached it
//   consumed   the voice needs no stream frame below this again, with release after its reads;
//              the fill may overwrite a slot only for a frame below consumed + R
//
// The voice reads the ring as it reads a looping clip of R frames — the same 32.32 playhead, the
// same interpolation, the same arithmetic in the same order — so a streamed clip that never runs
// dry mixes to exactly the bytes the same clip does stored. The determinism test holds it to that.
//
// `StreamRing` is the part both sides touch, one cache line each, so the fill's stores and the
// voice's never share a line. It is public so the size table can pin it.

#include <core/base/types.h>

#include <atomic>

namespace engine::audio {

inline constexpr u64 k_stream_no_end = ~u64{0};

struct StreamRing {
  // ---- written by the fill, read by the voice
  alignas(64) std::atomic<u64> filled{0};
  std::atomic<u64> end{k_stream_no_end};
  // ---- written by the voice (the audio thread), read by the fill and the controlling thread
  alignas(64) std::atomic<u64> consumed{0};
  // Frames of silence the voice played for want of data: an underrun. Written by the audio thread
  // alone, read by the controlling thread to log the voice's first one.
  std::atomic<u64> underrun_frames{0};
  // The stream frame in ring slot 0 of the pass the playhead is in: the audio thread's own.
  u64 base = 0;
};

}  // namespace engine::audio

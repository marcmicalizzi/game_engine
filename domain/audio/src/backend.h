#pragma once

// The one door to miniaudio (cmake/EngineAudio.cmake). backend.cpp is the only translation unit
// in the engine that includes <miniaudio.h>; everything else in this module speaks the engine
// types below, so the library's types, its defines and its warnings stay in one file.

#include "endpoint.h"

#include <core/base/types.h>
#include <domain/audio/clip_store.h>
#include <domain/audio/device.h>
#include <foundation/io/vfs.h>

#include <atomic>
#include <span>
#include <string>

namespace engine::audio::backend {

// Bytes in, 48 kHz f32 mono or stereo out (ClipStore's jobs, `decode_clip`).
DecodeStatus decode(std::span<const u8> encoded, DecodedClip& out);

// ---- incremental decoding, for streams (stream.h) ----------------------------------------------
//
// The same decoders and the same conversion `decode` runs — f32, the mix rate, the linear resampler
// at its highest filter order, more than two channels folded to stereo — over a source read a piece
// at a time: bytes the store holds, or a file read by range through `io::FileHandle`, so a stream
// reads the part of the file it is about to play and nothing else.

// What a stream decoder reads. `memory` when the store holds the bytes; otherwise `file`, whose
// `size` bytes are read by range.
struct ByteSource {
  std::span<const u8> memory;
  const io::FileHandle* file = nullptr;
  u64 size = 0;
};

// What the decoder produces, as the store needs it to decide whether a clip streams.
struct StreamFormat {
  u64 frames = 0;   // at the mix rate; 0 when the decoder cannot say without decoding it all
  u8 channels = 0;  // 1 or 2
  u8 source_channels = 0;
  u32 source_rate = 0;
};

struct StreamDecoder;

// Opens a decoder over `source`. Null, with `status` saying why, when no decoder recognizes it. The
// source must outlive the decoder.
StreamDecoder* open_stream_decoder(const ByteSource& source, StreamFormat& format,
                                   DecodeStatus& status);
// Decodes up to `frames` frames into `out`, interleaved. Fewer only at the end (0 there); `status`
// is `Corrupt` when the decoder failed rather than ended.
u32 read_stream_decoder(StreamDecoder* decoder, f32* out, u32 frames, DecodeStatus& status);
// Positions the decoder at `frame` of its output, at the mix rate.
bool seek_stream_decoder(StreamDecoder* decoder, u64 frame);
void close_stream_decoder(StreamDecoder* decoder) noexcept;
// Opens, reads the format, closes: whether a clip would stream, without decoding it.
DecodeStatus probe(const ByteSource& source, StreamFormat& format);

// Playback devices from the first platform backend that initializes, each with the layout and
// rate its endpoint really has.
DeviceList enumerate();

struct DeviceRequest {
  const std::string* name = nullptr;  // empty or null: the default device
  u32 period_frames = 0;
};

// Opens and starts a playback device fed in the mixer's layout, whose callback renders `mixer`.
// Null on failure, with the reason in `why`; `opened` describes the endpoint as the platform
// reported it. miniaudio's device notifications set `DeviceEvent` bits in `events`, and nothing
// else. The default device is opened as "the default" rather than by its id, so a PulseAudio
// stream follows the default sink; WASAPI's automatic rerouting is off (backend.cpp says why).
Output::Device* open_device(const DeviceRequest& request, Mixer& mixer, std::atomic<u32>& events,
                            DeviceInfo& opened, std::string& why);
// Stops the device (joining its thread, so no callback is running when this returns) and frees it.
void close_device(Output::Device* device) noexcept;
// Whether the device has stopped. Nothing stops an open device but a failed stream — the
// platform's own rerouting is off — so a device that says it has is gone. Any thread.
bool device_stopped(const Output::Device* device) noexcept;
// The endpoint's id as the platform spells it (endpoint.h), for the watcher; null if none.
const endpoint::IdChar* endpoint_id(const Output::Device* device) noexcept;

}  // namespace engine::audio::backend

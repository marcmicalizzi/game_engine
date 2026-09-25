#pragma once

// The wire format between the controlling thread and the audio thread (docs/subsystems/audio.md,
// "The thread model").
//
// Everything the controlling thread asks of the mix is a `Command`: fixed size, trivially
// copyable, written into an `SpscQueue<Command>` and applied by the audio thread at the top of its
// next block, in the order it was written. Everything the audio thread tells the controlling
// thread back is a `VoiceEvent` on a second queue. There is no third channel: the mixer's state is
// a function of the commands it has applied, which is what makes the mix reproducible from a
// command sequence (the determinism test pins it) and what lets the audio thread go without a
// lock.
//
// **Why 128 bytes.** A source is an object all the way down (spatial.h), and a `Play` has to carry
// the whole of it — clip (or stream), level, pitch, and the 56-byte spatial block — so that a voice
// is never audible for a block with half its description. That is 104 bytes; the rest is padding to
// two whole cache lines, so no command shares a line with its neighbour. Splitting a play into a
// play and a source update would halve the ring's footprint (128 KB at the default 1024 commands)
// and open a window in which a render's drain budget could apply one without the other.
//
// The payloads are plain structs of scalars and float arrays rather than `Vec3`s because they share
// a union, and a union member may not have a default member initializer.

#include <core/base/types.h>

namespace engine::audio {

// A voice, as the controlling thread names it: a slot in the fixed pool and the generation that
// slot was on when the voice started. A handle whose generation the slot has moved past names a
// voice that is gone, and every command carrying it is ignored and counted rather than applied to
// whichever voice took the slot over. Generation 0 is never issued, so a default handle is null.
struct VoiceHandle {
  u32 slot = 0;
  u32 generation = 0;

  constexpr bool is_null() const noexcept { return generation == 0; }
  constexpr bool operator==(const VoiceHandle&) const noexcept = default;
};

enum class CommandKind : u8 {
  Play = 0,     // start a voice in `slot`, replacing whatever the slot held
  Stop,         // fade the voice out over the ramp time, then free it
  SetParams,    // gain, pitch, pan, bus and loop of a live voice
  SetSource,    // the whole spatial block of a live voice
  SetListener,  // where the listener is; applies to every 3D source
  SetBusGain,   // one bus's gain; `bus` names it
};

// Voice flags, in `Command::flags` and in the audio thread's voice.
inline constexpr u8 k_voice_loop = 1u << 0;
// The voice plays a stream (stream.h): `samples` is its ring and `frames` the ring's length, and
// `PlayPayload::stream` names the stream. Set by a Play only; a SetParams never changes it.
inline constexpr u8 k_voice_stream = 1u << 1;
// `PlayPayload::stream` of a voice that plays a stored clip.
inline constexpr u32 k_no_stream = 0xFFFF'FFFFu;

// `SourceSpatial`, flattened.
struct SourcePayload {
  f32 position[3];
  f32 orientation[4];  // x, y, z, w
  f32 min_distance;
  f32 max_distance;
  f32 cone_inner_cos;
  f32 cone_outer_cos;
  f32 cone_outer_gain;
  f32 spread;
  u8 distance_model;
  u8 directivity;
  u8 mapping;
  u8 flags;
};

struct PlayPayload {
  const f32* samples;  // the clip's interleaved samples at the mix rate, or a stream's ring
  u32 frames;
  f32 gain;
  f32 pitch;
  f32 pan;
  SourcePayload source;
  u32 stream;  // the mixer's stream the voice reads, or k_no_stream
};

struct ParamsPayload {
  f32 gain;
  f32 pitch;
  f32 pan;
};

// An orthonormal basis (`ListenerBasis`), already reduced by the controlling thread.
struct ListenerPayload {
  f32 position[3];
  f32 right[3];
  f32 up[3];
  f32 forward[3];
};

struct BusPayload {
  f32 gain;
};

union CommandPayload {
  PlayPayload play;
  ParamsPayload params;
  SourcePayload source;
  ListenerPayload listener;
  BusPayload bus;
};

struct alignas(64) Command {
  CommandKind kind = CommandKind::Play;
  u8 channels = 0;  // Play: 1 or 2
  u8 bus = 0;       // Play, SetParams, SetBusGain
  u8 flags = 0;     // Play, SetParams: k_voice_*
  u32 slot = 0;
  u32 generation = 0;
  u32 start_frame = 0;  // Play: where in the clip the voice starts
  CommandPayload payload{};
};

// The audio thread's only message back: this voice has ended — its clip ran out, or a stop's fade
// finished — and its slot is free. The controlling thread frees the slot when it reads this, never
// before, so a slot it hands to a new voice is one the audio thread has finished with — unless it
// is stealing that voice on purpose, which is a `Play` into a live slot and says so.
struct VoiceEvent {
  u32 slot = 0;
  u32 generation = 0;
};

}  // namespace engine::audio

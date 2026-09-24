#pragma once

// The decode stage: sources in, the declared layout's channels out (docs/subsystems/audio.md,
// "The decode stage").
//
// This is the one place in the audio path that knows how many speakers there are and where they
// stand. Everything before it — the voice loop, the bus tree, the source model — works on sources
// (a signal and a `SpatialParams`); everything after it is the master's clip and the device. So a
// different way of reaching the listener's ears is a different `Decoder`, and nothing else:
//
//   * the stereo panner (v0, `k_stereo_panner`): constant power between the front pair;
//   * a vector-base amplitude panner over every speaker of a declared layout, heights included;
//   * an ambisonic encoder into an intermediate bus and a decoder from it to the layout;
//   * the platform's object renderer (Windows Spatial Sound), which takes sources as objects;
//   * binaural rendering over headphones (Steam Audio's HRTF);
//   * an object stream to an external renderer — a sound card or receiver that places positional
//     objects against its own room calibration — which emits objects instead of channels.
//
// Only the first is built (the docs page's "Not yet" says what each other one's seam is). What they
// need from the mixer is here already: the source's signal before any panning, the direction and
// spread from the source model, the 2D flag and channel mapping for a non-diegetic source, the
// layout's channel-position table, and per-source state of a size the decoder declares.
//
// **Why a table of function pointers and not a virtual interface** (ADR-0027: registration points
// are constant-initialized tables): the mixer calls `decode` once per source per block, and the
// loop over the block's frames is inside it, so there is exactly one indirect call per source per
// block and none in the loop. A decoder is chosen when the mixer is built and never changes under
// it.

#include <core/base/types.h>
#include <domain/audio/format.h>
#include <domain/audio/spatial.h>

#include <schemas/audio.h>

namespace engine::audio {

// One source, one block, as the decode stage sees it.
struct DecodeInput {
  // The source's signal for this block after pitch and interpolation, before any gain:
  // `frames` frames of `channels` interleaved samples. Frames after a one-shot's end are zero.
  const f32* signal = nullptr;
  u32 frames = 0;
  u8 channels = 0;  // 1 or 2
  // The level the block should end at: the voice's gain, its bus path, and the source model's
  // attenuation. 0 for a voice that is stopping.
  f32 gain = 0.0f;
  // First block of a voice: start at the target gains rather than ramping from the last block's.
  bool fresh = false;
  // Non-diegetic: map channels by `mapping` and `pan`, ignore `spatial`.
  bool two_d = false;
  ChannelMapping mapping = ChannelMapping::FrontPair;  // never Inherit here: the mixer resolved it
  f32 pan = 0.0f;
  // What the source model made of the object (3D sources).
  SpatialParams spatial;
  // The object itself, whole: a decoder that emits objects rather than channels — a platform
  // object renderer, an object stream to an external renderer — reads it from here, and the
  // listener's basis beside it, instead of the stereo panner's summary above.
  const SourceSpatial* source = nullptr;
  const ListenerBasis* listener = nullptr;
};

struct Decoder {
  const char* name;
  // Floats of state the decoder keeps per source (its last block's gains, say). The mixer
  // allocates voices x this once, zeroed, and hands each call its own source's slice.
  u32 state_floats;
  // Whether it can feed `layout`. A mixer built with a decoder that cannot falls back to the
  // stereo panner and says so.
  bool (*supports)(ChannelLayout layout) noexcept;
  // Adds one source's block into `out`: `in.frames` frames of `layout.channels` interleaved
  // channels. Reads and rewrites `state`. Called on the audio thread: no allocation, no lock, no
  // log.
  void (*decode)(const DecodeInput& in, const LayoutInfo& layout, f32* state, f32* out) noexcept;
};

// v0's decoder. Constant power between the layout's front pair for a mono 3D source, panned by the
// lateral component of its direction narrowed by its spread; balance for a stereo one; the channel
// mapping for a 2D source. On a layout wider than stereo it feeds the front pair and leaves the
// other speakers silent — honest about being a stereo panner — and a mono layout gets the fold.
// Every gain ramps linearly across the block from where the last block left it.
extern const Decoder k_stereo_panner;

// Writes nothing: the mix is silence whatever the sources do. For tests that exercise the mixer's
// bookkeeping with the decode stage out of the picture, and the proof that the stage is one.
extern const Decoder k_null_decoder;

}  // namespace engine::audio

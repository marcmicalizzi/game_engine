#pragma once

// The mix format and the speaker-layout profiles (docs/subsystems/audio.md, "The mix format" and
// "Layouts").
//
// **One rate and one sample type** for everything the mixer touches: 48 kHz, 32-bit float,
// interleaved. Clips are brought to it when they are decoded, off the audio thread, so the voice
// loop never converts a format and a voice at pitch 1 reads its clip's samples unchanged.
//
// **The layout is a profile from a shipped table, declared, never assumed.** Each row is plain
// data: the enumeration value, the channel count, the channel order as speaker roles — which is
// the mapping onto the device's channel map — and each channel's azimuth and elevation at the
// canonical ITU-R angles (BS.775 for 5.1; BS.2051 Systems I and J for 7.1 and 7.1.4, whose height
// ring sits at 45 degrees up). The master bus feeds one profile, chosen with no configuration from
// the channel layout the endpoint reports and falling back to stereo (device.h,
// `resolve_layout`) — the consumer path, exactly as games work today — or named by the
// `audio.layout` setting. Everything upstream of the decode stage (decoder.h) is layout-free: a
// source is an object with a position, never a pair of channel gains, and the one place that turns
// objects into speaker feeds reads this table. The stereo panner reads the front pair from it
// today; a VBAP or ambisonic decoder reads every row's positions later; and a measured calibration
// is an override of the same rows — their positions, and the gains and delays that will join them
// — not a separate path.

#include <core/base/types.h>
#include <core/math/math.h>

#include <schemas/audio.h>
#include <span>

namespace engine::audio {

// The mix rate. Fixed, not a tunable: it is a data-selected dimension (every clip is converted to
// it at decode time and every voice's step is expressed against it), and ADR-0011 keeps those out
// of the tunables. 48 kHz rather than 44.1 because it is what current output devices run at
// natively, so the device path usually has nothing to resample.
inline constexpr u32 k_sample_rate = 48000;

// Frames in `ms` milliseconds at the mix rate: 480 for the 10 ms block the bench and the device
// default use.
constexpr u32 frames_for_ms(u32 ms) noexcept { return k_sample_rate / 1000u * ms; }

// The most channels any shipped profile has (7.1.4). The decode stage's per-source state and the
// mixer's output stride are sized from the profile actually declared, never from this.
inline constexpr u32 k_max_layout_channels = 12;

// One loudspeaker, by role. The names are the platforms' (WAVE, miniaudio): a profile's channel
// order is a list of these, and matching it against the endpoint's list is how a device's layout
// is recognized and how the device is fed.
enum class Speaker : u8 {
  FrontLeft = 0,
  FrontRight,
  FrontCentre,
  LowFrequency,
  BackLeft,
  BackRight,
  SideLeft,
  SideRight,
  TopFrontLeft,
  TopFrontRight,
  TopBackLeft,
  TopBackRight,
  Mono,  // the single channel of a mono endpoint: a centre with nothing either side of it
  Count,
};

const char* speaker_name(Speaker speaker) noexcept;

// A row of the table.
struct LayoutInfo {
  ChannelLayout layout = ChannelLayout::Unknown;
  const char* name = "unknown";
  u8 channels = 0;
  // Channel indices of the front pair, or -1. A decoder that only knows a stereo image writes
  // these; mono has one channel and both point at it.
  i8 front_left = -1;
  i8 front_right = -1;
  // The channel order, as roles: interleaved channel c feeds `speakers[c]`.
  Speaker speakers[k_max_layout_channels] = {};
  // Where each channel's speaker is, in degrees: azimuth 0 ahead and positive to the right,
  // elevation 0 at ear height and positive up. The LFE has no position and reads 0, 0.
  f32 azimuth[k_max_layout_channels] = {};
  f32 elevation[k_max_layout_channels] = {};
};

// The row for `layout`. `Unknown` gets a row with no channels.
const LayoutInfo& layout_info(ChannelLayout layout) noexcept;
constexpr u32 layout_channels(ChannelLayout layout) noexcept {
  switch (layout) {
    case ChannelLayout::Mono: return 1;
    case ChannelLayout::Stereo: return 2;
    case ChannelLayout::Headphones: return 2;
    case ChannelLayout::Quad: return 4;
    case ChannelLayout::Surround51: return 6;
    case ChannelLayout::Surround71: return 8;
    case ChannelLayout::Surround714: return 12;
    case ChannelLayout::Unknown: return 0;
  }
  return 0;
}
const char* layout_name(ChannelLayout layout) noexcept;

// The unit vector toward channel `channel`'s speaker in listener space (+x right, +y up, -z
// ahead), from its azimuth and elevation through `cos_degrees` — no libm, so the same bits
// everywhere. Zero for the LFE.
Vec3 speaker_direction(const LayoutInfo& layout, u32 channel) noexcept;

// The profile a device's channel map is, or `Unknown`. The order must match too: a map is a
// promise about which interleaved channel feeds which speaker. Two spellings the platforms use for
// rows the table already has are accepted: a mono endpoint that calls its channel the front
// centre, and 5.1 with its surrounds labelled as sides (Windows' default "5.1 surround"). A
// headphone endpoint reports a stereo map, so it is recognized as stereo; the headphones profile is
// chosen by the setting until the platform says what the endpoint is.
ChannelLayout layout_from_speakers(std::span<const Speaker> speakers) noexcept;

}  // namespace engine::audio

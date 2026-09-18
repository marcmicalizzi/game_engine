#pragma once

// Animation clips (docs/plan/05-simulation.md §5.11). A clip is a set of keyframe tracks, one
// per (joint, channel) pair, sampled into a `Pose`. The three glTF sampler modes are all here,
// because a clip is imported and not authored: LINEAR, STEP, and CUBICSPLINE with the tangents
// the exporter wrote (docs/subsystems/assets.md, "Skins and animations").
//
// **SoA again, and one level deeper than the pose.** A clip holds *every* track's keyframe times
// in one array and every track's values in another, with a small `Track` record naming each
// one's run. A clip is read-only data that outlives thousands of samples of it, so the layout is
// chosen for the sample: the tracks of a clip are sampled in order, so their times are walked in
// memory order, and the alternative — a `Vector` per track — would be one allocation and one
// pointer chase per joint per channel. Sampling allocates nothing.
//
// A clip **writes only the joints its tracks name.** Fill the pose with `rest_pose` first (or
// with the output of another clip, for a layered blend); a joint nothing animates then keeps
// that value instead of whatever the buffer held.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/time/time.h>
#include <domain/anim/skeleton.h>

#include <string>

namespace engine::anim {

// How a track interpolates between keys. The values mirror `assets::k_interp_*`, so an import is
// a copy rather than a switch.
inline constexpr u8 k_interp_linear = 0;
inline constexpr u8 k_interp_step = 1;
inline constexpr u8 k_interp_cubic = 2;

// Which channel of a joint a track drives; mirrors `assets::k_path_*`.
inline constexpr u8 k_channel_translation = 0;
inline constexpr u8 k_channel_rotation = 1;
inline constexpr u8 k_channel_scale = 2;

// One keyframe track: a run of `Clip::times` and the matching run of `Clip::values`. `components`
// is 3 for a translation or a scale and 4 for a rotation; a CUBICSPLINE track stores three
// elements per key (in-tangent, value, out-tangent), so its run of values is three times as long.
struct Track {
  u32 joint = 0;
  u32 first_key = 0;  // into Clip::times
  u32 key_count = 0;
  u32 first_value = 0;  // into Clip::values, in floats
  u8 channel = k_channel_translation;
  u8 interpolation = k_interp_linear;
  u8 components = 3;
  u8 pad = 0;
};

struct Clip {
  std::string name;
  Vector<Track> tracks;
  Vector<f32> times;   // every track's keyframe times, back to back
  Vector<f32> values;  // every track's values, back to back
  f32 duration = 0.0f;
  u32 joint_count = 0;  // the skeleton this clip was authored against

  // Appends one track and its keys. `times` must be non-decreasing and `values` must hold
  // `components` floats per key (three times that for CUBICSPLINE). Returns false, leaving the
  // clip untouched, when the lengths do not agree; `duration` grows to the last key's time.
  bool add_track(u32 joint, u8 channel, u8 interpolation, u8 components,
                 std::span<const f32> key_times, std::span<const f32> key_values);

  // Samples every track into `out`, which must already hold `joint_count` joints (it is not
  // resized, because a clip writes only what it animates). `time` is in seconds. With `loop`,
  // time wraps into [0, duration); without it, time is clamped, so a clip held past its end
  // holds its last pose. A track with one key is that key everywhere; a time before the first
  // key or after the last takes that key's value rather than extrapolating, which is what every
  // DCC tool does and what keeps a blend from flying apart at a clip boundary.
  void sample(f32 time, Pose& out, bool loop = true) const;
  // The same on the engine's game clock, which is integer microseconds (core/time, ADR-0017):
  // a scheduler-driven layer never has to invent a float.
  void sample(GameTime time, Pose& out, bool loop = true) const;

  // Parallel arrays, runs inside the streams, non-decreasing times, joints under `joint_count`.
  bool validate(std::string* error = nullptr) const;
};

}  // namespace engine::anim

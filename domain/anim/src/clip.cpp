#include <domain/anim/clip.h>

#include <cmath>

namespace engine::anim {

namespace {

bool fail(std::string* error, std::string message) {
  if (error != nullptr) *error = std::move(message);
  return false;
}

// The key at or before `time`, and how far between it and the next one `time` sits. A time
// before the first key or after the last one clamps to that key with a fraction of 0, so a clip
// holds its ends rather than extrapolating — every DCC tool does the same, and extrapolating a
// quaternion past its last key is how a blend flies apart at a clip boundary.
//
// Linear from the front rather than a binary search: a track has a handful of keys, they are
// walked in memory order, and the branchless compare is faster than the unpredictable one until
// counts nothing here will see.
struct KeyPair {
  u32 key = 0;
  f32 fraction = 0.0f;
};

KeyPair locate(std::span<const f32> times, f32 time) noexcept {
  KeyPair out;
  const u32 count = static_cast<u32>(times.size());
  if (count <= 1 || time <= times[0]) return out;
  if (time >= times[count - 1]) {
    out.key = count - 1;
    return out;
  }
  u32 key = 0;
  while (key + 2 < count && times[key + 1] <= time)
    ++key;
  const f32 span = times[key + 1] - times[key];
  out.key = key;
  out.fraction = span > 0.0f ? (time - times[key]) / span : 0.0f;
  return out;
}

Vec3 vec3_at(const f32* values) noexcept { return Vec3{values[0], values[1], values[2]}; }
Quat quat_at(const f32* values) noexcept {
  return Quat{values[0], values[1], values[2], values[3]};
}

// The cubic Hermite basis glTF's CUBICSPLINE is defined by, with the tangents scaled by the key
// interval, which is what the specification's pseudo-code does and what makes a resampled clip
// look the same as the authored one.
f32 hermite(f32 p0, f32 m0, f32 p1, f32 m1, f32 t, f32 span) noexcept {
  const f32 t2 = t * t;
  const f32 t3 = t2 * t;
  return (2.0f * t3 - 3.0f * t2 + 1.0f) * p0 + (t3 - 2.0f * t2 + t) * span * m0 +
         (-2.0f * t3 + 3.0f * t2) * p1 + (t3 - t2) * span * m1;
}

}  // namespace

bool Clip::add_track(u32 joint, u8 channel, u8 interpolation, u8 components,
                     std::span<const f32> key_times, std::span<const f32> key_values) {
  if (key_times.empty()) return false;
  if (components != 3 && components != 4) return false;
  const usize per_key = interpolation == k_interp_cubic ? usize{3} : usize{1};
  if (key_values.size() != key_times.size() * per_key * components) return false;
  for (usize i = 1; i < key_times.size(); ++i) {
    if (key_times[i] < key_times[i - 1]) return false;
  }

  Track track;
  track.joint = joint;
  track.channel = channel;
  track.interpolation = interpolation;
  track.components = components;
  track.first_key = times.size();
  track.key_count = static_cast<u32>(key_times.size());
  track.first_value = values.size();
  times.append(key_times);
  values.append(key_values);
  tracks.push_back(track);
  const f32 last = key_times[key_times.size() - 1];
  if (last > duration) duration = last;
  if (joint >= joint_count) joint_count = joint + 1;
  return true;
}

void Clip::sample(f32 time, Pose& out, bool loop) const {
  if (out.joint_count() == 0) return;
  f32 at = time;
  if (loop && duration > 0.0f) {
    at = std::fmod(time, duration);
    if (at < 0.0f) at += duration;
  } else {
    at = time < 0.0f ? 0.0f : (time > duration ? duration : time);
  }

  const u32 joints = out.joint_count();
  for (const Track& track : tracks) {
    if (track.joint >= joints || track.key_count == 0) continue;
    const std::span<const f32> key_times(times.data() + track.first_key, track.key_count);
    const KeyPair pair = locate(key_times, at);
    const u32 components = track.components;
    const u32 per_key = track.interpolation == k_interp_cubic ? 3u : 1u;
    const f32* run = values.data() + track.first_value;
    const f32* current = run + static_cast<usize>(pair.key) * per_key * components;
    // A CUBICSPLINE key is in-tangent, value, out-tangent; the others are just the value.
    const f32* value = track.interpolation == k_interp_cubic ? current + components : current;

    if (track.interpolation == k_interp_step || pair.fraction <= 0.0f ||
        pair.key + 1 >= track.key_count) {
      if (track.channel == k_channel_rotation) {
        out.rotation[track.joint] = normalize(quat_at(value));
      } else if (track.channel == k_channel_scale) {
        out.scale[track.joint] = vec3_at(value);
      } else {
        out.translation[track.joint] = vec3_at(value);
      }
      continue;
    }

    const f32* next = run + static_cast<usize>(pair.key + 1) * per_key * components;
    const f32* next_value = track.interpolation == k_interp_cubic ? next + components : next;
    if (track.interpolation == k_interp_cubic) {
      const f32 span = key_times[pair.key + 1] - key_times[pair.key];
      const f32* out_tangent = value + components;  // this key's out-tangent
      const f32* in_tangent = next;                 // the next key's in-tangent
      f32 result[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      for (u32 c = 0; c < components; ++c) {
        result[c] =
            hermite(value[c], out_tangent[c], next_value[c], in_tangent[c], pair.fraction, span);
      }
      if (track.channel == k_channel_rotation) {
        out.rotation[track.joint] = normalize(quat_at(result));
      } else if (track.channel == k_channel_scale) {
        out.scale[track.joint] = vec3_at(result);
      } else {
        out.translation[track.joint] = vec3_at(result);
      }
      continue;
    }

    if (track.channel == k_channel_rotation) {
      // slerp on the short arc, not a component lerp: the two keys of a quarter turn are 45
      // degrees apart in the middle either way, but a lerp gets there at the wrong speed.
      out.rotation[track.joint] =
          slerp(normalize(quat_at(value)), normalize(quat_at(next_value)), pair.fraction);
    } else if (track.channel == k_channel_scale) {
      const Vec3 a = vec3_at(value);
      out.scale[track.joint] = a + (vec3_at(next_value) - a) * pair.fraction;
    } else {
      const Vec3 a = vec3_at(value);
      out.translation[track.joint] = a + (vec3_at(next_value) - a) * pair.fraction;
    }
  }
}

void Clip::sample(GameTime time, Pose& out, bool loop) const {
  sample(static_cast<f32>(static_cast<f64>(time.us) * 1.0e-6), out, loop);
}

bool Clip::validate(std::string* error) const {
  for (u32 t = 0; t < tracks.size(); ++t) {
    const Track& track = tracks[t];
    const std::string where = "clip track " + std::to_string(t);
    if (track.components != 3 && track.components != 4)
      return fail(error, where + " has " + std::to_string(track.components) + " components");
    if (track.channel == k_channel_rotation && track.components != 4)
      return fail(error, where + " drives a rotation with " + std::to_string(track.components) +
                             " components");
    if (track.key_count == 0) return fail(error, where + " has no keys");
    if (u64{track.first_key} + track.key_count > times.size())
      return fail(error, where + "'s keyframe times run past the end of the clip");
    const u32 per_key = track.interpolation == k_interp_cubic ? 3u : 1u;
    const u64 value_count = u64{track.key_count} * per_key * track.components;
    if (u64{track.first_value} + value_count > values.size())
      return fail(error, where + "'s values run past the end of the clip");
    if (track.joint >= joint_count)
      return fail(error, where + " names joint " + std::to_string(track.joint) + ", outside the " +
                             std::to_string(joint_count) + " the clip was authored against");
    for (u32 k = 1; k < track.key_count; ++k) {
      if (times[track.first_key + k] < times[track.first_key + k - 1])
        return fail(error, where + "'s keyframe times are not in order");
    }
    if (times[track.first_key + track.key_count - 1] > duration)
      return fail(error, where + " runs past the clip's duration");
  }
  return true;
}

}  // namespace engine::anim

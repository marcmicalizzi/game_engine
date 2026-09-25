#include <core/base/macros.h>
#include <domain/audio/decoder.h>

namespace engine::audio {

namespace {

// ---- the stereo panner --------------------------------------------------------------------------
//
// State, per source: four gains — source channel s to target t at [s * 2 + t] — twice over, where
// the current ramp started and where it is going, and how far into the ramp the gains are. The two
// targets are the layout's front pair (for a `Direct` mapping, channels 0 and 1, which every table
// layout wider than mono puts at the front pair too, so a voice changing mapping keeps ramping from
// the right place).
constexpr u32 k_gains = 4;
constexpr u32 k_start = 0;    // [0, 4): the gains the ramp started from
constexpr u32 k_target = 4;   // [4, 8): the gains it is heading for, and holds once there
constexpr u32 k_elapsed = 8;  // frames of the ramp done, as an f32 (exact: a ramp is < 2^24 frames)
constexpr u32 k_panner_state = 9;

bool panner_supports(ChannelLayout layout) noexcept {
  return layout != ChannelLayout::Unknown && layout_info(layout).front_left >= 0;
}

// ---- the fixed-time ramp ------------------------------------------------------------------------
//
// A gain change is a linear ramp of `ramp` frames from where the gain was to where it is going:
// ramp frame k (1-based) is at `start + (target - start) / ramp * k`, and frame `ramp` and every
// frame after it is at `target` exactly. The ramp's progress is kept per source, so a ramp longer
// than a block continues in the next one and a ramp shorter than a block ends inside it — the
// gain on any frame is a function of how many frames ago the change arrived and nothing else, and
// the block size, which is the device's period, does not appear (docs/subsystems/audio.md,
// "Parameter changes ramp in fixed time"). The per-frame step is computed once per block from the
// same three numbers every time, so a ramp split across two blocks computes the same gains as the
// same ramp inside one.

// The gain `elapsed` frames into a ramp: what the last frame mixed carried, and where a new ramp
// starts when the target moves before the old one arrived.
ENGINE_FORCE_INLINE f32 ramp_gain(f32 start, f32 target, u32 elapsed, u32 ramp) noexcept {
  if (elapsed >= ramp) return target;
  return start + (target - start) / static_cast<f32>(ramp) * static_cast<f32>(elapsed);
}

// One source channel into two layout channels at constant gains: the plain multiply-add, which is
// every block of a voice with no change in flight. `stride` is the source's channel count and
// `channels` the layout's; neither changes the loop's shape, so they are not template parameters
// (plan 11 §11.4).
ENGINE_FORCE_INLINE void add_constant2(const f32* ENGINE_RESTRICT signal, u32 stride, u32 first,
                                       u32 last, f32* ENGINE_RESTRICT out, u32 channels, u32 a,
                                       u32 b, f32 ga, f32 gb) noexcept {
  for (u32 i = first; i < last; ++i) {
    const f32 x = signal[i * stride];
    out[i * channels + a] += x * ga;
    out[i * channels + b] += x * gb;
  }
}

ENGINE_FORCE_INLINE void add_constant1(const f32* ENGINE_RESTRICT signal, u32 stride, u32 first,
                                       u32 last, f32* ENGINE_RESTRICT out, u32 channels, u32 a,
                                       f32 ga) noexcept {
  for (u32 i = first; i < last; ++i)
    out[i * channels + a] += signal[i * stride] * ga;
}

// The ramp's frames of a block: block frames [0, count) are ramp frames k0 + 1 .. k0 + count, each
// at start + step * k.
ENGINE_FORCE_INLINE void add_ramped2(const f32* ENGINE_RESTRICT signal, u32 stride, u32 count,
                                     u32 k0, f32* ENGINE_RESTRICT out, u32 channels, u32 a, u32 b,
                                     f32 sa, f32 da, f32 sb, f32 db) noexcept {
  for (u32 i = 0; i < count; ++i) {
    const f32 x = signal[i * stride];
    const f32 k = static_cast<f32>(k0 + i + 1u);
    out[i * channels + a] += x * (sa + da * k);
    out[i * channels + b] += x * (sb + db * k);
  }
}

ENGINE_FORCE_INLINE void add_ramped1(const f32* ENGINE_RESTRICT signal, u32 stride, u32 count,
                                     u32 k0, f32* ENGINE_RESTRICT out, u32 channels, u32 a, f32 sa,
                                     f32 da) noexcept {
  for (u32 i = 0; i < count; ++i) {
    const f32 k = static_cast<f32>(k0 + i + 1u);
    out[i * channels + a] += signal[i * stride] * (sa + da * k);
  }
}

void panner_decode(const DecodeInput& in, const LayoutInfo& layout, f32* state, f32* out) noexcept {
  const u32 channels = layout.channels;
  const bool fold = channels == 1;
  u32 a = static_cast<u32>(layout.front_left);
  u32 b = static_cast<u32>(layout.front_right);
  const bool direct = in.two_d && in.mapping == ChannelMapping::Direct;
  if (direct && !fold) {
    a = 0;
    b = 1;
  }

  // Targets: source channel s to (a, b) at t[s * 2 + 0], t[s * 2 + 1].
  f32 t[k_gains] = {0.0f, 0.0f, 0.0f, 0.0f};
  const f32 pan = in.two_d ? in.pan : in.spatial.direction.x * (1.0f - in.spatial.spread);
  if (in.channels == 1) {
    if (fold) {
      t[0] = in.gain;  // the one speaker gets the source at its level
    } else if (direct) {
      t[0] = in.gain;  // source channel 0 to layout channel 0
    } else {
      const PanGains g = pan_constant_power(pan);
      t[0] = in.gain * g.left;
      t[1] = in.gain * g.right;
    }
  } else {
    if (fold) {
      t[0] = in.gain * 0.5f;
      t[3] = in.gain * 0.5f;
    } else if (direct) {
      t[0] = in.gain;
      t[3] = in.gain;
    } else {
      const PanGains g = pan_balance(pan);
      t[0] = in.gain * g.left;
      t[3] = in.gain * g.right;
    }
  }

  // Where the gains are, and whether this block starts a ramp. A fresh voice starts at its targets
  // (docs page: ramping a new voice up would smear its attack); a target that moved starts a new
  // ramp from wherever the old one had got to, so the gain never jumps.
  f32* start = state + k_start;
  f32* target = state + k_target;
  const u32 ramp = in.ramp_frames != 0 ? in.ramp_frames : 1u;
  u32 elapsed = static_cast<u32>(state[k_elapsed]);
  if (in.fresh) {
    for (u32 g = 0; g < k_gains; ++g) {
      start[g] = t[g];
      target[g] = t[g];
    }
    elapsed = ramp;
  } else if (t[0] != target[0] || t[1] != target[1] || t[2] != target[2] || t[3] != target[3]) {
    for (u32 g = 0; g < k_gains; ++g) {
      start[g] = ramp_gain(start[g], target[g], elapsed, ramp);
      target[g] = t[g];
    }
    elapsed = 0;
  }

  // Frames of this block still on the ramp: ramp frames elapsed + 1 .. ramp - 1. Frame `ramp` is
  // the target itself, and so is everything after it.
  const u32 frames = in.frames;
  const u32 left = elapsed + 1u < ramp ? ramp - 1u - elapsed : 0u;
  const u32 ramped = left < frames ? left : frames;
  const u32 k0 = elapsed;
  state[k_elapsed] = static_cast<f32>(elapsed + frames < ramp ? elapsed + frames : ramp);

  // A source at rest at zero adds nothing: skipping it is bit-identical (x * 0 added to a sum
  // leaves it unchanged) and is what a voice past its max_distance, or on a muted bus, costs.
  if (ramped == 0 && target[0] == 0.0f && target[1] == 0.0f && target[2] == 0.0f &&
      target[3] == 0.0f) {
    return;
  }

  const f32 r = static_cast<f32>(ramp);
  if (in.channels == 1) {
    if (fold || direct) {
      if (ramped != 0) {
        add_ramped1(in.signal, 1, ramped, k0, out, channels, a, start[0],
                    (target[0] - start[0]) / r);
      }
      add_constant1(in.signal, 1, ramped, frames, out, channels, a, target[0]);
    } else {
      if (ramped != 0) {
        add_ramped2(in.signal, 1, ramped, k0, out, channels, a, b, start[0],
                    (target[0] - start[0]) / r, start[1], (target[1] - start[1]) / r);
      }
      add_constant2(in.signal, 1, ramped, frames, out, channels, a, b, target[0], target[1]);
    }
  } else {
    // Balance, direct and the fold are all diagonal: left to a, right to b (the same channel when
    // folding to mono).
    if (ramped != 0) {
      add_ramped1(in.signal, 2, ramped, k0, out, channels, a, start[0], (target[0] - start[0]) / r);
      add_ramped1(in.signal + 1, 2, ramped, k0, out, channels, b, start[3],
                  (target[3] - start[3]) / r);
    }
    add_constant1(in.signal, 2, ramped, frames, out, channels, a, target[0]);
    add_constant1(in.signal + 1, 2, ramped, frames, out, channels, b, target[3]);
  }
}

// ---- the null decoder ---------------------------------------------------------------------------

bool null_supports(ChannelLayout layout) noexcept { return layout != ChannelLayout::Unknown; }

void null_decode(const DecodeInput&, const LayoutInfo&, f32*, f32*) noexcept {}

}  // namespace

const Decoder k_stereo_panner{"stereo_panner", k_panner_state, &panner_supports, &panner_decode};
const Decoder k_null_decoder{"null", 0, &null_supports, &null_decode};

}  // namespace engine::audio

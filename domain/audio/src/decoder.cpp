#include <core/base/macros.h>
#include <domain/audio/decoder.h>

namespace engine::audio {

namespace {

// ---- the stereo panner --------------------------------------------------------------------------
//
// State, per source: the gains its last block ended on, source channel s to target t, at
// [s * 2 + t] — four floats. The two targets are the layout's front pair (for a `Direct` mapping,
// channels 0 and 1, which every table layout wider than mono puts at the front pair too, so a
// voice changing mapping keeps ramping from the right place).
constexpr u32 k_panner_state = 4;

bool panner_supports(ChannelLayout layout) noexcept {
  return layout != ChannelLayout::Unknown && layout_info(layout).front_left >= 0;
}

// One source channel into two layout channels, ramping each gain linearly from `a0`/`b0` to
// `a1`/`b1` across the block: frame i at g0 + (g1 - g0) / n * (i + 1). `stride` is the source's
// channel count and `channels` the layout's; neither changes the loop's shape, so they are not
// template parameters (plan 11 §11.4).
ENGINE_FORCE_INLINE void add_ramped2(const f32* ENGINE_RESTRICT signal, u32 stride, u32 frames,
                                     f32* ENGINE_RESTRICT out, u32 channels, u32 a, u32 b, f32 a0,
                                     f32 a1, f32 b0, f32 b1) noexcept {
  const f32 n = static_cast<f32>(frames);
  const f32 da = (a1 - a0) / n;
  const f32 db = (b1 - b0) / n;
  for (u32 i = 0; i < frames; ++i) {
    const f32 x = signal[i * stride];
    const f32 k = static_cast<f32>(i + 1u);
    out[i * channels + a] += x * (a0 + da * k);
    out[i * channels + b] += x * (b0 + db * k);
  }
}

// The same into one layout channel.
ENGINE_FORCE_INLINE void add_ramped1(const f32* ENGINE_RESTRICT signal, u32 stride, u32 frames,
                                     f32* ENGINE_RESTRICT out, u32 channels, u32 a, f32 a0,
                                     f32 a1) noexcept {
  const f32 n = static_cast<f32>(frames);
  const f32 da = (a1 - a0) / n;
  for (u32 i = 0; i < frames; ++i) {
    const f32 k = static_cast<f32>(i + 1u);
    out[i * channels + a] += signal[i * stride] * (a0 + da * k);
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
  f32 t[k_panner_state] = {0.0f, 0.0f, 0.0f, 0.0f};
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

  f32 from[k_panner_state];
  for (u32 i = 0; i < k_panner_state; ++i)
    from[i] = in.fresh ? t[i] : state[i];

  if (in.channels == 1) {
    if (fold || direct) {
      add_ramped1(in.signal, 1, in.frames, out, channels, a, from[0], t[0]);
    } else {
      add_ramped2(in.signal, 1, in.frames, out, channels, a, b, from[0], t[0], from[1], t[1]);
    }
  } else {
    // Balance, direct and the fold are all diagonal: left to a, right to b (the same channel when
    // folding to mono).
    add_ramped1(in.signal, 2, in.frames, out, channels, a, from[0], t[0]);
    add_ramped1(in.signal + 1, 2, in.frames, out, channels, b, from[3], t[3]);
  }
  for (u32 i = 0; i < k_panner_state; ++i)
    state[i] = t[i];
}

// ---- the null decoder ---------------------------------------------------------------------------

bool null_supports(ChannelLayout layout) noexcept { return layout != ChannelLayout::Unknown; }

void null_decode(const DecodeInput&, const LayoutInfo&, f32*, f32*) noexcept {}

}  // namespace

const Decoder k_stereo_panner{"stereo_panner", k_panner_state, &panner_supports, &panner_decode};
const Decoder k_null_decoder{"null", 0, &null_supports, &null_decode};

}  // namespace engine::audio

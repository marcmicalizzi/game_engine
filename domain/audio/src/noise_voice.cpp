// The shaped-noise voice (noise_voice.h; docs/subsystems/audio.md, "The shaped-noise voice").

#include <core/base/macros.h>
#include <domain/audio/format.h>
#include <domain/audio/noise_voice.h>
#include <domain/audio/spatial.h>

#include <cmath>

namespace engine::audio {

namespace {

constexpr f32 k_two_to_minus_31 = 4.656612873077392578125e-10f;  // exact: a power of two

// One frame of every lane. `Ramp` moves the gains by their step after the frame; without it the
// gains stand. Lanes are independent and their sum is taken in lane order, so a compiler that
// vectorizes the lanes computes the same bits as one that does not.
template <bool Ramp>
ENGINE_FORCE_INLINE void noise_run(NoiseState& n, f32* ENGINE_RESTRICT out, u32 count) noexcept {
  u32 rng = n.rng;
  for (u32 i = 0; i < count; ++i) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    const f32 x = static_cast<f32>(static_cast<i32>(rng)) * k_two_to_minus_31;
    f32 y[k_noise_lanes];
    for (u32 k = 0; k < k_noise_lanes; ++k) {
      const f32 cx = n.c[k] * x;
      y[k] = cx + n.s1[k];
      n.s1[k] = n.s2[k] - n.a1[k] * y[k];
      n.s2[k] = -cx - n.a2[k] * y[k];
    }
    f32 sum = 0.0f;
    for (u32 k = 0; k < k_noise_lanes; ++k)
      sum += n.gain[k] * y[k];
    out[i] = sum;
    if constexpr (Ramp) {
      for (u32 k = 0; k < k_noise_lanes; ++k)
        n.gain[k] += n.step[k];
    }
  }
  n.rng = rng;
}

}  // namespace

NoiseBand noise_band(f32 centre_hz, f32 q) noexcept {
  f32 f = std::isfinite(centre_hz) ? centre_hz : k_noise_tone_min_hz;
  f = f < k_noise_tone_min_hz ? k_noise_tone_min_hz : (f > k_noise_tone_max_hz ? k_noise_tone_max_hz : f);
  f32 qq = std::isfinite(q) ? q : k_noise_band_q;
  qq = qq < 0.5f ? 0.5f : (qq > 50.0f ? 50.0f : qq);
  // w = 2 pi f / fs as quarter turns, t = w / (pi / 2) = 4 f / fs, in [0, 1] below 12 kHz.
  const f32 t = 4.0f * f / static_cast<f32>(k_sample_rate);
  const f32 sin_w = sin_quarter(t);
  const f32 cos_w = sin_quarter(1.0f - t);
  const f32 alpha = sin_w / (2.0f * qq);
  const f32 a0 = 1.0f + alpha;
  NoiseBand out;
  const f32 b0 = alpha / a0;
  out.c = std::sqrt(3.0f * b0);
  out.a1 = -2.0f * cos_w / a0;
  out.a2 = (1.0f - alpha) / a0;
  return out;
}

void render_noise(NoiseState& n, f32* out, u32 frames) noexcept {
  u32 done = 0;
  if (n.ramp != 0) {
    const u32 run = frames < n.ramp ? frames : n.ramp;
    noise_run<true>(n, out, run);
    n.ramp -= run;
    done = run;
    if (n.ramp == 0) {
      // The ramp lands exactly on its targets, whatever its steps' rounding left.
      for (u32 k = 0; k < k_noise_lanes; ++k) {
        n.gain[k] = n.target[k];
        n.step[k] = 0.0f;
      }
    }
  }
  if (done < frames) noise_run<false>(n, out + done, frames - done);
}

}  // namespace engine::audio

// The shaped-noise voice (noise_voice.h; docs/subsystems/audio.md, "The shaped-noise voice").

#include <core/base/macros.h>
#include <domain/audio/format.h>
#include <domain/audio/noise_voice.h>
#include <domain/audio/spatial.h>

#include <cmath>
#include <immintrin.h>

namespace engine::audio {

namespace {

constexpr f32 k_two_to_minus_31 = 4.656612873077392578125e-10f;  // exact: a power of two

// The eight fixed bands as two SSE registers of four lanes, the movable band as a scalar beside
// them: SSE is in both CPU baselines (x86-64-v2 and v3, ADR-0031), so one path serves both. Every
// lane does the same IEEE operations a scalar loop would (a multiply, then an add or a subtract:
// no fused multiply-add, ADR-0035), and the lanes' sum is a fixed tree — (b0 + b4) + (b2 + b6) and
// (b1 + b5) + (b3 + b7), then the two, then the movable band — so every toolchain makes the same
// bits. Measured 2026-11-21 against the scalar loop it replaced: 10.2 to about 3 us a voice a
// 10 ms block (docs/subsystems/audio.md, "Performance notes").
struct Lanes {
  __m128 v[2];
};

ENGINE_FORCE_INLINE Lanes load_lanes(const f32* p) noexcept {
  return Lanes{{_mm_loadu_ps(p), _mm_loadu_ps(p + 4)}};
}
ENGINE_FORCE_INLINE void store_lanes(f32* p, const Lanes& l) noexcept {
  _mm_storeu_ps(p, l.v[0]);
  _mm_storeu_ps(p + 4, l.v[1]);
}

template <bool Ramp>
ENGINE_FORCE_INLINE void noise_run(NoiseState& n, f32* ENGINE_RESTRICT out, u32 count) noexcept {
  u32 rng = n.rng;
  Lanes s1 = load_lanes(n.s1);
  Lanes s2 = load_lanes(n.s2);
  const Lanes c = load_lanes(n.c);
  const Lanes a1 = load_lanes(n.a1);
  const Lanes a2 = load_lanes(n.a2);
  Lanes g = load_lanes(n.gain);
  const Lanes step = load_lanes(n.step);
  constexpr u32 t = k_noise_tone;
  f32 ts1 = n.s1[t];
  f32 ts2 = n.s2[t];
  const f32 tc = n.c[t];
  const f32 ta1 = n.a1[t];
  const f32 ta2 = n.a2[t];
  f32 tg = n.gain[t];
  const f32 tstep = n.step[t];
  const __m128 zero = _mm_setzero_ps();
  for (u32 i = 0; i < count; ++i) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    const f32 x = static_cast<f32>(static_cast<i32>(rng)) * k_two_to_minus_31;
    const __m128 xv = _mm_set1_ps(x);
    __m128 p[2];
    for (u32 h = 0; h < 2; ++h) {
      const __m128 cx = _mm_mul_ps(c.v[h], xv);
      const __m128 y = _mm_add_ps(cx, s1.v[h]);
      s1.v[h] = _mm_sub_ps(s2.v[h], _mm_mul_ps(a1.v[h], y));
      s2.v[h] = _mm_sub_ps(_mm_sub_ps(zero, cx), _mm_mul_ps(a2.v[h], y));
      p[h] = _mm_mul_ps(g.v[h], y);
    }
    // (b0 + b4, b1 + b5, b2 + b6, b3 + b7), then (.. + b2b6, .. + b3b7), then the two.
    const __m128 q = _mm_add_ps(p[0], p[1]);
    const __m128 r = _mm_add_ps(q, _mm_movehl_ps(q, q));
    const f32 bands = _mm_cvtss_f32(_mm_add_ss(r, _mm_shuffle_ps(r, r, 1)));
    const f32 tcx = tc * x;
    const f32 ty = tcx + ts1;
    ts1 = ts2 - ta1 * ty;
    ts2 = -tcx - ta2 * ty;
    out[i] = bands + tg * ty;
    if constexpr (Ramp) {
      g.v[0] = _mm_add_ps(g.v[0], step.v[0]);
      g.v[1] = _mm_add_ps(g.v[1], step.v[1]);
      tg += tstep;
    }
  }
  store_lanes(n.s1, s1);
  store_lanes(n.s2, s2);
  n.s1[t] = ts1;
  n.s2[t] = ts2;
  if constexpr (Ramp) {
    store_lanes(n.gain, g);
    n.gain[t] = tg;
  }
  n.rng = rng;
}

}  // namespace

NoiseBand noise_band(f32 centre_hz, f32 q) noexcept {
  f32 f = std::isfinite(centre_hz) ? centre_hz : k_noise_tone_min_hz;
  f = f < k_noise_tone_min_hz ? k_noise_tone_min_hz
                              : (f > k_noise_tone_max_hz ? k_noise_tone_max_hz : f);
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

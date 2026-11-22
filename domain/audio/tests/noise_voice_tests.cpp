// The shaped-noise voice (noise_voice.h; docs/subsystems/audio.md, "The shaped-noise voice";
// roadmap S112): the bands' literals against their derivation, each band's unit RMS, the movable
// band's coefficients, the ramp landing on its targets, the voice in the mixer at its gain, and the
// scripted session's bytes pinned. The pin was taken on MSVC; GCC and Clang (the Linux container)
// must reproduce it.

#include <domain/audio/audio.h>
#include <domain/audio/mixer.h>
#include <domain/audio/noise_voice.h>

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

using namespace engine;
using namespace engine::audio;

namespace {

// A band's squared impulse response, summed: c^2 / b0^2 times the unscaled filter's b0, which is 3.
f64 impulse_power(const NoiseBand& b) {
  f64 s1 = 0.0;
  f64 s2 = 0.0;
  f64 sum = 0.0;
  for (u32 n = 0; n < 400000; ++n) {
    const f64 x = n == 0 ? 1.0 : 0.0;
    const f64 cx = static_cast<f64>(b.c) * x;
    const f64 y = cx + s1;
    s1 = s2 - static_cast<f64>(b.a1) * y;
    s2 = -cx - static_cast<f64>(b.a2) * y;
    sum += y * y;
  }
  return sum;
}

NoiseState one_band(u32 band, f32 gain, u32 seed) {
  NoiseState n;
  for (u32 k = 0; k < k_noise_bands; ++k) {
    n.c[k] = k_noise_band_table[k].c;
    n.a1[k] = k_noise_band_table[k].a1;
    n.a2[k] = k_noise_band_table[k].a2;
  }
  n.gain[band] = gain;
  n.target[band] = gain;
  n.rng = noise_seed(seed);
  return n;
}

f64 rms(const std::vector<f32>& x, usize from = 0) {
  f64 sum = 0.0;
  for (usize i = from; i < x.size(); ++i)
    sum += static_cast<f64>(x[i]) * static_cast<f64>(x[i]);
  return std::sqrt(sum / static_cast<f64>(x.size() - from));
}

}  // namespace

TEST_CASE("noise voice: the fixed bands' literals are their derivation, each passing unit RMS") {
  for (u32 k = 0; k < k_noise_bands; ++k) {
    // The cookbook's band-pass in double, at the band's centre and Q.
    const f64 w = 2.0 * 3.14159265358979323846 * static_cast<f64>(k_noise_band_hz[k]) / 48000.0;
    const f64 alpha = std::sin(w) / (2.0 * std::sqrt(2.0));
    const f64 a0 = 1.0 + alpha;
    const NoiseBand& b = k_noise_band_table[k];
    CHECK(static_cast<f64>(b.c) == doctest::Approx(std::sqrt(3.0 * alpha / a0)).epsilon(1e-6));
    CHECK(static_cast<f64>(b.a1) == doctest::Approx(-2.0 * std::cos(w) / a0).epsilon(1e-6));
    CHECK(static_cast<f64>(b.a2) == doctest::Approx((1.0 - alpha) / a0).epsilon(1e-6));
    // Its squared impulse response sums to 3, so the generator's noise (variance 1/3) leaves it at
    // an RMS of one.
    CHECK(impulse_power(b) == doctest::Approx(3.0).epsilon(1e-4));
    // The movable band's own computation lands on the same filter at the same centre.
    const NoiseBand m = noise_band(k_noise_band_hz[k], k_noise_band_q);
    CHECK(m.c == doctest::Approx(b.c).epsilon(1e-4));
    CHECK(m.a1 == doctest::Approx(b.a1).epsilon(1e-4));
    CHECK(m.a2 == doctest::Approx(b.a2).epsilon(1e-4));
  }
  // Measured on the generator: the 1 kHz and 4 kHz bands at a gain of 0.25 for two seconds (bands
  // a few hundred hertz wide: a few percent of spread).
  for (const u32 band : {4u, 6u}) {
    NoiseState n = one_band(band, 0.25f, 7u);
    std::vector<f32> out(96000);
    render_noise(n, out.data(), static_cast<u32>(out.size()));
    CHECK(rms(out, 4800) == doctest::Approx(0.25).epsilon(0.05));
  }
}

TEST_CASE("noise voice: the movable band is held to its range, and a ramp lands on its targets") {
  const NoiseBand low = noise_band(1.0f, 2.0f);
  const NoiseBand floor = noise_band(k_noise_tone_min_hz, 2.0f);
  CHECK(low.c == floor.c);
  CHECK(low.a1 == floor.a1);
  const NoiseBand nan = noise_band(NAN, NAN);
  CHECK(std::isfinite(nan.c));
  CHECK(std::isfinite(nan.a1));

  NoiseState n = one_band(3, 0.0f, 3u);
  n.target[3] = 0.5f;
  n.step[3] = 0.5f / 2400.0f;
  n.ramp = 2400;
  std::vector<f32> out(1000);
  render_noise(n, out.data(), 1000);  // part of the way
  CHECK(n.ramp == 1400u);
  CHECK(n.gain[3] > 0.15f);
  CHECK(n.gain[3] < 0.25f);
  render_noise(n, out.data(), 1000);
  render_noise(n, out.data(), 1000);  // past the end: exactly the target, the step cleared
  CHECK(n.ramp == 0u);
  CHECK(n.gain[3] == 0.5f);
  CHECK(n.step[3] == 0.0f);
}

namespace {

// A scripted session of shaped-noise voices: two 3D voices either side of the listener, a 2D one,
// a set that ramps, a movable band that moves, a stop; rendered in the calls given.
std::vector<f32> noise_session(std::initializer_list<u32> calls, u64* plays = nullptr) {
  ClipStore clips;
  MixerConfig config;
  config.voices = 8;
  config.ramp_frames = 480;
  config.field_ramp_frames = 2400;
  config.limiter = LimiterMode::Off;
  Mixer mixer(clips, config);
  NoisePlayParams a;
  a.noise.seed = 11;
  for (u32 k = 0; k < k_noise_bands; ++k)
    a.noise.gain[k] = 0.02f * static_cast<f32>(k + 1);
  a.source.position = Vec3{-5.0f, 1.0f, -3.0f};
  a.source.distance_model = DistanceModel::None;
  NoisePlayParams b = a;
  b.noise.seed = 12;
  b.noise.gain[k_noise_tone] = 0.1f;
  b.noise.tone = noise_band(1700.0f, 6.0f);
  b.source.position = Vec3{6.0f, 0.0f, -1.0f};
  NoisePlayParams c = a;
  c.noise.seed = 13;
  c.source.flags = k_source_2d;
  c.gain = 0.5f;
  const VoiceHandle va = mixer.play_noise(a);
  const VoiceHandle vb = mixer.play_noise(b);
  const VoiceHandle vc = mixer.play_noise(c);
  REQUIRE(!va.is_null());
  REQUIRE(!vb.is_null());
  REQUIRE(!vc.is_null());
  std::vector<f32> all;
  std::vector<f32> block(4800 * 2);
  for (u32 tick = 0; tick < 12; ++tick) {
    if (tick == 3) {
      NoiseParams p = a.noise;
      for (f32& g : p.gain)
        g *= 2.0f;
      CHECK(mixer.set_noise(va, p));
    }
    if (tick == 5) {
      NoiseParams p = b.noise;
      p.tone = noise_band(2600.0f, 6.0f);
      CHECK(mixer.set_noise(vb, p));
    }
    if (tick == 8) CHECK(mixer.stop(vc));
    // Each tick is 4,800 frames, rendered in the calls given.
    u32 done = 0;
    while (done < 4800) {
      for (const u32 call : calls) {
        const u32 n = call < 4800 - done ? call : 4800 - done;
        mixer.render(block.data(), n);
        all.insert(all.end(), block.begin(), block.begin() + n * 2);
        done += n;
        if (done == 4800) break;
      }
    }
    mixer.update();
  }
  if (plays != nullptr) *plays = mixer.control_stats().noise_plays;
  CHECK_FALSE(mixer.set_noise(vc, c.noise));  // stopped and ended
  return all;
}

}  // namespace

TEST_CASE("noise voice: a scripted session is the same bytes however the frames are cut, pinned") {
  u64 plays = 0;
  const std::vector<f32> whole = noise_session({480}, &plays);
  CHECK(plays == 3u);
  const std::vector<f32> uneven = noise_session({7, 473, 160, 1024});
  REQUIRE(whole.size() == 12u * 4800u * 2u);
  REQUIRE(uneven.size() == whole.size());
  CHECK(whole == uneven);
  // Heard: the first second's level is the voices' bands, and the last ticks, with the 2D voice
  // stopped, are quieter than the ticks before the stop.
  f64 before = 0.0;
  f64 after = 0.0;
  for (usize i = 6u * 9600u; i < 8u * 9600u; ++i)
    before += static_cast<f64>(whole[i]) * static_cast<f64>(whole[i]);
  for (usize i = 10u * 9600u; i < 12u * 9600u; ++i)
    after += static_cast<f64>(whole[i]) * static_cast<f64>(whole[i]);
  CHECK(after < before);
  CHECK(after > 0.0);
  const u64 hash =
      hash_mix(ChannelLayout::Stereo, std::span<const f32>(whole.data(), whole.size()));
  MESSAGE("noise session hash: " << hash);
  CHECK(hash == 0x4a2e2b356bc6e533ull);  // pinned on MSVC 2026-11-21; GCC and Clang reproduce it
}

TEST_CASE("noise voice: in the mixer a 2D voice's power is its gain times its band's") {
  ClipStore clips;
  MixerConfig config;
  config.voices = 4;
  Mixer mixer(clips, config);
  NoisePlayParams p;
  p.noise.seed = 5;
  p.noise.gain[4] = 0.2f;  // 1 kHz
  p.gain = 0.5f;
  p.source.flags = k_source_2d;
  const VoiceHandle v = mixer.play_noise(p);
  REQUIRE(!v.is_null());
  std::vector<f32> out(96000 * 2);
  mixer.render(out.data(), 96000);
  // The panner's centre is constant power: left^2 + right^2 is the voice's signal^2.
  f64 sum = 0.0;
  for (usize f = 4800; f < 96000; ++f)
    sum += static_cast<f64>(out[2 * f]) * out[2 * f] +
           static_cast<f64>(out[2 * f + 1]) * out[2 * f + 1];
  CHECK(std::sqrt(sum / 91200.0) == doctest::Approx(0.1).epsilon(0.05));
  // Refusals: a gain that is not a number, a negative band.
  NoisePlayParams bad = p;
  bad.noise.gain[2] = -1.0f;
  CHECK(mixer.play_noise(bad).is_null());
  bad = p;
  bad.noise.tone.a1 = NAN;
  CHECK(mixer.play_noise(bad).is_null());
  CHECK(mixer.control_stats().refused_params == 2u);
  // A SetNoise naming a clip voice's slot is stale on the audio thread, never applied.
  CHECK(mixer.control_stats().noise_plays == 1u);
}

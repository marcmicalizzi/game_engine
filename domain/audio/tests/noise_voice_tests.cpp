// The shaped-noise voice (noise_voice.h; docs/subsystems/audio.md, "The shaped-noise voice";
// roadmap S112): the bands' literals against their derivation, each band's unit RMS, the movable
// band's coefficients, the ramp landing on its targets, the voice in the mixer at its gain, and the
// scripted session's bytes pinned. The pin was taken on MSVC; GCC and Clang (the Linux container)
// must reproduce it.

#include <domain/audio/audio.h>
#include <domain/audio/mixer.h>
#include <domain/audio/noise_voice.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
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
  // Pinned on MSVC 2026-11-22 (S112-2: a fresh voice ramps up from nothing, so the session's first
  // ticks moved; it was 0x4a2e2b356bc6e533); GCC and Clang must reproduce it.
  CHECK(hash == 0x39115f278d92b677ull);
}

// M2 of S112's review: a fresh noise voice took its gains at once, a step from silence to
// broadband noise at every start (a click). It ramps from nothing over the field's ramp now.
TEST_CASE("noise voice: a fresh voice ramps up from nothing, with no step at its start") {
  ClipStore clips;
  MixerConfig config;
  config.voices = 4;
  Mixer mixer(clips, config);
  NoisePlayParams p;
  p.noise.seed = 9;
  for (u32 k = 0; k < k_noise_bands; ++k)
    p.noise.gain[k] = 0.1f;
  p.source.flags = k_source_2d;
  REQUIRE(!mixer.play_noise(p).is_null());
  std::vector<f32> out(9600 * 2);
  mixer.render(out.data(), 9600);
  const auto power = [&](u32 from, u32 to) {
    f64 s = 0.0;
    for (u32 f = from; f < to; ++f)
      s += static_cast<f64>(out[2 * f]) * static_cast<f64>(out[2 * f]) +
           static_cast<f64>(out[2 * f + 1]) * static_cast<f64>(out[2 * f + 1]);
    return s / static_cast<f64>(to - from);
  };
  // The first 24 frames (0.5 ms) carry well under a hundredth of the settled power, and the level
  // rises through the ramp rather than standing at its end from the first frame.
  const f64 settled = power(4800, 9600);
  REQUIRE(settled > 0.0);
  CHECK(power(0, 24) < 0.01 * settled);
  CHECK(power(0, 120) < power(120, 480));
  CHECK(power(4800, 9600) == doctest::Approx(settled));
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
    sum += static_cast<f64>(out[2 * f]) * static_cast<f64>(out[2 * f]) +
           static_cast<f64>(out[2 * f + 1]) * static_cast<f64>(out[2 * f + 1]);
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

// ---- the struck resonators (S113) ---------------------------------------------------------------

TEST_CASE("noise voice: a struck resonator rings at its frequency and dies at its damping") {
  // A bubble of 2 kHz damped at 300 a second, alone on a voice whose bands are silent.
  NoiseState n;
  const NoiseStrike s = noise_strike(1, 2000.0f, 300.0f, 0.5f);
  CHECK(s.resonator == 1u);
  strike_noise(n, s);
  std::vector<f32> out(4800, 0.0f);
  render_noise(n, out.data(), 4800);
  // Its first sample is A sin w, and it rings A r^n sin(w (n + 1)) after (f32's recursion against
  // the closed form in double, to a part in a thousand of the peak).
  const f64 w = 2.0 * 3.14159265358979323846 * 2000.0 / 48000.0;
  const f64 r = std::exp(-300.0 / 48000.0);
  f64 worst = 0.0;
  for (u32 i = 0; i < 2000; ++i) {
    const f64 want = 0.5 * std::pow(r, static_cast<f64>(i)) * std::sin(w * (i + 1.0));
    worst = std::max(worst, std::fabs(static_cast<f64>(out[i]) - want));
  }
  CHECK(worst < 5e-4);
  // It rings until r^n is 1e-4 (ln 1e4 / (300 / 48000) = 1,473.7 frames) and is then still.
  CHECK(s.life == 1474u);
  CHECK(n.res_life[1] == 0u);
  for (u32 i = s.life; i < 4800; ++i)
    CHECK(out[i] == 0.0f);
  // A voice never struck renders nothing from its resonators.
  NoiseState quiet;
  std::vector<f32> none(480, 0.0f);
  render_noise(quiet, none.data(), 480);
  for (const f32 x : none)
    CHECK(x == 0.0f);
}

namespace {

std::vector<f32> strike_session(std::initializer_list<u32> calls, u64* strikes) {
  ClipStore clips;
  MixerConfig config;
  config.voices = 4;
  config.field_ramp_frames = 2400;
  config.limiter = LimiterMode::Off;
  Mixer mixer(clips, config);
  NoisePlayParams p;
  p.noise.seed = 21;
  p.noise.gain[5] = 0.01f;  // a faint 2 kHz band under the bubbles
  p.source.flags = k_source_2d;
  const VoiceHandle v = mixer.play_noise(p);
  REQUIRE(!v.is_null());
  std::vector<f32> all;
  std::vector<f32> block(4800 * 2);
  for (u32 tick = 0; tick < 10; ++tick) {
    // Two bubbles a tick on the four resonators in turn, 1.1 to 7.4 kHz, damped as van den Doel's
    // bubbles are (0.043 f + 0.0014 f^1.5 a second).
    for (u32 k = 0; k < 2; ++k) {
      const u32 i = tick * 2 + k;
      const f32 f = 1100.0f + 330.0f * static_cast<f32>(i);
      const f32 d = 0.043f * f + 0.0014f * f * std::sqrt(f);
      CHECK(mixer.strike_noise(v, noise_strike(i % k_noise_resonators, f, d, 0.2f)));
    }
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
  if (strikes != nullptr) *strikes = mixer.control_stats().noise_strikes;
  // A strike on a voice that is gone is refused.
  CHECK(mixer.stop(v));
  mixer.render(block.data(), 4800);
  mixer.update();
  CHECK_FALSE(mixer.strike_noise(v, noise_strike(0, 1000.0f, 100.0f, 0.1f)));
  return all;
}

}  // namespace

TEST_CASE(
    "noise voice: strikes in the mixer are the same bytes however the frames are cut, pinned") {
  u64 strikes = 0;
  const std::vector<f32> whole = strike_session({480}, &strikes);
  CHECK(strikes == 20u);
  const std::vector<f32> uneven = strike_session({7, 473, 160, 1024}, nullptr);
  REQUIRE(whole.size() == 10u * 4800u * 2u);
  CHECK(whole == uneven);
  // Heard: a tick opens with its bubbles, far over the faint band under them at its end.
  f64 head = 0.0;
  f64 tail = 0.0;
  for (usize i = 9600u; i < 9600u + 400u; ++i)
    head += static_cast<f64>(whole[i]) * static_cast<f64>(whole[i]);
  for (usize i = 9600u + 8000u; i < 9600u + 8400u; ++i)
    tail += static_cast<f64>(whole[i]) * static_cast<f64>(whole[i]);
  CHECK(head > 100.0 * tail);
  const u64 hash =
      hash_mix(ChannelLayout::Stereo, std::span<const f32>(whole.data(), whole.size()));
  MESSAGE("noise strike session hash: " << hash);
  // Pinned on MSVC 2026-11-23 (S113); GCC and Clang must reproduce it.
  CHECK(hash == 0x0730cf117e1b9046ull);
}

namespace {

// The power of `x` through four of the bank's own octave band-passes in a row, in double, after
// `from`: one alone leaks a band five octaves away at only 30 dB down, which a low band 40 dB over
// the top would fill; four in a row leak it 120 dB down.
f64 band_power(const std::vector<f32>& x, u32 band, usize from) {
  const NoiseBand& b = k_noise_band_table[band];
  f64 s1[4] = {};
  f64 s2[4] = {};
  f64 sum = 0.0;
  for (usize i = 0; i < x.size(); ++i) {
    f64 y = static_cast<f64>(x[i]);
    for (u32 k = 0; k < 4; ++k) {
      const f64 cx = static_cast<f64>(b.c) * y;
      y = cx + s1[k];
      s1[k] = s2[k] - static_cast<f64>(b.a1) * y;
      s2[k] = -cx - static_cast<f64>(b.a2) * y;
    }
    if (i >= from) sum += y * y;
  }
  return sum / static_cast<f64>(x.size() - from);
}

}  // namespace

TEST_CASE("noise voice: the low-pass after the bands takes the skirts' top and leaves the bottom") {
  // S121: the 125 Hz band alone, whose skirt reaches the top octaves at about 3 dB an octave of
  // band power, with and without a low-pass at 300 Hz. The coefficients are the cookbook's
  // Butterworth in double to an f32's precision.
  const NoiseLowpass lp = noise_lowpass(300.0f);
  const f64 w = 2.0 * 3.14159265358979323846 * 300.0 / 48000.0;
  const f64 alpha = std::sin(w) / std::sqrt(2.0);
  CHECK(static_cast<f64>(lp.b0) ==
        doctest::Approx((1.0 - std::cos(w)) / 2.0 / (1.0 + alpha)).epsilon(1e-4));
  CHECK(static_cast<f64>(lp.a1) ==
        doctest::Approx(-2.0 * std::cos(w) / (1.0 + alpha)).epsilon(1e-5));
  CHECK(static_cast<f64>(lp.a2) == doctest::Approx((1.0 - alpha) / (1.0 + alpha)).epsilon(1e-5));
  CHECK(noise_lowpass(1.0f).b0 == noise_lowpass(k_noise_tone_min_hz).b0);  // held to its range

  NoiseState plain = one_band(1, 0.25f, 11u);
  NoiseState low = one_band(1, 0.25f, 11u);
  low.lp_b0 = lp.b0;
  low.lp_a1 = lp.a1;
  low.lp_a2 = lp.a2;
  std::vector<f32> a(96000);
  std::vector<f32> b(96000);
  render_noise(plain, a.data(), static_cast<u32>(a.size()));
  render_noise(low, b.data(), static_cast<u32>(b.size()));
  const f64 at_125_plain = band_power(a, 1, 4800);
  const f64 at_125_low = band_power(b, 1, 4800);
  const f64 at_4k_plain = band_power(a, 6, 4800);
  const f64 at_4k_low = band_power(b, 6, 4800);
  MESSAGE("125 Hz: " << 10.0 * std::log10(at_125_low / at_125_plain)
                     << " dB; 4 kHz: " << 10.0 * std::log10(at_4k_low / at_4k_plain) << " dB");
  // Its own octave within half a decibel; the skirt at 4 kHz, five octaves up, 40 dB and more
  // further down (a second order at 300 Hz takes 2 x 6 dB an octave past its corner: 48 dB).
  CHECK(std::fabs(10.0 * std::log10(at_125_low / at_125_plain)) < 0.5);
  CHECK(10.0 * std::log10(at_4k_low / at_4k_plain) < -40.0);
  // A voice with none is untouched: the same samples as one that never heard of the low-pass.
  NoiseState again = one_band(1, 0.25f, 11u);
  std::vector<f32> c(96000);
  render_noise(again, c.data(), static_cast<u32>(c.size()));
  CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(f32)) == 0);
}

// L8 of S121's review: a low-pass or a movable band whose poles are not stable would grow without
// bound on the audio thread; the mixer refuses it at the door, and takes every pair the two makers
// give from the bottom of their range to the top.
TEST_CASE("noise voice: coefficients with unstable poles are refused, the makers' never") {
  ClipStore clips;
  MixerConfig config;
  config.voices = 16;
  Mixer mixer(clips, config);
  NoisePlayParams p;
  p.noise.seed = 3;
  p.noise.gain[0] = 0.1f;
  p.source.flags = k_source_2d;
  for (const f32 hz : {k_noise_tone_min_hz, 100.0f, 1000.0f, 10000.0f, k_noise_tone_max_hz}) {
    p.noise.lowpass = noise_lowpass(hz);
    p.noise.tone = noise_band(hz, 3.0f);
    const VoiceHandle v = mixer.play_noise(p);
    CHECK(!v.is_null());
    CHECK(mixer.set_noise(v, p.noise));
    (void)mixer.stop(v);
  }
  NoisePlayParams bad = p;
  bad.noise.lowpass = noise_lowpass(1000.0f);
  bad.noise.lowpass.a2 = 1.0f;  // a pole on the circle
  CHECK(mixer.play_noise(bad).is_null());
  bad.noise.lowpass = noise_lowpass(1000.0f);
  bad.noise.lowpass.a1 = -2.5f;  // a real pole outside it
  CHECK(mixer.play_noise(bad).is_null());
  NoisePlayParams tone = p;
  tone.noise.lowpass = NoiseLowpass{};
  tone.noise.tone.a2 = -1.5f;
  CHECK(mixer.play_noise(tone).is_null());
  const VoiceHandle v = mixer.play_noise(p);
  REQUIRE(!v.is_null());
  CHECK_FALSE(mixer.set_noise(v, bad.noise));
}

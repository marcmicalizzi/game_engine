// The master's look-ahead limiter (limiter.h, docs/subsystems/audio.md "The master"): nothing
// leaves above its ceiling however far over the mix goes, material under the ceiling leaves bit for
// bit (only later, by the look-ahead), and it lets go again within its release. Through the mixer,
// the way a game turns it on, and on its own, where the attack's shape can be read frame by frame.
#include "audio_test_support.h"

#include <domain/audio/audio.h>
#include <foundation/tunables/tunables.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::audio;
using namespace engine::audio::test;

namespace {

// -1 dBFS, written linearly: a test that pins limited output does not go through the C library's
// pow (limiter.h, "determinism").
constexpr f32 k_ceiling = 0.89125094f;
constexpr u32 k_latency = Limiter::k_lookahead_frames;

MixerConfig limited(LimiterMode mode = LimiterMode::On) {
  MixerConfig config;
  config.limiter = mode;
  config.limiter_ceiling = k_ceiling;
  config.limiter_release_frames = 4800;  // 100 ms
  return config;
}

struct Rig {
  ClipStore clips;
  Mixer mixer;

  explicit Rig(const MixerConfig& config) : mixer(clips, config) {}

  // A 2D mono loop, hard left: the left channel is the voice.
  VoiceHandle play(std::span<const f32> pcm, u64 key, f32 gain) {
    const ClipHandle clip = clips.add_pcm(Id128{key, key}, pcm, 1);
    PlayParams p;
    p.clip = clip;
    p.gain = gain;
    p.pan = -1.0f;
    p.loop = true;
    p.source.flags = k_source_2d;
    return mixer.play(p);
  }

  Vector<f32> render(u32 frames) {
    Vector<f32> out;
    out.resize_exact(frames * mixer.channels());
    mixer.render(out.data(), frames);
    return out;
  }
};

void append(Vector<f32>& to, const Vector<f32>& from) {
  to.append(std::span<const f32>(from.data(), from.size()));
}

f32 peak_of(std::span<const f32> samples) {
  f32 peak = 0.0f;
  for (const f32 s : samples)
    peak = std::fabs(s) > peak ? std::fabs(s) : peak;
  return peak;
}

}  // namespace

TEST_CASE("the limiter is off by default, and its tunables say what it does when it is on") {
  CHECK_FALSE(tunable_limiter());
  CHECK(std::fabs(tunable_limiter_ceiling() - k_ceiling) < 1e-6f);  // -1 dBFS
  CHECK(tunable_limiter_release_frames() == 4800u);                 // 100 ms
  ClipStore clips;
  CHECK_FALSE(Mixer(clips).limiter_enabled());

  tunables::Tunable* on = tunables::find("audio.limiter");
  REQUIRE(on != nullptr);
  std::string error;
  REQUIRE(on->set_from_text("on", &error));
  {
    Mixer mixer(clips);
    CHECK(mixer.limiter_enabled());
    CHECK(mixer.latency_frames() == 240u);  // 5 ms
    CHECK(std::fabs(mixer.limiter_ceiling() - k_ceiling) < 1e-6f);
  }
  on->reset();
  // A mixer that says Off is off whatever the tunable says, and one that says On is on.
  MixerConfig forced = limited(LimiterMode::Off);
  CHECK_FALSE(Mixer(clips, forced).limiter_enabled());
  forced.limiter = LimiterMode::On;
  CHECK(Mixer(clips, forced).limiter_enabled());
}

TEST_CASE("under its ceiling the limiter passes the mix bit for bit, 5 ms later") {
  const Vector<f32> sine = exact_sine(4800, 48, 0.8f);  // 0.8: under -1 dBFS (0.891)
  MixerConfig plain_config;
  plain_config.limiter = LimiterMode::Off;
  Rig plain(plain_config);
  Rig limiter(limited());
  REQUIRE(limiter.mixer.latency_frames() == k_latency);
  REQUIRE_FALSE(plain.play(sine, 1, 1.0f).is_null());
  REQUIRE_FALSE(limiter.play(sine, 1, 1.0f).is_null());

  // In calls of assorted sizes, so the delay line wraps inside one call and across two.
  Vector<f32> dry;
  Vector<f32> wet;
  for (const u32 n : {480u, 100u, 1000u, 240u, 3000u, 7u}) {
    append(dry, plain.render(n));
    append(wet, limiter.render(n));
  }
  const u32 frames = dry.size() / 2u;
  for (u32 k = 0; k < k_latency; ++k) {
    CHECK(wet[2u * k] == 0.0f);
    CHECK(wet[2u * k + 1u] == 0.0f);
  }
  for (u32 k = k_latency; k < frames; ++k) {
    CHECK(wet[2u * k] == dry[2u * (k - k_latency)]);
    CHECK(wet[2u * k + 1u] == dry[2u * (k - k_latency) + 1u]);
  }
  const MixerStats stats = limiter.mixer.stats();
  CHECK(stats.limited_frames == 0u);
  CHECK(stats.limiter_gain == 1.0f);
  CHECK(stats.clipped_samples == 0u);
}

TEST_CASE("a sine twelve decibels over full scale leaves the limiter under its ceiling") {
  const Vector<f32> sine = exact_sine(4800, 48, 0.5f);
  Rig rig(limited());
  REQUIRE_FALSE(rig.play(sine, 2, 8.0f).is_null());  // 0.5 x 8 = 4.0: +12 dBFS

  Vector<f32> out;
  for (u32 block = 0; block < 100; ++block)  // a second, in the default period
    append(out, rig.render(480));
  // Nothing over the ceiling, anywhere, and so nothing for the hard clip to do.
  CHECK(peak_of(out) <= k_ceiling);
  const MixerStats stats = rig.mixer.stats();
  CHECK(stats.clipped_samples == 0u);
  CHECK(stats.limited_frames > 40000u);  // everything after the first look-ahead
  CHECK(static_cast<f64>(stats.peak) == doctest::Approx(4.0).epsilon(1e-3));  // the mix, before
  // And it is not simply turned down: in the steady state the gain is the ceiling over the peak,
  // and the output's peak is the ceiling.
  CHECK(static_cast<f64>(stats.limiter_gain) ==
        doctest::Approx(static_cast<f64>(k_ceiling) / 4.0).epsilon(1e-3));
  const std::span<const f32> last(out.data() + out.size() - 960u, 960u);
  CHECK(peak_of(last) > 0.999f * k_ceiling);
  // The right channel is silent in, and so out: the gain is linked, not made up.
  for (u32 k = 0; k < out.size() / 2u; ++k)
    CHECK(out[2u * k + 1u] == 0.0f);
}

TEST_CASE("after a burst the limiter lets go within its release, and is bit-exact again") {
  const Vector<f32> quiet = exact_sine(4800, 48, 0.25f);
  MixerConfig plain_config;
  plain_config.limiter = LimiterMode::Off;
  Rig plain(plain_config);
  Rig rig(limited());
  const VoiceHandle bed = rig.play(quiet, 3, 1.0f);
  REQUIRE_FALSE(plain.play(quiet, 3, 1.0f).is_null());
  REQUIRE_FALSE(bed.is_null());

  // A one-shot burst of a tenth of a second, +7 dBFS with the bed under it, then the bed alone.
  const Vector<f32> loud = exact_sine(4800, 96, 0.5f);
  PlayParams burst;
  burst.clip = rig.clips.add_pcm(Id128{4, 4}, loud, 1);
  burst.gain = 4.0f;
  burst.pan = -1.0f;
  burst.source.flags = k_source_2d;
  REQUIRE_FALSE(rig.mixer.play(burst).is_null());
  rig.render(4800);
  CHECK(rig.mixer.stats().limiter_gain < 0.5f);
  plain.render(4800);
  // A release time after the burst it is still letting go: the release is a time constant, not a
  // gate. (The call right after the burst still carries the burst's last 5 ms out of the delay
  // line, at the burst's gain; this is the one after that.)
  for (u32 i = 0; i < 2; ++i) {
    rig.render(4800);
    plain.render(4800);
  }
  CHECK(rig.mixer.stats().limiter_gain < 1.0f);
  CHECK(rig.mixer.stats().limiter_gain > 0.5f);

  // Fifteen release times later the reduction has decayed under 2^-16 and been let go entirely:
  // the gain is exactly 1 again, and the output is the bed's own samples, delayed, bit for bit.
  for (u32 i = 0; i < 15; ++i) {
    rig.render(4800);
    plain.render(4800);
  }
  CHECK(rig.mixer.stats().limiter_gain == 1.0f);
  const Vector<f32> dry = plain.render(960);
  const Vector<f32> wet = rig.render(960);
  // The bed is a 48-frame loop and both mixers have mixed the same number of frames, so the wet
  // output at frame k is the dry one at k - 240.
  for (u32 k = k_latency; k < 960u; ++k)
    CHECK(wet[2u * k] == dry[2u * (k - k_latency)]);
  const u64 limited_after_burst = rig.mixer.stats().limited_frames;
  rig.render(4800);
  CHECK(rig.mixer.stats().limited_frames == limited_after_burst);  // at rest: counts nothing
}

TEST_CASE("the limiter's attack is a ramp across the look-ahead that is down when the peak is") {
  // One frame at 3.0 among frames at 0.1: the peak needs a gain of ceiling / 3.
  Limiter limiter;
  limiter.configure(1, k_ceiling, 4800);
  REQUIRE(limiter.enabled());
  constexpr u32 k_frames = 2000;
  constexpr u32 k_spike = 1000;
  Vector<f32> in(k_frames, 0.1f);
  in[k_spike] = 3.0f;
  Vector<f32> out = in;
  // In calls of 480 like a device, the spike inside the third.
  for (u32 done = 0; done < k_frames; done += 480u) {
    const u32 n = k_frames - done < 480u ? k_frames - done : 480u;
    limiter.process(out.data() + done, n, peak_of({in.data() + done, n}));
  }
  const f32 needed = k_ceiling / 3.0f;
  // The spike comes out 240 frames later at the ceiling, never above it.
  CHECK(out[k_spike + k_latency] <= k_ceiling);
  CHECK(out[k_spike + k_latency] > 0.99f * k_ceiling);
  // The gain on the frames before it (output / delayed input) falls linearly across the 241-frame
  // window, from 1 to what the peak needs, and never rises on the way down: no step, no overshoot.
  f32 previous = 1.0f;
  for (u32 k = k_spike; k < k_spike + k_latency; ++k) {
    const f32 gain = out[k] / in[k - k_latency];
    CHECK(gain <= previous + 1e-6f);
    previous = gain;
  }
  const f32 step = (1.0f - needed) / static_cast<f32>(Limiter::k_window);
  CHECK(std::fabs(out[k_spike] / in[k_spike - k_latency] - (1.0f - step)) < 1e-4f);  // first step
  CHECK(out[k_spike - 1u] == in[k_spike - 1u - k_latency]);                          // untouched
  // After it the gain lets go by the release, and the whole run stayed under the ceiling.
  CHECK(out[k_spike + k_latency + 1u] / in[k_spike + 1u] < 0.5f);
  CHECK(peak_of(out) <= k_ceiling);
  CHECK(limiter.limited_frames() > 0u);
}

// The determinism claim (mixer.h, docs/subsystems/audio.md "Determinism"): for a given clip set, a
// given declared layout, and a given sequence of commands and render calls, the mix is the same
// bytes on every run, compiler and C library. The scripted session below exercises every command
// kind, pitch through the interpolating path, looping, one-shots running out, a stereo clip, 2D
// sources on a 2D bus and panned by hand, 3D sources moving past a turning listener with a cone and
// a spread, a bus change, the pool stealing and refusing, and the master clipping — and its output
// is hashed, **with its layout**, and pinned for two layouts. The pinned values were taken on MSVC;
// GCC 13 and Clang 18 (the Linux container build) must reproduce them, which is the whole of the
// cross-toolchain half of the claim.
//
// The clips are generated without libm (audio_test_support.h, `exact_sine` and `lcg_noise`), so the
// hash depends on the mixer and nothing else.
#include "audio_test_support.h"

#include <domain/audio/audio.h>

#include <doctest/doctest.h>

using namespace engine;
using namespace engine::audio;
using namespace engine::audio::test;

namespace {

constexpr u32 k_blocks = 48;
constexpr u32 k_block_frames = 480;

// Every block of the session — the frames between two rounds of commands — is rendered as calls of
// these sizes in turn, which must add up to `k_block_frames`: one call of 480 is what a device with
// a 480-frame period pulls; three of 160, or 7 and 473, are other devices given the same commands
// at the same frames.
constexpr u32 k_whole[] = {k_block_frames};

struct Options {
  std::span<const u32> calls = k_whole;
  u32 ramp_frames = 480;  // 10 ms, audio.ramp_ms's default: part of what is pinned
  // The master's limiter, at -1 dBFS given linearly (limiter.h: the dB tunable goes through the C
  // library's pow) and a 100 ms release. Off is the default and what the first two pins are.
  bool limiter = false;
};

Vector<f32> run_session(ChannelLayout layout, const Options& options = {}) {
  const std::span<const u32> calls = options.calls;
  ClipStore clips;
  MixerConfig config;
  config.voices = 8;  // small, so the burst at block 30 has to steal and refuse
  config.layout = layout;
  config.ramp_frames = options.ramp_frames;
  config.limiter = options.limiter ? LimiterMode::On : LimiterMode::Off;
  config.limiter_ceiling = 0.89125094f;
  config.limiter_release_frames = 4800;
  Mixer mixer(clips, config);
  REQUIRE(mixer.layout() == layout);

  const Vector<f32> tone = exact_sine(12000, 48, 0.5f);
  const Vector<f32> pad = exact_sine(9600, 96, 0.4f, 2);
  const Vector<f32> hiss = lcg_noise(6000, 42, 0.3f);
  Vector<f32> texture = lcg_noise(8000, 7, 0.2f);
  for (u32 i = 0; i < texture.size(); i += 2)
    texture[i] = -texture[i];  // a stereo pair, not a copy
  const ClipHandle tone_clip = clips.add_pcm(Id128{1, 1}, tone, 1);
  const ClipHandle pad_clip = clips.add_pcm(Id128{1, 2}, pad, 2);
  const ClipHandle hiss_clip = clips.add_pcm(Id128{1, 3}, hiss, 1);
  const ClipHandle texture_clip = clips.add_pcm(Id128{1, 4}, texture, 2);

  VoiceHandle tone_voice;
  VoiceHandle pad_voice;
  VoiceHandle hiss_voice;
  const u32 channels = mixer.channels();
  Vector<f32> all;
  all.reserve(k_blocks * k_block_frames * channels);
  Vector<f32> block;
  block.resize_exact(k_block_frames * channels);

  for (u32 b = 0; b < k_blocks; ++b) {
    mixer.update();
    if (b == 0) {
      PlayParams p;
      p.clip = tone_clip;  // a 2D source panned by hand
      p.gain = 0.7f;
      p.pan = -0.3f;
      p.loop = true;
      p.source.flags = k_source_2d;
      tone_voice = mixer.play(p);
      p = PlayParams{};
      p.clip = pad_clip;  // on the music bus: 2D because the bus is
      p.gain = 0.5f;
      p.pan = 0.2f;
      p.loop = true;
      p.bus = k_bus_music;
      p.source.position = Vec3{50.0f, 0.0f, 0.0f};  // ignored
      pad_voice = mixer.play(p);
      p = PlayParams{};
      p.clip = hiss_clip;  // a 3D one-shot
      p.source.position = Vec3{4.0f, 0.0f, -2.0f};
      p.source.min_distance = 1.0f;
      p.source.max_distance = 20.0f;
      hiss_voice = mixer.play(p);
    }
    if (b == 5) {
      VoiceParams v;
      v.gain = 0.9f;
      v.pitch = 1.37f;
      v.pan = -0.3f;
      v.loop = true;
      mixer.set_params(tone_voice, v);
      Listener l;
      l.forward = Vec3{0.3f, 0.0f, -1.0f};
      mixer.set_listener(l);
    }
    if (b == 8) {
      PlayParams p;
      p.clip = hiss_clip;
      p.pitch = 0.73f;
      p.start_frame = 100;
      p.priority = 10;
      p.source.position = Vec3{-6.0f, 1.0f, 0.0f};
      p.source.min_distance = 0.5f;
      p.source.max_distance = 30.0f;
      p.source.distance_model = DistanceModel::Linear;
      p.source.directivity = Directivity::Cone;
      const Cone cone = make_cone(60.0f, 200.0f);
      p.source.cone_inner_cos = cone.inner_cos;
      p.source.cone_outer_cos = cone.outer_cos;
      p.source.cone_outer_gain = 0.3f;
      p.source.spread = 0.25f;
      hiss_voice = mixer.play(p);
    }
    if (b >= 10 && b < 20) {
      SourceSpatial s;
      s.position = Vec3{-6.0f + static_cast<f32>(b - 10u), 1.0f, -0.5f * static_cast<f32>(b - 10u)};
      s.orientation = normalize(Quat{0.0f, 0.2f * static_cast<f32>(b - 10u), 0.0f, 1.0f});
      s.min_distance = 0.5f;
      s.max_distance = 30.0f;
      s.distance_model = DistanceModel::Linear;
      s.directivity = Directivity::Cone;
      const Cone cone = make_cone(60.0f, 200.0f);
      s.cone_inner_cos = cone.inner_cos;
      s.cone_outer_cos = cone.outer_cos;
      s.cone_outer_gain = 0.3f;
      s.spread = 0.25f;
      mixer.set_source(hiss_voice, s);
    }
    if (b == 10) mixer.set_bus_gain(k_bus_music, 0.3f);
    if (b == 15) mixer.stop(pad_voice);
    if (b == 20) {
      PlayParams p;
      p.clip = texture_clip;
      p.pitch = 1.9f;
      p.pan = 0.8f;
      p.loop = true;
      p.bus = k_bus_ambient;
      p.source.flags = k_source_2d;
      mixer.play(p);
    }
    if (b == 25) mixer.set_bus_gain(k_bus_master, 3.0f);  // into the clip
    if (b == 28) mixer.set_bus_gain(k_bus_master, 1.0f);
    if (b == 30) {
      // A burst: more one-shots than the pool has free voices. The first eight outrank the two
      // loops still playing, so the last two of those steal them; the four after that outrank
      // nothing and are refused.
      for (u32 i = 0; i < 12; ++i) {
        PlayParams p;
        p.clip = i % 2u == 0 ? hiss_clip : tone_clip;
        p.gain = 0.1f;
        p.pitch = 0.5f + 0.125f * static_cast<f32>(i);
        p.priority = i < 8u ? 200 : 20;
        p.source.position = Vec3{static_cast<f32>(i) - 6.0f, 0.0f, -3.0f};
        mixer.play(p);
      }
    }
    if (b == 40) {
      Listener l;
      l.position = Vec3{1.0f, 0.0f, 1.0f};
      l.forward = Vec3{-1.0f, 0.0f, 0.0f};
      l.up = Vec3{0.0f, 1.0f, 0.1f};
      mixer.set_listener(l);
    }
    u32 done = 0;
    for (const u32 n : calls) {
      mixer.render(block.data() + static_cast<usize>(done) * channels, n);
      done += n;
    }
    REQUIRE(done == k_block_frames);
    all.append(std::span<const f32>(block.data(), block.size()));
  }

  // The session has to have done what it says, or the hash pins less than it claims.
  const MixerStats stats = mixer.stats();
  const ControlStats& control = mixer.control_stats();
  CHECK(stats.blocks == k_blocks * calls.size());
  if (options.limiter) {
    // The master bus's 3x at block 25 is over the ceiling: the limiter takes it, the clip does not.
    CHECK(stats.limited_frames > 0u);
    CHECK(stats.clipped_samples == 0u);
  } else {
    CHECK(stats.clipped_samples > 0u);
  }
  CHECK(stats.stale_commands == 0u);
  CHECK(stats.events_dropped == 0u);
  CHECK(control.steals > 0u);
  CHECK(control.refused_pool > 0u);
  CHECK(control.events > 0u);
  return all;
}

Options split(std::span<const u32> calls, const Options& base = {}) {
  Options options = base;
  options.calls = calls;
  return options;
}

void check_session(ChannelLayout layout, u64 pinned, const Options& options = {}) {
  const Vector<f32> first = run_session(layout, options);
  const Vector<f32> second = run_session(layout, options);
  REQUIRE(first.size() == k_blocks * k_block_frames * layout_channels(layout));
  CHECK(first == second);
  const u64 hash = hash_mix(layout, {first.data(), first.size()});
  const std::string name =
      std::string(layout_name(layout)) + (options.limiter ? " limited" : "") + " session hash ";
  MESSAGE(name << hex64(hash));
  CHECK(hash == pinned);
}

}  // namespace

// Taken on MSVC (msvc-debug) on 2026-09-24. A change to the mix — the order voices are summed in,
// the pan law, the ramp, the interpolation, the source model, the layout table — moves them, and
// the commit that moves them says why. They have moved once: when the ramp became a fixed time
// (10 ms) instead of the block. The session's blocks are 480 frames, the same as the ramp, so every
// ramp still starts and ends where it did; what changed is the ramp's last frame, which is now the
// target itself rather than the ramp formula's rounding of it. Two samples of 46,080 (stereo) and
// two of 138,240 (5.1) moved, each on a block's last frame, by 3e-8 at most (v0's pins were
// 0x8daba54729a3c2f1 and 0xb15db834d3793bf4, reproduced with v0's panner before moving them).
TEST_CASE("a scripted session mixes to the same bytes every time, on every compiler: stereo") {
  check_session(ChannelLayout::Stereo, 0x4e7aa0ccb3a4fdafull);
}

TEST_CASE("the same session declared 5.1 is its own pinned output") {
  check_session(ChannelLayout::Surround51, 0xc8486cc4956e413aull);
}

// The same session through the master's limiter, which the session's 3x master gain drives well
// over its ceiling. The limiter's arithmetic is IEEE f32 and f64 with contraction off, integers
// and `ceil` (limiter.h), so this is a cross-toolchain pin like the two above. Under the ceiling
// the limiter is a pure delay, so until the session first goes over it this is the stereo pin's
// output 240 frames later (the next test holds it to that).
TEST_CASE("the session through the master's limiter is its own pinned output") {
  Options options;
  options.limiter = true;
  check_session(ChannelLayout::Stereo, 0xdc377a2dac6eb43aull, options);
}

// The period a device asks for is not an input: the same commands at the same frames mix to the
// same bytes however the frames between them are divided into render calls. Before the ramp was a
// time this was false — a change ramped across whatever block it landed in, so a 160-frame period
// ramped three times as fast as a 480-frame one — and it is what lets a command log recorded on one
// machine replay to the same mix on another whose device runs a different period.
TEST_CASE("the session mixes to the same bytes whatever the render calls' sizes") {
  constexpr u32 thirds[] = {160, 160, 160};
  constexpr u32 ragged[] = {7, 473};
  constexpr u32 many[] = {64, 64, 64, 64, 64, 64, 64, 32};
  for (const ChannelLayout layout : {ChannelLayout::Stereo, ChannelLayout::Surround51}) {
    CAPTURE(layout_name(layout));
    const Vector<f32> whole = run_session(layout);
    CHECK(run_session(layout, split(thirds)) == whole);
    CHECK(run_session(layout, split(ragged)) == whole);
    CHECK(run_session(layout, split(many)) == whole);
  }
  // A ramp longer than a block (15 ms: 720 frames) crosses the calls, and still does not care.
  Options long_ramp;
  long_ramp.ramp_frames = 720;
  const Vector<f32> slow = run_session(ChannelLayout::Stereo, long_ramp);
  CHECK(run_session(ChannelLayout::Stereo, split(thirds, long_ramp)) == slow);
  CHECK(run_session(ChannelLayout::Stereo, split(ragged, long_ramp)) == slow);
  CHECK(slow != run_session(ChannelLayout::Stereo));  // and it is a different mix
  // Nor does the limiter, whose state is per frame and carried across calls; its delay line wraps
  // inside a call and across two.
  Options limited;
  limited.limiter = true;
  const Vector<f32> through = run_session(ChannelLayout::Stereo, limited);
  CHECK(run_session(ChannelLayout::Stereo, split(thirds, limited)) == through);
  CHECK(run_session(ChannelLayout::Stereo, split(ragged, limited)) == through);
  CHECK(run_session(ChannelLayout::Stereo, split(many, limited)) == through);
}

TEST_CASE("under the ceiling the limited session is the plain one, 5 ms later, bit for bit") {
  Options limited;
  limited.limiter = true;
  const Vector<f32> plain = run_session(ChannelLayout::Stereo);
  const Vector<f32> through = run_session(ChannelLayout::Stereo, limited);
  // Until the first frame over the ceiling enters the limiter, its gain is 1 on every frame: output
  // frame n is input frame n - 240 exactly.
  constexpr f32 k_ceiling = 0.89125094f;
  constexpr u32 k_delay = Limiter::k_lookahead_frames;
  u32 first_loud = static_cast<u32>(plain.size() / 2u);
  for (u32 i = 0; i < plain.size(); ++i) {
    if (std::fabs(plain[i]) > k_ceiling) {
      first_loud = i / 2u;
      break;
    }
  }
  MESSAGE("first frame over the ceiling: " << first_loud);
  CHECK(first_loud > 10u * k_block_frames);  // enough of the session to mean something
  for (u32 i = 0; i < 2u * k_delay; ++i)
    CHECK(through[i] == 0.0f);
  u32 differ = 0;
  for (u32 i = 2u * k_delay; i < 2u * first_loud; ++i)
    differ += through[i] == plain[i - 2u * k_delay] ? 0u : 1u;
  CHECK(differ == 0u);
}

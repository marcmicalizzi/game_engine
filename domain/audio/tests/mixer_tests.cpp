// The mixer against analytic expectations, all through the null backend: a generated clip goes in,
// `render()` pulls the mix into the test's own buffer, and every sample is compared with what the
// gain, the pan law, the pitch, the bus tree, the source model or the declared layout says it must
// be (docs/subsystems/audio.md).
#include "audio_test_support.h"

#include <domain/audio/audio.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string_view>

using namespace engine;
using namespace engine::audio;
using namespace engine::audio::test;

namespace {

struct Rig {
  ClipStore clips;
  Mixer mixer;

  explicit Rig(const MixerConfig& config = {}) : mixer(clips, config) {}

  ClipHandle add(std::span<const f32> pcm, u8 channels = 1) {
    const Id128 key = clip_key(
        std::span<const u8>(reinterpret_cast<const u8*>(pcm.data()), pcm.size() * sizeof(f32)));
    return clips.add_pcm(key, pcm, channels);
  }

  Vector<f32> render(u32 frames) {
    Vector<f32> out;
    out.resize_exact(frames * mixer.channels());
    mixer.render(out.data(), frames);
    return out;
  }
};

MixerConfig voices(u32 count) {
  MixerConfig config;
  config.voices = count;
  return config;
}

MixerConfig layout(ChannelLayout declared, const Decoder* decoder = &k_stereo_panner) {
  MixerConfig config;
  config.layout = declared;
  config.decoder = decoder;
  return config;
}

// A 2D source: not in the world, panned by `pan`.
PlayParams flat(ClipHandle clip, f32 pan = 0.0f) {
  PlayParams play;
  play.clip = clip;
  play.pan = pan;
  play.source.flags = k_source_2d;
  return play;
}

f32 max_abs_error(std::span<const f32> a, std::span<const f32> b) {
  f32 worst = 0.0f;
  for (usize i = 0; i < a.size() && i < b.size(); ++i) {
    const f32 e = std::fabs(a[i] - b[i]);
    worst = e > worst ? e : worst;
  }
  return worst;
}

Vector<f32> channel(std::span<const f32> interleaved, u32 which, u32 channels = 2) {
  Vector<f32> out;
  out.resize_exact(static_cast<u32>(interleaved.size() / channels));
  for (u32 i = 0; i < out.size(); ++i)
    out[i] = interleaved[channels * i + which];
  return out;
}

}  // namespace

TEST_CASE("a voice's gain scales the clip, centred at -3 dB per channel") {
  Rig rig;
  const Vector<f32> sine = reference_sine(4800, 1000.0, 0.8);
  PlayParams play = flat(rig.add(sine));
  play.gain = 0.5f;
  const VoiceHandle voice = rig.mixer.play(play);
  REQUIRE_FALSE(voice.is_null());
  const Vector<f32> out = rig.render(480);

  // Bit for bit: pitch 1 reads the samples unchanged, a fresh voice starts at its level, and the
  // centre of the constant-power law is sin_quarter(0.5) in both channels.
  const f32 g = 0.5f * sin_quarter(0.5f);
  for (u32 k = 0; k < 480; ++k) {
    CHECK(out[2u * k] == sine[k] * g);
    CHECK(out[2u * k + 1u] == sine[k] * g);
  }
  // And analytically: 0.8 * 0.5 * cos(pi/4) * sin(2 pi 1000 t).
  const Vector<f32> expected = reference_sine(480, 1000.0, 0.8 * 0.5 * std::sqrt(0.5));
  CHECK(max_abs_error(channel(out, 0), expected) < 1e-6f);
  CHECK(max_abs_error(channel(out, 1), expected) < 1e-6f);
}

TEST_CASE("pan follows the constant-power law for mono and balance for stereo") {
  const Vector<f32> sine = reference_sine(960, 1000.0, 0.8);

  Rig hard;
  REQUIRE_FALSE(hard.mixer.play(flat(hard.add(sine), -1.0f)).is_null());
  Vector<f32> out = hard.render(480);
  for (u32 k = 0; k < 480; ++k) {
    CHECK(out[2u * k] == sine[k]);
    CHECK(out[2u * k + 1u] == 0.0f);
  }

  Rig part;
  REQUIRE_FALSE(part.mixer.play(flat(part.add(sine), 0.5f)).is_null());  // theta = 3 pi / 8
  out = part.render(480);
  const Vector<f32> expected_left = reference_sine(480, 1000.0, 0.8 * std::cos(3.0 * k_pi_d / 8.0));
  const Vector<f32> expected_right =
      reference_sine(480, 1000.0, 0.8 * std::sin(3.0 * k_pi_d / 8.0));
  CHECK(max_abs_error(channel(out, 0), expected_left) < 1e-6f);
  CHECK(max_abs_error(channel(out, 1), expected_right) < 1e-6f);

  // A stereo clip panned right keeps its right channel and loses half its left.
  Rig stereo;
  const Vector<f32> pair = exact_sine(480, 48, 0.5f, 2);
  REQUIRE_FALSE(stereo.mixer.play(flat(stereo.add(pair, 2), 0.5f)).is_null());
  out = stereo.render(480);
  for (u32 k = 0; k < 480; ++k) {
    CHECK(out[2u * k] == pair[2u * k] * 0.5f);
    CHECK(out[2u * k + 1u] == pair[2u * k + 1u]);
  }
}

TEST_CASE("pitch is a playback rate: an octave up reads every second sample") {
  const Vector<f32> sine = reference_sine(9600, 500.0, 0.8);

  Rig up;
  PlayParams play = flat(up.add(sine), -1.0f);  // hard left: the left channel is the voice
  play.pitch = 2.0f;
  REQUIRE_FALSE(up.mixer.play(play).is_null());
  Vector<f32> out = up.render(480);
  for (u32 k = 0; k < 480; ++k)
    CHECK(out[2u * k] == sine[2u * k]);
  // 500 Hz read twice as fast is 1 kHz.
  CHECK(max_abs_error(channel(out, 0), reference_sine(480, 1000.0, 0.8)) < 1e-6f);

  // An octave down interpolates halfway between neighbours on every odd frame.
  Rig down;
  play = flat(down.add(sine), -1.0f);
  play.pitch = 0.5f;
  REQUIRE_FALSE(down.mixer.play(play).is_null());
  out = down.render(480);
  for (u32 k = 0; k < 240; ++k) {
    CHECK(out[4u * k] == sine[k]);
    CHECK(out[4u * k + 2u] == sine[k] + (sine[k + 1u] - sine[k]) * 0.5f);
  }
  // 250 Hz, within linear interpolation's error at 96 samples a cycle.
  CHECK(max_abs_error(channel(out, 0), reference_sine(480, 250.0, 0.8)) < 1e-3f);

  // A rate that is not a power of two: 1.5 x 1 kHz is 1.5 kHz.
  Rig odd;
  const Vector<f32> khz = reference_sine(9600, 1000.0, 0.8);
  play = flat(odd.add(khz), -1.0f);
  play.pitch = 1.5f;
  REQUIRE_FALSE(odd.mixer.play(play).is_null());
  out = odd.render(480);
  CHECK(max_abs_error(channel(out, 0), reference_sine(480, 1500.0, 0.8)) < 3e-3f);
}

TEST_CASE("bus gains compose from the voice's bus up to the master") {
  Rig rig;
  const Vector<f32> sine = exact_sine(4800, 48, 1.0f);
  PlayParams play = flat(rig.add(sine), -1.0f);
  play.loop = true;
  play.bus = k_bus_sfx;
  REQUIRE(rig.mixer.set_bus_gain(k_bus_master, 0.5f));
  REQUIRE(rig.mixer.set_bus_gain(k_bus_sfx, 0.5f));
  REQUIRE(rig.mixer.set_bus_gain(k_bus_music, 0.0f));  // a sibling: must not touch sfx
  REQUIRE_FALSE(rig.mixer.play(play).is_null());
  Vector<f32> out = rig.render(480);
  for (u32 k = 0; k < 480; ++k)
    CHECK(out[2u * k] == sine[k] * 0.25f);
  CHECK(rig.mixer.bus_gain(k_bus_master) == 0.5f);

  // A deeper tree, from configuration: footsteps under sfx under the master.
  constexpr ChannelMapping front = ChannelMapping::FrontPair;
  const BusDesc tree[] = {{"master", k_no_parent, 0.5f, false, front},
                          {"music", 0, 1.0f, true, front},
                          {"sfx", 0, 0.5f, false, front},
                          {"voice", 0, 1.0f, false, front},
                          {"ambient", 0, 1.0f, false, front},
                          {"footsteps", 2, 0.5f, false, front}};
  MixerConfig config;
  config.buses = tree;
  Rig deep(config);
  CHECK(deep.mixer.bus_count() == 6u);
  play = flat(deep.add(sine), -1.0f);
  play.bus = 5;
  REQUIRE_FALSE(deep.mixer.play(play).is_null());
  out = deep.render(480);
  for (u32 k = 0; k < 480; ++k)
    CHECK(out[2u * k] == sine[k] * 0.125f);

  // A malformed tree (a parent that is not an earlier bus) falls back to the default one.
  const BusDesc broken[] = {{"master", k_no_parent, 1.0f, false, front},
                            {"loop", 1, 1.0f, false, front}};
  MixerConfig bad;
  bad.buses = broken;
  Rig fallback(bad);
  CHECK(fallback.mixer.bus_count() == 5u);
}

namespace {

// The left channel of `frames` frames rendered in calls of `block` frames — what a device with that
// period would have pulled.
Vector<f32> render_left(Rig& rig, u32 frames, u32 block) {
  Vector<f32> left;
  for (u32 done = 0; done < frames;) {
    const u32 n = frames - done < block ? frames - done : block;
    const Vector<f32> out = rig.render(n);
    for (u32 k = 0; k < n; ++k)
      left.push_back(out[2u * k]);
    done += n;
  }
  return left;
}

// Ramp frame k (1-based) of a change from `from` to `to` over `ramp` frames, in the kernel's own
// arithmetic; the target itself from frame `ramp` on.
f32 ramp_at(f32 from, f32 to, u32 k, u32 ramp) {
  return k >= ramp ? to : from + (to - from) / static_cast<f32>(ramp) * static_cast<f32>(k);
}

}  // namespace

TEST_CASE("a change reaches its target after the ramp time, whatever the block size") {
  // The default is 10 ms, 480 frames; the mixer reads it once.
  CHECK(tunable_ramp_frames() == 480u);
  CHECK(Rig().mixer.ramp_frames() == 480u);

  const Vector<f32> ones(48000, 1.0f);
  // A device's period is anything from a few dozen frames to thousands, and the render call can be
  // longer than the mixer's own 1024-frame block (4096 is four of them).
  for (const u32 block : {64u, 256u, 480u, 1000u, 4096u}) {
    CAPTURE(block);
    MixerConfig config;
    config.ramp_frames = 480;
    Rig rig(config);
    PlayParams play = flat(rig.add(ones), -1.0f);  // hard left: the left channel is the gain
    play.loop = true;
    const VoiceHandle voice = rig.mixer.play(play);
    REQUIRE_FALSE(voice.is_null());
    for (const f32 s : render_left(rig, 1000, block))
      CHECK(s == 1.0f);  // a new voice starts at its level

    // 1 -> 0: linear over 480 frames, then held.
    VoiceParams params;
    params.pan = -1.0f;
    params.loop = true;
    params.gain = 0.0f;
    REQUIRE(rig.mixer.set_params(voice, params));
    const Vector<f32> down = render_left(rig, 1200, block);
    for (u32 k = 0; k < down.size(); ++k)
      CHECK(down[k] == ramp_at(1.0f, 0.0f, k + 1u, 480));
    CHECK(down[478] > 0.0f);
    CHECK(down[479] == 0.0f);  // exactly the target on frame 480

    // A change that arrives mid-ramp starts a new ramp from where the gain had got to: no jump.
    params.gain = 1.0f;
    REQUIRE(rig.mixer.set_params(voice, params));
    const Vector<f32> part = render_left(rig, 200, block < 200u ? block : 200u);
    for (u32 k = 0; k < 200; ++k)
      CHECK(part[k] == ramp_at(0.0f, 1.0f, k + 1u, 480));
    params.gain = 0.5f;
    REQUIRE(rig.mixer.set_params(voice, params));
    const Vector<f32> turn = render_left(rig, 1000, block);
    const f32 reached = ramp_at(0.0f, 1.0f, 200, 480);
    CHECK(std::fabs(turn[0] - reached) < 0.01f);
    for (u32 k = 0; k < turn.size(); ++k)
      CHECK(turn[k] == ramp_at(reached, 0.5f, k + 1u, 480));

    // A stop fades over the same time and then frees the voice, spanning calls when it must.
    REQUIRE(rig.mixer.stop(voice));
    CHECK(rig.mixer.is_live(voice));  // until the audio thread says it has ended
    u32 faded = 0;
    Vector<f32> tail;
    while (rig.mixer.is_live(voice)) {
      REQUIRE(faded < 2000u);
      const u32 n = block;
      const Vector<f32> out = rig.render(n);
      for (u32 k = 0; k < n; ++k)
        tail.push_back(out[2u * k]);
      faded += n;
      rig.mixer.update();
    }
    CHECK(faded >= 480u);         // not before the fade has run
    CHECK(faded < 480u + block);  // and in the call it ended in
    for (u32 k = 0; k < tail.size(); ++k)
      CHECK(tail[k] == ramp_at(0.5f, 0.0f, k + 1u, 480));
    CHECK(rig.mixer.live_voices() == 0u);
    for (const f32 s : rig.render(480))
      CHECK(s == 0.0f);
  }
}

TEST_CASE("a ramp shorter than the block ends inside it and holds the target after") {
  MixerConfig config;
  config.ramp_frames = 240;
  Rig rig(config);
  CHECK(rig.mixer.ramp_frames() == 240u);
  const Vector<f32> ones(9600, 1.0f);
  PlayParams play = flat(rig.add(ones), -1.0f);
  play.loop = true;
  const VoiceHandle voice = rig.mixer.play(play);
  rig.render(480);
  VoiceParams params;
  params.pan = -1.0f;
  params.loop = true;
  params.gain = 0.25f;
  REQUIRE(rig.mixer.set_params(voice, params));
  const Vector<f32> out = rig.render(480);
  for (u32 k = 0; k < 480; ++k)
    CHECK(out[2u * k] == ramp_at(1.0f, 0.25f, k + 1u, 240));
  CHECK(out[2u * 239u] == 0.25f);
  CHECK(out[2u * 479u] == 0.25f);
}

TEST_CASE("a one-shot ends at its last frame and a loop wraps exactly") {
  Rig rig;
  Vector<f32> ramp;
  for (u32 i = 0; i < 1000; ++i)
    ramp.push_back(static_cast<f32>(i + 1u) / 1024.0f);
  const VoiceHandle shot = rig.mixer.play(flat(rig.add(ramp), -1.0f));
  REQUIRE_FALSE(shot.is_null());
  Vector<f32> all;
  for (int block = 0; block < 3; ++block) {
    const Vector<f32> out = rig.render(480);
    for (u32 k = 0; k < 480; ++k)
      all.push_back(out[2u * k]);
  }
  for (u32 i = 0; i < 1000; ++i)
    CHECK(all[i] == ramp[i]);
  for (u32 i = 1000; i < all.size(); ++i)
    CHECK(all[i] == 0.0f);
  CHECK(rig.mixer.update() == 1u);
  CHECK_FALSE(rig.mixer.is_live(shot));
  CHECK(rig.mixer.stats().voices_playing == 0u);

  Rig loop_rig;
  Vector<f32> short_ramp;
  for (u32 i = 0; i < 100; ++i)
    short_ramp.push_back(static_cast<f32>(i) / 128.0f);
  PlayParams play = flat(loop_rig.add(short_ramp), -1.0f);
  play.loop = true;
  play.start_frame = 30;
  REQUIRE_FALSE(loop_rig.mixer.play(play).is_null());
  const Vector<f32> out = loop_rig.render(480);
  for (u32 k = 0; k < 480; ++k)
    CHECK(out[2u * k] == short_ramp[(k + 30u) % 100u]);

  // Half speed across the loop point interpolates from the last frame toward the first.
  Rig slow;
  play = flat(slow.add(short_ramp), -1.0f);
  play.loop = true;
  play.pitch = 0.5f;
  play.start_frame = 99;
  REQUIRE_FALSE(slow.mixer.play(play).is_null());
  const Vector<f32> wrap = slow.render(4);
  CHECK(wrap[0] == short_ramp[99]);
  CHECK(wrap[2] == short_ramp[99] + (short_ramp[0] - short_ramp[99]) * 0.5f);
  CHECK(wrap[4] == short_ramp[0]);
}

TEST_CASE("a render longer than a block is mixed a block at a time, with the same samples") {
  Rig rig;
  const Vector<f32> sine = exact_sine(9600, 48, 0.5f);
  PlayParams play = flat(rig.add(sine), -1.0f);
  play.loop = true;
  REQUIRE_FALSE(rig.mixer.play(play).is_null());
  const Vector<f32> out = rig.render(3000);  // 1024 + 1024 + 952
  for (u32 k = 0; k < 3000; ++k)
    CHECK(out[2u * k] == sine[k]);
  CHECK(rig.mixer.stats().blocks == 3u);
  CHECK(rig.mixer.stats().frames == 3000u);
}

TEST_CASE("a full pool refuses what it may not take and steals the least important otherwise") {
  Rig rig(voices(4));
  const Vector<f32> sine = exact_sine(4800, 48, 0.1f);
  PlayParams play = flat(rig.add(sine));
  play.loop = true;
  play.priority = 100;
  VoiceHandle handles[4];
  for (VoiceHandle& h : handles) {
    h = rig.mixer.play(play);
    REQUIRE_FALSE(h.is_null());
  }
  CHECK(rig.mixer.live_voices() == 4u);

  play.priority = 50;
  CHECK(rig.mixer.play(play).is_null());
  CHECK(rig.mixer.control_stats().refused_pool == 1u);

  // A loop may not take an equal priority: loops of one priority would trade the pool for ever.
  play.priority = 100;
  CHECK(rig.mixer.play(play).is_null());
  CHECK(rig.mixer.control_stats().refused_pool == 2u);

  // A one-shot at equal priority steals the oldest: the newest of a burst is the one to hear.
  play.loop = false;
  const VoiceHandle equal = rig.mixer.play(play);
  REQUIRE_FALSE(equal.is_null());
  CHECK(equal.slot == handles[0].slot);
  CHECK_FALSE(rig.mixer.is_live(handles[0]));
  CHECK(rig.mixer.control_stats().steals == 1u);

  // Higher priority steals the lowest, oldest first among equals.
  play.priority = 200;
  const VoiceHandle higher = rig.mixer.play(play);
  REQUIRE_FALSE(higher.is_null());
  CHECK(higher.slot == handles[1].slot);

  // A stopping voice goes first whatever its priority: it is fading out anyway.
  REQUIRE(rig.mixer.stop(higher));
  play.priority = 1;
  const VoiceHandle low = rig.mixer.play(play);
  REQUIRE_FALSE(low.is_null());
  CHECK(low.slot == higher.slot);

  // A stolen voice's commands are refused by the controlling thread, not misapplied.
  CHECK_FALSE(rig.mixer.stop(handles[0]));
  CHECK(rig.mixer.live_voices() == 4u);

  // Rendering applies the steals as replacements: four voices sound, none has ended.
  rig.render(480);
  CHECK(rig.mixer.stats().voices_playing == 4u);
}

TEST_CASE("a stop the ring refused is kept and re-sent, so a loop cannot play for ever") {
  MixerConfig config;
  config.command_capacity = 4;
  Rig rig(config);
  CHECK(rig.mixer.command_capacity() == 4u);
  const Vector<f32> sine = exact_sine(4800, 48, 0.5f);
  PlayParams play = flat(rig.add(sine));
  play.loop = true;
  const VoiceHandle voice = rig.mixer.play(play);
  REQUIRE_FALSE(voice.is_null());
  rig.render(480);
  // Fill the ring, then ask for the stop it has no room for.
  for (int i = 0; i < 4; ++i)
    REQUIRE(rig.mixer.set_bus_gain(k_bus_sfx, 1.0f));
  CHECK_FALSE(rig.mixer.set_bus_gain(k_bus_sfx, 1.0f));
  CHECK(rig.mixer.stop(voice));  // accepted: kept for update()
  CHECK(rig.mixer.control_stats().queue_full == 2u);
  rig.render(480);  // drains the four gains; the voice plays on
  CHECK(rig.mixer.stats().voices_playing == 1u);
  CHECK(rig.mixer.update() == 0u);  // re-sends the stop
  rig.render(480);                  // fades and frees it
  CHECK(rig.mixer.update() == 1u);
  CHECK_FALSE(rig.mixer.is_live(voice));
  CHECK(rig.mixer.stats().events_dropped == 0u);
}

TEST_CASE("the mixer refuses what it cannot play and counts why") {
  Rig rig;
  PlayParams play;
  CHECK(rig.mixer.play(play).is_null());  // no clip
  CHECK(rig.mixer.control_stats().refused_clip == 1u);

  const Vector<f32> sine = exact_sine(480, 48, 0.1f);
  play.clip = rig.add(sine);
  play.pitch = 0.0f;
  CHECK(rig.mixer.play(play).is_null());
  play.pitch = 100.0f;
  CHECK(rig.mixer.play(play).is_null());
  play.pitch = 1.0f;
  play.gain = std::nanf("");
  CHECK(rig.mixer.play(play).is_null());
  play.gain = -1.0f;
  CHECK(rig.mixer.play(play).is_null());
  play.gain = 1.0f;
  play.bus = 42;
  CHECK(rig.mixer.play(play).is_null());
  play.bus = k_bus_sfx;
  play.source.position = Vec3{std::nanf(""), 0.0f, 0.0f};
  CHECK(rig.mixer.play(play).is_null());
  CHECK(rig.mixer.control_stats().refused_params == 6u);
  CHECK_FALSE(rig.mixer.set_bus_gain(9, 1.0f));
  CHECK_FALSE(rig.mixer.set_bus_gain(0, -2.0f));
}

TEST_CASE("a command for a voice that already ended is counted stale, not applied") {
  Rig rig;
  const Vector<f32> sine = exact_sine(96, 48, 0.1f);
  const VoiceHandle voice = rig.mixer.play(flat(rig.add(sine)));
  REQUIRE_FALSE(voice.is_null());
  rig.render(480);  // the clip is 96 frames: it ends inside this block
  // The controlling thread has not drained the event, so the voice is live here.
  CHECK(rig.mixer.is_live(voice));
  CHECK(rig.mixer.set_params(voice, VoiceParams{}));
  CHECK(rig.mixer.stop(voice));
  rig.render(480);
  CHECK(rig.mixer.stats().stale_commands == 2u);
  CHECK(rig.mixer.update() == 1u);
  CHECK_FALSE(rig.mixer.is_live(voice));
}

TEST_CASE("a 3D source is panned by its direction and attenuated by its distance model") {
  Rig rig;
  const Vector<f32> sine = exact_sine(4800, 48, 0.5f);
  PlayParams play;
  play.clip = rig.add(sine);
  play.loop = true;
  play.source.position = Vec3{3.0f, 0.0f, 0.0f};  // to the right of the default listener
  play.source.min_distance = 1.0f;
  play.source.max_distance = 10.0f;
  const VoiceHandle voice = rig.mixer.play(play);
  REQUIRE_FALSE(voice.is_null());
  Vector<f32> out = rig.render(480);
  const f32 attenuation = distance_gain(DistanceModel::InverseTapered, 3.0f, 1.0f, 10.0f);
  CHECK(static_cast<f64>(attenuation) == doctest::Approx((1.0 / 3.0) * (7.0 / 9.0)).epsilon(1e-6));
  for (u32 k = 0; k < 480; ++k) {
    CHECK(out[2u * k] == 0.0f);
    CHECK(out[2u * k + 1u] == sine[k] * attenuation);
  }

  // Turn the listener to face the source: it is now ahead, and centred. The first block after the
  // turn ramps; the second holds.
  Listener facing;
  facing.forward = Vec3{1.0f, 0.0f, 0.0f};
  REQUIRE(rig.mixer.set_listener(facing));
  rig.render(480);
  out = rig.render(480);
  CHECK(out[100] == out[101]);
  CHECK(out[100] != 0.0f);

  // Past max_distance it is silent.
  SourceSpatial far;
  far.position = Vec3{30.0f, 0.0f, 0.0f};
  far.min_distance = 1.0f;
  far.max_distance = 10.0f;
  REQUIRE(rig.mixer.set_source(voice, far));
  rig.render(480);
  out = rig.render(480);
  for (const f32 s : out)
    CHECK(s == 0.0f);

  // Spread widens a source toward the centre: fully spread, a hard-right source is centred.
  SourceSpatial wide;
  wide.position = Vec3{3.0f, 0.0f, 0.0f};
  wide.max_distance = 10.0f;
  wide.spread = 1.0f;
  Rig spread;
  play.clip = spread.add(sine);
  play.source = wide;
  REQUIRE_FALSE(spread.mixer.play(play).is_null());
  out = spread.render(480);
  for (u32 k = 0; k < 480; ++k)
    CHECK(out[2u * k] == out[2u * k + 1u]);
}

TEST_CASE("a directional source is quieter behind its cone") {
  // The source sits 2 m ahead of the listener. Unrotated it faces -z, away from the listener, which
  // is then 180 degrees off its axis; rotated half a turn about y it faces +z, straight at the
  // listener. Inner cone 90 degrees, outer 180, a quarter of the level outside.
  const Vector<f32> sine = exact_sine(4800, 48, 0.5f);
  const Cone cone = make_cone(90.0f, 180.0f);
  CHECK(static_cast<f64>(cone.inner_cos) == doctest::Approx(std::cos(k_pi_d / 4.0)).epsilon(1e-6));
  CHECK(std::fabs(cone.outer_cos) < 1e-6f);

  PlayParams play;
  play.loop = true;
  play.source.position = Vec3{0.0f, 0.0f, -2.0f};
  play.source.min_distance = 5.0f;  // inside min: distance leaves it alone
  play.source.directivity = Directivity::Cone;
  play.source.cone_inner_cos = cone.inner_cos;
  play.source.cone_outer_cos = cone.outer_cos;
  play.source.cone_outer_gain = 0.25f;

  Rig facing_away;
  play.clip = facing_away.add(sine);
  play.source.orientation = Quat{};  // faces -z: away from the listener
  REQUIRE_FALSE(facing_away.mixer.play(play).is_null());
  const Vector<f32> away = facing_away.render(480);

  Rig facing_toward;
  play.clip = facing_toward.add(sine);
  play.source.orientation = Quat{0.0f, 1.0f, 0.0f, 0.0f};  // half a turn about y: faces +z
  REQUIRE_FALSE(facing_toward.mixer.play(play).is_null());
  const Vector<f32> toward = facing_toward.render(480);

  // Facing the listener it is at full level; facing away, the listener is outside the outer cone
  // and hears a quarter of it. Both are straight ahead of the listener, so both are centred.
  const f32 centre = sin_quarter(0.5f);
  for (u32 k = 0; k < 480; ++k) {
    CHECK(away[2u * k] == sine[k] * (0.25f * centre));
    CHECK(toward[2u * k] == sine[k] * centre);
  }
  CHECK(cone_gain(0.5f, cone.inner_cos, cone.outer_cos, 0.25f) > 0.25f);
  CHECK(cone_gain(0.5f, cone.inner_cos, cone.outer_cos, 0.25f) < 1.0f);
}

TEST_CASE("the music bus is 2D: its sources ignore position and map to the front pair") {
  Rig rig;
  const Vector<f32> pair = exact_sine(4800, 96, 0.5f, 2);
  PlayParams play;
  play.clip = rig.add(pair, 2);
  play.bus = k_bus_music;
  play.source.position = Vec3{100.0f, 0.0f, 0.0f};  // far off to the right: must not matter
  REQUIRE_FALSE(rig.mixer.play(play).is_null());
  const Vector<f32> out = rig.render(480);
  for (u32 k = 0; k < 960; ++k)
    CHECK(out[k] == pair[k]);  // unity: balance at the centre

  // `Direct` maps source channel k to layout channel k and ignores the pan.
  Rig direct;
  PlayParams stem = flat(direct.add(pair, 2), 0.9f);
  stem.source.mapping = ChannelMapping::Direct;
  REQUIRE_FALSE(direct.mixer.play(stem).is_null());
  const Vector<f32> mapped = direct.render(480);
  for (u32 k = 0; k < 960; ++k)
    CHECK(mapped[k] == pair[k]);
}

TEST_CASE(
    "the master's declared layout shapes the output, and the decode stage is the only part "
    "that knows it") {
  const Vector<f32> sine = exact_sine(4800, 48, 0.5f);

  // 5.1: the stereo panner feeds the front pair and leaves the centre, the LFE and the surrounds
  // silent — the same front pair a stereo layout gets, sample for sample.
  Rig stereo;
  Rig surround(layout(ChannelLayout::Surround51));
  CHECK(surround.mixer.channels() == 6u);
  CHECK(surround.mixer.layout() == ChannelLayout::Surround51);
  PlayParams play = flat(stereo.add(sine), 0.3f);
  REQUIRE_FALSE(stereo.mixer.play(play).is_null());
  play.clip = surround.add(sine);
  REQUIRE_FALSE(surround.mixer.play(play).is_null());
  const Vector<f32> two = stereo.render(480);
  const Vector<f32> six = surround.render(480);
  REQUIRE(six.size() == 480u * 6u);
  for (u32 k = 0; k < 480; ++k) {
    CHECK(six[6u * k + 0u] == two[2u * k]);
    CHECK(six[6u * k + 1u] == two[2u * k + 1u]);
    for (u32 c = 2; c < 6; ++c)
      CHECK(six[6u * k + c] == 0.0f);
  }

  // Headphones in v0 is the stereo profile: the same two channels, sample for sample.
  Rig phones(layout(ChannelLayout::Headphones));
  play.clip = phones.add(sine);
  REQUIRE_FALSE(phones.mixer.play(play).is_null());
  CHECK(phones.render(480) == two);

  // 7.1.4: twelve channels, the front pair fed, the height ring silent.
  Rig atmos(layout(ChannelLayout::Surround714));
  play.clip = atmos.add(sine);
  REQUIRE_FALSE(atmos.mixer.play(play).is_null());
  const Vector<f32> twelve = atmos.render(480);
  REQUIRE(twelve.size() == 480u * 12u);
  for (u32 k = 0; k < 480; ++k) {
    CHECK(twelve[12u * k + 1u] == two[2u * k + 1u]);
    for (u32 c = 2; c < 12; ++c)
      CHECK(twelve[12u * k + c] == 0.0f);
  }

  // Mono folds: a centred mono source at its own level.
  Rig mono(layout(ChannelLayout::Mono));
  REQUIRE_FALSE(mono.mixer.play(flat(mono.add(sine))).is_null());
  const Vector<f32> one = mono.render(480);
  REQUIRE(one.size() == 480u);
  for (u32 k = 0; k < 480; ++k)
    CHECK(one[k] == sine[k]);

  // An unknown layout is refused in favour of stereo.
  Rig unknown(layout(ChannelLayout::Unknown));
  CHECK(unknown.mixer.layout() == ChannelLayout::Stereo);

  // The null decoder: every voice still plays, ends and reports — the mixer's bookkeeping is all
  // there — and nothing reaches the speakers.
  Rig silent(layout(ChannelLayout::Stereo, &k_null_decoder));
  CHECK(std::string_view{silent.mixer.decoder().name} == "null");
  const Vector<f32> short_clip = exact_sine(96, 48, 0.5f);
  REQUIRE_FALSE(silent.mixer.play(flat(silent.add(short_clip))).is_null());
  const Vector<f32> nothing = silent.render(480);
  for (const f32 s : nothing)
    CHECK(s == 0.0f);
  CHECK(silent.mixer.update() == 1u);
}

TEST_CASE("the profile table and the device classification agree") {
  CHECK(layout_channels(ChannelLayout::Surround71) == 8u);
  CHECK(layout_channels(ChannelLayout::Surround714) == 12u);
  const LayoutInfo& s51 = layout_info(ChannelLayout::Surround51);
  CHECK(s51.channels == 6u);
  CHECK(s51.speakers[3] == Speaker::LowFrequency);
  CHECK(speaker_direction(s51, 3) == Vec3{});
  for (ChannelLayout l :
       {ChannelLayout::Mono, ChannelLayout::Stereo, ChannelLayout::Headphones, ChannelLayout::Quad,
        ChannelLayout::Surround51, ChannelLayout::Surround71, ChannelLayout::Surround714}) {
    const LayoutInfo& info = layout_info(l);
    CHECK(info.channels == layout_channels(l));
    CHECK(info.front_left >= 0);
    // A headphone endpoint reports a stereo map: recognized as stereo, chosen by the setting.
    const ChannelLayout expected = l == ChannelLayout::Headphones ? ChannelLayout::Stereo : l;
    CHECK(layout_from_speakers(std::span<const Speaker>(info.speakers, info.channels)) == expected);
    for (u32 c = 0; c < info.channels; ++c) {
      if (info.speakers[c] == Speaker::LowFrequency) continue;
      CHECK(std::fabs(length(speaker_direction(info, c)) - 1.0f) < 1e-6f);
    }
  }
  // The front pair of a stereo layout is 30 degrees either side of ahead, level with the ears.
  const Vec3 front_right = speaker_direction(layout_info(ChannelLayout::Stereo), 1);
  CHECK(static_cast<f64>(front_right.x) == doctest::Approx(0.5).epsilon(1e-6));
  CHECK(static_cast<f64>(front_right.z) == doctest::Approx(-std::sqrt(0.75)).epsilon(1e-6));
  CHECK(std::fabs(front_right.y) < 1e-6f);
  // 7.1.4's height ring is 45 degrees up: y = sin(45).
  const Vec3 top = speaker_direction(layout_info(ChannelLayout::Surround714), 8);
  CHECK(static_cast<f64>(top.y) == doctest::Approx(std::sqrt(0.5)).epsilon(1e-6));
  CHECK(top.x < 0.0f);  // top front left
  CHECK(top.z < 0.0f);
  // Windows' "5.1 surround" labels its surrounds as sides; it is still 5.1.
  const Speaker side51[] = {Speaker::FrontLeft,    Speaker::FrontRight, Speaker::FrontCentre,
                            Speaker::LowFrequency, Speaker::SideLeft,   Speaker::SideRight};
  CHECK(layout_from_speakers(side51) == ChannelLayout::Surround51);
  const Speaker odd[] = {Speaker::FrontRight, Speaker::FrontLeft};
  CHECK(layout_from_speakers(odd) == ChannelLayout::Unknown);
  CHECK(layout_info(ChannelLayout::Unknown).channels == 0u);
}

TEST_CASE("the master clips hard at full scale and counts what it clipped") {
  Rig rig;
  const Vector<f32> loud(960, 0.9f);
  const ClipHandle clip = rig.add(loud);
  REQUIRE_FALSE(rig.mixer.play(flat(clip, -1.0f)).is_null());
  REQUIRE_FALSE(rig.mixer.play(flat(clip, -1.0f)).is_null());  // 0.9 + 0.9 on the left
  const Vector<f32> out = rig.render(480);
  for (u32 k = 0; k < 480; ++k) {
    CHECK(out[2u * k] == 1.0f);
    CHECK(out[2u * k + 1u] == 0.0f);
  }
  const MixerStats stats = rig.mixer.stats();
  CHECK(stats.clipped_samples == 480u);
  CHECK(static_cast<f64>(stats.peak) == doctest::Approx(1.8));
  CHECK(stats.voices_peak == 2u);
  CHECK(stats.blocks == 1u);
  CHECK(stats.frames == 480u);
}

TEST_CASE("render with no frames applies commands and mixes nothing") {
  Rig rig;
  const Vector<f32> sine = exact_sine(480, 48, 0.1f);
  REQUIRE_FALSE(rig.mixer.play(flat(rig.add(sine))).is_null());
  REQUIRE(rig.mixer.set_bus_gain(k_bus_master, 0.25f));
  rig.mixer.render(nullptr, 0);
  const MixerStats stats = rig.mixer.stats();
  CHECK(stats.commands == 2u);
  CHECK(stats.blocks == 0u);
  CHECK(stats.frames == 0u);
}

// What a voice costs the audio thread (docs/subsystems/audio.md, "Performance notes"): one 10 ms
// block (480 frames) per iteration, N voices playing, so the median divided by N is the cost of one
// voice for one block — the number a voice budget is set from. Items are voices, so "items per
// second" is voice-blocks per second.
//
//   audio.mix.3d       mono clips at assorted pitches, positioned around the listener: the voice
//                      loop's interpolating path, the source model and the stereo panner
//   audio.mix.2d       stereo clips at pitch 1, 2D on the music bus: the voice loop's cheapest path
//   audio.mix.3d_51    the first, declared 5.1: what a wider layout's stride costs the panner
//   audio.mix.3d_turning
//                      the first, with the listener turning a little every block, so every voice
//                      has a pan change in flight every block: the fixed-time ramp's price, where
//                      the rows above are its fast path (the plain multiply-add)
//
// The clips are a second long and loop, so the pool is full for every block measured.

#include <core/containers/vector.h>
#include <domain/audio/audio.h>
#include <foundation/bench/bench.h>

using namespace engine;
using namespace engine::audio;

namespace {

Vector<f32> noise(u32 frames, u32 channels, u64 seed) {
  Vector<f32> out;
  out.resize_exact(frames * channels);
  u64 state = seed;
  for (u32 i = 0; i < out.size(); ++i) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    out[i] = static_cast<f32>(static_cast<i32>(state >> 40) - (1 << 23)) / 8388608.0f * 0.1f;
  }
  return out;
}

enum class Kind { Positioned, Flat, Turning };

void run(bench::State& state, Kind kind, ChannelLayout layout) {
  const u32 voices = static_cast<u32>(state.arg());
  ClipStore clips;
  MixerConfig config;
  config.voices = voices;
  config.layout = layout;
  Mixer mixer(clips, config);
  const u8 channels = kind == Kind::Flat ? 2 : 1;
  const Vector<f32> pcm = noise(k_sample_rate, channels, 1234);
  const ClipHandle clip = clips.add_pcm(Id128{1, 1}, pcm, channels);

  for (u32 v = 0; v < voices; ++v) {
    PlayParams p;
    p.clip = clip;
    p.loop = true;
    p.start_frame = v * 997u;
    if (kind == Kind::Flat) {
      p.bus = k_bus_music;
      p.pan = static_cast<f32>(v % 9u) / 4.0f - 1.0f;
    } else {
      p.pitch = 0.8f + 0.013f * static_cast<f32>(v % 31u);
      p.source.position =
          Vec3{static_cast<f32>(v % 11u) - 5.0f, 0.0f, -1.0f - static_cast<f32>(v % 7u)};
      p.source.max_distance = 40.0f;
    }
    mixer.play(p);
  }

  Vector<f32> out;
  out.resize_exact(480u * mixer.channels());
  mixer.render(out.data(), 480);  // applies the plays; the first block is not the one measured
  u32 turn = 0;
  while (state.keep_running()) {
    if (kind == Kind::Turning) {
      // One command a block, and every positioned voice's pan moves with it.
      Listener listener;
      listener.forward = Vec3{0.01f * static_cast<f32>(++turn % 64u), 0.0f, -1.0f};
      mixer.set_listener(listener);
    }
    mixer.render(out.data(), 480);
    bench::keep(out[0]);
  }
  state.set_items(voices);
}

}  // namespace

ENGINE_BENCH_ARGS(mix_3d, "audio.mix.3d", 1, 16, 64, 256) {
  run(state, Kind::Positioned, ChannelLayout::Stereo);
}

ENGINE_BENCH_ARGS(mix_2d, "audio.mix.2d", 1, 16, 64, 256) {
  run(state, Kind::Flat, ChannelLayout::Stereo);
}

ENGINE_BENCH_ARGS(mix_3d_51, "audio.mix.3d_51", 64) {
  run(state, Kind::Positioned, ChannelLayout::Surround51);
}

ENGINE_BENCH_ARGS(mix_3d_turning, "audio.mix.3d_turning", 64) {
  run(state, Kind::Turning, ChannelLayout::Stereo);
}

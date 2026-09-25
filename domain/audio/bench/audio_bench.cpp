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
//   audio.mix.3d_limiter_idle
//                      the first, with the master's limiter on and the mix a quarter of its level:
//                      the limiter at rest, which only delays the block
//   audio.mix.3d_limited
//                      the first, with the limiter on and the master bus at 4x: its per-frame path
//                      on every frame, the worst it costs
//   audio.mix.3d_distinct
//                      the first with a clip of its own for every voice — 64 clips, 12 MB, where
//                      the rows above share one clip that stays in L2: what memory costs a voice,
//                      apart from its arithmetic
//   audio.mix.stream   the first with every voice streamed: the callback reading each voice's ring
//                      instead of its clip. The fills run between blocks with the clock stopped,
//                      so the row is the audio thread's cost alone, beside `audio.mix.3d`'s
//
// The clips are a second long and loop, so the pool is full for every block measured.
//
// And the other half of a stream, the decode job (docs/subsystems/audio.md, "Streaming"): one
// iteration is one fill of the default fill-ahead, 250 ms (12,000 frames of stereo), which is what
// a streaming voice asks of the Efficiency pool four times a second. Items are frames.
//
//   audio.stream.fill_wav        16-bit stereo WAV the store holds in memory: the conversion alone
//   audio.stream.fill_wav_file   the same WAV read from a file by range, a ranged read a fill
//   audio.stream.fill_flac       16-bit stereo FLAC in memory (verbatim subframes: the framing and
//                                its CRCs, not a predictor — a lower bound on a real FLAC)
//   audio.stream.fill_resample   the WAV at 44.1 kHz: the linear resampler at its highest order

#include "../tests/audio_test_support.h"

#include <core/containers/vector.h>
#include <domain/audio/audio.h>
#include <foundation/bench/bench.h>

#include <test_temp_dir.h>

#include <fstream>
#include <string>

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

enum class Kind { Positioned, Flat, Turning, LimiterIdle, Limited, Distinct };

void run(bench::State& state, Kind kind, ChannelLayout layout) {
  const u32 voices = static_cast<u32>(state.arg());
  ClipStore clips;
  MixerConfig config;
  config.voices = voices;
  config.layout = layout;
  const bool limiter = kind == Kind::LimiterIdle || kind == Kind::Limited;
  config.limiter = limiter ? LimiterMode::On : LimiterMode::Off;
  Mixer mixer(clips, config);
  if (limiter) mixer.set_bus_gain(k_bus_master, kind == Kind::Limited ? 4.0f : 0.25f);
  const u8 channels = kind == Kind::Flat ? 2 : 1;
  const Vector<f32> pcm = noise(k_sample_rate, channels, 1234);
  const ClipHandle clip = clips.add_pcm(Id128{1, 1}, pcm, channels);
  // A clip of its own for every voice, for the row that prices memory rather than arithmetic.
  Vector<ClipHandle> distinct;
  if (kind == Kind::Distinct) {
    for (u32 v = 0; v < voices; ++v)
      distinct.push_back(clips.add_pcm(Id128{2, v}, noise(k_sample_rate, 1, 77u + v), 1));
  }

  for (u32 v = 0; v < voices; ++v) {
    PlayParams p;
    p.clip = kind == Kind::Distinct ? distinct[v] : clip;
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

ENGINE_BENCH_ARGS(mix_3d_limiter_idle, "audio.mix.3d_limiter_idle", 64) {
  run(state, Kind::LimiterIdle, ChannelLayout::Stereo);
}

ENGINE_BENCH_ARGS(mix_3d_limited, "audio.mix.3d_limited", 64) {
  run(state, Kind::Limited, ChannelLayout::Stereo);
}

ENGINE_BENCH_ARGS(mix_3d_distinct, "audio.mix.3d_distinct", 64) {
  run(state, Kind::Distinct, ChannelLayout::Stereo);
}

namespace {

// `audio.mix.3d` with every voice streamed from the WAV of the same noise: the ring read. The fills
// run where they run in a game, on the job system's Efficiency pool — on another core, so the
// decoder's code and data do not pass through the caches of the thread that mixes — and the
// loop waits for them with the clock stopped, so no voice runs dry.
void run_streamed(bench::State& state) {
  const u32 voices = static_cast<u32>(state.arg());
  const Vector<f32> pcm = noise(k_sample_rate, 1, 1234);
  const Vector<u8> wav = audio::test::wav_s16(audio::test::to_s16(pcm), 1, k_sample_rate);
  jobs::JobSystemConfig jobs_config;
  jobs_config.performance_workers = 1;
  jobs_config.efficiency_workers = 2;
  jobs::JobSystem jobs(jobs_config);
  ClipStoreConfig store_config;
  store_config.stream_threshold_bytes = 1024;
  store_config.jobs = &jobs;
  ClipStore clips(store_config);
  const ClipHandle clip = clips.load(Id128{1, 1}, {wav.data(), wav.size()});
  clips.wait();
  MixerConfig config;
  config.voices = voices;
  config.streams = voices;
  Mixer mixer(clips, config);
  for (u32 v = 0; v < voices; ++v) {
    PlayParams p;
    p.clip = clip;
    p.loop = true;
    p.start_frame = v * 997u;
    p.pitch = 0.8f + 0.013f * static_cast<f32>(v % 31u);
    p.source.position =
        Vec3{static_cast<f32>(v % 11u) - 5.0f, 0.0f, -1.0f - static_cast<f32>(v % 7u)};
    p.source.max_distance = 40.0f;
    mixer.play(p);
  }
  mixer.wait_streams();
  Vector<f32> out;
  out.resize_exact(480u * mixer.channels());
  mixer.render(out.data(), 480);
  while (state.keep_running()) {
    state.pause_timing();
    mixer.update();  // the fills: the decode job's side, measured by the rows below
    mixer.wait_streams();
    state.resume_timing();
    mixer.render(out.data(), 480);
    bench::keep(out[0]);
  }
  state.set_items(voices);
}

enum class Source { Wav, WavFile, Flac, Resample };

// One fill of the fill-ahead per iteration: the voice plays it away with the clock stopped, and
// the next `update()` — the one timed — tops the ring up by the same amount.
void run_fill(bench::State& state, Source kind) {
  const u32 rate = kind == Source::Resample ? 44100u : k_sample_rate;
  const Vector<f32> pcm = noise(10u * rate, 2, 99);  // ten seconds, looped
  const Vector<i16> s16 = audio::test::to_s16(pcm);
  const Vector<u8> file =
      kind == Source::Flac ? audio::test::flac_s16(s16, 2) : audio::test::wav_s16(s16, 2, rate);
  const engine::test::TempDir tmp("audio_bench_stream");
  const std::string path = tmp.file("clip.bin");
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(file.data()),
              static_cast<std::streamsize>(file.size()));
  }
  ClipStoreConfig store_config;
  store_config.stream_threshold_bytes = 1024;
  ClipStore clips(store_config);
  const ClipHandle clip = kind == Source::WavFile
                              ? clips.load_file(Id128{2, 1}, path)
                              : clips.load(Id128{2, 1}, {file.data(), file.size()});
  MixerConfig config;
  config.voices = 1;
  config.streams = 1;
  Mixer mixer(clips, config);
  PlayParams p;
  p.clip = clip;
  p.loop = true;
  p.bus = k_bus_music;
  mixer.play(p);
  const u32 fill = mixer.stream_fill_frames();
  Vector<f32> out;
  out.resize_exact(fill * 2u);
  while (state.keep_running()) {
    state.pause_timing();
    mixer.render(out.data(), fill);  // plays the fill-ahead's worth away: the ring is now low
    state.resume_timing();
    mixer.update();  // one fill, of the same amount
  }
  bench::keep(out[0]);
  state.set_items(fill);
}

}  // namespace

ENGINE_BENCH_ARGS(mix_stream, "audio.mix.stream", 1, 16, 64) { run_streamed(state); }

ENGINE_BENCH(stream_fill_wav, "audio.stream.fill_wav") { run_fill(state, Source::Wav); }
ENGINE_BENCH(stream_fill_wav_file, "audio.stream.fill_wav_file") {
  run_fill(state, Source::WavFile);
}
ENGINE_BENCH(stream_fill_flac, "audio.stream.fill_flac") { run_fill(state, Source::Flac); }
ENGINE_BENCH(stream_fill_resample, "audio.stream.fill_resample") {
  run_fill(state, Source::Resample);
}

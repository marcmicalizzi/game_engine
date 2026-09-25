// Streaming (stream.h, docs/subsystems/audio.md "Streaming"): a clip over the threshold is played
// through a ring a fill keeps ahead of the voice, from the bytes the store keeps or from the file
// by range. The claim the tests hold it to is that a stream that never runs dry mixes to exactly
// the bytes the same clip does stored — one-shots running out, loops going round, pitch through
// the interpolating path, starts in the middle, a ring much shorter than the clip — and that one
// that does run dry holds its place, plays silence, counts it and says so once.
#include "audio_test_support.h"

#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/log/log.h>
#include <domain/audio/audio.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>
#include <tuple>

using namespace engine;
using namespace engine::audio;
using namespace engine::audio::test;

namespace {

std::span<const u8> bytes(const Vector<u8>& v) { return {v.data(), v.size()}; }

bool write_bytes(const std::string& path, const Vector<u8>& data) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

// A clip with something in every sample: a sine through the exact polynomial plus noise, so an
// off-by-one frame anywhere changes the bytes.
Vector<f32> test_signal(u32 frames, u32 channels, u64 seed) {
  const Vector<f32> sine = exact_sine(frames, 96, 0.4f);
  const Vector<f32> noise = lcg_noise(frames * channels, seed, 0.2f);
  Vector<f32> out;
  out.resize_exact(frames * channels);
  for (u32 i = 0; i < frames; ++i) {
    for (u32 c = 0; c < channels; ++c)
      out[i * channels + c] = (c == 0 ? sine[i] : -sine[i]) + noise[i * channels + c];
  }
  return out;
}

constexpr u32 k_block = 480;

struct Played {
  Vector<f32> out;
  MixerStats stats;
  ControlStats control;
  u32 streams_in_use = 0;  // after the last update
};

// One voice playing `clip` for `blocks` blocks, `update()` before each block as a tick would.
Played play_blocks(ClipStore& store, ClipHandle clip, PlayParams params, u32 blocks,
                   const MixerConfig& config = {}) {
  Played result;
  Mixer mixer(store, config);
  params.clip = clip;
  const VoiceHandle voice = mixer.play(params);
  REQUIRE_FALSE(voice.is_null());
  Vector<f32> block;
  block.resize_exact(k_block * mixer.channels());
  for (u32 b = 0; b < blocks; ++b) {
    mixer.update();
    mixer.wait_streams();  // with a job system, so nothing runs dry: this test is not about timing
    mixer.render(block.data(), k_block);
    result.out.append(std::span<const f32>(block.data(), block.size()));
  }
  mixer.update();
  result.stats = mixer.stats();
  result.control = mixer.control_stats();
  result.streams_in_use = mixer.streams_in_use();
  return result;
}

MixerConfig small_ring() {
  MixerConfig config;
  config.streams = 2;
  config.stream_ring_frames = 3000;  // far shorter than the clips: the ring goes round many times
  config.stream_fill_frames = 2000;
  return config;
}

ClipStoreConfig streaming_store() {
  ClipStoreConfig config;
  config.stream_threshold_bytes = 1024;  // everything the tests load streams
  return config;
}

struct Case {
  const char* name;
  u32 frames;
  u32 channels;
  f32 pitch;
  bool loop;
  u32 start;
  u32 blocks;
};

}  // namespace

TEST_CASE("a streamed clip mixes to exactly the bytes of the same clip stored") {
  const engine::test::TempDir tmp("audio_stream");
  REQUIRE(tmp.ok());
  const Case cases[] = {
      // Runs out part-way: the voice ends on the silent frame the fill wrote after the last.
      {"mono one-shot", 20000, 1, 1.0f, false, 0, 50},
      // Loops round three times at a pitch that interpolates, from a start past the middle.
      {"stereo loop", 9000, 2, 1.37f, true, 6789, 60},
      // Slower than the mix rate, so the playhead crosses the loop point between two frames.
      {"mono slow loop", 7001, 1, 0.61f, true, 0, 40},
      // A one-shot started near its end at a high pitch: the end arrives within the first fill.
      {"stereo late one-shot", 12000, 2, 2.5f, false, 11000, 4},
  };
  for (const Case& c : cases) {
    CAPTURE(c.name);
    const Vector<f32> signal = test_signal(c.frames, c.channels, 7 + c.frames);
    const Vector<u8> wav = wav_s16(to_s16(signal), c.channels, k_sample_rate);
    const std::string path = tmp.file(std::string(c.name) + ".wav");
    REQUIRE(write_bytes(path, wav));

    PlayParams params;
    params.pitch = c.pitch;
    params.loop = c.loop;
    params.start_frame = c.start;
    params.bus = k_bus_music;  // 2D: the mapping at unity, so the bytes are the clip's own

    ClipStore stored;
    const ClipHandle stored_clip = stored.load(Id128{1, 1}, bytes(wav));
    REQUIRE(stored.state(stored_clip) == ClipState::Ready);
    REQUIRE_FALSE(stored.info(stored_clip).streamed);

    ClipStore from_memory(streaming_store());
    const ClipHandle memory_clip = from_memory.load(Id128{1, 1}, bytes(wav));
    REQUIRE(from_memory.state(memory_clip) == ClipState::Ready);
    REQUIRE(from_memory.info(memory_clip).streamed);
    CHECK(from_memory.info(memory_clip).frames == c.frames);
    CHECK(from_memory.resident_bytes() == wav.size());  // the bytes it streams from, no samples

    ClipStore from_file(streaming_store());
    const ClipHandle file_clip = from_file.load_file(Id128{1, 1}, path);
    REQUIRE(from_file.state(file_clip) == ClipState::Ready);
    REQUIRE(from_file.info(file_clip).streamed);
    CHECK(from_file.resident_bytes() == 0u);  // nothing: the file is read by range as it plays

    const Played a = play_blocks(stored, stored_clip, params, c.blocks);
    const Played b = play_blocks(from_memory, memory_clip, params, c.blocks, small_ring());
    const Played d = play_blocks(from_file, file_clip, params, c.blocks, small_ring());
    CHECK(b.control.stream_plays == 1u);
    CHECK(d.control.stream_plays == 1u);
    CHECK(b.stats.underrun_frames == 0u);
    CHECK(d.stats.underrun_frames == 0u);
    CHECK(b.control.stream_underruns == 0u);
    // The ring was topped up as it played, not filled once — wherever the voice read more of the
    // clip than the ring holds (the late one-shot reads 1000 frames of it).
    if (c.blocks > 10) CHECK(b.control.stream_fills > 1u);
    const u64 hash = hash_mix(ChannelLayout::Stereo, {a.out.data(), a.out.size()});
    CHECK(hash_mix(ChannelLayout::Stereo, {b.out.data(), b.out.size()}) == hash);
    CHECK(hash_mix(ChannelLayout::Stereo, {d.out.data(), d.out.size()}) == hash);
    CHECK(b.out == a.out);
    CHECK(d.out == a.out);
    // The one-shots ended — the stored voice and the streamed ones on the same block — and the
    // streams went back to the pool.
    CHECK(a.stats.voices_playing == (c.loop ? 1u : 0u));
    CHECK(b.stats.voices_playing == a.stats.voices_playing);
    CHECK(d.stats.voices_playing == a.stats.voices_playing);
    if (!c.loop) {
      CHECK(b.streams_in_use == 0u);
      CHECK(d.streams_in_use == 0u);
    }
  }
}

TEST_CASE("a FLAC and a 44.1 kHz WAV stream to the bytes they decode to stored") {
  const engine::test::TempDir tmp("audio_stream_formats");
  REQUIRE(tmp.ok());
  PlayParams params;
  params.loop = true;
  params.pitch = 0.9f;
  params.bus = k_bus_music;

  // FLAC: a decoder with frames of its own (4096 samples) that the fill's reads cut across.
  const Vector<f32> signal = test_signal(15000, 2, 99);
  const Vector<u8> flac = flac_s16(to_s16(signal), 2);
  const std::string flac_path = tmp.file("clip.flac");
  REQUIRE(write_bytes(flac_path, flac));
  // 44.1 kHz: the resampler, run in the fill's pieces rather than the store's whole.
  const Vector<f32> sine = exact_sine(30000, 48, 0.5f);
  const Vector<u8> resampled = wav_s16(to_s16(sine), 1, 44100);
  const std::string resampled_path = tmp.file("clip44.wav");
  REQUIRE(write_bytes(resampled_path, resampled));

  for (const auto& [name, file, path] :
       {std::tuple{"flac", &flac, flac_path}, std::tuple{"44.1 kHz", &resampled, resampled_path}}) {
    CAPTURE(name);
    ClipStore stored;
    const ClipHandle stored_clip = stored.load(Id128{2, 1}, bytes(*file));
    REQUIRE(stored.state(stored_clip) == ClipState::Ready);
    ClipStore streamed(streaming_store());
    const ClipHandle streamed_clip = streamed.load_file(Id128{2, 1}, path);
    REQUIRE(streamed.state(streamed_clip) == ClipState::Ready);
    REQUIRE(streamed.info(streamed_clip).streamed);
    // The clip's length as the store reports it is the decoder's own answer before decoding. For
    // the FLAC it is exact; for the resampled clip it is miniaudio's arithmetic estimate, which
    // comes out a frame short of what the resampler then produces (32,653 against 32,654). The mix
    // does not depend on it — a stream goes round when the decoder says it has ended, which is
    // where the stored clip ends — and the bytes below are the same; a start frame taken modulo
    // the length, and a caller's clock, can differ by that frame.
    const i64 frames_streamed = streamed.info(streamed_clip).frames;
    const i64 frames_stored = stored.info(stored_clip).frames;
    CHECK(frames_streamed - frames_stored <= 0);
    CHECK(frames_streamed - frames_stored >= -1);
    if (std::string_view(name) == "flac") CHECK(frames_streamed == frames_stored);

    const Played a = play_blocks(stored, stored_clip, params, 90);
    const Played b = play_blocks(streamed, streamed_clip, params, 90, small_ring());
    CHECK(b.stats.underrun_frames == 0u);
    CHECK(b.out == a.out);
  }
}

TEST_CASE("a stream that runs dry holds its place, plays silence, counts it and says so once") {
  log::RingSink ring(64);
  log::add_sink(&ring);

  const Vector<f32> signal = test_signal(40000, 1, 3);
  const Vector<u8> wav = wav_s16(to_s16(signal), 1, k_sample_rate);
  ClipStore stored;
  const ClipHandle stored_clip = stored.load(Id128{3, 1}, bytes(wav));
  ClipStore streamed(streaming_store());
  const ClipHandle streamed_clip = streamed.load(Id128{3, 1}, bytes(wav));
  REQUIRE(streamed.info(streamed_clip).streamed);

  MixerConfig config;
  config.streams = 1;
  config.stream_ring_frames = 2000;
  config.stream_fill_frames = 1500;
  Mixer mixer(streamed, config);
  PlayParams params;
  params.clip = streamed_clip;
  params.bus = k_bus_music;
  const VoiceHandle voice = mixer.play(params);  // the first fill runs here: 1999 frames
  REQUIRE_FALSE(voice.is_null());

  // Five blocks and no tick between them: 2400 frames asked of a ring the fill left 1999 in.
  Vector<f32> out(5u * k_block * 2u, 0.0f);
  for (u32 b = 0; b < 5; ++b)
    mixer.render(out.data() + b * k_block * 2u, k_block);
  const MixerStats dry = mixer.stats();
  // The voice played frames 0..1997 (the last one needs frame 1998 as its right tap, and 1998 is
  // the last filled); every frame after that in the five blocks is silence, counted.
  CHECK(dry.underrun_frames == 5u * k_block - 1998u);
  CHECK(dry.voices_playing == 1u);  // held, not ended
  const Vector<f32> head = [&] {
    ClipStore& s = stored;
    Mixer reference(s);
    PlayParams p = params;
    p.clip = stored_clip;
    REQUIRE_FALSE(reference.play(p).is_null());
    Vector<f32> r(5u * k_block * 2u, 0.0f);
    reference.render(r.data(), 5u * k_block);
    return r;
  }();
  for (u32 i = 0; i < 1998u * 2u; ++i)
    REQUIRE(out[i] == head[i]);
  for (u32 i = 1998u * 2u; i < out.size(); ++i)
    REQUIRE(out[i] == 0.0f);

  // A tick: the underrun is logged, once, with the voice and the clip, and the ring is topped up.
  mixer.update();
  CHECK(mixer.control_stats().stream_underruns == 1u);
  u32 logged = 0;
  ring.for_each(0, [&](const log::RingSink::Entry& entry) {
    if (entry.message == std::string_view("stream underrun")) ++logged;
  });
  CHECK(logged == 1u);

  // It picks up where it held: the next block is the clip from frame 1998 on, exactly as a stored
  // voice plays it from there.
  Vector<f32> next(k_block * 2u, 0.0f);
  mixer.render(next.data(), k_block);
  {
    Mixer reference(stored);
    PlayParams p = params;
    p.clip = stored_clip;
    p.start_frame = 1998;
    REQUIRE_FALSE(reference.play(p).is_null());
    Vector<f32> r(k_block * 2u, 0.0f);
    reference.render(r.data(), k_block);
    CHECK(next == r);
  }

  // Dry again, a tick again: still counted, not logged again.
  for (u32 b = 0; b < 8; ++b)
    mixer.render(out.data(), k_block);
  mixer.update();
  CHECK(mixer.stats().underrun_frames > dry.underrun_frames);
  CHECK(mixer.control_stats().stream_underruns == 1u);
  logged = 0;
  ring.for_each(0, [&](const log::RingSink::Entry& entry) {
    if (entry.message == std::string_view("stream underrun")) ++logged;
  });
  CHECK(logged == 1u);
  log::remove_sink(&ring);
}

TEST_CASE("fills run on the Efficiency pool, and a voice waits for its first without starving") {
  jobs::JobSystemConfig jobs_config;
  jobs_config.performance_workers = 1;
  jobs_config.efficiency_workers = 2;
  jobs_config.pin_threads = false;
  jobs::JobSystem jobs(jobs_config);

  const Vector<f32> signal = test_signal(30000, 2, 11);
  const Vector<u8> wav = wav_s16(to_s16(signal), 2, k_sample_rate);
  ClipStore stored;
  const ClipHandle stored_clip = stored.load(Id128{4, 1}, bytes(wav));
  ClipStoreConfig store_config = streaming_store();
  store_config.jobs = &jobs;
  ClipStore streamed(store_config);
  const ClipHandle streamed_clip = streamed.load(Id128{4, 1}, bytes(wav));
  streamed.wait();
  REQUIRE(streamed.info(streamed_clip).streamed);

  PlayParams params;
  params.loop = true;
  params.pitch = 1.21f;
  params.bus = k_bus_music;
  const u64 executed = jobs.stats().jobs_executed;
  const Played a = play_blocks(stored, stored_clip, params, 80);
  const Played b = play_blocks(streamed, streamed_clip, params, 80, small_ring());
  CHECK(jobs.stats().jobs_executed >= executed + b.control.stream_fills);
  CHECK(b.control.stream_fills > 5u);
  CHECK(b.stats.underrun_frames == 0u);
  CHECK(b.out == a.out);

  // Played without waiting, the first fill may land after the first block: the voice holds at its
  // first frame, silent, and that is a start rather than an underrun.
  Mixer mixer(streamed, small_ring());
  PlayParams p = params;
  p.clip = streamed_clip;
  REQUIRE_FALSE(mixer.play(p).is_null());
  Vector<f32> block(k_block * 2u, 0.0f);
  mixer.render(block.data(), k_block);
  mixer.wait_streams();
  // The first fill publishes 2999 frames at once, so the block saw none or all of them.
  CHECK(mixer.stats().underrun_frames == 0u);
}

TEST_CASE("the stream pool is a budget: a full one refuses, and streams come back when voices go") {
  const Vector<f32> signal = test_signal(20000, 1, 5);
  const Vector<u8> wav = wav_s16(to_s16(signal), 1, k_sample_rate);
  ClipStore store(streaming_store());
  const ClipHandle clip = store.load(Id128{5, 1}, bytes(wav));
  const Vector<f32> short_tone = exact_sine(4800, 48, 0.5f);
  const ClipHandle stored_clip = store.add_pcm(Id128{5, 2}, short_tone, 1);

  MixerConfig config = small_ring();
  config.streams = 1;
  config.voices = 2;
  Mixer mixer(store, config);
  CHECK(mixer.stream_count() == 1u);
  PlayParams p;
  p.clip = clip;
  p.loop = true;
  p.priority = 10;
  const VoiceHandle first = mixer.play(p);
  REQUIRE_FALSE(first.is_null());
  CHECK(mixer.streams_in_use() == 1u);
  CHECK(mixer.play(p).is_null());  // a voice is free, a stream is not
  CHECK(mixer.control_stats().refused_stream == 1u);
  CHECK(mixer.control_stats().refused_pool == 0u);

  // Stolen: a louder stored one-shot takes the streamed voice's slot. Its stream is read until the
  // audio thread applies the steal, and free after.
  Vector<f32> out(k_block * 2u, 0.0f);
  mixer.render(out.data(), k_block);
  PlayParams loud;
  loud.clip = stored_clip;
  loud.priority = 200;
  REQUIRE_FALSE(mixer.play(loud).is_null());  // takes the free voice
  REQUIRE_FALSE(mixer.play(loud).is_null());  // steals the stream's
  CHECK(mixer.control_stats().steals == 1u);
  CHECK_FALSE(mixer.is_live(first));
  mixer.update();
  CHECK(mixer.streams_in_use() == 1u);  // the Play that stole it has not been applied yet
  mixer.render(out.data(), k_block);
  mixer.update();
  CHECK(mixer.streams_in_use() == 0u);

  // A streamed voice that is stopped gives its stream back once its fade has run.
  const VoiceHandle again = mixer.play(p);
  REQUIRE(again.is_null());  // both voices are the loud one-shots now, which outrank it
  mixer.render(out.data(), k_block);
  for (u32 b = 0; b < 12; ++b)
    mixer.render(out.data(), k_block);  // the one-shots run out (4800 frames)
  mixer.update();
  const VoiceHandle third = mixer.play(p);
  REQUIRE_FALSE(third.is_null());
  CHECK(mixer.streams_in_use() == 1u);
  mixer.render(out.data(), k_block);
  CHECK(mixer.stop(third));
  mixer.render(out.data(), k_block);  // the 10 ms fade is one 480-frame block
  mixer.update();
  CHECK_FALSE(mixer.is_live(third));
  CHECK(mixer.streams_in_use() == 0u);
  CHECK(mixer.stats().events_dropped == 0u);
}

TEST_CASE("a stream is topped up to its ring when it has less than the fill-ahead decoded") {
  const Vector<f32> signal = test_signal(48000, 1, 21);
  const Vector<u8> wav = wav_s16(to_s16(signal), 1, k_sample_rate);
  ClipStore clips(streaming_store());
  const ClipHandle clip = clips.load(Id128{6, 1}, bytes(wav));
  Mixer mixer(clips);  // the defaults: a 500 ms ring kept 250 ms ahead
  CHECK(mixer.stream_ring_frames() == 24000u);
  CHECK(mixer.stream_fill_frames() == 12000u);
  PlayParams p;
  p.clip = clip;
  p.loop = true;
  p.pitch = 0.8f;  // 384 frames of the clip a block
  REQUIRE_FALSE(mixer.play(p).is_null());
  CHECK(mixer.control_stats().stream_fills == 1u);  // the first, at play: 23,999 frames
  Vector<f32> out(k_block * 2u, 0.0f);
  Vector<u32> filled_at;
  for (u32 b = 0; b < 200; ++b) {
    const u64 before = mixer.control_stats().stream_fills;
    mixer.update();
    if (mixer.control_stats().stream_fills != before) filled_at.push_back(b);
    mixer.render(out.data(), k_block);
  }
  // Each fill leaves 23,999 frames decoded ahead, and the tick that finds fewer than 12,000 is the
  // one after 32 blocks (31 blocks is 11,904 frames played, 32 is 12,288). One fill a quarter
  // second or so, as the tunables say — not one a block.
  REQUIRE(filled_at.size() == 6u);
  for (u32 i = 0; i < filled_at.size(); ++i)
    CHECK(filled_at[i] == 32u * (i + 1u));
  CHECK(mixer.stats().underrun_frames == 0u);
}

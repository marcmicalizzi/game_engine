// Eviction (clip_store.h, docs/subsystems/audio.md "Eviction"): the store's budget evicts the clips
// no voice holds, least recently used first, when a decode would not fit; a clip a voice holds is
// never evicted; everything resident in use means the decode is refused. The mixer tells the store
// what its voices hold at the tick — a play retains, the voice's end or the applied steal releases
// — so the audio thread takes no part in it.
#include "audio_test_support.h"

#include <core/jobs/job_system.h>
#include <domain/audio/audio.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>

using namespace engine;
using namespace engine::audio;
using namespace engine::audio::test;

namespace {

constexpr u32 k_frames = 4800;                             // 100 ms of mono
constexpr u64 k_clip_bytes = u64{k_frames} * sizeof(f32);  // 19,200 bytes decoded
constexpr u32 k_block = 480;

bool write_bytes(const std::string& path, const Vector<u8>& data) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

// Six different clips as WAV files, and a store whose budget holds three of them decoded.
struct Fixture {
  engine::test::TempDir tmp{"audio_eviction"};
  Vector<std::string> paths;
  Vector<Vector<f32>> signals;

  Fixture() {
    REQUIRE(tmp.ok());
    for (u32 i = 0; i < 6; ++i) {
      signals.push_back(exact_sine(k_frames, 48u * (i + 1u), 0.3f));
      paths.push_back(tmp.file("clip" + std::to_string(i) + ".wav"));
      REQUIRE(write_bytes(paths[i], wav_s16(to_s16(signals[i]), 1, k_sample_rate)));
    }
  }
};

ClipStoreConfig three_clips(jobs::JobSystem* jobs = nullptr) {
  ClipStoreConfig config;
  config.budget_bytes = 3u * k_clip_bytes + 100u;
  config.jobs = jobs;
  return config;
}

PlayParams looping(ClipHandle clip) {
  PlayParams p;
  p.clip = clip;
  p.loop = true;
  p.bus = k_bus_music;
  return p;
}

}  // namespace

TEST_CASE("the budget evicts the least recently used clip no voice holds, and nothing else") {
  Fixture f;
  ClipStore store(three_clips());
  MixerConfig config;
  config.voices = 4;
  config.streams = 4;
  Mixer mixer(store, config);
  Vector<f32> out(k_block * 2u, 0.0f);

  ClipHandle clips[6];
  for (u32 i = 0; i < 3; ++i) {
    clips[i] = store.load_file(Id128{10, i}, f.paths[i]);
    REQUIRE(store.state(clips[i]) == ClipState::Ready);
    REQUIRE(store.info(clips[i]).resident);
    CHECK(store.info(clips[i]).evictable);
  }
  CHECK(store.resident_bytes() == 3u * k_clip_bytes);

  // Clip 0 is played: held. Its samples are what the voice reads.
  const VoiceHandle a = mixer.play(looping(clips[0]));
  REQUIRE_FALSE(a.is_null());
  CHECK(store.info(clips[0]).refs == 1u);
  const f32* a_samples = store.view(clips[0]).samples;
  REQUIRE(a_samples != nullptr);
  mixer.render(out.data(), k_block);

  // A fourth clip: one must go. Clip 0 is held; of 1 and 2, clip 1 was used least recently.
  clips[3] = store.load_file(Id128{10, 3}, f.paths[3]);
  CHECK(store.info(clips[3]).resident);
  CHECK(store.evictions() == 1u);
  CHECK_FALSE(store.info(clips[1]).resident);
  CHECK(store.state(clips[1]) == ClipState::Ready);  // still playable: it streams from its file
  CHECK(store.info(clips[1]).streamed);
  CHECK(store.info(clips[2]).resident);
  CHECK(store.view(clips[0]).samples == a_samples);  // the held clip did not move
  CHECK(store.resident_bytes() == 3u * k_clip_bytes);

  // Clip 2 is played and stopped: its release makes it the most recently used, so the next clip
  // evicts clip 3 instead, which nothing has touched since it was loaded.
  const VoiceHandle c = mixer.play(looping(clips[2]));
  REQUIRE_FALSE(c.is_null());
  mixer.render(out.data(), k_block);
  CHECK(mixer.stop(c));
  mixer.render(out.data(), k_block);  // the fade
  mixer.update();                     // the event: the voice has ended, its clip is released
  CHECK(store.info(clips[2]).refs == 0u);
  clips[4] = store.load_file(Id128{10, 4}, f.paths[4]);
  CHECK(store.evictions() == 2u);
  CHECK_FALSE(store.info(clips[3]).resident);
  CHECK(store.info(clips[2]).resident);
  CHECK(store.info(clips[4]).resident);

  // Everything resident in use: clip 0 still playing, and 2 and 4 now too. A sixth clip cannot be
  // made room for, so it is refused residency — and, loaded from a file, it streams instead.
  REQUIRE_FALSE(mixer.play(looping(clips[2])).is_null());
  REQUIRE_FALSE(mixer.play(looping(clips[4])).is_null());
  clips[5] = store.load_file(Id128{10, 5}, f.paths[5]);
  CHECK(store.over_budget() == 1u);
  CHECK(store.evictions() == 2u);
  CHECK(store.state(clips[5]) == ClipState::Ready);
  CHECK_FALSE(store.info(clips[5]).resident);
  CHECK(store.info(clips[5]).streamed);
  // A clip loaded from bytes has no file to fall back on: the same refusal is OverBudget.
  const Vector<u8> wav = wav_s16(to_s16(f.signals[5]), 1, k_sample_rate);
  const ClipHandle bytes_clip = store.load(Id128{11, 1}, {wav.data(), wav.size()});
  CHECK(store.state(bytes_clip) == ClipState::OverBudget);
  CHECK(store.over_budget() == 2u);
  CHECK(store.view(clips[0]).samples == a_samples);
  CHECK(store.info(clips[0]).resident);

  // The refused clip plays anyway, streamed; and since nothing could be evicted for it, playing it
  // does not ask for it to be decoded again.
  const VoiceHandle refused = mixer.play(looping(clips[5]));
  REQUIRE_FALSE(refused.is_null());
  CHECK(mixer.control_stats().stream_plays == 1u);
  CHECK(store.reloads() == 0u);
  mixer.render(out.data(), k_block);
  CHECK(mixer.stats().underrun_frames == 0u);
}

TEST_CASE("a voice started on an evicted clip streams it, and the store decodes it again") {
  Fixture f;
  ClipStore store(three_clips());
  MixerConfig config;
  config.voices = 4;
  Mixer mixer(store, config);
  Vector<f32> out(k_block * 2u, 0.0f);
  ClipHandle clips[4];
  for (u32 i = 0; i < 4; ++i)
    clips[i] = store.load_file(Id128{20, i}, f.paths[i]);
  REQUIRE(store.evictions() == 1u);
  REQUIRE_FALSE(store.info(clips[0]).resident);  // the oldest, and nothing held it

  // Played with no job system, the decode lands inside play(): room is made (clip 1, the least
  // recently used now) and this very voice plays it resident. With one, the voice would stream it
  // until the decode lands.
  const VoiceHandle voice = mixer.play(looping(clips[0]));
  REQUIRE_FALSE(voice.is_null());
  CHECK(store.reloads() == 1u);
  CHECK(store.info(clips[0]).resident);
  CHECK(store.evictions() == 2u);
  CHECK_FALSE(store.info(clips[1]).resident);
  mixer.render(out.data(), k_block);

  // Its bytes are the clip's: the same as a fresh store's decode of the same file.
  ClipStore fresh;
  const ClipHandle reference = fresh.load_file(Id128{20, 0}, f.paths[0]);
  const ClipView a = store.view(clips[0]);
  const ClipView b = fresh.view(reference);
  REQUIRE(a.frames == b.frames);
  for (u32 i = 0; i < a.frames; ++i)
    REQUIRE(a.samples[i] == b.samples[i]);
}

TEST_CASE("with a job system, an evicted clip streams while its decode is in flight") {
  Fixture f;
  jobs::JobSystemConfig jobs_config;
  jobs_config.performance_workers = 1;
  jobs_config.efficiency_workers = 2;
  jobs_config.pin_threads = false;
  jobs::JobSystem jobs(jobs_config);
  ClipStore store(three_clips(&jobs));
  MixerConfig config;
  config.voices = 4;
  Mixer mixer(store, config);
  ClipHandle clips[4];
  for (u32 i = 0; i < 4; ++i) {
    clips[i] = store.load_file(Id128{30, i}, f.paths[i]);
    // A decode lands at the controlling thread's next update(): until then the clip is Pending.
    store.wait();
    REQUIRE(store.state(clips[i]) == ClipState::Ready);
  }
  REQUIRE(store.evictions() == 1u);
  REQUIRE_FALSE(store.info(clips[0]).resident);

  const VoiceHandle voice = mixer.play(looping(clips[0]));
  REQUIRE_FALSE(voice.is_null());
  CHECK(mixer.control_stats().stream_plays == 1u);  // streamed: the decode is in flight
  CHECK(store.reloads() == 1u);
  CHECK_FALSE(store.info(clips[0]).resident);
  mixer.wait_streams();
  store.wait();  // lands the decode: resident again, and clip 1 evicted for it
  CHECK(store.info(clips[0]).resident);
  CHECK_FALSE(store.info(clips[1]).resident);
  // The next voice plays it resident; the first keeps its stream until it ends.
  REQUIRE_FALSE(mixer.play(looping(clips[0])).is_null());
  CHECK(mixer.control_stats().stream_plays == 1u);
  CHECK(store.info(clips[0]).refs == 2u);
}

TEST_CASE("a stolen voice's clip is held until the audio thread has applied the steal") {
  Fixture f;
  ClipStore store(three_clips());
  MixerConfig config;
  config.voices = 1;
  Mixer mixer(store, config);
  Vector<f32> out(k_block * 2u, 0.0f);
  ClipHandle clips[4];
  for (u32 i = 0; i < 3; ++i)
    clips[i] = store.load_file(Id128{40, i}, f.paths[i]);

  PlayParams low = looping(clips[0]);
  low.priority = 10;
  REQUIRE_FALSE(mixer.play(low).is_null());
  mixer.render(out.data(), k_block);
  PlayParams high = looping(clips[1]);
  high.priority = 200;
  REQUIRE_FALSE(mixer.play(high).is_null());  // steals the only voice
  CHECK(mixer.control_stats().steals == 1u);

  // Not rendered yet: the audio thread is still reading clip 0, so a load that needs room must
  // take clip 2 (unheld) and leave clip 0 alone, although it is the least recently used.
  CHECK(store.info(clips[0]).refs == 1u);
  clips[3] = store.load_file(Id128{40, 3}, f.paths[3]);
  CHECK(store.info(clips[0]).resident);
  CHECK_FALSE(store.info(clips[2]).resident);

  // Rendered: the steal is applied, and the next tick releases clip 0.
  mixer.render(out.data(), k_block);
  mixer.update();
  CHECK(store.info(clips[0]).refs == 0u);
  CHECK(store.info(clips[1]).refs == 1u);
}

TEST_CASE("a mixer gives its clips back when it goes") {
  Fixture f;
  ClipStore store(three_clips());
  const ClipHandle clip = store.load_file(Id128{50, 0}, f.paths[0]);
  {
    Mixer mixer(store);
    REQUIRE_FALSE(mixer.play(looping(clip)).is_null());
    REQUIRE_FALSE(mixer.play(looping(clip)).is_null());
    CHECK(store.info(clip).refs == 2u);
  }
  CHECK(store.info(clip).refs == 0u);
}

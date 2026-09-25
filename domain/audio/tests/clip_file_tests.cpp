// The built clip (clip_file.h, docs/subsystems/audio.md "Built clips"): the container round-trips
// and refuses what it should, its golden hashes are pinned across toolchains, and the clip store
// takes one — named directly or found in the derived-data cache — as a copy or as a stream, with
// the same bytes a decode of the source gives, and decodes the source when there is none.
#include "audio_test_support.h"

#include <core/hash/hash.h>
#include <domain/audio/audio.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::audio;
using namespace engine::audio::test;

namespace {

std::span<const u8> bytes_of(const Vector<u8>& v) { return {v.data(), v.size()}; }

bool write_bytes(const std::string& path, std::span<const u8> data) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

Vector<u8> read_bytes(const std::string& path) {
  std::string text;
  REQUIRE(io::read_file(path, text) == io::Status::Ok);
  Vector<u8> out;
  out.append(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
  return out;
}

// Generated without libm, so the file and every hash below are the same bytes on every toolchain:
// a stereo pair of the exact sine and noise, 16-bit, at the mix rate.
Vector<u8> stereo_wav(u32 frames) {
  const Vector<f32> sine = exact_sine(frames, 96, 0.45f);
  const Vector<f32> noise = lcg_noise(frames * 2u, 17, 0.25f);
  Vector<f32> interleaved;
  interleaved.resize_exact(frames * 2u);
  for (u32 i = 0; i < frames; ++i) {
    interleaved[2u * i] = sine[i] + noise[2u * i];
    interleaved[2u * i + 1u] = -sine[i] + noise[2u * i + 1u];
  }
  return wav_s16(to_s16(interleaved), 2, k_sample_rate);
}

// The same clip at 44.1 kHz, through the resampler.
Vector<u8> resampled_wav() { return wav_s16(to_s16(exact_sine(44100, 48, 0.5f)), 1, 44100); }

u64 samples_hash(const Vector<f32>& samples) {
  return hash_bytes(samples.data(), samples.size() * sizeof(f32));
}

// One voice over `blocks` blocks, `update()` before each, as the stream tests do.
Vector<f32> play_through(ClipStore& store, ClipHandle clip, u32 blocks, bool loop, f32 pitch) {
  MixerConfig config;
  config.streams = 2;
  config.stream_ring_frames = 3000;
  config.stream_fill_frames = 2000;
  Mixer mixer(store, config);
  PlayParams p;
  p.clip = clip;
  p.loop = loop;
  p.pitch = pitch;
  p.bus = k_bus_music;
  REQUIRE_FALSE(mixer.play(p).is_null());
  Vector<f32> all;
  Vector<f32> block(480u * 2u, 0.0f);
  for (u32 b = 0; b < blocks; ++b) {
    mixer.update();
    mixer.render(block.data(), 480);
    all.append(std::span<const f32>(block.data(), block.size()));
  }
  CHECK(mixer.stats().underrun_frames == 0u);
  return all;
}

}  // namespace

TEST_CASE("a built clip round-trips: header, sections, identity and every sample") {
  const engine::test::TempDir tmp("audio_clip_file");
  REQUIRE(tmp.ok());
  const Vector<u8> wav = stereo_wav(10000);
  ClipFileData built;
  REQUIRE(build_clip(bytes_of(wav), built) == DecodeStatus::Ok);
  CHECK(built.channels == 2u);
  CHECK(built.frames == 10000u);
  CHECK(built.source_channels == 2u);
  CHECK(built.source_rate == k_sample_rate);
  CHECK(built.source_hash == clip_source_hash(bytes_of(wav)));
  CHECK(built.build_key == clip_cache_key(built.source_hash));
  CHECK(built.build_key != built.source_hash);
  // What the store decodes, sample for sample.
  DecodedClip decoded;
  REQUIRE(decode_clip(bytes_of(wav), decoded) == DecodeStatus::Ok);
  CHECK(built.samples == decoded.samples);

  const std::string path = tmp.file("tone.clip");
  std::string error;
  REQUIRE(write_clip_file(path, built, &error));
  const Vector<u8> file = read_bytes(path);
  CHECK(is_clip_file(bytes_of(file)));
  CHECK_FALSE(is_clip_file(bytes_of(wav)));
  ClipFileHeader header;
  Vector<ClipFileSection> sections;
  REQUIRE(read_clip_file_table(bytes_of(file), header, sections, &error));
  CHECK(header.version == k_clip_file_version);
  CHECK(header.total_bytes == file.size());
  CHECK(header.content_hash == clip_file_hash(built));
  REQUIRE(sections.size() == 3u);
  CHECK(sections[0].kind == static_cast<u32>(ClipSection::Desc));
  CHECK(sections[1].kind == static_cast<u32>(ClipSection::Samples));
  CHECK(sections[2].kind == static_cast<u32>(ClipSection::SourceHash));
  for (const ClipFileSection& s : sections)
    CHECK(s.offset % k_clip_file_alignment == 0u);
  CHECK(sections[1].element_count == 20000u);

  ClipFileData back;
  REQUIRE(read_clip_file(path, back, &error));
  CHECK(back.channels == built.channels);
  CHECK(back.frames == built.frames);
  CHECK(back.samples == built.samples);
  CHECK(back.source_hash == built.source_hash);
  CHECK(back.build_key == built.build_key);

  u64 source_hash = 0;
  u64 build_key = 0;
  REQUIRE(read_clip_file_identity(path, source_hash, build_key, &error));
  CHECK(source_hash == built.source_hash);
  CHECK(build_key == built.build_key);

  // The front, by range, as a stream opens it: the frames start where the table says.
  io::FileHandle handle;
  REQUIRE(handle.open(path) == io::Status::Ok);
  ClipFileLayout layout;
  REQUIRE(read_clip_file_layout(handle, layout, &error));
  CHECK(layout.channels == 2u);
  CHECK(layout.frames == 10000u);
  CHECK(layout.samples_offset == sections[1].offset);
  CHECK(layout.source_hash == built.source_hash);
  CHECK(layout.build_key == built.build_key);
  f32 first[2] = {0.0f, 0.0f};
  u64 got = 0;
  REQUIRE(handle.read_at(layout.samples_offset, first, sizeof(first), got) == io::Status::Ok);
  CHECK(first[0] == built.samples[0]);
  CHECK(first[1] == built.samples[1]);

  CHECK(clip_cache_path("root", built.build_key).find("clips") != std::string::npos);
}

TEST_CASE("a built clip that is not whole, not ours or not at the mix rate is refused") {
  const Vector<u8> wav = stereo_wav(2000);
  ClipFileData built;
  REQUIRE(build_clip(bytes_of(wav), built) == DecodeStatus::Ok);
  const engine::test::TempDir tmp("audio_clip_refusals");
  REQUIRE(tmp.ok());
  const std::string path = tmp.file("good.clip");
  REQUIRE(write_clip_file(path, built));
  const Vector<u8> good = read_bytes(path);

  auto refused = [](const Vector<u8>& bytes, const char* expect) {
    ClipFileData out;
    std::string error;
    CHECK_FALSE(read_clip_file_memory(bytes_of(bytes), out, &error));
    CHECK(out.samples.empty());
    CAPTURE(error);
    CHECK(error.find(expect) != std::string::npos);
  };
  Vector<u8> truncated = good;
  truncated.resize(truncated.size() - 7u);
  refused(truncated, "truncated");
  Vector<u8> magic = good;
  magic[0] = 'X';
  refused(magic, "magic");
  Vector<u8> version = good;
  version[4] = 99;
  refused(version, "version");
  Vector<u8> flipped = good;
  flipped[flipped.size() - 20u] ^= 0x40u;  // a sample's bit: only the content hash can tell
  refused(flipped, "content hash");

  // A description that claims another rate, written through the table so the hash is right.
  ClipFileHeader header;
  Vector<ClipFileSection> sections;
  REQUIRE(read_clip_file_table(bytes_of(good), header, sections));
  Vector<u8> rate = good;
  const u32 rate_441 = 44100;
  std::memcpy(rate.data() + sections[0].offset + offsetof(ClipFileDesc, sample_rate), &rate_441,
              sizeof(rate_441));
  const u64 rehash =
      hash_bytes(rate.data() + sizeof(ClipFileHeader), rate.size() - sizeof(ClipFileHeader));
  std::memcpy(rate.data() + offsetof(ClipFileHeader, content_hash), &rehash, sizeof(rehash));
  refused(rate, "44100 Hz");

  // The writer refuses what the reader would.
  ClipFileData bad = built;
  bad.samples.resize(bad.samples.size() - 1u);
  std::string error;
  CHECK_FALSE(write_clip_file(tmp.file("bad.clip"), bad, &error));
  CHECK_FALSE(validate_clip(bad));
}

TEST_CASE("a section kind a reader does not know is skipped") {
  // A newer build's file: the three sections this one knows and a fourth of kind 99, laid out by
  // hand exactly as the writer lays out its own.
  const Vector<u8> wav = stereo_wav(100);
  ClipFileData built;
  REQUIRE(build_clip(bytes_of(wav), built) == DecodeStatus::Ok);
  ClipFileDesc desc;
  desc.sample_rate = k_sample_rate;
  desc.channels = built.channels;
  desc.frames = built.frames;
  desc.source_channels = built.source_channels;
  desc.source_rate = built.source_rate;
  const u64 identity[2] = {built.source_hash, built.build_key};
  const u32 extra[3] = {7, 8, 9};
  struct Part {
    u32 kind;
    u32 element_size;
    u64 count;
    const void* data;
  };
  const Part parts[] = {
      {1, sizeof(ClipFileDesc), 1, &desc},
      {99, sizeof(u32), 3, extra},
      {2, sizeof(f32), built.samples.size(), built.samples.data()},
      {3, sizeof(u64), 2, identity},
  };
  u64 offset = sizeof(ClipFileHeader) + sizeof(ClipFileSection) * 4u;
  Vector<ClipFileSection> table;
  for (const Part& part : parts) {
    offset = (offset + 15u) / 16u * 16u;
    table.push_back(ClipFileSection{part.kind, part.element_size, part.count, offset});
    offset += u64{part.element_size} * part.count;
  }
  Vector<u8> bytes(static_cast<u32>(offset), u8{0});
  ClipFileHeader header;
  header.section_count = 4;
  header.total_bytes = offset;
  for (u32 i = 0; i < 4; ++i) {
    std::memcpy(bytes.data() + sizeof(ClipFileHeader) + sizeof(ClipFileSection) * i, &table[i],
                sizeof(ClipFileSection));
    std::memcpy(bytes.data() + table[i].offset, parts[i].data,
                static_cast<usize>(parts[i].element_size * parts[i].count));
  }
  header.content_hash =
      hash_bytes(bytes.data() + sizeof(ClipFileHeader), bytes.size() - sizeof(ClipFileHeader));
  std::memcpy(bytes.data(), &header, sizeof(header));
  ClipFileData out;
  std::string error;
  REQUIRE(read_clip_file_memory(bytes_of(bytes), out, &error));
  CHECK(out.samples == built.samples);
  CHECK(out.build_key == built.build_key);
}

// The toolchain-independence rule `.tex` and `.clusters` keep (clip_file.h): these were taken on
// MSVC (msvc-debug) and GCC and Clang must reproduce them. A change to the decode, the container or
// the key moves them, bumps `k_clip_cache_version`, and says why in its commit.
TEST_CASE("a built clip is the same bytes from every toolchain") {
  struct Golden {
    const char* name;
    Vector<u8> source;
    u64 file_hash;
    u64 samples_hash;
  };
  Golden goldens[] = {
      {"16-bit stereo at 48 kHz", stereo_wav(12000), 0xbdf041eca33d2c2full, 0xb92b922062cf46efull},
      {"16-bit mono at 44.1 kHz, resampled", resampled_wav(), 0x0c4f1a81606508d7ull,
       0x4fd32be85084006dull},
  };
  for (const Golden& g : goldens) {
    const std::string name = g.name;
    CAPTURE(name);
    ClipFileData built;
    REQUIRE(build_clip(bytes_of(g.source), built) == DecodeStatus::Ok);
    const u64 file_hash = clip_file_hash(built);
    const u64 samples = samples_hash(built.samples);
    MESSAGE(name << ": file " << hex64(file_hash) << ", samples " << hex64(samples) << ", key "
                 << hex64(built.build_key));
    CHECK(file_hash == g.file_hash);
    CHECK(samples == g.samples_hash);
  }
}

TEST_CASE("the store takes a built clip named directly: a copy, or a stream by range") {
  const engine::test::TempDir tmp("audio_clip_store_built");
  REQUIRE(tmp.ok());
  const Vector<u8> wav = stereo_wav(20000);
  ClipFileData built;
  REQUIRE(build_clip(bytes_of(wav), built) == DecodeStatus::Ok);
  const std::string path = tmp.file("music.clip");
  REQUIRE(write_clip_file(path, built));

  ClipStore decoded;
  const ClipHandle reference = decoded.load(Id128{1, 1}, bytes_of(wav));
  REQUIRE(decoded.state(reference) == ClipState::Ready);

  ClipStore copied;
  const ClipHandle copy = copied.load_file(Id128{1, 1}, path);
  REQUIRE(copied.state(copy) == ClipState::Ready);
  CHECK(copied.info(copy).resident);
  CHECK(copied.built_loads() == 1u);
  CHECK(copied.info(copy).source_rate == k_sample_rate);
  const ClipView a = copied.view(copy);
  const ClipView b = decoded.view(reference);
  REQUIRE(a.frames == b.frames);
  CHECK(std::memcmp(a.samples, b.samples, u64{a.frames} * 2u * sizeof(f32)) == 0);

  // Over the threshold: streamed from the file's frames by range, with no decoder at all.
  ClipStoreConfig config;
  config.stream_threshold_bytes = 1024;
  ClipStore streamed(config);
  const ClipHandle stream = streamed.load_file(Id128{1, 1}, path);
  REQUIRE(streamed.state(stream) == ClipState::Ready);
  CHECK(streamed.info(stream).streamed);
  CHECK(streamed.source(stream).kind == ClipSourceKind::ClipFile);
  CHECK(streamed.built_loads() == 1u);
  CHECK(play_through(streamed, stream, 60, true, 1.23f) ==
        play_through(decoded, reference, 60, true, 1.23f));

  // Handed over as bytes, a built clip is a copy too.
  ClipStore from_bytes;
  const Vector<u8> file = read_bytes(path);
  const ClipHandle held = from_bytes.load(Id128{1, 1}, bytes_of(file));
  REQUIRE(from_bytes.state(held) == ClipState::Ready);
  CHECK(from_bytes.built_loads() == 1u);
  CHECK(std::memcmp(from_bytes.view(held).samples, b.samples, u64{b.frames} * 2u * 4u) == 0);
}

TEST_CASE("with a derived-data root the store finds a source's built clip, and decodes on a miss") {
  const engine::test::TempDir tmp("audio_clip_store_ddc");
  REQUIRE(tmp.ok());
  const std::string ddc = tmp.file("ddc");
  REQUIRE(io::make_directories(io::join_path(ddc, "clips")) == io::Status::Ok);
  const Vector<u8> wav = stereo_wav(20000);
  const std::string source = tmp.file("music.wav");
  REQUIRE(write_bytes(source, bytes_of(wav)));
  ClipFileData built;
  REQUIRE(build_clip(bytes_of(wav), built) == DecodeStatus::Ok);

  ClipStore reference_store;
  const ClipHandle reference = reference_store.load(Id128{2, 1}, bytes_of(wav));

  ClipStoreConfig config;
  config.ddc_root = ddc;
  {
    // A miss: no entry, so the source is decoded, as it would be with no root.
    ClipStore store(config);
    const ClipHandle clip = store.load_file(Id128{2, 1}, source);
    REQUIRE(store.state(clip) == ClipState::Ready);
    CHECK(store.built_loads() == 0u);
    CHECK(store.info(clip).resident);
  }
  const std::string entry = clip_cache_path(ddc, built.build_key);
  REQUIRE(write_clip_file(entry, built));
  {
    // A hit, from a file and from bytes: a copy, with no decode.
    ClipStore store(config);
    const ClipHandle from_file = store.load_file(Id128{2, 1}, source);
    const ClipHandle from_bytes = store.load(Id128{2, 2}, bytes_of(wav));
    REQUIRE(store.state(from_file) == ClipState::Ready);
    REQUIRE(store.state(from_bytes) == ClipState::Ready);
    CHECK(store.built_loads() == 2u);
    CHECK(std::memcmp(store.view(from_file).samples, reference_store.view(reference).samples,
                      u64{built.frames} * 2u * 4u) == 0);
    CHECK(store.info(from_file).evictable);  // a file's clip, wherever its frames came from
  }
  {
    // Over the threshold: the stream reads the cache entry's frames, not the WAV.
    ClipStoreConfig streaming = config;
    streaming.stream_threshold_bytes = 1024;
    ClipStore store(streaming);
    const ClipHandle clip = store.load_file(Id128{2, 1}, source);
    REQUIRE(store.state(clip) == ClipState::Ready);
    CHECK(store.source(clip).kind == ClipSourceKind::ClipFile);
    CHECK(store.source(clip).path == entry);
    CHECK(play_through(store, clip, 50, false, 0.87f) ==
          play_through(reference_store, reference, 50, false, 0.87f));
  }
  {
    // An entry that records another build is not the answer: decoded instead.
    ClipFileData stale = built;
    stale.build_key ^= 1u;
    REQUIRE(write_clip_file(entry, stale));
    ClipStore store(config);
    const ClipHandle clip = store.load_file(Id128{2, 1}, source);
    REQUIRE(store.state(clip) == ClipState::Ready);
    CHECK(store.built_loads() == 0u);
  }
}

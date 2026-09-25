// End to end: the audio capability's derived step (docs/subsystems/audio.md, "Built clips";
// docs/subsystems/apps.md, "engine-content"). A WAV written at test time builds into a scratch
// derived-data root as `<ddc>/clips/<key>.clip` — the key the clip store computes from the same
// bytes — and into a named output as the same bytes; `info` reads it; `build-all` builds a
// manifest's "clips" (with no "meshes") once and skips them on a second run by identity; and a
// source the decoders refuse fails with the step's rule. Compiled only where the configuration
// has the audio capability. Nothing binary lives in the tree.
#include <core/json/json.h>
#include <core/platform/process.h>
#include <domain/audio/clip_file.h>
#include <domain/audio/format.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct Run {
  i32 exit_code = -1;
  std::string output;
  std::vector<JsonValue> lines;  // stdout, one JSON object per line
};

Run content(std::vector<std::string> args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Run run;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn engine-content: " << error);
    return run;
  }
  p.close_stdin();
  p.read_all(run.output);
  run.exit_code = p.wait();
  usize at = 0;
  while (at < run.output.size()) {
    usize end = run.output.find('\n', at);
    if (end == std::string::npos) end = run.output.size();
    std::string_view line(run.output.data() + at, end - at);
    while (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    JsonValue value;
    if (!line.empty() && parse_json(line, value).ok && value.is_object())
      run.lines.push_back(std::move(value));
    at = end + 1;
  }
  return run;
}

u64 number(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  u64 out = 0;
  return value != nullptr && value->get_u64(out) ? out : ~u64{0};
}

std::string text_of(const JsonValue& object, const char* key) {
  const JsonValue* value = object.find(key);
  return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string();
}

std::string file_bytes(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A 16-bit stereo WAV of `frames` frames of LCG noise: generated, so nothing binary is committed,
// and without libm, so its bytes — and the clip built from it — are the same everywhere.
std::string wav(u32 frames, u64 seed) {
  std::string out;
  auto u16le = [&out](u32 v) {
    out.push_back(static_cast<char>(v & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
  };
  auto u32le = [&](u32 v) {
    u16le(v & 0xFFFFu);
    u16le(v >> 16);
  };
  const u32 data_bytes = frames * 4u;
  out += "RIFF";
  u32le(36u + data_bytes);
  out += "WAVEfmt ";
  u32le(16);
  u16le(1);
  u16le(2);
  u32le(audio::k_sample_rate);
  u32le(audio::k_sample_rate * 4u);
  u16le(4);
  u16le(16);
  out += "data";
  u32le(data_bytes);
  u64 state = seed;
  for (u32 i = 0; i < frames * 2u; ++i) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    u16le(static_cast<u32>(state >> 48));
  }
  return out;
}

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
  return static_cast<bool>(out);
}

}  // namespace

TEST_CASE("engine-content builds a clip into the cache and a named output, and reads it back") {
  const test::TempDir tmp("engine_content_clip");
  REQUIRE(tmp.ok());
  const std::string source = tmp.file("tone.wav");
  const std::string bytes = wav(9600, 5);
  REQUIRE(write_text(source, bytes));
  const std::string ddc = tmp.file("ddc");

  const Run cached = content({"build", source, "--cache", "--ddc", ddc});
  REQUIRE(cached.exit_code == 0);
  REQUIRE(cached.lines.size() == 1u);
  const JsonValue& line = cached.lines[0];
  CHECK(text_of(line, "step") == "audio");
  CHECK(number(line, "channels") == 2u);
  CHECK(number(line, "frames") == 9600u);
  CHECK(number(line, "source_rate") == audio::k_sample_rate);
  // Addressed as the clip store addresses it: by the key over the source's bytes.
  const std::span<const u8> source_bytes(reinterpret_cast<const u8*>(bytes.data()), bytes.size());
  const u64 source_hash = audio::clip_source_hash(source_bytes);
  CHECK(number(line, "source_hash") == source_hash);
  CHECK(number(line, "build_key") == audio::clip_cache_key(source_hash));
  const std::string entry = audio::clip_cache_path(ddc, audio::clip_cache_key(source_hash));
  CHECK(std::filesystem::path(text_of(line, "path")) == std::filesystem::path(entry));
  REQUIRE(std::filesystem::exists(entry));
  // The tool writes what the library builds from the same bytes.
  audio::ClipFileData built;
  REQUIRE(audio::build_clip(source_bytes, built) == audio::DecodeStatus::Ok);
  CHECK(number(line, "hash") == audio::clip_file_hash(built));
  audio::ClipFileData back;
  REQUIRE(audio::read_clip_file(entry, back));
  CHECK(back.samples == built.samples);

  const std::string named = tmp.file("tone.clip");
  const Run to_file = content({"build", source, named});
  REQUIRE(to_file.exit_code == 0);
  CHECK(file_bytes(named) == file_bytes(entry));  // one clip, whatever it is called

  const Run info = content({"info", named});
  REQUIRE(info.exit_code == 0);
  REQUIRE(info.lines.size() == 1u);
  CHECK(text_of(info.lines[0], "kind") == "clip");
  CHECK(number(info.lines[0], "frames") == 9600u);
  CHECK(number(info.lines[0], "channels") == 2u);
  CHECK(number(info.lines[0], "sample_rate") == audio::k_sample_rate);
  CHECK(number(info.lines[0], "build_key") == audio::clip_cache_key(source_hash));
  const JsonValue* sections = info.lines[0].find("sections");
  REQUIRE(sections != nullptr);
  CHECK(sections->size() == 3u);

  // What the decoders refuse fails the build with the step's rule, and writes nothing.
  const std::string junk = tmp.file("junk.wav");
  REQUIRE(write_text(junk, std::string(512, 'x')));
  const Run refused = content({"build", junk, tmp.file("junk.clip")});
  CHECK(refused.exit_code == 1);
  CHECK(refused.output.find("{") == std::string::npos);
  CHECK_FALSE(std::filesystem::exists(tmp.file("junk.clip")));
}

TEST_CASE("engine-content build-all builds a manifest's clips once and then skips them") {
  const test::TempDir tmp("engine_content_clips_manifest");
  REQUIRE(tmp.ok());
  REQUIRE(write_text(tmp.file("a.wav"), wav(4800, 1)));
  REQUIRE(write_text(tmp.file("b.wav"), wav(7200, 2)));
  const std::string manifest = tmp.file("clips.json");
  REQUIRE(write_text(manifest,
                     "{\"clips\":[{\"source\":\"a.wav\"},"
                     "{\"source\":\"b.wav\",\"output\":\"b.clip\"}]}"));
  const std::string ddc = tmp.file("ddc");

  const Run first = content({"build-all", manifest, "--cache", "--ddc", ddc});
  REQUIRE(first.exit_code == 0);
  REQUIRE(first.lines.size() == 3u);  // a line a clip, and the summary
  CHECK(text_of(first.lines[0], "status") == "built");
  CHECK(text_of(first.lines[0], "step") == "audio");
  CHECK(number(first.lines[0], "frames") == 4800u);
  CHECK(text_of(first.lines[1], "status") == "built");
  CHECK(std::filesystem::path(text_of(first.lines[1], "path")) ==
        std::filesystem::path(tmp.file("b.clip")));
  const JsonValue* steps = first.lines[2].find("steps");
  REQUIRE(steps != nullptr);
  const JsonValue* audio_counts = steps->find("audio");
  REQUIRE(audio_counts != nullptr);
  CHECK(number(*audio_counts, "built") == 2u);
  CHECK(number(first.lines[2], "built") == 0u);  // no meshes

  const Run second = content({"build-all", manifest, "--cache", "--ddc", ddc});
  REQUIRE(second.exit_code == 0);
  REQUIRE(second.lines.size() == 3u);
  CHECK(text_of(second.lines[0], "status") == "skipped");
  CHECK(text_of(second.lines[1], "status") == "skipped");
  CHECK(number(*second.lines[2].find("steps")->find("audio"), "skipped") == 2u);

  // A changed source is a new key and a new build; the other is still the answer.
  REQUIRE(write_text(tmp.file("a.wav"), wav(4800, 3)));
  const Run third = content({"build-all", manifest, "--cache", "--ddc", ddc});
  REQUIRE(third.exit_code == 0);
  CHECK(text_of(third.lines[0], "status") == "built");
  CHECK(text_of(third.lines[1], "status") == "skipped");

  // With no --cache, an entry that names no output is refused before anything is built.
  const Run no_cache = content({"build-all", manifest});
  CHECK(no_cache.exit_code == 1);
}

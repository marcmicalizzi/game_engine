// engine-content's audio step (docs/subsystems/audio.md, "Built clips"; docs/subsystems/apps.md,
// "engine-content"): a clip's source — WAV, FLAC or MP3, whatever the clip store decodes — decoded
// once into the mix format and written as a `.clip`, which the store then loads as a copy or a
// stream. The step is the audio capability's; content_build runs it (its derived-step table, which
// a module that must build without any capability can hold), and this file is where engine-content
// hands it over when its configuration has the capability. `build` runs it on an audio source,
// `build-all` on a manifest's "clips", and `info` reads a `.clip`.
//
// Without the capability (`msvc-minimal`, `ENGINE_WITH_AUDIO=OFF`) there is no step: `build` of an
// audio file is refused as a mesh it cannot import, a manifest's "clips" are not read, and `info`
// says what it would say of any file it does not know.
#include "content_commands.h"

#if defined(ENGINE_CONTENT_AUDIO)
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <domain/audio/clip_file.h>
#include <domain/audio/format.h>
#include <foundation/io/vfs.h>

#include <cctype>
#include <cstdio>
#include <string>
#endif

namespace engine::content {

#if defined(ENGINE_CONTENT_AUDIO)

namespace {

// By extension, as the store's decoders are chosen by content but a command line has to choose
// between a mesh and a clip before it reads anything.
bool accepts(std::string_view path) {
  std::string ext(io::extension(path));
  for (char& c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".wav" || ext == ".flac" || ext == ".mp3";
}

bool identity(const std::string& output, u64& source_hash, u64& build_key) {
  return audio::read_clip_file_identity(output, source_hash, build_key, nullptr);
}

bool build(std::span<const u8> source, u64 source_hash, u64 build_key, const std::string& output,
           JsonValue& report, std::string& error) {
  audio::ClipFileData data;
  const audio::DecodeStatus status = audio::build_clip(source, data);
  if (status != audio::DecodeStatus::Ok) {
    error = std::string("the decoders refused it: ") + audio::decode_status_name(status);
    return false;
  }
  // `build_clip` takes the identity over the same bytes by the same rule; the build's words are
  // the ones the skip compares, so they are what the file records.
  data.source_hash = source_hash;
  data.build_key = build_key;
  if (!audio::write_clip_file(output, data, &error)) return false;
  report.set("channels", JsonValue(data.channels));
  report.set("frames", JsonValue(data.frames));
  report.set("seconds",
             JsonValue(static_cast<f64>(data.frames) / static_cast<f64>(audio::k_sample_rate)));
  report.set("source_channels", JsonValue(data.source_channels));
  report.set("source_rate", JsonValue(data.source_rate));
  report.set("hash", JsonValue(audio::clip_file_hash(data)));
  return true;
}

u64 build_key(u64 source_hash) { return audio::clip_cache_key(source_hash); }

std::string cache_path(std::string_view ddc, u64 key) { return audio::clip_cache_path(ddc, key); }

const content_build::DerivedStep k_audio_step{"audio",     "clips",   &accepts, &build_key,
                                              &cache_path, &identity, &build};

}  // namespace

std::span<const content_build::DerivedStep* const> derived_steps() {
  static const content_build::DerivedStep* const k_steps[] = {&k_audio_step};
  return k_steps;
}

bool clip_info(const std::string& path, int& code) {
  std::string file;
  if (io::read_file(path, file) != io::Status::Ok) return false;
  const std::span<const u8> bytes(reinterpret_cast<const u8*>(file.data()), file.size());
  if (!audio::is_clip_file(bytes)) return false;
  audio::ClipFileHeader header;
  Vector<audio::ClipFileSection> records;
  std::string error;
  audio::ClipFileData data;
  if (!audio::read_clip_file_table(bytes, header, records, &error) ||
      !audio::read_clip_file_memory(bytes, data, &error)) {
    std::fprintf(stderr, "engine-content: %s\n", error.c_str());
    code = 1;
    return true;
  }
  // The table as the bytes have it, a newer build's sections included, each with its payload's
  // own hash, as `info` reports a container's.
  JsonValue sections = JsonValue::array();
  for (const audio::ClipFileSection& section : records) {
    JsonValue entry = JsonValue::object();
    entry.set("kind", JsonValue(section.kind));
    entry.set("name", JsonValue(audio::clip_section_name(section.kind)));
    entry.set("element_size", JsonValue(section.element_size));
    entry.set("element_count", JsonValue(section.element_count));
    entry.set("offset", JsonValue(section.offset));
    const u64 payload = u64{section.element_size} * section.element_count;
    if (section.offset <= file.size() && payload <= file.size() - section.offset)
      entry.set("hash", JsonValue(hash_bytes(file.data() + section.offset, payload)));
    sections.push_back(std::move(entry));
  }
  JsonValue summary = JsonValue::object();
  summary.set("path", JsonValue(path));
  summary.set("kind", JsonValue("clip"));
  summary.set("version", JsonValue(header.version));
  summary.set("flags", JsonValue(header.flags));
  summary.set("total_bytes", JsonValue(header.total_bytes));
  summary.set("hash", JsonValue(header.content_hash));
  summary.set("sections", std::move(sections));
  summary.set("channels", JsonValue(data.channels));
  summary.set("frames", JsonValue(data.frames));
  summary.set("sample_rate", JsonValue(audio::k_sample_rate));
  summary.set("seconds",
              JsonValue(static_cast<f64>(data.frames) / static_cast<f64>(audio::k_sample_rate)));
  summary.set("source_channels", JsonValue(data.source_channels));
  summary.set("source_rate", JsonValue(data.source_rate));
  summary.set("source_hash", JsonValue(data.source_hash));
  summary.set("build_key", JsonValue(data.build_key));
  std::string line = write_json(summary, JsonWriteOptions{.pretty = false});
  line.push_back('\n');
  std::fwrite(line.data(), 1, line.size(), stdout);
  code = 0;
  return true;
}

#else

std::span<const content_build::DerivedStep* const> derived_steps() { return {}; }

bool clip_info(const std::string&, int&) { return false; }

#endif

}  // namespace engine::content

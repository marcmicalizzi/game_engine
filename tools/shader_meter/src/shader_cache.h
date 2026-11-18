#pragma once

// The shader compile cache (roadmap T76; docs/ci/what-to-run.md, "The shader cache"): slangc's
// SPIR-V for one engine_shaders() compile, kept in a machine-wide store and served to any checkout
// whose compile would make the same bytes, the way ccache serves objects.
//
// A lookup is two levels, as ccache's direct mode is:
//
//   - the **manifest key** is a SHA-256 over the cache's format version, the compiler's identity
//     (cmake/EngineShaders.cmake hashes every file of the Slang package), the source's file name
//     and bytes, and every argument of the compile with the source, the output and each -I
//     directory written relative to the source's directory, so the same compile in two checkouts
//     has one key;
//   - the manifest it names lists the compiles stored under that key, each with every file slangc
//     read (its -depfile, so no hand-kept list can be short) and that file's SHA-256, and every
//     place an `#include "..."` of those files could have been found and was not (the shadows: a
//     file created there later would be read instead). An entry is served only when every file it
//     lists still has its hash and every shadow is still absent.
//
// The result an entry names holds the SPIR-V, the compile's measured cost and whatever slangc
// printed, so a hit is the compile's bytes, its cost line and its warnings. Writes go to a
// temporary file in the store and are renamed into place, results are written once and never
// replaced, and a manifest that two builds update at once loses at most one of their entries,
// which is a later miss and never a wrong hit.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace shader_meter {

struct CacheRequest {
  std::filesystem::path store;    // the machine's store (ENGINE_SHADER_CACHE_DIR)
  std::string compiler;           // the compiler's identity
  std::filesystem::path source;   // the shader's .slang, absolute
  std::filesystem::path output;   // the SPIR-V the build wants
  std::filesystem::path cost;     // the meter's cost file
  std::vector<std::string> args;  // the compile's arguments after the program, UTF-8
  std::uint64_t max_bytes = 0;    // the store's limit
};

// The manifest key, or an empty string when the source cannot be read (the compile then runs
// uncached and slangc reports the problem).
std::string cache_manifest_key(const CacheRequest& request);

// Serves a stored compile: true when an entry of the manifest matched, with the SPIR-V copied to
// the output, the cost file written (its stored lines and `cache hit`) and slangc's stored output
// in `log`. False on any miss or any failure to restore, in which case the caller compiles.
bool cache_lookup(const CacheRequest& request, const std::string& manifest_key, std::string& log);

// Stores a compile that succeeded: the SPIR-V at the output, the cost file the meter just wrote,
// the depfile slangc wrote and its printed output. `started` is the file clock just before slangc
// started: a file it read that was written at or after that may have changed while it was read,
// and the compile is then not stored (ccache's "include file too new"); the hashes are taken after
// the compile, so a file edited between the read and the hash is caught the same way. Returns the
// cost file's last line, `cache stored` or `cache not stored: <why>`.
std::string cache_store(const CacheRequest& request, const std::string& manifest_key,
                        const std::filesystem::path& depfile, const std::string& log,
                        std::filesystem::file_time_type started);

// The files a make-style depfile names after its target, unescaped (exposed for the self-test's
// sake through `shader-meter --parse-depfile`).
std::vector<std::string> parse_depfile(const std::string& text);

// A unique temporary path in the store for one write.
std::filesystem::path cache_temp_path(const std::filesystem::path& store, const char* suffix);

bool read_whole_file(const std::filesystem::path& path, std::string& out);

}  // namespace shader_meter

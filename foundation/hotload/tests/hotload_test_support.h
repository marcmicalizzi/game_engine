#pragma once

// What the hotload tests share: placing a built module in the test's scratch directory, and
// compiling one from sources there with the command engine_game_module() exports.

#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace engine::hotload::test {

// A packaged test bundle carries no game modules and no compiler: the tests that need them say
// so and pass rather than fail on a machine that cannot run them.
inline bool in_bundle() {
  if (engine::test::bundle_root().empty()) return false;
  MESSAGE("hotload: game modules and the compiler are not part of a test bundle; skipped");
  return true;
}

inline void write_text(const std::string& path, std::string_view text) {
  std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// Copies a built module to `<dir>/<name><extension>` and moves its modification time on by
// `seconds`, so a poll sees the change even when two copies land inside one tick of the file
// system's clock. Returns the path.
inline std::string place(const engine::test::TempDir& dir, const char* built, std::string_view name,
                         int seconds = 0) {
  const std::filesystem::path from(built);
  const std::string to = dir.file(std::string(name) + from.extension().string());
  std::error_code ec;
  std::filesystem::copy_file(from, std::filesystem::path(to),
                             std::filesystem::copy_options::overwrite_existing, ec);
  REQUIRE_MESSAGE(!ec, "copying " << built << ": " << ec.message());
  std::filesystem::last_write_time(
      std::filesystem::path(to),
      std::filesystem::file_time_type::clock::now() + std::chrono::seconds(seconds), ec);
  return to;
}

// Builds every .cpp and .schema in `source_dir` into `output` with the build's own compiler and
// flags (cmake/EngineGameModuleBuild.cmake). The compiler's output, success or not, is `log`.
inline bool build_module(const std::string& source_dir, const std::string& output,
                         std::string& log) {
  const std::string config = std::string("-DCONFIG=") + ENGINE_GAME_MODULE_BUILD_CONFIG;
  const std::string sources = "-DSOURCE_DIR=" + source_dir;
  const std::string out = "-DOUTPUT=" + output;
  const std::string_view argv[] = {ENGINE_CMAKE_COMMAND,           config, sources, out, "-P",
                                   ENGINE_GAME_MODULE_BUILD_SCRIPT};
  platform::Process process;
  std::string error;
  if (!process.spawn(argv, &error, true)) {
    log = "cannot start cmake: " + error;
    return false;
  }
  process.close_stdin();
  process.read_all(log);
  return process.wait() == 0;
}

}  // namespace engine::hotload::test

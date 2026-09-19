#pragma once

// Where a test finds the things that are *not* its own scratch space: the executables the build
// produced, and the data files under `content/`.
//
// A test knows those as compile-time absolute paths — `ENGINE_APP_PATH`, `ENGINE_HOST_PATH`,
// `ENGINE_SCHEMAC_PATH`, `ENGINE_SOURCE_DIR`, `ENGINE_SHADER_MANIFEST` — baked in by CMake. That
// is exactly right under
// CTest and wrong everywhere else. `tools/package-tests.ps1` builds a bundle that runs on a
// machine with no toolchain and no checkout, where those paths name directories that do not
// exist; and, worse, **on the build machine they name directories that do**, so a bundle that
// merely *fell back* to a bundled copy would pass by accident here and prove nothing about the
// machine it was made for.
//
// So the rule is take-over, not fallback. `ENGINE_BUNDLE_ROOT`, set by the bundle's
// `run-tests.ps1`, replaces the build tree completely: every path below is resolved inside it and
// nothing consults the compiled-in one. Unset — a plain CTest run — every path is the build's own
// and nothing changes.
//
//     // <bundle>/bin/engine-host.exe, or the built path outside a bundle
//     const std::string host = test::app_path(ENGINE_HOST_PATH);
//
//     const std::string logs =
//         test::data_path(ENGINE_SOURCE_DIR "/content/input-logs", "content/input-logs");
//     if (!test::path_exists(logs)) {
//       MESSAGE("not in this bundle: " << logs);
//       return;
//     }
//
// Header-only and standard-library-only, like `test_temp_dir.h` beside it: `engine_test_main`
// puts `tests/support` on every test and bench target's include path, so `#include
// <test_paths.h>` needs no new dependency.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace engine::test {

namespace detail {

// getenv, spelled the way MSVC will compile without a deprecation warning.
inline std::string environment(const char* name) {
#if defined(_MSC_VER)
  char* value = nullptr;
  size_t size = 0;
  if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return {};
  std::string out(value);
  std::free(value);
  return out;
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : std::string{};
#endif
}

inline std::string forward_slashes(std::string path) {
  for (char& c : path) {
    if (c == '\\') c = '/';
  }
  return path;
}

}  // namespace detail

// The unpacked test bundle this run belongs to, or empty under a plain CTest run.
inline const std::string& bundle_root() {
  static const std::string root =
      detail::forward_slashes(detail::environment("ENGINE_BUNDLE_ROOT"));
  return root;
}

inline bool path_exists(const std::string& path) {
  if (path.empty()) return false;
  std::error_code ignored;
  return std::filesystem::exists(std::filesystem::path(path), ignored);
}

// An executable the build produced, by the path CMake baked in. In a bundle every executable
// lives in `bin/` under one name, so only the file name survives the move.
inline std::string app_path(std::string_view built_path) {
  if (bundle_root().empty()) return std::string(built_path);
  const std::filesystem::path built(std::string{built_path});
  return bundle_root() + "/bin/" + built.filename().string();
}

// A data file or directory, by the path CMake baked in and by where the bundle puts it (relative
// to the bundle root, e.g. "content/input-logs"). The caller checks `path_exists`: a bundle is
// allowed not to carry something, and a test that finds it missing says so and skips rather than
// failing for a missing source tree.
inline std::string data_path(std::string_view built_path, std::string_view bundle_relative) {
  if (bundle_root().empty()) return std::string(built_path);
  std::string out = bundle_root();
  out.push_back('/');
  out.append(bundle_relative);
  return out;
}

}  // namespace engine::test

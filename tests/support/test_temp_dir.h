#pragma once

// A scratch directory of one test's own, under `<system temp>/engine-tests/`.
//
// Why this exists, and why nothing in a test may name a fixed path under the system temp
// directory again. Every test that needed scratch space used to open
// `temp_directory_path() / "engine_<something>_tests"`, start with `remove_all` on it and end
// with another. That is a *shared mutable global* whose scope is the whole machine, not the
// process: a second copy of the same test — another worktree's build, a release build beside a
// debug one, a second agent, a developer running the suite while CI runs it on the same box —
// lands on the same path and the two runs destroy each other's fixtures. What that looks like
// from the outside is a test that fails in a full run and passes when re-run alone, which reads
// like a timeout or a load problem and is neither. The measured form: two copies of the five
// end-to-end app test binaries run at the same time failed 19 times out of 30, while the same
// suite run alone, and run `-j 16 --repeat until-fail:3` with a release build compiling and
// three 1600x1000 engine-view windows open, passed every time. Load was never the cause.
// CTest's `RESOURCE_LOCK` does not help, because it orders tests *within one ctest invocation*
// and knows nothing about a second one.
//
// So: one directory per `TempDir` object, created under a name nothing else can guess, removed
// when the object dies. `create_directory` is the arbiter — it fails rather than succeeds when
// the name is taken, so uniqueness is settled by the filesystem and not by hoping. Everything a
// test writes goes inside it, and a test never removes anything above it. Whatever a crashed run
// leaves behind is under `<system temp>/engine-tests/` and can be deleted wholesale.
//
//   engine::test::TempDir tmp("engine_cli");
//   const std::string doc = tmp.file("world");     // "<...>/engine-tests/engine_cli-3f2a.../world"
//
// A directory that is cleaned away the moment a test fails takes the evidence with it, so a
// failing test keeps its scratch space and says on stderr where it is. What counts as failing
// is what the destructor can actually see: an exception on its way out, which is what doctest's
// `REQUIRE`, `FAIL` and an uncaught throw in the test body all are — the three that took these
// tests down every time they collided. A plain `CHECK` does not unwind and doctest exposes no
// supported way to ask whether the *current* test has failed (`CurrentTestCaseStats` is filled
// after the test body and its destructors have run, and the live counter is in an
// implementation-only struct), so a test that wants its fixture kept after a non-fatal failure
// asks for it with `tmp.keep()`.
//
// Header-only and standard-library-only: `engine_test_main` puts `tests/support` on every test
// target's include path, so `#include <test_temp_dir.h>` works from any module's tests without
// a new dependency.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>

namespace engine::test {

namespace detail {

// A name that is a function of the process, the moment, and a counter, so two processes never
// choose the same one and two objects in one process never do either. The filesystem still has
// the last word: `create_directory` reports whether this name was actually free.
inline std::uint64_t temp_dir_token() {
  static std::atomic<std::uint64_t> counter{0};
  static const std::uint64_t seed = [] {
    std::random_device entropy;
    return (static_cast<std::uint64_t>(entropy()) << 32) ^ static_cast<std::uint64_t>(entropy());
  }();
  const auto now =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t mixed = seed ^ (now * 0x9e3779b97f4a7c15ull) ^ (n * 0xbf58476d1ce4e5b9ull);
  mixed ^= mixed >> 33;
  mixed *= 0xff51afd7ed558ccdull;
  mixed ^= mixed >> 29;
  return mixed;
}

inline std::string temp_dir_hex(std::uint64_t value) {
  static const char* digits = "0123456789abcdef";
  std::string out(16, '0');
  for (std::size_t i = 0; i < 16; ++i)
    out[15 - i] = digits[(value >> (4 * i)) & 0xfu];
  return out;
}

// Anything that is not a plain file-name character becomes an underscore, so a caller may pass a
// test's own name without thinking about what the filesystem will make of it.
inline std::string temp_dir_label(std::string_view name) {
  std::string out;
  out.reserve(name.size());
  for (const char c : name) {
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '.' || c == '-' || c == '_';
    out.push_back(plain ? c : '_');
  }
  if (out.empty()) out = "test";
  if (out.size() > 48) out.resize(48);
  return out;
}

}  // namespace detail

class TempDir {
 public:
  // `name` only has to be recognizable in a directory listing; uniqueness is the token's job.
  explicit TempDir(std::string_view name = "test") : unwinding_(std::uncaught_exceptions()) {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "engine-tests";
    std::error_code ignored;
    std::filesystem::create_directories(root, ignored);
    const std::string label = detail::temp_dir_label(name);
    // A retry loop rather than one attempt: the only way to know a name is free is to have
    // created it, and a collision — however unlikely — must not turn into two tests sharing a
    // directory, which is the failure this class exists to end.
    for (int attempt = 0; attempt < 64; ++attempt) {
      std::filesystem::path candidate =
          root / (label + "-" + detail::temp_dir_hex(detail::temp_dir_token()));
      std::error_code error;
      if (std::filesystem::create_directory(candidate, error) && !error) {
        dir_ = std::move(candidate);
        break;
      }
    }
    path_ = dir_.string();
    for (char& c : path_) {
      if (c == '\\') c = '/';
    }
  }

  ~TempDir() {
    if (dir_.empty()) return;
    // More exceptions in flight than when this object was made means the test is on its way out
    // of a REQUIRE, a FAIL, or a throw: keep what it was working with and say where.
    if (keep_ || std::uncaught_exceptions() > unwinding_) {
      std::fprintf(stderr, "test scratch kept at %s\n", path_.c_str());
      return;
    }
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(TempDir&&) = delete;

  // UTF-8 with forward slashes and no trailing slash, which is the form the engine's own path
  // helpers and every command line in these tests use. Empty only if the directory could not be
  // created at all, which a caller may check with `ok()`.
  const std::string& path() const noexcept { return path_; }
  bool ok() const noexcept { return !dir_.empty(); }
  const std::filesystem::path& native() const noexcept { return dir_; }

  // Leave the directory behind whatever happens: for a test that wants to look at its fixture
  // after a `CHECK` that did not unwind, or while a case is being written.
  void keep() const noexcept { keep_ = true; }

  // A path inside this directory. It does not create anything.
  std::string file(std::string_view name) const {
    std::string out = path_;
    out.push_back('/');
    out.append(name);
    return out;
  }
  std::filesystem::path native_file(std::string_view name) const {
    return dir_ / std::filesystem::path(std::string(name));
  }

 private:
  std::filesystem::path dir_;
  std::string path_;
  mutable bool keep_ = false;
  int unwinding_ = 0;
};

}  // namespace engine::test

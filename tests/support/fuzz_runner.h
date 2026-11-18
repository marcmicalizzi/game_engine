#pragma once

// The in-tree half of the hostile-input harness: one doctest case a target, which replays the
// target's regressions, then runs its seeds through the mutators of fuzz_harness.h a counted number
// of times, and holds every input to the oracles below (roadmap T13, stage 0;
// docs/experiments/fuzzing-design.md, "2. The harness: both, with different jobs"; how to add a
// target and what a finding looks like: docs/ci/fuzzing.md).
//
// The oracles, each checked on every input:
//   - a crash or fault: the platform's fault report and doctest's crash report end the run, and
//     the INFO context below names the input; `CrashListener` keeps its bytes as well, best effort,
//     since it runs where the fault left the process;
//   - an `ENGINE_ASSERT` reached from bytes: a reader never asserts on external input (AGENTS.md,
//     "Conventions"), so the assert hook keeps the input, prints one line naming it and ends the
//     process with `k_exit_assert`;
//   - an allocation past the target's cap, against the engine allocator's counters (`mem::Stats`;
//     `AllocationWatch` says how a monotonic peak gives an exact answer);
//   - a hang: a watchdog thread whose only job is to name an input that has run for
//     `TargetSpec::watchdog_s` seconds, keep it, and end the process with `k_exit_hang`. Counted
//     mutation keeps an honest run far below it; it is never what bounds the run;
//   - a broken contract, which is the target function's own `Verdict`.
//
// A failing input is written into the case's `TempDir`, kept, and named with its target, seed and
// iteration (`<seed>#<iteration>`, or `regress/<file>`), which reproduces it on any machine:
// `ENGINE_FUZZ_REPLAY=<target>|<id>` runs that one input alone.
//
// Header-only. Needs `engine::json`, `engine::memory` and `engine::platform`, and the definitions
// `ENGINE_FUZZ_CONTENT_DIR` and `ENGINE_FUZZ_TEST_EXE_NAME`, all of which `engine_module_fuzz()`
// (cmake/EngineFuzz.cmake) gives the module's test executable.

#include <core/base/assert.h>
#include <core/base/types.h>
#include <core/memory/memory.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <fuzz_harness.h>
#include <test_environment.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifndef ENGINE_FUZZ_CONTENT_DIR
#error "engine_module_fuzz() (cmake/EngineFuzz.cmake) defines ENGINE_FUZZ_CONTENT_DIR"
#endif
#ifndef ENGINE_FUZZ_TEST_EXE_NAME
#error "engine_module_fuzz() (cmake/EngineFuzz.cmake) defines ENGINE_FUZZ_TEST_EXE_NAME"
#endif

namespace engine::test::fuzz {

// The exit codes of a process the harness ends itself; neither is used by anything else in the tree
// (75 is the GPU lock's, 78 the CPU baseline's).
inline constexpr int k_exit_assert = 86;
inline constexpr int k_exit_hang = 87;

// `<target>|<input id>`: run that one input of that target and nothing else.
inline constexpr const char* k_replay_env = "ENGINE_FUZZ_REPLAY";
// Where a finding's bytes are written, when not the case's own `TempDir`: what a parent case hands
// the child it starts, so the child's finding lands in the parent's scratch.
inline constexpr const char* k_keep_env = "ENGINE_FUZZ_KEEP_DIR";

// The corpus's caps (fuzzing-design.md, "Corpora"): a seed, a target's seeds, all of content/fuzz.
inline constexpr usize k_max_seed_bytes = 16 * 1024;
inline constexpr usize k_max_target_seed_bytes = 256 * 1024;
inline constexpr u64 k_max_corpus_bytes = 2 * 1024 * 1024;
// A run stops at this many findings: past it the report is noise.
inline constexpr u32 k_max_findings = 8;

namespace detail {

inline i64 now_ns() {
  return static_cast<i64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count());
}

// The input being run, for the three that have to name it from outside the call: the assert hook,
// the watchdog and the crash listener. Set before the call, cleared after it.
struct Current {
  std::atomic<bool> active{false};
  std::atomic<i64> started_ns{0};
  std::atomic<const u8*> data{nullptr};
  std::atomic<usize> size{0};
  std::atomic<const std::string*> id{nullptr};
  const char* target = "";
  const std::string* keep_dir = nullptr;
};
inline Current g_current;

inline std::string kept_path(const char* target, const std::string& id, const std::string& dir) {
  std::string name(target);
  name.push_back('-');
  for (const char c : id) {
    const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '.' || c == '-' || c == '_';
    name.push_back(plain ? c : '_');
  }
  name += ".input";
  return dir + "/" + name;
}

// Best effort: a stream error is a missing file, never a throw, and nothing asserts on it.
inline bool write_bytes(const std::string& path, const u8* data, usize size) {
  std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  return static_cast<bool>(out);
}

inline bool read_bytes(const std::filesystem::path& path, Bytes& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  out.assign(reinterpret_cast<const u8*>(text.data()),
             reinterpret_cast<const u8*>(text.data()) + text.size());
  return true;
}

// Keeps the current input and returns where, for the three outside the call.
inline std::string keep_current() {
  Current& c = g_current;
  const std::string* id = c.id.load(std::memory_order_acquire);
  if (id == nullptr || c.keep_dir == nullptr) return "(nowhere: no input is current)";
  const std::string path = kept_path(c.target, *id, *c.keep_dir);
  if (!write_bytes(path, c.data.load(std::memory_order_acquire),
                   c.size.load(std::memory_order_acquire))) {
    return path + " (could not be written)";
  }
  return path;
}

// The test main's own scratch (tests/support/test_main.cpp): the directory `ENGINE_TEST_RUN` names
// and, when the run reports its devices, the directory holding the `opened` marker. The main removes
// both as it returns; a process the harness ends with `std::_Exit` never returns, so the harness
// removes them first, or every assert and hang found would leave a directory under the tests'
// scratch root that nobody owns (AGENTS.md, "Conventions"). Each one removed is named on stderr,
// which is how a parent case checks that its child left nothing behind. The case's own `fuzz-*`
// scratch, which holds the kept input, is not touched.
inline void remove_main_scratch() {
  const std::string run = test::detail::environment("ENGINE_TEST_RUN");
  std::string devices = test::detail::environment("ENGINE_TEST_DEVICES");
  constexpr std::string_view k_marker = "/opened";
  if (devices.size() > k_marker.size() &&
      std::string_view(devices).substr(devices.size() - k_marker.size()) == k_marker) {
    devices.resize(devices.size() - k_marker.size());
  } else {
    devices.clear();
  }
  const std::string* const dirs[] = {&run, &devices};
  for (const std::string* dir : dirs) {
    if (dir->empty()) continue;
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(*dir), ec);
    std::fprintf(stderr, "fuzz: removed this process's test scratch %s\n", dir->c_str());
  }
}

inline const char* current_id() {
  const std::string* id = g_current.id.load(std::memory_order_acquire);
  return id != nullptr ? id->c_str() : "?";
}

// The assert oracle. `assert_fail` calls the hook and then aborts; an assert reached while an input
// is current is a finding, so the hook keeps the input, says so, and ends the process itself with a
// code a parent case can tell from any other failure. An assert anywhere else (the harness's own
// code) is left to `assert_fail`.
inline void on_assert(const char* expression, const char* message, const char* file,
                      int line) noexcept {
  if (!g_current.active.load(std::memory_order_acquire)) return;
  const std::string path = keep_current();
  std::fprintf(stderr, "\nfuzz finding: %s %s: ENGINE_ASSERT(%s) \"%s\" at %s:%d; input kept at %s\n",
               g_current.target, current_id(), expression, message != nullptr ? message : "", file,
               line, path.c_str());
  remove_main_scratch();
  std::fflush(stderr);
  std::_Exit(k_exit_assert);
}

class AssertHookScope {
 public:
  AssertHookScope() { set_assert_hook(&on_assert); }
  ~AssertHookScope() { set_assert_hook(nullptr); }
  AssertHookScope(const AssertHookScope&) = delete;
  AssertHookScope& operator=(const AssertHookScope&) = delete;
};

// The hang oracle: a thread that looks twice a second and, when one input has been current for the
// limit, keeps it, names it and ends the process. Its only job is to turn a hang into a failure
// that names the input; nothing about a passing run depends on it, and a run never waits for it.
class Watchdog {
 public:
  explicit Watchdog(u32 limit_s) : limit_ns_(static_cast<i64>(limit_s) * 1'000'000'000) {
    thread_ = std::thread([this] { loop(); });
  }
  ~Watchdog() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    thread_.join();
  }
  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

 private:
  void loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
      wake_.wait_for(lock, std::chrono::milliseconds(500));
      if (stop_) break;
      Current& c = g_current;
      if (!c.active.load(std::memory_order_acquire)) continue;
      const i64 started = c.started_ns.load(std::memory_order_acquire);
      if (now_ns() - started < limit_ns_) continue;
      // Still the same input: the runner restamps `started_ns` for every one.
      if (!c.active.load(std::memory_order_acquire) ||
          c.started_ns.load(std::memory_order_acquire) != started) {
        continue;
      }
      const std::string path = keep_current();
      std::fprintf(stderr,
                   "\nfuzz finding: %s %s: still running after %lld s (the watchdog's limit); "
                   "input kept at %s\n",
                   c.target, current_id(), static_cast<long long>(limit_ns_ / 1'000'000'000),
                   path.c_str());
      remove_main_scratch();
      std::fflush(stderr);
      std::_Exit(k_exit_hang);
    }
  }

  i64 limit_ns_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false;
  std::thread thread_;
};

// The allocation oracle, from counters that only grow: `bytes_peak` is the process's high-water
// mark and nothing resets it. If the peak rises during the call, the call's own peak is exactly
// `peak_after - base`, since nothing else allocates (no threads of the harness's allocate through
// the engine). If it does not rise, the call held at most `peak_before - base`, the headroom. So the
// check is exact whenever the headroom is at most the cap, and the watch makes it so by holding a
// ballast of `headroom - cap` bytes through the call when an earlier case left more. A ballast
// larger than `k_max_ballast` is not taken (the debug heap writes every byte it hands out), and that
// input's check is counted as blind rather than passed in silence.
//
// It sees the engine allocator only: the standard library's `new` (a `std::string` key) and a
// third-party `malloc` are not counted here, which is the nightly's `-malloc_limit_mb`.
class AllocationWatch {
 public:
  static constexpr u64 k_max_ballast = 8ull << 20;

  void begin(u64 cap) {
    cap_ = cap;
    blind_ = false;
    const mem::Stats before = mem::total_stats();
    const u64 headroom = before.bytes_peak - before.bytes_current;
    if (headroom > cap_) {
      const u64 need = headroom - cap_;
      if (need > k_max_ballast) {
        blind_ = true;
      } else {
        ballast_bytes_ = static_cast<usize>(need);
        ballast_ = mem::try_allocate(ballast_bytes_, 16);
        if (ballast_ == nullptr) {
          ballast_bytes_ = 0;
          blind_ = true;
        }
      }
    }
    const mem::Stats now = mem::total_stats();
    base_ = now.bytes_current;
    peak_before_ = now.bytes_peak;
  }

  // True when the call held more than the cap; `held` is what it held, when the peak says.
  bool end(u64& held) {
    const mem::Stats after = mem::total_stats();
    held = after.bytes_peak > peak_before_ ? after.bytes_peak - base_ : 0;
    if (ballast_ != nullptr) {
      mem::deallocate(ballast_, ballast_bytes_, 16);
      ballast_ = nullptr;
      ballast_bytes_ = 0;
    }
    return !blind_ && held > cap_;
  }

  bool blind() const noexcept { return blind_; }

 private:
  u64 cap_ = 0;
  u64 base_ = 0;
  u64 peak_before_ = 0;
  void* ballast_ = nullptr;
  usize ballast_bytes_ = 0;
  bool blind_ = false;
};

// The crash oracle's second half: doctest reports a fault as a crashed case with the INFO context
// that names the input; this keeps the input's bytes beside it. It runs inside doctest's fault
// handler, so it is best effort and never relied on.
class CrashListener : public doctest::IReporter {
 public:
  explicit CrashListener(const doctest::ContextOptions&) {}
  void report_query(const doctest::QueryData&) override {}
  void test_run_start() override {}
  void test_run_end(const doctest::TestRunStats&) override {}
  void test_case_start(const doctest::TestCaseData&) override {}
  void test_case_reenter(const doctest::TestCaseData&) override {}
  void test_case_end(const doctest::CurrentTestCaseStats&) override {}
  void test_case_exception(const doctest::TestCaseException& e) override {
    if (!e.is_crash || !g_current.active.load(std::memory_order_acquire)) return;
    const std::string path = keep_current();
    std::fprintf(stderr, "\nfuzz finding: %s %s: crashed (%s); input kept at %s\n",
                 g_current.target, current_id(), e.error_string.c_str(), path.c_str());
    std::fflush(stderr);
  }
  void subcase_start(const doctest::SubcaseSignature&) override {}
  void subcase_end() override {}
  void log_assert(const doctest::AssertData&) override {}
  void log_message(const doctest::MessageData&) override {}
  void test_case_skipped(const doctest::TestCaseData&) override {}
};

}  // namespace detail

REGISTER_LISTENER("engine_fuzz_crash", 1, detail::CrashListener);

// --- the corpus -----------------------------------------------------------------------------

// An input known to fail, with the roadmap row that will mend it. The run leaves it out and says
// how many it left out; and it replays every one in a child process, failing while an entry's input
// no longer fails, so the list cannot outlive what it lists.
struct KnownFailure {
  std::string id;       // `<seed>#<iteration>` or `regress/<file>`
  std::string roadmap;  // the roadmap id, letters then digits
  std::string why;
  u32 line = 0;
};

struct Corpus {
  std::string dir;  // content/fuzz/<target>, or empty when the target has none or it is absent
  std::vector<Seed> seeds;        // the builtin seeds, then the files of seeds/ by name
  std::vector<Seed> regressions;  // regress/<file>, by name, every one with a line in NOTES.txt
  std::vector<KnownFailure> known;
};

namespace detail {

inline std::string trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
    s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.remove_suffix(1);
  return std::string(s);
}

// The lines of a text file that are not empty and not `#` comments, with their numbers.
inline std::vector<std::pair<u32, std::string>> content_lines(const std::filesystem::path& path) {
  std::vector<std::pair<u32, std::string>> out;
  std::ifstream in(path);
  std::string line;
  u32 n = 0;
  while (std::getline(in, line)) {
    ++n;
    std::string t = trim(line);
    if (t.empty() || t.front() == '#') continue;
    out.emplace_back(n, std::move(t));
  }
  return out;
}

inline bool is_roadmap_id(std::string_view s) {
  usize letters = 0;
  while (letters < s.size() && s[letters] >= 'A' && s[letters] <= 'Z')
    ++letters;
  if (letters == 0 || letters == s.size()) return false;
  for (usize i = letters; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return false;
  }
  return true;
}

inline std::vector<std::filesystem::path> files_by_name(const std::filesystem::path& dir) {
  std::vector<std::filesystem::path> out;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return out;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec)) out.push_back(entry.path());
  }
  std::sort(out.begin(), out.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
    return a.filename().generic_string() < b.filename().generic_string();
  });
  return out;
}

inline u64 directory_bytes(const std::filesystem::path& dir) {
  u64 total = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec)) total += static_cast<u64>(entry.file_size(ec));
  }
  return total;
}

inline std::string input_id(const Seed& seed, u32 iteration) {
  return seed.name + "#" + std::to_string(iteration);
}

}  // namespace detail

// Reads a target's builtin seeds and, when it has content, `content/fuzz/<target>/`: `seeds/`,
// `regress/` with its `NOTES.txt`, and `known-failures.txt`. Every cap and every rule of the
// directory is a REQUIRE, so a corpus that broke one fails its case before any input runs.
inline Corpus load_corpus(const TargetSpec& spec) {
  Corpus corpus;
  for (usize i = 0; i < spec.builtin_seeds.size(); ++i)
    corpus.seeds.push_back(make_seed("builtin-" + std::to_string(i), spec.builtin_seeds[i]));
  if (!spec.has_content) return corpus;

  const std::string name(spec.name);
  const std::string root = data_path(ENGINE_FUZZ_CONTENT_DIR, "content/fuzz");
  corpus.dir = root + "/" + name;
  if (!path_exists(corpus.dir)) {
    MESSAGE("fuzz: " << name << ": no " << corpus.dir << " in this tree or bundle; builtin seeds only");
    corpus.dir.clear();
    return corpus;
  }
  const u64 all = detail::directory_bytes(std::filesystem::path(root));
  REQUIRE_MESSAGE(all <= k_max_corpus_bytes, "content/fuzz holds " << all << " bytes, past its cap of "
                                                                    << k_max_corpus_bytes);

  usize seed_bytes = 0;
  for (const std::filesystem::path& file : detail::files_by_name(corpus.dir + "/seeds")) {
    Seed seed;
    seed.name = file.filename().generic_string();
    REQUIRE_MESSAGE(detail::read_bytes(file, seed.bytes), "cannot read " << file.generic_string());
    REQUIRE_MESSAGE(seed.bytes.size() <= k_max_seed_bytes,
                    seed.name << " is " << seed.bytes.size() << " bytes, past a seed's cap of "
                              << k_max_seed_bytes);
    seed_bytes += seed.bytes.size();
    seed.hash = fnv1a(seed.bytes);
    corpus.seeds.push_back(std::move(seed));
  }
  REQUIRE_MESSAGE(seed_bytes <= k_max_target_seed_bytes,
                  name << "'s seeds are " << seed_bytes << " bytes, past a target's cap of "
                       << k_max_target_seed_bytes);

  // regress/: every input has its line in NOTES.txt, and every line names an input.
  const std::filesystem::path regress(corpus.dir + "/regress");
  std::vector<std::string> noted;
  for (const auto& [line, text] : detail::content_lines(regress / "NOTES.txt")) {
    const std::string file = text.substr(0, text.find(' '));
    REQUIRE_MESSAGE(text.size() > file.size(),
                    "regress/NOTES.txt:" << line << " names " << file << " and says nothing of it");
    noted.push_back(file);
  }
  for (const std::filesystem::path& file : detail::files_by_name(regress)) {
    const std::string base = file.filename().generic_string();
    if (base == "NOTES.txt") continue;
    REQUIRE_MESSAGE(std::find(noted.begin(), noted.end(), base) != noted.end(),
                    "regress/" << base << " has no line in regress/NOTES.txt saying what it found");
    Seed input;
    input.name = "regress/" + base;
    REQUIRE_MESSAGE(detail::read_bytes(file, input.bytes), "cannot read " << file.generic_string());
    input.hash = fnv1a(input.bytes);
    corpus.regressions.push_back(std::move(input));
  }
  for (const std::string& file : noted) {
    REQUIRE_MESSAGE(path_exists((regress / file).generic_string()),
                    "regress/NOTES.txt names " << file << ", which is not in regress/");
  }

  // known-failures.txt: `<input id> <roadmap id> <why>`, and every id names an input there is.
  const u32 per_seed = iterations_per_seed(spec);
  for (const auto& [line, text] : detail::content_lines(corpus.dir + "/known-failures.txt")) {
    KnownFailure k;
    k.line = line;
    const usize a = text.find(' ');
    REQUIRE_MESSAGE(a != std::string::npos, "known-failures.txt:" << line << ": no roadmap id");
    k.id = text.substr(0, a);
    const std::string rest = detail::trim(std::string_view(text).substr(a));
    const usize b = rest.find(' ');
    k.roadmap = rest.substr(0, b);
    k.why = b == std::string::npos ? std::string() : detail::trim(std::string_view(rest).substr(b));
    REQUIRE_MESSAGE(detail::is_roadmap_id(k.roadmap),
                    "known-failures.txt:" << line << ": " << k.roadmap << " is not a roadmap id");
    REQUIRE_MESSAGE(!k.why.empty(), "known-failures.txt:" << line << ": says nothing of why");
    bool exists = false;
    for (const Seed& r : corpus.regressions)
      exists = exists || r.name == k.id;
    const usize hash = k.id.rfind('#');
    if (!exists && hash != std::string::npos) {
      const std::string seed_name = k.id.substr(0, hash);
      const std::string number = k.id.substr(hash + 1);
      u32 iteration = 0;
      const auto parsed =
          std::from_chars(number.data(), number.data() + number.size(), iteration);
      const bool numeric = parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size();
      for (const Seed& s : corpus.seeds)
        exists = exists || (numeric && s.name == seed_name && iteration < per_seed);
    }
    REQUIRE_MESSAGE(exists, "known-failures.txt:" << line << ": " << k.id
                                                   << " names no input of this target");
    corpus.known.push_back(std::move(k));
  }
  return corpus;
}

// --- one input, and a child that runs one ---------------------------------------------------

struct RunState {
  const TargetSpec* spec = nullptr;
  std::string keep_dir;
  TempDir* scratch = nullptr;  // null when a parent named the keep directory
  u64 inputs = 0;
  u64 blind = 0;
  u32 findings = 0;
};

// Runs one input through the target under every oracle. A broken contract or an allocation past the
// cap keeps the input and fails the case; an assert, a hang or a crash ends the process from the
// hook, the watchdog or doctest's handler, each naming the input.
inline void check_one(RunState& run, const std::string& id, const Bytes& input) {
  const TargetSpec& spec = *run.spec;
  INFO("fuzz input: " << spec.name << " " << id);
  detail::Current& c = detail::g_current;
  c.id.store(&id, std::memory_order_release);
  c.data.store(input.data(), std::memory_order_release);
  c.size.store(input.size(), std::memory_order_release);
  c.started_ns.store(detail::now_ns(), std::memory_order_release);
  c.active.store(true, std::memory_order_release);

  const u64 cap = spec.cap_floor + spec.cap_per_byte * static_cast<u64>(input.size());
  detail::AllocationWatch watch;
  watch.begin(cap);
  const Verdict verdict = spec.one(std::span<const u8>(input.data(), input.size()));
  u64 held = 0;
  const bool breach = watch.end(held);

  c.active.store(false, std::memory_order_release);
  c.id.store(nullptr, std::memory_order_release);
  ++run.inputs;
  if (watch.blind()) ++run.blind;

  std::string why;
  if (!verdict.ok) {
    why = verdict.why.empty() ? std::string("a broken contract with no reason given") : verdict.why;
  } else if (breach) {
    why = "held " + std::to_string(held) + " bytes of the engine allocator, past its cap of " +
          std::to_string(cap);
  }
  if (why.empty()) return;

  ++run.findings;
  if (run.scratch != nullptr) run.scratch->keep();
  std::string path = detail::kept_path(spec.name, id, run.keep_dir);
  if (!detail::write_bytes(path, input.data(), input.size())) path += " (could not be written)";
  std::fprintf(stderr, "\nfuzz finding: %s %s: %s; input kept at %s\n", spec.name, id.c_str(),
               why.c_str(), path.c_str());
  FAIL_CHECK("fuzz finding: " << spec.name << " " << id << ": " << why << "; input kept at " << path);
  if (run.findings >= k_max_findings) FAIL("fuzz: " << spec.name << ": stopped at " << run.findings << " findings");
}

struct ChildRun {
  int code = -1;
  std::string output;
};

// Runs one doctest case of this same executable in a child process, with the harness's two
// variables set for it, and returns its exit code and everything it printed. How an input that ends
// a process (an assert, a hang) is checked without ending this one.
inline ChildRun run_child_case(const std::string& test_case, const std::string& keep_dir,
                               const std::string& replay) {
  const ScopedEnv keep(k_keep_env, keep_dir);
  const ScopedEnv one(k_replay_env, replay);
  const std::string exe = platform::executable_directory() + "/" + ENGINE_FUZZ_TEST_EXE_NAME;
  const std::string filter = "--test-case=" + test_case;
  const std::string_view argv[] = {exe, filter, "--no-skip=true"};
  ChildRun run;
  platform::Process process;
  std::string error;
  if (!process.spawn(std::span<const std::string_view>(argv), &error, true)) {
    run.output = "could not start " + exe + ": " + error;
    return run;
  }
  process.close_stdin();
  process.read_all(run.output);
  run.code = process.wait();
  return run;
}

// --- the case -------------------------------------------------------------------------------

// The body of a target's doctest case: regressions first, then every seed's inputs, then the known
// failures checked in children; or, under `ENGINE_FUZZ_REPLAY`, the one input it names.
inline void run_target(const TargetSpec& spec) {
  REQUIRE(spec.one != nullptr);
  const std::string name(spec.name);
  const std::string replay = test::detail::environment(k_replay_env);
  const std::string keep_env = test::detail::environment(k_keep_env);

  std::optional<TempDir> scratch;
  RunState run;
  run.spec = &spec;
  if (keep_env.empty()) {
    scratch.emplace("fuzz-" + name);
    REQUIRE(scratch->ok());
    run.scratch = &*scratch;
    run.keep_dir = scratch->path();
  } else {
    run.keep_dir = keep_env;
  }

  const Corpus corpus = load_corpus(spec);
  REQUIRE_MESSAGE(!corpus.seeds.empty(), "fuzz: " << name << " has no seed");

  detail::g_current.target = spec.name;
  detail::g_current.keep_dir = &run.keep_dir;
  const detail::AssertHookScope hook;
  const detail::Watchdog watchdog(spec.watchdog_s);

  if (!replay.empty()) {
    const usize bar = replay.find('|');
    if (bar == std::string::npos || replay.substr(0, bar) != name) {
      MESSAGE("fuzz: " << name << ": " << k_replay_env << " names another target: " << replay);
      return;
    }
    const std::string id = replay.substr(bar + 1);
    for (const Seed& r : corpus.regressions) {
      if (r.name == id) {
        check_one(run, id, r.bytes);
        return;
      }
    }
    for (usize s = 0; s < corpus.seeds.size(); ++s) {
      for (u32 i = 0; i < iterations_per_seed(spec); ++i) {
        if (detail::input_id(corpus.seeds[s], i) != id) continue;
        check_one(run, id, generate(spec, corpus.seeds, s, i));
        return;
      }
    }
    FAIL("fuzz: " << name << ": " << k_replay_env << " names no input of this target: " << id);
  }

  std::vector<std::string> left_out;
  for (const KnownFailure& k : corpus.known)
    left_out.push_back(k.id);
  std::sort(left_out.begin(), left_out.end());
  const auto known = [&](const std::string& id) {
    return std::binary_search(left_out.begin(), left_out.end(), id);
  };

  u64 skipped = 0;
  for (const Seed& r : corpus.regressions) {
    if (known(r.name)) {
      ++skipped;
      continue;
    }
    check_one(run, r.name, r.bytes);
  }
  for (usize s = 0; s < corpus.seeds.size(); ++s) {
    for (u32 i = 0; i < iterations_per_seed(spec); ++i) {
      const std::string id = detail::input_id(corpus.seeds[s], i);
      if (known(id)) {
        ++skipped;
        continue;
      }
      check_one(run, id, generate(spec, corpus.seeds, s, i));
    }
  }

  // Each known failure must still fail, or its line goes: replayed alone in a child, it has to end
  // with a finding of the harness's own.
  for (const KnownFailure& k : corpus.known) {
    const ChildRun child = run_child_case("fuzz: " + name, run.keep_dir, name + "|" + k.id);
    const bool still_fails =
        child.code != 0 && child.output.find("fuzz finding:") != std::string::npos;
    CHECK_MESSAGE(still_fails, "known-failures.txt:"
                                   << k.line << ": " << k.id << " (" << k.roadmap
                                   << ") no longer fails; remove its line. The child said:\n"
                                   << child.output);
  }

  MESSAGE("fuzz: " << name << ": " << corpus.seeds.size() << " seeds, " << corpus.regressions.size()
                   << " regressions, " << run.inputs << " inputs, " << skipped
                   << " known failures left out (" << corpus.known.size() << " listed), "
                   << run.blind << " allocation checks blind, " << run.findings << " findings");
}

}  // namespace engine::test::fuzz

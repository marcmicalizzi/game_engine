#pragma once

// Micro-benchmark harness (ADR-0011; docs/plan/11-performance-principles.md §11.8): "is this
// faster?" is a command, not an opinion.
//
//     ENGINE_BENCH_ARGS(flat_map_find, "containers.find.flat_map", 16, 256, 4096) {
//       const u32 n = static_cast<u32>(state.arg());
//       FlatMap<u32, u32> map = build(n);              // setup: before the loop, untimed
//       while (state.keep_running()) {                  // the harness picks the iteration count
//         u64 sum = 0;
//         for (const u32 k : keys) sum += *map.find_value(k);
//         bench::keep(sum);                             // the result is "used"
//       }
//       state.set_items(n);                             // per iteration, for a rate column
//     }
//
// Benchmarks register at static initialization and live in each module's bench/ directory;
// engine_module_bench() builds one executable per module with run_main as its entry point.
// The runner calibrates an iteration count that fills a minimum time, repeats it, and reports
// the median, minimum, and spread per iteration, as a table and as JSON lines. A run can
// sweep a tunable over candidate values so a parameter's effect is measured rather than
// argued about, which is the input experiment E3 needs.
//
// Measurements from one machine settle layout and traversal decisions; they do not decide
// cross-machine defaults (11 §11.8).

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <foundation/bench/machine_state.h>

#include <atomic>
#include <span>
#include <string>
#include <string_view>

namespace engine::bench {

struct Runner;

// Handed to every benchmark body. Timing starts at the first keep_running() and stops when it
// returns false; pause_timing()/resume_timing() exclude setup inside the loop.
class State {
 public:
  bool keep_running() noexcept;
  i64 arg(usize i = 0) const noexcept { return i < args_.size() ? args_[i] : 0; }
  usize arg_count() const noexcept { return args_.size(); }
  u64 iterations() const noexcept { return target_; }
  void set_items(u64 per_iteration) noexcept { items_ = per_iteration; }
  void set_bytes(u64 per_iteration) noexcept { bytes_ = per_iteration; }
  void pause_timing() noexcept;
  void resume_timing() noexcept;

 private:
  friend struct Runner;
  State(std::span<const i64> args, u64 target) noexcept : args_(args), target_(target) {}
  std::span<const i64> args_;
  u64 target_;
  u64 done_ = 0;
  i64 started_ns_ = 0;
  i64 elapsed_ns_ = 0;
  bool started_ = false;
  bool timing_ = false;
  u64 items_ = 0;
  u64 bytes_ = 0;
};

namespace detail {
// Out of line and opaque, so the compiler must materialize whatever address it receives.
ENGINE_NO_INLINE void escape(const volatile void* p) noexcept;
}  // namespace detail

// Marks a value as used so the optimizer keeps the work that produced it.
template <class T>
ENGINE_FORCE_INLINE void keep(const T& value) noexcept {
  detail::escape(&value);
  std::atomic_signal_fence(std::memory_order_acq_rel);
}
// Compiler barrier: memory may have been read or written here.
ENGINE_FORCE_INLINE void clobber_memory() noexcept {
  std::atomic_signal_fence(std::memory_order_acq_rel);
}

using Fn = void (*)(State&);

class Registration {
 public:
  Registration(const char* name, Fn function, std::span<const i64> args = {}) noexcept;
  ~Registration();
  ENGINE_NON_COPYABLE(Registration);

  const char* name() const noexcept { return name_; }
  Fn fn() const noexcept { return fn_; }
  std::span<const i64> args() const noexcept { return args_; }
  Registration* next() const noexcept { return next_; }

 private:
  const char* name_;
  Fn fn_;
  std::span<const i64> args_;
  Registration* next_ = nullptr;
};

Registration* first_registration() noexcept;
usize registration_count() noexcept;

// Pattern with '*' wildcards, anchored; a pattern without '*' matches as a substring; the
// empty pattern matches everything.
bool glob_match(std::string_view pattern, std::string_view text) noexcept;

struct Stats {
  f64 median = 0;
  f64 min = 0;
  f64 max = 0;
  f64 mean = 0;
  f64 stddev = 0;  // population
};
Stats compute_stats(std::span<const f64> samples);

struct Options {
  std::string_view filter;
  u32 repeats = 7;
  u32 warmup_repeats = 1;
  i64 min_time_ns = 100'000'000;  // per repeat
  bool pin = true;                // pin to a performance CPU and raise priority
  bool smoke = false;             // one iteration, one repeat: does it run at all
  bool quiet = false;             // no table on stdout
  std::string_view json_path;     // append JSON lines here
  std::string_view sweep;         // "tunable=v1;v2;v3"
  std::string_view overrides;     // "tunable=value,..." applied before running

  // Measurement hygiene on a shared machine (machine_state.h, docs/subsystems/bench.md). A
  // measured run samples the machine before and after itself; `smoke` runs skip all of this,
  // because a "does it run at all" pass is not a measurement and CTest should not pay for it.
  QuietThresholds quiet_thresholds;   // what counts as a machine somebody else is using
  bool require_quiet = false;         // refuse to measure a busy machine: exit code 4, nothing run
  i64 wait_quiet_s = 0;               // poll every 5 s until quiet or this many seconds have passed
  MachineSampler* sampler = nullptr;  // null uses system_sampler(); tests inject a fake

  // The machine-wide GPU lock (foundation/gpu_lock/gpu_lock.h). With `gpu_lock`, a measured run
  // waits for the lock, holds it from before its first sample to after its last, refreshes it
  // between benchmarks, and releases it however it ends; a lock a wrapper already took for this
  // process is used as it is. A smoke run never takes it.
  bool gpu_lock = false;
  i64 gpu_lock_timeout_s = 4 * 3600;  // give up (exit 4) if somebody else still holds it by then
  i64 gpu_lock_poll_s = 20;           // GPU-LOCK.md asks waiters to look every 15-30 s
  // Empty: gpu_lock::default_path(), and the sampler's own reading of the lock stands. Set, the
  // run takes *and* reads the lock here instead — which is how a test keeps the machine's real
  // lock, and whoever holds it, out of its results.
  std::string_view gpu_lock_path;
};

// run() returns this instead of measuring when --require-quiet finds a busy machine, and when
// --gpu-lock gave up waiting for somebody else's lock: both mean "come back later".
inline constexpr int k_exit_not_quiet = 4;

struct Result {
  std::string name;     // registration name, plus "/<arg>" when the benchmark has arguments
  std::string tunable;  // "name=value" during a sweep, otherwise empty
  u64 iterations = 0;   // per repeat
  u32 repeats = 0;
  f64 median_ns = 0;  // per iteration
  f64 min_ns = 0;
  f64 max_ns = 0;
  f64 mean_ns = 0;
  f64 stddev_ns = 0;
  u64 items_per_iteration = 0;
  u64 bytes_per_iteration = 0;
  // The worst "others" CPU percentage any sample of the run this result came from saw, so that
  // one JSON line carries its own caveat. False when the run took no sample (a smoke run).
  bool machine_state_known = false;
  f64 worst_others_cpu_pct = 0;

  f64 items_per_second() const noexcept {
    return median_ns > 0 ? static_cast<f64>(items_per_iteration) * 1e9 / median_ns : 0;
  }
  f64 bytes_per_second() const noexcept {
    return median_ns > 0 ? static_cast<f64>(bytes_per_iteration) * 1e9 / median_ns : 0;
  }
};

// Runs every registered benchmark matching the filter. Returns a process exit code: 0 on
// success, 2 for a bad option or tunable, 4 when --require-quiet found a busy machine (nothing
// was measured). Results are appended to `results` when given, after the closing sample, so
// that every one of them carries the run's worst "others" CPU.
int run(const Options& options, Vector<Result>* results = nullptr);
// True while run() executes with Options::smoke: fixtures built outside any State (a corpus a
// whole bench file shares) size themselves from this, so the CTest smoke run stays seconds long.
bool smoke_mode() noexcept;

// Command-line entry point used by every bench executable:
//   --list  --filter=<glob>  --repeats=N  --warmup=N  --min-time=<ms>  --smoke  --quiet
//   --no-pin  --json=<path>  --sweep=<tunable>=<v1;v2;...>  --set=<tunable=value,...>
//   --require-quiet  --wait-quiet=<seconds>  --quiet-cpu=<pct>  --quiet-gpu=<pct>
//   --gpu-lock[=<max wait seconds>]
// A bare argument is a filter.
int run_main(int argc, char** argv);

}  // namespace engine::bench

// Defines and registers a benchmark function `ident` under `name`.
#define ENGINE_BENCH(ident, name)                                                         \
  static void ident(::engine::bench::State& state);                                       \
  static ::engine::bench::Registration ENGINE_CONCAT(ident, _registration){name, &ident}; \
  static void ident(::engine::bench::State& state)

// Same, run once per argument; the body reads the current one with state.arg().
#define ENGINE_BENCH_ARGS(ident, name, ...)                                     \
  static void ident(::engine::bench::State& state);                             \
  static constexpr ::engine::i64 ENGINE_CONCAT(ident, _args)[] = {__VA_ARGS__}; \
  static ::engine::bench::Registration ENGINE_CONCAT(ident, _registration){     \
      name, &ident, ENGINE_CONCAT(ident, _args)};                               \
  static void ident(::engine::bench::State& state)

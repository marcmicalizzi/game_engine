#include <core/base/assert.h>
#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/platform/cpu_features.h>
#include <core/platform/spin_lock.h>
#include <core/platform/thread.h>
#include <core/platform/topology.h>
#include <core/time/time.h>
#include <foundation/bench/bench.h>
#include <foundation/bench/machine_state.h>
#include <foundation/tunables/tunables.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace engine::bench {

namespace detail {
void escape(const volatile void*) noexcept {}
}  // namespace detail

namespace {

bool g_smoke = false;

struct RegistryState {
  platform::SpinLock lock;
  Registration* head = nullptr;
  usize count = 0;
};

RegistryState g_registry;

struct Measurement {
  i64 elapsed_ns = 0;
  u64 items = 0;
  u64 bytes = 0;
};

void append_i64(std::string& out, i64 v) {
  char buf[24];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, static_cast<usize>(r.ptr - buf));
}

// "123.4 ns", "12.34 us", "1.234 ms", "2.500 s".
void format_duration(f64 ns, char* buf, usize n) {
  if (ns < 1e3) {
    std::snprintf(buf, n, "%.1f ns", ns);
  } else if (ns < 1e6) {
    std::snprintf(buf, n, "%.2f us", ns / 1e3);
  } else if (ns < 1e9) {
    std::snprintf(buf, n, "%.3f ms", ns / 1e6);
  } else {
    std::snprintf(buf, n, "%.3f s", ns / 1e9);
  }
}

// "1.23 G/s", "456 M/s", "12.3 K/s", "5.0 /s".
void format_rate(f64 per_second, char* buf, usize n) {
  if (per_second <= 0) {
    std::snprintf(buf, n, "-");
  } else if (per_second >= 1e9) {
    std::snprintf(buf, n, "%.2f G/s", per_second / 1e9);
  } else if (per_second >= 1e6) {
    std::snprintf(buf, n, "%.1f M/s", per_second / 1e6);
  } else if (per_second >= 1e3) {
    std::snprintf(buf, n, "%.1f K/s", per_second / 1e3);
  } else {
    std::snprintf(buf, n, "%.1f /s", per_second);
  }
}

void write_line(std::FILE* f, const char* text) { std::fputs(text, f); }

bool parse_u32(std::string_view text, u32& out) noexcept {
  const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
  return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

bool parse_f64(std::string_view text, f64& out) noexcept {
  const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
  return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

// A filter may name one variant of an ENGINE_BENCH_ARGS registration the way `--list` prints it
// — `sim.tiers.assign/4`, `physics.e19.tick/216010081`, `*/4` — and everything before the first
// '/' is what selects the registration. Without this, the only way to reach one variant of a
// fifty-variant registration was to run all fifty, and on a shared machine the length of a run
// is the length of the window that has to stay quiet (see "Measuring on a shared machine").
std::string_view registration_filter(std::string_view filter) noexcept {
  const usize slash = filter.find('/');
  return slash == std::string_view::npos ? filter : filter.substr(0, slash);
}

// The name one variant would be reported under, which is what a filter carrying a '/' is
// matched against.
void variant_name(const Registration& reg, std::span<const i64> args, char* out,
                  usize capacity) noexcept {
  if (args.empty()) {
    std::snprintf(out, capacity, "%s", reg.name());
  } else {
    std::snprintf(out, capacity, "%s/%lld", reg.name(), static_cast<long long>(args[0]));
  }
}

Vector<const Registration*> matching(std::string_view filter) {
  Vector<const Registration*> out;
  {
    std::lock_guard lock(g_registry.lock);
    for (const Registration* r = g_registry.head; r != nullptr; r = r->next()) {
      if (glob_match(filter, r->name())) out.push_back(r);
    }
  }
  std::sort(out.begin(), out.end(), [](const Registration* a, const Registration* b) {
    return std::strcmp(a->name(), b->name()) < 0;
  });
  return out;
}

const char* usage_text() {
  return "usage: <bench> [filter] [--list] [--filter=<glob>] [--repeats=N] [--warmup=N]\n"
         "               [--min-time=<ms>] [--smoke] [--quiet] [--no-pin] [--json=<path>]\n"
         "               [--sweep=<tunable>=<v1;v2;...>] [--set=<tunable=value,...>]\n"
         "               [--require-quiet] [--wait-quiet=<seconds>]\n"
         "               [--quiet-cpu=<pct>] [--quiet-gpu=<pct>]\n";
}

// How often --wait-quiet looks again. A diffusion job or another agent's build ends on its own
// schedule and polling faster only costs a CPU sample each time.
constexpr i64 k_poll_interval_s = 5;

}  // namespace

// Owns the measurement protocol; the only code that constructs a State.
struct Runner {
  static Measurement measure(const Registration& reg, std::span<const i64> args, u64 iterations) {
    State state(args, iterations);
    reg.fn()(state);
    state.pause_timing();
    return Measurement{state.elapsed_ns_, state.items_, state.bytes_};
  }

  // Grows the iteration count until one repeat fills min_time_ns.
  static u64 calibrate(const Registration& reg, std::span<const i64> args, i64 min_time_ns) {
    u64 iterations = 1;
    for (;;) {
      const Measurement m = measure(reg, args, iterations);
      if (m.elapsed_ns >= min_time_ns || iterations >= (u64{1} << 40)) return iterations;
      const f64 per_iteration =
          static_cast<f64>(std::max<i64>(m.elapsed_ns, 1)) / static_cast<f64>(iterations);
      const auto target = static_cast<u64>(static_cast<f64>(min_time_ns) * 1.2 / per_iteration);
      iterations = std::max(target + 1, iterations * 2);
    }
  }

  static Result measure_one(const Registration& reg, std::span<const i64> args,
                            const Options& options) {
    Result result;
    result.name = reg.name();
    if (!args.empty()) {
      result.name.push_back('/');
      append_i64(result.name, args[0]);
    }
    const u64 iterations = options.smoke ? 1 : calibrate(reg, args, options.min_time_ns);
    const u32 repeats = options.smoke ? 1 : std::max<u32>(options.repeats, 1);
    const u32 warmup = options.smoke ? 0 : options.warmup_repeats;
    for (u32 i = 0; i < warmup; ++i)
      (void)measure(reg, args, iterations);

    Vector<f64> samples;
    samples.reserve(repeats);
    for (u32 i = 0; i < repeats; ++i) {
      const Measurement m = measure(reg, args, iterations);
      samples.push_back(static_cast<f64>(m.elapsed_ns) / static_cast<f64>(iterations));
      result.items_per_iteration = m.items;
      result.bytes_per_iteration = m.bytes;
    }
    const Stats s = compute_stats(samples);
    result.iterations = iterations;
    result.repeats = repeats;
    result.median_ns = s.median;
    result.min_ns = s.min;
    result.max_ns = s.max;
    result.mean_ns = s.mean;
    result.stddev_ns = s.stddev;
    return result;
  }
};

// ---- State -----------------------------------------------------------------------------------

bool State::keep_running() noexcept {
  if (!started_) {
    started_ = true;
    resume_timing();
    return target_ > 0;
  }
  if (++done_ >= target_) {
    pause_timing();
    return false;
  }
  return true;
}

void State::pause_timing() noexcept {
  if (timing_) {
    elapsed_ns_ += time::monotonic_ns() - started_ns_;
    timing_ = false;
  }
}

void State::resume_timing() noexcept {
  if (!timing_) {
    started_ns_ = time::monotonic_ns();
    timing_ = true;
  }
}

// ---- registry --------------------------------------------------------------------------------

Registration::Registration(const char* name, Fn function, std::span<const i64> args) noexcept
    : name_(name), fn_(function), args_(args) {
  ENGINE_VERIFY(name != nullptr && name[0] != '\0' && function != nullptr,
                "bench: registration needs a name and a function");
  std::lock_guard lock(g_registry.lock);
  next_ = g_registry.head;
  g_registry.head = this;
  ++g_registry.count;
}

Registration::~Registration() {
  std::lock_guard lock(g_registry.lock);
  Registration** link = &g_registry.head;
  while (*link != nullptr && *link != this)
    link = &(*link)->next_;
  if (*link == this) {
    *link = next_;
    --g_registry.count;
  }
}

Registration* first_registration() noexcept {
  std::lock_guard lock(g_registry.lock);
  return g_registry.head;
}

usize registration_count() noexcept {
  std::lock_guard lock(g_registry.lock);
  return g_registry.count;
}

// ---- helpers ---------------------------------------------------------------------------------

bool glob_match(std::string_view pattern, std::string_view text) noexcept {
  if (pattern.empty()) return true;
  if (pattern.find('*') == std::string_view::npos) {
    return text.find(pattern) != std::string_view::npos;
  }
  // Iterative wildcard match with backtracking to the last '*'.
  usize p = 0;
  usize t = 0;
  usize star = std::string_view::npos;
  usize star_t = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      star_t = t;
    } else if (p < pattern.size() && pattern[p] == text[t]) {
      ++p;
      ++t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++star_t;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*')
    ++p;
  return p == pattern.size();
}

Stats compute_stats(std::span<const f64> samples) {
  Stats s;
  if (samples.empty()) return s;
  Vector<f64> sorted;
  sorted.reserve(static_cast<u32>(samples.size()));
  for (const f64 v : samples)
    sorted.push_back(v);
  std::sort(sorted.begin(), sorted.end());
  const u32 n = sorted.size();
  s.min = sorted.front();
  s.max = sorted.back();
  s.median = (n % 2 == 1) ? sorted[n / 2] : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
  f64 sum = 0;
  for (const f64 v : sorted)
    sum += v;
  s.mean = sum / static_cast<f64>(n);
  f64 sq = 0;
  for (const f64 v : sorted)
    sq += (v - s.mean) * (v - s.mean);
  s.stddev = std::sqrt(sq / static_cast<f64>(n));
  return s;
}

// ---- run -------------------------------------------------------------------------------------

bool smoke_mode() noexcept { return g_smoke; }

int run(const Options& options, Vector<Result>* results) {
  g_smoke = options.smoke;
  if (!options.overrides.empty()) {
    std::string error;
    if (!tunables::apply_overrides(options.overrides, &error)) {
      std::fprintf(stderr, "bench: %s\n", error.c_str());
      return 2;
    }
  }

  tunables::Tunable* sweep_target = nullptr;
  Vector<std::string_view> sweep_values;
  std::string sweep_original;
  if (!options.sweep.empty()) {
    const usize eq = options.sweep.find('=');
    if (eq == std::string_view::npos) {
      std::fprintf(stderr, "bench: --sweep expects <tunable>=<v1;v2;...>\n");
      return 2;
    }
    const std::string name(options.sweep.substr(0, eq));
    sweep_target = tunables::find(name);
    if (sweep_target == nullptr) {
      std::fprintf(stderr, "bench: unknown tunable '%s'\n", name.c_str());
      return 2;
    }
    std::string_view rest = options.sweep.substr(eq + 1);
    while (!rest.empty()) {
      const usize semi = rest.find(';');
      const std::string_view v = rest.substr(0, semi);
      if (!v.empty()) sweep_values.push_back(v);
      rest = semi == std::string_view::npos ? std::string_view{} : rest.substr(semi + 1);
    }
    if (sweep_values.empty()) {
      std::fprintf(stderr, "bench: --sweep needs at least one value\n");
      return 2;
    }
    sweep_target->append_value(sweep_original);
  }

  // What else the machine is doing (machine_state.h). A smoke run neither samples nor waits:
  // it answers "does this run at all", CTest runs one per module, and a quarter second plus a
  // process spawn per module buys nothing there. Nothing has been changed yet at this point, so
  // an early return needs no unwinding.
  MachineSampler& sampler = options.sampler != nullptr ? *options.sampler : system_sampler();
  MachineState start;
  const bool sampled = !options.smoke;
  if (sampled) {
    start = sampler.sample(k_sample_window_ms);
    if (!is_quiet(start, options.quiet_thresholds)) {
      if (options.require_quiet) {
        std::fprintf(stderr, "bench: the machine is busy and --require-quiet was given: %s\n",
                     describe(start).c_str());
        return k_exit_not_quiet;
      }
      if (options.wait_quiet_s > 0) {
        std::fprintf(stderr, "bench: waiting up to %lld s for a quiet machine: %s\n",
                     static_cast<long long>(options.wait_quiet_s), describe(start).c_str());
        const i64 began_ns = time::monotonic_ns();
        const i64 deadline_ns = began_ns + options.wait_quiet_s * i64{1'000'000'000};
        for (;;) {
          // Never sleep past the deadline: --wait-quiet is a bound on the waiting, not a
          // rounding of it up to the next poll.
          const i64 remaining_ns = deadline_ns - time::monotonic_ns();
          if (remaining_ns <= 0) break;
          const i64 sleep_ns = std::min(remaining_ns, k_poll_interval_s * i64{1'000'000'000});
          std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
          start = sampler.sample(k_sample_window_ms);
          if (is_quiet(start, options.quiet_thresholds)) break;
        }
        std::fprintf(stderr, "bench: proceeding on a %s machine after %.0f s: %s\n",
                     is_quiet(start, options.quiet_thresholds) ? "quiet" : "busy",
                     static_cast<f64>(time::monotonic_ns() - began_ns) / 1e9,
                     describe(start).c_str());
      }
    }
  }

  if (options.pin && !options.smoke) {
    const platform::Topology& topo = platform::topology();
    if (!topo.performance_cpus.empty())
      (void)platform::pin_current_thread(topo.performance_cpus.first());
    (void)platform::set_current_thread_priority(platform::ThreadPriority::High);
  }

  // The file is opened now so that a path that cannot be written fails before anything runs,
  // and written when the run ends: the header carries the machine's state at *both* ends of the
  // run, and the closing sample does not exist until the last benchmark has finished. A run
  // killed part way therefore leaves no JSON, which is the right trade — its numbers were taken
  // under whatever killed it.
  std::FILE* json = nullptr;
  if (!options.json_path.empty()) {
    const std::string path(options.json_path);
#if ENGINE_COMPILER_MSVC
    (void)fopen_s(&json, path.c_str(), "ab");
#else
    json = std::fopen(path.c_str(), "ab");
#endif
    if (json == nullptr) {
      std::fprintf(stderr, "bench: cannot open '%s'\n", path.c_str());
      return 2;
    }
  }

  const Vector<const Registration*> regs = matching(registration_filter(options.filter));
  const bool per_variant = options.filter.find('/') != std::string::npos;
  if (!options.quiet) {
    char line[256];
    std::snprintf(line, sizeof(line), "%-56s %12s %12s %8s %12s%s\n", "benchmark", "median/iter",
                  "min/iter", "spread", "rate", sweep_target != nullptr ? "  relative" : "");
    write_line(stdout, line);
  }

  // Baseline medians per result name for the relative column of a sweep.
  Vector<std::string> baseline_names;
  Vector<f64> baseline_medians;

  // Results are held until the run ends so that each one can carry the worst "others" CPU of
  // the whole run, which is not known until the closing sample.
  Vector<Result> ran_results;
  Vector<f64> ran_relatives;

  const u32 sweep_count = sweep_target != nullptr ? sweep_values.size() : 1;
  for (u32 si = 0; si < sweep_count; ++si) {
    std::string tunable_text;
    if (sweep_target != nullptr) {
      std::string error;
      if (!sweep_target->set_from_text(sweep_values[si], &error)) {
        std::fprintf(stderr, "bench: %s\n", error.c_str());
        (void)sweep_target->set_from_text(sweep_original);
        if (json != nullptr) std::fclose(json);
        return 2;
      }
      tunable_text.assign(sweep_target->name());
      tunable_text.push_back('=');
      sweep_target->append_value(tunable_text);
    }
    for (const Registration* reg : regs) {
      const std::span<const i64> all_args = reg->args();
      const usize variants = all_args.empty() ? 1 : all_args.size();
      for (usize ai = 0; ai < variants; ++ai) {
        const std::span<const i64> args =
            all_args.empty() ? std::span<const i64>{} : all_args.subspan(ai, 1);
        if (per_variant) {
          char variant[256];
          variant_name(*reg, args, variant, sizeof(variant));
          if (!glob_match(options.filter, variant)) continue;
        }
        Result result = Runner::measure_one(*reg, args, options);
        result.tunable = tunable_text;

        f64 relative = 0;
        if (sweep_target != nullptr) {
          if (si == 0) {
            baseline_names.push_back(result.name);
            baseline_medians.push_back(result.median_ns);
            relative = 1.0;
          } else {
            for (u32 i = 0; i < baseline_names.size(); ++i) {
              if (baseline_names[i] == result.name && result.median_ns > 0) {
                relative = baseline_medians[i] / result.median_ns;
              }
            }
          }
        }

        if (!options.quiet) {
          char median[32];
          char minimum[32];
          char rate[32];
          char rel[32] = "";
          format_duration(result.median_ns, median, sizeof(median));
          format_duration(result.min_ns, minimum, sizeof(minimum));
          format_rate(result.items_per_second(), rate, sizeof(rate));
          const f64 spread = result.median_ns > 0 ? 100.0 * result.stddev_ns / result.median_ns : 0;
          if (sweep_target != nullptr) std::snprintf(rel, sizeof(rel), "  x%.2f", relative);
          std::string label = result.name;
          if (!result.tunable.empty()) {
            label.append(" [");
            label.append(result.tunable);
            label.push_back(']');
          }
          char line[512];
          std::snprintf(line, sizeof(line), "%-56s %12s %12s %7.1f%% %12s%s\n", label.c_str(),
                        median, minimum, spread, rate, rel);
          write_line(stdout, line);
        }

        ran_results.push_back(std::move(result));
        ran_relatives.push_back(relative);
      }
    }
  }

  if (sweep_target != nullptr) (void)sweep_target->set_from_text(sweep_original);

  // The closing sample, and the worst of the two: a run that started quiet and ended under a
  // diffusion job is not a quiet run, and the report says so once for the whole table.
  MachineState finish;
  MachineState worst;
  if (sampled) {
    finish = sampler.sample(k_sample_window_ms);
    worst = worst_of(start, finish);
  }
  for (Result& result : ran_results) {
    result.machine_state_known = sampled;
    result.worst_others_cpu_pct = sampled && worst.cpu_valid ? worst.cpu_others_pct : 0.0;
  }

  if (json != nullptr) {
    JsonValue header = JsonValue::object();
    header.set("header", true);
    header.set("cpu", platform::cpu_features().brand);
    header.set("logical_cpus", static_cast<u32>(platform::topology().cpu_count()));
    header.set("cache_domains", static_cast<u32>(platform::topology().cache_domains.size()));
    header.set("build", ENGINE_DEBUG ? "debug" : "release");
    header.set("unix_ms", time::wall_unix_ms());
    header.set("smoke", options.smoke);
    if (sampled) {
      JsonValue machine = JsonValue::object();
      machine.set("start", machine_state_json(start));
      machine.set("end", machine_state_json(finish));
      header.set("machine_state", std::move(machine));
    } else {
      header.set("machine_state", JsonValue::null());
    }
    std::string line = write_json(header, JsonWriteOptions{.pretty = false});
    line.push_back('\n');
    std::fwrite(line.data(), 1, line.size(), json);

    for (u32 i = 0; i < ran_results.size(); ++i) {
      const Result& result = ran_results[i];
      JsonValue o = JsonValue::object();
      o.set("bench", result.name);
      if (!result.tunable.empty()) o.set("tunable", result.tunable);
      o.set("iterations", result.iterations);
      o.set("repeats", result.repeats);
      o.set("median_ns", result.median_ns);
      o.set("min_ns", result.min_ns);
      o.set("max_ns", result.max_ns);
      o.set("mean_ns", result.mean_ns);
      o.set("stddev_ns", result.stddev_ns);
      o.set("items", result.items_per_iteration);
      o.set("bytes", result.bytes_per_iteration);
      o.set("items_per_s", result.items_per_second());
      o.set("worst_others_cpu_pct", result.machine_state_known && worst.cpu_valid
                                        ? JsonValue(result.worst_others_cpu_pct)
                                        : JsonValue::null());
      if (sweep_target != nullptr) o.set("relative", ran_relatives[i]);
      line = write_json(o, JsonWriteOptions{.pretty = false});
      line.push_back('\n');
      std::fwrite(line.data(), 1, line.size(), json);
    }
    std::fclose(json);
  }

  // One line, under the table it qualifies. With --quiet there is no table to put it under, so
  // it goes to stderr rather than being lost.
  if (sampled) (void)warn_if_busy(worst, options.quiet_thresholds, options.quiet ? stderr : stdout);

  if (results != nullptr) {
    for (Result& result : ran_results)
      results->push_back(std::move(result));
  }
  if (!options.quiet && ran_results.empty()) {
    write_line(stdout, "bench: no benchmark matched the filter\n");
  }
  std::fflush(stdout);
  return 0;
}

int run_main(int argc, char** argv) {
  Options options;
  bool list = false;
  std::string overrides;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    std::string_view v;
    auto value_of = [&](std::string_view prefix) {
      if (a.substr(0, prefix.size()) == prefix) {
        v = a.substr(prefix.size());
        return true;
      }
      return false;
    };
    if (a == "--list") {
      list = true;
    } else if (a == "--smoke") {
      options.smoke = true;
    } else if (a == "--quiet") {
      options.quiet = true;
    } else if (a == "--no-pin") {
      options.pin = false;
    } else if (a == "--require-quiet") {
      options.require_quiet = true;
    } else if (a == "--help" || a == "-h") {
      write_line(stdout, usage_text());
      return 0;
    } else if (value_of("--filter=")) {
      options.filter = v;
    } else if (value_of("--json=")) {
      options.json_path = v;
    } else if (value_of("--sweep=")) {
      options.sweep = v;
    } else if (value_of("--set=")) {
      if (!overrides.empty()) overrides.push_back(',');
      overrides.append(v);
    } else if (value_of("--repeats=")) {
      if (!parse_u32(v, options.repeats)) {
        std::fprintf(stderr, "bench: --repeats expects a number\n");
        return 2;
      }
    } else if (value_of("--warmup=")) {
      if (!parse_u32(v, options.warmup_repeats)) {
        std::fprintf(stderr, "bench: --warmup expects a number\n");
        return 2;
      }
    } else if (value_of("--min-time=")) {
      f64 ms = 0;
      if (!parse_f64(v, ms) || ms < 0) {
        std::fprintf(stderr, "bench: --min-time expects milliseconds\n");
        return 2;
      }
      options.min_time_ns = static_cast<i64>(ms * 1e6);
    } else if (value_of("--wait-quiet=")) {
      f64 seconds = 0;
      if (!parse_f64(v, seconds) || seconds < 0) {
        std::fprintf(stderr, "bench: --wait-quiet expects seconds\n");
        return 2;
      }
      options.wait_quiet_s = static_cast<i64>(seconds);
    } else if (value_of("--quiet-cpu=")) {
      f64 pct = 0;
      if (!parse_f64(v, pct) || pct < 0 || pct > 100) {
        std::fprintf(stderr, "bench: --quiet-cpu expects a percentage in 0..100\n");
        return 2;
      }
      options.quiet_thresholds.others_cpu_pct = pct;
    } else if (value_of("--quiet-gpu=")) {
      f64 pct = 0;
      if (!parse_f64(v, pct) || pct < 0 || pct > 100) {
        std::fprintf(stderr, "bench: --quiet-gpu expects a percentage in 0..100\n");
        return 2;
      }
      options.quiet_thresholds.gpu_util_pct = pct;
    } else if (!a.empty() && a[0] != '-') {
      options.filter = a;
    } else {
      std::fprintf(stderr, "bench: unknown option '%.*s'\n%s", static_cast<int>(a.size()), a.data(),
                   usage_text());
      return 2;
    }
  }
  options.overrides = overrides;

  if (list) {
    const bool per_variant = options.filter.find('/') != std::string::npos;
    for (const Registration* reg : matching(registration_filter(options.filter))) {
      const std::span<const i64> all_args = reg->args();
      const usize variants = all_args.empty() ? 1 : all_args.size();
      for (usize ai = 0; ai < variants; ++ai) {
        const std::span<const i64> args =
            all_args.empty() ? std::span<const i64>{} : all_args.subspan(ai, 1);
        char variant[256];
        variant_name(*reg, args, variant, sizeof(variant));
        if (per_variant && !glob_match(options.filter, variant)) continue;
        std::printf("%s\n", variant);
      }
    }
    return 0;
  }
  return run(options);
}

}  // namespace engine::bench

// The measurement-hygiene half of the harness (docs/subsystems/bench.md, "Measuring on a shared
// machine"). The system sampler is checked for sane ranges — nothing else is assertable about a
// machine a test does not control — and every decision the sampler drives is checked against a
// fake, so the quiet and not-quiet paths are exercised on any machine, loaded or idle.

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <foundation/bench/bench.h>
#include <foundation/bench/machine_state.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

// Returns a scripted sequence of states, repeating the last one, and counts the calls.
class FakeSampler final : public bench::MachineSampler {
 public:
  explicit FakeSampler(std::vector<bench::MachineState> states) : states_(std::move(states)) {}

  bench::MachineState sample(i64 window_ms) override {
    last_window_ms = window_ms;
    ++calls;
    if (states_.empty()) return bench::MachineState{};
    const usize i = calls - 1 < states_.size() ? calls - 1 : states_.size() - 1;
    return states_[i];
  }

  usize calls = 0;
  i64 last_window_ms = -1;

 private:
  std::vector<bench::MachineState> states_;
};

bench::MachineState busy_cpu(f64 others_pct) {
  bench::MachineState s;
  s.cpu_valid = true;
  s.cpu_total_pct = others_pct + 5.0;
  s.cpu_own_pct = 5.0;
  s.cpu_others_pct = others_pct;
  return s;
}

bench::MachineState quiet_state() {
  bench::MachineState s = busy_cpu(1.5);
  s.gpu_valid = true;
  s.gpu_util_pct = 3.0;
  s.gpu_memory_used_mib = 1024;
  s.gpu_memory_total_mib = 32607;
  s.session_locked = bench::Tristate::No;
  return s;
}

bench::Options quick(std::string_view filter, bench::MachineSampler* sampler) {
  bench::Options o;
  o.filter = filter;
  o.pin = false;
  o.quiet = true;
  o.repeats = 2;
  o.warmup_repeats = 0;
  o.min_time_ns = 200'000;
  o.sampler = sampler;
  return o;
}

}  // namespace

TEST_CASE("machine state: the system sampler returns values in range") {
  const bench::MachineState s = bench::sample_machine_state(bench::k_sample_window_ms);
  // The CPU counters exist on both platforms the engine builds for; a failure here is a broken
  // reader, not a busy machine.
  REQUIRE(s.cpu_valid);
  CHECK(s.cpu_total_pct >= 0.0);
  CHECK(s.cpu_total_pct <= 100.0);
  CHECK(s.cpu_own_pct >= 0.0);
  CHECK(s.cpu_own_pct <= s.cpu_total_pct);
  CHECK(s.cpu_others_pct >= 0.0);
  CHECK(s.cpu_others_pct == doctest::Approx(s.cpu_total_pct - s.cpu_own_pct));
  // This process just spent a quarter of a second asleep, so its own share is small; the
  // machine's own is whatever it is.
  CHECK(s.cpu_own_pct < 50.0);

  if (s.gpu_valid) {  // no nvidia-smi on PATH is a legitimate answer, on CI and on AMD
    CHECK(s.gpu_util_pct >= 0.0);
    CHECK(s.gpu_util_pct <= 100.0);
    CHECK(s.gpu_memory_total_mib > 0);
    CHECK(s.gpu_memory_used_mib <= s.gpu_memory_total_mib);
  }

  // A zero window reads no CPU counters at all, which is how a caller asks for the cheap fields.
  const bench::MachineState instant = bench::sample_machine_state(0);
  CHECK_FALSE(instant.cpu_valid);
}

TEST_CASE("machine state: quiet is a question about other processes, not about this one") {
  const bench::QuietThresholds t;
  CHECK(bench::is_quiet(quiet_state(), t));

  bench::MachineState own_heavy = busy_cpu(2.0);
  own_heavy.cpu_own_pct = 90.0;
  own_heavy.cpu_total_pct = 92.0;
  CHECK(bench::is_quiet(own_heavy, t));  // the benchmark itself is allowed to use the machine

  CHECK_FALSE(bench::is_quiet(busy_cpu(40.0), t));

  bench::MachineState gpu_busy = quiet_state();
  gpu_busy.gpu_util_pct = 97.0;
  CHECK_FALSE(bench::is_quiet(gpu_busy, t));

  // A threshold the caller widened past the load makes the same state quiet again.
  bench::QuietThresholds loose;
  loose.others_cpu_pct = 50.0;
  loose.gpu_util_pct = 99.0;
  CHECK(bench::is_quiet(busy_cpu(40.0), loose));
  CHECK(bench::is_quiet(gpu_busy, loose));

  // Nothing measured cannot make a machine noisy, or the flags would be useless where there is
  // no GPU to read.
  CHECK(bench::is_quiet(bench::MachineState{}, t));
}

TEST_CASE("machine state: worst_of takes the worse of each field") {
  const bench::MachineState a = busy_cpu(4.0);
  bench::MachineState b = quiet_state();
  b.gpu_util_pct = 80.0;
  const bench::MachineState w = bench::worst_of(a, b);
  CHECK(w.cpu_valid);
  CHECK(w.cpu_others_pct == doctest::Approx(4.0));
  CHECK(w.gpu_valid);
  CHECK(w.gpu_util_pct == doctest::Approx(80.0));
  // A sample that read nothing contributes nothing.
  const bench::MachineState with_nothing = bench::worst_of(bench::MachineState{}, b);
  CHECK(with_nothing.gpu_util_pct == doctest::Approx(80.0));
  CHECK(with_nothing.cpu_others_pct == doctest::Approx(b.cpu_others_pct));
}

TEST_CASE("machine state: the JSON object is null field by field, never absent") {
  JsonValue o = bench::machine_state_json(bench::MachineState{});
  REQUIRE(o.is_object());
  CHECK(o["cpu_total_pct"].is_null());
  CHECK(o["cpu_others_pct"].is_null());
  CHECK(o["gpu_util_pct"].is_null());
  CHECK(o["gpu_memory_used_mib"].is_null());
  CHECK(o["session_locked"].is_null());

  o = bench::machine_state_json(quiet_state());
  f64 others = 0;
  CHECK(o["cpu_others_pct"].get_f64(others));
  CHECK(others == doctest::Approx(1.5));
  u64 used = 0;
  CHECK(o["gpu_memory_used_mib"].get_u64(used));
  CHECK(used == 1024);
  CHECK(o["session_locked"] == JsonValue(false));
}

TEST_CASE("machine state: describe names an unknown field rather than leaving it out") {
  const std::string unknown = bench::describe(bench::MachineState{});
  CHECK(unknown.find("cpu unknown") != std::string::npos);
  CHECK(unknown.find("gpu unknown") != std::string::npos);
  CHECK(unknown.find("session unknown") != std::string::npos);
  const std::string known = bench::describe(quiet_state());
  CHECK(known.find("others 1.5%") != std::string::npos);
  CHECK(known.find("session unlocked") != std::string::npos);
}

TEST_CASE("bench: --require-quiet refuses a busy machine and measures a quiet one") {
  FakeSampler busy({busy_cpu(55.0)});
  bench::Options o = quick("test.bench.noop", &busy);
  o.require_quiet = true;
  Vector<bench::Result> results;
  CHECK(bench::run(o, &results) == bench::k_exit_not_quiet);
  CHECK(results.empty());
  CHECK(busy.calls == 1);  // it refused before running anything, and never sampled again
  CHECK(busy.last_window_ms == bench::k_sample_window_ms);

  FakeSampler calm({quiet_state()});
  bench::Options ok = quick("test.bench.noop", &calm);
  ok.require_quiet = true;
  CHECK(bench::run(ok, &results) == 0);
  CHECK(results.size() == 1);
  CHECK(calm.calls == 2);  // before and after the run
  CHECK(results[0].machine_state_known);
  CHECK(results[0].worst_others_cpu_pct == doctest::Approx(1.5));
}

TEST_CASE("bench: --smoke neither samples nor refuses") {
  FakeSampler busy({busy_cpu(95.0)});
  bench::Options o = quick("test.bench.noop", &busy);
  o.smoke = true;
  o.require_quiet = true;
  o.wait_quiet_s = 3600;
  Vector<bench::Result> results;
  CHECK(bench::run(o, &results) == 0);
  REQUIRE(results.size() == 1);
  CHECK(busy.calls == 0);
  CHECK_FALSE(results[0].machine_state_known);
}

TEST_CASE("bench: --wait-quiet polls until the machine is quiet") {
  // Busy, then quiet on the second look: the run proceeds after one poll. One second, because
  // a poll never sleeps past the deadline.
  FakeSampler settles({busy_cpu(70.0), quiet_state()});
  bench::Options o = quick("test.bench.noop", &settles);
  o.wait_quiet_s = 1;
  Vector<bench::Result> results;
  CHECK(bench::run(o, &results) == 0);
  CHECK(settles.calls == 3);  // the first look, one poll, and the closing sample
  REQUIRE(results.size() == 1);
  CHECK(results[0].worst_others_cpu_pct == doctest::Approx(1.5));

  // Still busy when the deadline passes: it proceeds anyway, and the result carries the load.
  FakeSampler stubborn({busy_cpu(70.0)});
  bench::Options give_up = quick("test.bench.noop", &stubborn);
  give_up.wait_quiet_s = 1;  // shorter than one poll interval: one look, then run
  results.clear();
  CHECK(bench::run(give_up, &results) == 0);
  REQUIRE(results.size() == 1);
  CHECK(results[0].machine_state_known);
  CHECK(results[0].worst_others_cpu_pct == doctest::Approx(70.0));
}

TEST_CASE("bench: a widened threshold makes a loaded machine quiet enough") {
  FakeSampler busy({busy_cpu(35.0)});
  bench::Options o = quick("test.bench.noop", &busy);
  o.require_quiet = true;
  o.quiet_thresholds.others_cpu_pct = 40.0;
  Vector<bench::Result> results;
  CHECK(bench::run(o, &results) == 0);
  CHECK(results.size() == 1);
}

TEST_CASE("bench: the JSON header carries both samples and every result its worst load") {
  test::TempDir dir("bench_machine_state");
  const std::string path = dir.file("machine_state.jsonl");

  FakeSampler sampler({busy_cpu(12.0), busy_cpu(30.0)});
  bench::Options o = quick("test.bench.args", &sampler);
  o.json_path = path;
  REQUIRE(bench::run(o, nullptr) == 0);

  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);)
    lines.push_back(line);
  in.close();
  REQUIRE(lines.size() == 3);  // header, args/8, args/64

  JsonValue header;
  REQUIRE(parse_json(lines[0], header).ok);
  CHECK(header["header"] == JsonValue(true));
  JsonValue& machine = header["machine_state"];
  REQUIRE(machine.is_object());
  f64 started = 0;
  REQUIRE(machine["start"]["cpu_others_pct"].get_f64(started));
  CHECK(started == doctest::Approx(12.0));
  f64 ended = 0;
  REQUIRE(machine["end"]["cpu_others_pct"].get_f64(ended));
  CHECK(ended == doctest::Approx(30.0));
  CHECK(machine["start"]["gpu_util_pct"].is_null());

  for (usize i = 1; i < lines.size(); ++i) {
    JsonValue result;
    REQUIRE(parse_json(lines[i], result).ok);
    f64 worst = 0;
    REQUIRE(result["worst_others_cpu_pct"].get_f64(worst));
    CHECK(worst == doctest::Approx(30.0));  // the worst of the run, on every line
  }

  // A smoke run says so rather than reporting a state it did not take.
  const std::string smoke_path = dir.file("smoke.jsonl");
  bench::Options smoke = quick("test.bench.noop", &sampler);
  smoke.smoke = true;
  smoke.json_path = smoke_path;
  REQUIRE(bench::run(smoke, nullptr) == 0);
  std::ifstream smoke_in(smoke_path);
  std::string first_line;
  REQUIRE(std::getline(smoke_in, first_line));
  smoke_in.close();
  JsonValue smoke_header;
  REQUIRE(parse_json(first_line, smoke_header).ok);
  CHECK(smoke_header["machine_state"].is_null());
}

TEST_CASE("bench: run_main parses the hygiene flags") {
  const char* bad_wait[] = {"bench", "--wait-quiet=soon"};
  CHECK(bench::run_main(2, const_cast<char**>(bad_wait)) == 2);
  const char* bad_cpu[] = {"bench", "--quiet-cpu=200"};
  CHECK(bench::run_main(2, const_cast<char**>(bad_cpu)) == 2);
  const char* bad_gpu[] = {"bench", "--quiet-gpu=-1"};
  CHECK(bench::run_main(2, const_cast<char**>(bad_gpu)) == 2);
  // --quiet still means "no table", and the three new flags do not shadow it.
  const char* smoke[] = {"bench",           "test.bench.noop", "--smoke",
                         "--quiet",         "--no-pin",        "--require-quiet",
                         "--wait-quiet=60", "--quiet-cpu=5",   "--quiet-gpu=1"};
  CHECK(bench::run_main(9, const_cast<char**>(smoke)) == 0);
}

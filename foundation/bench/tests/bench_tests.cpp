#include <core/json/json.h>
#include <foundation/bench/bench.h>
#include <foundation/tunables/tunables.h>

#include <doctest/doctest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace engine;

namespace {

std::atomic<int> g_noop_calls{0};

ENGINE_BENCH(bench_noop, "test.bench.noop") {
  ++g_noop_calls;
  u64 x = 0;
  while (state.keep_running()) {
    ++x;
    bench::keep(x);
  }
  state.set_items(1);
}

ENGINE_BENCH_ARGS(bench_args, "test.bench.args", 8, 64) {
  const u64 n = static_cast<u64>(state.arg());
  while (state.keep_running()) {
    u64 sum = 0;
    for (u64 i = 0; i < n; ++i)
      sum += i;
    bench::keep(sum);
  }
  state.set_items(n);
  state.set_bytes(n * 8);
}

tunables::Int t_sweep{"test.bench.sweep", 1, 0, 100, "Sweep target"};
std::vector<i64> g_seen;

ENGINE_BENCH(bench_sweep_target, "test.bench.sweep_target") {
  g_seen.push_back(t_sweep.get());
  while (state.keep_running()) {
  }
}

ENGINE_BENCH(bench_paused, "test.bench.paused") {
  while (state.keep_running()) {
    state.pause_timing();
    // Untimed setup would go here.
    state.resume_timing();
  }
}

bench::Options quick(std::string_view filter) {
  bench::Options o;
  o.filter = filter;
  o.pin = false;
  o.quiet = true;
  o.repeats = 3;
  o.warmup_repeats = 1;
  o.min_time_ns = 200'000;
  return o;
}

}  // namespace

TEST_CASE("bench: glob_match") {
  CHECK(bench::glob_match("", "anything"));
  CHECK(bench::glob_match("*", "anything"));
  CHECK(bench::glob_match("bench", "test.bench.noop"));  // substring without '*'
  CHECK_FALSE(bench::glob_match("noop", "test.bench.args"));
  CHECK(bench::glob_match("test.bench.*", "test.bench.noop"));
  CHECK_FALSE(bench::glob_match("test.bench.*", "test.other.noop"));
  CHECK(bench::glob_match("test.*.args", "test.bench.args"));
  CHECK(bench::glob_match("*.args", "test.bench.args"));
  CHECK(bench::glob_match("test.bench.n*p", "test.bench.noop"));
  CHECK_FALSE(bench::glob_match("test.bench.n*x", "test.bench.noop"));
  CHECK(bench::glob_match("a*b*c", "aXXbYYc"));
  CHECK_FALSE(bench::glob_match("a*b*c", "aXXbYY"));
}

TEST_CASE("bench: statistics") {
  const f64 odd[] = {5, 1, 3};
  bench::Stats s = bench::compute_stats(odd);
  CHECK(s.median == 3);
  CHECK(s.min == 1);
  CHECK(s.max == 5);
  CHECK(s.mean == 3);
  CHECK(s.stddev == doctest::Approx(1.632993));
  const f64 even[] = {4, 1, 3, 2};
  s = bench::compute_stats(even);
  CHECK(s.median == 2.5);
  CHECK(s.mean == 2.5);
  CHECK(bench::compute_stats({}).median == 0);
}

TEST_CASE("bench: registrations are found by name and expanded per argument") {
  CHECK(bench::registration_count() >= 4);
  bool found = false;
  for (const bench::Registration* r = bench::first_registration(); r != nullptr; r = r->next()) {
    if (std::string_view(r->name()) == "test.bench.args") {
      found = true;
      CHECK(r->args().size() == 2);
      CHECK(r->args()[1] == 64);
    }
  }
  CHECK(found);
}

TEST_CASE("bench: smoke run touches every matching benchmark once") {
  bench::Options o = quick("test.bench.*");
  o.smoke = true;
  Vector<bench::Result> results;
  const int before = g_noop_calls.load();
  CHECK(bench::run(o, &results) == 0);
  REQUIRE(results.size() == 5);  // noop, args/8, args/64, paused, sweep_target (name order)
  CHECK(results[0].name == "test.bench.args/8");
  CHECK(results[1].name == "test.bench.args/64");
  CHECK(results[2].name == "test.bench.noop");
  CHECK(results[3].name == "test.bench.paused");
  CHECK(results[4].name == "test.bench.sweep_target");
  for (const auto& r : results) {
    CHECK(r.iterations == 1);
    CHECK(r.repeats == 1);
    CHECK(r.tunable.empty());
  }
  CHECK(results[1].items_per_iteration == 64);
  CHECK(results[1].bytes_per_iteration == 512);
  CHECK(g_noop_calls.load() == before + 1);
}

TEST_CASE("bench: measured run calibrates, warms up, and repeats") {
  Vector<bench::Result> results;
  const int before = g_noop_calls.load();
  CHECK(bench::run(quick("test.bench.noop"), &results) == 0);
  REQUIRE(results.size() == 1);
  const auto& r = results[0];
  CHECK(r.iterations >= 1);
  CHECK(r.repeats == 3);
  CHECK(r.median_ns >= 0);
  CHECK(r.min_ns <= r.median_ns);
  CHECK(r.median_ns <= r.max_ns);
  CHECK(r.items_per_iteration == 1);
  // At least one calibration call, one warmup, and three repeats.
  CHECK(g_noop_calls.load() >= before + 5);
  CHECK(r.items_per_second() > 0);
}

TEST_CASE("bench: sweeping a tunable runs each value and restores the original") {
  g_seen.clear();
  bench::Options o = quick("test.bench.sweep_target");
  o.smoke = true;
  o.sweep = "test.bench.sweep=3;7;7";
  Vector<bench::Result> results;
  REQUIRE(t_sweep.get() == 1);
  CHECK(bench::run(o, &results) == 0);
  REQUIRE(results.size() == 3);
  CHECK(results[0].tunable == "test.bench.sweep=3");
  CHECK(results[1].tunable == "test.bench.sweep=7");
  CHECK(g_seen == std::vector<i64>{3, 7, 7});
  CHECK(t_sweep.get() == 1);

  o.sweep = "test.bench.missing=1";
  CHECK(bench::run(o, nullptr) == 2);
  o.sweep = "test.bench.sweep=500";  // out of range
  CHECK(bench::run(o, nullptr) == 2);
  CHECK(t_sweep.get() == 1);
  o.sweep = {};
  o.overrides = "test.bench.sweep=9";
  CHECK(bench::run(o, nullptr) == 0);
  CHECK(t_sweep.get() == 9);
  (void)t_sweep.set(1);
}

TEST_CASE("bench: JSON lines output has a header and one object per result") {
  const auto path = std::filesystem::temp_directory_path() / "engine_bench_tests.jsonl";
  std::filesystem::remove(path);
  const std::string path_text = path.string();
  bench::Options o = quick("test.bench.args");
  o.smoke = true;
  o.json_path = path_text;
  CHECK(bench::run(o, nullptr) == 0);
  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);)
    lines.push_back(line);
  in.close();
  std::filesystem::remove(path);
  REQUIRE(lines.size() == 3);
  JsonValue header;
  REQUIRE(parse_json(lines[0], header).ok);
  CHECK(header["header"] == JsonValue(true));
  CHECK(header["smoke"] == JsonValue(true));
  u64 cpus = 0;
  CHECK(header["logical_cpus"].get_u64(cpus));
  CHECK(cpus >= 1);
  JsonValue first;
  REQUIRE(parse_json(lines[1], first).ok);
  CHECK(first["bench"] == JsonValue("test.bench.args/8"));
  u64 items = 0;
  CHECK(first["items"].get_u64(items));
  CHECK(items == 8);
  u64 iterations = 0;
  CHECK(first["iterations"].get_u64(iterations));
  CHECK(iterations == 1);
}

TEST_CASE("bench: run_main parses flags") {
  const char* list_args[] = {"bench", "--list", "--filter=test.bench.args"};
  CHECK(bench::run_main(3, const_cast<char**>(list_args)) == 0);
  const char* bad[] = {"bench", "--bogus"};
  CHECK(bench::run_main(2, const_cast<char**>(bad)) == 2);
  const char* bad_repeats[] = {"bench", "--repeats=many"};
  CHECK(bench::run_main(2, const_cast<char**>(bad_repeats)) == 2);
  const char* smoke[] = {"bench", "test.bench.noop", "--smoke", "--quiet", "--no-pin"};
  CHECK(bench::run_main(5, const_cast<char**>(smoke)) == 0);
}

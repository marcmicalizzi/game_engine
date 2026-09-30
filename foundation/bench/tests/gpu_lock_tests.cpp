// The machine-wide GPU lock from the harness's side: what a reading of it makes of the machine
// state (quiet or not, the description, the JSON, the WARNING) and what `--gpu-lock` does to a
// run. The lock itself — reading, the lease, breaking, releasing — is foundation/gpu_lock's and
// tested there (docs/subsystems/gpu_lock.md). Every case writes its lock into its own scratch
// directory and points the reader or the run at it, so the machine's real lock — and whoever
// holds it while the suite runs — never reaches a result.

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/time/time.h>
#include <foundation/bench/bench.h>
#include <foundation/bench/machine_state.h>
#include <foundation/gpu_lock/gpu_lock.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace engine;

namespace {

i64 now_s() { return time::wall_unix_ms() / 1000; }

// A pid no test process has: the "somebody else" of every case.
constexpr u64 k_other_pid = 999'999'937;

std::string lock_json(std::string_view owner, u64 pid, i64 expires_unix_s, std::string_view host,
                      std::string_view purpose = "a test holder") {
  JsonValue o = JsonValue::object();
  o.set("owner", owner);
  o.set("purpose", purpose);
  o.set("pid", pid);
  o.set("started", gpu_lock::format_iso8601_utc(expires_unix_s - 600));
  o.set("expires", gpu_lock::format_iso8601_utc(expires_unix_s));
  o.set("host", host);
  return write_json(o, JsonWriteOptions{.pretty = false});
}

void write_file(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

bool exists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(std::filesystem::path(path), ec);
}

bench::MachineState calm() {
  bench::MachineState s;
  s.cpu_valid = true;
  s.cpu_total_pct = 3.0;
  s.cpu_own_pct = 1.0;
  s.cpu_others_pct = 2.0;
  s.gpu_valid = true;
  s.gpu_util_pct = 2.0;
  s.gpu_memory_used_mib = 900;
  s.gpu_memory_total_mib = 32607;
  s.session_locked = bench::Tristate::No;
  return s;
}

class CalmSampler final : public bench::MachineSampler {
 public:
  bench::MachineState sample(i64) override {
    ++calls;
    return calm();
  }
  usize calls = 0;
};

// Sees the lock from inside a measured benchmark, which is the only place that can say whether
// the run held it *while* it measured.
std::string g_probe_path;
bool g_probe_saw_mine = false;

ENGINE_BENCH(gpulock_probe, "test.gpulock.probe") {
  if (!g_probe_path.empty()) {
    const gpu_lock::State s = gpu_lock::read(g_probe_path, gpu_lock::current_identity(), now_s());
    g_probe_saw_mine = g_probe_saw_mine || (s.present && s.mine);
  }
  u64 x = 0;
  while (state.keep_running()) {
    ++x;
    bench::keep(x);
  }
}

bench::Options probe_run(const std::string& lock_path, bench::MachineSampler* sampler) {
  bench::Options o;
  o.filter = "test.gpulock.probe";
  o.pin = false;
  o.quiet = true;
  o.repeats = 2;
  o.warmup_repeats = 0;
  o.min_time_ns = 100'000;
  o.sampler = sampler;
  o.gpu_lock_path = lock_path;
  o.gpu_lock_timeout_s = 0;  // one look: a case never waits on a lock it wrote itself
  return o;
}

}  // namespace

TEST_CASE("gpu lock: absent is quiet") {
  test::TempDir dir("gpu_lock_absent");
  bench::MachineState s = calm();
  s.gpu_lock = gpu_lock::read(dir.file("gpu.lock"), gpu_lock::current_identity(), now_s());
  CHECK_FALSE(s.gpu_lock.held_by_other());
  CHECK(bench::is_quiet(s, bench::QuietThresholds{}));
  CHECK(bench::describe(s).find("gpu lock free") != std::string::npos);
  CHECK(bench::machine_state_json(s)["gpu_lock"].is_null());
}

TEST_CASE("gpu lock: held by somebody else is not quiet, whatever the GPU sample says") {
  test::TempDir dir("gpu_lock_other");
  const std::string path = dir.file("gpu.lock");
  const gpu_lock::Identity self = gpu_lock::current_identity();

  // Another tool on the machine: the Blender agent, rendering, with an idle-looking GPU sample.
  write_file(path, lock_json("astra-blender", 4321, now_s() + 1800, self.host, "bake: desert"));
  bench::MachineState s = calm();
  s.gpu_lock = gpu_lock::read(path, self, now_s());
  CHECK(s.gpu_lock.held_by_other());
  CHECK_FALSE(bench::is_quiet(s, bench::QuietThresholds{}));
  CHECK(bench::describe(s).find("gpu lock held by 'astra-blender': bake: desert") !=
        std::string::npos);

  JsonValue json = bench::machine_state_json(s);
  REQUIRE(json["gpu_lock"].is_object());
  CHECK(json["gpu_lock"]["owner"] == JsonValue("astra-blender"));
  CHECK(json["gpu_lock"]["mine"] == JsonValue(false));
  CHECK(json["gpu_lock"]["expired"] == JsonValue(false));

  // The WARNING names the lock, in the one sentence write-ups already explain.
  const std::string warn_path = dir.file("warning.txt");
  std::FILE* f = nullptr;
#if ENGINE_COMPILER_MSVC
  (void)fopen_s(&f, warn_path.c_str(), "wb");
#else
  f = std::fopen(warn_path.c_str(), "wb");
#endif
  REQUIRE(f != nullptr);
  CHECK(bench::warn_if_busy(s, bench::QuietThresholds{}, f));
  std::fclose(f);
  CHECK(read_file(warn_path).find("the GPU lock was held by 'astra-blender'") != std::string::npos);

  // Another *engine* agent: the same owner label, a different process.
  write_file(path, lock_json(self.owner, k_other_pid, now_s() + 1800, self.host));
  s.gpu_lock = gpu_lock::read(path, self, now_s());
  CHECK_FALSE(bench::is_quiet(s, bench::QuietThresholds{}));

  // The worst of a run's two samples keeps the one somebody else held.
  bench::MachineState free_sample = calm();
  const bench::MachineState worst = bench::worst_of(free_sample, s);
  CHECK(worst.gpu_lock.held_by_other());
}

TEST_CASE("gpu lock: held by this process, or for it, is quiet") {
  test::TempDir dir("gpu_lock_self");
  const std::string path = dir.file("gpu.lock");
  gpu_lock::Identity self = gpu_lock::current_identity();

  // The harness took it itself (--gpu-lock).
  write_file(path, lock_json(self.owner, self.pid, now_s() + 1800, self.host));
  bench::MachineState s = calm();
  s.gpu_lock = gpu_lock::read(path, self, now_s());
  CHECK(bench::is_quiet(s, bench::QuietThresholds{}));
  CHECK(bench::describe(s).find("gpu lock ours") != std::string::npos);
  CHECK(bench::machine_state_json(s)["gpu_lock"]["mine"] == JsonValue(true));

  // A wrapper, or a process with a GPU device open, took it for this process.
  self.holder_pid = 4242;
  write_file(path, lock_json(self.owner, 4242, now_s() + 1800, self.host));
  s.gpu_lock = gpu_lock::read(path, self, now_s());
  CHECK(bench::is_quiet(s, bench::QuietThresholds{}));
}

TEST_CASE("gpu lock: expired is quiet — except a person's") {
  test::TempDir dir("gpu_lock_expired");
  const std::string path = dir.file("gpu.lock");
  const gpu_lock::Identity self = gpu_lock::current_identity();

  write_file(path, lock_json("astra-blender", 4321, now_s() - 60, self.host));
  bench::MachineState s = calm();
  s.gpu_lock = gpu_lock::read(path, self, now_s());
  CHECK(bench::is_quiet(s, bench::QuietThresholds{}));
  CHECK(bench::describe(s).find("gpu lock expired ('astra-blender')") != std::string::npos);
  CHECK(bench::machine_state_json(s)["gpu_lock"]["expired"] == JsonValue(true));

  write_file(path, lock_json("marc", 4321, now_s() - 60, self.host));
  s.gpu_lock = gpu_lock::read(path, self, now_s());
  CHECK_FALSE(bench::is_quiet(s, bench::QuietThresholds{}));
}

TEST_CASE("bench: --gpu-lock holds the lock for the whole run and releases it") {
  test::TempDir dir("gpu_lock_run");
  const std::string path = dir.file("gpu.lock");
  const std::string json_path = dir.file("run.jsonl");
  CalmSampler sampler;
  bench::Options o = probe_run(path, &sampler);
  o.gpu_lock = true;
  o.require_quiet = true;  // the run's own lock must not count against it
  o.json_path = json_path;
  g_probe_path = path;
  g_probe_saw_mine = false;
  Vector<bench::Result> results;
  CHECK(bench::run(o, &results) == 0);
  g_probe_path.clear();
  CHECK(results.size() == 1);
  CHECK(g_probe_saw_mine);    // held while the benchmark measured
  CHECK_FALSE(exists(path));  // and released when the run ended

  std::ifstream in(json_path);
  std::string header_line;
  REQUIRE(std::getline(in, header_line));
  in.close();
  JsonValue header;
  REQUIRE(parse_json(header_line, header).ok);
  JsonValue& start_lock = header["machine_state"]["start"]["gpu_lock"];
  REQUIRE(start_lock.is_object());
  CHECK(start_lock["mine"] == JsonValue(true));
  CHECK(start_lock["purpose"] == JsonValue("bench test.gpulock.probe"));
}

TEST_CASE("bench: somebody else's lock refuses --require-quiet and times out --gpu-lock") {
  test::TempDir dir("gpu_lock_refused");
  const std::string path = dir.file("gpu.lock");
  const gpu_lock::Identity self = gpu_lock::current_identity();
  const std::string theirs = lock_json(self.owner, k_other_pid, now_s() + 1800, self.host);
  write_file(path, theirs);

  CalmSampler sampler;
  Vector<bench::Result> results;
  bench::Options quiet = probe_run(path, &sampler);
  quiet.require_quiet = true;
  CHECK(bench::run(quiet, &results) == bench::k_exit_not_quiet);
  CHECK(results.empty());
  CHECK(sampler.calls == 1);

  bench::Options locked = probe_run(path, &sampler);
  locked.gpu_lock = true;
  CHECK(bench::run(locked, &results) == bench::k_exit_not_quiet);
  CHECK(results.empty());
  CHECK(read_file(path) == theirs);

  // Without either flag the run goes ahead and the result carries the caveat in the header.
  const std::string json_path = dir.file("run.jsonl");
  bench::Options plain = probe_run(path, &sampler);
  plain.json_path = json_path;
  CHECK(bench::run(plain, &results) == 0);
  CHECK(results.size() == 1);
  std::ifstream in(json_path);
  std::string header_line;
  REQUIRE(std::getline(in, header_line));
  in.close();
  JsonValue header;
  REQUIRE(parse_json(header_line, header).ok);
  CHECK(header["machine_state"]["end"]["gpu_lock"]["mine"] == JsonValue(false));
}

TEST_CASE("bench: run_main parses --gpu-lock") {
  const char* bad[] = {"bench", "--gpu-lock=soon"};
  CHECK(bench::run_main(2, const_cast<char**>(bad)) == 2);
  const char* negative[] = {"bench", "--gpu-lock=-5"};
  CHECK(bench::run_main(2, const_cast<char**>(negative)) == 2);
  // A smoke run never takes the lock, so this touches nothing on the machine.
  const char* smoke[] = {"bench",   "test.gpulock.probe", "--smoke",
                         "--quiet", "--no-pin",           "--gpu-lock=0"};
  CHECK(bench::run_main(6, const_cast<char**>(smoke)) == 0);
}

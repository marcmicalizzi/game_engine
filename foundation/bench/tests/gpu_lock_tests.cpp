// The machine-wide GPU lock from the harness's side (foundation/bench/gpu_lock.h;
// docs/subsystems/bench.md, "The GPU lock"). Every case writes its lock into its own scratch
// directory and points the reader, the lease or the run at it, so the machine's real lock — and
// whoever holds it while the suite runs — never reaches a result.

#include <core/json/json.h>
#include <core/json/json_value.h>
#include <core/time/time.h>
#include <foundation/bench/bench.h>
#include <foundation/bench/gpu_lock.h>
#include <foundation/bench/machine_state.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
  o.set("started", bench::format_iso8601_utc(expires_unix_s - 600));
  o.set("expires", bench::format_iso8601_utc(expires_unix_s));
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
    const bench::GpuLockState s =
        bench::read_gpu_lock(g_probe_path, bench::current_gpu_lock_identity(), now_s());
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

TEST_CASE("gpu lock: ISO 8601 times round-trip, in the forms every holder writes") {
  // 2026-09-22T18:40:00Z; midnight of that day is 1790035200.
  const i64 t = 1790035200 + 18 * 3600 + 40 * 60;
  CHECK(bench::format_iso8601_utc(t) == "2026-09-22T18:40:00Z");
  i64 back = 0;
  REQUIRE(bench::parse_iso8601_utc("2026-09-22T18:40:00Z", back));
  CHECK(back == t);
  // What Python's isoformat() writes, and a zone that is not UTC.
  REQUIRE(bench::parse_iso8601_utc("2026-09-22T18:40:00.123456+00:00", back));
  CHECK(back == t);
  REQUIRE(bench::parse_iso8601_utc("2026-09-22T20:40:00+02:00", back));
  CHECK(back == t);
  CHECK(bench::format_iso8601_utc(0) == "1970-01-01T00:00:00Z");
  CHECK_FALSE(bench::parse_iso8601_utc("", back));
  CHECK_FALSE(bench::parse_iso8601_utc("22/09/2026 18:40", back));
  CHECK_FALSE(bench::parse_iso8601_utc("2026-09-22T18:40:00Zjunk", back));
  CHECK_FALSE(bench::parse_iso8601_utc("2026-13-22T18:40:00Z", back));
}

TEST_CASE("gpu lock: absent is free, and quiet") {
  test::TempDir dir("gpu_lock_absent");
  const bench::GpuLockIdentity self = bench::current_gpu_lock_identity();
  bench::MachineState s = calm();
  s.gpu_lock = bench::read_gpu_lock(dir.file("gpu.lock"), self, now_s());
  CHECK_FALSE(s.gpu_lock.present);
  CHECK_FALSE(s.gpu_lock.held_by_other());
  CHECK(bench::is_quiet(s, bench::QuietThresholds{}));
  CHECK(bench::describe(s).find("gpu lock free") != std::string::npos);
  CHECK(bench::machine_state_json(s)["gpu_lock"].is_null());
}

TEST_CASE("gpu lock: held by somebody else is not quiet, whatever the GPU sample says") {
  test::TempDir dir("gpu_lock_other");
  const std::string path = dir.file("gpu.lock");
  const bench::GpuLockIdentity self = bench::current_gpu_lock_identity();

  // Another tool on the machine: the Blender agent, rendering, with an idle-looking GPU sample.
  write_file(path, lock_json("astra-blender", 4321, now_s() + 1800, self.host, "bake: desert"));
  bench::MachineState s = calm();
  s.gpu_lock = bench::read_gpu_lock(path, self, now_s());
  CHECK(s.gpu_lock.present);
  CHECK(s.gpu_lock.readable);
  CHECK_FALSE(s.gpu_lock.mine);
  CHECK_FALSE(s.gpu_lock.expired);
  CHECK(s.gpu_lock.held_by_other());
  CHECK(s.gpu_lock.owner == "astra-blender");
  CHECK(s.gpu_lock.purpose == "bake: desert");
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

  // Another *engine* agent: the same owner label, a different process. An owner match alone
  // would call this ours, and one agent's measurement would run straight through another's render.
  write_file(path, lock_json(self.owner, k_other_pid, now_s() + 1800, self.host));
  s.gpu_lock = bench::read_gpu_lock(path, self, now_s());
  CHECK_FALSE(s.gpu_lock.mine);
  CHECK(s.gpu_lock.held_by_other());
  CHECK_FALSE(bench::is_quiet(s, bench::QuietThresholds{}));

  // The worst of a run's two samples keeps the one somebody else held.
  bench::MachineState free_sample = calm();
  const bench::MachineState worst = bench::worst_of(free_sample, s);
  CHECK(worst.gpu_lock.held_by_other());
}

TEST_CASE("gpu lock: held by this process, or for it, is quiet") {
  test::TempDir dir("gpu_lock_self");
  const std::string path = dir.file("gpu.lock");
  bench::GpuLockIdentity self = bench::current_gpu_lock_identity();
  REQUIRE(self.pid != 0);
  CHECK_FALSE(self.owner.empty());

  // The harness took it itself (--gpu-lock).
  write_file(path, lock_json(self.owner, self.pid, now_s() + 1800, self.host));
  bench::MachineState s = calm();
  s.gpu_lock = bench::read_gpu_lock(path, self, now_s());
  CHECK(s.gpu_lock.mine);
  CHECK_FALSE(s.gpu_lock.held_by_other());
  CHECK(bench::is_quiet(s, bench::QuietThresholds{}));
  CHECK(bench::describe(s).find("gpu lock ours") != std::string::npos);
  CHECK(bench::machine_state_json(s)["gpu_lock"]["mine"] == JsonValue(true));

  // A wrapper took it for this process (tools/gpu-lock.ps1 run sets ENGINE_GPU_LOCK_HOLDER).
  self.holder_pid = 4242;
  write_file(path, lock_json(self.owner, 4242, now_s() + 1800, self.host));
  s.gpu_lock = bench::read_gpu_lock(path, self, now_s());
  CHECK(s.gpu_lock.mine);
  CHECK(bench::is_quiet(s, bench::QuietThresholds{}));

  // The right pid under another owner label, or on another host, is not ours.
  write_file(path, lock_json("astra-blender", self.pid, now_s() + 1800, self.host));
  CHECK_FALSE(bench::read_gpu_lock(path, self, now_s()).mine);
  write_file(path, lock_json(self.owner, self.pid, now_s() + 1800, "some-other-box"));
  if (!self.host.empty()) CHECK_FALSE(bench::read_gpu_lock(path, self, now_s()).mine);
}

TEST_CASE("gpu lock: expired is free — except a person's") {
  test::TempDir dir("gpu_lock_expired");
  const std::string path = dir.file("gpu.lock");
  const bench::GpuLockIdentity self = bench::current_gpu_lock_identity();

  write_file(path, lock_json("astra-blender", 4321, now_s() - 60, self.host));
  bench::MachineState s = calm();
  s.gpu_lock = bench::read_gpu_lock(path, self, now_s());
  CHECK(s.gpu_lock.present);
  CHECK(s.gpu_lock.expired);
  CHECK_FALSE(s.gpu_lock.held_by_other());
  CHECK(bench::is_quiet(s, bench::QuietThresholds{}));
  CHECK(bench::describe(s).find("gpu lock expired ('astra-blender')") != std::string::npos);
  CHECK(bench::machine_state_json(s)["gpu_lock"]["expired"] == JsonValue(true));

  // GPU-LOCK.md: a lock owned by "marc" is never broken by a tool, expired or not, so the person
  // may still be using the GPU.
  write_file(path, lock_json("marc", 4321, now_s() - 60, self.host));
  s.gpu_lock = bench::read_gpu_lock(path, self, now_s());
  CHECK(s.gpu_lock.expired);
  CHECK(s.gpu_lock.held_by_other());
  CHECK_FALSE(bench::is_quiet(s, bench::QuietThresholds{}));
}

TEST_CASE("gpu lock: an unreadable file is being written until it is stale") {
  test::TempDir dir("gpu_lock_unreadable");
  const std::string path = dir.file("gpu.lock");
  const bench::GpuLockIdentity self = bench::current_gpu_lock_identity();
  write_file(path, "{\"owner\":\"astra-");  // a writer caught half way
  bench::GpuLockState lock = bench::read_gpu_lock(path, self, now_s());
  CHECK(lock.present);
  CHECK_FALSE(lock.readable);
  CHECK_FALSE(lock.expired);
  CHECK(lock.held_by_other());

  // Left behind by a crash: older than k_unreadable_stale_s, and anyone may break it.
  std::error_code ec;
  std::filesystem::last_write_time(std::filesystem::path(path),
                                   std::filesystem::file_time_type::clock::now() -
                                       std::chrono::seconds(bench::k_unreadable_stale_s + 60),
                                   ec);
  REQUIRE_FALSE(ec);
  lock = bench::read_gpu_lock(path, self, now_s());
  CHECK(lock.expired);
  CHECK_FALSE(lock.held_by_other());
}

TEST_CASE("gpu lock: a lease writes the protocol's file and deletes only its own") {
  test::TempDir dir("gpu_lock_lease");
  const std::string path = dir.file("gpu.lock");
  bench::GpuLockLease::Config config;
  config.path = path;
  config.self = bench::current_gpu_lock_identity();
  config.purpose = "bench test.*";
  config.lease_s = 600;
  config.timeout_s = 0;
  config.log = nullptr;

  {
    bench::GpuLockLease lease;
    REQUIRE(lease.acquire(config) == bench::GpuLockLease::Outcome::Taken);
    CHECK(lease.held());
    JsonValue body;
    REQUIRE(parse_json(read_file(path), body).ok);
    CHECK(body["owner"] == JsonValue(config.self.owner));
    CHECK(body["purpose"] == JsonValue("bench test.*"));
    u64 pid = 0;
    CHECK(body["pid"].get_u64(pid));
    CHECK(pid == config.self.pid);
    std::string_view expires;
    REQUIRE(body["expires"].get_string(expires));
    i64 expires_s = 0;
    REQUIRE(bench::parse_iso8601_utc(expires, expires_s));
    CHECK(expires_s >= now_s() + 590);
    CHECK(expires_s <= now_s() + 610);
    CHECK(bench::read_gpu_lock(path, config.self, now_s()).mine);

    // A second lease in the same process is told the lock is already its own and does not
    // wait on itself, and does not delete it when it goes.
    {
      bench::GpuLockLease again;
      CHECK(again.acquire(config) == bench::GpuLockLease::Outcome::AlreadyMine);
      CHECK_FALSE(again.held());
    }
    CHECK(exists(path));
  }
  CHECK_FALSE(exists(path));  // released by the destructor

  // Somebody replaced the file after this lease's own expired and broke it: releasing must not
  // delete theirs.
  {
    bench::GpuLockLease lease;
    REQUIRE(lease.acquire(config) == bench::GpuLockLease::Outcome::Taken);
    write_file(path, lock_json("astra-blender", 4321, now_s() + 1800, config.self.host));
    lease.release();
  }
  CHECK(exists(path));
  CHECK(read_file(path).find("astra-blender") != std::string::npos);
}

TEST_CASE(
    "gpu lock: a lease waits for somebody else's lock, breaks an expired one, and never "
    "a person's") {
  test::TempDir dir("gpu_lock_contended");
  const std::string path = dir.file("gpu.lock");
  bench::GpuLockLease::Config config;
  config.path = path;
  config.self = bench::current_gpu_lock_identity();
  config.purpose = "bench test.*";
  config.timeout_s = 0;
  config.log = nullptr;

  const std::string theirs = lock_json("astra-blender", 4321, now_s() + 1800, config.self.host);
  write_file(path, theirs);
  {
    bench::GpuLockLease lease;
    CHECK(lease.acquire(config) == bench::GpuLockLease::Outcome::TimedOut);
    CHECK_FALSE(lease.held());
  }
  CHECK(read_file(path) == theirs);  // untouched

  write_file(path, lock_json("astra-blender", 4321, now_s() - 5, config.self.host));
  {
    bench::GpuLockLease lease;
    CHECK(lease.acquire(config) == bench::GpuLockLease::Outcome::Taken);
    CHECK(bench::read_gpu_lock(path, config.self, now_s()).mine);
  }
  CHECK_FALSE(exists(path));

  const std::string persons = lock_json("marc", 4321, now_s() - 5, config.self.host);
  write_file(path, persons);
  {
    bench::GpuLockLease lease;
    CHECK(lease.acquire(config) == bench::GpuLockLease::Outcome::TimedOut);
  }
  CHECK(read_file(path) == persons);

  // A directory that does not exist is a machine that does not use the lock.
  config.path = dir.file("no-such-directory/gpu.lock");
  bench::GpuLockLease lease;
  CHECK(lease.acquire(config) == bench::GpuLockLease::Outcome::NoDirectory);
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
  const bench::GpuLockIdentity self = bench::current_gpu_lock_identity();
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
